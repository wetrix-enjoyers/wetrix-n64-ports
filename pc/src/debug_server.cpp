// Debug console. See include/debug_server.h for what it is for.

#include "debug_server.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <winsock2.h>
#   include <ws2tcpip.h>
using socket_t = SOCKET;
#   define CLOSESOCK closesocket
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <unistd.h>
using socket_t = int;
#   define INVALID_SOCKET (-1)
#   define CLOSESOCK close
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "f3d.h"
#include "render_select.h"
#include "sampler.h"

extern "C" FILE* wetrix_diag_out;

namespace wetrix::soft_ctx {
bool wetrix_soft_capture(std::vector<uint8_t>& rgba, int& w, int& h);
double wetrix_soft_bench(const uint8_t* ram, uint32_t dl, int n);
void* wetrix_soft_worker_handle(int i);
}

extern "C" void wetrix_trace_dump_all(int max_entries);
extern "C" void wetrix_trace_dump(uint64_t thread_id, int max_entries);
extern "C" void wetrix_dump_game_threads(void);

namespace wetrix::debug {

namespace {

uint8_t* g_rdram = nullptr;
constexpr uint32_t RDRAM_SIZE = 8 * 1024 * 1024;

// ---- pause / step --------------------------------------------------------
std::mutex pause_mutex;
std::condition_variable pause_cv;
std::atomic<bool> g_paused{ false };
std::atomic<int> g_step_budget{ 0 };
std::atomic<uint32_t> g_task_count{ 0 };
std::atomic<bool> g_f3d_active{ false };
std::function<void()> g_repaint;

struct TaskCopy {
    uint32_t data_ptr = 0, ucode = 0, ucode_data = 0;
    bool valid = false;
} last_task;

// RDRAM as it was when each task started. While paused the game keeps
// building the next frame in RDRAM, so any redraw has to run from a copy.
// `shown` is the task before the one we are paused on: the frame both
// renderers have drawn and the VI is showing.
// A ring of the last few tasks' starting RDRAM. The VI can be two frames
// behind the task we are paused on, so the frame on screen is found by
// replaying from newest to oldest until one ends on the presented target.
struct Snapshot {
    std::vector<uint8_t> ram;
    TaskCopy task;
};
constexpr int SNAPSHOTS = 4;
Snapshot snaps[SNAPSHOTS];
int snap_head = 0;   // next slot to write
bool snapshots_on = true;

// ---- gfx-thread jobs ------------------------------------------------------
struct Job {
    std::function<std::string()> fn;
    std::promise<std::string> result;
};
std::mutex job_mutex;
std::deque<std::shared_ptr<Job>> jobs;

std::string run_on_gfx(std::function<std::string()> fn, int timeout_ms = 10000) {
    auto job = std::make_shared<Job>();
    job->fn = std::move(fn);
    auto fut = job->result.get_future();
    {
        std::lock_guard<std::mutex> lk(job_mutex);
        jobs.push_back(job);
    }
    pause_cv.notify_all();
    if (fut.wait_for(std::chrono::milliseconds(timeout_ms)) != std::future_status::ready) {
        return "error: gfx thread did not service the request (hung, or no display lists and no VI?)";
    }
    return fut.get();
}

void service_jobs() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::lock_guard<std::mutex> lk(job_mutex);
            if (jobs.empty()) return;
            job = jobs.front();
            jobs.pop_front();
        }
        std::string out;
        try {
            out = job->fn();
        } catch (...) {
            out = "error: exception";
        }
        job->result.set_value(out);
    }
}

// ---- input injection ------------------------------------------------------
struct PadInject {
    uint16_t held = 0;
    uint16_t pulse = 0;
    std::chrono::steady_clock::time_point pulse_until{};
    bool stick = false;
    float sx = 0, sy = 0;
    std::chrono::steady_clock::time_point stick_until{};
};
std::mutex pad_mutex;
PadInject pads[4];

