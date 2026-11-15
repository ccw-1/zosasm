/* zsrc.c - the original sources in the object (zsrc.h). */
#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lz.h"
#include "zio.h"
#include "zsrc.h"

#define COMPRESS_MIN 64            /* shorter files are stored */
#define COMPRESS_MAX ((size_t)1 << 30) /* larger files are stored (LZ positions are 32 bit) */

/* ---- writer ---------------------------------------------------------------- */

uint8_t *zsrc_build(Module *m, size_t *len) {
    Buf out = {0}, p = {0};
    size_t i;
    uint8_t *res;
    *len = 0;
    if (m->zlevel < 2) return NULL;

    put_str(&p, "zosasm 1");
    buf_putc(&p, m->zlevel);
    put_uv(&p, m->zdefs.n);
    for (i = 0; i < m->zdefs.n; i++) put_str(&p, m->zdefs.v[i]);
    put_uv(&p, m->zsrc.n);
    zsym_put_chunk(&out, ZR_HEAD, &p);

    for (i = 0; i < m->zsrc.n; i++) {
        const ZSrcIn *f = &m->zsrc.v[i];
        Buf z = {0};
        int method = ZR_STORED;
        p.len = 0;
        put_str(&p, f->name);
        put_uv(&p, f->len);
        put_u32(&p, zsym_crc((const uint8_t *)f->text, f->len, NULL, 0, (int64_t)f->len));
        if (f->len >= COMPRESS_MIN && f->len <= COMPRESS_MAX) {
            lz_compress((const uint8_t *)f->text, f->len, &z);
            if (z.len < f->len) method = ZR_LZ;
        }
        buf_putc(&p, method);
        if (method == ZR_LZ)
            buf_write(&p, z.p, z.len);
        else
            buf_write(&p, f->text, f->len);
        zsym_put_chunk(&out, ZR_FILE, &p);
    }

    p.len = 0;
    put_uv(&p, m->zsrc.n);
    zsym_put_chunk(&out, ZR_END, &p);

    res = xmalloc(out.len + 1);
    memcpy(res, out.p, out.len);
    *len = out.len;
    return res;
}

/* ---- reader ---------------------------------------------------------------- */

static void parse_head(Rd *r, ZSrcBlock *b) {
    uint64_t nd, i;
    b->producer = get_str(r);
    b->level = get_u8(r);
    nd = get_uv(r);
    if (r->bad || nd > (uint64_t)(r->end - r->p)) {
        r->bad = 1;
        return;
    }
    for (i = 0; i < nd && !r->bad; i++) {
        char *d = get_str(r);
        vec_push(&b->defs, d);
    }
    b->want = get_uv(r);
}

/* a FILE chunk; false when it is damaged */
static int parse_file(Rd *r, ZSrcBlock *b) {
    ZSrcFile f;
    uint64_t len;
    int method;
    size_t packed;
    memset(&f, 0, sizeof f);
    f.name = get_str(r);
    len = get_uv(r);
    f.crc = get_u32(r);
    method = get_u8(r);
    if (r->bad) return 0;
    packed = (size_t)(r->end - r->p);
    f.method = method;
    f.packed = packed;
    f.len = (size_t)len;
    if (method == ZR_STORED) {
        if (len != packed) return 0;
        f.data = xmalloc(packed + 1);
        memcpy(f.data, r->p, packed);
    } else if (method == ZR_LZ) {
        /* a block expands at most about 255 times; refuse sizes it cannot reach */
        if (len > ((uint64_t)1 << 31) || len / 256 > packed) return 0;
        f.data = xmalloc((size_t)len + 1);
        if (!lz_decompress(r->p, packed, f.data, (size_t)len)) {
            free(f.data);
            return 0;
        }
    } else {
        return 0;
    }
    f.data[f.len] = 0;
    if (zsym_crc(f.data, f.len, NULL, 0, (int64_t)f.len) != f.crc) {
        free(f.data);
        return 0;
    }
    vec_push(&b->files, f);
    return 1;
}

