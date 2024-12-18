import pytest
from gmp import mpq, mpz
from hypothesis import given
from hypothesis.strategies import fractions


@given(fractions(), fractions())
def test_add(x, y):
    qx = mpq(x)
    assert isinstance(qx.numerator, mpz)
    assert isinstance(qx.denominator, mpz)
    qy = mpq(y)
    r = x + y
    qr = qx + qy
    assert qr == r
    assert qx + y == r
    assert x + qy == r
    assert isinstance(qr.numerator, mpz)
    assert isinstance(qr.denominator, mpz)


@given(fractions(), fractions())
def test_sub(x, y):
    qx = mpq(x)
    assert isinstance(qx.numerator, mpz)
    assert isinstance(qx.denominator, mpz)
    qy = mpq(y)
    r = x - y
    qr = qx - qy
    assert qr == r
    assert qx - y == r
    assert x - qy == r
    assert isinstance(qr.numerator, mpz)
    assert isinstance(qr.denominator, mpz)


@given(fractions(), fractions())
def test_mul(x, y):
    qx = mpq(x)
    assert isinstance(qx.numerator, mpz)
    assert isinstance(qx.denominator, mpz)
    qy = mpq(y)
    r = x * y
    qr = qx * qy
    assert qr == r
    assert qx * y == r
    assert x * qy == r
    assert isinstance(qr.numerator, mpz)
    assert isinstance(qr.denominator, mpz)


@given(fractions(), fractions())
def test_div(x, y):
    qx = mpq(x)
    assert isinstance(qx.numerator, mpz)
    assert isinstance(qx.denominator, mpz)
    qy = mpq(y)
    if not qy:
        with pytest.raises(ZeroDivisionError):
            qx / qy
        return
    r = x / y
    qr = qx / qy
    assert qr == r
    assert qx / y == r
    assert x / qy == r
    assert isinstance(qr.numerator, mpz)
    assert isinstance(qr.denominator, mpz)
