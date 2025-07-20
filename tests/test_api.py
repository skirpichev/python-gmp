import platform
from ctypes import (
    CDLL,
    POINTER,
    Structure,
    byref,
    c_bool,
    c_int8,
    c_long,
    c_uint8,
    c_uint64,
    c_ulong,
)
from enum import IntEnum

import pytest

if platform.system() != "Linux":
    pytest.skip("FIXME: find library!", allow_module_level=True)

class zz_t_struct(Structure):
    _fields_ = [("negative", c_bool),
                ("alloc", c_long),
                ("size", c_long),
                ("digits", POINTER(c_ulong))]

class mp_layout(Structure):
    _fields_ = [("bits_per_digit", c_uint8),
                ("digit_size", c_uint8),
                ("digits_order", c_int8),
                ("digit_endianness", c_int8)]

class mp_err(IntEnum):
    MP_OK = 0
    MP_MEM = -1
    MP_VAL = -2
    MP_BUF = -3

class mp_ord(IntEnum):
    MP_GT = +1
    MP_EQ = 0
    MP_LT = -1

class mp_rnd(IntEnum):
    MP_RNDD = 0
    MP_RNDN = 1

libzz = CDLL("libzz.so")
zz_from_i64 = libzz.zz_from_i64
zz_cmp_i32 = libzz.zz_cmp_i32
zz_add_i32 = libzz.zz_add_i32
zz_lsbpos = libzz.zz_lsbpos
zz_export = libzz.zz_export
zz_mul = libzz.zz_mul
zz_div = libzz.zz_div
zz_rem_u64 = libzz.zz_rem_u64
zz_pow = libzz.zz_pow
zz_powm = libzz.zz_powm
zz_sqrtrem = libzz.zz_sqrtrem

u, v, w = map(byref, map(zz_t_struct, [[]]*3))
int_layout = mp_layout(30, 4, -1, -1)


def test_zz_cmp_i32():
    assert zz_from_i64(13, u) == mp_err.MP_OK
    assert zz_cmp_i32(u, 1) == mp_ord.MP_GT
    assert zz_cmp_i32(u, 100) == mp_ord.MP_LT


def test_zz_add_i32():
    assert zz_from_i64(0, u) == mp_err.MP_OK
    assert zz_add_i32(u, 2, u) == mp_err.MP_OK
    assert zz_cmp_i32(u, 2) == mp_ord.MP_EQ


def test_zz_lsbpos():
    assert zz_from_i64(0, u) == mp_err.MP_OK
    assert zz_lsbpos(u, 0) == 0


@pytest.mark.skipif(platform.python_implementation() == "GraalVM",
                    reason=("NotImplementedError: Passing structs "
                            "by value is not supported on NFI backend"))
def test_zz_export():
    assert zz_from_i64(123, u) == mp_err.MP_OK
    assert zz_export(u, int_layout, 0, 0) == mp_err.MP_VAL


def test_zz_mul():
    assert zz_from_i64(2, u) == mp_err.MP_OK
    assert zz_from_i64(3, v) == mp_err.MP_OK
    assert zz_mul(u, v, u) == mp_err.MP_OK
    assert zz_cmp_i32(u, 6) == mp_ord.MP_EQ
    assert zz_mul(u, v, v) == mp_err.MP_OK
    assert zz_cmp_i32(v, 18) == mp_ord.MP_EQ


def test_zz_div():
    assert zz_from_i64(4, u) == mp_err.MP_OK
    assert zz_from_i64(2, v) == mp_err.MP_OK
    assert zz_div(u, v, mp_rnd.MP_RNDD, v, 0) == mp_err.MP_OK
    assert zz_cmp_i32(v, 2) == mp_ord.MP_EQ
    assert zz_div(u, v, 123, u, 0) == mp_err.MP_VAL


def test_zz_rem_u64():
    assert zz_from_i64(123, u) == mp_err.MP_OK
    p = c_uint64(0)
    assert zz_rem_u64(u, 0, byref(p)) == mp_err.MP_VAL
    assert zz_from_i64(111, u) == mp_err.MP_OK
    assert zz_rem_u64(u, 12, byref(p)) == mp_err.MP_OK
    assert p.value == 3
    assert zz_from_i64(-111, u) == mp_err.MP_OK
    assert zz_rem_u64(u, 12, byref(p)) == mp_err.MP_OK
    assert p.value == 9


def test_zz_pow():
    assert zz_from_i64(2, u) == mp_err.MP_OK
    assert zz_pow(u, 2, u) == mp_err.MP_OK
    assert zz_cmp_i32(u, 4) == mp_ord.MP_EQ


def test_zz_sqrtrem():
    assert zz_from_i64(4, u) == mp_err.MP_OK
    assert zz_from_i64(0, v) == mp_err.MP_OK
    assert zz_sqrtrem(u, u, v) == mp_err.MP_OK
    assert zz_cmp_i32(u, 2) == mp_ord.MP_EQ
    assert zz_cmp_i32(v, 0) == mp_ord.MP_EQ

def test_zz_powm():
    assert zz_from_i64(12, u) == mp_err.MP_OK
    assert zz_from_i64(4, v) == mp_err.MP_OK
    assert zz_from_i64(7, w) == mp_err.MP_OK
    assert zz_powm(u, v, w, u) == mp_err.MP_OK
    assert zz_cmp_i32(u, 2) == mp_ord.MP_EQ
    assert zz_from_i64(12, u) == mp_err.MP_OK
    assert zz_powm(u, v, w, v) == mp_err.MP_OK
    assert zz_cmp_i32(v, 2) == mp_ord.MP_EQ
    assert zz_from_i64(4, v) == mp_err.MP_OK
    assert zz_powm(u, v, w, w) == mp_err.MP_OK
    assert zz_cmp_i32(w, 2) == mp_ord.MP_EQ
