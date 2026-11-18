/* elf.c - write a Module as a relocatable ELF64 object for Linux on Z
 * (big endian, EM_S390, ET_REL).  See DESIGN.md section 17.
 *
 * Mapping: every class of a control section becomes one ELF section
 * (.text.NAME, .rodata.NAME, .data.NAME or .bss.NAME), CSECT names and ENTRY
 * labels become global symbols, other labels local ones, EXTRN/WXTRN symbols
 * undefined (weak) globals, COM sections common symbols.  The addend of an
 * address constant is moved from the section bytes to a RELA entry.  Q, J, R,
 * CXD, length and subtracted relocations have no ELF counterpart and are
 * rejected.  Nothing here has been checked against an IBM or GNU assembler
 * object, only against the binutils tools (readelf, objdump, ld). */
#include <stdlib.h>
#include <string.h>

#include "asm.h"
#include "goff.h"

#define EM_S390_ 22
#define PRIV_NAME_ELF "$PRIV000000"

enum {
    SHT_NULL_ = 0, SHT_PROGBITS_ = 1, SHT_SYMTAB_ = 2, SHT_STRTAB_ = 3, SHT_RELA_ = 4, SHT_NOBITS_ = 8,
    SHF_WRITE_ = 1, SHF_ALLOC_ = 2, SHF_EXECINSTR_ = 4, SHF_INFO_LINK_ = 0x40,
    STB_LOCAL_ = 0, STB_GLOBAL_ = 1, STB_WEAK_ = 2,
    STT_NOTYPE_ = 0, STT_OBJECT_ = 1, STT_FUNC_ = 2, STT_SECTION_ = 3, STT_FILE_ = 4,
    SHN_ABS_ = 0xfff1, SHN_COMMON_ = 0xfff2,
    R_390_32_ = 4, R_390_PC16DBL_ = 17, R_390_PLT16DBL_ = 18,
    R_390_PC32DBL_ = 19, R_390_PLT32DBL_ = 20, R_390_64_ = 22
};

/* ---- byte output ----------------------------------------------------- */

static void put16(Buf *b, unsigned v) {
    buf_putc(b, (int)((v >> 8) & 0xFF));
    buf_putc(b, (int)(v & 0xFF));
}
static void put32(Buf *b, uint32_t v) {
    put16(b, v >> 16);
    put16(b, v & 0xFFFF);
}
static void put64(Buf *b, uint64_t v) {
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}
static void pad_to(Buf *b, size_t align) {
    while (align > 1 && b->len % align) buf_putc(b, 0);
}

/* ---- string tables --------------------------------------------------- */

typedef struct {
    Buf b;
} StrTab;

static uint32_t strtab_add(StrTab *t, const char *s) {
    uint32_t off;
    if (t->b.len == 0) buf_putc(&t->b, 0); /* index 0 is the empty name */
    if (!s[0]) return 0;
    off = (uint32_t)t->b.len;
    buf_write(&t->b, s, strlen(s));
    buf_putc(&t->b, 0);
    return off;
}

/* ---- the sections ---------------------------------------------------- */

typedef struct {
    Class *cls;
    char *name;
    uint32_t type;
    uint64_t flags;
    uint64_t align;
    uint8_t *data; /* a copy, relocation addends cleared */
    uint32_t sym;  /* index of its section symbol */
    Buf rela;
    size_t nrela;
    uint64_t foff; /* file offset of the contents */
} ElfSec;

typedef struct {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
} ElfSym;

typedef struct {
    Module *m;
    char *err;
    size_t errsz;
    Vec(ElfSec) secs;
    Vec(ElfSym) syms;
    StrTab strtab;
    uint32_t nlocal;
} Ctx;

static int fail(Ctx *x, const char *what, const char *sect) {
    snprintf(x->err, x->errsz, "%s%s%s", sect && sect[0] ? sect : "", sect && sect[0] ? ": " : "", what);
    return 0;
}

static const char *sname(const Section *s) { return s->name[0] ? s->name : PRIV_NAME_ELF; }

static const char *sect_label(const Section *s) { return s->name[0] ? s->name : NULL; }

