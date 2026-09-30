// Host application for the Wetrix recompilation.
//
// The shape of this is dictated by N64ModernRuntime, which is a library set
// rather than a program: it deliberately has no window, no renderer and no
// input source, and asks the project to hand all of those in as callbacks. So
// main() is mostly a wiring exercise, and the pieces that are missing are
// missing on purpose, not by oversight.
//
// STATUS: boots to the title screen, rendering through Wetter or f3d, playing audio
// through SDL and reading input. The two boot failures that used to park every
// thread -- the ROM's own PI DMA, and a scheduler thread the map had split in
// half -- are fixed; what is still missing, and how to look for the next one, is
// in README.md.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

// SDL creates the window, feeds the audio device and supplies controller input.
// SDL_MAIN_HANDLED keeps SDL from redefining main; SDL_syswm.h exposes the
// native HWND the runtime's window handle wants on Windows.
#define SDL_MAIN_HANDLED
// SDL's headers are on the include path flat (SDL.h, not SDL2/SDL.h).
#include <SDL.h>
#if !defined(_WIN32)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#if defined(_WIN32) || defined(__APPLE__)
// Native window handles. On Linux it would drag in Xlib, whose
// `#define None` breaks ultramodern's enums -- and the handle there is just the
// SDL_Window*.
#include <SDL_syswm.h>
#endif

#include <ultramodern/ultra64.h>
#include <ultramodern/ultramodern.hpp>
#include <librecomp/game.hpp>
#include <librecomp/rsp.hpp>

#include "recomp.h"
#include "rom_adopt.h"

// funcs.h deliberately is NOT included here. It declares the ROM's own `main`
// inside `extern "C"`, which collides with the C++ entry point in the same
// translation unit. Only the entrypoint is needed, so declare it directly.
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);

// Installs a Windows unhandled-exception filter that prints a dbghelp backtrace.
// Documented in src/crash_handler.cpp, which also explains why it exists: this
// box has no debugger that can attach to the port.
void wetrix_install_crash_handler();

// Arms the hang detector. A stall raises no exception, so the crash handler above
// never sees it; this samples thread instruction pointers instead. See
// src/stall_probe.cpp. Inert unless WETRIX_STALL_PROBE is set.
void wetrix_start_stall_probe();

// Hands the generated section tables in recomp_overlays.inl to the runtime.
// Without it func_map is empty and the first thread the ROM starts cannot be
// resolved. See src/recomp_overlays.cpp.
void wetrix_register_overlays();

// Renderer selection (f3d, soft, hard, or a pair) lives in src/render_select.cpp; the
// debug console in src/debug_server.cpp.
#include "render_select.h"
#include "debug_server.h"

// The recompiled audio microcode. RspUcodeFunc and RspExitReason are at global
// scope in librecomp/rsp.hpp, not in recomp::rsp, and the generated definition
// has no header of its own, so it is declared here. Declaring it inside the
// anonymous namespace below would not link: the generated function is at global
// scope, so an anonymous-namespace declaration mangles to a different name.
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr);

// ---------------------------------------------------------------------------
// Renderer and window.
//
// ultramodern has no renderer and no window of its own: it asks the frontend
// for a window, then asks for a RendererContext that will draw into it. The
// renderers live in src/render_select.cpp; this function is the window half of
// the handoff.
//
// On Windows the runtime wants the raw HWND plus the id of the thread that owns
// it.
// ---------------------------------------------------------------------------
// The software renderer (port/soft) presents through SDL_Renderer on the window
// thread and finds the window through this.
SDL_Window* g_main_sdl_window = nullptr;
extern "C" SDL_Window* wetrix_window() { return g_main_sdl_window; }
extern "C" void wetrix_present_if_ready();

namespace {

ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    static SDL_Window* window = nullptr;

    uint32_t flags = SDL_WINDOW_RESIZABLE;
    if (wetrix::render::main_window_is_gl()) flags |= SDL_WINDOW_OPENGL;
    // WETRIX_FULLSCREEN=1: cover the display (the R36S launcher sets it; on
    // kmsdrm there is no desktop to put a window on).
    if (const char* fs = std::getenv("WETRIX_FULLSCREEN"); fs != nullptr && fs[0] == '1') {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }

    int win_x, win_y, win_w, win_h;
    wetrix::render::window_rect(0, win_x, win_y, win_w, win_h);
    window = SDL_CreateWindow(wetrix::render::main_window_title(),
                              win_x, win_y, win_w, win_h, flags);
    if (window == nullptr) {
        fprintf(stderr, "[wetrix] SDL_CreateWindow failed: %s\n", SDL_GetError());
        std::exit(EXIT_FAILURE);
    }

    fprintf(stderr, "[wetrix] window created with SDL driver %s\n", SDL_GetCurrentVideoDriver());
    fflush(stderr);

    wetrix::render::set_main_window(window);
    g_main_sdl_window = window;
    if (wetrix::render::mode() == wetrix::render::Mode::SoftAB ||
        wetrix::render::mode() == wetrix::render::Mode::HardAB) {
        // The second renderer's own OpenGL window, beside the soft one: the live A/B.
        wetrix::render::create_f3d_window();
    }

    // SDL only tracks keyboard state for the *focused* window, so whatever holds
    // focus when the game starts decides whether keys work at all. Raise the
    // window before anything else can claim it; detach_owned_console has already
    // removed the console that a double-click would otherwise leave in front.
    SDL_RaiseWindow(window);

#if defined(_WIN32)
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
    SetForegroundWindow(wmInfo.info.win.window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.win.window, GetCurrentThreadId() };
#elif defined(__linux__) || defined(__ANDROID__)
    return ultramodern::renderer::WindowHandle{ window };
#elif defined(__APPLE__)
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    SDL_GetWindowWMInfo(window, &wmInfo);
    SDL_MetalView view = SDL_Metal_CreateView(window);
    return ultramodern::renderer::WindowHandle{ wmInfo.info.cocoa.window, SDL_Metal_GetLayer(view) };
#endif
}

