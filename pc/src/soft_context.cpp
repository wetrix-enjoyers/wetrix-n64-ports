// The soft renderer (Wetter's software RDP) as an ultramodern RendererContext.
// Wetter draws into RDRAM and composes the VI's picture; this file hands it the
// tasks and the VI registers, feeds the gameplay detector, and presents through
// SDL_Renderer.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include <ultramodern/ultra64.h>
#include <ultramodern/config.hpp>
#include <ultramodern/renderer_context.hpp>
#include <ultramodern/ultramodern.hpp>

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <wetter/wetter.h>

#include "wetrix_run.h"

extern "C" void wetrix_progress_tick();
extern "C" void wetrix_set_rdram(uint8_t* rdram);

extern "C" SDL_Window* wetrix_window();

namespace wetrix::soft_ctx {

namespace {

// The last composed frame, handed from the gfx thread to the main thread.
struct SharedFrame {
    std::mutex mutex;
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    bool ready = false;
};
SharedFrame g_frame;

std::atomic<bool> g_capture_requested{ false };

void exit_hook(uint64_t frame) { wetrix::exit_if_run_finished(frame); }

class SoftContext;
SoftContext* g_context = nullptr;

class SoftContext final : public ultramodern::renderer::RendererContext {
public:
    explicit SoftContext(uint8_t* rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;

        wetter::Hooks hooks;
        hooks.in_gameplay = &wetrix::in_gameplay;
        hooks.first_gameplay_frame = &wetrix::first_gameplay_frame;
        hooks.gameplay_frames = &wetrix::gameplay_frames;
        hooks.frame_composed = &exit_hook;
        // Wetrix picks its levels of detail through G_BRANCH_Z and is drawn with
        // the branch always taken (as f3d does); WETRIX_BRANCH_Z=real
        // runs the depth test instead.
        wetter::Options options;
        const char* branch = std::getenv("WETRIX_BRANCH_Z");
        options.force_branch_z = branch == nullptr || std::strcmp(branch, "real") != 0;
        renderer_ = wetter::Renderer::create(wetter::Backend::Soft, rdram, options, hooks);
        g_context = this;
    }

    ~SoftContext() override {
        if (g_context == this) g_context = nullptr;
    }

    wetter::Renderer& renderer() { return *renderer_; }

    bool valid() override { return renderer_ != nullptr; }

    void send_dl(const OSTask* task) override {
        wetrix_progress_tick();
        renderer_->run_task(static_cast<uint32_t>(task->t.data_ptr));
    }

    void update_screen() override {
        const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();

        // One frame's submitted triangles is what the gameplay detector sees.
        const uint64_t triangles = renderer_->triangles_submitted();
        wetrix::observe_frame(renderer_->frames() + 1u, triangles - frame_triangles_submitted_);
        frame_triangles_submitted_ = triangles;

        if (g_capture_requested.exchange(false)) renderer_->request_capture("capture.ppm");

        wetter::ViRegs regs;
        regs.origin = vi->VI_ORIGIN_REG;
        regs.width = vi->VI_WIDTH_REG;
        regs.status = vi->VI_STATUS_REG;
        regs.v_start = vi->VI_V_START_REG;
        regs.y_scale = vi->VI_Y_SCALE_REG;
        renderer_->compose(regs);

        const wetter::Frame f = renderer_->frame();
        std::lock_guard<std::mutex> lock(g_frame.mutex);
        const size_t count = size_t(f.stride) * size_t(f.height);
        g_frame.pixels.assign(f.pixels, f.pixels + count);
        g_frame.width = f.width;
        g_frame.height = f.height;
        g_frame.stride = f.stride;
        g_frame.ready = true;
    }

    bool update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                       const ultramodern::renderer::GraphicsConfig& new_config) override {
        (void)old_config;
        (void)new_config;
        return false;
    }

    void enable_instant_present() override {}
    void send_dummy_workload(uint32_t fb_address) override { (void)fb_address; }
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }
    void shutdown() override {}

private:
    std::unique_ptr<wetter::Renderer> renderer_;
    // The renderer's cumulative triangle count as of the last composed frame.
    uint64_t frame_triangles_submitted_ = 0;
};

}  // namespace