static int class_is_code(const Class *c) {
    return !strcmp(c->name, "B_TEXT") || c->attr.executable == GOFF_EXEC_CODE;
}

static uint64_t class_align(const Class *c) {
    int lg = c->attr.align >= 0 ? c->attr.align : GOFF_ALIGN_DBL;
    uint64_t a = (uint64_t)1 << lg;
    if ((uint64_t)c->sect->max_align > a) a = (uint64_t)c->sect->max_align;
    return a;
}

/* the ELF section of a class, or NULL (class without bytes) */
static ElfSec *sec_of(Ctx *x, const Class *c) {
    return c->esd ? &x->secs.v[c->esd - 1] : NULL;
}

static void add_sec(Ctx *x, Section *s, Class *c) {
    ElfSec e;
    const char *prefix, *label = sect_label(s);
    Buf nm = {0};
    memset(&e, 0, sizeof e);
    e.cls = c;
    e.align = class_align(c);
    if (c->attr.noload) {
        prefix = ".bss";
        e.type = SHT_NOBITS_;
        e.flags = SHF_ALLOC_ | SHF_WRITE_;
    } else if (class_is_code(c)) {
        prefix = ".text";
        e.type = SHT_PROGBITS_;
        e.flags = SHF_ALLOC_ | SHF_EXECINSTR_;
    } else if (c->attr.readonly) {
        prefix = ".rodata";
        e.type = SHT_PROGBITS_;
        e.flags = SHF_ALLOC_;
    } else {
        prefix = ".data";
        e.type = SHT_PROGBITS_;
        e.flags = SHF_ALLOC_ | SHF_WRITE_;
    }
    buf_write(&nm, prefix, strlen(prefix));
    if (label) {
        buf_putc(&nm, '.');
        buf_write(&nm, label, strlen(label));
    }
    if (strcmp(c->name, "B_TEXT") != 0 && strcmp(c->name, "B_PRV") != 0) {
        buf_putc(&nm, '.');
        buf_write(&nm, c->name, strlen(c->name));
    }
    e.name = xstrdup(buf_cstr(&nm));
    if (e.type != SHT_NOBITS_ && c->len > 0) {
        e.data = xmalloc((size_t)c->len);
        memcpy(e.data, c->data, (size_t)c->len);
    }
    vec_push(&x->secs, e);
    c->esd = (uint32_t)x->secs.n; /* 1 based, see sec_of */
}

/* ---- symbols --------------------------------------------------------- */

static uint32_t add_sym(Ctx *x, const char *name, int bind, int type, uint16_t shndx, uint64_t value,
                        uint64_t size) {
    ElfSym s;
    s.name = strtab_add(&x->strtab, name);
    s.info = (uint8_t)((bind << 4) | type);
    s.other = 0;
    s.shndx = shndx;
    s.value = value;
    s.size = size;
    vec_push(&x->syms, s);
    return (uint32_t)x->syms.n - 1;
}

/* section header index of an emitted class */
static uint16_t shndx_of(const Class *c) { return (uint16_t)c->esd; }

static int64_t label_offset(const Symbol *sym) {
    return sym->piece->start + (sym->kind == SYM_LABEL ? sym->off : 0);
}

static int sym_in_object(const Symbol *sym) {
    return sym->piece && sym->piece->cls && sym->piece->cls->sect->kind != K_DSECT &&
           sym->piece->cls->esd != 0;
}

/* a symbol of a COM section stands for the whole section */
static Symbol *common_symbol(Ctx *x, const Section *s) {
    return module_symbol(x->m, s->name);
}

/* ---- relocations ----------------------------------------------------- */

static int64_t read_be(const uint8_t *p, int n, int is_signed) {
    uint64_t v = 0;
    int i;
    for (i = 0; i < n; i++) v = (v << 8) | p[i];
    if (is_signed && n < 8 && (v >> (n * 8 - 1))) v |= ~(uint64_t)0 << (n * 8);
    return (int64_t)v;
}

static void write_rela(ElfSec *e, uint64_t off, uint32_t sym, uint32_t type, int64_t addend) {
    put64(&e->rela, off);
    put64(&e->rela, ((uint64_t)sym << 32) | type);
    put64(&e->rela, (uint64_t)addend);
    e->nrela++;
}

