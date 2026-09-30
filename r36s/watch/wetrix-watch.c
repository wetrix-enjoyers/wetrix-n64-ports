// wetrix-watch: PortMaster exit-hotkey watcher (port infrastructure, not game code).
// Watches pads for Select+Start and SIGKILLs the game PID so the launcher
// returns to ES. SIGKILL, not SIGTERM: SDL turns SIGTERM into a quit event
// and the game's quit path hangs. Saves are written on save, so nothing is lost.
// Exits on its own when the game is gone.
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <SDL2/SDL.h>

#define MAX_PADS 4

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: wetrix-watch <game-pid>\n"); return 1; }
    int target = atoi(argv[1]);
    if (target <= 0) return 1;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1"); // let the launcher's kill work
    if (SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) return 1;

    SDL_GameController *ctrl[MAX_PADS] = {0};
    SDL_Joystick *joy[MAX_PADS] = {0};
    int n = SDL_NumJoysticks();
    if (n < 0) n = 0;
    if (n > MAX_PADS) n = MAX_PADS;
    for (int i = 0; i < n; i++) {
        if (SDL_IsGameController(i)) {
            ctrl[i] = SDL_GameControllerOpen(i);
        } else {
            joy[i] = SDL_JoystickOpen(i);
        }
    }

    for (;;) {
        if (kill(target, 0) != 0) break; // game gone, done
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) { (void)ev; }
        int held = 0;
        for (int i = 0; i < n; i++) {
            int sel = 0, start = 0;
            if (ctrl[i]) {
                sel = SDL_GameControllerGetButton(ctrl[i], SDL_CONTROLLER_BUTTON_BACK);
                start = SDL_GameControllerGetButton(ctrl[i], SDL_CONTROLLER_BUTTON_START);
            } else if (joy[i]) {
                // Raw fallback: Select=8 / Start=9.
                sel = SDL_JoystickGetButton(joy[i], 8);
                start = SDL_JoystickGetButton(joy[i], 9);
            }
            if (sel && start) { held = 1; break; }
        }
        if (held) { kill(target, SIGKILL); break; }
        SDL_Delay(50);
    }

    for (int i = 0; i < n; i++) {
        if (ctrl[i]) SDL_GameControllerClose(ctrl[i]);
        else if (joy[i]) SDL_JoystickClose(joy[i]);
    }
    SDL_Quit();
    return 0;
}
