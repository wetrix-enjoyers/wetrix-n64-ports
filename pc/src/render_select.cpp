// Renderer selection: the f3d (Fast3D-lineage, GLES) renderer, Wetter's soft and
// hard, or a pair of them side by side. See include/render_select.h.
//
// In the A/B modes the two renderers see exactly the same display lists: the
// front context below hands each task to both before the runtime signals DP
// completion, so neither can see a list the other did not. Each draws into its
// own window.

#include "render_select.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <SDL.h>

#include "debug_server.h"
#include "ultramodern/ultramodern.hpp"
#include "f3d.h"

#include <wetter/wetter.h>

extern "C" void wetrix_progress_tick();
extern "C" void wetrix_set_rdram(uint8_t* rdram);

namespace wetrix::soft_ctx {
std::unique_ptr<ultramodern::renderer::RendererContext>
create_soft_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);
}

namespace wetrix::render {

namespace {

Mode g_mode = Mode::Fast3D;
bool g_mode_from_args = false;
SDL_Window* g_main_window = nullptr;
SDL_Window* g_f3d_window = nullptr;
int g_frame_vis = 3;   // --frame-vis / WETRIX_FRAME_VIS; 0 = unpaced

Mode parse_mode(const char* s, Mode fallback) {
    if (s == nullptr) return fallback;
    std::string v(s);
    if (v == "fast3d" || v == "f3d" || v == "gles") return Mode::Fast3D;
    if (v == "soft" || v == "own" || v == "software") return Mode::Soft;
    if (v == "softab" || v == "soft+f3d") return Mode::SoftAB;
    if (v == "hard" || v == "gpu") return Mode::Hard;
    if (v == "hardab" || v == "soft+hard") return Mode::HardAB;
    fprintf(stderr, "[render] unknown renderer '%s', keeping %s\n", s, mode_name(fallback));
    return fallback;
}

// Sets the context attributes SDL reads at SDL_GL_CreateContext. GLES 3.0 is
// tried first unless WETRIX_GL=core: it is the R36S's API, and running the
// desktop build on an ES context is what keeps the renderer honest about it.
void set_gl_attributes(bool gles) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    if (gles) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    } else {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    }
}

f3d::Options options_from_env() {
    f3d::Options o;
    if (const char* s = std::getenv("WETRIX_F3D_SCALE")) o.scale = std::max(1, atoi(s));
    if (const char* s = std::getenv("WETRIX_F3D_BRANCH_Z")) o.force_branch_z = strcmp(s, "real") != 0;
    if (const char* s = std::getenv("WETRIX_F3D_FILTER")) o.bilinear = strcmp(s, "point") != 0;
    // WETRIX_F3D_ASPECT: "16:9", "21:9", or a number; 4:3 when unset.
    if (const char* s = std::getenv("WETRIX_F3D_ASPECT")) {
        float num = 0, den = 0;
        if (sscanf(s, "%f:%f", &num, &den) == 2 && num > 0 && den > 0) o.aspect = num / den;
        else if (atof(s) > 0) o.aspect = (float)atof(s);
    }
    return o;
}

// ---------------------------------------------------------------------------
// The f3d renderer as an ultramodern RendererContext.
// ---------------------------------------------------------------------------
class F3DContext final : public ultramodern::renderer::RendererContext {
public:
    F3DContext(uint8_t* rdram, SDL_Window* window) : window_(window) {
        setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        if (window_ == nullptr) {
            fprintf(stderr, "[f3d] no GL window\n");
            return;
        }
        const char* want = std::getenv("WETRIX_GL");
        const bool try_es = want == nullptr || strcmp(want, "core") != 0;
        if (try_es) {
            set_gl_attributes(true);
            ctx_ = SDL_GL_CreateContext(window_);
            if (ctx_ != nullptr) gles_ = true;
            else fprintf(stderr, "[f3d] GLES 3.0 context unavailable (%s); trying GL 3.3 core\n", SDL_GetError());
        }
        if (ctx_ == nullptr) {
            set_gl_attributes(false);
            ctx_ = SDL_GL_CreateContext(window_);
        }
        if (ctx_ == nullptr) {
            fprintf(stderr, "[f3d] could not create a GL context: %s\n", SDL_GetError());
            return;
        }
        SDL_GL_MakeCurrent(window_, ctx_);
        // The VI thread paces presentation; vsync here would stall the gfx thread.
        SDL_GL_SetSwapInterval(0);
        if (!f3d::init(rdram, 8 * 1024 * 1024, options_from_env(), gles_)) {
            return;
        }
        setup_result = ultramodern::renderer::SetupResult::Success;
        ok_ = true;
        fprintf(stderr, "[f3d] renderer up (%s, scale %d)\n", gles_ ? "GLES 3.0" : "GL 3.3 core", f3d::options().scale);
        debug::set_f3d_active(true);
    }

