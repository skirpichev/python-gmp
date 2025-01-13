import random
import resource

from gmp import factorial, mpz


def test_factorial_outofmemory(x):
    soft, hard = resource.getrlimit(resource.RLIMIT_AS)
    resource.setrlimit(resource.RLIMIT_AS, (1024*64*1024, hard))
    x = mpz(x)
    while True:
        try:
            factorial(x)
            x *= 2
        except MemoryError:
            break
    resource.setrlimit(resource.RLIMIT_AS, (soft, hard))


def bug():
    for _ in range(10000):
        a = random.randint(12811, 24984)
        test_factorial_outofmemory(a)


bug()