uint16_t parse_buttons(const std::string& list, bool& ok) {
    static const struct {
        const char* name;
        uint16_t bit;
    } names[] = { { "a", 0x8000 }, { "b", 0x4000 }, { "z", 0x2000 }, { "start", 0x1000 }, { "up", 0x0800 },
                  { "down", 0x0400 }, { "left", 0x0200 }, { "right", 0x0100 }, { "l", 0x0020 }, { "r", 0x0010 },
                  { "cu", 0x0008 }, { "cd", 0x0004 }, { "cl", 0x0002 }, { "cr", 0x0001 } };
    uint16_t out = 0;
    ok = true;
    std::stringstream ss(list);
    std::string tok;
    while (std::getline(ss, tok, '+')) {
        bool found = false;
        for (auto& n : names) {
            if (tok == n.name) {
                out |= n.bit;
                found = true;
            }
        }
        if (!found) ok = false;
    }
    return out;
}

// ---- helpers -------------------------------------------------------------
std::string capture_diag(const std::function<void()>& fn) {
    FILE* tmp = tmpfile();
    if (tmp == nullptr) return "error: tmpfile failed";
    wetrix_diag_out = tmp;
    fn();
    wetrix_diag_out = nullptr;
    fflush(tmp);
    std::string out;
    long len = ftell(tmp);
    rewind(tmp);
    out.resize(len > 0 ? (size_t)len : 0);
    if (len > 0) fread(out.data(), 1, (size_t)len, tmp);
    fclose(tmp);
    return out;
}

bool write_png(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    return stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4) != 0;
}

std::string f3d_only() { return "error: the f3d renderer is not active (WETRIX_RENDERER=fast3d, softab or hardab)"; }

const char* HELP =
    "commands (one per line; each reply ends with a line containing only '.'):\n"
    "  help                      this text\n"
    "  status                    renderer, pause state, task count\n"
    "  pause | resume            freeze / unfreeze the game at the next display list\n"
    "  step [n]                  while paused: let n display lists through (default 1)\n"
    "  stats                     f3d state of the last display list (counts, targets, tiles)\n"
    "  shot <file.png>           f3d: what the VI is showing, at internal resolution\n"
    "  target <addr> <file.png>  f3d: any render target by RDRAM address (hex)\n"
    "  targets                   f3d: list render targets\n"
    "  redraw                    f3d: re-run the last display list and repaint (use while paused)\n"
    "  shot-hard <file.png>      hard: the shown picture at render resolution\n"
    "  bench-soft [n] [file]     soft: time a display list n times on a private memory copy;\n"
    "                            file: replay that saved frame, or save the latest one there\n"
    "                            prof: sample the gfx thread (profw: worker 0) into <file>.samples (wetter/tools/soft_prof.py)\n"
    "  limit <n|-1>              f3d: draw only the first n draw events (bisect a frame)\n"
    "  highlight <n|-1>          f3d: paint draw event n magenta\n"
    "  wire on|off               f3d: wireframe (desktop GL only)\n"
    "  pixhist <x> <y>           f3d: every draw event that changed pixel (x,y) of the shown frame (pause first)\n"
    "  trace <file>              f3d: disassemble every command of the next display list\n"
    "  dl <file>                 f3d: dump the last display list (addr, words, depth, draw #, disasm)\n"
    "  tmem <file>               f3d: hex dump of emulated TMEM\n"
    "  rdram <file>              raw 8MB RDRAM (host word order: byte A at offset A^3)\n"
    "  capture <file>            last task + VI state + RDRAM, for tools/f3d_replay\n"
    "  read <addr> [len]         hex dump of RDRAM (N64 byte order)\n"
    "  threads                   game threads and their states\n"
    "  calls [n]                 recent recompiled calls on every thread (trace rings)\n"
    "  press <btns> [ms]         tap buttons (a b z start up down left right l r cu cd cl cr, join with +)\n"
    "  hold <btns> | release     hold buttons until released\n"
    "  stick <x> <y> [ms]        analog stick, -1..1\n"
    "  quit                      exit the game\n";

// Re-runs the frame the VI is showing, from its own RDRAM snapshot (gfx thread).
// Skips the newest snapshot: that is the task we are paused on, not yet drawn.
int shown_slot = -1;

