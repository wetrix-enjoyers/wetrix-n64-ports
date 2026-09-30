// Backtrace on crash.
//
// There is no debugger in the MinGW toolchain -- no gdb in MSYS2, and the Windows SDK
// ships only dbghelp.dll, not cdb.exe -- so a crash inside recomp::start was
// previously just "exit 139" with nothing to go on. This gives the process a way
// to report where it died on its own.
//
// dbghelp's StackWalk64 walks the x64 unwind tables rather than chasing frame
// pointers, so it still produces a usable stack from an -O2 build with frame
// pointers omitted. The addresses are printed as module + offset rather than
// symbol names because a MinGW build has no PDB for dbghelp to read; the offset
// is resolved offline against the executable's own symbol table:
//
//   nm --numeric-sort --defined-only build/win/wetrix.exe
//
// which is enough to name the function. tools/resolve_backtrace.py does
// exactly that lookup.
//
// Two entry points are needed rather than one, because the two failure modes do
// not arrive the same way:
//
//   * an access violation reaches SetUnhandledExceptionFilter, with a context
//     record pointing at the faulting instruction;
//   * std::terminate does not raise a Windows exception at all -- the CRT exits
//     directly -- so std::set_terminate is the only way to see it. That one is
//     worth catching: "terminate called without an active exception" is what a
//     std::thread destroyed while still joinable looks like, and it says nothing
//     about *which* thread object it was.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>

namespace {

const char* basename(const char* path) {
    const char* last = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            last = p + 1;
        }
    }
    return last;
}

void print_frame(int index, DWORD64 address) {
    HMODULE module = nullptr;
    char module_path[MAX_PATH] = "?";

    // UNCHANGED_REFCOUNT so that this does not add a reference to a module we
    // are only inspecting, and by-address so it works for any address, not just
    // exported entry points.
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(static_cast<uintptr_t>(address)),
                           &module)) {
        GetModuleFileNameA(module, module_path, MAX_PATH);
    }

    const DWORD64 module_base = reinterpret_cast<DWORD64>(module);
    fprintf(stderr, "[wetrix]   #%-2d %-20s + 0x%-8llx (0x%llx)\n", index,
            basename(module_path),
            static_cast<unsigned long long>(address - module_base),
            static_cast<unsigned long long>(address));
}

void print_stack(const CONTEXT& start) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();

    // No symbol path and no module enumeration: nothing here is resolved at
    // runtime, so all this is for is StackWalk64's unwind table lookup.
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(process, nullptr, TRUE);

    CONTEXT context = start;

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 64; ++i) {
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame,
                        &context, nullptr, SymFunctionTableAccess64,
                        SymGetModuleBase64, nullptr) == FALSE) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        print_frame(i, frame.AddrPC.Offset);
    }

    fflush(stderr);
}

LONG WINAPI unhandled_exception_filter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;

    fprintf(stderr, "\n[wetrix] *** crash: exception 0x%08lx at 0x%llx\n",
            static_cast<unsigned long>(record->ExceptionCode),
            static_cast<unsigned long long>(
                reinterpret_cast<uintptr_t>(record->ExceptionAddress)));

    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        record->NumberParameters >= 2) {
        fprintf(stderr, "[wetrix] *** %s at 0x%llx\n",
                record->ExceptionInformation[0] != 0 ? "write to" : "read from",
                static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }

    fprintf(stderr, "[wetrix] *** stack:\n");
    print_stack(*info->ContextRecord);

    // Terminate rather than chain on: the default action would be Windows Error
    // Reporting, whose dialog is no use for a headless run.
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] void terminate_handler() {
    fprintf(stderr, "\n[wetrix] *** std::terminate\n");
    fprintf(stderr, "[wetrix] *** stack:\n");

    // RtlCaptureContext rather than GetThreadContext: we are the terminate
    // handler, running on the thread that failed, so the stack to walk is right
    // here. GetThreadContext would need the thread suspended first.
    CONTEXT context{};
    RtlCaptureContext(&context);
    print_stack(context);

    // _exit, not abort: abort would raise SIGABRT and the CRT would print its
    // own message over the trace we just produced.
    _exit(3);
}

} // namespace

void wetrix_install_crash_handler() {
    SetUnhandledExceptionFilter(unhandled_exception_filter);
    std::set_terminate(terminate_handler);
}

#else

// The Linux build can get a backtrace from gdb, and printf-style diagnosis
// there already works.
void wetrix_install_crash_handler() {}

#endif // _WIN32
