#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

static double cpu_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static uint32_t rnd_state = 0xBEEF1234u;
static uint32_t rnd(void) {
    rnd_state = rnd_state * 1664525u + 1013904223u;
    return rnd_state & 0x7fffffffu;
}

static void count_plain(const uint32_t *src, const int *key, int n, int *cnt) {
    for (int i = 0; i < n; i++)
        cnt[key[src[i]]]++;
}

static void count_pf_key(const uint32_t *src, const int *key, int n, int *cnt, int look) {
    for (int i = 0; i < n; i++) {
        if (i + look < n)
            __builtin_prefetch(&key[src[i + look]], 0, 1);
        cnt[key[src[i]]]++;
    }
}

static void count_pf_both(const uint32_t *src, const int *key, int n, int *cnt, int look) {
    for (int i = 0; i < n; i++) {
        if (i + look < n) {
            uint32_t s = src[i + look];
            __builtin_prefetch(&key[s], 0, 1);
            __builtin_prefetch(&cnt[key[s]], 1, 1);
        }
        cnt[key[src[i]]]++;
    }
}

static void count_pf_src_key(const uint32_t *src, const int *key, int n, int *cnt, int look) {
    for (int i = 0; i < n; i++) {
        if (i + look < n) {
            __builtin_prefetch(&src[i + look], 0, 3);
            __builtin_prefetch(&key[src[i + look]], 0, 1);
        }
        cnt[key[src[i]]]++;
    }
}

static void count_unroll4(const uint32_t *src, const int *key, int n, int *cnt) {
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        int k0 = key[src[i]];
        int k1 = key[src[i + 1]];
        int k2 = key[src[i + 2]];
        int k3 = key[src[i + 3]];
        cnt[k0]++;
        cnt[k1]++;
        cnt[k2]++;
        cnt[k3]++;
    }
    for (; i < n; i++)
        cnt[key[src[i]]]++;
}

static void count_seq_key(const uint32_t *src, const int *key, int n, int *cnt) {
    (void)src;
    for (int i = 0; i < n; i++)
        cnt[key[i]]++;
}

static void count_seq_sa(const uint32_t *src, const int *key, int n, int *cnt) {
    (void)key;
    for (int i = 0; i < n; i++)
        cnt[src[i] % (uint32_t)n]++;
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : (1 << 20);
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    uint32_t *src = malloc((size_t)n * sizeof(uint32_t));
    int *key = malloc((size_t)n * sizeof(int));
    int *cnt = malloc((size_t)(n + 1) * sizeof(int));
    uint32_t *perm = malloc((size_t)n * sizeof(uint32_t));
    if (!src || !key || !cnt || !perm)
        return 1;
    for (int i = 0; i < n; i++)
        perm[i] = (uint32_t)i;
    for (int i = n - 1; i > 0; i--) {
        int j = (int)(rnd() % (uint32_t)(i + 1));
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    for (int i = 0; i < n; i++) {
        src[i] = perm[i];
        key[perm[i]] = i;
    }

    double best[7];
    for (int v = 0; v < 7; v++)
        best[v] = 1e30;
    for (int r = 0; r < reps; r++) {
        double t;
        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_plain(src, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[0])
            best[0] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_pf_key(src, key, n, cnt, 16);
        t = cpu_ms() - t;
        if (t < best[1])
            best[1] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_pf_both(src, key, n, cnt, 16);
        t = cpu_ms() - t;
        if (t < best[2])
            best[2] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_pf_src_key(src, key, n, cnt, 16);
        t = cpu_ms() - t;
        if (t < best[3])
            best[3] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_unroll4(src, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[4])
            best[4] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_seq_key(src, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[5])
            best[5] = t;

        memset(cnt, 0, (size_t)(n + 1) * sizeof(int));
        t = cpu_ms();
        count_seq_sa(src, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[6])
            best[6] = t;
    }
    printf("n=%d reps=%d\n", n, reps);
    printf("  plain     %.1fms\n", best[0]);
    printf("  pf_key    %.1fms (%.2fx)\n", best[1], best[0] / best[1]);
    printf("  pf_both   %.1fms (%.2fx)\n", best[2], best[0] / best[2]);
    printf("  pf_src+k  %.1fms (%.2fx)\n", best[3], best[0] / best[3]);
    printf("  unroll4   %.1fms (%.2fx)\n", best[4], best[0] / best[4]);
    printf("  seq_key   %.1fms (floor, sequential key)\n", best[5]);
    printf("  seq_sa    %.1fms (floor, sequential sa into cnt)\n", best[6]);
    free(src);
    free(key);
    free(cnt);
    free(perm);
    return 0;
}
