#include "pythoncapi_compat.h"

#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <float.h>
#include <gmp.h>
#include <setjmp.h>

static jmp_buf gmp_env;
#define GMP_TRACKER_SIZE_INCR 16
#define CHECK_NO_MEM_LEAK (setjmp(gmp_env) != 1)
static struct {
    size_t size;
    size_t alloc;
    void **ptrs;
} gmp_tracker;

static void *
gmp_allocate_function(size_t size)
{
    if (gmp_tracker.size >= gmp_tracker.alloc) {
        void **tmp = gmp_tracker.ptrs;

        gmp_tracker.alloc += GMP_TRACKER_SIZE_INCR;
        gmp_tracker.ptrs = realloc(tmp, gmp_tracker.alloc * sizeof(void *));
        if (!gmp_tracker.ptrs) {
            gmp_tracker.alloc -= GMP_TRACKER_SIZE_INCR;
/* Workaround https://gcc.gnu.org/bugzilla/show_bug.cgi?id=110501 */
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 13
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wuse-after-free"
#endif
            gmp_tracker.ptrs = tmp;
#if defined(__GNUC__) && !defined(__clang__)
#  pragma GCC diagnostic pop
#endif
            goto err;
        }
    }

    void *ret = malloc(size);

    if (!ret) {
        goto err;
    }
    gmp_tracker.ptrs[gmp_tracker.size] = ret;
    gmp_tracker.size++;
    return ret;
err:
    for (size_t i = 0; i < gmp_tracker.size; i++) {
        if (gmp_tracker.ptrs[i]) {
            free(gmp_tracker.ptrs[i]);
            gmp_tracker.ptrs[i] = NULL;
        }
    }
    gmp_tracker.alloc = 0;
    gmp_tracker.size = 0;
    longjmp(gmp_env, 1);
}

static void *
gmp_reallocate_function(void *ptr, size_t old_size, size_t new_size)
{
    void *ret = realloc(ptr, new_size);

    if (!ret) {
        goto err;
    }
    for (size_t i = gmp_tracker.size - 1; i >= 0; i--) {
        if (gmp_tracker.ptrs[i] == ptr) {
            gmp_tracker.ptrs[i] = ret;
            break;
        }
    }
    return ret;
err:
    for (size_t i = 0; i < gmp_tracker.size; i++) {
        if (gmp_tracker.ptrs[i]) {
            free(gmp_tracker.ptrs[i]);
            gmp_tracker.ptrs[i] = NULL;
        }
    }
    gmp_tracker.alloc = 0;
    gmp_tracker.size = 0;
    longjmp(gmp_env, 1);
}

static void
gmp_free_function(void *ptr, size_t size)
{
    for (size_t i = gmp_tracker.size - 1; i >= 0; i--) {
        if (gmp_tracker.ptrs[i] && gmp_tracker.ptrs[i] == ptr) {
            gmp_tracker.ptrs[i] = 0;
            if (i == gmp_tracker.size - 1) {
                gmp_tracker.size--;
            }
            break;
        }
    }
    free(ptr);
}

typedef struct _mpzobject {
    PyObject_HEAD
    uint8_t negative;
    mp_size_t size;
    /* XXX: add alloc field? */
    mp_limb_t *digits;
} MPZ_Object;

PyTypeObject MPZ_Type;

static MPZ_Object *
MPZ_new(mp_size_t size, uint8_t negative)
{
    MPZ_Object *res = PyObject_New(MPZ_Object, &MPZ_Type);

    if (!res) {
        return NULL;
    }
    res->negative = negative;
    res->size = size;
    res->digits = PyMem_New(mp_limb_t, size);
    if (!res->digits) {
        return (MPZ_Object *)PyErr_NoMemory();
    }
    return res;
}

static void
MPZ_normalize(MPZ_Object *u)
{
    while (u->size && u->digits[u->size - 1] == 0) {
        u->size--;
    }
    if (!u->size) {
        u->negative = 0;
    }
}

static MPZ_Object *
MPZ_FromDigitSign(mp_limb_t digit, uint8_t negative)
{
    MPZ_Object *res = MPZ_new(1, negative);

    if (!res) {
        return NULL;
    }
    res->digits[0] = digit;
    MPZ_normalize(res);
    return res;
}

