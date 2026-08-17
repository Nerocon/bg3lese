#ifndef BG3LESE_PATCH_H
#define BG3LESE_PATCH_H

#include <stddef.h>
#include <stdint.h>

enum {
    PATCH_OK = 0,
    PATCH_MISMATCH = -1,   /* the bytes there are not what the caller expected */
    PATCH_CONFLICT = -2,   /* something already patched an overlapping range */
    PATCH_NO_ISLAND = -3,  /* no free page within rel32 reach */
    PATCH_MPROTECT = -4,
    PATCH_BADARG = -5,     /* length out of range, or straddles two words */
};

/* Every write below verifies `expect` first, registers the range in a ledger,
 * and lands as a single aligned 8-byte atomic store — so a thread executing the
 * site can never observe a half-written instruction. Ranges are capped at 8
 * bytes within one aligned word for exactly that reason; a wider patch cannot
 * be made atomically and is refused rather than torn. */
int patch_nop(uintptr_t at, size_t len, const uint8_t *expect);
int patch_bytes(uintptr_t at, const uint8_t *bytes, const uint8_t *expect, size_t len);

/* Non-destructive call counter. The target's first five bytes must equal
 * `expect`; its prologue still runs, relocated into a trampoline island. */
int patch_count_calls(uintptr_t fn, const uint8_t *expect,
                      volatile uint64_t **counter_out);

/* Undo everything, newest first. Safe to call twice. */
void patch_restore_all(void);

size_t patch_active(void);

#endif
