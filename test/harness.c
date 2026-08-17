/* A stand-in for the game: pushes known keys, then reports what SDL_PollEvent
 * handed back. Uses SDL's dummy drivers, so no display is needed. */
#include <SDL2/SDL.h>
#include <stdio.h>

static void push_key(SDL_Scancode sc)
{
    SDL_Event e;
    SDL_zero(e);
    e.type = SDL_KEYDOWN;
    e.key.state = SDL_PRESSED;
    e.key.keysym.scancode = sc;
    e.key.keysym.sym = SDL_GetKeyFromScancode(sc);
    SDL_PushEvent(&e);
}

int main(void)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 2;
    }

    push_key(SDL_SCANCODE_X);   /* consumed by "early" */
    push_key(SDL_SCANCODE_Y);   /* consumed by "late" */
    push_key(SDL_SCANCODE_Z);   /* consumed by nobody */

    int keys = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type != SDL_KEYDOWN) continue;
        keys++;
        printf("  GAME SAW %s\n", SDL_GetScancodeName(e.key.keysym.scancode));
    }
    printf("RESULT keys=%d\n", keys);
    SDL_Quit();
    return 0;
}
