/*
 * Copyright 2026 @macario_moreno
 * SPDX-License-Identifier: Apache-2.0
 *
 * cz_sdl2_keytest.c - Minimal SDL2 keyboard diagnostic tool for CardputerZero
 *
 * Prints SDL_KEYDOWN / SDL_KEYUP (scancode, keycode, modifiers) and SDL_TEXTINPUT
 * to stdout/stderr so you can verify KMSDRM input over SSH:
 *   ./bin/cz-sdl2-run ./build/cz_sdl2_keytest
 * Press ESC twice to exit.
 */

#include <SDL2/SDL.h>
#include <stdio.h>

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win = SDL_CreateWindow(
        "CardputerZero SDL2 KeyTest",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        320, 170,
        SDL_WINDOW_SHOWN
    );
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_StartTextInput();

    printf("SDL2 video driver: %s\n", SDL_GetCurrentVideoDriver());
    printf("Press keys (Sym + E/R/U/I/O/P, etc.). Press ESC twice to quit.\n");
    fflush(stdout);

    int esc_count = 0;
    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                const char* tag = (ev.type == SDL_KEYDOWN) ? "KEYDOWN" : "KEYUP  ";
                const Uint8* ks = SDL_GetKeyboardState(NULL);
                int lshift = ks[SDL_SCANCODE_LSHIFT];
                printf("[%s] scancode=%3d (%-14s) sym=%-10s mod=0x%04x state[LSHIFT]=%d\n",
                       tag,
                       ev.key.keysym.scancode,
                       SDL_GetScancodeName(ev.key.keysym.scancode),
                       SDL_GetKeyName(ev.key.keysym.sym),
                       ev.key.keysym.mod,
                       lshift);
                fflush(stdout);

                if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        if (++esc_count >= 2)
                            running = 0;
                    } else {
                        esc_count = 0;
                    }
                }
            } else if (ev.type == SDL_TEXTINPUT) {
                printf("[TEXTIN ] \"%s\"\n", ev.text.text);
                fflush(stdout);
            }
        }

        if (ren) {
            SDL_SetRenderDrawColor(ren, 20, 28, 40, 255);
            SDL_RenderClear(ren);
            SDL_RenderPresent(ren);
        }
        SDL_Delay(16);
    }

    SDL_StopTextInput();
    if (ren)
        SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
