// A sampling profiler for one thread: another thread stops it every ~50 us and
// records where it was. Offsets are relative to the executable's load address,
// ready for addr2line (see tools/soft_prof.py). Windows only; elsewhere it
// records nothing.

#include "sampler.h"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace wetrix::sampler {

struct Session {
    std::atomic<bool> stop{ false };
    std::vector<uint64_t> offsets;
    std::thread thread;
#ifdef _WIN32
    HANDLE target = nullptr;
#endif
};

namespace {

Session* start(void* handle);

}  // namespace

Session* start_on_this_thread() {
#ifdef _WIN32
    return start(GetCurrentThread());
#else
    return start(nullptr);
#endif
}

Session* start_on_handle(void* handle) { return start(handle); }

namespace {

Session* start(void* handle) {
    auto* s = new Session;
#ifdef _WIN32
    if (handle == nullptr) return s;
    DuplicateHandle(GetCurrentProcess(), static_cast<HANDLE>(handle), GetCurrentProcess(), &s->target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
    const uint64_t base = reinterpret_cast<uint64_t>(GetModuleHandleW(nullptr));
    s->offsets.reserve(1 << 20);
    s->thread = std::thread([s, base] {
        while (!s->stop.load(std::memory_order_relaxed)) {
            if (SuspendThread(s->target) != static_cast<DWORD>(-1)) {
                CONTEXT c{};
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(s->target, &c)) s->offsets.push_back(c.Rip - base);
                ResumeThread(s->target);
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(50);
            while (std::chrono::steady_clock::now() < until) {
            }
        }
    });
#else
    (void)handle;
#endif
    return s;
}

}  // namespace

std::vector<uint64_t> stop(Session* s) {
    s->stop.store(true);
    if (s->thread.joinable()) s->thread.join();
#ifdef _WIN32
    if (s->target != nullptr) CloseHandle(s->target);
#endif
    std::vector<uint64_t> out = std::move(s->offsets);
    delete s;
    return out;
}

}  // namespace wetrix::sampler
