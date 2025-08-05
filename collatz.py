import pyperf
from gmp import mpz

#from gmpy2 import mpz
#mpz = int

zero = mpz(0)
one = mpz(1)
two = mpz(2)
three = mpz(3)

def collatz(n):
    # https://en.wikipedia.org/wiki/Collatz_conjecture
    total = 0
    n = mpz(n)
    while n > 1:
        n = n*three + one if n & one else n//two
        total += 1
    return total

runner = pyperf.Runner()
for i in [97, 871]:
    h = f"collatz({i})"
    runner.bench_func(h, collatz, i)
