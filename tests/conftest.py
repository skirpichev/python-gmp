import os
import platform
from datetime import timedelta

import gmp
from hypothesis import settings

default = settings.get_profile("default")
settings.register_profile("default",
                          settings(default,
                                   deadline=timedelta(seconds=300)))
ci = settings.get_profile("ci")
if platform.python_implementation() != "GraalVM":
    ci = settings(ci, max_examples=10000)
else:
    ci = settings(ci, max_examples=1000)
settings.register_profile("ci", ci)


def pytest_report_header(config):
    print(f"""
  Using the ZZ library v{gmp._zz_version}

  Bits per digit   :      {gmp.mpz_info.bits_per_digit}
  sizeof(zz_limb_t):      {gmp.mpz_info.sizeof_digit}
  Maximal bit count:      {gmp.mpz_info.bitcnt_max}
""")


def pytest_sessionstart(session):
    os.environ["MPMATH_NOGMPY"] = "Y"
