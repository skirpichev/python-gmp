import math

import pytest
from gmp import (
    _mpmath_create,
    _mpmath_normalize,
    double_fac,
    factorial,
    fib,
    gcd,
    gcdext,
    isqrt,
    isqrt_rem,
    mpz,
)
from hypothesis import example, given
from hypothesis.strategies import booleans, integers, sampled_from


@given(integers(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    r = math.isqrt(x)
    assert isqrt(mx) == isqrt(x) == r


@given(integers(min_value=0))
def test_isqrt_rem(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.sqrtrem_python(x)
    assert isqrt_rem(mx) == isqrt_rem(x) == r


@given(integers(min_value=0, max_value=12345))
def test_double_fac(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.ifac2_python(x)
    assert double_fac(mx) == double_fac(x) == r


@given(integers(min_value=0, max_value=12345))
def test_fib(x):
    mpmath = pytest.importorskip("mpmath")
    mx = mpz(x)
    r = mpmath.libmp.libintmath.ifib_python(x)
    assert fib(mx) == fib(x) == r


@given(integers(min_value=0, max_value=12345))
def test_factorial(x):
    mx = mpz(x)
    r = math.factorial(x)
    assert factorial(mx) == factorial(x) == r


@given(integers(), integers())
@example(1<<(67*2), 1<<65)
def test_gcd(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r


def python_gcdext(a, b):
    if not a and not b:
        return 0, 0, 0
    if not a:
        return 0, b//abs(b), abs(b)
    if not b:
        return a//abs(a), 0, abs(a)
    if a < 0:
        a, x_sign = -a, -1
    else:
        x_sign = 1
    if b < 0:
        b, y_sign = -b, -1
    else:
        y_sign = 1
    x, y, r, s = 1, 0, 0, 1
    while b:
        c, q = a % b, a // b
        a, b, r, s, x, y = b, c, x - q*r, y - q*s, r, s
    return x*x_sign, y*y_sign, a


@given(integers(), integers())
@example(1<<(67*2), 1<<65)
def test_gcdext(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = python_gcdext(x, y)
    r = r[2], r[0], r[1]
    assert gcdext(mx, my) == r
    assert gcdext(x, my) == r
    assert gcdext(mx, y) == r


def test_interfaces():
    assert gcd() == 0
    with pytest.raises(TypeError):
        gcd(1j)
    with pytest.raises(TypeError):
        gcd(1, 1j)
    with pytest.raises(TypeError):
        isqrt(1j)
    with pytest.raises(ValueError):
        isqrt(-1)
    with pytest.raises(TypeError):
        factorial(1j)
    with pytest.raises(ValueError):
        factorial(-1)
    with pytest.raises(OverflowError):
        factorial(2**1000)


@given(booleans(), integers(min_value=0), integers(),
       integers(min_value=1, max_value=1<<30),
       sampled_from(["n", "f", "c", "u", "d"]))
@example(0, 232, -4, 4, "n")
@example(0, 9727076909039105, -48, 53, "n")
def test__mpmath_normalize(sign, man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    sign = int(sign)
    bc = mman.bit_length()
    res = mpmath.libmp.libmpf._normalize(sign, mman, exp, bc, prec, rnd)
    assert _mpmath_normalize(sign, mman, exp, bc, prec, rnd) == res


@given(integers(), integers(),
       integers(min_value=0, max_value=1<<30),
       sampled_from(["n", "f", "c", "u", "d"]))
def test__mpmath_create(man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    res = mpmath.libmp.from_man_exp(man, exp, prec, rnd)
    assert _mpmath_create(mman, exp, prec, rnd) == res
