#include <cstdint>
// Stall detector and sampler.
//
// The failure past the title screen is a hang, not a crash: the window stops
// changing, the process stays alive, and one thread burns a full core. A hang
// raises no exception, so crash_handler.cpp never fires and there is nothing to
// resolve -- and there is no debugger available to attach instead.
//
// This is the instrument for that case. It watches a counter that only moves when
// the game submits work, and when that counter stands still it suspends each thread
// in the process one at a time, reads its instruction pointer, and prints it. Two
// samples a couple of seconds apart make the answer obvious: the spinning thread
// holds the same RIP in both, while the others sit in a wait or move around.
//
// Printing is done *after* every thread is resumed, and the sample vector is sized
// up front, so nothing here allocates or prints while a thread is suspended. That
// matters: the suspended thread may be holding the heap or stdio lock, and blocking
// on it would turn a diagnosis into a second hang.
//
// The offsets are printed as "module + 0xoffset", matching what the crash handler
// emits, so port/tools/resolve_backtrace.py resolves both the same way.
//
// Enabled by WETRIX_STALL_PROBE; without it this is inert, so an ordinary run is
// unaffected. Remove once the stall past the title screen is understood.
//
// This file answers "where is it stuck". It cannot answer "how did it get there",
// because a spin has no stack to walk. That is what the trace is for -- see
// src/trace.cpp, which records the calls and is dumped here, at the moment the
// stall is detected, while the history still exists.

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <thread>
#include <vector>

