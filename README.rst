|python versions| |supported implementations| |CI status| |latest docs|

.. |python versions| image:: https://img.shields.io/pypi/pyversions/python-gmp
   :alt: PyPI - Python Version
.. |supported implementations| image:: https://img.shields.io/pypi/implementation/python-gmp
   :alt: PyPI - Implementation
.. |CI status| image:: https://img.shields.io/github/actions/workflow/status/diofant/python-gmp/.github%2Fworkflows%2Ftest.yml
   :alt: GitHub Actions Workflow Status
.. |latest docs| image:: https://img.shields.io/readthedocs/python-gmp/latest
   :alt: Read the Docs (version)

Python-GMP
==========

Python extension module providing safe bindings to the GNU GMP.  This module
shouldn't crash the interpreter!

It can be used as a gmpy2/python-flint replacement to provide
CPython-compatible integer (mpz) and rational (not ready yet) types.  The
module includes also few functions (factorial, gcd and isqrt), compatible with
stdlib's module math.


Motivation
----------

The CPython (and most other Python implementations, like PyPy) is optimized to
work with small integers.  Algorithms used here for "big enough" integers
usually aren't best known in the field.  Fortunately, it's possible to use
bindings (for example, the `gmpy2 <https://pypi.org/project/gmpy2/>`_ package)
to the GNU Multiple Precision Arithmetic Library (GMP), which aims to be
faster than any other bignum library for all operand sizes.

But such extension modules usually rely on default GMP's memory allocation
functions and can't recover from errors such as out of memory.  So, e.g. it's
easy to crash the Python interpreter during the interactive session.  Above
example with the gmpy2 should work on most Unix systems:

.. code:: pycon

   >>> import gmpy2, resource
   >>> gmpy2.__version__
   '2.2.1'
   >>> resource.setrlimit(resource.RLIMIT_AS, (1024*32*1024, -1))
   >>> z = gmpy2.mpz(29925959575501)
   >>> while True:  # this loop will crash interpter
   ...     z = z*z
   ...
   GNU MP: Cannot allocate memory (size=2949160)
   Aborted

The gmp module handles such errors correctly:

.. code:: pycon

   >>> z = gmp.mpz(29925959575501)
   >>> while True:
   ...     z = z*z
   ...
   Traceback (most recent call last):
     File "<python-input-3>", line 2, in <module>
       z = z*z
           ~^~
   MemoryError
   >>> # interpreter still works, all variables in
   >>> # the current scope are available,
   >>> z.bit_length()  # including pre-failure value of z
   mpz(5867630)
