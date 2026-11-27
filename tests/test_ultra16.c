#define _POSIX_C_SOURCE 200809L
#include "lz0.h"
#include "lz0_internal.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Real-mode 8086 execution test for the assembled flat binary
 * bin/lz0dec_ultra_tiny_x86_16.bin (executed byte-for-byte directly from
 * the assembled machine code in 1 MiB segmented memory).
 */
typedef struct {
    uint8_t mem[1u << 20];
    uint16_t r[8]; /* AX, CX, DX, BX, SP, BP, SI, DI */
    uint16_t cs, ds, es, ss, ip;
    int cf, zf, sf, pf, df;
} x86_16_cpu;

enum { R_AX = 0, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI };

static inline uint32_t lin(uint16_t seg, uint16_t off) {
    return (((uint32_t)seg << 4) + (uint32_t)off) & 0xFFFFFu;
}

static inline uint8_t fetch8(x86_16_cpu *c) {
    uint8_t v = c->mem[lin(c->cs, c->ip)];
    c->ip = (uint16_t)(c->ip + 1u);
    return v;
}

static inline uint16_t fetch16(x86_16_cpu *c) {
    uint16_t lo = fetch8(c);
    uint16_t hi = fetch8(c);
    return (uint16_t)(lo | (hi << 8));
}

static inline uint8_t r8_get(const x86_16_cpu *c, int idx) {
    int reg = idx & 3;
    return (idx & 4) ? (uint8_t)(c->r[reg] >> 8) : (uint8_t)(c->r[reg] & 0xFFu);
}

static inline void r8_set(x86_16_cpu *c, int idx, uint8_t v) {
    int reg = idx & 3;
    if (idx & 4) c->r[reg] = (uint16_t)((c->r[reg] & 0x00FFu) | ((uint16_t)v << 8));
    else c->r[reg] = (uint16_t)((c->r[reg] & 0xFF00u) | v);
}

static inline void push16(x86_16_cpu *c, uint16_t v) {
    c->r[R_SP] = (uint16_t)(c->r[R_SP] - 2u);
    uint32_t a = lin(c->ss, c->r[R_SP]);
    c->mem[a] = (uint8_t)(v & 0xFFu);
    c->mem[(a + 1u) & 0xFFFFFu] = (uint8_t)(v >> 8);
}

static inline uint16_t pop16(x86_16_cpu *c) {
    uint32_t a = lin(c->ss, c->r[R_SP]);
    uint16_t v = (uint16_t)(c->mem[a] | ((uint16_t)c->mem[(a + 1u) & 0xFFFFFu] << 8));
    c->r[R_SP] = (uint16_t)(c->r[R_SP] + 2u);
    return v;
}

static inline void set_flags8(x86_16_cpu *c, uint16_t res) {
    uint8_t b = (uint8_t)res;
    c->cf = (res >> 8) & 1;
    c->zf = (b == 0);
    c->sf = (b >> 7) & 1;
    c->pf = (__builtin_parity((unsigned)b) == 0);
}

static inline void set_flags16(x86_16_cpu *c, uint32_t res) {
    uint16_t w = (uint16_t)res;
    c->cf = (res >> 16) & 1;
    c->zf = (w == 0);
    c->sf = (w >> 15) & 1;
    c->pf = (__builtin_parity((unsigned)(w & 0xFFu)) == 0);
}

static uint32_t modrm_ea(const x86_16_cpu *c, uint8_t modrm) {
    uint8_t mod = modrm >> 6;
    uint8_t rm = modrm & 7;
    if (mod == 0 && rm == 4) return lin(c->ds, c->r[R_SI]);
    if (mod == 0 && rm == 5) return lin(c->ds, c->r[R_DI]);
    if (mod == 0 && rm == 7) return lin(c->ds, c->r[R_BX]);
    fprintf(stderr, "test_ultra16: unsupported ModRM 0x%02x\n", modrm);
    exit(2);
}