// ---------------------------------------------------------------------------
// RSP microcode.
//
// ultramodern reimplements the OS, but not the RSP microcode. Graphics ucode is
// the renderer's job (the renderers emulate F3DEX), but *audio* ucode is a
// program the ROM carries and the RSP runs, so it has to be recompiled from the
// ROM by N64Recomp's RSPRecomp. ../wetrix_rsp.toml holds that config and
// documents how the microcode was located in the ROM; the output is built into
// this executable as `aspMain`.
//
// librecomp treats a nullptr here as fatal, so returning nothing for a task type
// the port has no ucode for exits the process. M_GFXTASK never reaches this
// callback -- the renderer consumes display lists -- so this is only ever asked
// for audio.
// ---------------------------------------------------------------------------

// The audio task is submitted with the game's command list in RDRAM and the
// microcode expects to find it in its own data memory: its dispatch loop sets
// $sp = 0x380 and reads each 8-byte command from DMEM[$sp], and its loader at
// IMEM 0x10D4 refills that window from the task's data_ptr in blocks clamped
// to 0x140 bytes.
//
// The port used to DMA the first window in here, on the theory that the OS
// primes it as part of osSpTaskLoad. Reading the ROM's own osSpTaskLoad
// (config/us/symbol_addrs_libultra.txt names 0x8005B91C; the disassembly is in
// asm/5C400.s) disproves that: it copies the task to a static buffer, converts
// the task's pointers to physical addresses, sets SP_STATUS and the RSP's PC,
// then DMAs the 0x40-byte task copy to DMEM 0xFC0 and the microcode text to
// IMEM 0x1000. It never touches DMEM 0x380. osSpTaskStartGo just sets
// SP_STATUS. So the microcode loads its own command window, and priming DMEM
// here only overwrites the microcode's own data segment -- the task still
// never completes. The port is therefore back to a plain call; the window
// loading has to happen the way the microcode itself does it.
// RDRAM, for the audio buffer fix below. Captured here because this callback is
// the first one the port owns that is handed the pointer, and it runs before any
// audio buffer is produced.
uint8_t* game_rdram = nullptr;

RspExitReason wetrix_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    game_rdram = rdram;
    return aspMain(rdram, ucode_addr);
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    if (task->t.type == M_AUDTASK) {
        return wetrix_aspMain;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Input.
//
// The runtime's `buttons` value is written straight into `OSContPad.button`, so
// it uses the N64 button bitmap (the same one RecompFrontend's input layer
// produces): A 0x8000, B 0x4000, Z 0x2000, Start 0x1000, D-pad 0x0800..0x0100,
// L 0x0020, R 0x0010, C-buttons 0x0008..0x0001. The analog stick is in [-1, 1]
// and the runtime squares it off to the N64's range itself.
//
// Keyboard:
//   stick  arrow keys or WASD      A  X        B  C        Z  Z or Left Shift
//   Start  Enter                   L  Q        R  E
//   C up/down/left/right  I/K/J/L
// Gamepad: left stick -> analog, d-pad -> d-pad, A/B -> A/B, X -> C-left,
// Y -> C-up, LB/RB -> L/R, Back -> Z, Start -> Start, right stick -> C-buttons.
// ---------------------------------------------------------------------------
constexpr uint16_t BTN_A = 0x8000;
constexpr uint16_t BTN_B = 0x4000;
constexpr uint16_t BTN_Z = 0x2000;
constexpr uint16_t BTN_START = 0x1000;
constexpr uint16_t BTN_DUP = 0x0800;
constexpr uint16_t BTN_DDOWN = 0x0400;
constexpr uint16_t BTN_DLEFT = 0x0200;
constexpr uint16_t BTN_DRIGHT = 0x0100;
constexpr uint16_t BTN_L = 0x0020;
constexpr uint16_t BTN_R = 0x0010;
constexpr uint16_t BTN_CUP = 0x0008;
constexpr uint16_t BTN_CDOWN = 0x0004;
constexpr uint16_t BTN_CLEFT = 0x0002;
constexpr uint16_t BTN_CRIGHT = 0x0001;

constexpr int MAX_PLAYERS = 4;

SDL_GameController* gamepads[MAX_PLAYERS] = {};

void refresh_controllers() {
    for (int player = 0; player < MAX_PLAYERS; player++) {
        if (gamepads[player] != nullptr && SDL_GameControllerGetAttached(gamepads[player])) {
            continue;
        }
        if (gamepads[player] != nullptr) {
            SDL_GameControllerClose(gamepads[player]);
            gamepads[player] = nullptr;
        }
        // Opening by joystick index is enough here: Wetrix is one or two
        // players, and SDL lists the connected pads in order.
        if (player < SDL_NumJoysticks() && SDL_IsGameController(player)) {
            gamepads[player] = SDL_GameControllerOpen(player);
        }
    }
}

void poll_input() {
    // Deliberately does NOT call SDL_PollEvent. SDL's event pump is thread
    // affine: the window is created on the thread that calls recomp::start, and
    // on Windows its messages are queued to that thread, so pumping from this
    // (game) thread would drain nothing and leave SDL_GetKeyboardState stale --
    // which is exactly what made every keypress invisible. The pump lives in
    // update_gfx, on the window thread. Controller enumeration is also done only
    // on that thread (see refresh_controllers), so nothing here writes the
    // shared gamepad table. What remains is per-poll pad state, which is driver
    // polling rather than event-queue work and is safe from this thread.
    SDL_GameControllerUpdate();
}

// SDL's video subsystem and the window both live on the thread that called
// recomp::start (create_window runs there, before the game thread even starts).
// recomp::start then spins a loop on that same thread calling update_gfx once
// per millisecond, and that is the one place a frontend gets to run code on the
// window thread. Pumping here is what makes keyboard and pad state visible to
// get_input on the game thread.
void* create_gfx() {
    return nullptr;
}

void update_gfx(void*) {
    if (wetrix::render::mode() == wetrix::render::Mode::Soft ||
        wetrix::render::mode() == wetrix::render::Mode::SoftAB ||
        wetrix::render::mode() == wetrix::render::Mode::HardAB) {
        wetrix_present_if_ready();
    }
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                std::exit(0);
            case SDL_WINDOWEVENT:
                // The A/B modes have two windows; closing either ends the game.
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) std::exit(0);
                break;
            case SDL_KEYDOWN:
                // F10: freeze and dump the game's state; again to resume.
                if (event.key.keysym.sym == SDLK_F10 && event.key.repeat == 0) wetrix::debug::toggle_freeze_dump();
                break;
            case SDL_CONTROLLERDEVICEADDED:
            case SDL_CONTROLLERDEVICEREMOVED:
                refresh_controllers();
            default:
                break;
        }
    }
}

