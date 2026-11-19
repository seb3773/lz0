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

static void count_prefix(const int *key, const uint32_t *src, int n, int maxk, int *cnt) {
    memset(cnt, 0, (size_t)(maxk + 1) * sizeof(int));
    for (int i = 0; i < n; i++)
        cnt[key[src[i]]]++;
    for (int i = 1; i <= maxk; i++)
        cnt[i] += cnt[i - 1];
}

static void scatter_plain(const uint32_t *src, uint32_t *out, const int *key, int n, int *cnt) {
    for (int i = n - 1; i >= 0; i--) {
        int k = key[src[i]];
        out[--cnt[k]] = src[i];
    }
}

static void scatter_pf1(const uint32_t *src, uint32_t *out, const int *key, int n, int *cnt, int look) {
    for (int i = n - 1; i >= 0; i--) {
        if (i - look >= 0)
            __builtin_prefetch(&out[cnt[key[src[i - look]]] - 1], 1, 1);
        int k = key[src[i]];
        out[--cnt[k]] = src[i];
    }
}

static void scatter_pf2(const uint32_t *src, uint32_t *out, const int *key, int n, int *cnt, int look) {
    for (int i = n - 1; i >= 0; i--) {
        if (i - look >= 0)
            __builtin_prefetch(&key[src[i - look]], 0, 1);
        if (i - 2 * look >= 0) {
            int kp = key[src[i - 2 * look]];
            __builtin_prefetch(&out[cnt[kp] - 1], 1, 1);
        }
        int k = key[src[i]];
        out[--cnt[k]] = src[i];
    }
}

static void scatter_fwd(const uint32_t *src, uint32_t *out, const int *key, int n, int *cnt) {
    int *pos = cnt;
    for (int i = 0; i < n; i++) {
        int k = key[src[i]];
        out[pos[k]++] = src[i];
    }
}

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : (1 << 20);
    int maxk = n;
    int reps = argc > 2 ? atoi(argv[2]) : 20;
    uint32_t *src = malloc((size_t)n * sizeof(uint32_t));
    uint32_t *out = malloc((size_t)n * sizeof(uint32_t));
    int *key = malloc((size_t)n * sizeof(int));
    int *cnt = malloc((size_t)(maxk + 1) * sizeof(int));
    int *snap = malloc((size_t)(maxk + 1) * sizeof(int));
    uint32_t *perm = malloc((size_t)n * sizeof(uint32_t));
    if (!src || !out || !key || !cnt || !snap || !perm)
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

    count_prefix(key, src, n, maxk, snap);

    double best[4];
    for (int v = 0; v < 4; v++)
        best[v] = 1e30;
    for (int r = 0; r < reps; r++) {
        double t;

        memcpy(cnt, snap, (size_t)(maxk + 1) * sizeof(int));
        t = cpu_ms();
        scatter_plain(src, out, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[0])
            best[0] = t;

        memcpy(cnt, snap, (size_t)(maxk + 1) * sizeof(int));
        t = cpu_ms();
        scatter_pf1(src, out, key, n, cnt, 16);
        t = cpu_ms() - t;
        if (t < best[1])
            best[1] = t;

        memcpy(cnt, snap, (size_t)(maxk + 1) * sizeof(int));
        t = cpu_ms();
        scatter_pf2(src, out, key, n, cnt, 16);
        t = cpu_ms() - t;
        if (t < best[2])
            best[2] = t;

        memcpy(cnt, snap, (size_t)(maxk + 1) * sizeof(int));
        t = cpu_ms();
        scatter_fwd(src, out, key, n, cnt);
        t = cpu_ms() - t;
        if (t < best[3])
            best[3] = t;
    }
    printf("n=%d reps=%d  plain=%.1fms  pf1=%.1fms(%.2fx)  pf2=%.1fms(%.2fx)  fwd=%.1fms(%.2fx)\n",
           n, reps, best[0], best[1], best[0] / best[1], best[2], best[0] / best[2],
           best[3], best[0] / best[3]);
    free(src);
    free(out);
    free(key);
    free(cnt);
    free(snap);
    free(perm);
    return 0;
}
