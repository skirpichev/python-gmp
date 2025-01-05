import math
import platform

import pytest
from gmp import _mpmath_normalize, factorial, gcd, isqrt, mpz
from hypothesis import given
from hypothesis.strategies import booleans, integers, sampled_from


try:
    import resource
except ImportError:
    pass


@given(integers(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    r = math.isqrt(x)
    assert isqrt(mx) == isqrt(x) == r


@given(integers(min_value=0, max_value=12345))
def test_factorial(x):
    mx = mpz(x)
    r = math.factorial(x)
    assert factorial(mx) == factorial(x) == r


@pytest.mark.skipif(platform.system() != "Linux",
                    reason="FIXME: setrlimit fails with ValueError on MacOS")
@pytest.mark.skipif(platform.python_implementation() == "PyPy",
                    reason="XXX: bug in PyNumber_ToBase()?")
@given(integers(min_value=12811, max_value=24984))
def test_factorial_outofmemory(x):
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, hard))
    a = mpz(x)
    while True:
        try:
            factorial(a)
            a *= 2
        except MemoryError:
            break
    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


@given(integers(), integers())
def test_gcd(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r


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
       integers(min_value=1, max_value=1<<32),
       sampled_from(["n", "f", "c", "u", "d"]))
def test__mpmath_normalize(sign, man, exp, prec, rnd):
    mpmath = pytest.importorskip("mpmath")
    mman = mpz(man)
    sign = int(sign)
    bc = mman.bit_length()
    res = mpmath.libmp.libmpf._normalize(sign, mman, exp, bc, prec, rnd)
    assert _mpmath_normalize(sign, mman, exp, bc, prec, rnd) == res