    bool valid() override { return ok_; }

    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
        return false;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        if (dl_count_++ == 0) {
            fprintf(stderr, "[f3d] first display list 0x%08X (ucode 0x%08X data 0x%08X)\n", task->t.data_ptr,
                    task->t.ucode, task->t.ucode_data);
            fflush(stderr);
        }
        f3d::run_dl(task->t.data_ptr & 0x3FFFFFF, task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF);
    }

    void send_dummy_workload(uint32_t) override {}

    void update_screen() override {
        const auto* vi = ultramodern::renderer::get_vi_regs();
        last_vi_ = *vi;
        have_vi_ = true;
        repaint();
    }

    void repaint() {
        if (!have_vi_) return;
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        f3d::present(last_vi_.VI_ORIGIN_REG, last_vi_.VI_WIDTH_REG, last_vi_.VI_STATUS_REG, last_vi_.VI_X_SCALE_REG,
                     last_vi_.VI_Y_SCALE_REG, w, h);
        SDL_GL_SwapWindow(window_);
    }

    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return (float)f3d::options().scale; }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    bool gles_ = false;
    bool ok_ = false;
    uint32_t dl_count_ = 0;
    ultramodern::renderer::ViRegs last_vi_{};
    bool have_vi_ = false;
};

// ---------------------------------------------------------------------------
// Wetter's GPU renderer (hard) as an ultramodern RendererContext. Like f3d it
// owns a GL context on the main window; Wetter only issues GL calls into it.
// ---------------------------------------------------------------------------
void* gl_proc(const char* name) { return SDL_GL_GetProcAddress(name); }

class HardContext;
HardContext* g_hard = nullptr;

class HardContext final : public ultramodern::renderer::RendererContext {
public:
    HardContext(uint8_t* rdram, SDL_Window* window) : window_(window) {
        setup_result = ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        if (window_ == nullptr) {
            fprintf(stderr, "[hard] no GL window\n");
            return;
        }
        const char* want = std::getenv("WETRIX_GL");
        const bool try_es = want == nullptr || strcmp(want, "core") != 0;
        if (try_es) {
            set_gl_attributes(true);
            ctx_ = SDL_GL_CreateContext(window_);
            if (ctx_ != nullptr) gles_ = true;
            else fprintf(stderr, "[hard] GLES 3.0 context unavailable (%s); trying GL 3.3 core\n", SDL_GetError());
        }
        if (ctx_ == nullptr) {
            set_gl_attributes(false);
            ctx_ = SDL_GL_CreateContext(window_);
        }
        if (ctx_ == nullptr) {
            fprintf(stderr, "[hard] could not create a GL context: %s\n", SDL_GetError());
            return;
        }
        SDL_GL_MakeCurrent(window_, ctx_);
        SDL_GL_SetSwapInterval(0);   // the VI thread paces presentation

        wetter::Options options;
        options.gl_get_proc_address = &gl_proc;
        options.gles = gles_;
        options.scale = f3d::Options{}.scale;
        if (const char* s = std::getenv("WETRIX_HARD_SCALE")) options.scale = std::max(1, atoi(s));
        // WETRIX_HARD_COPYBACK=task: every drawn image back to RDRAM after each
        // task, for CPU reads of the framebuffer (texture reads are always synced).
        if (const char* s = std::getenv("WETRIX_HARD_COPYBACK")) options.copy_back_every_task = strcmp(s, "task") == 0;
        // WETRIX_HARD_ASPECT: "16:9", "21:9", or a number; 4:3 when unset.
        if (const char* s = std::getenv("WETRIX_HARD_ASPECT")) {
            float num = 0, den = 0;
            if (sscanf(s, "%f:%f", &num, &den) == 2 && num > 0 && den > 0) options.aspect = num / den;
            else if (atof(s) > 0) options.aspect = (float)atof(s);
        }
        // Wetrix is drawn with G_BRANCH_Z always taken (see soft_context.cpp).
        const char* branch = std::getenv("WETRIX_BRANCH_Z");
        options.force_branch_z = branch == nullptr || strcmp(branch, "real") != 0;
        renderer_ = wetter::Renderer::create(wetter::Backend::Hard, rdram, options);
        if (!renderer_) {
            fprintf(stderr, "[hard] renderer failed to start\n");
            return;
        }
        setup_result = ultramodern::renderer::SetupResult::Success;
        g_hard = this;
    }

