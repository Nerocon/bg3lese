/* Two built-in plugins, used to prove the host's dispatch contract without
 * needing Baldur's Gate 3:
 *
 *   - plugins init in priority order, not link order
 *   - an event consumed by an early plugin never reaches a later one, and never
 *     reaches the game
 *   - an event nobody consumes reaches everyone, and the game
 *
 * They register through the same BG3LESE_BUILTIN macro a real built-in plugin
 * uses, so this exercises the shipping path rather than a test-only one.
 */
#include "bg3lese.h"

static const bg3lese_api *early_api, *late_api;

/* ---- "late" is declared first on purpose: link order must not decide ---- */

static int late_init(const bg3lese_api *api)
{
    late_api = api;
    api->log("init");
    return 0;
}

static int late_on_event(const SDL_Event *ev)
{
    if (ev->type != SDL_KEYDOWN) return 0;
    late_api->log("saw %s", SDL_GetScancodeName(ev->key.keysym.scancode));
    return ev->key.keysym.scancode == SDL_SCANCODE_Y;   /* consume Y */
}

static const bg3lese_plugin late_plugin = {
    .abi = BG3LESE_ABI,
    .name = "late",
    .version = "test",
    .priority = 10,
    .init = late_init,
    .on_event = late_on_event,
};
BG3LESE_BUILTIN(late_plugin);

/* ---- "early" runs first because its priority is lower ---- */

static int early_init(const bg3lese_api *api)
{
    early_api = api;
    api->log("init");
    return 0;
}

static int early_on_event(const SDL_Event *ev)
{
    if (ev->type != SDL_KEYDOWN) return 0;
    early_api->log("saw %s", SDL_GetScancodeName(ev->key.keysym.scancode));
    return ev->key.keysym.scancode == SDL_SCANCODE_X;   /* consume X */
}

static void early_on_frame(void)
{
    static int once;
    if (!once++) early_api->log("first frame");
}

static const bg3lese_plugin early_plugin = {
    .abi = BG3LESE_ABI,
    .name = "early",
    .version = "test",
    .priority = 0,
    .init = early_init,
    .on_event = early_on_event,
    .on_frame = early_on_frame,
};
BG3LESE_BUILTIN(early_plugin);

/* ---- a plugin the host must refuse: wrong ABI ---- */

static const bg3lese_plugin stale_plugin = {
    .abi = BG3LESE_ABI + 1,
    .name = "stale",
    .version = "test",
    .priority = 5,
};
BG3LESE_BUILTIN(stale_plugin);
