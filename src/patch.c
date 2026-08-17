/* Code patching with a ledger.
 *
 * Two properties matter more than anything else here:
 *
 *   Atomicity. Every write is one aligned 8-byte store, so a thread executing
 *   the patched site cannot see it half-applied. Anything that will not fit
 *   that shape is refused rather than torn.
 *
 *   Reversibility. The host records every write and undoes them at shutdown,
 *   so a plugin that forgets to clean up — or crashes — cannot leave the game's
 *   code modified. That also gives us conflict detection for free: two plugins
 *   patching the same instruction is a bug, and the second one is told so.
 *
 * The target binary makes this unusually safe: it has no Intel CET (zero
 * endbr64 across ~90 MB of .text) and no anti-tamper, so there is no landing
 * pad to preserve and nothing watching the pages.
 */
#define _GNU_SOURCE
#include "patch.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ISLAND_SZ 64
#define STUB_OFF 8
#define MAX_RECORDS 64

typedef struct {
    uintptr_t word;      /* the aligned 8-byte word we rewrote */
    uint64_t original;
    uintptr_t lo, hi;    /* the range the caller asked for, for conflict checks */
    void *island;        /* trampoline page, if this was a hook */
    int active;
} record;

static record g_rec[MAX_RECORDS];
static size_t g_nrec;

static uintptr_t page_of(uintptr_t a)
{
    return a & ~(uintptr_t)(sysconf(_SC_PAGESIZE) - 1);
}

static int unprotect(uintptr_t addr, size_t len)
{
    uintptr_t start = page_of(addr);
    size_t span = (addr + len) - start;
    return mprotect((void *)start, span, PROT_READ | PROT_WRITE | PROT_EXEC);
}

static int overlaps(uintptr_t lo, uintptr_t hi)
{
    for (size_t i = 0; i < g_nrec; i++)
        if (g_rec[i].active && lo < g_rec[i].hi && g_rec[i].lo < hi)
            return 1;
    return 0;
}

/* Applies `buf` (8 bytes) at an aligned word, recording the original. */
static int commit(uintptr_t word, const uint8_t buf[8], uintptr_t lo, uintptr_t hi,
                  void *island)
{
    if (g_nrec >= MAX_RECORDS) return PATCH_BADARG;
    if (unprotect(word, 8) != 0) return PATCH_MPROTECT;

    record *r = &g_rec[g_nrec];
    memcpy(&r->original, (const void *)word, 8);
    r->word = word;
    r->lo = lo;
    r->hi = hi;
    r->island = island;
    r->active = 1;
    g_nrec++;

    uint64_t want;
    memcpy(&want, buf, 8);
    __atomic_store_n((uint64_t *)word, want, __ATOMIC_SEQ_CST);
    return PATCH_OK;
}

/* Shared front half of patch_nop and patch_bytes: validate, then splice the new
 * bytes into a copy of the containing word. */
static int splice(uintptr_t at, const uint8_t *newb, const uint8_t *expect,
                  size_t len, uint8_t out[8], uintptr_t *word_out)
{
    if (len < 1 || len > 8) return PATCH_BADARG;

    uintptr_t word = at & ~(uintptr_t)7;
    if (at + len > word + 8) return PATCH_BADARG;   /* straddles two words */

    /* Conflict is checked before the byte compare on purpose: a second patch
     * necessarily sees bytes the first one changed, so testing `expect` first
     * would report every double-patch as a mismatch and send the plugin author
     * looking for a signature bug that is not there. */
    if (overlaps(at, at + len)) return PATCH_CONFLICT;
    if (memcmp((const void *)at, expect, len) != 0) return PATCH_MISMATCH;

    memcpy(out, (const void *)word, 8);
    memcpy(out + (at - word), newb, len);
    *word_out = word;
    return PATCH_OK;
}

