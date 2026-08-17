#ifndef BG3LESE_SCAN_H
#define BG3LESE_SCAN_H

#include <stddef.h>
#include <stdint.h>

#define SCAN_MAX_PAT 64

typedef struct {
    uint8_t byte[SCAN_MAX_PAT];
    uint8_t mask[SCAN_MAX_PAT];   /* 0xFF = must match, 0 = wildcard */
    size_t len;
} scan_pat;

/* Parses "48 8b 05 ?? ?? ?? ??". Returns 0 on success, -1 on a malformed
 * pattern or one longer than SCAN_MAX_PAT. */
int scan_parse(const char *pattern, scan_pat *out);

/* Every occurrence in [lo,hi) — pass 0 for either bound to mean "unbounded".
 * Writes up to `max` addresses and returns the true total, which may exceed
 * `max`; callers that need uniqueness must compare against it. */
size_t scan_find(const scan_pat *pat, const uint8_t *code, size_t len,
                 uintptr_t base_va, uintptr_t lo, uintptr_t hi,
                 uintptr_t *out, size_t max);

/* Direct `call rel32` / `jmp rel32` sites targeting `target`. */
size_t scan_callers(uintptr_t target, const uint8_t *code, size_t len,
                    uintptr_t base_va, uintptr_t *out, size_t max);

#endif