uint16_t keyboard_buttons() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t buttons = 0;
    if (keys[SDL_SCANCODE_X])           buttons |= BTN_A;
    if (keys[SDL_SCANCODE_C])           buttons |= BTN_B;
    if (keys[SDL_SCANCODE_Z] || keys[SDL_SCANCODE_LSHIFT]) buttons |= BTN_Z;
    if (keys[SDL_SCANCODE_RETURN])      buttons |= BTN_START;
    if (keys[SDL_SCANCODE_Q])           buttons |= BTN_L;
    if (keys[SDL_SCANCODE_E])           buttons |= BTN_R;
    if (keys[SDL_SCANCODE_I])           buttons |= BTN_CUP;
    if (keys[SDL_SCANCODE_K])           buttons |= BTN_CDOWN;
    if (keys[SDL_SCANCODE_J])           buttons |= BTN_CLEFT;
    if (keys[SDL_SCANCODE_L])           buttons |= BTN_CRIGHT;

    // -----------------------------------------------------------------------
    // TEMP PROBE -- answers "is the keyboard dead, or is the window unfocused?".
    //
    // SDL_GetKeyboardState only reflects the window that holds keyboard focus, so
    // holding a key while another window is focused produces nothing here. If this
    // never prints while a key is genuinely held over the game window, the fault is
    // in event delivery, not in the mapping above. If it prints, the mapping is
    // fine and the original report was a focus problem.
    // -----------------------------------------------------------------------
    static int reported = 0;
    if (buttons != 0 && reported < 8) {
        reported++;
        fprintf(stderr, "[keyboard] buttons=0x%04X  keyboard_focus=%s  video_driver=%s\n",
                buttons, SDL_GetKeyboardFocus() != nullptr ? "yes" : "none",
                SDL_GetCurrentVideoDriver());
        fflush(stderr);
    }

    return buttons;
}

void keyboard_stick(float* x, float* y) {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    float sx = 0.0f;
    float sy = 0.0f;
    if (keys[SDL_SCANCODE_UP] || keys[SDL_SCANCODE_W])    sy += 1.0f;
    if (keys[SDL_SCANCODE_DOWN] || keys[SDL_SCANCODE_S])  sy -= 1.0f;
    if (keys[SDL_SCANCODE_RIGHT] || keys[SDL_SCANCODE_D]) sx += 1.0f;
    if (keys[SDL_SCANCODE_LEFT] || keys[SDL_SCANCODE_A])  sx -= 1.0f;
    *x = sx;
    *y = sy;
}

// ---------------------------------------------------------------------------
// TEMP PROBE -- unattended input. Remove once the crash past the title screen is
// understood.
//
// The interesting failure only happens after the title screen, which takes
// repeated A presses, and a human cannot sit there tapping while a
// build-analyse-fix loop runs. With WETRIX_AUTOPRESS in the environment this taps
// A for a quarter second every 10 seconds, and taps Start once at 2s, so the same
// path runs unattended and reproducibly.
//
// Gated on the environment so an ordinary run is unaffected.
// ---------------------------------------------------------------------------
uint16_t autopress_buttons() {
    static const bool enabled = std::getenv("WETRIX_AUTOPRESS") != nullptr;
    if (!enabled) {
        return 0;
    }

    static const auto start = std::chrono::steady_clock::now();
    const double t =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    uint16_t buttons = 0;
    if (t >= 2.0 && t < 2.4) {
        buttons |= BTN_START;
    }
    if (t >= 3.0 && std::fmod(t, 10.0) < 0.25) {
        buttons |= BTN_A;
    }

    static double announced = -1000.0;
    if (buttons != 0 && t - announced >= 5.0) {
        announced = t;
        fprintf(stderr, "[autopress] t=%.1fs  buttons=0x%04X\n", t, buttons);
        fflush(stderr);
    }
    return buttons;
}