ZSrc *zsrc_parse(const uint8_t *b, size_t n) {
    ZSrc *z = xcalloc(sizeof *z);
    ZSrcBlock *cur = NULL;
    size_t i = 0;
    int in_junk = 0;
    while (i < n) {
        Rd r;
        int ver, kind;
        size_t used = chunk_at(b, n, i, &ver, &kind, &r);
        if (used) {
            in_junk = 0;
            if (ver != ZSYM_VERSION) {
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
            } else if (kind == ZR_HEAD) {
                cur = xcalloc(sizeof *cur);
                vec_push(&z->blocks, cur);
                parse_head(&r, cur);
                if (r.bad) cur->damaged = 1;
            } else if (kind == ZR_FILE) {
                if (!cur) {
                    z->bad_chunks++;
                } else if (!parse_file(&r, cur)) {
                    cur->damaged = 1;
                }
            } else if (kind == ZR_END) {
                if (!cur) {
                    z->bad_chunks++;
                } else {
                    uint64_t nf = get_uv(&r);
                    cur->complete = !r.bad && !cur->damaged && nf == cur->files.n && nf == cur->want;
                    if (r.bad) cur->damaged = 1;
                    cur = NULL;
                }
            } else {
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
            }
            i += used;
        } else {
            if (b[i] != 0 && !in_junk) {
                z->bad_chunks++;
                if (cur) cur->damaged = 1;
                in_junk = 1;
            }
            i++;
        }
    }
    return z;
}

uint8_t *zsrc_extract(const GoffModule *m, size_t *len) { return zsym_extract_class(m, ZSRC_CLASS, len); }

void zsrc_dump(const ZSrc *z, FILE *f) {
    size_t bi, i;
    fprintf(f, "ZSRC: %zu block%s", z->blocks.n, z->blocks.n == 1 ? "" : "s");
    if (z->bad_chunks)
        fprintf(f, ", %d damaged or unknown chunk%s skipped", z->bad_chunks, z->bad_chunks == 1 ? "" : "s");
    fputc('\n', f);
    for (bi = 0; bi < z->blocks.n; bi++) {
        const ZSrcBlock *b = z->blocks.v[bi];
        fprintf(f, "block %zu: %s, level %d, %s\n", bi + 1, b->producer ? b->producer : "?", b->level,
                b->complete ? "complete" : b->damaged ? "DAMAGED" : "INCOMPLETE");
        for (i = 0; i < b->defs.n; i++) fprintf(f, "  define %s\n", b->defs.v[i]);
        for (i = 0; i < b->files.n; i++) {
            const ZSrcFile *x = &b->files.v[i];
            fprintf(f, "  file %zu: %s, %zu bytes", i, x->name, x->len);
            if (x->method == ZR_LZ)
                fprintf(f, ", lz %zu bytes (%zu%%)", x->packed, x->len ? x->packed * 100 / x->len : 0);
            else
                fputs(", stored", f);
            fprintf(f, ", crc %08X\n", (unsigned)x->crc);
        }
    }
}

/* ---- unpack ---------------------------------------------------------------- */

/* the file name below dir: no leading "/", no "." and ".." components */
static char *safe_path(const char *dir, const char *name) {
    Buf b = {0};
    const char *p = name;
    int any = 0;
    buf_puts(&b, dir);
    while (*p) {
        size_t n = strcspn(p, "/");
        if (n == 0 || (n == 1 && p[0] == '.')) {
            /* skip */
        } else {
            buf_putc(&b, '/');
            if (n == 2 && p[0] == '.' && p[1] == '.')
                buf_puts(&b, "__");
            else
                buf_write(&b, p, n);
            any = 1;
        }
        p += n;
        if (*p == '/') p++;
    }
    if (!any) buf_puts(&b, "/source");
    return xstrdup(buf_cstr(&b));
}

static int make_dirs(const char *path) { /* every directory above the last component */
    char *s = xstrdup(path);
    size_t i;
    for (i = 1; s[i]; i++) {
        if (s[i] != '/') continue;
        s[i] = 0;
        if (mkdir(s, 0777) != 0 && errno != EEXIST) {
            fprintf(stderr, "zosasm: %s: %s\n", s, strerror(errno));
            return 0;
        }
        s[i] = '/';
    }
    return 1;
}

int zsrc_unpack(const ZSrc *z, const char *dir, FILE *out) {
    size_t bi, i;
    int count = 0;
    for (bi = 0; bi < z->blocks.n; bi++) {
        const ZSrcBlock *b = z->blocks.v[bi];
        char *base = bi ? xasprintf("%s/%zu", dir, bi + 1) : xstrdup(dir);
        for (i = 0; i < b->files.n; i++) {
            const ZSrcFile *x = &b->files.v[i];
            char *path = safe_path(base, x->name);
            FILE *f;
            if (!make_dirs(path)) return -1;
            f = fopen(path, "wb");
            if (!f) {
                fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
                return -1;
            }
            if ((x->len && fwrite(x->data, 1, x->len, f) != x->len) || fclose(f) != 0) {
                fprintf(stderr, "zosasm: %s: %s\n", path, strerror(errno));
                return -1;
            }
            if (out) fprintf(out, "%s\n", path);
            count++;
        }
    }
    return count;
}
