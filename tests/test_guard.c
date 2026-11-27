/*
 * test_guard: strict memory-safety and differential test for all decoders.
 *
 * Unlike the other tests (which use malloc'd buffers with slack), every
 * decoder call here gets:
 *   - an input buffer whose last byte is immediately followed by a
 *     PROT_NONE page, so any read past in_size faults;
 *   - an output buffer of exactly out_max bytes, also followed by a
 *     PROT_NONE page, so any write (or read) past out_max faults.
 *
 * For each input file it checks, against lz0dec_ref:
 *   1. exact round trip (out_max == size, and out_max == size + 1..64);
 *   2. out_max smaller than the decoded size -> -1;
 *   3. truncated streams at many cut points -> identical return value;
 *   4. random bit flips -> identical return value and identical output.
 *
 * Usage: test_guard [file ...]   (default: a built-in corpus list)
 *        Raw streams are produced with lz0_compress (level 6), or loaded
 *        from <file>.l6.lz0raw when that file exists.
 */
#define _DEFAULT_SOURCE
#include "lz0.h"
#include "lz0dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

typedef int (*decfn)(const unsigned char *, int, unsigned char *, int);
static const struct { const char *name; decfn fn; } DEC[] = {
    {"lib", lz0_decompress_to},
    {"min", lz0dec_min},
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(_WIN32)
    {"tiny", lz0dec_tiny_x86_64}, {"asm", lz0dec_x86_64}, {"fast", lz0dec_fast_x86_64},
#elif (defined(__aarch64__) || defined(_M_ARM64)) && !defined(_WIN32)
    {"tiny", lz0dec_tiny_arm64}, {"asm", lz0dec_arm64}, {"fast", lz0dec_fast_arm64},
#elif (defined(__riscv) && __riscv_xlen == 64) && !defined(_WIN32)
    {"tiny", lz0dec_tiny_riscv64}, {"asm", lz0dec_riscv64}, {"fast", lz0dec_fast_riscv64},
#endif
};
#define NDEC ((int)(sizeof DEC / sizeof DEC[0]))

static size_t pg;

/* Buffer of n bytes ending exactly at a PROT_NONE guard page. */
typedef struct { unsigned char *map; size_t maplen; unsigned char *p; } gbuf;

static gbuf galloc(size_t n) {
    gbuf g;
    size_t body = (n + pg - 1) / pg * pg;
    if (body == 0) body = pg;
    g.maplen = body + pg;
    g.map = mmap(NULL, g.maplen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g.map == MAP_FAILED) { perror("mmap"); exit(2); }
    if (mprotect(g.map + body, pg, PROT_NONE)) { perror("mprotect"); exit(2); }
    g.p = g.map + body - n;
    return g;
}
static void gfree(gbuf g) { munmap(g.map, g.maplen); }

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static int fails = 0;

/* Runs every decoder on (in, in_n) with an exact out_max guard buffer and
   compares return value and (on success) output bytes with lz0dec_ref. */
static void check(const char *what, const unsigned char *src, int in_n, int out_max) {
    gbuf in = galloc((size_t)in_n);
    memcpy(in.p, src, (size_t)in_n);
    gbuf ref = galloc((size_t)out_max);
    int rr = lz0dec_ref(in.p, in_n, ref.p, out_max);
    for (int d = 0; d < NDEC; d++) {
        gbuf out = galloc((size_t)out_max);
        int r = DEC[d].fn(in.p, in_n, out.p, out_max);
        if (r != rr || (rr > 0 && memcmp(out.p, ref.p, (size_t)rr))) {
            printf("FAIL %-12s %-5s in=%d out_max=%d ret=%d ref=%d\n", what, DEC[d].name, in_n, out_max, r, rr);
            fails++;
        }
        gfree(out);
    }
    gfree(ref);
    gfree(in);
}

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    unsigned char *b = malloc(sz > 0 ? (size_t)sz : 1);
    if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}