namespace {

// Moves when the game submits a display list. Nothing else writes it.
std::atomic<uint64_t> g_progress{ 0 };

// The recorded call history, from src/trace.cpp. Takes the id of the thread whose
// history is wanted, because each thread keeps its own ring.
extern "C" void wetrix_trace_dump(uint64_t thread_id, int max_entries);

// Every ring instead of one. For the case where the sampler finds no spinner: a
// boot stall parks *every* thread, so there is no id to ask for and the per-thread
// tails are all there is to go on.
extern "C" void wetrix_trace_dump_all(int max_entries);

// The game threads the runtime started, with their ROM-side state, from
// src/main.cpp. It joins the sample below to the rings above by host thread id.
extern "C" void wetrix_dump_game_threads(void);

// The 8 MB of emulated RAM, handed over by the renderer context at startup. The
// wait past the title screen is a spin in the game's own logic, so the question is
// not "which thread" -- the sampler above answers that -- but "what is the loop
// looking at, and why is it never satisfied". That means reading the game's memory,
// which is what this is for. See dump_wait_state().
uint8_t* g_rdram = nullptr;

// RDRAM addressing, matching the generated code:
//
//   MEM_W(i, addr) = *(rdram + ((addr + i) ^ 3) - 0xFFFFFFFF80000000)
//
// The crucial detail is that the generated code passes addr as a *sign-extended*
// int64, so 0x80080BA4 becomes 0xFFFFFFFF80080BA4 before the subtraction and the
// result is 0x80BA4. Subtracting 0xFFFFFFFF80000000 from the zero-extended 32-bit
// value instead wraps to 0x100080BA4 -- 256 MB out of range, which the bounds check
// below then reports as "unreadable". That mistake is worth spelling out because it
// looks exactly like "the address does not exist" rather than "the arithmetic is
// wrong".
//
// Every address dumped here is aligned, so the XOR does not come into it.
uint8_t* ram_addr(uint32_t addr, size_t size) {
    if (g_rdram == nullptr) {
        return nullptr;
    }

    const int64_t sign_extended = static_cast<int64_t>(static_cast<int32_t>(addr));
    const int64_t kseg_base = static_cast<int64_t>(INT32_MIN);   // 0xFFFFFFFF80000000
    const int64_t offset = sign_extended - kseg_base;

    if (offset < 0 || static_cast<uint64_t>(offset) + size > 8u * 1024u * 1024u) {
        return nullptr;
    }
    return g_rdram + offset;
}

// These two mirror recomp.h exactly, because the layout is not what it first looks
// like. N64 big-endian words are stored *byte-swapped* -- native little-endian -- in
// the host buffer, so a word read is a plain native read:
//
//   #define MEM_W(offset, reg)   (*(int32_t*)(rdram + (((reg) + (offset)) - 0xFFFFFFFF80000000)))
//   #define MEM_HU(offset, reg)  (*(uint16_t*)(rdram + (((reg) + (offset)) ^ 2) - 0xFFFFFFFF80000000))
//
// Note the `^ 2` on the halfword: it is not optional. Composing bytes by hand in
// big-endian order instead reads 0x14000000 where the game holds 0x14, and prints
// pointers back to front -- both of which look like plausible garbage rather than
// like a bug.
uint32_t ram_read_u32(uint32_t addr) {
    const int32_t* p = reinterpret_cast<const int32_t*>(ram_addr(addr, 4));
    return p == nullptr ? 0 : static_cast<uint32_t>(*p);
}

uint16_t ram_read_u16(uint32_t addr) {
    const uint16_t* p = reinterpret_cast<const uint16_t*>(ram_addr(addr ^ 2, 2));
    return p == nullptr ? 0 : *p;
}

// The state of the spin loop found at ROM address 0x8002525C:
//
//   func_8004C120(2, 0)                 // set up
//   while (func_8004C1A0(2) != 0) n++;  // wait, no timeout
//
// func_8004C1A0 walks a table and returns how many entries are *not* ready. This
// prints the table itself, so the wait becomes readable data instead of a guess.
void dump_wait_state() {
    if (g_rdram == nullptr) {
        fprintf(stderr, "[stall] no RDRAM pointer; cannot dump game state\n");
        return;
    }

    // Note the addresses: the code is `lui $v0, 0x8008` then `lw 0xBA4($v0)`, so these
    // live at 0x80080BA4 and 0x80080BAC -- not 0x8008BA4. Getting that wrong reads
    // zeros and looks like "the table is empty", which is a convincing lie.
    // Before trusting any single address, prove the pointer addresses live game memory
    // at all. If this scan comes back empty, the pointer is wrong and every value
    // below is a lie rather than a finding.
    {
        size_t nonzero = 0;
        size_t first = SIZE_MAX;
        for (size_t i = 0; i < 0x200000; ++i) {
            if (g_rdram[i] != 0) {
                ++nonzero;
                if (first == SIZE_MAX) {
                    first = i;
                }
            }
        }
        fprintf(stderr,
                "[stall] rdram=%p: %zu non-zero bytes in the first 2MB, first at 0x%zX\n",
                static_cast<void*>(g_rdram), nonzero, first);
    }

    // Words around the addresses the wait depends on, decoded the way the game sees
    // them, so a wrong address looks obviously wrong instead of tidily zero.
    const uint32_t windows[] = { 0x80080BA0, 0x8007AD50, 0x800A5920 };
    for (uint32_t base_addr : windows) {
        fprintf(stderr, "[stall]   words at 0x%08X:", base_addr);
        for (uint32_t i = 0; i < 32; i += 4) {
            fprintf(stderr, " %08X", ram_read_u32(base_addr + i));
        }
        fprintf(stderr, "\n");
    }

    const uint32_t count = ram_read_u32(0x80080BA4);
    const uint32_t table = ram_read_u32(0x80080BAC);

    fprintf(stderr,
            "[stall] wait state: table_count=%u (0x%08X) table=0x%08X "
            "retries=%u gate=0x%08X\n",
            count, count, table, ram_read_u32(0x800A5920), ram_read_u32(0x8007AD50));

    // Anything other than a small table means the pointer or count itself is wrong,
    // which would be a finding in its own right.
    if (table < 0x80000000u || table > 0x80000000u + 8u * 1024u * 1024u || count > 64u) {
        fprintf(stderr, "[stall]   table pointer/count look invalid; not walking\n");
        fflush(stderr);
        return;
    }

    constexpr uint32_t STRIDE = 0x130;

    // The two kinds of record side by side, in full. Only the non-zero words are
    // printed, because the shape of what is set is more informative than 76 words
    // of hex, and comparing a slot the game considers live with one it is waiting
    // on is the whole question.
    const uint32_t interesting[] = { 0, 1, 16, 17 };
    for (uint32_t index : interesting) {
        if (index >= count) {
            continue;
        }
        const uint32_t record = table + index * STRIDE;

        fprintf(stderr, "[stall]   record %u at 0x%08X, non-zero words:", index, record);
        for (uint32_t off = 0; off < STRIDE; off += 4) {
            const uint32_t value = ram_read_u32(record + off);
            if (value != 0) {
                fprintf(stderr, " +0x%X=%08X", off, value);
            }
        }
        fprintf(stderr, "\n");

        // What the payload pointer actually points at.
        const uint32_t payload = ram_read_u32(record);
        if (payload >= 0x80000000u && payload < 0x80000000u + 8u * 1024u * 1024u) {
            fprintf(stderr, "[stall]     payload at 0x%08X:", payload);
            for (uint32_t w = 0; w < 32; w += 4) {
                fprintf(stderr, " %08X", ram_read_u32(payload + w));
            }
            fprintf(stderr, "\n");
        }
    }

    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t entry = table + i * STRIDE;

        // func_8004C1A0 counts this entry when word[0] != 0 and h[0x9E] == 0, so those
        // are the two fields that decide whether the loop can ever exit.
        const uint32_t first = ram_read_u32(entry);
        const uint16_t ready = ram_read_u16(entry + 0x9E);
        const uint32_t at_c = ram_read_u32(entry + 0xC);

        fprintf(stderr,
                "[stall]   [%2u] 0x%08X: word0=0x%08X h[0x9E]=0x%04X word[0xC]=0x%08X words:",
                i, entry, first, ready, at_c);

        for (uint32_t w = 0; w < 32; w += 4) {
            fprintf(stderr, " %08X", ram_read_u32(entry + w));
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

// How long progress has to stand still before it counts as a stall, and how long
// to wait between the two samples. The display-list rate is ~60/s, so ten seconds
// of silence is unambiguous.
constexpr double STALL_SECONDS = 10.0;
constexpr double SAMPLE_GAP_SECONDS = 2.0;

constexpr size_t MAX_THREADS = 512;

struct Sample {
    DWORD tid;
    uintptr_t rip;
};

// RIP of every thread in this process, one at a time.
size_t sample_threads(Sample* out, size_t capacity) {
    const DWORD self = GetCurrentProcessId();

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "[stall] could not snapshot threads (%lu)\n", GetLastError());
        return 0;
    }

    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);

