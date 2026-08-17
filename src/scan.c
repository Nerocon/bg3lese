/* Generic wildcard byte-pattern scanning over the host image.
 *
 * This is the part of signature scanning that is not specific to any one thing
 * we are looking for: parse "48 8b 05 ?? ?? ?? ??" into bytes plus a mask, and
 * find every occurrence. Knowledge of *which* patterns matter belongs to the
 * plugin that cares.
 */
#include "scan.h"

#include <string.h>

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int scan_parse(const char *pattern, scan_pat *out)
{
    out->len = 0;
    for (const char *p = pattern; *p; ) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (out->len >= SCAN_MAX_PAT) return -1;

        if (p[0] == '?') {
            /* accept both "?" and "??" */
            out->byte[out->len] = 0;
            out->mask[out->len] = 0;
            out->len++;
            p += (p[1] == '?') ? 2 : 1;
            continue;
        }
        int hi = hexval(p[0]), lo = p[1] ? hexval(p[1]) : -1;
        if (hi < 0 || lo < 0) return -1;
        out->byte[out->len] = (uint8_t)((hi << 4) | lo);
        out->mask[out->len] = 0xFF;
        out->len++;
        p += 2;
    }
    return out->len ? 0 : -1;
}

size_t scan_find(const scan_pat *pat, const uint8_t *code, size_t len,
                 uintptr_t base_va, uintptr_t lo, uintptr_t hi,
                 uintptr_t *out, size_t max)
{
    if (!pat->len || pat->len > len) return 0;

    /* Anchor on the first non-wildcard byte so the common case rejects fast. */
    size_t anchor = 0;
    while (anchor < pat->len && !pat->mask[anchor]) anchor++;
    if (anchor == pat->len) return 0;   /* all wildcards matches everything */

    size_t start = 0, end = len - pat->len;
    if (lo && lo > base_va) start = lo - base_va;
    if (hi && hi < base_va + len) {
        uintptr_t h = hi - base_va;
        if (h < pat->len) return 0;
        end = h - pat->len;
    }

    size_t found = 0;
    const uint8_t want = pat->byte[anchor];
    for (size_t i = start; i <= end; i++) {
        if (code[i + anchor] != want) continue;
        size_t j = 0;
        for (; j < pat->len; j++)
            if (pat->mask[j] && code[i + j] != pat->byte[j]) break;
        if (j != pat->len) continue;
        if (found < max) out[found] = base_va + i;
        found++;
    }
    return found;
}

size_t scan_callers(uintptr_t target, const uint8_t *code, size_t len,
                    uintptr_t base_va, uintptr_t *out, size_t max)
{
    size_t found = 0;
    for (size_t i = 0; i + 5 <= len; i++) {
        if (code[i] != 0xE8 && code[i] != 0xE9) continue;   /* call / jmp rel32 */
        int32_t rel;
        memcpy(&rel, code + i + 1, 4);
        if (base_va + i + 5 + (intptr_t)rel != target) continue;
        if (found < max) out[found] = base_va + i;
        found++;
    }
    return found;
}
