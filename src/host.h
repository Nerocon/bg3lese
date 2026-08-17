#ifndef BG3LESE_HOST_H
#define BG3LESE_HOST_H

#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *code;   /* live, mapped */
    size_t len;
    uintptr_t base_va;     /* load bias already applied */
} host_image;

/* Fills `exec_out` with the main executable's code segment and returns 1 if
 * this process is Baldur's Gate 3, 0 otherwise. The test is independent of any
 * plugin: it looks for the game's branding in read-only data. */
int host_probe(host_image *exec_out);

#endif