    ~HardContext() override {
        if (g_hard == this) g_hard = nullptr;
    }

    wetter::Renderer* renderer() { return renderer_.get(); }

    bool valid() override { return renderer_ != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig&, const ultramodern::renderer::GraphicsConfig&) override {
        return false;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override { renderer_->run_task(static_cast<uint32_t>(task->t.data_ptr)); }

    void send_dummy_workload(uint32_t) override {}

    void update_screen() override {
        const auto* vi = ultramodern::renderer::get_vi_regs();
        wetter::ViRegs regs;
        regs.origin = vi->VI_ORIGIN_REG;
        regs.width = vi->VI_WIDTH_REG;
        regs.status = vi->VI_STATUS_REG;
        regs.v_start = vi->VI_V_START_REG;
        regs.y_scale = vi->VI_Y_SCALE_REG;
        renderer_->compose(regs);
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window_, &w, &h);
        renderer_->present(w, h);
        SDL_GL_SwapWindow(window_);
    }

    void shutdown() override {
        if (renderer_) renderer_->report();
    }
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

private:
    SDL_Window* window_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    bool gles_ = false;
    std::unique_ptr<wetter::Renderer> renderer_;
};

// ---------------------------------------------------------------------------
// Front: debug hooks and pacing around whichever backends are live.
// ---------------------------------------------------------------------------
class FrontContext final : public ultramodern::renderer::RendererContext {
public:
    // `primary` is the main window's renderer (null when f3d runs alone, in
    // `second`); `second` is the other window's, or the only one.
    FrontContext(std::unique_ptr<ultramodern::renderer::RendererContext> primary,
                 std::unique_ptr<ultramodern::renderer::RendererContext> second)
        : primary_(std::move(primary)), second_(std::move(second)) {
        auto& first = primary_ ? primary_ : second_;
        setup_result = first ? first->get_setup_result() : ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
        chosen_api = first ? first->get_chosen_api() : ultramodern::renderer::GraphicsApi::Auto;
        // While the game is paused nothing else calls update_screen, so the
        // console repaints the windows itself.
        debug::set_repaint([this] {
            if (primary_) primary_->update_screen();
            if (auto* f3d = dynamic_cast<F3DContext*>(second_.get())) f3d->repaint();
            else if (second_) second_->update_screen();
        });
    }

    bool valid() override {
        if (primary_ && !primary_->valid()) return false;
        if (second_ && !second_->valid()) {
            if (primary_) {
                fprintf(stderr, "[render] the second renderer failed to start; continuing with one\n");
                second_.reset();
                return true;
            }
            return false;
        }
        return primary_ || second_;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig& o, const ultramodern::renderer::GraphicsConfig& n) override {
        bool changed = false;
        if (primary_) changed |= primary_->update_config(o, n);
        if (second_) changed |= second_->update_config(o, n);
        return changed || !primary_;
    }

    void enable_instant_present() override {
        if (primary_) primary_->enable_instant_present();
    }

    void send_dl(const OSTask* task) override {
        wetrix_progress_tick();
        debug::on_task(task);
        const double arrive = pace_clock_us();
        pace();
        const double start = pace_clock_us();
        task_busy_ = true;
        if (primary_) primary_->send_dl(task);
        if (second_) second_->send_dl(task);
        task_busy_ = false;
        if (pace_log()) {
            // WETRIX_PACE_LOG: when each task arrived, when pacing let it start,
            // when drawing finished (microseconds since the first event).
            fprintf(stderr, "[pace] task %llu arrive %.0f start %.0f end %.0f\n",
                    static_cast<unsigned long long>(++tasks_), arrive, start, pace_clock_us());
        }
    }