bool get_input(int controller_num, uint16_t* buttons, float* x, float* y) {
    if (controller_num < 0 || controller_num >= MAX_PLAYERS) {
        return false;
    }

    uint16_t result_buttons = 0;
    float result_x = 0.0f;
    float result_y = 0.0f;

    if (controller_num == 0) {
        result_buttons |= keyboard_buttons();
        keyboard_stick(&result_x, &result_y);
        result_buttons |= autopress_buttons();   // TEMP PROBE
    }

    // Input injected over the debug console (press / hold / stick).
    result_buttons |= wetrix::debug::injected_buttons(controller_num);
    wetrix::debug::injected_stick(controller_num, &result_x, &result_y);

    if (SDL_GameController* pad = gamepads[controller_num]; pad != nullptr) {
        auto pressed = [pad](SDL_GameControllerButton button) {
            return SDL_GameControllerGetButton(pad, button) != 0;
        };

        if (pressed(SDL_CONTROLLER_BUTTON_A)) result_buttons |= BTN_A;
        if (pressed(SDL_CONTROLLER_BUTTON_B)) result_buttons |= BTN_B;
        if (pressed(SDL_CONTROLLER_BUTTON_X)) result_buttons |= BTN_CLEFT;
        if (pressed(SDL_CONTROLLER_BUTTON_Y)) result_buttons |= BTN_CUP;
        if (pressed(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  result_buttons |= BTN_L;
        if (pressed(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) result_buttons |= BTN_R;
        if (pressed(SDL_CONTROLLER_BUTTON_BACK))   result_buttons |= BTN_Z;
        if (pressed(SDL_CONTROLLER_BUTTON_START))  result_buttons |= BTN_START;
        if (pressed(SDL_CONTROLLER_BUTTON_DPAD_UP))    result_buttons |= BTN_DUP;
        if (pressed(SDL_CONTROLLER_BUTTON_DPAD_DOWN))  result_buttons |= BTN_DDOWN;
        if (pressed(SDL_CONTROLLER_BUTTON_DPAD_LEFT))  result_buttons |= BTN_DLEFT;
        if (pressed(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) result_buttons |= BTN_DRIGHT;

        auto axis = [pad](SDL_GameControllerAxis which) {
            return SDL_GameControllerGetAxis(pad, which) / 32767.0f;
        };

        // Either analog trigger (L2/R2 on a handheld, LT/RT on a pad) is the N64's Z.
        if (axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f || axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f) {
            result_buttons |= BTN_Z;
        }

        result_x += axis(SDL_CONTROLLER_AXIS_LEFTX);
        result_y -= axis(SDL_CONTROLLER_AXIS_LEFTY);   // SDL is +down, N64 is +up

        constexpr float c_threshold = 0.5f;
        float c_right = axis(SDL_CONTROLLER_AXIS_RIGHTX);
        float c_up = -axis(SDL_CONTROLLER_AXIS_RIGHTY);
        if (c_up > c_threshold)    result_buttons |= BTN_CUP;
        if (c_up < -c_threshold)   result_buttons |= BTN_CDOWN;
        if (c_right > c_threshold) result_buttons |= BTN_CRIGHT;
        if (c_right < -c_threshold) result_buttons |= BTN_CLEFT;
    }

    *buttons = result_buttons;
    *x = result_x;
    *y = result_y;
    return true;
}

ultramodern::input::connected_device_info_t
get_connected_device_info(int controller_num) {
    ultramodern::input::connected_device_info_t info{};
    if (controller_num >= 0 && controller_num < MAX_PLAYERS) {
        // Always report a pad in every port: input is synthesised from the
        // keyboard for player 1 and from any open gamepad, so "nothing is
        // plugged in" would be a lie and would lock the game out of input.
        info.connected_device = ultramodern::input::Device::Controller;
        info.connected_pak = ultramodern::input::Pak::RumblePak;
    }
    return info;
}

void set_rumble(int controller_num, bool rumble) {
    if (controller_num < 0 || controller_num >= MAX_PLAYERS) {
        return;
    }
    if (SDL_GameController* pad = gamepads[controller_num]; pad != nullptr) {
        SDL_GameControllerRumble(pad, rumble ? 0xFFFF : 0, rumble ? 0xFFFF : 0,
                                 rumble ? 5000 : 0);
    }
}

// ---------------------------------------------------------------------------
// Audio output.
//
// ultramodern deliberately owns no audio device: it runs the game's audio
// microcode, hands the resulting buffer to the AI, and calls queue_samples().
// What happens to those samples is the port's business, and until now the answer
// was "nothing" -- they were dropped on the floor, and get_frames_remaining()
// reported zero so the AI looked like it drained instantly. The game saw an AI
// that consumed everything and accepted everything, which is why it never
// complained, and why the port appeared to have working audio.
//
// SDL's queued-audio API is the whole implementation. There is no callback and no
// mixing: whichever thread the runtime produces buffers on calls
// SDL_QueueAudio, and the device consumes from that queue. SDL_QueueAudio and
// SDL_GetQueuedAudioSize are both thread-safe, which matters because the buffers
// do not arrive on the main thread.

SDL_AudioDeviceID audio_device = 0;

// How many buffers were refused because the queue was already at the cap. This
// is the number that says whether the game is feeding the AI faster than the
// device plays: a healthy run keeps it near zero.
size_t dropped_buffers = 0;

// Total bytes handed to the device, and the moment the device opened. Together
// with the queue depth these give the device's real consumption rate, rather than
// the one it was asked for.
uint64_t queued_bytes_total = 0;
std::chrono::steady_clock::time_point audio_opened_at = std::chrono::steady_clock::now();

// The largest single buffer the game has asked the AI for. osAiGetLength() is
// clamped to this, because a real AI can only be holding one submitted buffer at
// a time, so no larger number is one the game's buffer arithmetic is prepared
// for. Learned from the buffers themselves rather than hardcoded. The full
// reasoning is written out with get_frames_remaining() below.
static size_t ai_max_buffer_frames = 0;

// How shallow the AI's backlog ever gets, and how often it is empty when the game
// asks. This is the number that says whether the audio is actually continuous: a
// queue that reaches zero means the device ran out, which is an audible gap, and
// it would mean the rate the game is producing at is below the rate the device is
// playing at.
size_t ai_min_queued_frames = 0;
size_t ai_empty_reports = 0;

// Per-buffer and periodic audio stats. Off by default: the steady state is a
// single reassuring line every five seconds, which is useful while working on the
// audio path and noise in every other run. WETRIX_AUDIO_LOG turns it on, matching
// the other probes.
static const bool audio_log = std::getenv("WETRIX_AUDIO_LOG") != nullptr;

// The rate the game asked for, remembered rather than acted on immediately.
// ultramodern calls set_audio_frequency(48000) during init as a placeholder and
// the game replaces it with the real rate shortly after, so opening a device on
// the first call would open one at the wrong frequency and immediately reopen it.
// The device is therefore opened on the first buffer, by which point the rate is
// the real one.
uint32_t audio_frequency = 48000;

void close_audio_device() {
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
    }
}

void open_audio_device(uint32_t frequency) {
    close_audio_device();

    SDL_AudioSpec wanted{};
    wanted.freq = static_cast<int>(frequency);
    // The N64's AI is big-endian; ultramodern has already byte-swapped the data
    // by the time it reaches here (see queue_audio_buffer), so the host's own
    // format is the right one.
    wanted.format = AUDIO_S16SYS;
    wanted.channels = 2;
    // ~11ms at 48kHz. Small enough that input feels immediate, large enough not
    // to underrun when the game thread stalls for a frame.
    wanted.samples = 512;

    SDL_AudioSpec obtained{};
    audio_device = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, 0);

    if (audio_device == 0) {
        fprintf(stderr, "[audio] no output device, continuing silent: %s\n", SDL_GetError());
        return;
    }

    // A mismatch here is not corrected for: the queue is interpreted in the
    // device's format, so a rate change would shift the pitch rather than fail.
    // Worth knowing about, so say it rather than let it be a mystery.
    if (obtained.freq != wanted.freq || obtained.channels != wanted.channels) {
        fprintf(stderr, "[audio] device format differs: asked %d Hz/%dch, got %d Hz/%dch\n",
                wanted.freq, wanted.channels, obtained.freq, obtained.channels);
    }
    else {
        fprintf(stderr, "[audio] output device open: %d Hz, %d channels\n",
                obtained.freq, obtained.channels);
    }

    audio_opened_at = std::chrono::steady_clock::now();
    queued_bytes_total = 0;
    SDL_PauseAudioDevice(audio_device, 0);
}

// The buffer address this ROM hands the AI has to be repaired before it can be
// read, and nothing noticed until SDL actually read it for the first time.
//
// librecomp's osAiSetNextBuffer_recomp passes the ROM's own address straight to
// queue_audio_buffer, which converts it with TO_PTR -- `rdram[var -
// 0xFFFFFFFF80000000]`, i.e. KSEG0 only. This ROM passes a *physical* address
// there instead, so the conversion adds 0x80000000 to it and the pointer lands
// 2GB past the end of RDRAM. On a real console both forms reach the same memory,
// which is why the game can do it at all.
//
// It went unnoticed because the previous implementation threw the samples away
// without looking at them. A dropped buffer cannot fault.
//
// The repair is to undo exactly what was applied: if the pointer is outside
// RDRAM but subtracting 0x80000000 puts it inside, that is the same buffer, one
// wrongly-applied translation away. This is the third bug of this exact shape in
// this port (see the entrypoint sign-extension note above and osPiRawReadIo in
// ultra_shims.cpp): the ROM assumes the hardware's address folding, and every
// place that assumes KSEG0 has to be told otherwise.
int16_t* host_audio_pointer(int16_t* samples, size_t count) {
    constexpr uintptr_t RDRAM_SIZE = 8u * 1024u * 1024u;

    if (samples == nullptr || game_rdram == nullptr) {
        return nullptr;
    }

    const uintptr_t base = reinterpret_cast<uintptr_t>(game_rdram);
    const uintptr_t end = base + RDRAM_SIZE;
    const size_t bytes = count * sizeof(int16_t);

    const uintptr_t raw = reinterpret_cast<uintptr_t>(samples);
    if (raw >= base && raw + bytes <= end) {
        return samples;   // KSEG0 -- the ordinary case
    }

    const uintptr_t corrected = raw - 0x80000000u;
    if (corrected >= base && corrected + bytes <= end) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            fprintf(stderr,
                    "[audio] AI buffer address 0x%llX is physical, not KSEG0; "
                    "corrected to 0x%llX (RDRAM base 0x%llX)\n",
                    static_cast<unsigned long long>(raw),
                    static_cast<unsigned long long>(corrected),
                    static_cast<unsigned long long>(base));
            fflush(stderr);
        }
        return reinterpret_cast<int16_t*>(corrected);
    }

    static bool complained = false;
    if (!complained) {
        complained = true;
        fprintf(stderr,
                "[audio] dropping a buffer: address 0x%llX, rdram 0x%llX, "
                "delta %+lld bytes\n",
                static_cast<unsigned long long>(raw),
                static_cast<unsigned long long>(base),
                static_cast<long long>(raw) - static_cast<long long>(base));
        fflush(stderr);
    }
    return nullptr;
}