static int run_16(const uint8_t *code, size_t code_sz,
                  uint16_t ds_seg, uint16_t si_off,
                  uint16_t es_seg, uint16_t di_off,
                  const uint8_t *in, size_t in_sz,
                  uint8_t *out, size_t max_out) {
    x86_16_cpu cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.cs = 0x07C0;
    cpu.ds = ds_seg;
    cpu.es = es_seg;
    cpu.ss = 0x3000;
    cpu.r[R_SP] = 0xFF00;
    cpu.r[R_BP] = 0xBEEF;
    cpu.r[R_SI] = si_off;
    cpu.r[R_DI] = di_off;
    cpu.df = 1; /* verify routine executes CLD explicitly */

    memcpy(cpu.mem + lin(cpu.cs, 0), code, code_sz);
    memcpy(cpu.mem + lin(ds_seg, si_off), in, in_sz);
    push16(&cpu, 0xFFFFu);

    uint64_t steps = 0;
    while (steps++ < 50000000ull) {
        if (cpu.ip == 0xFFFFu) {
            if (cpu.ds != ds_seg || cpu.es != es_seg || cpu.r[R_BP] != 0xBEEF || cpu.df != 0) {
                fprintf(stderr, "test_ultra16: ABI state clobbered (ds=%04x es=%04x bp=%04x df=%d)\n",
                        cpu.ds, cpu.es, cpu.r[R_BP], cpu.df);
                return -1;
            }
            size_t got = (size_t)cpu.r[R_AX];
            if (got <= max_out)
                memcpy(out, cpu.mem + lin(es_seg, di_off), got);
            return (int)got;
        }

        uint8_t op = fetch8(&cpu);
        if (op == 0xFC) { cpu.df = 0; }
        else if (op >= 0x50 && op <= 0x57) { push16(&cpu, cpu.r[op & 7]); }
        else if (op >= 0x58 && op <= 0x5F) { cpu.r[op & 7] = pop16(&cpu); }
        else if (op == 0x1E) { push16(&cpu, cpu.ds); }
        else if (op == 0x1F) { cpu.ds = pop16(&cpu); }
        else if (op == 0x06) { push16(&cpu, cpu.es); }
        else if (op >= 0xB0 && op <= 0xB7) { r8_set(&cpu, op & 7, fetch8(&cpu)); }
        else if (op >= 0xB8 && op <= 0xBF) { cpu.r[op & 7] = fetch16(&cpu); }
        else if (op == 0xE8) { int16_t rel = (int16_t)fetch16(&cpu); push16(&cpu, cpu.ip); cpu.ip = (uint16_t)(cpu.ip + rel); }
        else if (op == 0xC3) { cpu.ip = pop16(&cpu); }
        else if (op == 0xEB) { int8_t rel = (int8_t)fetch8(&cpu); cpu.ip = (uint16_t)(cpu.ip + rel); }
        else if (op == 0x72) { int8_t rel = (int8_t)fetch8(&cpu); if (cpu.cf) cpu.ip = (uint16_t)(cpu.ip + rel); }
        else if (op == 0x73) { int8_t rel = (int8_t)fetch8(&cpu); if (!cpu.cf) cpu.ip = (uint16_t)(cpu.ip + rel); }
        else if (op == 0x75) { int8_t rel = (int8_t)fetch8(&cpu); if (!cpu.zf) cpu.ip = (uint16_t)(cpu.ip + rel); }
        else if (op >= 0x91 && op <= 0x97) { uint16_t t = cpu.r[R_AX]; cpu.r[R_AX] = cpu.r[op & 7]; cpu.r[op & 7] = t; }
        else if (op == 0xAC) { r8_set(&cpu, R_AX, cpu.mem[lin(cpu.ds, cpu.r[R_SI])]); cpu.r[R_SI] = (uint16_t)(cpu.r[R_SI] + (cpu.df ? -1 : 1)); }
        else if (op == 0x46) { uint16_t v = (uint16_t)(cpu.r[R_SI] + 1u); cpu.r[R_SI] = v; cpu.zf = (v == 0); cpu.sf = (v >> 15); }
        else if (op == 0xF3) {
            uint8_t op2 = fetch8(&cpu);
            if (op2 != 0xA4) { fprintf(stderr, "test_ultra16: bad REP op 0x%02x\n", op2); return -1; }
            int d = cpu.df ? -1 : 1;
            while (cpu.r[R_CX] != 0) {
                cpu.mem[lin(cpu.es, cpu.r[R_DI])] = cpu.mem[lin(cpu.ds, cpu.r[R_SI])];
                cpu.r[R_SI] = (uint16_t)(cpu.r[R_SI] + d);
                cpu.r[R_DI] = (uint16_t)(cpu.r[R_DI] + d);
                cpu.r[R_CX] = (uint16_t)(cpu.r[R_CX] - 1u);
            }
        } else if (op == 0x04) { uint16_t res = (uint16_t)r8_get(&cpu, R_AX) + fetch8(&cpu); set_flags8(&cpu, res); r8_set(&cpu, R_AX, (uint8_t)res); }
        else if (op == 0x3C) { uint16_t res = (uint16_t)r8_get(&cpu, R_AX) - fetch8(&cpu); set_flags8(&cpu, res); }
        else if (op == 0x83) {
            uint8_t m = fetch8(&cpu); uint8_t regop = (m >> 3) & 7;
            uint16_t imm = (uint16_t)(int16_t)(int8_t)fetch8(&cpu);
            if ((m >> 6) != 3 || regop != 0) { fprintf(stderr, "test_ultra16: bad 0x83 ModRM 0x%02x\n", m); return -1; }
            uint32_t res = (uint32_t)cpu.r[m & 7] + imm; set_flags16(&cpu, res); cpu.r[m & 7] = (uint16_t)res;
        } else if (op == 0x88 || op == 0x89 || op == 0x00 || op == 0x10 || op == 0x11 || op == 0x29 || op == 0x30 || op == 0x31) {
            uint8_t m = fetch8(&cpu); int reg = (m >> 3) & 7, rm = m & 7;
            if ((m >> 6) != 3) { fprintf(stderr, "test_ultra16: unexpected mem ModRM 0x%02x for op 0x%02x\n", m, op); return -1; }
            if (op == 0x88) r8_set(&cpu, rm, r8_get(&cpu, reg));
            else if (op == 0x89) cpu.r[rm] = cpu.r[reg];
            else if (op == 0x00) { uint16_t r = (uint16_t)r8_get(&cpu, rm) + r8_get(&cpu, reg); set_flags8(&cpu, r); r8_set(&cpu, rm, (uint8_t)r); }
            else if (op == 0x10) { uint16_t r = (uint16_t)r8_get(&cpu, rm) + r8_get(&cpu, reg) + cpu.cf; set_flags8(&cpu, r); r8_set(&cpu, rm, (uint8_t)r); }
            else if (op == 0x11) { uint32_t r = (uint32_t)cpu.r[rm] + cpu.r[reg] + cpu.cf; set_flags16(&cpu, r); cpu.r[rm] = (uint16_t)r; }
            else if (op == 0x29) { uint32_t r = (uint32_t)cpu.r[rm] - cpu.r[reg]; set_flags16(&cpu, r); cpu.r[rm] = (uint16_t)r; }
            else if (op == 0x30) { uint8_t v = (uint8_t)(r8_get(&cpu, rm) ^ r8_get(&cpu, reg)); set_flags8(&cpu, v); cpu.cf = 0; r8_set(&cpu, rm, v); }
            else if (op == 0x31) { uint16_t v = (uint16_t)(cpu.r[rm] ^ cpu.r[reg]); set_flags16(&cpu, v); cpu.cf = 0; cpu.r[rm] = v; }
        } else if (op == 0x8A) {
            uint8_t m = fetch8(&cpu); int reg = (m >> 3) & 7;
            uint8_t v = ((m >> 6) == 3) ? r8_get(&cpu, m & 7) : cpu.mem[modrm_ea(&cpu, m)];
            r8_set(&cpu, reg, v);
        } else {
            fprintf(stderr, "test_ultra16: unknown opcode 0x%02x at ip=%04x\n", op, cpu.ip - 1);
            return -1;
        }
    }
    fprintf(stderr, "test_ultra16: step limit exceeded\n");
    return -1;
}

