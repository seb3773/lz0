#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    size_t n = 32768;
    unsigned char *p = (unsigned char *)malloc(n);
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)((i / 17) & 0xff);
    for (size_t i = n / 4; i + n / 8 < n && i < n; i += n / 7) {
        size_t L = n / 8 < n - i ? n / 8 : n - i;
        memcpy(p + i, p, L);
    }

    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_GREEDY);
    lz0_finder f;
    lz0_finder_init(&f, p, (uint32_t)n, &cfg);

    uint32_t pos = (uint32_t)(n / 4);
    lz0_finder_insert_upto(&f, pos);
    lz0_match cands[64];
    int last_len = 0;
    int nc = lz0_find_matches(&f, pos, 1, cands, &last_len);
    printf("pos=%u last_len=%d nc=%d byte=%u prev=%u\n", pos, last_len, nc, p[pos], p[pos - 1]);
    for (int k = 0; k < nc; k++)
        printf("  cand off=%u len=%u cost_new=%d\n", cands[k].offset, cands[k].length,
               lz0_cost_new((int)cands[k].offset, (int)cands[k].length));

    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_parse_greedy(&f, &tl);
    uint32_t last = 1;
    int shown = 0;
    for (uint32_t i = 0; i < tl.n; i++) {
        uint32_t a = tl.t[i].pos;
        uint32_t b = a + tl.t[i].length;
        if (b < pos - 40 || a > pos + 40)
            continue;
        const char *k = !tl.t[i].offset ? "LIT" : (tl.t[i].offset == last ? "LAST" : "NEW");
        printf("tok %u pos=%u len=%u off=%u %s\n", i, a, tl.t[i].length, tl.t[i].offset, k);
        if (tl.t[i].offset)
            last = tl.t[i].offset;
        if (++shown > 20)
            break;
    }
    printf("tokens=%u\n", tl.n);
    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
    free(p);
    return 0;
}
