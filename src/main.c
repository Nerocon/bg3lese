/* bg3lese — the extender core.
 *
 * Owns the process-wide singletons: host identification, the SDL_PollEvent
 * interposition, the plugin registry, the patch ledger and the log. Knows
 * nothing about any particular game feature — movement, camera and everything
 * else live in plugins that talk to it through include/bg3lese.h.
 */
#define _GNU_SOURCE
#include "bg3lese.h"

#include "host.h"
#include "patch.h"
#include "scan.h"
#include "symres.h"

#include <dirent.h>
#include <limits.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define BG3LESE_VERSION "0.2.0"
#define MAX_PLUGINS 16
#define MAX_MATCHES 64

/* The linker gives us these for any section whose name is a C identifier. */
extern const bg3lese_plugin *const __start_bg3lese_plugins[] __attribute__((weak));
extern const bg3lese_plugin *const __stop_bg3lese_plugins[] __attribute__((weak));

/* ---------------------------------------------------------------- logging */

static FILE *g_logf;
static int g_mirror_stderr;
static int cfg_verbose;
static const char *g_logging_for;   /* plugin name, for message attribution */

static void vlog_at(const char *who, const char *fmt, va_list ap)
{
    if (!g_logf) return;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(g_logf, "[bg3lese %7ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
    if (who) fprintf(g_logf, "%s: ", who);

    va_list copy;
    va_copy(copy, ap);
    vfprintf(g_logf, fmt, ap);
    fputc('\n', g_logf);
    fflush(g_logf);

    if (g_mirror_stderr) {
        fputs("bg3lese: ", stderr);
        if (who) fprintf(stderr, "%s: ", who);
        vfprintf(stderr, fmt, copy);
        fputc('\n', stderr);
    }
    va_end(copy);
}

__attribute__((format(printf, 1, 2)))
static void core_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_at(NULL, fmt, ap);
    va_end(ap);
}

/* Plugins get their name prefixed automatically, so a log line always says who
 * produced it without every plugin having to remember. */
static void api_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog_at(g_logging_for, fmt, ap);
    va_end(ap);
}

/* ----------------------------------------------------------- configuration */

/* The env prefix stays BG3LE_ rather than BG3LESE_: 0.1.0 shipped with those
 * names and people have them in their Steam launch options. */