typedef struct {
    int64_t off;
    size_t idx;
} Key;

static int cmp_key(const void *p, const void *q) {
    const Key *a = p, *b = q;
    if (a->off != b->off) return a->off < b->off ? -1 : 1;
    return a->idx < b->idx ? -1 : a->idx > b->idx;
}

static int do_relocs(Ctx *x, ElfSec *e) {
    Class *c = e->cls;
    const char *sn = sname(c->sect);
    Key *keys = xmalloc((c->relocs.n + 1) * sizeof *keys);
    size_t k;
    for (k = 0; k < c->relocs.n; k++) {
        keys[k].off = c->relocs.v[k].off;
        keys[k].idx = k;
    }
    qsort(keys, c->relocs.n, sizeof *keys, cmp_key);
    for (k = 0; k < c->relocs.n; k++)
        if (c->relocs.v[k].sub)
            return fail(x, "a subtracted relocatable term (A(X-Y) across sections) has no ELF form", sn);
    for (k = 0; k < c->relocs.n; k++) {
        const Reloc *r = &c->relocs.v[keys[k].idx];
        uint32_t sym = 0, type = 0;
        int64_t addend;
        uint8_t *at;
        if (e->type == SHT_NOBITS_) return fail(x, "a class without load text has relocations", sn);
        if (k > 0 && keys[k - 1].off == r->off)
            return fail(x, "an address constant with several relocatable terms has no ELF form", sn);
        if (!e->data || r->off < 0 || r->off + r->len > e->cls->len)
            return fail(x, "a relocation lies outside the class text", sn);
        switch (r->kind) {
        case REL_ADDR:
        case REL_V:
        case REL_RI:
            break;
        case REL_Q:
        case REL_QY:
            return fail(x, "Q-type constants have no ELF form", sn);
        case REL_CXD:
            return fail(x, "CXD has no ELF form", sn);
        case REL_LEN:
            return fail(x, "length relocations have no ELF form", sn);
        default:
            return fail(x, "R-type constants have no ELF form", sn);
        }
        at = e->data + r->off;
        if (r->kind == REL_RI) {
            /* the instruction starts two bytes before the immediate field */
            int plt = 0;
            if (r->off < 2 || (r->len != 2 && r->len != 4))
                return fail(x, "this relative relocation has no ELF form", sn);
            /* an external target stores its own offset only (as IBM does); the instruction starts 2 bytes before
             * the field, which is what the PC-relative relocation measures from */
            addend = r->target ? read_be(at, r->len, 1) * 2 + 2 : read_be(at, r->len, 1) * 2 + r->off;
            if (r->target && r->target->kind == SYM_EXT) {
                /* BRC/BRAS (A7x4/A7x5) and BRCL/BRASL (C0x4/C0x5) may go through a PLT */
                int op0 = at[-2], op1 = at[-1] & 0x0F;
                plt = (op0 == 0xA7 || op0 == 0xC0) && (op1 == 4 || op1 == 5);
            }
            type = r->len == 4 ? (plt ? R_390_PLT32DBL_ : R_390_PC32DBL_)
                               : (plt ? R_390_PLT16DBL_ : R_390_PC16DBL_);
        } else {
            addend = read_be(at, r->len, 1);
            switch (r->len) {
            case 4: type = R_390_32_; break;
            case 8: type = R_390_64_; break;
            default:
                return fail(x, "a relocatable constant of this length has no ELF form (use A or AD)", sn);
            }
        }
        memset(at, 0, (size_t)r->len);
        if (!r->target) {
            ElfSec *t = r->cls ? sec_of(x, r->cls) : NULL;
            if (r->cls && r->cls->sect->kind == K_COM) {
                Symbol *cs = common_symbol(x, r->cls->sect);
                if (!cs || !cs->esd) return fail(x, "relocation against a COM section without a name", sn);
                sym = cs->esd;
            } else if (!t) {
                return fail(x, "relocation against a class without text", sn);
            } else {
                sym = t->sym;
            }
        } else if (r->target->kind == SYM_EXT) {
            sym = r->target->esd;
        } else {
            /* a label or CSECT name: the section symbol plus the label offset */
            const Symbol *t = r->target;
            if (!sym_in_object(t)) return fail(x, "relocation against a symbol that is not in a section", sn);
            sym = sec_of(x, t->piece->cls)->sym;
            addend += label_offset(t);
        }
        write_rela(e, (uint64_t)r->off, sym, type, addend);
    }
    free(keys);
    return 1;
}