bool replay_slot(int slot) {
    Snapshot& sn = snaps[slot];
    if (!sn.task.valid || sn.ram.size() != RDRAM_SIZE) return false;
    uint8_t* live = f3d::set_memory(sn.ram.data());
    f3d::run_dl(sn.task.data_ptr, sn.task.ucode, sn.task.ucode_data);
    f3d::set_memory(live);
    return true;
}

bool replay_shown() {
    const uint32_t want = f3d::presented_target();
    if (shown_slot >= 0 && replay_slot(shown_slot) && f3d::last_cimg() == want) return true;
    for (int back = 2; back <= SNAPSHOTS; back++) {
        const int slot = ((snap_head - back) % SNAPSHOTS + SNAPSHOTS) % SNAPSHOTS;
        if (replay_slot(slot) && f3d::last_cimg() == want) {
            shown_slot = slot;
            return true;
        }
    }
    shown_slot = -1;
    return false;
}

std::string handle(const std::string& line) {
    std::stringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd.empty()) return "";
    if (cmd == "help") return HELP;

    if (cmd == "status") {
        char buf[256];
        snprintf(buf, sizeof(buf), "renderer %s  paused %s  tasks %u  f3d %s  last task dl 0x%08X",
                 render::mode_name(render::mode()), g_paused ? "yes" : "no", g_task_count.load(),
                 g_f3d_active ? "active" : "inactive", last_task.data_ptr);
        return buf;
    }
    if (cmd == "pause") {
        g_step_budget = 0;
        g_paused = true;
        return "paused (takes effect at the next display list)";
    }
    if (cmd == "resume") {
        g_paused = false;
        pause_cv.notify_all();
        return "resumed";
    }
    if (cmd == "step") {
        int n = 1;
        ss >> n;
        g_paused = true;
        g_step_budget += std::max(n, 1);
        pause_cv.notify_all();
        return "stepping " + std::to_string(std::max(n, 1));
    }
    if (cmd == "threads") return capture_diag([] { wetrix_dump_game_threads(); });
    if (cmd == "calls") {
        int n = 40;
        ss >> n;
        return capture_diag([n] { wetrix_trace_dump_all(n); });
    }
    if (cmd == "read") {
        std::string a;
        uint32_t len = 64;
        ss >> a >> len;
        uint32_t addr = (uint32_t)strtoul(a.c_str(), nullptr, 16) & (RDRAM_SIZE - 1);
        len = std::min<uint32_t>(len, 4096);
        std::string out;
        char buf[16];
        for (uint32_t i = 0; i < len; i++) {
            if (i % 16 == 0) {
                snprintf(buf, sizeof(buf), "%s%06X:", i ? "\n" : "", addr + i);
                out += buf;
            }
            snprintf(buf, sizeof(buf), " %02X", g_rdram[((addr + i) ^ 3) & (RDRAM_SIZE - 1)]);
            out += buf;
        }
        return out;
    }
    if (cmd == "rdram") {
        std::string path;
        ss >> path;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return "error: cannot open " + path;
        fwrite(g_rdram, 1, RDRAM_SIZE, f);
        fclose(f);
        return "wrote " + path;
    }
    if (cmd == "capture") {
        std::string path;
        ss >> path;
        if (!last_task.valid) return "error: no display list seen yet";
        return run_on_gfx([path] {
            FILE* f = fopen(path.c_str(), "wb");
            if (!f) return std::string("error: cannot open ") + path;
            const auto* vi = ultramodern::renderer::get_vi_regs();
            const uint32_t header[8] = { 0x50414357 /* 'WCAP' */, 1, last_task.data_ptr, last_task.ucode,
                                         last_task.ucode_data, RDRAM_SIZE, 0, 0 };
            fwrite(header, sizeof(header), 1, f);
            fwrite(vi, sizeof(*vi), 1, f);
            fwrite(g_rdram, 1, RDRAM_SIZE, f);
            fclose(f);
            return std::string("wrote ") + path + " (note: RDRAM is as of now, the game may have moved on unless paused)";
        });
    }
    if (cmd == "press" || cmd == "hold") {
        std::string list;
        int ms = 120;
        ss >> list >> ms;
        bool ok;
        uint16_t b = parse_buttons(list, ok);
        if (!ok) return "error: unknown button in '" + list + "'";
        std::lock_guard<std::mutex> lk(pad_mutex);
        if (cmd == "hold") {
            pads[0].held |= b;
        } else {
            pads[0].pulse = b;
            pads[0].pulse_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        }
        return "ok";
    }
    if (cmd == "release") {
        std::lock_guard<std::mutex> lk(pad_mutex);
        pads[0].held = 0;
        pads[0].pulse = 0;
        pads[0].stick = false;
        return "ok";
    }
    if (cmd == "stick") {
        float x = 0, y = 0;
        int ms = 250;
        ss >> x >> y >> ms;
        std::lock_guard<std::mutex> lk(pad_mutex);
        pads[0].stick = true;
        pads[0].sx = x;
        pads[0].sy = y;
        pads[0].stick_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        return "ok";
    }
    if (cmd == "quit") {
        std::exit(0);
    }

    if (cmd == "bench-soft") {
        // With a file: replay that saved frame if it exists, else save the latest
        // snapshot there first, so later builds can be timed on the same frame.
        int n = 50;
        std::string file, prof;
        ss >> n >> file >> prof;
        return run_on_gfx([n, file, prof] {
            std::vector<uint8_t> ram;
            uint32_t dl = 0;
            FILE* in = file.empty() ? nullptr : fopen(file.c_str(), "rb");
            if (in != nullptr) {
                ram.resize(RDRAM_SIZE);
                const bool ok = fread(&dl, 4, 1, in) == 1 && fread(ram.data(), 1, RDRAM_SIZE, in) == RDRAM_SIZE;
                fclose(in);
                if (!ok) return std::string("error: ") + file + " is not a saved frame";
            } else {
                const int slot = ((snap_head - 1) % SNAPSHOTS + SNAPSHOTS) % SNAPSHOTS;
                const Snapshot& sn = snaps[slot];
                if (!sn.task.valid || sn.ram.size() != RDRAM_SIZE) return std::string("error: no snapshot yet");
                ram = sn.ram;
                dl = sn.task.data_ptr;
                if (!file.empty()) {
                    FILE* out = fopen(file.c_str(), "wb");
                    if (out == nullptr) return std::string("error: cannot write ") + file;
                    fwrite(&dl, 4, 1, out);
                    fwrite(ram.data(), 1, RDRAM_SIZE, out);
                    fclose(out);
                }
            }
            wetrix::sampler::Session* sampling =
                prof == "prof"    ? wetrix::sampler::start_on_this_thread()
                : prof == "profw" ? wetrix::sampler::start_on_handle(wetrix::soft_ctx::wetrix_soft_worker_handle(0))
                                  : nullptr;
            const double ms = wetrix::soft_ctx::wetrix_soft_bench(ram.data(), dl, n);
            if (sampling != nullptr) {
                const std::vector<uint64_t> offsets = wetrix::sampler::stop(sampling);
                if (FILE* out = fopen((file + ".samples").c_str(), "w")) {
                    for (uint64_t o : offsets) fprintf(out, "%llx\n", static_cast<unsigned long long>(o));
                    fclose(out);
                }
            }
            if (ms < 0.0) return std::string("error: soft renderer not running");
            char buf[160];
            snprintf(buf, sizeof(buf), "soft: %.3f ms per display list, %d runs of dl 0x%08X", ms, n, dl);
            return std::string(buf);
        }, 120000);
    }
    if (cmd == "shot-hard") {
        std::string path;
        ss >> path;
        return run_on_gfx([path] {
            std::vector<uint8_t> px;
            int w = 0, h = 0;
            if (!render::hard_capture(px, w, h)) return std::string("error: hard renderer has no frame");
            if (!write_png(path, px, w, h)) return std::string("error: png write failed");
            return "wrote " + path + " " + std::to_string(w) + "x" + std::to_string(h);
        });
    }
    if (cmd == "shot-soft") {
        std::string path;
        ss >> path;
        std::vector<uint8_t> px;
        int w, h;
        if (!wetrix::soft_ctx::wetrix_soft_capture(px, w, h)) return "error: soft renderer has no frame";
        if (!write_png(path, px, w, h)) return "error: png write failed";
        return "wrote " + path + " " + std::to_string(w) + "x" + std::to_string(h);
    }
    if (cmd == "ab") {
        std::string prefix;
        ss >> prefix;
        const bool hard = render::mode() == render::Mode::HardAB;
        const bool soft = render::mode() == render::Mode::SoftAB || hard;
        if (!hard && (!soft || !g_f3d_active)) {
            return "error: ab needs WETRIX_RENDERER=softab or hardab";
        }
        std::vector<uint8_t> a, b;
        int aw, ah, bw, bh;
        if (!wetrix::soft_ctx::wetrix_soft_capture(a, aw, ah)) return "error: soft renderer has no frame";
        std::string r = run_on_gfx([&] {
            if (hard) return render::hard_capture(b, bw, bh) ? std::string() : std::string("error: hard has no frame");
            return f3d::grab_presented(b, bw, bh) ? std::string() : std::string("error: f3d has presented nothing");
        });
        if (!r.empty()) return r;
        // Compare at soft's size; the other image is resampled (nearest) if they differ.
        std::vector<uint8_t> bs((size_t)aw * ah * 4), diff((size_t)aw * ah * 4);
        uint64_t sum = 0;
        size_t over16 = 0, over64 = 0;
        for (int y = 0; y < ah; y++) {
            for (int x = 0; x < aw; x++) {
                // hard renders the same lines at an integer scale (and keeps rows
                // the VI does not show), so it is sampled pixel for pixel, not stretched.
                const int k = std::max(1, bw / aw);
                const int sx = hard ? std::min(bw - 1, x * k + k / 2) : x * bw / aw;
                const int sy = hard ? std::min(bh - 1, y * k + k / 2) : y * bh / ah;
                const uint8_t* pb = &b[((size_t)sy * bw + sx) * 4];
                const uint8_t* pa = &a[((size_t)y * aw + x) * 4];
                uint8_t* pd = &diff[((size_t)y * aw + x) * 4];
                memcpy(&bs[((size_t)y * aw + x) * 4], pb, 4);
                int m = 0;
                for (int c = 0; c < 3; c++) {
                    const int d = std::abs((int)pa[c] - (int)pb[c]);
                    sum += d;
                    m = std::max(m, d);
                }
                if (m > 16) over16++;
                if (m > 64) over64++;
                // Diff image: grey = the soft image dimmed, red = where they disagree.
                const uint8_t g = (uint8_t)((pa[0] + pa[1] + pa[2]) / 12);
                pd[0] = (uint8_t)std::min(255, g + m * 3);
                pd[1] = g;
                pd[2] = g;
                pd[3] = 255;
            }
        }
        write_png(prefix + "_soft.png", a, aw, ah);
        write_png(prefix + (hard ? "_hard.png" : "_f3d.png"), bs, aw, ah);
        write_png(prefix + "_diff.png", diff, aw, ah);
        const double n = (double)aw * ah;
        char buf[256];
        snprintf(buf, sizeof(buf), hard ? "%s %dx%d  hard %dx%d  mean abs err %.2f/255  pixels >16: %.2f%%  >64: %.2f%%"
                                        : "%s %dx%d  f3d %dx%d  mean abs err %.2f/255  pixels >16: %.2f%%  >64: %.2f%%",
                 "soft", aw, ah, bw, bh, sum / (n * 3), 100.0 * over16 / n, 100.0 * over64 / n);
        return buf;
    }

    // Everything below needs the f3d renderer and its GL context.
    if (!g_f3d_active) return f3d_only();

    if (cmd == "stats") return run_on_gfx([] { return f3d::describe_state(); });
    if (cmd == "targets") {
        return run_on_gfx([] {
            std::string out;
            char buf[128];
            for (auto& t : f3d::targets()) {
                snprintf(buf, sizeof(buf), "0x%06X %ux%u siz %u last frame %u\n", t.addr, t.width, t.height, t.siz,
                         t.last_frame);
                out += buf;
            }
            return out;
        });
    }
    if (cmd == "shot") {
        std::string path;
        ss >> path;
        return run_on_gfx([path] {
            std::vector<uint8_t> px;
            int w, h;
            if (!f3d::grab_presented(px, w, h)) return std::string("error: nothing presented yet");
            if (!write_png(path, px, w, h)) return std::string("error: png write failed");
            return "wrote " + path + " " + std::to_string(w) + "x" + std::to_string(h);
        });
    }
    if (cmd == "target") {
        std::string a, path;
        ss >> a >> path;
        uint32_t addr = (uint32_t)strtoul(a.c_str(), nullptr, 16);
        return run_on_gfx([addr, path] {
            std::vector<uint8_t> px;
            int w, h;
            if (!f3d::grab_target(addr, px, w, h)) return std::string("error: no such target");
            if (!write_png(path, px, w, h)) return std::string("error: png write failed");
            return "wrote " + path;
        });
    }
    if (cmd == "redraw") {
        return run_on_gfx([] {
            if (!replay_shown()) return std::string("error: no snapshot yet (needs two tasks with f3d active)");
            if (g_repaint) g_repaint();
            return std::string("redrawn\n") + f3d::describe_state();
        });
    }
    if (cmd == "limit" || cmd == "highlight") {
        int n = -1;
        ss >> n;
        const bool lim = cmd == "limit";
        return run_on_gfx([n, lim] {
            if (lim) f3d::set_draw_limit(n);
            else f3d::set_highlight(n);
            return std::string("ok (takes effect on the next display list; 'redraw' to apply now)");
        });
    }
    if (cmd == "probe") {
        int n = -1;
        std::string dir;
        ss >> n >> dir;
        return run_on_gfx([n, dir] {
            f3d::set_probe(n, dir);
            if (g_paused && replay_shown()) {
                if (g_repaint) g_repaint();
                f3d::set_probe(-1, "");
                return "probed draw " + std::to_string(n) + " into " + dir;
            }
            return "will probe draw " + std::to_string(n) + " of the next display list";
        });
    }
    if (cmd == "set") {
        std::string name;
        int v = 1;
        ss >> name >> v;
        return run_on_gfx([name, v] {
            auto& o = f3d::options();
            if (name == "nocull") o.no_cull = v;
            else if (name == "noclip") o.no_clip_reject = v;
            else if (name == "noculldl") o.no_culldl = v;
            else if (name == "branchz") o.force_branch_z = v;
            else if (name == "bilinear") o.bilinear = v;
            else return std::string("error: set nocull|noclip|noculldl|branchz|bilinear 0|1");
            return name + " = " + std::to_string(v);
        });
    }
    if (cmd == "pixhist") {
        int x = 0, y = 0;
        ss >> x >> y;
        if (!g_paused || !last_task.valid) return "error: pause first";
        return run_on_gfx([x, y] {
            // Redraw with draw_limit 0..N and report each change of the pixel.
            if (!replay_shown()) return std::string("error: no snapshot yet");
            const int n = (int)f3d::last_stats().draw_events;
            uint8_t prev[4] = { 0, 0, 0, 0 }, cur[4];
            std::string out;
            char buf[160];
            for (int limit = 0; limit <= n; limit++) {
                f3d::set_draw_limit(limit);
                replay_shown();
                if (!f3d::presented_pixel(x, y, cur)) {
                    f3d::set_draw_limit(-1);
                    return std::string("error: no presented f3d target");
                }
                if (limit == 0 || memcmp(cur, prev, 4) != 0) {
                    snprintf(buf, sizeof(buf), "after %4d draws: %3u %3u %3u %3u%s\n", limit, cur[0], cur[1], cur[2],
                             cur[3], limit == 0 ? "  (start of frame)" : "");
                    out += buf;
                    memcpy(prev, cur, 4);
                }
            }
            f3d::set_draw_limit(-1);
            replay_shown();
            if (g_repaint) g_repaint();
            out += "(draw k changes the pixel when it appears as 'after k+1 draws'; probe k for its state)";
            return out;
        }, 60000);
    }
    if (cmd == "wire") {
        std::string v;
        ss >> v;
        const bool on = v == "on";
        return run_on_gfx([on] {
            f3d::set_wireframe(on);
            return std::string("ok");
        });
    }
    if (cmd == "trace") {
        std::string path;
        ss >> path;
        return run_on_gfx([path] {
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return std::string("error: cannot open ") + path;
            f3d::set_trace_file(f);
            // f3d flushes and drops the handle after the next display list; the
            // OS closes it at exit. Redraw now if paused so the trace exists.
            if (g_paused && replay_shown()) {
                if (g_repaint) g_repaint();
                fclose(f);
                return "traced last display list into " + path;
            }
            return "tracing the next display list into " + path;
        });
    }
    if (cmd == "dl") {
        std::string path;
        ss >> path;
        return run_on_gfx([path] {
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return std::string("error: cannot open ") + path;
            const auto& log = f3d::last_commands();
            for (const auto& c : log) {
                if (c.draw_index >= 0) {
                    fprintf(f, "%*s%06X: %08X %08X  [draw %d] %s\n", c.depth * 2, "", c.addr, c.w0, c.w1, c.draw_index,
                            f3d::disasm(c.w0, c.w1).c_str());
                } else {
                    fprintf(f, "%*s%06X: %08X %08X  %s\n", c.depth * 2, "", c.addr, c.w0, c.w1,
                            f3d::disasm(c.w0, c.w1).c_str());
                }
            }
            fclose(f);
            return "wrote " + std::to_string(log.size()) + " commands to " + path;
        });
    }
    if (cmd == "tmem") {
        std::string path;
        ss >> path;
        return run_on_gfx([path] {
            FILE* f = fopen(path.c_str(), "w");
            if (!f) return std::string("error: cannot open ") + path;
            f3d::dump_tmem(f);
            fclose(f);
            return "wrote " + path;
        });
    }
    return "error: unknown command '" + cmd + "' (try help)";
}