static uint8_t *read_file_all(const char *path, size_t *n_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    uint8_t *b = (uint8_t *)malloc(sz > 0 ? (size_t)sz : 1);
    if (!b || (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz)) { fclose(f); free(b); return NULL; }
    fclose(f);
    *n_out = (size_t)sz;
    return b;
}

static int verify_case(const uint8_t *code, size_t csz, const uint8_t *orig, size_t n, const uint8_t *comp, size_t rsz) {
    uint8_t *out = (uint8_t *)malloc(n);
    int r1 = run_16(code, csz, 0x1000, 0x0123, 0x2000, 0x0400, comp, rsz, out, n);
    if (r1 != (int)n || memcmp(out, orig, n) != 0) { free(out); return 1; }
    int r2 = run_16(code, csz, 0x2000, 0x8000, 0x2000, 0x0000, comp, rsz, out, n);
    if (r2 != (int)n || memcmp(out, orig, n) != 0) { free(out); return 1; }
    free(out);
    return 0;
}

int main(void) {
    size_t code_sz = 0;
    uint8_t *code = read_file_all("bin/lz0dec_ultra_tiny_x86_16.bin", &code_sz);
    if (!code || code_sz == 0 || code_sz > 128) {
        fprintf(stderr, "FAIL: bin/lz0dec_ultra_tiny_x86_16.bin invalid size=%zu\n", code_sz);
        free(code);
        return 1;
    }

    static const char *vecs[] = {
        "onebyte", "literals", "overlap", "mixed", "matchlast", "matchnew", "eof_after_lit"
    };
    for (size_t i = 0; i < sizeof(vecs) / sizeof(vecs[0]); i++) {
        char p1[256], p2[256];
        snprintf(p1, sizeof(p1), "tests/vectors/%s.bin", vecs[i]);
        snprintf(p2, sizeof(p2), "tests/vectors/%s.lz0raw", vecs[i]);
        size_t n = 0, rn = 0;
        uint8_t *in = read_file_all(p1, &n);
        uint8_t *raw = read_file_all(p2, &rn);
        if (!in || !raw || verify_case(code, code_sz, in, n, raw, rn)) {
            printf("FAIL vec %s\n", vecs[i]); return 1;
        }
        free(raw); free(in);
    }

    const uint32_t offs[] = {1, 127, 128, 129, 255, 256, 257, 32639, 32640, 32641};
    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        uint32_t off = offs[i], ml = 32, n = off + ml;
        uint8_t *in = (uint8_t *)malloc(n);
        for (uint32_t k = 0; k < off; k++) in[k] = (uint8_t)(k * 131u + 17u);
        for (uint32_t k = 0; k < ml; k++) in[off + k] = in[k];
        lz0_toklist tl; lz0_toklist_init(&tl);
        lz0_toklist_push(&tl, 0, off, 0);
        lz0_toklist_push(&tl, off, ml, off);
        uint8_t *comp = NULL; size_t cs = 0; lz0_bitacct acct;
        lz0_emit(in, n, &tl, &comp, &cs, &acct);
        if (verify_case(code, code_sz, in, n, comp, cs)) {
            printf("FAIL off=%u\n", off); return 1;
        }
        free(comp); lz0_toklist_free(&tl); free(in);
    }

    lz0_config cfg; lz0_config_level(&cfg, 6);
    static const char *corp[] = {"bench/corpus/fields.c", "bench/corpus/cp.html"};
    for (size_t i = 0; i < 2; i++) {
        size_t n = 0, cn = 0;
        uint8_t *in = read_file_all(corp[i], &n);
        uint8_t *comp = NULL; lz0_stats st;
        lz0_compress(in, n, &comp, &cn, &cfg, &st);
        if (verify_case(code, code_sz, in, n, comp, cn)) {
            printf("FAIL corpus %s\n", corp[i]); return 1;
        }
        free(comp); free(in);
    }

    uint32_t rng = 0x16B00716u;
    for (int i = 0; i < 200; i++) {
        rng = rng * 1664525u + 1013904223u;
        size_t n = 1 + (rng % 512u);
        uint8_t *in = (uint8_t *)malloc(n);
        for (size_t k = 0; k < n; k++) {
            rng = rng * 1664525u + 1013904223u;
            in[k] = (k < 8 || (i & 1)) ? (uint8_t)(rng >> 24) : in[k - 1 - (rng & 7)];
        }
        uint8_t *comp = NULL; size_t cs = 0; lz0_stats st;
        lz0_compress(in, n, &comp, &cs, &cfg, &st);
        if (verify_case(code, code_sz, in, n, comp, cs)) {
            printf("FAIL fuzz i=%d\n", i); return 1;
        }
        free(comp); free(in);
    }

    printf("ultra16 (%zu B binary): vectors + boundaries + corpus + 200 fuzz OK (DS!=ES & DS==ES)\n", code_sz);
    free(code);
    return 0;
}