void queue_samples(int16_t* samples, size_t count) {
    static size_t buffers = 0;
    ++buffers;

    if (audio_device == 0) {
        open_audio_device(audio_frequency);
        if (audio_device == 0) {
            return;
        }
    }

    // The largest single buffer the game has asked for. osAiGetLength() is clamped
    // to this (see get_frames_remaining), because that is the ceiling the real AI
    // has: it can only be holding one submitted buffer at a time.
    const size_t frames = count / 2;
    if (frames > ai_max_buffer_frames) {
        ai_max_buffer_frames = frames;
    }

    // The first few buffers are logged with their offset into RDRAM and their
    // size, because that is the shape that made both the address repair and the
    // reported-remaining clamp visible. A healthy run opens with one 1248-sample
    // buffer and then settles on 704 samples (352 stereo frames) forever, cycling
    // through a few offsets. Behind WETRIX_AUDIO_LOG, because an ordinary run has
    // nothing to say about its audio.
    if (audio_log && buffers <= 6 && game_rdram != nullptr) {
        const long long delta = static_cast<long long>(reinterpret_cast<uintptr_t>(samples)) -
                                static_cast<long long>(reinterpret_cast<uintptr_t>(game_rdram));
        // The queue depth is printed as "before", because that is the value the
        // game just read back through osAiGetLength when it decided this buffer's
        // size. Size plus depth is the whole feedback loop in one line.
        const size_t queue_before = SDL_GetQueuedAudioSize(audio_device) / (2 * sizeof(int16_t));
        fprintf(stderr,
                "[audio] buffer %zu: offset %+lld bytes, size %zu samples (%zu frames), "
                "queue %zu frames\n",
                buffers, delta, count, frames, queue_before);
        fflush(stderr);
    }

    int16_t* host_samples = host_audio_pointer(samples, count);
    if (host_samples == nullptr) {
        return;
    }

    // Frames are stereo pairs, because the N64's audio is stereo and that is what
    // the device was opened with.
    constexpr size_t BYTES_PER_FRAME = 2 * sizeof(int16_t);
    const size_t bytes = count * sizeof(int16_t);

    // Keep latency bounded. In steady state the game paces itself and this never
    // fires; what it catches is the host falling behind -- a long frame, a
    // debugger, a slow disk -- where the queue would otherwise stay seconds deep
    // and audio would play long after the frame that produced it. 250ms is
    // several times the 25-35ms steady state, so it is a backstop rather than
    // part of the pacing.
    constexpr size_t MAX_LATENCY_MS = 250;
    const size_t max_bytes =
        static_cast<size_t>(audio_frequency) * BYTES_PER_FRAME * MAX_LATENCY_MS / 1000;
    if (SDL_GetQueuedAudioSize(audio_device) > max_bytes) {
        ++dropped_buffers;
        return;
    }

    SDL_QueueAudio(audio_device, host_samples, static_cast<uint32_t>(bytes));
    queued_bytes_total += bytes;

    if (buffers == 1) {
        fprintf(stderr, "[audio] first buffer queued (%zu samples)\n", count);
        fflush(stderr);
    }
    else if (audio_log && buffers % 300 == 0) {
        // The steady state is what matters for whether this is really audible:
        // a queue that keeps draining to zero means gaps, and a drop count that
        // keeps climbing means the game is outrunning the device and chunks of
        // audio are being discarded.
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - audio_opened_at)
                .count();

        // What the device has actually consumed, derived from what was handed to
        // it minus what is still sitting in the queue. Compared against
        // `audio_frequency * 4` -- the rate it was opened at -- this says whether
        // a mismatch is the game over-producing or the device under-consuming.
        const uint64_t consumed = queued_bytes_total - SDL_GetQueuedAudioSize(audio_device);

        fprintf(stderr,
                "[audio] t=%.1fs %zu buffers seen, %zu dropped, %llu bytes fed "
                "(%.0f B/s in), device consuming %.0f B/s of %u expected, queue ~%.0fms, "
                "min %zu frames, %zu asks saw an empty AI\n",
                elapsed, buffers, dropped_buffers,
                static_cast<unsigned long long>(queued_bytes_total),
                queued_bytes_total / elapsed, consumed / elapsed,
                audio_frequency * 4, 
                1000.0 * SDL_GetQueuedAudioSize(audio_device) / (4.0 * audio_frequency),
                ai_min_queued_frames, ai_empty_reports);
        ai_min_queued_frames = 0;
        ai_empty_reports = 0;
        fflush(stderr);
    }
}