void serve_client(socket_t c) {
    std::string buf;
    char tmp[1024];
    for (;;) {
        int n = recv(c, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string reply = handle(line);
            if (!reply.empty() && reply.back() != '\n') reply += '\n';
            reply += ".\n";
            send(c, reply.data(), (int)reply.size(), 0);
        }
    }
    CLOSESOCK(c);
}

void server_thread(int port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return;
    int yes = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, (sockaddr*)&addr, sizeof(addr)) != 0 || listen(s, 4) != 0) {
        fprintf(stderr, "[debug] could not listen on 127.0.0.1:%d\n", port);
        CLOSESOCK(s);
        return;
    }
    fprintf(stderr, "[debug] console on 127.0.0.1:%d (python tools/wdbg.py help)\n", port);
    fflush(stderr);
    for (;;) {
        socket_t c = accept(s, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        std::thread(serve_client, c).detach();
    }
}

} // namespace

void start(uint8_t* rdram) {
    g_rdram = rdram;
    if (const char* e = std::getenv("WETRIX_SNAPSHOT")) snapshots_on = atoi(e) != 0;
    static bool started = false;
    if (started) return;
    started = true;
    int port = 7464;
    if (const char* p = std::getenv("WETRIX_DEBUG_PORT")) port = atoi(p);
    if (port <= 0) return;
    std::thread(server_thread, port).detach();
}

