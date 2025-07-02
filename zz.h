#ifndef ZZ_H
#define ZZ_H

#include <gmp.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool negative;
    mp_size_t alloc;
    mp_size_t size;
    mp_limb_t *digits;
} zz_t;

typedef enum {
    MP_OK = 0,
    MP_MEM = -1,
    MP_VAL = -2,
    MP_BUF = -3,
} mp_err;

mp_err zz_init(zz_t *u);
mp_err zz_resize(mp_size_t size, zz_t *u);
void zz_clear(zz_t *u);
void zz_normalize(zz_t *u);

typedef enum {
    MP_GT = +1,
    MP_EQ = 0,
    MP_LT = -1,
} mp_ord;

mp_ord zz_cmp(const zz_t *u, const zz_t *v);

mp_err zz_from_i64(int64_t u, zz_t *v);
mp_err zz_to_i64(const zz_t *u, int64_t *v);

mp_err zz_copy(const zz_t *u, zz_t *v);
mp_err zz_abs(const zz_t *u, zz_t *v);
mp_err zz_neg(const zz_t *u, zz_t *v);

extern int OPT_TAG, OPT_PREFIX;

mp_err zz_to_str(const zz_t *u, int base, int options, char **buf);
mp_err zz_from_str(const char *str, size_t len, int base, zz_t *u);

mp_err zz_to_double(const zz_t *u, double *d);

mp_err zz_to_bytes(const zz_t *u, size_t length, int is_little,
                   int is_signed, unsigned char **buffer);
mp_err zz_from_bytes(const unsigned char *buffer, size_t length,
                     int is_little, int is_signed, zz_t *u);

mp_err zz_add(const zz_t *u, const zz_t *v, zz_t *w);
mp_err zz_sub(const zz_t *u, const zz_t *v, zz_t *w);

mp_err zz_mul(const zz_t *u, const zz_t *v, zz_t *w);

typedef enum {
    MP_RNDD = 0,
    MP_RNDN = 1,
} mp_rnd;

mp_err zz_div(const zz_t *u, const zz_t *v, mp_rnd rnd, zz_t *q, zz_t *r);

mp_err zz_rshift1(const zz_t *u, mp_limb_t rshift, zz_t *v);
mp_err zz_lshift1(const zz_t *u, mp_limb_t lshift, zz_t *v);

mp_err zz_truediv(const zz_t *u, const zz_t *v, double *res);

mp_err zz_invert(const zz_t *u, zz_t *v);
mp_err zz_and(const zz_t *u, const zz_t *v, zz_t *w);
mp_err zz_or(const zz_t *u, const zz_t *v, zz_t *w);
mp_err zz_xor(const zz_t *u, const zz_t *v, zz_t *w);

mp_err zz_pow(const zz_t *u, const zz_t *v, zz_t *w);

mp_err zz_gcd(const zz_t *u, const zz_t *v, zz_t *w);
mp_err zz_gcdext(const zz_t *u, const zz_t *v, zz_t *g, zz_t *s, zz_t *t);
mp_err zz_inverse(const zz_t *u, const zz_t *v, zz_t *w);

mp_err zz_powm(const zz_t *u, const zz_t *v, const zz_t *w, zz_t *res);

mp_err zz_sqrtrem(const zz_t *u, zz_t *v, zz_t *w);

mp_err zz_fac(int64_t u, zz_t *v);
mp_err zz_fac2(int64_t u, zz_t *v);
mp_err zz_fib(int64_t u, zz_t *v);

#endif /* ZZ_H */