// The AI's view of how much audio is still unplayed, which is what the game
// paces its own synthesis against. It is not advisory -- the game computes each
// buffer's length as `target - osAiGetLength()`, so this number *is* the throttle,
// and getting it wrong changes how much audio the game produces.
//
// Reporting zero is what the previous version did, and it is wrong in a way that
// is easy to miss: with the AI always claiming to be empty, the game asks for a
// full target-sized buffer (624 stereo frames, 28ms) on every 16.6ms video frame.
// That is 37.4kHz of audio for a 22050Hz device -- 1.7x too fast -- and the port
// only looked healthy because the surplus was being thrown away: 41% of all
// buffers never reached the device.
//
// Reporting the real backlog makes the game pace itself correctly. Measured on
// this ROM: the ask settles at 352 frames per video frame (about 21kHz, i.e. the
// rate the game itself asked for), the queue holds steady at 25-35ms, and nothing
// is dropped.
//
// The one thing that must never happen is reporting *more* than the hardware
// could. On a console the AI holds the buffer it is playing, so osAiGetLength()
// can never report more than the length of the buffer the game itself submitted.
// Exceed that and the subtraction above goes negative, and this ROM will then
// hand osAiSetNextBuffer a length of 0xFFFFF8C0 (-1856 bytes): it asks the
// synthesizer to fill a negative size, and its own bookkeeping has gone
// inconsistent. Clamping the report to the largest buffer the game has actually
// submitted is therefore the hardware's own ceiling rather than a fudge, and it
// is what the host audio device's warm-up needs: the device consumes nothing for
// the first few frames after it opens, so the queue briefly runs deeper than the
// game's model allows. That transient is exactly what produced the negative
// length above before the clamp existed.
size_t get_frames_remaining() {
    if (audio_device == 0) {
        return 0;
    }

    constexpr size_t BYTES_PER_FRAME = 2 * sizeof(int16_t);
    const size_t queued = SDL_GetQueuedAudioSize(audio_device) / BYTES_PER_FRAME;

    // Hold back one video frame's worth of the report, so the game aims at a
    // backlog that is one frame deeper than the device strictly needs. Reporting
    // the bare truth leaves the queue touching empty on roughly one frame in
    // seven, which is where gaps come from; the cushion is the only lever that
    // moves the game's target backlog, because the length it asks for is
    // `target - <this number>`. It costs one frame (~17ms) of extra latency.
    const size_t cushion = audio_frequency / 60;
    const size_t reported = queued > cushion ? queued - cushion : 0;

    if (queued == 0) {
        ++ai_empty_reports;
    }
    if (ai_min_queued_frames == 0 || queued < ai_min_queued_frames) {
        ai_min_queued_frames = queued;
    }

    if (ai_max_buffer_frames == 0 || reported <= ai_max_buffer_frames) {
        return reported;
    }

    static bool reported_clamp = false;
    if (!reported_clamp) {
        reported_clamp = true;
        fprintf(stderr,
                "[audio] AI backlog %zu frames is deeper than one buffer (%zu); "
                "reporting the hardware ceiling\n",
                reported, ai_max_buffer_frames);
        fflush(stderr);
    }
    return ai_max_buffer_frames;
}

void set_audio_frequency(uint32_t freq) {
    if (freq == 0 || freq == audio_frequency) {
        return;
    }

    audio_frequency = freq;

    // Only reopen a device that is already running. On the first call this is a
    // no-op and the device opens at this rate instead; afterwards it means the
    // game changed the sample rate mid-run, which does happen.
    if (audio_device != 0) {
        fprintf(stderr, "[audio] sample rate changed to %u Hz; reopening device\n", freq);
        open_audio_device(freq);
    }
}

// Hand the emulated machine a chance to run, for a ROM loop that would otherwise
// spin forever without giving up the only emulated CPU there is.
//
// Called from the port's func_8004C1A0 (src/replaced_funcs.cpp), which is the
// condition of every sound-drain wait in the game -- nine call sites, all shaped
// as "purge the voices, then loop until the pool settles". The settling is done
// by the audio thread a frame later, and this runtime never preempts: it hands
// off only from a yield or a blocking call (see ultramodern/src/scheduling.cpp),
// so a thread that spins parks the audio thread along with everything else. The
// `ignored` entry for func_8004C1A0 in wetrix.toml has the full account.
//
// The timed form is the point. wait_for_external_message() blocks until some
// external event arrives, which would turn a wait the ROM expects to end into a
// hang of a different kind; the timed form delivers a pending message if there
// is one -- the messages are what make a blocked thread runnable -- and
// otherwise just bounds each poll to a millisecond. check_running_queue() then
// hands off to the highest-priority runnable thread, which is the audio thread
// when it has work.
extern "C" void wetrix_wait_handoff(uint8_t* rdram) {
    ultramodern::wait_for_external_message_timed(rdram, 1);
    ultramodern::check_running_queue(rdram);
}

void vi_callback() {}
void gfx_init_callback() {}

void message_box(const char* msg) { fprintf(stderr, "[wetrix] %s\n", msg); }

// Every game thread the runtime has started. Filled in by get_game_thread_name
// below and described where it is printed, after the namespace. Small and fixed
// because it is written from whichever thread is starting, with no lock: a slot
// is claimed by incrementing a counter, and a duplicate is impossible because the
// runtime starts each thread exactly once.
struct GameThreadRecord {
    uint32_t host_id;
    int32_t game_id;
    int32_t priority;
    const OSThread* thread;
};

