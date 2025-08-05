import pyperf
from gmp import mpz

#from gmpy2 import mpz
#mpz = int

three = mpz(3)

def collatz(n):
    # https://en.wikipedia.org/wiki/Collatz_conjecture
    total = 0
    n = mpz(n)
    while n > 1:
        n = n*three + 1 if n & 1 else n//2
        total += 1
    return total

runner = pyperf.Runner()
for i in [97, 871]:
    h = f"collatz({i})"
    runner.bench_func(h, collatz, i)