    size_t count = 0;
    if (Thread32First(snapshot, &entry)) {
        do {
            if (entry.th32OwnerProcessID != self || count >= capacity) {
                continue;
            }

            // Never suspend the sampler itself: SuspendThread on the calling thread
            // returns, and then this thread never runs again to resume it. That is a
            // deadlock, not a sample -- it cost one debugging round to learn.
            if (entry.th32ThreadID == GetCurrentThreadId()) {
                continue;
            }

            HANDLE thread = OpenThread(
                THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                FALSE, entry.th32ThreadID);
            if (thread == nullptr) {
                continue;
            }

            if (SuspendThread(thread) != (DWORD)-1) {
                CONTEXT context{};
                context.ContextFlags = CONTEXT_CONTROL;

                if (GetThreadContext(thread, &context)) {
                    out[count].tid = entry.th32ThreadID;
                    out[count].rip = static_cast<uintptr_t>(context.Rip);
                    ++count;
                }

                ResumeThread(thread);
            }

            CloseHandle(thread);
        } while (Thread32Next(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return count;
}

// The exe's load address, so RIPs can be printed as offsets the resolver matches
// against nm's link-time symbols.
uintptr_t module_base() {
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

bool in_exe(uintptr_t rip, uintptr_t base) {
    return rip >= base && rip < base + 0x10000000;
}

// "Module+0xoffset" for an address, e.g. "libwinpthread-1.dll+0x73ED".
//
// Resolved through VirtualQuery rather than a module snapshot: for any address
// inside a mapped image, AllocationBase *is* the module's base, and
// GetModuleFileNameW accepts it as an HMODULE. That round trip is what makes the
// parked threads legible. "None of them in this program's code" was true of the
// boot stall and said nothing useful; naming the module said they were all in
// libwinpthread's condition-variable wait, which is a very different story.
//
// The path is narrowed by hand rather than with %ls. Module file names are ASCII
// in practice, and a diagnostic path is the wrong place to depend on the C
// library's wide-print behaviour (the MinGW build links a different CRT than the
// MSVC one).
void resolve_address(uintptr_t address, char* out, size_t capacity) {
    MEMORY_BASIC_INFORMATION info{};
    wchar_t path[MAX_PATH] = {};

    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 ||
        info.AllocationBase == nullptr ||
        GetModuleFileNameW(static_cast<HMODULE>(info.AllocationBase), path, MAX_PATH) == 0) {
        std::snprintf(out, capacity, "0x%llX", static_cast<unsigned long long>(address));
        return;
    }

    const wchar_t* name = std::wcsrchr(path, L'\\');
    name = name != nullptr ? name + 1 : path;

    char narrow[MAX_PATH] = {};
    size_t used = 0;
    for (; used + 1 < sizeof(narrow) && name[used] != L'\0'; ++used) {
        narrow[used] = name[used] < 0x80 ? static_cast<char>(name[used]) : '?';
    }

    std::snprintf(out, capacity, "%s+0x%llX", narrow,
                  static_cast<unsigned long long>(
                      address - reinterpret_cast<uintptr_t>(info.AllocationBase)));
}

// Every thread, grouped by where it is.
//
// This used to print only the threads inside this program's code, on the
// assumption that the spinner is the answer and that the other sixty threads are
// parked in ntdll and not worth reading. That holds for a spin and fails for a
// boot stall, where nothing is in this program's code and where everything else
// went is exactly the question. Grouping keeps it to a handful of lines at 59
// threads, because most of them are stopped in the same place.
void print_sample(const char* label, const Sample* samples, size_t count) {
    // Static rather than automatic: 512 threads of keys and counts is 50 KB, which
    // is more than a watcher thread's stack wants to hand over for a diagnostic.
    static char keys[MAX_THREADS][96];
    static uint32_t repeats[MAX_THREADS];
    size_t unique = 0;

    const uintptr_t base = module_base();
    size_t in_program = 0;

    for (size_t i = 0; i < count; ++i) {
        if (in_exe(samples[i].rip, base)) {
            ++in_program;
        }

        char key[96] = {};
        resolve_address(samples[i].rip, key, sizeof(key));

        size_t slot = 0;
        for (; slot < unique; ++slot) {
            if (std::strcmp(keys[slot], key) == 0) {
                break;
            }
        }

        if (slot == unique) {
            if (unique >= MAX_THREADS) {
                continue;  // unreachable: count is itself bounded by MAX_THREADS
            }
            std::snprintf(keys[unique], sizeof(keys[unique]), "%s", key);
            repeats[unique] = 0;
            ++unique;
        }

        ++repeats[slot];
    }

    fprintf(stderr, "[stall] %s: %zu threads, %zu of them in this program's code\n", label,
            count, in_program);
    for (size_t i = 0; i < unique; ++i) {
        fprintf(stderr, "[stall]   %-42s x%u\n", keys[i], repeats[i]);
    }
    fflush(stderr);
}

void watch_for_stall() {
    std::vector<Sample> first(MAX_THREADS);
    std::vector<Sample> second(MAX_THREADS);

    using clock = std::chrono::steady_clock;

    uint64_t last_value = g_progress.load(std::memory_order_relaxed);
    auto last_change = clock::now();

    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        const uint64_t value = g_progress.load(std::memory_order_relaxed);
        if (value != last_value) {
            last_value = value;
            last_change = clock::now();
            continue;
        }

        const double stalled =
            std::chrono::duration<double>(clock::now() - last_change).count();
        if (stalled < STALL_SECONDS) {
            continue;
        }

        fprintf(stderr,
                "[stall] no progress for %.1fs (counter stuck at %llu); sampling threads\n",
                stalled, static_cast<unsigned long long>(value));
        dump_wait_state();
        fflush(stderr);

        const size_t first_count = sample_threads(first.data(), first.size());
        std::this_thread::sleep_for(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::duration<double>(SAMPLE_GAP_SECONDS)));
        const size_t second_count = sample_threads(second.data(), second.size());

        // Which ROM thread each host thread is, and what the ROM's scheduler thinks
        // it is doing. Printed before the samples because the two lists are joined
        // by host thread id, and the same id is printed for each ring below.
        wetrix_dump_game_threads();

        print_sample("sample 1", first.data(), first_count);
        print_sample("sample 2", second.data(), second_count);

        // The spinner is the thread executing this program's code in *both* samples.
        // Matching on the exact RIP would miss it: a tight loop moves by the size of
        // its body between samples, which is how the loop was first spotted. Match on
        // the thread instead, and report both RIPs so the distance between them shows
        // how tight the loop is.
        const uintptr_t base = module_base();
        fprintf(stderr, "[stall] still running this program's code after %.1fs:\n",
                SAMPLE_GAP_SECONDS);
        size_t spinners = 0;
        DWORD spinner_tid = 0;
        for (size_t i = 0; i < first_count; ++i) {
            if (!in_exe(first[i].rip, base)) {
                continue;
            }
            for (size_t j = 0; j < second_count; ++j) {
                if (first[i].tid != second[j].tid || !in_exe(second[j].rip, base)) {
                    continue;
                }
                spinners++;
                if (spinner_tid == 0) {
                    spinner_tid = first[i].tid;
                }
                fprintf(stderr, "[stall]   tid=%-6lu wetrix.exe+0x%llX then +0x%llX\n",
                        first[i].tid,
                        static_cast<unsigned long long>(first[i].rip - base),
                        static_cast<unsigned long long>(second[j].rip - base));
                break;
            }
        }
        if (spinners == 0) {
            fprintf(stderr, "[stall]   none\n");
        }
        fflush(stderr);

        // The call history that led into the loop, for the thread that is stuck.
        // Dumped here rather than earlier because the thread id is only known
        // once the two samples have been compared -- and it has to be *that*
        // thread's ring; the others are still running and their traffic is not
        // the answer.
        if (spinner_tid != 0) {
            wetrix_trace_dump(spinner_tid, 200);
        }

        // And every thread's ring, not only the spinner's. When nothing is spinning
        // there is no thread to name, and each ring's tail is then the whole of the
        // evidence -- see wetrix_trace_dump_all in src/trace.cpp.
        wetrix_trace_dump_all(120);

        // A hung run cannot make progress, so stop rather than leave a spinning
        // process behind for the rest of the session.
        std::exit(0);
    }
}

}  // namespace

// Called once per submitted display list, from the renderer context.
extern "C" void wetrix_progress_tick() {
    g_progress.fetch_add(1, std::memory_order_relaxed);
}

// Called by the renderer context with the RDRAM the runtime allocated, so the dump
// above can read the game's memory. Deliberately not used for anything else.
extern "C" void wetrix_set_rdram(uint8_t* rdram) {
    g_rdram = rdram;
}

void wetrix_start_stall_probe() {
    if (std::getenv("WETRIX_STALL_PROBE") == nullptr) {
        return;
    }

    fprintf(stderr, "[stall] probe armed: reporting if progress stops for %.0fs\n",
            STALL_SECONDS);
    fflush(stderr);

    std::thread(watch_for_stall).detach();
}

#else

extern "C" void wetrix_progress_tick() {}

extern "C" void wetrix_set_rdram(uint8_t* rdram) { (void)rdram; }

void wetrix_start_stall_probe() {}

#endif