constexpr size_t MAX_GAME_THREADS = 64;
GameThreadRecord g_game_threads[MAX_GAME_THREADS];
std::atomic<size_t> g_game_thread_count{ 0 };

// OSThread::state, named. The values are the runtime's own OSThreadState enum
// (STOPPED, QUEUED, RUNNING, BLOCKED), so this is a lookup and not an
// interpretation.
const char* game_thread_state_name(uint16_t state) {
    switch (static_cast<OSThreadState>(state)) {
    case STOPPED:
        return "STOPPED";
    case QUEUED:
        return "QUEUED";
    case RUNNING:
        return "RUNNING";
    case BLOCKED:
        return "BLOCKED";
    default:
        return "unknown";
    }
}

// Called by the runtime from each game thread's own start routine, so it is the
// only callback that observes thread creation and doubles as the progress probe:
// if the ROM's boot code spawned threads, they show up here.
//
// It is also the only place a game thread's *host* thread id is available, which
// is why the record below exists. It runs on the thread being started (see
// _thread_func in ultramodern/src/threads.cpp), so GetCurrentThreadId() here is
// that thread's host id -- the same id src/trace.cpp files its ring under and the
// stall probe prints for each thread it samples. Keeping it is what lets the
// three lists be joined into one sentence: ROM thread 8 is host thread 4321, it
// is parked here, and this is the call it was in.
std::string get_game_thread_name(const OSThread* thread) {
    fprintf(stderr, "[wetrix] thread created: id=%d pri=%d\n",
            thread != nullptr ? thread->id : -1,
            thread != nullptr ? thread->priority : -1);
    fflush(stderr);

    const size_t slot = g_game_thread_count.fetch_add(1, std::memory_order_relaxed);
    if (slot < MAX_GAME_THREADS) {
#if defined(_WIN32)
        g_game_threads[slot].host_id = static_cast<uint32_t>(GetCurrentThreadId());
#else
        g_game_threads[slot].host_id = static_cast<uint32_t>(syscall(SYS_gettid));
#endif
        g_game_threads[slot].game_id = thread != nullptr ? thread->id : -1;
        g_game_threads[slot].priority = thread != nullptr ? thread->priority : -1;
        g_game_threads[slot].thread = thread;
    }

    return "game";
}

} // namespace

// Printed by the stall probe next to the sampled instruction pointers and the
// trace rings. The three are meant to be read together: this says which ROM
// thread each host thread is and what the ROM's scheduler believes it is doing,
// the sample says where the process actually is, and the rings say how each
// thread got there.
//
// state and queue are the ROM's own fields, read the way the runtime's scheduler
// reads them, so no translation is involved: BLOCKED with a queue means the thread
// is parked on that message queue, and QUEUED means it surrendered the CPU and is
// waiting for someone to resume it. A thread the ROM created but never started
// does not appear at all, which is the finding rather than a gap when boot stops
// early -- the runtime only reports threads it has actually run.
extern "C" FILE* wetrix_diag_out;
extern "C" void wetrix_dump_game_threads(void) {
    const size_t count = g_game_thread_count.load(std::memory_order_acquire);
    const size_t limit = count < MAX_GAME_THREADS ? count : MAX_GAME_THREADS;

    fprintf((wetrix_diag_out != nullptr ? wetrix_diag_out : stderr), "[threads] %zu game thread(s) started\n", limit);

    for (size_t i = 0; i < limit; ++i) {
        const GameThreadRecord& record = g_game_threads[i];
        const OSThread* thread = record.thread;

        fprintf((wetrix_diag_out != nullptr ? wetrix_diag_out : stderr), "[threads]   host=%-6u game_id=%-3d pri=%-3d state=%s(%u) queue=0x%08X\n",
                record.host_id, record.game_id, record.priority,
                thread != nullptr ? game_thread_state_name(thread->state) : "unknown",
                thread != nullptr ? static_cast<unsigned>(thread->state) : 0u,
                thread != nullptr ? static_cast<uint32_t>(thread->queue) : 0u);
    }
    fflush((wetrix_diag_out != nullptr ? wetrix_diag_out : stderr));
}

// A double-clicked executable gets its own folder as the working directory, but
// a shell or a debugger can launch it from anywhere. librecomp resolves the
// ROM, mods and saves relative to the working directory and the registered
// config path, so both are pinned to the folder wetrix.exe lives in. That is
// what makes double-clicking work: baserom.z64, mods/ and saves/ are looked for
// next to the exe, not wherever Explorer happened to be.
std::filesystem::path executable_directory(const char* argv0) {
    if (argv0 == nullptr) {
        return {};
    }
    std::error_code ec;
    std::filesystem::path exe =
        std::filesystem::weakly_canonical(std::filesystem::absolute(argv0, ec), ec);
    if (ec) {
        return {};
    }
    return exe.parent_path();
}

// The ROM side of startup is in rom_adopt.c: finding the user's dump, checking
// it against the ROM this port was recompiled from, and moving it to the one
// name and one folder librecomp loads from. It is in the port rather than beside
// either caller because the build tool needs exactly the same behaviour, long
// before there is an exe to run.

// A console app launched by double-click gets a console window of its own, and
// that window sits in front holding the keyboard focus -- so the game window
// never sees a key press -- while everything the port prints is scrolled out at
// console speed. Neither is wanted. When the console belongs to this process
// alone (double-click), send the diagnostics to a log beside the exe and detach
// it. A console inherited from a shell is left attached, because there the
// output is the point of launching it that way.
void detach_owned_console(const std::filesystem::path& exe_dir) {
#if defined(_WIN32)
    DWORD processes[8] = {};
    if (GetConsoleProcessList(processes, 8) > 1) {
        return;     // inherited from a terminal: keep writing to it
    }

    const std::filesystem::path log_path =
        exe_dir.empty() ? std::filesystem::path("wetrix.log") : exe_dir / "wetrix.log";
    if (freopen(log_path.string().c_str(), "w", stderr) != nullptr) {
        // Unbuffered: with the console gone there is nothing to flush on exit,
        // and a forced kill would otherwise lose whatever was still buffered.
        setvbuf(stderr, nullptr, _IONBF, 0);
        // Same destination, append mode, so the two streams stay ordered.
        if (freopen(log_path.string().c_str(), "a", stdout) != nullptr) {
            setvbuf(stdout, nullptr, _IONBF, 0);
        }
        fprintf(stderr, "[wetrix] no console to print to; logging to %s\n",
                log_path.string().c_str());
        FreeConsole();
    }
#else
    (void)exe_dir;
#endif
}