void* wetrix_soft_worker_handle(int i) {
    return g_context != nullptr ? g_context->renderer().worker_handle(i) : nullptr;
}

double wetrix_soft_bench(const uint8_t* ram, uint32_t dl, int n) {
    return g_context != nullptr ? g_context->renderer().bench(ram, dl, n) : -1.0;
}

bool wetrix_soft_capture(std::vector<uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> lock(g_frame.mutex);
    if (g_frame.width <= 0 || g_frame.pixels.empty()) return false;
    w = g_frame.width;
    h = g_frame.height;
    rgba.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        const uint32_t* row = g_frame.pixels.data() + (size_t)y * g_frame.stride;
        for (int x = 0; x < w; x++) {
            const uint32_t p = row[x];
            uint8_t* o = &rgba[((size_t)y * w + x) * 4];
            o[0] = (p >> 16) & 0xFF;
            o[1] = (p >> 8) & 0xFF;
            o[2] = p & 0xFF;
            o[3] = 255;
        }
    }
    return true;
}

extern "C" void wetrix_request_capture() {
    g_capture_requested.store(true);
}

extern "C" void wetrix_present_if_ready() {
    static SDL_Renderer* renderer = nullptr;
    static SDL_Texture* texture = nullptr;
    static int texture_w = 0, texture_h = 0;
    static bool failed = false;
    static bool reported = false;

    if (failed) return;

    SDL_Window* window = wetrix_window();
    if (window == nullptr) return;

    if (renderer == nullptr) {
        // VSync paces the window; the game's VI timing is simulated.
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (renderer == nullptr) {
            // Fall back to SDL's software renderer.
            renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
        }
        if (renderer == nullptr) {
            failed = true;
            std::fprintf(stderr, "[render] SDL_CreateRenderer failed: %s\n", SDL_GetError());
            std::fflush(stderr);
            return;
        }
        SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }

    int width = 0;
    int height = 0;
    {
        std::lock_guard<std::mutex> lock(g_frame.mutex);
        if (!g_frame.ready) return;
        g_frame.ready = false;
        width = g_frame.width;
        height = g_frame.height;
        const int rows = static_cast<int>(g_frame.pixels.size() / size_t(g_frame.stride));
        if (texture == nullptr || texture_w != g_frame.stride || texture_h != rows) {
            if (texture != nullptr) SDL_DestroyTexture(texture);
            texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                        g_frame.stride, rows);
            texture_w = g_frame.stride;
            texture_h = rows;
            if (texture == nullptr) {
                failed = true;
                std::fprintf(stderr, "[render] SDL_CreateTexture failed: %s\n", SDL_GetError());
                std::fflush(stderr);
                return;
            }
        }
        SDL_UpdateTexture(texture, nullptr, g_frame.pixels.data(), g_frame.stride * 4);
    }

    // The VI shows the framebuffer at 4:3 whatever its line count; fit that.
    int out_w = 0, out_h = 0;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);
    SDL_Rect dest{ 0, 0, out_w, out_h };
    if (out_w * 3 > out_h * 4) dest.w = out_h * 4 / 3;
    else dest.h = out_w * 3 / 4;
    dest.x = (out_w - dest.w) / 2;
    dest.y = (out_h - dest.h) / 2;

    const SDL_Rect source{ 0, 0, width, height };
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, &source, &dest);
    SDL_RenderPresent(renderer);

    if (!reported) {
        reported = true;
        std::fprintf(stderr, "[render] presenting %dx%d via %s\n", width, height, SDL_GetCurrentVideoDriver());
        std::fflush(stderr);
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext>
create_soft_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    // The stall probe reads the game's memory when it decides the game has hung,
    // and this is where the runtime first hands the pointer to the port.
    wetrix_set_rdram(rdram);

    (void)window_handle;    // SDL owns the window; the renderer draws into RDRAM.
    (void)developer_mode;
    return std::make_unique<SoftContext>(rdram);
}

}  // namespace wetrix::soft_ctx