    static bool pace_log() {
        static const bool on = std::getenv("WETRIX_PACE_LOG") != nullptr;
        return on;
    }
    static double pace_clock_us() {
        static const auto epoch = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - epoch).count();
    }

    // Wetrix steps its simulation once per frame, not per unit of time, so its
    // speed is its frame rate. On the console the RDP's drawing time held it to
    // about 3 VIs per frame; a renderer that finishes instantly lets it run a
    // frame every VI (3x speed). WETRIX_FRAME_VIS=<n> restores the budget: each
    // graphics task starts no sooner than n VI periods after the previous one.
    //
    // The budget is counted in VI ticks on the runtime's own VI clock (tick k
    // falls at start + k/60 s), so a task starts on a tick and a game frame lasts
    // a whole number of VIs. A task that arrives after its tick (the last frame
    // ran long) starts at once and counts from the tick it arrived in, rather
    // than rushing later frames to catch up.
    void pace() {
        static const int vis = [] {
            const char* e = std::getenv("WETRIX_FRAME_VIS");
            return e != nullptr ? std::max(0, atoi(e)) : g_frame_vis;
        }();
        if (vis == 0) return;
        using clock = std::chrono::high_resolution_clock;
        const clock::time_point start = ultramodern::get_start();
        const uint64_t rate = 60u * std::max<uint32_t>(1u, ultramodern::get_speed_multiplier());
        auto tick_time = [&](int64_t tick) {
            return start + std::chrono::duration_cast<clock::duration>(std::chrono::nanoseconds(tick * 1000000000LL / int64_t(rate)));
        };
        const int64_t now_tick =
            std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now() - start).count() * int64_t(rate) / 1000000000LL;
        if (last_task_tick_ >= 0 && now_tick < last_task_tick_ + vis) {
            last_task_tick_ += vis;
            ultramodern::sleep_until(tick_time(last_task_tick_));
        } else {
            last_task_tick_ = now_tick;
        }
    }

    void send_dummy_workload(uint32_t fb) override {
        if (primary_) primary_->send_dummy_workload(fb);
    }

    void update_screen() override {
        debug::poll_gfx();
        const auto* vi = ultramodern::renderer::get_vi_regs();
        const uint32_t origin = vi->VI_ORIGIN_REG & 0xFFFFFFu;
        // The picture only changes when the VI points somewhere new or a task
        // has drawn since; presenting the same picture again costs a buffer swap
        // that waits on the display (7-8 ms each on the R36S).
        // WETRIX_PRESENT_EVERY_VI=1 presents on every VI regardless.
        static const bool every_vi = std::getenv("WETRIX_PRESENT_EVERY_VI") != nullptr;
        const bool changed = origin != presented_origin_ || tasks_ != presented_tasks_;
        if (!changed && !every_vi) {
            ++presents_skipped_;
            return;
        }
        presented_origin_ = origin;
        presented_tasks_ = tasks_;
        const double begin = pace_clock_us();
        if (primary_) primary_->update_screen();
        if (second_) second_->update_screen();
        if (pace_log()) {
            // Each present: when it ran, how long it took, the buffer the VI
            // shows, how many tasks had finished, and presents skipped so far.
            fprintf(stderr, "[pace] vi %.0f took %.0f origin %06X after task %llu skipped %llu\n", begin,
                    pace_clock_us() - begin, origin, static_cast<unsigned long long>(tasks_),
                    static_cast<unsigned long long>(presents_skipped_));
        }
    }

    void shutdown() override {
        if (primary_) primary_->shutdown();
        if (second_) second_->shutdown();
    }

    uint32_t get_display_framerate() const override {
        return primary_ ? primary_->get_display_framerate() : second_->get_display_framerate();
    }

    float get_resolution_scale() const override {
        return primary_ ? primary_->get_resolution_scale() : second_->get_resolution_scale();
    }

private:
    std::unique_ptr<ultramodern::renderer::RendererContext> primary_;
    std::unique_ptr<ultramodern::renderer::RendererContext> second_;
    int64_t last_task_tick_ = -1;   // the VI tick the last task started on
    uint64_t tasks_ = 0;
    bool task_busy_ = false;
    uint32_t presented_origin_ = 0xFFFFFFFFu;
    uint64_t presented_tasks_ = ~0ull;
    uint64_t presents_skipped_ = 0;
};

} // namespace

void parse_args(int argc, char** argv) {
    g_mode = parse_mode(std::getenv("WETRIX_RENDERER"), Mode::Fast3D);
    for (int i = 1; i + 1 < argc; i++) {
        if (strcmp(argv[i], "--frame-vis") == 0) {
            g_frame_vis = std::max(0, atoi(argv[i + 1]));
        }
        if (strcmp(argv[i], "--renderer") == 0) {
            g_mode = parse_mode(argv[i + 1], g_mode);
            g_mode_from_args = true;
        }
    }
    fprintf(stderr, "[render] renderer: %s\n", mode_name(g_mode));
}

Mode mode() { return g_mode; }

const char* mode_name(Mode m) {
    switch (m) {
        case Mode::Fast3D: return "fast3d";
        case Mode::Soft: return "soft";
        case Mode::SoftAB: return "softab";
        case Mode::Hard: return "hard";
        case Mode::HardAB: return "hardab";
    }
    return "?";
}

