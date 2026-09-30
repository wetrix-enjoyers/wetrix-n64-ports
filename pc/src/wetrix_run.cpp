// Scripted-run helpers. See wetrix_run.h.

#include "wetrix_run.h"

#include <cstdio>
#include <cstdlib>

namespace wetrix {

namespace {

// The largest frame named by WETRIX_FRAME_DUMP_AT, or 0 for "no list".
uint64_t last_dumped_frame() {
    const char* at = std::getenv("WETRIX_FRAME_DUMP_AT");
    if (at == nullptr) return 0;
    uint64_t last = 0;
    const char* p = at;
    while (*p != '\0') {
        char* end = nullptr;
        const unsigned long long value = std::strtoull(p, &end, 10);
        // A character that is not a number ends the scan rather than looping: a
        // malformed list should stop the run early, not sit in a busy loop.
        if (end == p) break;
        if (value > last) last = value;
        p = end;
        while (*p == ',' || *p == ' ' || *p == '\t') ++p;
    }
    return last;
}

uint64_t explicit_exit_frame() {
    const char* env = std::getenv("WETRIX_EXIT_AT");
    if (env == nullptr || *env == '\0') return 0;
    return std::strtoull(env, nullptr, 10);
}

constexpr uint64_t GameplayTriangleThreshold = 64;

constexpr uint64_t QuietFramesToLeaveGameplay = 120;

bool g_in_gameplay = false;
uint64_t g_first_gameplay_frame = 0;
uint64_t g_gameplay_frames = 0;
uint64_t g_quiet_frames = 0;

}  // namespace

bool observe_frame(uint64_t composed_frame, uint64_t triangles_submitted) {
    if (!g_in_gameplay) {
        if (triangles_submitted < GameplayTriangleThreshold) {
            return false;
        }

        g_in_gameplay = true;
        if (g_first_gameplay_frame == 0) {
            g_first_gameplay_frame = composed_frame;
        }
        g_gameplay_frames = 1;
        g_quiet_frames = 0;

        std::fprintf(stderr, "[wetrix] gameplay begins: frame %llu submits %llu triangles\n",
                     static_cast<unsigned long long>(composed_frame),
                     static_cast<unsigned long long>(triangles_submitted));
        std::fflush(stderr);
        return true;
    }

    ++g_gameplay_frames;
    if (triangles_submitted >= GameplayTriangleThreshold) {
        g_quiet_frames = 0;
        return true;
    }

    if (++g_quiet_frames < QuietFramesToLeaveGameplay) {
        return true;
    }

    std::fprintf(stderr,
                 "[wetrix] gameplay ends: %llu frames with no geometry (last frame %llu)\n",
                 static_cast<unsigned long long>(g_quiet_frames),
                 static_cast<unsigned long long>(composed_frame));
    std::fflush(stderr);
    g_in_gameplay = false;
    g_quiet_frames = 0;
    return false;
}

bool in_gameplay() { return g_in_gameplay; }

uint64_t first_gameplay_frame() { return g_first_gameplay_frame; }

uint64_t gameplay_frames() { return g_gameplay_frames; }

void exit_if_run_finished(uint64_t frame) {
    static const bool disabled = std::getenv("WETRIX_NO_EXIT") != nullptr;
    if (disabled) return;

    static const uint64_t explicit_at = explicit_exit_frame();
    static const uint64_t last_dump = last_dumped_frame();
    const uint64_t target = explicit_at != 0 ? explicit_at : last_dump;
    if (target == 0 || frame < target) return;

    std::fprintf(stderr, "[wetrix] frame %llu: run reached its last frame (%llu), exiting\n",
                 static_cast<unsigned long long>(frame), static_cast<unsigned long long>(target));
    std::fflush(stderr);
    std::_Exit(0);
}

}  // namespace wetrix