static void run_file(const char *path) {
    size_t n = 0, cn = 0;
    unsigned char *orig = slurp(path, &n), *comp = NULL;
    if (!orig || n == 0) { free(orig); return; }
    char rp[4096];
    snprintf(rp, sizeof rp, "%s.l6.lz0raw", path);
    comp = slurp(rp, &cn);
    if (!comp) {
        lz0_config cfg; lz0_stats st;
        lz0_config_level(&cfg, 6);
        if (lz0_compress(orig, n, &comp, &cn, &cfg, &st) != 0) { printf("compress failed %s\n", path); fails++; free(orig); return; }
    }
    int before = fails;
    /* 1. exact round trip, and with spare output room */
    check("exact", comp, (int)cn, (int)n);
    gbuf o = galloc(n);
    int r = lz0dec_ref(comp, (int)cn, o.p, (int)n);
    if (r != (int)n || memcmp(o.p, orig, n)) { printf("FAIL ref round trip %s\n", path); fails++; }
    gfree(o);
    for (int extra = 1; extra <= 64; extra += 9) check("spare", comp, (int)cn, (int)n + extra);
    /* 2. output too small */
    for (int cut = 1; cut <= 40 && (size_t)cut < n; cut += 3) check("short_out", comp, (int)cn, (int)n - cut);
    check("short_out", comp, (int)cn, (int)(n / 2 + 1));
    /* 3. truncated input: every cut near the end, then sampled cuts */
    for (int cut = 1; cut <= 40 && (size_t)cut < cn; cut++) check("trunc", comp, (int)cn - cut, (int)n);
    for (int k = 0; k < 24; k++) check("trunc", comp, (int)(1 + rnd() % (cn - 1 ? cn - 1 : 1)), (int)n);
    /* 4. random bit flips (differential vs ref, including success cases) */
    unsigned char *mut = malloc(cn);
    for (int k = 0; k < 48; k++) {
        memcpy(mut, comp, cn);
        int flips = 1 + (int)(rnd() % 3);
        for (int f = 0; f < flips; f++) {
            size_t pos = (k & 1) ? cn - 1 - rnd() % (cn < 64 ? cn : 64) : rnd() % cn;
            mut[pos] ^= (unsigned char)(1u << (rnd() % 8));
        }
        check("flip", mut, (int)cn, (int)n);
    }
    free(mut);
    printf("%-40s n=%-8zu raw=%-8zu %s\n", path, n, cn, fails == before ? "ok" : "FAIL");
    free(comp); free(orig);
}

int main(int argc, char **argv) {
    pg = (size_t)sysconf(_SC_PAGESIZE);
    if (argc > 1) {
        for (int i = 1; i < argc; i++) run_file(argv[i]);
    } else {
        static const char *def[] = {
            "tests/vectors/literals.bin", "tests/vectors/matchlast.bin", "tests/vectors/matchnew.bin",
            "tests/vectors/mixed.bin", "bench/corpus/fields.c", "bench/corpus/grammar.lsp",
            "bench/corpus/xargs.1", "bench/corpus/sum", "bench/corpus/cp.html", NULL };
        for (int i = 0; def[i]; i++) run_file(def[i]);
    }
    /* small synthetic streams: tiny outputs, all out_max values near the end */
    for (int len = 1; len <= 300; len += (len < 40 ? 1 : 37)) {
        unsigned char *buf = malloc((size_t)len);
        for (int i = 0; i < len; i++) buf[i] = (unsigned char)((i % 7 == 0) ? rnd() : (i % 3 ? 'a' : 'b'));
        unsigned char *c = NULL; size_t cs = 0;
        lz0_config cfg; lz0_stats st; lz0_config_level(&cfg, 6);
        if (lz0_compress(buf, (size_t)len, &c, &cs, &cfg, &st) == 0) {
            for (int om = 1; om <= len + 20; om++) check("synthetic", c, (int)cs, om);
            for (int cut = 1; (size_t)cut < cs; cut++) check("synth_trunc", c, (int)cs - cut, len);
            free(c);
        }
        free(buf);
    }
    if (fails) { printf("test_guard: %d failure(s)\n", fails); return 1; }
    printf("test_guard: all decoders ok (guard pages, truncation, bit flips)\n");
    return 0;
}