static MPZ_Object *
MPZ_copy(MPZ_Object *u)
{
    if (!u->size) {
        return MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(u->size, u->negative);

    if (!res) {
        return NULL;
    }
    mpn_copyi(res->digits, u->digits, u->size);
    return res;
}

static MPZ_Object *
MPZ_abs(MPZ_Object *u)
{
    MPZ_Object *res = MPZ_copy(u);

    if (!res) {
        return NULL;
    }
    res->negative = 0;
    return res;
}

/* Maps 1-byte integer to digit character for bases up to 36 and
   from 37 up to 62, respectively. */
static const char *num_to_text36 = "0123456789abcdefghijklmnopqrstuvwxyz";
static const char *num_to_text62 = ("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                                    "abcdefghijklmnopqrstuvwxyz");

static const char *mpz_tag = "mpz(";
static int OPT_TAG = 0x1;
static int OPT_PREFIX = 0x2;

static PyObject *
MPZ_to_str(MPZ_Object *u, int base, int options)
{
    if (base < 2 || base > 62) {
        PyErr_SetString(PyExc_ValueError,
                        "base must be in the interval [2, 62]");
        return NULL;
    }

    size_t len = mpn_sizeinbase(u->digits, u->size, base);

    /*                                tag sign prefix        )   \0 */
    unsigned char *buf = PyMem_Malloc(4 + 1   + 2    + len + 1 + 1), *p = buf;

    if (options & OPT_TAG) {
        strcpy((char *)buf, mpz_tag);
        p += strlen(mpz_tag);
    }
    if (u->negative) {
        *(p++) = '-';
    }
    if (options & OPT_PREFIX) {
        if (base == 2) {
            *(p++) = '0';
            *(p++) = 'b';
        }
        else if (base == 8) {
            *(p++) = '0';
            *(p++) = 'o';
        }
        else if (base == 16) {
            *(p++) = '0';
            *(p++) = 'x';
        }
    }
    if (CHECK_NO_MEM_LEAK) {
        len -= (mpn_get_str(p, base, u->digits, u->size) != len);
    }
    else {
        PyMem_Free(buf);
        return PyErr_NoMemory();
    }

    const char *num_to_text = base > 36 ? num_to_text62 : num_to_text36;

    for (size_t i = 0; i < len; i++) {
        *p = num_to_text[*p];
        p++;
    }
    if (options & OPT_TAG) {
        *(p++) = ')';
    }
    *(p++) = '\0';

    PyObject *res = PyUnicode_FromString((char *)buf);

    PyMem_Free(buf);
    return res;
}

/* Table of digit values for 8-bit string->mpz conversion.
   Note that when converting a base B string, a char c is a legitimate
   base B digit iff gmp_digit_value_tab[c] < B. */
const unsigned char gmp_digit_value_tab[] =
{
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1,-1,-1,-1,-1,-1,
  -1,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,
  25,26,27,28,29,30,31,32,33,34,35,-1,-1,-1,-1,-1,
  -1,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,
  51,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
  -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
};

static MPZ_Object *
MPZ_from_str(PyObject *obj, int base)
{
    if (base != 0 && (base < 2 || base > 62)) {
        PyErr_SetString(PyExc_ValueError,
                        "base must be 0 or in the interval [2, 62]");
        return NULL;
    }

    Py_ssize_t len;
    const char *str = PyUnicode_AsUTF8AndSize(obj, &len);

    if (!str) {
        return NULL;
    }

    unsigned char *buf = PyMem_Malloc(len), *p = buf;

    if (!buf) {
        return (MPZ_Object *)PyErr_NoMemory();
    }
    memcpy(buf, str, len);

    int8_t negative = (buf[0] == '-');

    p += negative;
    len -= negative;
    if (p[0] == '0' && len > 2 && p[1] != '\0') {
        if (base == 0) {
            if (tolower(p[1]) == 'b') {
                base = 2;
            }
            else if (tolower(p[1]) == 'o') {
                base = 8;
            }
            else if (tolower(p[1]) == 'x') {
                base = 16;
            }
            else {
                goto err;
            }
        }
        if ((tolower(p[1]) == 'b' && base == 2)
            || (tolower(p[1]) == 'o' && base == 8)
            || (tolower(p[1]) == 'x' && base == 16))
        {
            p += 2;
            len -= 2;
            if (p[0] == '_') {
                p += 1;
                len--;
            }
        }
    }
    if (base == 0) {
        base = 10;
    }
    if (p[0] == '_') {
        goto err;
    }
    if (!len) {
        goto err;
    }

    const unsigned char *digit_value = gmp_digit_value_tab;

    if (base > 36) {
        digit_value += 208;
    }

    Py_ssize_t new_len = len;

    for (Py_ssize_t i = 0; i < len; i++) {
        if (p[i] == '_') {
            if (i == len - 1 || p[i + 1] == '_') {
                goto err;
            }
            new_len--;
            memmove(p + i, p + i + 1, len - i - 1);
        }
        p[i] = digit_value[p[i]];
        if (p[i] >= base) {
            goto err;
        }
    }
    len = new_len;

    MPZ_Object *res = MPZ_new(1 + len/2, negative);

    if (!res) {
        PyMem_Free(buf);
        return NULL;
    }
    if (CHECK_NO_MEM_LEAK) {
        res->size = mpn_set_str(res->digits, p, len, base);
    }
    else {
        Py_DECREF(res);
        PyMem_Free(buf);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    PyMem_Free(buf);

    mp_limb_t *tmp = res->digits;

    res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
    if (!res->digits) {
        res->digits = tmp;
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    MPZ_normalize(res);
    return res;
err:
    PyMem_Free(buf);
    PyErr_Format(PyExc_ValueError,
                 "invalid literal for mpz() with base %d: %.200R",
                 base, obj);
    return NULL;
}

#if !defined(PYPY_VERSION)
#define BITS_TO_LIMBS(n) (((n) + (GMP_NUMB_BITS - 1))/GMP_NUMB_BITS)

static size_t int_digit_size, int_nails, int_bits_per_digit;
static int int_digits_order, int_endianness;
#endif

static MPZ_Object *
MPZ_from_int(PyObject *obj)
{
#if !defined(PYPY_VERSION)
    static PyLongExport long_export;
    MPZ_Object *res = NULL;

    if (PyLong_Export(obj, &long_export) < 0) {
        return res;
    }

    if (long_export.digits) {
        mp_size_t ndigits = long_export.ndigits;
        mp_size_t size = BITS_TO_LIMBS(ndigits*(8*int_digit_size - int_nails));
        res = MPZ_new(size, long_export.negative);
        if (!res) {
            return res;
        }

        mpz_t z;

        z->_mp_d = res->digits;
        z->_mp_size = (res->negative ? -1 : 1)*res->size;
        z->_mp_alloc = res->size;
        mpz_import(z, ndigits, int_digits_order, int_digit_size,
                   int_endianness, int_nails, long_export.digits);
        PyLong_FreeExport(&long_export);
    }
    else {
        int64_t value = long_export.value;
        mp_size_t size = BITS_TO_LIMBS(8*sizeof(int64_t) - int_nails);
        res = MPZ_new(size, value < 0);
        if (!res) {
            return res;
        }

        mpz_t z;

        z->_mp_d = res->digits;
        z->_mp_size = (res->negative ? -1 : 1)*res->size;
        z->_mp_alloc = res->size;
        if (res->negative) {
            value = -value;
        }
        mpz_import(z, 1, -1, sizeof(int64_t), 0, 0, &value);
    }
    MPZ_normalize(res);
    return res;
#else
    PyObject *str = PyNumber_ToBase(obj, 16);

    if (!str) {
        return NULL;
    }

    MPZ_Object *res = MPZ_from_str(str, 16);

    if (!res) {
        return NULL;
    }
    Py_DECREF(str);
    return res;
#endif
}

static PyObject *
MPZ_to_int(MPZ_Object *u)
{
#if !defined(PYPY_VERSION)
    mpz_t z;

    z->_mp_d = u->digits;
    z->_mp_size = (u->negative ? -1 : 1)*u->size;
    z->_mp_alloc = u->size;
    if (mpz_fits_slong_p(z)) {
        return PyLong_FromLong(mpz_get_si(z));
    }

    size_t size = (mpn_sizeinbase(u->digits, u->size, 2) +
                   int_bits_per_digit - 1)/int_bits_per_digit;
    void *digits;
    PyLongWriter *writer = PyLongWriter_Create(u->negative, size, &digits);

    if (!writer) {
        return NULL;
    }
    mpz_export(digits, NULL, int_digits_order, int_digit_size,
               int_endianness, int_nails, z);
    return PyLongWriter_Finish(writer);
#else
    PyObject *str = MPZ_to_str(u, 16, 0);

    if (!str) {
        return NULL;
    }

    PyObject *res = PyLong_FromUnicodeObject(str, 16);

    Py_DECREF(str);
    return res;
#endif
}

static int
MPZ_compare(MPZ_Object *u, MPZ_Object *v)
{
    if (u == v) {
        return 0;
    }

    int sign = u->negative ? -1 : 1;

    if (u->negative != v->negative) {
        return sign;
    }
    else if (u->size != v->size) {
        return (u->size < v->size) ? -sign : sign;
    }

    int r = mpn_cmp(u->digits, v->digits, u->size);

    return u->negative ? -r : r;
}

static mp_limb_t
MPZ_AsManAndExp(MPZ_Object *u, Py_ssize_t *e)
{
    mp_limb_t high = 1ULL << DBL_MANT_DIG;
    mp_limb_t r = 0, carry, left;
    mp_size_t us = u->size, i, bits = 0;

    if (!us) {
        *e = 0;
        return 0;
    }
    r = u->digits[us - 1];
    if (r >= high) {
        while ((r >> bits) >= high) {
            bits++;
        }
        left = 1ULL << (bits - 1);
        carry = r & (2*left - 1);
        r >>= bits;
        i = us - 1;
        *e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG + bits;
    }
    else {
        while (!((r << 1) & high)) {
            r <<= 1;
            bits++;
        }
        i = us - 1;
        *e = (us - 1)*GMP_NUMB_BITS + DBL_MANT_DIG - bits;
        for (i = us - 1; i && bits >= GMP_NUMB_BITS;) {
            bits -= GMP_NUMB_BITS;
            r += u->digits[--i] << bits;
        }
        if (i == 0) {
            return r;
        }
        if (bits) {
            bits = GMP_NUMB_BITS - bits;
            left = 1ULL << (bits - 1);
            r += u->digits[i - 1] >> bits;
            carry = u->digits[i - 1] & (2*left - 1);
            i--;
        }
        else {
            left = 1ULL << (GMP_NUMB_BITS - 1);
            carry = u->digits[i - 1];
            i--;
        }
    }
    if (carry > left) {
        r++;
    }
    else if (carry == left) {
        if (r%2 == 1) {
            r++;
        }
        else {
            mp_size_t j;

            for (j = 0; j < i; j++) {
                if (u->digits[j]) {
                    break;
                }
            }
            if (i != j) {
                r++;
            }
        }
    }
    return r;
}

static double
MPZ_AsDoubleAndExp(MPZ_Object *u, Py_ssize_t *e)
{
    mp_limb_t man = MPZ_AsManAndExp(u, e);
    double d = ldexp(man, -DBL_MANT_DIG);

    if (u->negative) {
        d = -d;
    }
    return d;
}

#define SWAP(T, a, b)  \
    do {               \
        T tmp = a;     \
        a = b;         \
        b = tmp;       \
    } while (0);

static PyObject *
MPZ_add(MPZ_Object *u, MPZ_Object *v, int subtract)
{
    MPZ_Object *res;
    uint8_t negu = u->negative, negv = v->negative;

    if (subtract) {
        negv = !negv;
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object *, u, v);
        SWAP(uint8_t, negu, negv);
    }
    if (negu == negv) {
        res = MPZ_new(Py_MAX(u->size, v->size) + 1, negu);
        if (!res) {
            return NULL;
        }
        res->digits[res->size - 1] = mpn_add(res->digits,
                                             u->digits, u->size,
                                             v->digits, v->size);
    }
    else {
        if (u->size > v->size || mpn_cmp(u->digits, v->digits, u->size) >= 0) {
            res = MPZ_new(Py_MAX(u->size, v->size), negu);
            if (!res) {
                return NULL;
            }
            mpn_sub(res->digits, u->digits, u->size, v->digits, v->size);
        }
        else {
            res = MPZ_new(Py_MAX(u->size, v->size), negv);
            if (!res) {
                return NULL;
            }
            mpn_sub_n(res->digits, v->digits, u->digits, u->size);
        }
    }
    MPZ_normalize(res);
    return (PyObject *)res;
}

static PyObject *
MPZ_mul(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size || !v->size) {
        return (PyObject *)MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(u->size + v->size, u->negative != v->negative);

    if (!res) {
        return NULL;
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object *, u, v);
    }
    if (u == v) {
        if (CHECK_NO_MEM_LEAK) {
            mpn_sqr(res->digits, u->digits, u->size);
        }
        else {
            Py_DECREF(res);
            return PyErr_NoMemory();
        }
    }
    else {
        if (CHECK_NO_MEM_LEAK) {
            mpn_mul(res->digits, u->digits, u->size, v->digits, v->size);
        }
        else {
            Py_DECREF(res);
            return PyErr_NoMemory();
        }
    }
    MPZ_normalize(res);
    return (PyObject *)res;
}

static int
MPZ_DivMod(MPZ_Object *u, MPZ_Object *v, MPZ_Object **q, MPZ_Object **r)
{
    if (!v->size) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return -1;
    }
    if (!u->size) {
        *q = MPZ_FromDigitSign(0, 0);
        *r = MPZ_FromDigitSign(0, 0);
    }
    else if (u->size < v->size) {
        if (u->negative != v->negative) {
            *q = MPZ_FromDigitSign(1, 1);
            *r = (MPZ_Object *)MPZ_add(u, v, 0);
        }
        else {
            *q = MPZ_FromDigitSign(0, 0);
            *r = MPZ_copy(u);
        }
    }
    else {
        *q = MPZ_new(u->size - v->size + 1, u->negative != v->negative);
        if (!*q) {
            return -1;
        }
        *r = MPZ_new(v->size, v->negative);
        if (!*r) {
            Py_DECREF(*q);
            return -1;
        }
        if (CHECK_NO_MEM_LEAK) {
            mpn_tdiv_qr((*q)->digits, (*r)->digits, 0, u->digits, u->size,
                        v->digits, v->size);
        }
        else {
            Py_DECREF(*q);
            Py_DECREF(*r);
            return -1;
        }
        if ((*q)->negative) {
            if (u->digits[u->size - 1] == GMP_NUMB_MAX
                && v->digits[v->size - 1] == 1)
            {
                (*q)->size++;

                mp_limb_t *tmp = (*q)->digits;

                (*q)->digits = PyMem_Resize(tmp, mp_limb_t, (*q)->size);
                if (!(*q)->digits) {
                    (*q)->digits = tmp;
                    Py_DECREF(*q);
                    Py_DECREF(*r);
                    return -1;
                }
                (*q)->digits[(*q)->size - 1] = 0;
            }
            for (mp_size_t i = 0; i < v->size; i++) {
                if ((*r)->digits[i]) {
                    mpn_sub_n((*r)->digits, v->digits, (*r)->digits, v->size);
                    if (mpn_add_1((*q)->digits, (*q)->digits,
                                  (*q)->size - 1, 1))
                    {
                        (*q)->digits[(*q)->size - 2] = 1;
                    }
                    break;
                }
            }
        }
        MPZ_normalize(*q);
        MPZ_normalize(*r);
        return 0;
    }
    if (!*q || !*r) {
        if (*q) {
            Py_DECREF(*q);
        }
        if (*r) {
            Py_DECREF(*r);
        }
        return -1;
    }
    return 0;
}

static MPZ_Object *
MPZ_rshift1(MPZ_Object *u, mp_limb_t rshift, uint8_t negative)
{
    mp_size_t whole = rshift / GMP_NUMB_BITS;
    mp_size_t size = u->size;

    rshift %= GMP_NUMB_BITS;
    if (whole >= size) {
        return MPZ_FromDigitSign(u->negative, negative);
    }
    size -= whole;

    MPZ_Object *res = MPZ_new(size + 1, negative);

    if (!res) {
        return NULL;
    }
    res->digits[size] = 0;

    int carry = 0;

    if (rshift) {
        if (mpn_rshift(res->digits, u->digits + whole, size, rshift)) {
            carry = negative;
        }
    }
    else {
        mpn_copyi(res->digits, u->digits + whole, size);
    }
    if (carry) {
        if (mpn_add_1(res->digits, res->digits, size, 1)) {
            res->digits[size] = 1;
        }
    }
    MPZ_normalize(res);
    return res;
}

static int
MPZ_DivModNear(MPZ_Object *u, MPZ_Object *v, MPZ_Object **q, MPZ_Object **r)
{
    int unexpect = v->negative ? -1 : 1;

    if (MPZ_DivMod(u, v, q, r) == -1) {
        return -1;
    }

    MPZ_Object *halfQ = MPZ_rshift1(v, 1, 0);

    if (!halfQ) {
        Py_DECREF(*q);
        Py_DECREF(*r);
        return -1;
    }
    if (v->negative) {
        halfQ->negative = !halfQ->negative;
    }

    int cmp = MPZ_compare(*r, halfQ);

    Py_DECREF(halfQ);
    if (cmp == 0 && v->digits[0]%2 == 0) {
        if ((*q)->size && (*q)->digits[0]%2 != 0) {
            cmp = unexpect;
        }
    }
    if (cmp == unexpect) {
        MPZ_Object *tmp = *q;
        MPZ_Object *one = MPZ_FromDigitSign(1, 0);

        if (!one) {
            return -1;
        }
        *q = (MPZ_Object *)MPZ_add(*q, one, 0);
        if (!*q) {
            Py_DECREF(tmp);
            Py_DECREF(*r);
            Py_DECREF(one);
            return -1;
        }
        Py_DECREF(tmp);
        Py_DECREF(one);
        tmp = *r;
        *r = (MPZ_Object *)MPZ_add(*r, v, 1);
        if (!*r) {
            Py_DECREF(tmp);
            Py_DECREF(*q);
            return -1;
        }
        Py_DECREF(tmp);
    }
    return 0;
}

static MPZ_Object *
MPZ_lshift1(MPZ_Object *u, mp_limb_t lshift, uint8_t negative)
{
    mp_size_t whole = lshift / GMP_NUMB_BITS;
    mp_size_t size = u->size + whole;

    lshift %= GMP_NUMB_BITS;
    if (lshift) {
        size++;
    }
    if (u->size == 1 && !whole) {
        mp_limb_t t = u->digits[0] << lshift;

        if (t >> lshift == u->digits[0]) {
            return MPZ_FromDigitSign(t, negative);
        }
    }

    MPZ_Object *res = MPZ_new(size, negative);

    if (!res) {
        return NULL;
    }
    if (whole) {
        mpn_zero(res->digits, whole);
    }
    res->digits[size - 1] = mpn_lshift(res->digits + whole, u->digits,
                                       u->size, lshift);
    MPZ_normalize(res);
    return res;
}

static PyObject *
MPZ_truediv(MPZ_Object *u, MPZ_Object *v)
{
    if (!v->size) {
        PyErr_SetString(PyExc_ZeroDivisionError, "division by zero");
        return NULL;
    }
    if (!u->size) {
        return PyFloat_FromDouble(v->negative ? -0.0 : 0.0);
    }

    Py_ssize_t shift = (mpn_sizeinbase(v->digits, v->size, 2)
                        - mpn_sizeinbase(u->digits, u->size, 2));
    Py_ssize_t n = shift;
    MPZ_Object *a = u, *b = v;

    if (shift < 0) {
        SWAP(MPZ_Object *, a, b);
        n = -n;
    }

    mp_size_t whole = n / GMP_NUMB_BITS;

    n %= GMP_NUMB_BITS;
    for (mp_size_t i = b->size; i--;) {
        mp_limb_t da, db = b->digits[i];

        if (i >= whole) {
            if (i - whole < a->size) {
                da = a->digits[i - whole] << n;
            }
            else {
                da = 0;
            }
            if (n && i > whole) {
                da |= a->digits[i - whole - 1] >> (GMP_NUMB_BITS - n);
            }
        }
        else {
            da = 0;
        }
        if (da < db) {
            if (shift >= 0) {
                shift++;
            }
            break;
        }
        if (da > db) {
            if (shift < 0) {
                shift++;
            }
            break;
        }
    }
    shift += DBL_MANT_DIG - 1;
    if (shift > 0) {
        a = MPZ_lshift1(u, shift, 0);
    }
    else {
        a = MPZ_abs(u);
    }
    if (!a) {
        return NULL;
    }
    if (shift < 0) {
        b = MPZ_lshift1(v, -shift, 0);
    }
    else {
        b = MPZ_abs(v);
    }
    if (!b) {
        Py_DECREF(a);
        return NULL;
    }

    MPZ_Object *c, *d;

    if (MPZ_DivModNear(a, b, &c, &d) == -1) {
        Py_DECREF(a);
        Py_DECREF(b);
        return NULL;
    }
    Py_DECREF(a);
    Py_DECREF(b);
    Py_DECREF(d);

    Py_ssize_t exp;
    double res = MPZ_AsDoubleAndExp(c, &exp);

    Py_DECREF(c);
    if (u->negative != v->negative) {
        res = -res;
    }
    exp -= shift;

    if (exp > DBL_MAX_EXP) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    res = ldexp(res, exp);
    if (isinf(res)) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(res);
}

static MPZ_Object *
MPZ_invert(MPZ_Object *u)
{
    MPZ_Object *res;

    if (u->negative) {
        res = MPZ_new(u->size, 0);
        if (!res) {
            return NULL;
        }
        mpn_sub_1(res->digits, u->digits, u->size, 1);
        res->size -= res->digits[u->size - 1] == 0;
    }
    else if (!u->size) {
        return MPZ_FromDigitSign(1, 1);
    }
    else {
        res = MPZ_new(u->size + 1, 1);
        if (!res) {
            return NULL;
        }
        res->digits[u->size] = mpn_add_1(res->digits, u->digits, u->size, 1);
        MPZ_normalize(res);
    }
    return res;
}

static MPZ_Object *
MPZ_lshift(MPZ_Object *u, MPZ_Object *v)
{
    if (v->negative) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!u->size) {
        return MPZ_FromDigitSign(0, 0);
    }
    if (!v->size) {
        return MPZ_copy(u);
    }
    if (v->size > 1) {
        PyErr_SetString(PyExc_OverflowError, "too many digits in integer");
        return NULL;
    }
    return MPZ_lshift1(u, v->digits[0], u->negative);
}

