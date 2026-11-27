#define _DEFAULT_SOURCE
#include "lz0.h"
#include "lz0dec.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

extern int lz0dec_rust_safe(const unsigned char *in, int in_size,
                            unsigned char *out, int out_max);
extern int lz0dec_rust_fast(const unsigned char *in, int in_size,
                            unsigned char *out, int out_max);

typedef int (*decfn)(const unsigned char *, int, unsigned char *, int);
static const struct {
    const char *name;
    decfn fn;
} RUST_DEC[] = {
    {"rs_safe", lz0dec_rust_safe},
    {"rs_fast", lz0dec_rust_fast},
};

static size_t pg;
typedef struct {
    unsigned char *map;
    size_t maplen;
    unsigned char *p;
} gbuf;

static gbuf galloc(size_t n) {
    gbuf g;
    size_t body = (n + pg - 1) / pg * pg;
    if (body == 0)
        body = pg;
    g.maplen = body + pg;
    g.map = mmap(NULL, g.maplen, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g.map == MAP_FAILED) {
        perror("mmap");
        exit(2);
    }
    if (mprotect(g.map + body, pg, PROT_NONE)) {
        perror("mprotect");
        exit(2);
    }
    g.p = g.map + body - n;
    return g;
}
static void gfree(gbuf g) { munmap(g.map, g.maplen); }

static uint64_t rng = 0x5A17D1FF12345678ull;
static uint64_t rnd(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return rng;
}

static int fails = 0;

static void check(const char *what, const unsigned char *src, int in_n, int out_max) {
    gbuf in = galloc((size_t)in_n);
    if (in_n > 0)
        memcpy(in.p, src, (size_t)in_n);
    gbuf ref = galloc((size_t)out_max);
    int rr = lz0dec_ref(in.p, in_n, ref.p, out_max);
    for (int d = 0; d < 2; d++) {
        gbuf out = galloc((size_t)out_max);
        int r = RUST_DEC[d].fn(in.p, in_n, out.p, out_max);
        if ((rr < 0 && r >= 0) || (rr >= 0 && r != rr) ||
            (rr > 0 && memcmp(out.p, ref.p, (size_t)rr))) {
            printf("FAIL %-12s %-8s in=%d out_max=%d ret=%d ref=%d\n",
                   what, RUST_DEC[d].name, in_n, out_max, r, rr);
            fails++;
        }
        gfree(out);
    }
    gfree(ref);
    gfree(in);
}

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    unsigned char *b = malloc(sz > 0 ? (size_t)sz : 1);
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return NULL;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

static void run_file_level(const char *path, int level) {
    size_t n = 0, cn = 0;
    unsigned char *orig = slurp(path, &n), *comp = NULL;
    if (!orig || n == 0) {
        free(orig);
        return;
    }
    lz0_config cfg;
    lz0_stats st;
    lz0_config_level(&cfg, level);
    if (lz0_compress(orig, n, &comp, &cn, &cfg, &st) != 0) {
        printf("compress failed %s L%d\n", path, level);
        fails++;
        free(orig);
        return;
    }
    int before = fails;
    check("exact", comp, (int)cn, (int)n);
    if (level == 6) {
        for (int extra = 1; extra <= 64; extra += 9)
            check("spare", comp, (int)cn, (int)n + extra);
        for (int cut = 1; cut <= 40 && (size_t)cut < n; cut += 3)
            check("short_out", comp, (int)cn, (int)n - cut);
        for (int cut = 1; cut <= 40 && (size_t)cut < cn; cut++)
            check("trunc", comp, (int)cn - cut, (int)n);
        unsigned char *mut = malloc(cn);
        for (int k = 0; k < 48; k++) {
            memcpy(mut, comp, cn);
            int flips = 1 + (int)(rnd() % 3);
            for (int f = 0; f < flips; f++) {
                size_t pos = (k & 1) ? cn - 1 - rnd() % (cn < 64 ? cn : 64)
                                     : rnd() % cn;
                mut[pos] ^= (unsigned char)(1u << (rnd() % 8));
            }
            check("flip", mut, (int)cn, (int)n);
        }
        free(mut);
    }
    if (level == 6)
        printf("%-36s n=%-7zu raw=%-7zu %s\n", path, n, cn,
               fails == before ? "ok" : "FAIL");
    free(comp);
    free(orig);
}

int main(void) {
    pg = (size_t)sysconf(_SC_PAGESIZE);

    /* Null / <= 0 boundary contract checks */
    {
        unsigned char b[8] = {0};
        for (int d = 0; d < 2; d++) {
            if (RUST_DEC[d].fn(NULL, 1, b, 8) >= 0 ||
                RUST_DEC[d].fn(b, 0, b, 8) >= 0 ||
                RUST_DEC[d].fn(b, -1, b, 8) >= 0 ||
                RUST_DEC[d].fn(b, 1, NULL, 8) >= 0 ||
                RUST_DEC[d].fn(b, 1, b, 0) >= 0 ||
                RUST_DEC[d].fn(b, 1, b, -1) >= 0) {
                printf("FAIL limits %s\n", RUST_DEC[d].name);
                fails++;
            }
        }
    }

    static const char *def[] = {
        "tests/vectors/literals.bin",
        "tests/vectors/matchlast.bin",
        "tests/vectors/matchnew.bin",
        "tests/vectors/mixed.bin",
        "bench/corpus/fields.c",
        "bench/corpus/grammar.lsp",
        "bench/corpus/xargs.1",
        "bench/corpus/sum",
        "bench/corpus/cp.html",
        NULL
    };
    for (int i = 0; def[i]; i++) {
        run_file_level(def[i], 1);
        run_file_level(def[i], 6);
        run_file_level(def[i], 9);
    }

    for (int len = 1; len <= 200; len += (len < 32 ? 1 : 29)) {
        unsigned char *buf = malloc((size_t)len);
        for (int i = 0; i < len; i++)
            buf[i] = (unsigned char)((i % 7 == 0) ? rnd() : (i % 3 ? 'a' : 'b'));
        unsigned char *c = NULL;
        size_t cs = 0;
        lz0_config cfg;
        lz0_stats st;
        lz0_config_level(&cfg, 6);
        if (lz0_compress(buf, (size_t)len, &c, &cs, &cfg, &st) == 0) {
            for (int om = 1; om <= len + 20; om++)
                check("synthetic", c, (int)cs, om);
            for (int cut = 1; (size_t)cut < cs; cut++)
                check("synth_trunc", c, (int)cs - cut, len);
            free(c);
        }
        free(buf);
    }

    if (fails) {
        printf("test_guard_rust: %d failure(s)\n", fails);
        return 1;
    }
    printf("test_guard_rust: rs_safe & rs_fast ok (L1/L6/L9, guard pages, trunc, bit flips)\n");
    return 0;
}

