#include "lz0.h"
#include "lz0_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static void try_one(const char *name, const unsigned char *in, size_t n) {
    lz0_config cfg;
    lz0_config_default(&cfg, LZ0_PARSE_LITE1);
    lz0_finder f;
    if (lz0_finder_init(&f, in, (uint32_t)n, &cfg) < 0) {
        printf("%s finder fail\n", name);
        return;
    }
    lz0_toklist tl;
    lz0_toklist_init(&tl);
    int rc = lz0_parse_lite1(&f, &tl);
    int valid = rc == 0 ? lz0_tokens_valid(&tl, (uint32_t)n) : 0;
    printf("%s n=%zu parse=%d tokens=%u valid=%d\n", name, n, rc, tl.n, valid);
    uint32_t last = 1;
    uint32_t cover = 0;
    int after_lit = 0;
    int can_last = 0;
    for (uint32_t i = 0; i < tl.n && i < 12; i++) {
        const lz0_token *t = &tl.t[i];
        const char *k = !t->offset ? "LIT" : (t->offset == last ? "LAST" : "NEW");
        printf("  [%u] pos=%u len=%u off=%u %s%s%s\n", i, t->pos, t->length, t->offset, k,
               t->pos != cover ? " POS" : "",
               (!t->offset && after_lit) ? " DBLIT" : "");
        if (!t->offset) {
            after_lit = 1;
            can_last = 1;
        } else {
            after_lit = 0;
            last = t->offset;
            can_last = 0;
        }
        cover += t->length;
        (void)can_last;
    }
    if (tl.n > 12)
        printf("  ... %u tokens cover=%u\n", tl.n, cover);
    lz0_toklist_free(&tl);
    lz0_finder_free(&f);
}

int main(void) {
    const char *s = "the quick brown fox jumps over the lazy dog. the quick brown fox.";
    try_one("fox", (const unsigned char *)s, strlen(s));
    unsigned char mixed[256];
    for (int i = 0; i < 256; i++)
        mixed[i] = (unsigned char)((i * 7) ^ (i >> 3));
    memcpy(mixed + 80, mixed, 40);
    try_one("mix256", mixed, sizeof(mixed));
    return 0;
}