bool main_window_is_gl() { return g_mode == Mode::Fast3D || g_mode == Mode::Hard; }

const char* main_window_title() {
    switch (g_mode) {
        case Mode::Fast3D: return "Wetrix - f3d (GLES)";
        case Mode::Soft:
        case Mode::SoftAB: return "Wetrix - soft";
        case Mode::Hard: return "Wetrix - hard (GLES)";
        case Mode::HardAB: return "Wetrix - soft";
    }
    return "Wetrix";
}

void window_rect(int slot, int& x, int& y, int& w, int& h) {
    SDL_Rect usable{ 0, 0, 1280, 720 };
    SDL_GetDisplayUsableBounds(0, &usable);
    const int panes = (g_mode == Mode::SoftAB || g_mode == Mode::HardAB) ? 2 : 1;
    // Leave room for title bars / borders, which SDL's size does not include.
    const int frame_w = 16, frame_h = 48, gap = 8;
    int avail_w = (usable.w - gap * (panes + 1)) / panes - frame_w;
    int avail_h = usable.h - frame_h - gap * 2;
    w = std::min(960, avail_w);
    h = w * 3 / 4;
    if (h > avail_h) {
        h = avail_h;
        w = h * 4 / 3;
    }
    if (const char* e = std::getenv("WETRIX_WINDOW")) {
        int ew = 0, eh = 0;
        if (sscanf(e, "%dx%d", &ew, &eh) == 2 && ew > 0 && eh > 0) {
            w = ew;
            h = eh;
        }
    }
    const int total = panes * (w + frame_w) + (panes - 1) * gap;
    x = usable.x + (usable.w - total) / 2 + slot * (w + frame_w + gap) + frame_w / 2;
    y = usable.y + (usable.h - h - frame_h) / 2 + frame_h - 8;
}

SDL_Window* create_f3d_window() {
    set_gl_attributes(std::getenv("WETRIX_GL") == nullptr || strcmp(std::getenv("WETRIX_GL"), "core") != 0);
    int x, y, w, h;
    window_rect(1, x, y, w, h);
    const char* title = g_mode == Mode::HardAB ? "Wetrix - hard (GLES)" : "Wetrix - f3d (GLES)";
    g_f3d_window = SDL_CreateWindow(title, x, y, w, h, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (g_f3d_window == nullptr) fprintf(stderr, "[render] f3d window failed: %s\n", SDL_GetError());
    return g_f3d_window;
}

void set_main_window(SDL_Window* w) {
    g_main_window = w;
    if (g_mode == Mode::Fast3D) g_f3d_window = w;
}

std::unique_ptr<ultramodern::renderer::RendererContext>
create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    wetrix_set_rdram(rdram);
    debug::start(rdram);
    if (g_mode == Mode::Soft || g_mode == Mode::SoftAB) {
        std::unique_ptr<ultramodern::renderer::RendererContext> f3d_ab;
        if (g_mode == Mode::SoftAB) f3d_ab = std::make_unique<F3DContext>(rdram, g_f3d_window);
        return std::make_unique<FrontContext>(
            wetrix::soft_ctx::create_soft_context(rdram, window_handle, developer_mode), std::move(f3d_ab));
    }
    if (g_mode == Mode::Hard) {
        return std::make_unique<FrontContext>(std::make_unique<HardContext>(rdram, g_main_window), nullptr);
    }
    if (g_mode == Mode::HardAB) {
        // soft in the main window, hard in the second: both see every list.
        return std::make_unique<FrontContext>(wetrix::soft_ctx::create_soft_context(rdram, window_handle, developer_mode),
                                              std::make_unique<HardContext>(rdram, g_f3d_window));
    }
    return std::make_unique<FrontContext>(nullptr, std::make_unique<F3DContext>(rdram, g_f3d_window));
}

bool hard_capture(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (g_hard == nullptr || g_hard->renderer() == nullptr) return false;
    const wetter::Frame f = g_hard->renderer()->frame();
    if (f.pixels == nullptr) return false;
    w = f.width;
    h = f.height;
    rgba.resize(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = f.pixels[size_t(y) * f.stride + x];
            uint8_t* o = &rgba[(size_t(y) * w + x) * 4];
            o[0] = (p >> 16) & 0xFF;
            o[1] = (p >> 8) & 0xFF;
            o[2] = p & 0xFF;
            o[3] = 255;
        }
    }
    return true;
}

} // namespace wetrix::render