// Startup progress on stderr. Without a window or any output, a crash here is
// otherwise silent, and the first crossing that does not print is the one that
// died.
#define TRACE(msg) do { fprintf(stderr, "[wetrix] %s\n", msg); fflush(stderr); } while (0)

int main(int argc, char** argv) {
    wetrix_install_crash_handler();
    wetrix_start_stall_probe();
    TRACE("start");
    wetrix::render::parse_args(argc, argv);

    // Run from (and configure librecomp for) the directory holding the exe.
    const std::filesystem::path exe_dir = executable_directory(argc > 0 ? argv[0] : nullptr);
    if (!exe_dir.empty()) {
        std::error_code ec;
        std::filesystem::current_path(exe_dir, ec);
        recomp::register_config_path(exe_dir);
        fprintf(stderr, "[wetrix] working directory: %s\n", exe_dir.string().c_str());
        fflush(stderr);
        detach_owned_console(exe_dir);
    }

    // SDL owns the window and the game controllers. Video has to be up before
    // the runtime asks for a window, and gamecontroller before input is polled.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[wetrix] SDL_Init failed: %s\n", SDL_GetError());
    }
    else {
        fprintf(stderr, "[wetrix] SDL initialized (%d game controller(s) present)\n",
                SDL_NumJoysticks());
        fflush(stderr);
        // Enumerate once here, on the thread that owns SDL video and the window.
        // Hotplug afterwards is handled in update_gfx on this same thread, so the
        // shared gamepad table has a single writer.
        refresh_controllers();
    }

    wetrix_register_overlays();
    // librecomp's check_hash() compares XXH3_64bits of the ROM data against
    // this, and rejects the ROM silently if it differs, so it has to be the
    // real value rather than a placeholder. Computed with tools/rom_hash.c,
    // which calls the runtime's own xxHash so the two cannot drift.
    constexpr uint64_t rom_hash = 0x7A64FDF6513D3659ULL;

    recomp::GameEntry game{
        .rom_hash = rom_hash,
        // The ROM's header name field (0x20), which librecomp compares
        // case-sensitively when reporting *why* a ROM was rejected. Capitalising
        // this looks tidier and silently turns "wrong revision" into "wrong
        // game", so it stays exactly as the ROM spells it.
        .internal_name = "Wetrix",
        .display_name = "Wetrix",
        .game_id = u8"wetrix",
        .mod_game_id = "",
        .save_type = recomp::SaveType::None,   // no save device in this ROM
        .thumbnail_bytes = {},
        .is_enabled = true,
        // Sign-extended, and that is not optional. `entrypoint_address` is a
        // gpr (uint64_t) that the runtime hands to do_rom_read, which addresses
        // RDRAM through the same MEM_B macro the recompiled code uses:
        //
        //   MEM_B(i, addr) = *(rdram + (((addr + i) ^ 3) - 0xFFFFFFFF80000000))
        //
        // That subtraction only lands inside rdram when addr is KSEG0
        // sign-extended. A bare 0x80000400 makes it rdram + 0x100000400 -- 4GB
        // past the end of the 8MB allocation, so the very first ROM DMA in
        // recomp::init writes outside the process and the game dies before it
        // starts. The low 32 bits still have to read as the entrypoint, which is
        // why load_overlays casts to int32_t and is happy either way.
        .entrypoint_address = 0xFFFFFFFF80000400ULL,   // 0x80000400 sign-extended
        .entrypoint = recomp_entrypoint,
        // entrypoint = 0x80000400 in wetrix.toml stays 32-bit: N64Recomp reads
        // it against the ELF to find the function, not as a runtime address.
    };

    TRACE("registering game");
    if (!recomp::register_game(game)) {
        fprintf(stderr, "[wetrix] failed to register game\n");
        return 1;
    }

    recomp::Configuration config{};
    config.argc = argc;
    config.argv = argv;
    config.project_version = recomp::Version{0, 1, 0, ""};

    config.rsp_callbacks.get_rsp_microcode = get_rsp_microcode;
    config.renderer_callbacks.create_render_context = wetrix::render::create_render_context;
    config.gfx_callbacks.create_gfx = create_gfx;
    config.gfx_callbacks.create_window = create_window;
    config.gfx_callbacks.update_gfx = update_gfx;

    config.audio_callbacks.queue_samples = queue_samples;
    config.audio_callbacks.get_frames_remaining = get_frames_remaining;
    config.audio_callbacks.set_frequency = set_audio_frequency;

    config.input_callbacks.poll_input = poll_input;
    config.input_callbacks.get_input = get_input;
    config.input_callbacks.set_rumble = set_rumble;
    config.input_callbacks.get_connected_device_info = get_connected_device_info;

    config.events_callbacks.vi_callback = vi_callback;
    config.events_callbacks.gfx_init_callback = gfx_init_callback;

    config.error_handling_callbacks.message_box = message_box;

    config.threads_callbacks.get_game_thread_name = get_game_thread_name;

    // librecomp loads config_path/wetrix.z64 by itself once the game starts, so
    // the file has to be there before start_game. Usually it already is and this
    // amounts to reading it and checking its hash; the other path is the one
    // that turns a dropped ROM into a port that runs.
    TRACE("checking rom");
    std::error_code cwd_ec;
    const std::string rom_dir =
        !exe_dir.empty() ? exe_dir.string() : std::filesystem::current_path(cwd_ec).string();
    if (wetrix_rom_adopt(rom_dir.c_str(), nullptr, "[wetrix] ") != 0) {
        return 1;
    }

    // start_game must come first. recomp::start blocks inside the game loop
    // waiting on the status that start_game sets, so calling it afterwards is
    // unreachable and the process just hangs with no output.
    TRACE("calling start_game");
    recomp::start_game(u8"wetrix", "");
    TRACE("entering recomp::start (blocks in the game loop)");
    recomp::start(config);
    return 0;
}
