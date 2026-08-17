/* bg3lese — script extender ABI for the native Linux build of Baldur's Gate 3.
 *
 * A plugin is a small C library that receives this API at init, subscribes to
 * the input stream and the frame tick, and asks the host to find and patch code
 * on its behalf. The host owns the two things that cannot be shared safely:
 *
 *   - SDL_PollEvent, the game's only input ingress. Exactly one library may
 *     interpose it, so the host does, and dispatches to plugins in priority
 *     order. A plugin that consumes an event stops dispatch, which is how two
 *     plugins avoid both eating the same key.
 *
 *   - The code-patch ledger. Every patch goes through the host, which refuses
 *     overlapping writes and restores everything on shutdown even if a plugin
 *     forgets or dies.
 *
 * ABI stability: this is version 1 and is NOT frozen. While bg3lese ships with
 * a single built-in plugin the ABI may change between releases; BG3LESE_ABI is
 * bumped when it does, and the host refuses to load a plugin built against a
 * different number. It will be frozen once a second independent plugin exists.
 */
#ifndef BG3LESE_H
#define BG3LESE_H

#include <SDL2/SDL.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BG3LESE_ABI 2

/* Return codes shared by the patching calls. */
enum {
    BG3LESE_OK = 0,
    BG3LESE_MISMATCH = -1,   /* the bytes there are not what you said to expect */
    BG3LESE_CONFLICT = -2,   /* another plugin already patched that range */
    BG3LESE_NO_ISLAND = -3,  /* no free page within rel32 reach of the target */
    BG3LESE_MPROTECT = -4,
    BG3LESE_BADARG = -5,
};

typedef struct bg3lese_api {
    uint32_t abi;
    const char *host_version;

    /* ---- discovery ---------------------------------------------------- */

    /* Resolve a mangled symbol from the host's .symtab, load bias applied.
     * Returns 0 when unavailable — the symbol table lives past the end of every
     * PT_LOAD, so this reads the executable back off disk and Steam's container
     * can make that fail. Treat symbols as a bonus, never a requirement. */
    uintptr_t (*symbol)(const char *mangled);

    /* Wildcard byte-pattern scan over the host's executable segment. Pattern is
     * space-separated hex with ?? for don't-care: "48 8b 05 ?? ?? ?? ??".
     * Writes up to `max` match addresses and returns how many were found —
     * which may exceed `max`, so a caller wanting uniqueness must check. */
    size_t (*scan)(const char *pattern, uintptr_t *out, size_t max);
    size_t (*scan_range)(const char *pattern, uintptr_t lo, uintptr_t hi,
                         uintptr_t *out, size_t max);

    /* Direct `call rel32` / `jmp rel32` sites whose target is `fn`. */
    size_t (*callers_of)(uintptr_t fn, uintptr_t *out, size_t max);

    /* The host's executable segment, for plugins that would rather scan it
     * themselves. `base_va` is already bias-adjusted. */
    void (*image)(const uint8_t **code, size_t *len, uintptr_t *base_va);

    /* ---- patching ------------------------------------------------------ */

    /* All of these verify `expect` before writing and register the range with
     * the host, which restores it at shutdown. Writes are single aligned atomic
     * stores where the range allows, so a thread executing the site cannot
     * observe a half-written instruction. */
    int (*patch_nop)(uintptr_t at, size_t len, const uint8_t *expect);
    int (*patch_bytes)(uintptr_t at, const uint8_t *bytes, const uint8_t *expect,
                       size_t len);

    /* Non-destructive call counter: `fn`'s prologue still runs, and a counter
     * is incremented on entry. `expect` is the first five bytes, verified.
     * Useful for answering "is this function called at all?" — which is not a
     * question static analysis can settle. */
    int (*count_calls)(uintptr_t fn, const uint8_t *expect,
                       volatile uint64_t **counter_out);

    /* ---- services ------------------------------------------------------ */

    void (*log)(const char *fmt, ...);

    /* Hand the game an event it never actually received. Injected events are
     * delivered ahead of real ones and are NOT dispatched back to plugins — a
     * plugin must not be able to consume, or infinitely re-see, its own
     * injection. Returns 0 on success, BG3LESE_BADARG if the queue is full.
     *
     * This exists because consuming input is only half of remapping it: a
     * plugin that swallows a right-drag usually wants to hand the game some
     * other gesture in its place. */
    int (*push_event)(const SDL_Event *ev);

    /* True while the game has a text field focused — naming a save, renaming a
     * character. Any plugin that consumes keys must stand down while this is
     * set, so the host tracks it centrally rather than each plugin guessing. */
    int (*text_input_active)(void);

    /* Configuration comes from the environment so it fits in a Steam launch
     * option. `key` is uppercased and prefixed: cfg("WALK_KEY") reads
     * BG3LE_WALK_KEY. Plugins should namespace their own keys. */
    const char *(*cfg)(const char *key, const char *dflt);
    int (*cfg_int)(const char *key, int dflt);
    double (*cfg_num)(const char *key, double dflt);
} bg3lese_api;

typedef struct bg3lese_plugin {
    uint32_t abi;              /* must equal BG3LESE_ABI */
    const char *name;          /* short, lowercase, e.g. "wasd" */
    const char *version;

    /* Lower runs first. Input plugins that consume keys want to be early;
     * observers want to be late. Ties break on load order. */
    int priority;

    /* Return 0 to stay loaded. Nonzero unloads cleanly and reverts any patches
     * already made, which is the right response to "this game build is not one
     * I recognise". */
    int (*init)(const bg3lese_api *api);

    /* Return nonzero to consume the event: the game never sees it and no
     * later plugin is offered it. May be NULL. */
    int (*on_event)(const SDL_Event *ev);

    /* Once per frame, on the main thread, after the game has drained its
     * events and before it simulates. May be NULL. */
    void (*on_frame)(void);

    void (*shutdown)(void);    /* may be NULL */
} bg3lese_plugin;

/* Dynamic plugins export exactly this symbol. */
const bg3lese_plugin *bg3lese_plugin_entry(void);

/* Built-in plugins are statically linked and register through a linker set, so
 * they need no loader and no exported symbol. Same ABI, same api pointer, same
 * lifecycle — which is the point: the built-in is a real client of the public
 * interface, not a privileged special case. */
#define BG3LESE_BUILTIN(plugin_sym)                                    \
    static const bg3lese_plugin *const bg3lese_builtin_##plugin_sym    \
        __attribute__((used, section("bg3lese_plugins"))) = &plugin_sym

#ifdef __cplusplus
}
#endif
#endif /* BG3LESE_H */
