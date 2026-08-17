/* Pattern scanning: parsing, wildcards, bounds, and call-site discovery.
 * Pure unit tests over synthetic buffers — no game, no display. */
#include "scan.h"

#include <stdio.h>
#include <string.h>

static int fails;

static void ok(int cond, const char *what, const char *detail)
{
    printf("%s  %s%s%s\n", cond ? "PASS" : "FAIL", what,
           detail ? " — " : "", detail ? detail : "");
    if (!cond) fails++;
}

int main(void)
{
    char buf[160];
    scan_pat p;

    puts("\n=== pattern parsing ===");
    ok(scan_parse("48 8b 05 ?? ?? ?? ??", &p) == 0 && p.len == 7,
       "parses hex with wildcards", NULL);
    ok(p.mask[0] == 0xFF && p.byte[0] == 0x48, "fixed byte kept", NULL);
    ok(p.mask[3] == 0x00, "wildcard masked off", NULL);
    ok(scan_parse("488b05", &p) == 0 && p.len == 3, "tolerates no spaces", NULL);
    ok(scan_parse("48 ? 05", &p) == 0 && p.len == 3 && p.mask[1] == 0,
       "single ? is a wildcard too", NULL);
    ok(scan_parse("zz", &p) != 0, "rejects non-hex", NULL);
    ok(scan_parse("", &p) != 0, "rejects empty", NULL);

    puts("\n=== finding ===");
    /* A needle at a known offset, plus a near-miss that only wildcards match. */
    static uint8_t hay[4096];
    for (size_t i = 0; i < sizeof hay; i++) hay[i] = (uint8_t)(i * 7);
    const uint8_t needle[] = {0x48, 0x8b, 0x05, 0xDE, 0xAD, 0xBE, 0xEF};
    memcpy(hay + 1000, needle, sizeof needle);
    memcpy(hay + 3000, needle, sizeof needle);
    hay[3003] = 0x11;   /* differs only in a wildcard position */

    uintptr_t out[8];
    const uintptr_t BASE = 0x400000;

    scan_parse("48 8b 05 de ad be ef", &p);
    size_t n = scan_find(&p, hay, sizeof hay, BASE, 0, 0, out, 8);
    snprintf(buf, sizeof buf, "%zu at %#lx", n, n ? (unsigned long)out[0] : 0UL);
    ok(n == 1 && out[0] == BASE + 1000, "exact pattern finds one", buf);

    scan_parse("48 8b 05 ?? ad be ef", &p);
    n = scan_find(&p, hay, sizeof hay, BASE, 0, 0, out, 8);
    snprintf(buf, sizeof buf, "%zu found", n);
    ok(n == 2, "wildcard matches both", buf);

    n = scan_find(&p, hay, sizeof hay, BASE, BASE + 2000, 0, out, 8);
    snprintf(buf, sizeof buf, "%zu found, first %#lx", n, n ? (unsigned long)out[0] : 0UL);
    ok(n == 1 && out[0] == BASE + 3000, "lo bound excludes the earlier match", buf);

    n = scan_find(&p, hay, sizeof hay, BASE, 0, BASE + 2000, out, 8);
    ok(n == 1 && out[0] == BASE + 1000, "hi bound excludes the later match", NULL);

    /* The true total is returned even when it exceeds the caller's buffer, so a
     * caller checking for uniqueness is not fooled by truncation. */
    n = scan_find(&p, hay, sizeof hay, BASE, 0, 0, out, 1);
    snprintf(buf, sizeof buf, "returned %zu with max=1", n);
    ok(n == 2, "reports the true count past the buffer limit", buf);

    scan_parse("48 8b 05 de ad be ee", &p);
    ok(scan_find(&p, hay, sizeof hay, BASE, 0, 0, out, 8) == 0,
       "a near miss finds nothing", NULL);

    puts("\n=== call sites ===");
    /* call rel32 at 200 targeting 0x400500, and a jmp rel32 at 300 doing same. */
    static uint8_t code[1024];
    memset(code, 0x90, sizeof code);
    const uintptr_t TARGET = BASE + 0x500;
    int32_t rel = (int32_t)(TARGET - (BASE + 200 + 5));
    code[200] = 0xE8; memcpy(code + 201, &rel, 4);
    rel = (int32_t)(TARGET - (BASE + 300 + 5));
    code[300] = 0xE9; memcpy(code + 301, &rel, 4);
    rel = (int32_t)((TARGET + 8) - (BASE + 400 + 5));
    code[400] = 0xE8; memcpy(code + 401, &rel, 4);   /* different target */

    n = scan_callers(TARGET, code, sizeof code, BASE, out, 8);
    snprintf(buf, sizeof buf, "%zu found", n);
    ok(n == 2, "finds the call and the jmp, not the third", buf);
    ok(out[0] == BASE + 200 && out[1] == BASE + 300, "at the right addresses", NULL);

    printf("\n%s (%d failures)\n", fails ? "FAILED" : "ALL PASSED", fails);
    return fails != 0;
}