static MPZ_Object *
MPZ_rshift(MPZ_Object *u, MPZ_Object *v)
{
    if (v->negative) {
        PyErr_SetString(PyExc_ValueError, "negative shift count");
        return NULL;
    }
    if (!u->size) {
        return MPZ_FromDigitSign(0, 0);
    }
    if (!v->size) {
        return MPZ_copy(u);
    }
    if (v->size > 1) {
        if (u->negative) {
            return MPZ_FromDigitSign(1, 1);
        }
        else {
            return MPZ_FromDigitSign(0, 0);
        }
    }
    return MPZ_rshift1(u, v->digits[0], u->negative);
}

static MPZ_Object *
MPZ_and(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size || !v->size) {
        return MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = MPZ_invert(v);
            if (!v) {
                Py_DECREF(u);
                return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object *, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(1, 1);
            }
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_ior_n(res->digits, u->digits, v->digits, v->size);
            }
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(v->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_andn_n(res->digits, v->digits, u->digits, v->size);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(u->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            if (v->size) {
                mpn_andn_n(res->digits, u->digits, v->digits, v->size);
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(v->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_and_n(res->digits, u->digits, v->digits, v->size);
    MPZ_normalize(res);
    return res;
}

static MPZ_Object *
MPZ_or(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size) {
        return MPZ_copy(v);
    }
    if (!v->size) {
        return MPZ_copy(u);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = MPZ_invert(v);
            if (!v) {
                Py_DECREF(u);
                return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object *, u, v);
        }
        if (u->negative & v->negative) {
            if (!v->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(1, 1);
            }
            res = MPZ_new(v->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_and_n(res->digits, u->digits, v->digits, v->size);
            res->digits[v->size] = mpn_add_1(res->digits, res->digits,
                                             v->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_andn_n(res->digits, u->digits, v->digits, v->size);
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(v->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            if (v->size) {
                mpn_andn_n(res->digits, v->digits, u->digits, v->size);
                res->digits[v->size] = mpn_add_1(res->digits, res->digits,
                                                 v->size, 1);
                MPZ_normalize(res);
            }
            else {
                res->digits[0] = 1;
            }
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(u->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_ior_n(res->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                  u->size - v->size);
    }
    else {
        MPZ_normalize(res);
    }
    return res;
}

static MPZ_Object *
MPZ_xor(MPZ_Object *u, MPZ_Object *v)
{
    if (!u->size) {
        return MPZ_copy(v);
    }
    if (!v->size) {
        return MPZ_copy(u);
    }

    MPZ_Object *res;

    if (u->negative || v->negative) {
        if (u->negative) {
            u = MPZ_invert(u);
            if (!u) {
                return NULL;
            }
            u->negative = 1;
        }
        else {
            Py_INCREF(u);
        }
        if (v->negative) {
            v = MPZ_invert(v);
            if (!v) {
                Py_DECREF(u);
                return NULL;
            }
            v->negative = 1;
        }
        else {
            Py_INCREF(v);
        }
        if (u->size < v->size) {
            SWAP(MPZ_Object *, u, v);
        }
        if (u->negative & v->negative) {
            if (!u->size) {
                Py_DECREF(u);
                Py_DECREF(v);
                return MPZ_FromDigitSign(0, 0);
            }
            res = MPZ_new(u->size, 0);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(res->digits, u->digits, v->digits, v->size);
            }
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else if (u->negative) {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            mpn_xor_n(res->digits, v->digits, u->digits, v->size);
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
        else {
            res = MPZ_new(u->size + 1, 1);
            if (!res) {
                Py_DECREF(u);
                Py_DECREF(v);
                return NULL;
            }
            mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                      u->size - v->size);
            if (v->size) {
                mpn_xor_n(res->digits, u->digits, v->digits, v->size);
            }
            res->digits[u->size] = mpn_add_1(res->digits, res->digits,
                                             u->size, 1);
            MPZ_normalize(res);
            Py_DECREF(u);
            Py_DECREF(v);
            return res;
        }
    }
    if (u->size < v->size) {
        SWAP(MPZ_Object *, u, v);
    }
    res = MPZ_new(u->size, 0);
    if (!res) {
        return NULL;
    }
    mpn_xor_n(res->digits, u->digits, v->digits, v->size);
    if (u->size != v->size) {
        mpn_copyi(&res->digits[v->size], &u->digits[v->size],
                  u->size - v->size);
    }
    else {
        MPZ_normalize(res);
    }
    return res;
}

static MPZ_Object *
MPZ_pow1(MPZ_Object *u, mp_limb_t e)
{
    MPZ_Object *res = MPZ_new(u->size * e, u->negative && e%2);

    if (!res) {
        return NULL;
    }

    mp_limb_t *tmp = PyMem_New(mp_limb_t, res->size);

    if (!tmp) {
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    if (CHECK_NO_MEM_LEAK) {
        res->size = mpn_pow_1(res->digits, u->digits, u->size, e, tmp);
    }
    else {
        PyMem_Free(tmp);
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    PyMem_Free(tmp);
    tmp = res->digits;
    res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
    if (!res->digits) {
        res->digits = tmp;
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    return res;
}

/* XXX: don't use mpz_powm() for odd w, replace
   mpn_sec_powm() by mpn_powm(). */
static MPZ_Object *
MPZ_powm(MPZ_Object *u, MPZ_Object *v, MPZ_Object *w)
{
    if (mpn_scan1(w->digits, 0)) {
        __mpz_struct tmp, b, e, m;

       b._mp_d = u->digits;
       b._mp_size = u->size;
       e._mp_d = v->digits;
       e._mp_size = v->size;
       m._mp_d = w->digits;
       m._mp_size = w->size;
       if (CHECK_NO_MEM_LEAK) {
           mpz_init(&tmp);
           mpz_powm(&tmp, &b, &e, &m);
       }
       else {
           return (MPZ_Object *)PyErr_NoMemory();
       }

       MPZ_Object *res = MPZ_new(tmp._mp_size, 0);

       if (!res) {
           mpz_clear(&tmp);
           return NULL;
       }
       mpn_copyi(res->digits, tmp._mp_d, res->size);
       mpz_clear(&tmp);
       return res;
    }

    MPZ_Object *res = MPZ_new(w->size, 0);

    if (!res) {
        return NULL;
    }

    mp_size_t enb = v->size * GMP_NUMB_BITS;
    mp_size_t tmp_size = mpn_sec_powm_itch(u->size, enb, w->size);
    mp_limb_t *tmp = PyMem_New(mp_limb_t, tmp_size);

    if (!tmp) {
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    if (CHECK_NO_MEM_LEAK) {
        mpn_sec_powm(res->digits, u->digits, u->size, v->digits,
                     enb, w->digits, w->size, tmp);
    }
    else {
        PyMem_Free(tmp);
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    PyMem_Free(tmp);
    MPZ_normalize(res);
    return res;
}

/* XXX: use mpn_gcdext() */
static MPZ_Object *
MPZ_inverse(MPZ_Object *u, MPZ_Object *v)
{
    MPZ_Object *a = MPZ_copy(u);
    MPZ_Object *n = MPZ_copy(v);
    MPZ_Object *b = MPZ_FromDigitSign(1, 0);
    MPZ_Object *c = MPZ_FromDigitSign(0, 0);

    if (!a || !n || !b || !c) {
        Py_XDECREF(a);
        Py_XDECREF(n);
        Py_XDECREF(b);
        Py_XDECREF(c);
        return NULL;
    }
    while (n->size) {
        MPZ_Object *q, *r;

        if (MPZ_DivMod(a, n, &q, &r) == -1) {
            Py_DECREF(a);
            Py_DECREF(n);
            Py_DECREF(b);
            Py_DECREF(c);
            return NULL;
        }
        Py_SETREF(a, n);
        n = r;

        MPZ_Object *t = (MPZ_Object *)MPZ_mul(q, c);

        if (!t) {
            Py_DECREF(a);
            Py_DECREF(n);
            Py_DECREF(b);
            Py_DECREF(c);
            return NULL;
        }

        MPZ_Object *s = (MPZ_Object *)MPZ_add(b, t, 1);

        if (!s) {
            Py_DECREF(t);
            Py_DECREF(a);
            Py_DECREF(n);
            Py_DECREF(b);
            Py_DECREF(c);
            return NULL;
        }
        Py_DECREF(t);
        Py_SETREF(b, c);
        c = s;
    }
    Py_DECREF(c);
    Py_DECREF(n);
    if (a->size == 1 && a->digits[0] == 1) {
        Py_DECREF(a);
        return b;
    }
    Py_DECREF(a);
    Py_DECREF(b);
    PyErr_SetString(PyExc_ValueError,
                    "base is not invertible for the given modulus");
    return NULL;
}

static void
revstr(char *s, size_t l, size_t r)
{
    while (l < r) {
        SWAP(char, s[l], s[r]);
        l++;
        r--;
    }
}

static PyObject *
MPZ_to_bytes(MPZ_Object *u, Py_ssize_t length, int is_little, int is_signed)
{
    MPZ_Object *tmp = NULL;
    int is_negative = u->negative;

    if (is_negative) {
        if (!is_signed) {
            PyErr_SetString(PyExc_OverflowError,
                            "can't convert negative mpz to unsigned");
            return NULL;
        }
        tmp = MPZ_new((8*length)/GMP_NUMB_BITS + 1, 0);
        if (!tmp) {
            return NULL;
        }
        mpn_zero(tmp->digits, tmp->size);
        tmp->digits[tmp->size - 1] = 1;
        tmp->digits[tmp->size - 1] <<= (8*length) % (GMP_NUMB_BITS*tmp->size);
        mpn_sub(tmp->digits, tmp->digits, tmp->size, u->digits, u->size);
        MPZ_normalize(tmp);
        u = tmp;
    }

    Py_ssize_t nbits = u->size ? mpn_sizeinbase(u->digits, u->size, 2) : 0;

    if (nbits > 8*length
        || (is_signed && nbits
            && (nbits == 8 * length ? !is_negative : is_negative)))
    {
        PyErr_SetString(PyExc_OverflowError, "int too big to convert");
        return NULL;
    }

    char *buffer = PyMem_Malloc(length);
    Py_ssize_t gap = length - (nbits + GMP_NUMB_BITS/8 - 1)/(GMP_NUMB_BITS/8);

    if (!buffer) {
        Py_XDECREF(tmp);
        return PyErr_NoMemory();
    }
    memset(buffer, is_negative ? 0xFF : 0, gap);
    if (u->size) {
        if (CHECK_NO_MEM_LEAK) {
            mpn_get_str((unsigned char *)(buffer + gap), 256,
                        u->digits, u->size);
        }
        else {
            Py_XDECREF(tmp);
            return PyErr_NoMemory();
        }
    }
    Py_XDECREF(tmp);
    if (is_little && length) {
        revstr(buffer, 0, length - 1);
    }

    PyObject *bytes = PyBytes_FromStringAndSize(buffer, length);

    PyMem_Free(buffer);
    return bytes;
}

static MPZ_Object *
MPZ_from_bytes(PyObject *obj, int is_little, int is_signed)
{
    PyObject *bytes = PyObject_Bytes(obj);
    char *buffer;
    Py_ssize_t length;

    if (bytes == NULL) {
        return NULL;
    }
    if (PyBytes_AsStringAndSize(bytes, &buffer, &length) == -1) {
        return NULL;
    }
    if (!length) {
        Py_DECREF(bytes);
        return MPZ_FromDigitSign(0, 0);
    }

    MPZ_Object *res = MPZ_new(1 + length/2, 0);

    if (!res) {
        Py_DECREF(bytes);
        return NULL;
    }
    if (is_little) {
        char *tmp = PyMem_Malloc(length);

        if (!tmp) {
            Py_DECREF(bytes);
            return (MPZ_Object *)PyErr_NoMemory();
        }
        memcpy(tmp, buffer, length);
        buffer = tmp;
        revstr(buffer, 0, length - 1);
    }
    if (CHECK_NO_MEM_LEAK) {
        res->size = mpn_set_str(res->digits, (unsigned char *)buffer,
                                length, 256);
    }
    else {
        Py_DECREF(res);
        PyMem_Free(bytes);
        if (is_little) {
            PyMem_Free(buffer);
        }
        return (MPZ_Object *)PyErr_NoMemory();
    }
    Py_DECREF(bytes);
    if (is_little) {
        PyMem_Free(buffer);
    }

    mp_limb_t *tmp = res->digits;

    res->digits = PyMem_Resize(tmp, mp_limb_t, res->size);
    if (!res->digits) {
        res->digits = tmp;
        Py_DECREF(res);
        return (MPZ_Object *)PyErr_NoMemory();
    }
    MPZ_normalize(res);
    if (is_signed && mpn_sizeinbase(res->digits, res->size,
                                    2) == 8*(size_t)length)
    {
        if (res->size > 1) {
            if (mpn_sub_1(res->digits, res->digits, res->size, 1)) {
                res->digits[res->size - 1] -= 1;
            }
            mpn_com(res->digits, res->digits, res->size - 1);
        }
        else {
            res->digits[res->size - 1] -= 1;
        }
        res->digits[res->size - 1] = ~res->digits[res->size - 1];

        mp_size_t shift = GMP_NUMB_BITS*res->size - 8*length;

        res->digits[res->size - 1] <<= shift;
        res->digits[res->size - 1] >>= shift;
        res->negative = 1;
        MPZ_normalize(res);
    }
    return res;
}

#define MPZ_CheckExact(a) PyObject_TypeCheck((a), &MPZ_Type)

static PyObject *
new(PyTypeObject *Py_UNUSED(type), PyObject *args, PyObject *keywds)
{
    static char *kwlist[] = {"", "base", NULL};
    Py_ssize_t argc = PyTuple_GET_SIZE(args);
    int base = 10;
    PyObject *arg;

    if (argc == 0) {
        return (PyObject *)MPZ_FromDigitSign(0, 0);
    }
    if (argc == 1 && !keywds) {
        arg = PyTuple_GET_ITEM(args, 0);
        if (PyLong_Check(arg)) {
            return (PyObject *)MPZ_from_int(arg);
        }
        if (MPZ_CheckExact(arg)) {
            return Py_NewRef(arg);
        }
        if (PyNumber_Check(arg) && Py_TYPE(arg)->tp_as_number->nb_int) {
            PyObject *integer = Py_TYPE(arg)->tp_as_number->nb_int(arg);

            if (!integer) {
                return NULL;
            }
            if (!PyLong_Check(integer)) {
                PyErr_Format(PyExc_TypeError,
                             "__int__ returned non-int (type %.200s)",
                             Py_TYPE(integer)->tp_name);
                Py_DECREF(integer);
                return NULL;
            }
            if (!PyLong_CheckExact(integer)
                && PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                                    "__int__ returned non-int (type %.200s).  "
                                    "The ability to return an instance of a "
                                    "strict subclass of int "
                                    "is deprecated, and may be removed "
                                    "in a future version of Python.",
                                    Py_TYPE(integer)->tp_name))
            {
                Py_DECREF(integer);
                return NULL;
            }

            PyObject *res = (PyObject *)MPZ_from_int(integer);

            Py_DECREF(integer);
            return res;
        }
        goto str;
    }
    if (!PyArg_ParseTupleAndKeywords(args, keywds, "O|i",
                                     kwlist, &arg, &base))
    {
        return NULL;
    }
str:
    if (PyUnicode_Check(arg)) {
        return (PyObject *)MPZ_from_str(arg, base);
    }
    else if (PyByteArray_Check(arg) || PyBytes_Check(arg)) {
        const char *string;

        if (PyByteArray_Check(arg)) {
            string = PyByteArray_AS_STRING(arg);
        }
        else {
            string = PyBytes_AS_STRING(arg);
        }

        PyObject *str = PyUnicode_FromString(string);

        if (!str) {
            return NULL;
        }

        PyObject *res = (PyObject *)MPZ_from_str(str, base);

        Py_DECREF(str);
        return res;
    }
    PyErr_SetString(PyExc_TypeError,
                    "can't convert non-string with explicit base");
    return NULL;
}

static void
dealloc(PyObject *self)
{
    PyMem_Free(((MPZ_Object *)self)->digits);
    PyObject_Free(self);
}

static PyObject *
repr(PyObject *self)
{
    return MPZ_to_str((MPZ_Object *)self, 10, OPT_TAG);
}

static PyObject *
str(PyObject *self)
{
    return MPZ_to_str((MPZ_Object *)self, 10, 0);
}

#define CHECK_OP(u, a)              \
    static MPZ_Object *u;           \
                                    \
    if (MPZ_CheckExact(a)) {        \
        u = (MPZ_Object *)a;        \
        Py_INCREF(u);               \
    }                               \
    else if (PyLong_Check(a)) {     \
        u = MPZ_from_int(a);        \
        if (!u) {                   \
            goto end;               \
        }                           \
    }                               \
    else {                          \
        Py_RETURN_NOTIMPLEMENTED;   \
    }

static PyObject *
richcompare(PyObject *self, PyObject *other, int op)
{
    CHECK_OP(u, self);
    CHECK_OP(v, other);

    int r = MPZ_compare(u, v);

    Py_XDECREF(u);
    Py_XDECREF(v);
    switch (op) {
        case Py_LT:
            return PyBool_FromLong(r == -1);
        case Py_LE:
            return PyBool_FromLong(r != 1);
        case Py_GT:
            return PyBool_FromLong(r == 1);
        case Py_GE:
            return PyBool_FromLong(r != -1);
        case Py_EQ:
            return PyBool_FromLong(r == 0);
        case Py_NE:
            return PyBool_FromLong(r != 0);
    }
    Py_RETURN_NOTIMPLEMENTED;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}

static Py_hash_t
hash(PyObject *self)
{
    MPZ_Object *u = (MPZ_Object *)self;
    Py_hash_t r = mpn_mod_1(u->digits, u->size, _PyHASH_MODULUS);

    if (u->negative) {
        r = -r;
    }
    if (r == -1) {
        r = -2;
    }
    return r;
}

static PyObject *
plus(PyObject *self)
{
    return (PyObject *)MPZ_copy((MPZ_Object *)self);
}

static PyObject *
minus(PyObject *self)
{
    MPZ_Object *res = (MPZ_Object *)plus(self), *u = (MPZ_Object *)self;

    if (!res) {
        return NULL;
    }
    if (u->size) {
        res->negative = !u->negative;
    }
    return (PyObject *)res;
}

static PyObject *
absolute(PyObject *self)
{
    return (PyObject *)MPZ_abs((MPZ_Object *)self);
}

static PyObject *
to_int(PyObject *self)
{
    return MPZ_to_int((MPZ_Object *)self);
}

static PyObject *
to_float(PyObject *self)
{
    Py_ssize_t exp;
    MPZ_Object *u = (MPZ_Object *)self;
    double d = MPZ_AsDoubleAndExp(u, &exp);

    if (exp > DBL_MAX_EXP) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    d = ldexp(d, exp);
    if (isinf(d)) {
        PyErr_SetString(PyExc_OverflowError,
                        "integer too large to convert to float");
        return NULL;
    }
    return PyFloat_FromDouble(d);
}

static int
to_bool(PyObject *self)
{
    return ((MPZ_Object *)self)->size != 0;
}

static PyObject *
add(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = MPZ_add(u, v, 0);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
}

static PyObject *
sub(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = MPZ_add(u, v, 1);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
}

static PyObject *
mul(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = MPZ_mul(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
divmod(PyObject *self, PyObject *other)
{
    PyObject *res = PyTuple_New(2);

    if (!res) {
        return NULL;
    }
    CHECK_OP(u, self);
    CHECK_OP(v, other);

    MPZ_Object *q, *r;

    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        goto end;
    }
    PyTuple_SET_ITEM(res, 0, (PyObject *)q);
    PyTuple_SET_ITEM(res, 1, (PyObject *)r);
    return res;
end:
    Py_DECREF(res);
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}

static PyObject *
floordiv(PyObject *self, PyObject *other)
{
    MPZ_Object *q, *r;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        goto end;
    }
    Py_DECREF(r);
    return (PyObject *)q;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}

static PyObject *
truediv(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    res = MPZ_truediv(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return res;
}

static PyObject *
rem(PyObject *self, PyObject *other)
{
    MPZ_Object *q, *r;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    if (MPZ_DivMod(u, v, &q, &r) == -1) {
        return NULL;
    }
    Py_DECREF(q);
    return (PyObject *)r;
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return NULL;
}

static PyObject *
invert(PyObject *self)
{
    return (PyObject *)MPZ_invert((MPZ_Object *)self);
}

static PyObject *
lshift(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = (PyObject *)MPZ_lshift(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
rshift(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = (PyObject *)MPZ_rshift(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
bitwise_and(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = (PyObject *)MPZ_and(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
bitwise_or(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = (PyObject *)MPZ_or(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
bitwise_xor(PyObject *self, PyObject *other)
{
    PyObject *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);

    res = (PyObject *)MPZ_xor(u, v);
end:
    Py_XDECREF(u);
    Py_XDECREF(v);
    return (PyObject *)res;
}

static PyObject *
power(PyObject *self, PyObject *other, PyObject *module)
{
    MPZ_Object *res = NULL;

    CHECK_OP(u, self);
    CHECK_OP(v, other);
    if (Py_IsNone(module)) {
        if (v->negative) {
            PyObject *uf, *vf, *resf;

            uf = to_float((PyObject *)u);
            Py_DECREF(u);
            if (!uf) {
                Py_DECREF(v);
                return NULL;
            }
            vf = to_float((PyObject *)v);
            Py_DECREF(v);
            if (!vf) {
                Py_DECREF(uf);
                goto end;
            }
            resf = PyFloat_Type.tp_as_number->nb_power(uf, vf, Py_None);
            Py_DECREF(uf);
            Py_DECREF(vf);
            return resf;
        }
        if (!v->size) {
            res = MPZ_FromDigitSign(1, 0);
            goto end;
        }
        if (!u->size) {
            res = MPZ_FromDigitSign(0, 0);
            goto end;
        }
        if (u->size == 1 && u->digits[0] == 1) {
            if (u->negative) {
                res = MPZ_FromDigitSign(1, v->digits[0] % 2);
                goto end;
            }
            else {
                res = MPZ_FromDigitSign(1, 0);
                goto end;
            }
        }
        if (v->size == 1) {
            res = MPZ_pow1(u, v->digits[0]);
            goto end;
        }
        Py_DECREF(u);
        Py_DECREF(v);
        return PyErr_NoMemory();
    }
    else {
        MPZ_Object *w;

        if (MPZ_CheckExact(module)) {
            w = (MPZ_Object *)module;
            Py_INCREF(w);
        }
        else if (PyLong_Check(module)) {
            w = MPZ_from_int(module);
            if (!w) {
                goto end;
            }
        }
        else {
            Py_RETURN_NOTIMPLEMENTED;
        }
        if (!w->size) {
            PyErr_SetString(PyExc_ValueError,
                            "pow() 3rd argument cannot be 0");
            Py_DECREF(w);
            goto end;
        }

        int negativeOutput = 0;

        if (w->negative) {
            MPZ_Object *tmp = MPZ_copy(w);

            if (!tmp) {
                Py_DECREF(w);
                goto end;
            }
            negativeOutput = 1;
            tmp->negative = 0;
            Py_SETREF(w, tmp);
        }
        if (w->size == 1 && w->digits[0] == 1) {
            res = MPZ_FromDigitSign(0, 0);
            Py_DECREF(w);
            goto end;
        }
        if (v->negative) {
            MPZ_Object *tmp = MPZ_copy(v);

            if (!tmp) {
                Py_DECREF(w);
                goto end;
            }
            tmp->negative = 0;
            Py_SETREF(v, tmp);

            tmp = MPZ_inverse(u, w);
            if (!tmp) {
                Py_DECREF(w);
                goto end;
            }
            Py_SETREF(u, tmp);
        }
        if (u->negative || u->size > w->size) {
            MPZ_Object *q, *r;

            if (MPZ_DivMod(u, w, &q, &r) == -1) {
                Py_DECREF(w);
                goto end;
            }
            Py_DECREF(q);
            Py_SETREF(u, r);
        }
        if (!v->size) {
            res = MPZ_FromDigitSign(1, 0);
            goto negative;
        }
        if (!u->size) {
            res = MPZ_FromDigitSign(0, 0);
            goto negative;
        }
        if (u->size == 1 && u->digits[0] == 1) {
            res = MPZ_FromDigitSign(1, 0);
            goto negative;
        }
        res = MPZ_powm(u, v, w);
negative:
        if (negativeOutput && res->size) {
            MPZ_Object *tmp = res;

            res = (MPZ_Object *)MPZ_add(res, w, 1);
            if (!res) {
                Py_DECREF(tmp);
                Py_DECREF(w);
                goto end;
            }
            Py_DECREF(tmp);
        }
        Py_DECREF(w);
    }
end:
    Py_DECREF(u);
    Py_DECREF(v);
    return (PyObject *)res;
}

static PyNumberMethods as_number = {
    .nb_add = add,
    .nb_subtract = sub,
    .nb_multiply = mul,
    .nb_divmod = divmod,
    .nb_floor_divide = floordiv,
    .nb_true_divide = truediv,
    .nb_remainder = rem,
    .nb_power = power,
    .nb_positive = plus,
    .nb_negative = minus,
    .nb_absolute = absolute,
    .nb_invert = invert,
    .nb_lshift = lshift,
    .nb_rshift = rshift,
    .nb_and = bitwise_and,
    .nb_or = bitwise_or,
    .nb_xor = bitwise_xor,
    .nb_int = to_int,
    .nb_float = to_float,
    .nb_index = to_int,
    .nb_bool = to_bool,
};

static PyObject *
get_copy(PyObject *self, void *Py_UNUSED(closure))
{
    return Py_NewRef(self);
}

static PyObject *
get_one(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_FromDigitSign(1, 0);
}

static PyObject *
get_zero(PyObject *Py_UNUSED(self), void *Py_UNUSED(closure))
{
    return (PyObject *)MPZ_FromDigitSign(0, 0);
}

static PyGetSetDef getsetters[] = {
    {"numerator", (getter)get_copy, NULL,
     "the numerator of a rational number in lowest terms", NULL},
    {"denominator", (getter)get_one, NULL,
     "the denominator of a rational number in lowest terms", NULL},
    {"real", (getter)get_copy, NULL, "the real part of a complex number",
     NULL},
    {"imag", (getter)get_zero, NULL, "the imaginary part of a complex number",
     NULL},
    {NULL} /* sentinel */
};

static PyObject *
bit_length(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    mp_limb_t digit = u->size ? mpn_sizeinbase(u->digits, u->size, 2) : 0;

    return (PyObject *)MPZ_FromDigitSign(digit, 0);
}

static PyObject *
bit_count(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    mp_bitcnt_t count = u->size ? mpn_popcount(u->digits, u->size) : 0;

    return (PyObject *)MPZ_FromDigitSign(count, 0);
}

typedef struct gmp_pyargs {
    Py_ssize_t minpos;
    Py_ssize_t maxpos;
    Py_ssize_t minargs;
    Py_ssize_t maxargs;
    const char *fname;
    const char * const *keywords;
} gmp_pyargs;

static int
gmp_parse_pyargs(const gmp_pyargs *fnargs, int argidx[],
                 PyObject *const *args, Py_ssize_t nargs,
                 PyObject *kwnames)
{
    if (nargs > fnargs->maxpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu positional arguments",
                     fnargs->fname, fnargs->maxpos);
        return -1;
    }
    if (nargs < fnargs->minpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at least %zu positional arguments",
                     fnargs->fname, fnargs->minpos);
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        argidx[i] = i;
    }

    Py_ssize_t nkws = 0;

    if (kwnames) {
        nkws = PyTuple_GET_SIZE(kwnames);
    }
    if (nkws > fnargs->maxpos - fnargs->minpos) {
        PyErr_Format(PyExc_TypeError,
                     "%s() takes at most %zu keyword arguments",
                     fnargs->fname, fnargs->maxargs - fnargs->minpos);
        return -1;
    }
    if (nkws + nargs < fnargs->minargs) {
        PyErr_Format(PyExc_TypeError,
                     ("%s() takes at least %zu positional or "
                      "keyword arguments"), fnargs->fname, fnargs->minargs);
        return -1;
    }
    for (Py_ssize_t i = 0; i < nkws; i++) {
        const char *kwname = PyUnicode_AsUTF8(PyTuple_GET_ITEM(kwnames, i));
        Py_ssize_t j = fnargs->minpos;

        for (; j < fnargs->maxargs; j++) {
            if (strcmp(kwname, fnargs->keywords[j]) == 0) {
                if (j > fnargs->maxpos || nargs <= j) {
                    argidx[j] = (int)(nargs + i);
                    break;
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                 ("argument for %s() given by name "
                                  "('%s') and position (%zu)"), fnargs->fname,
                                 fnargs->keywords[j], j + 1);
                    return -1;
                }
            }
        }
        if (j == fnargs->maxargs) {
            PyErr_Format(PyExc_TypeError,
                    ("%s() got an unexpected keyword argument"
                     " '%s'"), fnargs->fname, kwname);
            return -1;
        }
    }
    return 0;
}

static PyObject *
to_bytes(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
         PyObject *kwnames)
{
    static const char * const keywords[] = {"length", "byteorder",
                                            "signed", NULL};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .minpos = 0,
        .maxpos = 2,
        .minargs = 0,
        .maxargs = 3,
        .fname = "to_bytes",
    };
    int argidx[3] = {-1, -1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    Py_ssize_t length = 1;
    int is_little = 0, is_signed = 0;

    if (argidx[0] >= 0) {
        PyObject *arg = args[argidx[0]];

        if (PyLong_Check(arg)) {
            length = PyLong_AsSsize_t(arg);
            if (length < 0) {
                PyErr_SetString(PyExc_ValueError,
                                "length argument must be non-negative");
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "to_bytes() takes an integer argument 'length'");
            return NULL;
        }
    }
    if (argidx[1] >= 0) {
        PyObject *arg = args[argidx[1]];

        if (PyUnicode_Check(arg)) {
            const char *byteorder = PyUnicode_AsUTF8(arg);

            if (!byteorder) {
                return NULL;
            }
            else if (strcmp(byteorder, "big") == 0) {
                is_little = 0;
            }
            else if (strcmp(byteorder, "little") == 0) {
                is_little = 1;
            }
            else {
                PyErr_SetString(PyExc_ValueError,
                                "byteorder must be either 'little' or 'big'");
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "to_bytes() argument 'byteorder' must be str");
            return NULL;
        }
    }
    if (argidx[2] >= 0) {
        is_signed = PyObject_IsTrue(args[argidx[2]]);
    }
    return MPZ_to_bytes((MPZ_Object *)self, length, is_little, is_signed);
}

static PyObject *
_from_bytes(PyObject *Py_UNUSED(module), PyObject *arg)
{
    return (PyObject *)MPZ_from_bytes(arg, 0, 1);
}

static PyObject *
from_bytes(PyTypeObject *Py_UNUSED(type), PyObject *const *args,
           Py_ssize_t nargs, PyObject *kwnames)
{
    static const char * const keywords[] = {"bytes", "byteorder",
                                            "signed", NULL};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .minpos = 0,
        .maxpos = 3,
        .minargs = 1,
        .maxargs = 3,
        .fname = "from_bytes",
    };
    int argidx[3] = {-1, -1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    int is_little = 0, is_signed = 0;

    if (argidx[1] >= 0) {
        PyObject *arg = args[argidx[1]];

        if (PyUnicode_Check(arg)) {
            const char *byteorder = PyUnicode_AsUTF8(arg);

            if (!byteorder) {
                return NULL;
            }
            else if (strcmp(byteorder, "big") == 0) {
                is_little = 0;
            }
            else if (strcmp(byteorder, "little") == 0) {
                is_little = 1;
            }
            else {
                PyErr_SetString(PyExc_ValueError,
                                ("byteorder must be either 'little'"
                                 " or 'big'"));
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            ("from_bytes() argument 'byteorder'"
                             " must be str"));
            return NULL;
        }
    }
    if (argidx[2] >= 0) {
        is_signed = PyObject_IsTrue(args[argidx[2]]);
    }
    return (PyObject *)MPZ_from_bytes(args[argidx[0]], is_little, is_signed);
}

static PyObject *
as_integer_ratio(PyObject *self, PyObject *Py_UNUSED(args))
{
    PyObject *one = (PyObject *)MPZ_FromDigitSign(1, 0);

    if (!one) {
        return NULL;
    }

    PyObject *u = Py_NewRef(self);
    PyObject *ratio_tuple = PyTuple_Pack(2, u, one);

    Py_DECREF(u);
    Py_DECREF(one);
    return ratio_tuple;
}

static PyObject *
__round__(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs > 1) {
        PyErr_Format(PyExc_TypeError,
                     "__round__ expected at most 1 argument, got %zu");
        return NULL;
    }

    MPZ_Object *u = (MPZ_Object *)self;

    if (!nargs) {
        return plus(self);
    }

    PyObject *ndigits = PyNumber_Index(args[0]);

    if (!ndigits) {
        return NULL;
    }
    if (!PyLong_IsNegative(ndigits)) {
        Py_DECREF(ndigits);
        return plus(self);
    }

    PyObject *tmp = PyNumber_Negative(ndigits);

    if (!tmp) {
        Py_DECREF(ndigits);
        return NULL;
    }
    Py_SETREF(ndigits, tmp);

    PyObject *ten = (PyObject *)MPZ_FromDigitSign(10, 0);

    if (!ten) {
        Py_DECREF(ndigits);
        return NULL;
    }

    PyObject *p = power(ten, ndigits, Py_None);

    Py_DECREF(ten);
    Py_DECREF(ndigits);
    if (!p) {
        return NULL;
    }

    MPZ_Object *q, *r;

    if (MPZ_DivModNear(u, (MPZ_Object *)p, &q, &r) == -1) {
        Py_DECREF(p);
        return NULL;
    }
    Py_DECREF(p);
    Py_DECREF(q);

    PyObject *res = MPZ_add(u, r, 1);

    Py_DECREF(r);
    return res;
}

static PyObject *from_bytes_func;

static PyObject *
__reduce__(PyObject *self, PyObject *Py_UNUSED(args))
{
    MPZ_Object *u = (MPZ_Object *)self;
    Py_ssize_t len = u->size ? mpn_sizeinbase(u->digits, u->size, 2) : 1;

    return Py_BuildValue("O(N)", from_bytes_func,
                         MPZ_to_bytes(u, (len + 7)/8 + 1, 0, 1));
}

/* FIXME: replace this stub */
static PyObject *
__format__(PyObject *self, PyObject *format_spec)
{
    PyObject *integer = to_int(self);

    if (!integer) {
        return NULL;
    }

    PyObject *res = PyObject_CallMethod(integer, "__format__", "O",
                                        format_spec);

    Py_DECREF(integer);
    return res;
}

static PyObject *
__sizeof__(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    MPZ_Object *u = (MPZ_Object *)self;

    return PyLong_FromSize_t(sizeof(MPZ_Object) + u->size*sizeof(mp_limb_t));
}

static PyObject *
is_integer(PyObject *Py_UNUSED(self), PyObject *Py_UNUSED(args))
{
    Py_RETURN_TRUE;
}

static PyObject *
digits(PyObject *self, PyObject *const *args, Py_ssize_t nargs,
       PyObject *kwnames)
{
    static const char * const keywords[] = {"base", "prefix", NULL};
    const static gmp_pyargs fnargs = {
        .keywords = keywords,
        .minpos = 0,
        .maxpos = 2,
        .minargs = 0,
        .maxargs = 2,
        .fname = "digits",
    };
    int argidx[2] = {-1, -1};

    if (gmp_parse_pyargs(&fnargs, argidx, args, nargs, kwnames) == -1) {
        return NULL;
    }

    int base = 10, prefix = 0;

    if (argidx[0] != -1) {
        PyObject *arg = args[argidx[0]];

        if (PyLong_Check(arg)) {
            base = PyLong_AsInt(arg);
            if (base == -1 && PyErr_Occurred()) {
                return NULL;
            }
        }
        else {
            PyErr_SetString(PyExc_TypeError,
                            "digits() takes an integer argument 'length'");
            return NULL;
        }
    }
    if (argidx[1] != -1) {
        prefix = PyObject_IsTrue(args[argidx[1]]);
    }
    return MPZ_to_str((MPZ_Object *)self, base, prefix ? OPT_PREFIX : 0);
}

PyDoc_STRVAR(to_bytes__doc__,
"to_bytes($self, /, length=1, byteorder=\'big\', *, signed=False)\n--\n\n"
"Return an array of bytes representing an integer.\n\n"
"  length\n"
"    Length of bytes object to use.  An OverflowError is raised if the\n"
"    integer is not representable with the given number of bytes.  Default\n"
"    is length 1.\n"
"  byteorder\n"
"    The byte order used to represent the integer.  If byteorder is \'big\',\n"
"    the most significant byte is at the beginning of the byte array.  If\n"
"    byteorder is \'little\', the most significant byte is at the end of the\n"
"    byte array.  To request the native byte order of the host system, use\n"
"    sys.byteorder as the byte order value.  Default is to use \'big\'.\n"
"  signed\n"
"    Determines whether two\'s complement is used to represent the integer.\n"
"    If signed is False and a negative integer is given, an OverflowError\n"
"    is raised.");
PyDoc_STRVAR(from_bytes__doc__,
"from_bytes($type, /, bytes, byteorder=\'big\', *, signed=False)\n--\n\n"
"Return the integer represented by the given array of bytes.\n\n"
"  bytes\n"
"    Holds the array of bytes to convert.  The argument must either\n"
"    support the buffer protocol or be an iterable object producing bytes.\n"
"    Bytes and bytearray are examples of built-in objects that support the\n"
"    buffer protocol.\n"
"  byteorder\n"
"    The byte order used to represent the integer.  If byteorder is \'big\',\n"
"    the most significant byte is at the beginning of the byte array.  If\n"
"    byteorder is \'little\', the most significant byte is at the end of the\n"
"    byte array.  To request the native byte order of the host system, use\n"
"    sys.byteorder as the byte order value.  Default is to use \'big\'.\n"
"  signed\n"
"    Indicates whether two\'s complement is used to represent the integer.");

static PyMethodDef methods[] = {
    {"conjugate", (PyCFunction)plus, METH_NOARGS,
     "Returns self, the complex conjugate of any int."},
    {"bit_length", bit_length, METH_NOARGS,
     "Number of bits necessary to represent self in binary."},
    {"bit_count", bit_count, METH_NOARGS,
     ("Number of ones in the binary representation of the "
      "absolute value of self.")},
    {"to_bytes", (PyCFunction)to_bytes, METH_FASTCALL | METH_KEYWORDS,
     to_bytes__doc__},
    {"from_bytes", (PyCFunction)from_bytes,
     METH_FASTCALL | METH_KEYWORDS | METH_CLASS, from_bytes__doc__},
    {"as_integer_ratio", as_integer_ratio, METH_NOARGS,
     ("Return a pair of integers, whose ratio is equal to "
      "the original int.\n\nThe ratio is in lowest terms "
      "and has a positive denominator.")},
    {"__trunc__", (PyCFunction)plus, METH_NOARGS,
     "Truncating an Integral returns itself."},
    {"__floor__", (PyCFunction)plus, METH_NOARGS,
     "Flooring an Integral returns itself."},
    {"__ceil__", (PyCFunction)plus, METH_NOARGS,
     "Ceiling of an Integral returns itself."},
    {"__round__", (PyCFunction)__round__, METH_FASTCALL,
     ("__round__($self, ndigits=None, /)\n--\n\n"
      "Rounding an Integral returns itself.\n\n"
      "Rounding with an ndigits argument also returns an integer.")},
    {"__reduce__", __reduce__, METH_NOARGS, NULL},
    {"__format__", __format__, METH_O,
     ("__format__($self, format_spec, /)\n--\n\n"
      "Convert to a string according to format_spec.")},
    {"__sizeof__", __sizeof__, METH_NOARGS,
     "Returns size in memory, in bytes."},
    {"is_integer", is_integer, METH_NOARGS,
     ("Returns True.  Exists for duck type compatibility "
      "with float.is_integer.")},
    {"digits", (PyCFunction)digits, METH_FASTCALL | METH_KEYWORDS,
     ("digits($self, base=10, prefix=False)\n--\n\n"
      "Return Python string representing self in the given base.\n\n"
      "Values for base can range between 2 to 62.")},
    {NULL} /* sentinel */
};

PyDoc_STRVAR(mpz_doc,
"mpz(x, /)\n\
mpz(x, /, base=10)\n\
\n\
Convert a number or string to an integer, or return 0 if no arguments\n\
are given.  If x is a number, return x.__int__().  For floating-point\n\
numbers, this truncates towards zero.\n\
\n\
If x is not a number or if base is given, then x must be a string,\n\
bytes, or bytearray instance representing an integer literal in the\n\
given base.  The literal can be preceded by '+' or '-' and be surrounded\n\
by whitespace.  The base defaults to 10.  Valid bases are 0 and 2-36.\n\
Base 0 means to interpret the base from the string as an integer literal.");

PyTypeObject MPZ_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "gmp.mpz",
    .tp_basicsize = sizeof(MPZ_Object),
    .tp_new = new,
    .tp_dealloc = dealloc,
    .tp_repr = repr,
    .tp_str = str,
    .tp_richcompare = richcompare,
    .tp_hash = hash,
    .tp_as_number = &as_number,
    .tp_getset = getsetters,
    .tp_methods = methods,
    .tp_doc = mpz_doc,
};

static PyObject *
gmp_gcd(PyObject *Py_UNUSED(module), PyObject *const *args, Py_ssize_t nargs)
{
    if (!nargs) {
        return (PyObject *)MPZ_FromDigitSign(0, 0);
    }

    mp_bitcnt_t nzeros_res = 0;
    MPZ_Object *res, *arg, *tmp;

    if (MPZ_CheckExact(args[0])) {
        arg = (MPZ_Object *)args[0];
        Py_INCREF(arg);
    }
    else if (PyLong_Check(args[0])) {
        arg = MPZ_from_int(args[0]);
        if (!arg) {
            return NULL;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError, "gcd() arguments must be integers");
        return NULL;
    }
    res = MPZ_abs(arg);
    Py_DECREF(arg);
    if (!res) {
        return NULL;
    }
    for (Py_ssize_t i = 1; i < nargs; i++) {
        if (res->size != 1 || res->negative || res->digits[0] != 1) {
            if (MPZ_CheckExact(args[i])) {
                arg = MPZ_abs((MPZ_Object *)args[i]);
            }
            else if (PyLong_Check(args[i])) {
                tmp = MPZ_from_int(args[i]);
                if (!tmp) {
                    Py_DECREF(res);
                    return NULL;
                }
                arg = MPZ_abs(tmp);
                if (!arg) {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    return NULL;
                }
                Py_DECREF(tmp);
            }
            else {
                Py_DECREF(res);
                PyErr_SetString(PyExc_TypeError,
                                "gcd() arguments must be integers");
                return NULL;
            }
            if (!res->size) {
                Py_DECREF(res);
                res = MPZ_abs(arg);
                if (!res) {
                    Py_DECREF(arg);
                    return NULL;
                }
                Py_DECREF(arg);
                continue;
            }
            nzeros_res = mpn_scan1(res->digits, 0);
            if (nzeros_res) {
                mpn_rshift(res->digits, res->digits, res->size, nzeros_res);
            }
            if (!arg->size) {
                Py_DECREF(arg);
                continue;
            }
            nzeros_res = Py_MIN(nzeros_res, mpn_scan1(arg->digits, 0));
            if (nzeros_res) {
                mpn_rshift(arg->digits, arg->digits, arg->size, nzeros_res);
            }
            tmp = MPZ_copy(res);
            if (!tmp) {
                Py_DECREF(res);
                Py_DECREF(arg);
                return NULL;
            }

            mp_size_t newsize;

            if (tmp->size >= arg->size) {
                if (CHECK_NO_MEM_LEAK) {
                    newsize = mpn_gcd(res->digits, tmp->digits, tmp->size,
                                      arg->digits, arg->size);
                }
                else {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    Py_DECREF(arg);
                    return PyErr_NoMemory();
                }
            }
            else {
                if (CHECK_NO_MEM_LEAK) {
                    newsize = mpn_gcd(res->digits, arg->digits, arg->size,
                                      tmp->digits, tmp->size);
                }
                else {
                    Py_DECREF(tmp);
                    Py_DECREF(res);
                    Py_DECREF(arg);
                    return PyErr_NoMemory();
                }
            }
            Py_DECREF(arg);
            Py_DECREF(tmp);
            if (newsize != res->size) {
                mp_limb_t *tmp_limbs = res->digits;

                res->digits = PyMem_Resize(tmp_limbs, mp_limb_t, newsize);
                if (!res->digits) {
                    res->digits = tmp_limbs;
                    Py_DECREF(res);
                    return PyErr_NoMemory();
                }
                res->size = newsize;
            }
        }
    }
    if (nzeros_res) {
        mpn_lshift(res->digits, res->digits, res->size, nzeros_res);
    }
    return (PyObject *)res;
}

static PyObject *
gmp_isqrt(PyObject *Py_UNUSED(module), PyObject *arg)
{
    static MPZ_Object *x, *res = NULL;

    if (MPZ_CheckExact(arg)) {
        x = (MPZ_Object *)arg;
        Py_INCREF(x);
    }
    else if (PyLong_Check(arg)) {
        x = MPZ_from_int(arg);
        if (!x) {
            goto end;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "isqrt() argument must be an integer");
        return NULL;
    }
    if (x->negative) {
        PyErr_SetString(PyExc_ValueError,
                        "isqrt() argument must be nonnegative");
        goto end;
    }
    else if (!x->size) {
        res = MPZ_FromDigitSign(0, 0);
        goto end;
    }
    res = MPZ_new((x->size + 1)/2, 0);
    if (!res) {
        goto end;
    }
    if (CHECK_NO_MEM_LEAK) {
        mpn_sqrtrem(res->digits, NULL, x->digits, x->size);
    }
    else {
        Py_DECREF(res);
        Py_DECREF(x);
        return PyErr_NoMemory();
    }
end:
    Py_DECREF(x);
    return (PyObject *)res;
}

static PyObject *
gmp_factorial(PyObject *Py_UNUSED(module), PyObject *arg)
{
    static MPZ_Object *x, *res = NULL;

    if (MPZ_CheckExact(arg)) {
        x = (MPZ_Object *)arg;
        Py_INCREF(x);
    }
    else if (PyLong_Check(arg)) {
        x = MPZ_from_int(arg);
        if (!x) {
            goto end;
        }
    }
    else {
        PyErr_SetString(PyExc_TypeError,
                        "factorial() argument must be an integer");
        return NULL;
    }

    __mpz_struct tmp;

    tmp._mp_d = x->digits;
    tmp._mp_size = (x->negative ? -1 : 1) * x->size;
    if (!mpz_fits_ulong_p(&tmp)) {
        PyErr_Format(PyExc_OverflowError,
                     "factorial() argument should not exceed %ld", LONG_MAX);
        goto end;
    }
    if (x->negative) {
        PyErr_SetString(PyExc_ValueError,
                        "factorial() not defined for negative values");
        goto end;
    }

    unsigned long n = mpz_get_ui(&tmp);

    if (CHECK_NO_MEM_LEAK) {
        mpz_init(&tmp);
        mpz_fac_ui(&tmp, n);
    }
    else {
        Py_DECREF(x);
        return PyErr_NoMemory();
    }
    res = MPZ_new(tmp._mp_size, 0);
    if (!res) {
        mpz_clear(&tmp);
        goto end;
    }
    mpn_copyi(res->digits, tmp._mp_d, res->size);
    mpz_clear(&tmp);
end:
    Py_DECREF(x);
    return (PyObject *)res;
}

static PyMethodDef functions[] = {
    {"gcd", (PyCFunction)gmp_gcd, METH_FASTCALL,
     ("gcd($module, /, *integers)\n--\n\n"
      "Greatest Common Divisor.")},
    {"isqrt", gmp_isqrt, METH_O,
     ("isqrt($module, n, /)\n--\n\n"
      "Return the integer part of the square root of the input.")},
    {"factorial", gmp_factorial, METH_O,
     ("factorial($module, n, /)\n--\n\n"
      "Find n!.\n\nRaise a ValueError if x is negative or non-integral.")},
    {"_from_bytes", _from_bytes, METH_O, NULL},
    {NULL} /* sentinel */
};

static struct PyModuleDef gmp_module = {
    PyModuleDef_HEAD_INIT,
    "gmp",
    "Bindings to the GNU GMP for Python.",
    -1,
    functions,
};

PyDoc_STRVAR(gmp_info__doc__,
"gmp.gmplib_info\n\
\n\
A named tuple that holds information about GNU GMP\n\
and it's internal representation of integers.\n\
The attributes are read only.");

static PyStructSequence_Field gmp_info_fields[] = {
    {"bits_per_limb", "size of a limb in bits"},
    {"nail_bits", "number of bits left unused at the top of each limb"},
    {"sizeof_limb", "size in bytes of the C type used to represent a limb"},
    {"version", "the GNU GMP version"},
    {NULL}
};

static PyStructSequence_Desc gmp_info_desc = {
    "gmp.gmplib_info",
    gmp_info__doc__,
    gmp_info_fields,
    4
};

PyMODINIT_FUNC
PyInit_gmp(void)
{
    mp_set_memory_functions(gmp_allocate_function, gmp_reallocate_function,
                            gmp_free_function);
#if !defined(PYPY_VERSION)
    /* Query parameters of Python’s internal representation of integers. */
    const PyLongLayout *layout = PyLong_GetNativeLayout();

    int_digit_size = layout->digit_size;
    int_digits_order = layout->digits_order;
    int_bits_per_digit = layout->bits_per_digit;
    int_nails = int_digit_size*8 - int_bits_per_digit;
    int_endianness = layout->digit_endianness;
#endif
    PyObject *m = PyModule_Create(&gmp_module);

    if (PyModule_AddType(m, &MPZ_Type) < 0) {
        return NULL;
    }

    PyTypeObject *GMP_InfoType = PyStructSequence_NewType(&gmp_info_desc);

    if (!GMP_InfoType) {
        Py_DECREF(GMP_InfoType);
        return NULL;
    }

    PyObject* gmp_info = PyStructSequence_New(GMP_InfoType);

    if (gmp_info == NULL) {
        return NULL;
    }
    Py_DECREF(GMP_InfoType);
    PyStructSequence_SET_ITEM(gmp_info, 0, PyLong_FromLong(GMP_LIMB_BITS));
    PyStructSequence_SET_ITEM(gmp_info, 1, PyLong_FromLong(GMP_NAIL_BITS));
    PyStructSequence_SET_ITEM(gmp_info, 2, PyLong_FromLong(sizeof(mp_limb_t)));
    PyStructSequence_SET_ITEM(gmp_info, 3, PyUnicode_FromString(gmp_version));
    if (PyErr_Occurred()) {
        Py_DECREF(gmp_info);
        return NULL;
    }
    if (PyModule_AddObject(m, "gmp_info", gmp_info) < 0) {
        Py_DECREF(gmp_info);
        return NULL;
    }

    PyObject *ns = PyDict_New();

    if (!ns) {
        return NULL;
    }
    if (PyDict_SetItemString(ns, "gmp", m) < 0) {
        Py_DECREF(ns);
        return NULL;
    }

    PyObject *gmp_fractions = PyImport_ImportModule("gmp_fractions");

    if (!gmp_fractions) {
        Py_DECREF(ns);
        return NULL;
    }

    PyObject *mpq = PyObject_GetAttrString(gmp_fractions, "mpq");

    if (!mpq) {
        Py_DECREF(ns);
        Py_DECREF(gmp_fractions);
        return NULL;
    }

    PyObject *mname = PyUnicode_FromString("gmp");

    if (!mname) {
        Py_DECREF(ns);
        Py_DECREF(gmp_fractions);
        Py_DECREF(mpq);
    }
    if (PyObject_SetAttrString(mpq, "__module__", mname) < 0) {
        Py_DECREF(ns);
        Py_DECREF(gmp_fractions);
        Py_DECREF(mpq);
        Py_DECREF(mname);
    }
    Py_DECREF(mname);
    if (PyModule_AddType(m, (PyTypeObject *)mpq) < 0) {
        Py_DECREF(ns);
        Py_DECREF(gmp_fractions);
        Py_DECREF(mpq);
        return NULL;
    }
    Py_DECREF(gmp_fractions);
    Py_DECREF(mpq);

    PyObject *numbers = PyImport_ImportModule("numbers");

    if (!numbers) {
        Py_DECREF(ns);
        return NULL;
    }

    const char *str = ("numbers.Integral.register(gmp.mpz)\n"
                       "numbers.Rational.register(gmp.mpq)\n");

    if (PyDict_SetItemString(ns, "numbers", numbers) < 0) {
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return NULL;
    }

    PyObject *res = PyRun_String(str, Py_file_input, ns, ns);

    if (!res) {
        Py_DECREF(numbers);
        Py_DECREF(ns);
        return NULL;
    }
    Py_DECREF(res);

    PyObject *importlib = PyImport_ImportModule("importlib.metadata");

    if (!importlib) {
        Py_DECREF(ns);
        return NULL;
    }
    if (PyDict_SetItemString(ns, "importlib", importlib) < 0) {
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return NULL;
    }
    str = "gmp.__version__ = importlib.version('python-gmp')\n";
    res = PyRun_String(str, Py_file_input, ns, ns);
    if (!res) {
        Py_DECREF(ns);
        Py_DECREF(importlib);
        return NULL;
    }
    Py_DECREF(ns);
    Py_DECREF(importlib);
    Py_DECREF(res);
    from_bytes_func = PyObject_GetAttrString(m, "_from_bytes");
    Py_INCREF(from_bytes_func);
    return m;
}
