# FIXME: see https://github.com/pypa/setuptools/issues/3025
import os
import platform

from setuptools import Extension, setup


ON_WINDOWS = platform.system() == "Windows"
if os.getenv("CIBUILDWHEEL"):
    include_dirs = [os.path.join(os.path.dirname(__file__), ".local",
                                 "include")]
    library_dirs = [os.path.join(os.path.dirname(__file__), ".local",
                                 "bin" if ON_WINDOWS else "lib")]
else:
    include_dirs = []
    library_dirs = []

setup(ext_modules=[Extension("gmp", sources=["main.c"], libraries=["gmp"],
                             include_dirs=include_dirs,
                             library_dirs=library_dirs)])