static const char *api_cfg(const char *key, const char *dflt)
{
    char buf[128];
    snprintf(buf, sizeof buf, "BG3LE_%s", key);
    for (char *p = buf; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p -= 32;
    const char *v = getenv(buf);
    return v ? v : dflt;
}

static int api_cfg_int(const char *key, int dflt)
{
    const char *v = api_cfg(key, NULL);
    return v ? atoi(v) : dflt;
}

static double api_cfg_num(const char *key, double dflt)
{
    const char *v = api_cfg(key, NULL);
    return v ? atof(v) : dflt;
}

static int cfg_flag(const char *key, int dflt)
{
    const char *v = api_cfg(key, NULL);
    return v ? (*v != '0') : dflt;
}

/* ------------------------------------------------------------ host access */

static host_image g_img;
static sr_ctx *g_syms;
static int g_text_input;

static uintptr_t api_symbol(const char *mangled)
{
    return g_syms ? sr_resolve(g_syms, mangled) : 0;
}

static size_t scan_common(const char *pattern, uintptr_t lo, uintptr_t hi,
                          uintptr_t *out, size_t max)
{
    scan_pat pat;
    if (scan_parse(pattern, &pat) != 0) return 0;
    return scan_find(&pat, g_img.code, g_img.len, g_img.base_va, lo, hi, out, max);
}

static size_t api_scan(const char *pattern, uintptr_t *out, size_t max)
{
    return scan_common(pattern, 0, 0, out, max);
}

static size_t api_scan_range(const char *pattern, uintptr_t lo, uintptr_t hi,
                             uintptr_t *out, size_t max)
{
    return scan_common(pattern, lo, hi, out, max);
}

static size_t api_callers_of(uintptr_t fn, uintptr_t *out, size_t max)
{
    return scan_callers(fn, g_img.code, g_img.len, g_img.base_va, out, max);
}

static void api_image(const uint8_t **code, size_t *len, uintptr_t *base_va)
{
    if (code) *code = g_img.code;
    if (len) *len = g_img.len;
    if (base_va) *base_va = g_img.base_va;
}

static int api_text_input_active(void) { return g_text_input; }

/* Events a plugin asked us to hand the game. Small on purpose: this is for
 * remapping a gesture, not for scripting a playthrough. */
#define MAX_INJECT 32
static SDL_Event g_inject[MAX_INJECT];
static size_t g_ninject;

static int api_push_event(const SDL_Event *ev)
{
    if (!ev || g_ninject >= MAX_INJECT) return BG3LESE_BADARG;
    g_inject[g_ninject++] = *ev;
    return BG3LESE_OK;
}

static const bg3lese_api API = {
    .abi = BG3LESE_ABI,
    .host_version = BG3LESE_VERSION,
    .symbol = api_symbol,
    .scan = api_scan,
    .scan_range = api_scan_range,
    .callers_of = api_callers_of,
    .image = api_image,
    .patch_nop = patch_nop,
    .patch_bytes = patch_bytes,
    .count_calls = patch_count_calls,
    .log = api_log,
    .push_event = api_push_event,
    .text_input_active = api_text_input_active,
    .cfg = api_cfg,
    .cfg_int = api_cfg_int,
    .cfg_num = api_cfg_num,
};

/* ------------------------------------------------------------- plugin set */

typedef struct {
    const bg3lese_plugin *p;
    void *dl;        /* NULL for built-ins */
    int live;
} slot;

static slot g_slot[MAX_PLUGINS];
static size_t g_nslot;
static int g_is_host;

static void add_plugin(const bg3lese_plugin *p, void *dl)
{
    if (!p || g_nslot >= MAX_PLUGINS) return;

    /* The same plugin can legitimately arrive twice: built into a bundle and
     * again as a dynamic .so someone dropped in plugins/. Loading both would
     * run it twice — two sets of patches, two consumers of the same key — so
     * first registration wins and the second is refused loudly. */
    for (size_t i = 0; i < g_nslot; i++) {
        if (p->name && g_slot[i].p->name && !strcmp(p->name, g_slot[i].p->name)) {
            core_log("plugin '%s' is already loaded%s — ignoring the duplicate",
                     p->name, dl ? " (built in)" : "");
            if (dl) dlclose(dl);
            return;
        }
    }

    if (p->abi != BG3LESE_ABI) {
        core_log("plugin '%s' built against ABI %u, host is %u — not loaded",
                 p->name ? p->name : "?", p->abi, BG3LESE_ABI);
        if (dl) dlclose(dl);
        return;
    }
    g_slot[g_nslot].p = p;
    g_slot[g_nslot].dl = dl;
    g_slot[g_nslot].live = 0;
    g_nslot++;
}

/* Where to look for dynamic plugins: an explicit override, else a `plugins`
 * directory beside this library. */
static void load_dynamic(void)
{
    char dir[PATH_MAX / 2];
    const char *override = api_cfg("PLUGIN_DIR", NULL);

    if (override) {
        snprintf(dir, sizeof dir, "%s", override);
    } else {
        Dl_info info;
        if (!dladdr((void *)&load_dynamic, &info) || !info.dli_fname) return;
        snprintf(dir, sizeof dir, "%s", info.dli_fname);
        char *slash = strrchr(dir, '/');
        if (!slash) return;
        snprintf(slash, sizeof dir - (size_t)(slash - dir), "/plugins");
    }

    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || strcmp(dot, ".so")) continue;

        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!h) { core_log("dlopen %s: %s", e->d_name, dlerror()); continue; }

        const bg3lese_plugin *(*entry)(void) = dlsym(h, "bg3lese_plugin_entry");
        if (!entry) {
            core_log("%s exports no bg3lese_plugin_entry — skipped", e->d_name);
            dlclose(h);
            continue;
        }
        add_plugin(entry(), h);
    }
    closedir(d);
}

static void sort_by_priority(void)
{
    for (size_t i = 1; i < g_nslot; i++) {
        slot k = g_slot[i];
        size_t j = i;
        while (j && g_slot[j - 1].p->priority > k.p->priority) {
            g_slot[j] = g_slot[j - 1];
            j--;
        }
        g_slot[j] = k;
    }
}

static void start_plugins(void)
{
    if (__start_bg3lese_plugins && __stop_bg3lese_plugins)
        for (const bg3lese_plugin *const *i = __start_bg3lese_plugins;
             i < __stop_bg3lese_plugins; i++)
            add_plugin(*i, NULL);

    load_dynamic();
    sort_by_priority();

    for (size_t i = 0; i < g_nslot; i++) {
        const bg3lese_plugin *p = g_slot[i].p;
        g_logging_for = p->name;
        int rc = p->init ? p->init(&API) : 0;
        g_logging_for = NULL;
        g_slot[i].live = (rc == 0);
        core_log("%s %s: %s", p->name, p->version ? p->version : "",
                 rc == 0 ? "loaded" : "declined to load");
    }
}

/* ------------------------------------------------------------- dispatch */

static int dispatch_event(const SDL_Event *ev)
{
    for (size_t i = 0; i < g_nslot; i++) {
        if (!g_slot[i].live || !g_slot[i].p->on_event) continue;
        g_logging_for = g_slot[i].p->name;
        int consumed = g_slot[i].p->on_event(ev);
        g_logging_for = NULL;
        if (consumed) return 1;
    }
    return 0;
}

