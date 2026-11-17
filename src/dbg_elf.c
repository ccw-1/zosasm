/* dbg_elf.c - the .dbg file of -g: an ELF32 big endian relocatable object
 * (machine S/390) with the DWARF sections, laid out like the file z/OS
 * /bin/as -g writes (DESIGN.md 21). */
#include <stdlib.h>
#include <string.h>

#include "dbg_int.h"

typedef struct {
    char *name;
    uint32_t type, flags, link, info, align, entsize;
    uint32_t off, size;
    const char *data; /* NULL: no bytes in the file */
    uint32_t name_off;
} ESec;

uint8_t *dbg_elf(Dbg *g, size_t *len) {
    Vec(ESec) secs = {0};
    Buf str = {0}, sym = {0}, shstr = {0}, out = {0}, *rels;
    int dbgsec[SEC_COUNT], relsec[SEC_COUNT];
    int i, nsecsym = 0, sec_of_sym[2 * SEC_COUNT + 1];
    size_t k, nsym;
    uint32_t symtab_idx, strtab_idx, first_secsym;
    ESec s;

    /* section table */
    memset(&s, 0, sizeof s);
    s.name = "";
    vec_push(&secs, s); /* 0: null */
    s.name = ".text";
    s.type = 8;      /* NOBITS */
    s.link = 0xFFF2; /* as z/OS as writes it */
    s.align = 1;
    vec_push(&secs, s);
    for (i = 0; i < SEC_COUNT; i++) {
        dbgsec[i] = relsec[i] = 0;
        if (!g->have[i]) continue;
        memset(&s, 0, sizeof s);
        s.name = (char *)g->sec[i].name;
        s.type = 1;
        s.align = 1;
        s.data = g->sec[i].d.p;
        s.size = (uint32_t)g->sec[i].d.len;
        dbgsec[i] = (int)secs.n;
        sec_of_sym[nsecsym++] = (int)secs.n;
        vec_push(&secs, s);
    }
    rels = xcalloc(SEC_COUNT * sizeof *rels);
    for (i = 0; i < SEC_COUNT; i++) {
        if (!g->have[i] || g->sec[i].rels.n == 0) continue;
        memset(&s, 0, sizeof s);
        s.name = xasprintf(".rel%s", g->sec[i].name);
        s.type = 9; /* REL */
        s.info = (uint32_t)dbgsec[i];
        s.align = 1;
        s.entsize = 8;
        relsec[i] = (int)secs.n;
        sec_of_sym[nsecsym++] = (int)secs.n;
        vec_push(&secs, s);
    }
    strtab_idx = (uint32_t)secs.n;
    memset(&s, 0, sizeof s);
    s.name = ".strtab";
    s.type = 3;
    s.align = 1;
    vec_push(&secs, s);
    symtab_idx = (uint32_t)secs.n;
    memset(&s, 0, sizeof s);
    s.name = ".symtab";
    s.type = 2;
    s.align = 1;
    s.entsize = 16;
    s.link = strtab_idx;
    vec_push(&secs, s);
    memset(&s, 0, sizeof s);
    s.name = ".shstrtab";
    s.type = 3;
    s.align = 1;
    vec_push(&secs, s);
    for (k = 0; k < secs.n; k++)
        if (secs.v[k].type == 9) secs.v[k].link = symtab_idx;

    /* symbols: null, FILE, one per control section, the signature, one per section, null */
    buf_putc(&str, 0);
    for (k = 0; k < 16; k++) buf_putc(&sym, 0);
    d32(&sym, (uint32_t)str.len);
    d32(&sym, 0);
    d32(&sym, 0);
    d8(&sym, 4); /* FILE, local */
    d8(&sym, 0);
    d16(&sym, 0xFFF1);
    buf_puts(&str, g->files.v[0].abs);
    buf_putc(&str, 0);
    for (k = 0; k < g->sects.n; k++) {
        char *nm = xasprintf(".text_cu1_id%u", (unsigned)g->sects.v[k]->id);
        d32(&sym, (uint32_t)str.len);
        d32(&sym, (uint32_t)g->sects.v[k]->base); /* offset in the module's B_TEXT; the object gives the base (DESIGN.md 21) */
        d32(&sym, (uint32_t)g->sects.v[k]->len);
        d8(&sym, 0);
        d8(&sym, 0);
        d16(&sym, 1);
        buf_puts(&str, nm);
        buf_putc(&str, 0);
    }
    d32(&sym, (uint32_t)str.len);
    d32(&sym, 0);
    d32(&sym, 0);
    d8(&sym, 0);
    d8(&sym, 0);
    d16(&sym, 1);
    buf_puts(&str, ".ADATA_");
    buf_puts(&str, g->sig_hex);
    buf_putc(&str, 0);
    first_secsym = (uint32_t)(sym.len / 16);
    for (i = 0; i < nsecsym; i++) {
        d32(&sym, 0);
        d32(&sym, 0);
        d32(&sym, secs.v[(size_t)sec_of_sym[i]].type == 9 ? 8 : 1);
        d8(&sym, 3); /* SECTION, local */
        d8(&sym, 0);
        d16(&sym, (unsigned)sec_of_sym[i]);
    }
    for (k = 0; k < 16; k++) buf_putc(&sym, 0);
    nsym = sym.len / 16;

    /* relocation tables */
    for (i = 0; i < SEC_COUNT; i++) {
        if (!relsec[i]) continue;
        for (k = 0; k < g->sec[i].rels.n; k++) {
            const DRel *r = &g->sec[i].rels.v[k];
            uint32_t symi = 0;
            if (r->kind == RK_TEXT) {
                symi = 2 + (uint32_t)r->idx;
            } else {
                int q = 0, j;
                for (j = 0; j < r->idx; j++)
                    if (dbgsec[j]) q++;
                symi = first_secsym + (uint32_t)q;
            }
            d32(&rels[i], r->off);
            d32(&rels[i], (symi << 8) | (uint32_t)r->type);
        }
        secs.v[(size_t)relsec[i]].data = rels[i].p;
        secs.v[(size_t)relsec[i]].size = (uint32_t)rels[i].len;
    }

    /* names, then the file */
    buf_putc(&shstr, 0);
    for (k = 1; k < secs.n; k++) {
        secs.v[k].name_off = (uint32_t)shstr.len;
        buf_puts(&shstr, secs.v[k].name);
        buf_putc(&shstr, 0);
    }
    secs.v[strtab_idx].data = str.p;
    secs.v[strtab_idx].size = (uint32_t)str.len;
    secs.v[symtab_idx].data = sym.p;
    secs.v[symtab_idx].size = (uint32_t)sym.len;
    secs.v[symtab_idx].info = (uint32_t)nsym;
    secs.v[secs.n - 1].data = shstr.p;
    secs.v[secs.n - 1].size = (uint32_t)shstr.len;

    for (k = 0; k < 52; k++) buf_putc(&out, 0);
    for (k = 1; k < secs.n; k++) {
        secs.v[k].off = secs.v[k].type == 8 ? 0 : (uint32_t)out.len;
        if (secs.v[k].data && secs.v[k].size) buf_write(&out, secs.v[k].data, secs.v[k].size);
    }
    while (out.len % 8) buf_putc(&out, 0); /* libelf of z/OS rejects an unaligned section header table */
    {
        uint32_t shoff = (uint32_t)out.len;
        Buf sh = {0}, hb = {0};
        static const uint8_t ident[16] = {0x7f, 'E', 'L', 'F', 1, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0};
        for (k = 0; k < secs.n; k++) {
            const ESec *e = &secs.v[k];
            d32(&sh, e->name_off);
            d32(&sh, e->type);
            d32(&sh, e->flags);
            d32(&sh, 0);
            d32(&sh, e->off);
            d32(&sh, e->size);
            d32(&sh, e->link);
            d32(&sh, e->info);
            d32(&sh, e->align);
            d32(&sh, e->entsize);
        }
        buf_write(&out, sh.p, sh.len);
        d16(&hb, 1);  /* ET_REL */
        d16(&hb, 22); /* EM_S390 */
        d32(&hb, 1);
        d32(&hb, 0);
        d32(&hb, 0);
        d32(&hb, shoff);
        d32(&hb, 0);
        d16(&hb, 52);
        d16(&hb, 0);
        d16(&hb, 0);
        d16(&hb, 40);
        d16(&hb, (unsigned)secs.n);
        d16(&hb, (unsigned)secs.n - 1);
        memcpy(out.p, ident, 16);
        memcpy(out.p + 16, hb.p, hb.len);
    }
    *len = out.len;
    return (uint8_t *)out.p;
}
