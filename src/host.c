/* Host identification and image bounds.
 *
 * Steam re-execs through a chain of helper processes and every one of them
 * inherits LD_PRELOAD, so the first thing the extender must do is decide
 * whether it is inside the game and go quiet if it is not.
 *
 * That test must not depend on any plugin's knowledge. An earlier version
 * identified the host by scanning for the *movement* signature, which inverted
 * the layering: the extender could not know what it was running in without
 * borrowing a mod's private notion of what matters. Instead we look for the
 * game's own branding in read-only data — anchors that have nothing to do with
 * any feature and that no helper process carries.
 */
#include "host.h"

#include <link.h>
#include <string.h>

/* Distinctive, and unrelated to anything a plugin might patch. Any one of them
 * is enough; requiring all three would make us brittle to Larian dropping one. */
static const char *const ANCHORS[] = {
    "bg3nat1.larian.com",
    "Baldur's Gate 3",
    "LoadCDivinityStats",
};

struct collect {
    host_image exec;      /* the executable segment: where code scanning happens */
    const uint8_t *ro[8]; /* readable, non-executable segments: where strings live */
    size_t ro_len[8];
    int n_ro;
    int done;
};

static int collect_cb(struct dl_phdr_info *info, size_t sz, void *data)
{
    (void)sz;
    struct collect *c = data;

    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *p = &info->dlpi_phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R))
            continue;
        uintptr_t va = info->dlpi_addr + p->p_vaddr;

        if (p->p_flags & PF_X) {
            /* Keep the largest executable segment; that is .text. */
            if (p->p_memsz > c->exec.len) {
                c->exec.base_va = va;
                c->exec.code = (const uint8_t *)va;
                c->exec.len = p->p_memsz;
            }
        } else if (c->n_ro < 8) {
            c->ro[c->n_ro] = (const uint8_t *)va;
            c->ro_len[c->n_ro] = p->p_memsz;
            c->n_ro++;
        }
    }
    c->done = 1;
    return 1;   /* the first object reported is the main executable */
}

int host_probe(host_image *exec_out)
{
    struct collect c;
    memset(&c, 0, sizeof c);
    dl_iterate_phdr(collect_cb, &c);

    if (!c.done || !c.exec.len)
        return 0;
    *exec_out = c.exec;

    /* Search only the non-executable readable segments. They hold .rodata and
     * total a few tens of MB, against ~90 MB of .text — worth the distinction
     * when this runs on every process Steam spawns. */
    for (int s = 0; s < c.n_ro; s++) {
        for (size_t a = 0; a < sizeof ANCHORS / sizeof *ANCHORS; a++) {
            size_t n = strlen(ANCHORS[a]);
            if (n > c.ro_len[s]) continue;
            if (memmem(c.ro[s], c.ro_len[s], ANCHORS[a], n))
                return 1;
        }
    }
    return 0;
}