static void dispatch_frame(void)
{
    for (size_t i = 0; i < g_nslot; i++) {
        if (!g_slot[i].live || !g_slot[i].p->on_frame) continue;
        g_logging_for = g_slot[i].p->name;
        g_slot[i].p->on_frame();
        g_logging_for = NULL;
    }
}

/* ---------------------------------------------------------- interposition */

static int (*real_poll)(SDL_Event *);

int SDL_PollEvent(SDL_Event *ev)
{
    if (!real_poll) {
        real_poll = dlsym(RTLD_NEXT, "SDL_PollEvent");
        if (!real_poll) {
            core_log("FATAL: no real SDL_PollEvent behind us: %s", dlerror());
            return 0;
        }
    }

    for (;;) {
        /* Injected events go to the game ahead of real ones, and deliberately
         * skip dispatch: a plugin must not consume or re-see its own. */
        if (g_is_host && g_ninject) {
            *ev = g_inject[0];
            memmove(g_inject, g_inject + 1, (--g_ninject) * sizeof *g_inject);
            return 1;
        }
        int r = real_poll(ev);
        if (!r) {
            /* The game drains events until this returns 0, so here we are
             * exactly once per frame, on the main thread, before simulation. */
            if (g_is_host) dispatch_frame();
            return 0;
        }
        if (g_is_host && dispatch_event(ev))
            continue;   /* consumed — hand the game the next event instead */
        return 1;
    }
}

/* The game tells us when a text field takes focus; we only have to listen, and
 * republish it so every plugin agrees on the answer. */
void SDL_StartTextInput(void)
{
    static void (*real_fn)(void);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "SDL_StartTextInput");
    g_text_input = 1;
    if (real_fn) real_fn();
}

void SDL_StopTextInput(void)
{
    static void (*real_fn)(void);
    if (!real_fn) real_fn = dlsym(RTLD_NEXT, "SDL_StopTextInput");
    g_text_input = 0;
    if (real_fn) real_fn();
}

/* ------------------------------------------------------------- lifecycle */

static void open_log(void)
{
    const char *want = api_cfg("LOG", NULL);
    const char *tries[2];
    int n = 0;
    if (want) tries[n++] = want;
    tries[n++] = "/tmp/bg3le.log";

    for (int i = 0; i < n; i++) {
        g_logf = fopen(tries[i], "ae");
        if (g_logf) {
            if (i > 0) {
                fprintf(stderr, "bg3lese: could not write %s, using %s\n", want, tries[i]);
                g_mirror_stderr = 1;
            }
            return;
        }
    }
    g_logf = stderr;
    fprintf(stderr, "bg3lese: no writable log file, logging to stderr\n");
}

__attribute__((destructor)) static void bg3lese_fini(void)
{
    if (!g_is_host) return;
    for (size_t i = 0; i < g_nslot; i++) {
        if (!g_slot[i].live || !g_slot[i].p->shutdown) continue;
        g_logging_for = g_slot[i].p->name;
        g_slot[i].p->shutdown();
        g_logging_for = NULL;
    }
    size_t n = patch_active();
    patch_restore_all();
    if (n) core_log("restored %zu patch(es)", n);
    if (g_syms) sr_close(g_syms);
}

__attribute__((constructor)) static void bg3lese_init(void)
{
    open_log();
    cfg_verbose = cfg_flag("VERBOSE", 0);
    if (cfg_verbose && g_logf != stderr) g_mirror_stderr = 1;

    g_is_host = host_probe(&g_img);
    /* Escape hatch: if a future build drops the branding anchors, identification
     * fails and everything goes quiet. This forces the host path so the failure
     * is diagnosable rather than silent. Also what the dispatch tests use. */
    if (!g_is_host && cfg_flag("FORCE_HOST", 0) && g_img.len) {
        g_is_host = 1;
        core_log("host identification overridden by BG3LE_FORCE_HOST");
    }
    if (!g_is_host) {
        if (cfg_verbose)
            core_log("not Baldur's Gate 3 (%zu KB of code) — idle", g_img.len / 1024);
        else
            g_logf = NULL;   /* Steam re-execs through many helpers; stay quiet */
        return;
    }

    core_log("bg3lese " BG3LESE_VERSION " — host identified, %.1f MB of code",
             g_img.len / 1048576.0);

    /* Symbols are a bonus, never a gate: .symtab sits past the end of every
     * PT_LOAD, so this reads the executable back off disk and can legitimately
     * fail inside Steam's container runtime. */
    char err[256] = {0};
    g_syms = sr_open_self(err, sizeof err);
    if (!g_syms)
        core_log("symbols unavailable (%s) — plugins relying on them will say so", err);

    start_plugins();
}