/* ---- the whole object ------------------------------------------------ */

uint8_t *module_emit_elf(Module *m, size_t *len, char *err, size_t errsz) {
    Ctx x;
    size_t si, ci, i, k, nsec, nrela_secs = 0;
    SectionVec secs = {0};
    SymbolVec externs = {0};
    Buf out = {0}, shstr = {0};
    uint32_t symtab_idx, strtab_idx, shstr_idx, note_idx, rela_first;
    uint64_t *secoff, shoff, symoff, stroff, shstroff;
    size_t nsh;
    Buf symbuf = {0};

    memset(&x, 0, sizeof x);
    x.m = m;
    x.err = err;
    x.errsz = errsz;

    for (si = 0; si < m->sections.n; si++)
        if (m->sections.v[si]->kind != K_DSECT) vec_push(&secs, m->sections.v[si]);
    if (secs.n == 0) {
        snprintf(err, errsz, "no control section to emit");
        return NULL;
    }
    if (!module_validate(m, err, errsz)) return NULL;

    /* clear a mark left by an earlier use of the class esdids */
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) secs.v[si]->classes.v[ci]->esd = 0;

    /* sections: one per class with bytes (the first class always, it holds the name) */
    for (si = 0; si < secs.n; si++) {
        Section *s = secs.v[si];
        if (s->kind == K_COM) continue;
        for (ci = 0; ci < s->classes.n; ci++) {
            Class *c = s->classes.v[ci];
            if (c->len > 0 || ci == 0) add_sec(&x, s, c);
        }
    }

    /* symbols: null, file, section symbols, local labels, then the globals */
    add_sym(&x, "", STB_LOCAL_, STT_NOTYPE_, 0, 0, 0);
    add_sym(&x, m->name && m->name[0] ? m->name : "zosasm", STB_LOCAL_, STT_FILE_, SHN_ABS_, 0, 0);
    for (i = 0; i < x.secs.n; i++) x.secs.v[i].sym = add_sym(&x, "", STB_LOCAL_, STT_SECTION_, (uint16_t)(i + 1), 0, 0);
    for (i = 0; i < m->sym_order.n; i++) {
        Symbol *sym = m->sym_order.v[i];
        if (sym->kind != SYM_LABEL || sym->entry || !sym_in_object(sym)) continue;
        sym->esd = add_sym(&x, sym->name, STB_LOCAL_, STT_NOTYPE_, shndx_of(sym->piece->cls),
                           (uint64_t)label_offset(sym), 0);
    }
    x.nlocal = (uint32_t)x.syms.n;
    for (i = 0; i < m->sym_order.n; i++) {
        Symbol *sym = m->sym_order.v[i];
        int code;
        uint64_t size = 0;
        if ((sym->kind != SYM_LABEL && sym->kind != SYM_SECT) || !sym_in_object(sym)) continue;
        if (!sym->entry && sym->kind != SYM_SECT) continue;
        if (sym->kind == SYM_SECT && (!sym->piece->cls || sym->piece->cls->sect->kind == K_COM)) continue;
        code = class_is_code(sym->piece->cls);
        if (sym->kind == SYM_SECT && sym->piece->cls == sym->piece->cls->sect->classes.v[0])
            size = (uint64_t)sym->piece->cls->len;
        sym->esd = add_sym(&x, sym->name, STB_GLOBAL_, code ? STT_FUNC_ : STT_OBJECT_,
                           shndx_of(sym->piece->cls), (uint64_t)label_offset(sym), size);
    }
    /* COM sections */
    for (si = 0; si < secs.n; si++) {
        Section *s = secs.v[si];
        Symbol *cs;
        int64_t total = 0;
        if (s->kind != K_COM) continue;
        for (ci = 0; ci < s->classes.n; ci++) total += s->classes.v[ci]->len;
        cs = common_symbol(&x, s);
        if (!cs || !s->name[0]) {
            snprintf(err, errsz, "an unnamed COM section has no ELF form");
            return NULL;
        }
        cs->esd = add_sym(&x, s->name, STB_GLOBAL_, STT_OBJECT_, SHN_COMMON_,
                          (uint64_t)(s->max_align > 0 ? s->max_align : 8), (uint64_t)total);
    }
    /* undefined symbols: every external symbol a relocation refers to */
    for (si = 0; si < secs.n; si++)
        for (ci = 0; ci < secs.v[si]->classes.n; ci++) {
            Class *c = secs.v[si]->classes.v[ci];
            for (k = 0; k < c->relocs.n; k++) {
                Symbol *t = c->relocs.v[k].target;
                size_t q;
                int seen = 0;
                if (!t || t->kind != SYM_EXT) continue;
                for (q = 0; q < externs.n; q++)
                    if (externs.v[q] == t) seen = 1;
                if (seen) continue;
                vec_push(&externs, t);
                t->esd = add_sym(&x, t->name, t->weak ? STB_WEAK_ : STB_GLOBAL_, STT_NOTYPE_, 0, 0, 0);
            }
        }
    /* ENTRY of the module (END operand) is not an ELF notion; nothing to emit */

    for (i = 0; i < x.secs.n; i++)
        if (!do_relocs(&x, &x.secs.v[i])) return NULL;

    /* section header numbering */
    nsec = x.secs.n;
    note_idx = (uint32_t)nsec + 1;
    symtab_idx = note_idx + 1;
    strtab_idx = symtab_idx + 1;
    shstr_idx = strtab_idx + 1;
    rela_first = shstr_idx + 1;
    for (i = 0; i < nsec; i++)
        if (x.secs.v[i].nrela) nrela_secs++;
    nsh = rela_first + nrela_secs;

    /* layout */
    buf_write(&out, "\x7f" "ELF", 4);
    buf_putc(&out, 2); /* ELFCLASS64 */
    buf_putc(&out, 2); /* ELFDATA2MSB */
    buf_putc(&out, 1); /* EV_CURRENT */
    while (out.len < 16) buf_putc(&out, 0);
    put16(&out, 1);        /* ET_REL */
    put16(&out, EM_S390_); /* EM_S390 */
    put32(&out, 1);
    put64(&out, 0);        /* entry */
    put64(&out, 0);        /* phoff */
    put64(&out, 0);        /* shoff, patched below */
    put32(&out, 0);        /* flags */
    put16(&out, 64);       /* ehsize */
    put16(&out, 0);        /* phentsize */
    put16(&out, 0);        /* phnum */
    put16(&out, 64);       /* shentsize */
    put16(&out, (unsigned)nsh);
    put16(&out, shstr_idx);

    secoff = xcalloc((nsh + 1) * sizeof *secoff);
    for (i = 0; i < nsec; i++) {
        ElfSec *e = &x.secs.v[i];
        pad_to(&out, (size_t)e->align);
        e->foff = out.len;
        if (e->type == SHT_NOBITS_) continue;
        if (e->cls->len > 0) buf_write(&out, e->data, (size_t)e->cls->len);
    }
    pad_to(&out, 8);
    {
        size_t r = 0;
        for (i = 0; i < nsec; i++) {
            ElfSec *e = &x.secs.v[i];
            if (!e->nrela) continue;
            secoff[rela_first + r++] = out.len;
            buf_write(&out, e->rela.p, e->rela.len);
        }
    }
    /* symbol table */
    for (i = 0; i < x.syms.n; i++) {
        ElfSym *s = &x.syms.v[i];
        put32(&symbuf, s->name);
        buf_putc(&symbuf, s->info);
        buf_putc(&symbuf, s->other);
        put16(&symbuf, s->shndx);
        put64(&symbuf, s->value);
        put64(&symbuf, s->size);
    }
    symoff = out.len;
    buf_write(&out, symbuf.p, symbuf.len);
    if (x.strtab.b.len == 0) buf_putc(&x.strtab.b, 0);
    stroff = out.len;
    buf_write(&out, x.strtab.b.p, x.strtab.b.len);

    /* section header string table */
    {
        uint32_t *nameoff = xcalloc((nsh + 1) * sizeof *nameoff);
        StrTab st = {0};
        size_t r = 0;
        char tmp[300];
        strtab_add(&st, "");
        for (i = 0; i < nsec; i++) nameoff[i + 1] = strtab_add(&st, x.secs.v[i].name);
        nameoff[note_idx] = strtab_add(&st, ".note.GNU-stack");
        nameoff[symtab_idx] = strtab_add(&st, ".symtab");
        nameoff[strtab_idx] = strtab_add(&st, ".strtab");
        nameoff[shstr_idx] = strtab_add(&st, ".shstrtab");
        for (i = 0; i < nsec; i++) {
            if (!x.secs.v[i].nrela) continue;
            snprintf(tmp, sizeof tmp, ".rela%s", x.secs.v[i].name);
            nameoff[rela_first + r++] = strtab_add(&st, tmp);
        }
        shstroff = out.len;
        buf_write(&out, st.b.p, st.b.len);
        shstr = st.b;
        pad_to(&out, 8);
        shoff = out.len;
        /* patch e_shoff */
        {
            size_t at = 40;
            for (k = 0; k < 8; k++) out.p[at + k] = (uint8_t)(shoff >> (56 - 8 * k));
        }
        /* null header */
        for (k = 0; k < 64; k++) buf_putc(&out, 0);
        for (i = 0; i < nsec; i++) {
            ElfSec *e = &x.secs.v[i];
            put32(&out, nameoff[i + 1]);
            put32(&out, e->type);
            put64(&out, e->flags);
            put64(&out, 0);
            put64(&out, e->foff);
            put64(&out, (uint64_t)e->cls->len);
            put32(&out, 0);
            put32(&out, 0);
            put64(&out, e->align);
            put64(&out, 0);
        }
        /* .note.GNU-stack: empty, tells the linker the stack need not be executable */
        put32(&out, nameoff[note_idx]);
        put32(&out, SHT_PROGBITS_);
        put64(&out, 0);
        put64(&out, 0);
        put64(&out, shoff);
        put64(&out, 0);
        put32(&out, 0);
        put32(&out, 0);
        put64(&out, 1);
        put64(&out, 0);
        /* .symtab */
        put32(&out, nameoff[symtab_idx]);
        put32(&out, SHT_SYMTAB_);
        put64(&out, 0);
        put64(&out, 0);
        put64(&out, symoff);
        put64(&out, symbuf.len);
        put32(&out, strtab_idx);
        put32(&out, x.nlocal);
        put64(&out, 8);
        put64(&out, 24);
        /* .strtab */
        put32(&out, nameoff[strtab_idx]);
        put32(&out, SHT_STRTAB_);
        put64(&out, 0);
        put64(&out, 0);
        put64(&out, stroff);
        put64(&out, x.strtab.b.len);
        put32(&out, 0);
        put32(&out, 0);
        put64(&out, 1);
        put64(&out, 0);
        /* .shstrtab */
        put32(&out, nameoff[shstr_idx]);
        put32(&out, SHT_STRTAB_);
        put64(&out, 0);
        put64(&out, 0);
        put64(&out, shstroff);
        put64(&out, shstr.len);
        put32(&out, 0);
        put32(&out, 0);
        put64(&out, 1);
        put64(&out, 0);
        /* .rela sections */
        r = 0;
        for (i = 0; i < nsec; i++) {
            ElfSec *e = &x.secs.v[i];
            if (!e->nrela) continue;
            put32(&out, nameoff[rela_first + r]);
            put32(&out, SHT_RELA_);
            put64(&out, SHF_INFO_LINK_);
            put64(&out, 0);
            put64(&out, secoff[rela_first + r]);
            put64(&out, e->rela.len);
            put32(&out, symtab_idx);
            put32(&out, (uint32_t)(i + 1));
            put64(&out, 8);
            put64(&out, 24);
            r++;
        }
    }
    *len = out.len;
    {
        uint8_t *copy = xmalloc(out.len + 1);
        memcpy(copy, out.p, out.len);
        return copy;
    }
}
