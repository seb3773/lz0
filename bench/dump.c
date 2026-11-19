#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lz0.h"
#include "lz0_internal.h"

int main(void) {
    const char *s = "the quick brown fox jumps over the lazy dog. the quick brown fox.";
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_LAZY);
    lz0_finder f;
    lz0_finder_init(&f, (const unsigned char *)s, (uint32_t)strlen(s), &cfg);
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    lz0_parse_lazy(&f, &tl);
    uint32_t last = LZ0_INITIAL_OFFSET;
    uint32_t covered = 0;
    for (uint32_t i = 0; i < tl.n; i++) {
        lz0_token *t = &tl.t[i];
        const char *k = !t->offset ? "LIT" : (t->offset == last ? "LAST" : "NEW");
        printf("%2u pos=%u len=%u off=%u %s\n", i, t->pos, t->length, t->offset, k);
        if (t->pos != covered)
            printf("  GAP covered=%u\n", covered);
        covered = t->pos + t->length;
        if (t->offset && t->offset != last)
            last = t->offset;
    }
    printf("n=%zu covered=%u tokens=%u\n", strlen(s), covered, tl.n);
    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
    return 0;
}
