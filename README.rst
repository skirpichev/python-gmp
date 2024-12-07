Python-GMP
==========

Python extension module providing safe bindings to the GNU GMP.  This module
shouldn't crash the interpreter!

Can be used as a gmpy2/python-flint replacement to provide CPython-compatible
integer and rational types:

* mpz — like builtin int int type
* mpq — like stdlib's Fraction type

Module includes also few functions:

* gcd — greatest common division, just like math.gcd
* isqrt — integer square root, just like math.isqrt