void on_task(const OSTask* task) {
    if (snapshots_on && g_f3d_active) {
        Snapshot& sn = snaps[snap_head];
        sn.ram.assign(g_rdram, g_rdram + RDRAM_SIZE);
        sn.task = { task->t.data_ptr, task->t.ucode, task->t.ucode_data, true };
        snap_head = (snap_head + 1) % SNAPSHOTS;
        shown_slot = -1;
    }
    last_task.data_ptr = task->t.data_ptr;
    last_task.ucode = task->t.ucode;
    last_task.ucode_data = task->t.ucode_data;
    last_task.valid = true;
    g_task_count++;
    service_jobs();
    if (!g_paused) return;
    if (g_step_budget > 0) {
        g_step_budget--;
        return;
    }
    fprintf(stderr, "[debug] paused at task %u (dl 0x%08X)\n", g_task_count.load(), last_task.data_ptr);
    fflush(stderr);
    auto last_paint = std::chrono::steady_clock::now();
    while (g_paused && g_step_budget == 0) {
        {
            std::unique_lock<std::mutex> lk(pause_mutex);
            pause_cv.wait_for(lk, std::chrono::milliseconds(16));
        }
        service_jobs();
        if (g_repaint && std::chrono::steady_clock::now() - last_paint > std::chrono::milliseconds(100)) {
            g_repaint();
            last_paint = std::chrono::steady_clock::now();
        }
    }
    if (g_step_budget > 0) g_step_budget--;
}

