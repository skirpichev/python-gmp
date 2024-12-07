import math

from hypothesis import given
from hypothesis.strategies import integers

from gmp import mpz, gcd, isqrt


@given(integers(min_value=0))
def test_isqrt(x):
    mx = mpz(x)
    assert isqrt(mx) == isqrt(x) == math.isqrt(x)


@given(integers(), integers())
def test_gcd(x, y):
    mx = mpz(x)
    my = mpz(y)
    r = math.gcd(x, y)
    assert gcd(mx, my) == r
    assert gcd(x, my) == r
    assert gcd(mx, y) == r
