#ifndef LZ0DEC_H
#define LZ0DEC_H

#ifdef __cplusplus
extern "C" {
#endif

int lz0dec_ref(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

int lz0dec_min(const unsigned char *in, int in_size,
               unsigned char *out, int out_max);

#if defined(__x86_64__) || defined(_M_X64)
int lz0dec_x86_64(const unsigned char *in, int in_size,
                  unsigned char *out, int out_max);
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
int lz0dec_arm64(const unsigned char *in, int in_size,
                 unsigned char *out, int out_max);
#endif

#ifdef __cplusplus
}
#endif

#endif