void poll_gfx() { service_jobs(); }

void set_repaint(std::function<void()> fn) { g_repaint = std::move(fn); }
void set_f3d_active(bool active) { g_f3d_active = active; }
bool paused() { return g_paused; }

void toggle_freeze_dump() {
    if (g_paused) {
        handle("resume");
        fprintf(stderr, "[debug] F10: resumed\n");
        return;
    }
    std::thread([] {
        namespace fs = std::filesystem;
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
        const fs::path dir = fs::path("dumps") / stamp;
        std::error_code ec;
        fs::create_directories(dir, ec);

        handle("pause");
        // The pause takes effect at the next display list; wait for the task
        // counter to hold still (the gfx thread is then parked in on_task).
        uint32_t last = g_task_count;
        for (int quiet = 0, tries = 0; quiet < 4 && tries < 60; tries++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            const uint32_t n = g_task_count;
            quiet = (n == last) ? quiet + 1 : 0;
            last = n;
        }

        auto put = [&](const char* name, const std::string& text) {
            std::ofstream(dir / name, std::ios::binary) << text << "\n";
        };
        auto path = [&](const char* name) { return (dir / name).generic_string(); };
        std::string log;
        auto run = [&](const std::string& cmd) {
            const std::string r = handle(cmd);
            log += cmd + " -> " + r.substr(0, r.find('\n')) + "\n";
            return r;
        };

        put("status.txt", run("status"));
        run("rdram " + path("rdram.bin"));
        put("calls.txt", run("calls 300"));
        put("threads.txt", run("threads"));
        switch (render::mode()) {
            case render::Mode::Hard:
            case render::Mode::HardAB: run("shot-hard " + path("picture.png")); break;
            case render::Mode::Soft:
            case render::Mode::SoftAB: run("shot-soft " + path("picture.png")); break;
            default: break;
        }
        if (g_f3d_active) {
            run("shot " + path("picture-f3d.png"));
            run("dl " + path("dl.txt"));
        }
        put("dump.log", log);
        fprintf(stderr, "[debug] F10: frozen, dumped to %s (F10 again to resume)\n", fs::absolute(dir).generic_string().c_str());
    }).detach();
}

uint16_t injected_buttons(int pad) {
    if (pad < 0 || pad >= 4) return 0;
    std::lock_guard<std::mutex> lk(pad_mutex);
    uint16_t b = pads[pad].held;
    if (pads[pad].pulse && std::chrono::steady_clock::now() < pads[pad].pulse_until) b |= pads[pad].pulse;
    return b;
}

bool injected_stick(int pad, float* x, float* y) {
    if (pad < 0 || pad >= 4) return false;
    std::lock_guard<std::mutex> lk(pad_mutex);
    if (!pads[pad].stick || std::chrono::steady_clock::now() >= pads[pad].stick_until) return false;
    *x = pads[pad].sx;
    *y = pads[pad].sy;
    return true;
}

} // namespace wetrix::debug
