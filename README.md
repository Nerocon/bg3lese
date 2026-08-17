# bg3lese

**A script extender for the native Linux build of Baldur's Gate 3.**

Norbyte's [Script Extender](https://github.com/Norbyte/bg3se) is Windows-only by
construction — it resolves functions from MSVC byte patterns that cannot match a
clang-built ELF. This is the Linux-native equivalent: an `LD_PRELOAD` host that finds
code in the running game, patches it safely, and hands plugins a frame tick and the
input stream.

It ships no game features of its own. [wasdlebg3](https://github.com/Nerocon/wasdlebg3)
is the first plugin.

> **Status:** working, ABI version 1, **not frozen** — see [Stability](#stability).

---

## Why a host at all

Two things in this process cannot be shared by independent mods, and both are the
reason to have one library own them.

**`SDL_PollEvent` is the game's only input ingress.** It imports no `SDL_PeepEvents`,
no `SDL_WaitEvent`, no `SDL_AddEventWatch`, not even `SDL_GetKeyboardState`, and it has
exactly one call site. Two `LD_PRELOAD` mods interposing it would chain in whatever
order the loader picked, each deciding independently to swallow a key and neither
knowing the other did. The host interposes it once and dispatches in priority order;
the first plugin to consume an event stops the chain.

**Code patches need a ledger.** Every patch goes through the host, which refuses
overlapping writes and restores everything at shutdown — so a plugin that forgets to
clean up, or crashes, cannot leave the game's code modified.

Everything else the host offers is there because it was the hard part and nobody should
have to redo it: symbol resolution across the game's 152,416-entry `.symtab`, wildcard
pattern scanning, call-site discovery, and atomic patching.

## Writing a plugin

```c
#include <bg3lese.h>

static const bg3lese_api *api;

static int init(const bg3lese_api *a)
{
    api = a;

    uintptr_t hits[4];
    if (api->scan("48 8b 05 ?? ?? ?? ?? 80 b8 ?? ?? ?? ?? 00", hits, 4) != 1) {
        api->log("signature did not match exactly once — declining to load");
        return -1;                      /* unloads cleanly, reverts nothing */
    }
    api->log("found it at %#lx", hits[0]);
    return 0;
}

static int on_event(const SDL_Event *ev)
{
    if (api->text_input_active()) return 0;         /* the user is typing */
    if (ev->type == SDL_KEYDOWN && ev->key.keysym.scancode == SDL_SCANCODE_G) {
        api->log("G pressed");
        return 1;                                   /* consume it */
    }
    return 0;
}

static const bg3lese_plugin plugin = {
    .abi = BG3LESE_ABI,
    .name = "example",
    .version = "1.0",
    .priority = 50,
    .init = init,
    .on_event = on_event,
};

const bg3lese_plugin *bg3lese_plugin_entry(void) { return &plugin; }
```

Build it as a shared object and drop it in a `plugins/` directory beside `bg3lese.so`,
or point `BG3LE_PLUGIN_DIR` at it.

The full interface is [`include/bg3lese.h`](include/bg3lese.h) — it is short, and worth
reading rather than summarising.

### A note on finding things

The game binary keeps its `.symtab` and, unusually, was linked with `--emit-relocs`.
So `api->symbol()` resolves Larian's ECS type registry by mangled name — 2,107 component
types and 934 systems — which is a far better anchor than any byte pattern.

But it can legitimately fail: `.symtab` sits past the end of every `PT_LOAD`, so
resolving it means reading the executable back off disk, and Steam's container runtime
can make that impossible. **Treat symbols as a bonus and signatures as the floor.** The
host itself follows this rule: it identifies the game by scanning read-only data for the
game's branding, which cannot fail for environmental reasons.

## Embedding it

A plugin can link the host in and ship as one file, which is what wasdlebg3 does — users
install a single `.so` and set one `LD_PRELOAD`.

```make
BG3LESE_LINK = -Wl,--whole-archive $(BG3LESE)/build/libbg3lese.a -Wl,--no-whole-archive
```

`--whole-archive` is **mandatory, not a tuning flag.** A plugin never *references*
anything in the core, so ordinary archive semantics drop `main.o` — and with it the
constructor and the `SDL_PollEvent` interposer. The result links cleanly, loads
cleanly, and does absolutely nothing.

## Building

```sh
make          # build/libbg3lese.a and build/bg3lese.so
make test     # 54 assertions
```

Tests need **neither Baldur's Gate 3 nor a display.** They cover pattern scanning over
synthetic buffers, symbol resolution against a live PIE, inline hooking of a local
function reproducing BG3's exact prologue, ledger conflict detection, and the full
dispatch contract — priority ordering, consumption, ABI rejection — against a stand-in
game driven by SDL's dummy drivers.

## Configuration

Environment variables, so they fit in a Steam launch option. The prefix is `BG3LE_`
rather than `BG3LESE_` for compatibility with what shipped before the split.

| variable | default | meaning |
|---|---|---|
| `BG3LE_LOG` | `/tmp/bg3le.log` | log file |
| `BG3LE_VERBOSE` | `0` | also log from Steam's helper processes |
| `BG3LE_PLUGIN_DIR` | `<dir of the .so>/plugins` | where to look for dynamic plugins |
| `BG3LE_FORCE_HOST` | `0` | skip host identification; an escape hatch if a future build drops the branding anchors |

## Stability

ABI version 1 is **not frozen.** While bg3lese ships with a single known plugin the
interface may change between releases; `BG3LESE_ABI` is bumped when it does, and the
host refuses to load a plugin built against a different number rather than crashing in
an interesting way.

It will be frozen once a second independent plugin exists — an interface with one
consumer has not yet been told what it got wrong.

## Safety

- Patches live **in memory only**. Nothing is written to the game directory or to disk,
  and every patch is reverted at shutdown.
- Every write verifies the bytes it expects first, and lands as a single aligned atomic
  store, so a thread executing the site cannot observe a half-written instruction.
  Anything that cannot fit that shape is refused rather than torn.
- The target binary has **no Intel CET** (zero `endbr64` across ~90 MB of `.text`) and no
  anti-tamper, so there is no landing pad to preserve and nothing watching the pages.
- Multiplayer is out of scope. The client streams derived state to the server; injecting
  input in co-op is untested and plausibly desyncs.

## License

MIT — see [LICENSE](LICENSE).

Contains no Larian code or assets. Interoperability software that reads the user's own
installed copy of the game at runtime. Baldur's Gate 3 is © Larian Studios.