/* Canonical multi-byte NOPs, indexed by length. */
static const uint8_t NOPS[9][8] = {
    {0}, {0x90},
    {0x66, 0x90},
    {0x0F, 0x1F, 0x00},
    {0x0F, 0x1F, 0x40, 0x00},
    {0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00},
    {0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00},
    {0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00},
};

int patch_nop(uintptr_t at, size_t len, const uint8_t *expect)
{
    if (len < 2 || len > 8) return PATCH_BADARG;
    uint8_t buf[8];
    uintptr_t word;
    int rc = splice(at, NOPS[len], expect, len, buf, &word);
    if (rc != PATCH_OK) return rc;
    return commit(word, buf, at, at + len, NULL);
}

int patch_bytes(uintptr_t at, const uint8_t *bytes, const uint8_t *expect, size_t len)
{
    uint8_t buf[8];
    uintptr_t word;
    int rc = splice(at, bytes, expect, len, buf, &word);
    if (rc != PATCH_OK) return rc;
    return commit(word, buf, at, at + len, NULL);
}

/* A rel32 jump reaches only ±2 GB, and a preloaded library lands terabytes away
 * from the game's code, so the trampoline has to be placed near the target. */
static void *alloc_island_near(uintptr_t target)
{
    size_t pg = sysconf(_SC_PAGESIZE);
    for (uintptr_t delta = pg; delta < 0x60000000; delta += 16 * pg) {
        for (int dir = 0; dir < 2; dir++) {
            uintptr_t at = page_of(dir ? target + delta : target - delta);
            if (!at) continue;
            void *p = mmap((void *)at, ISLAND_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (p != MAP_FAILED) return p;
        }
    }
    return NULL;
}

int patch_count_calls(uintptr_t fn, const uint8_t *expect, volatile uint64_t **counter_out)
{
    if (fn & 7) return PATCH_BADARG;          /* the atomic store needs alignment */
    if (overlaps(fn, fn + 5)) return PATCH_CONFLICT;
    if (memcmp((const void *)fn, expect, 5) != 0) return PATCH_MISMATCH;

    uint8_t *island = alloc_island_near(fn);
    if (!island) return PATCH_NO_ISLAND;

    /* island layout: [0] counter, [8] inc + stolen prologue + jmp back */
    uint8_t *stub = island + STUB_OFF;
    stub[0] = 0x48; stub[1] = 0xFF; stub[2] = 0x05;          /* inc qword [rip+d] */
    int32_t to_counter = (int32_t)((intptr_t)island - (intptr_t)(stub + 7));
    memcpy(stub + 3, &to_counter, 4);
    memcpy(stub + 7, expect, 5);                              /* the stolen bytes */
    stub[12] = 0xE9;                                          /* jmp back */
    int32_t back = (int32_t)((intptr_t)(fn + 5) - (intptr_t)(stub + 17));
    memcpy(stub + 13, &back, 4);

    /* Replace the prologue with a jump to the stub, keeping the three bytes
     * that follow so the store stays a single aligned write. */
    uint8_t buf[8];
    memcpy(buf, (const void *)fn, 8);
    buf[0] = 0xE9;
    int32_t to_stub = (int32_t)((intptr_t)stub - (intptr_t)(fn + 5));
    memcpy(buf + 1, &to_stub, 4);

    int rc = commit(fn, buf, fn, fn + 5, island);
    if (rc != PATCH_OK) {
        munmap(island, ISLAND_SZ);
        return rc;
    }
    *counter_out = (volatile uint64_t *)island;
    return PATCH_OK;
}

void patch_restore_all(void)
{
    while (g_nrec) {
        record *r = &g_rec[--g_nrec];
        if (!r->active) continue;
        __atomic_store_n((uint64_t *)r->word, r->original, __ATOMIC_SEQ_CST);
        if (r->island) munmap(r->island, ISLAND_SZ);
        r->active = 0;
    }
}

size_t patch_active(void)
{
    size_t n = 0;
    for (size_t i = 0; i < g_nrec; i++) n += g_rec[i].active != 0;
    return n;
}
