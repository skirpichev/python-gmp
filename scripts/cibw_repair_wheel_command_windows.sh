#!/bin/bash

set -e -x

wheel=$WHEELNAME
dest_dir=$WHEELHOUSE

delvewheel repair ${wheel} -w ${dest_dir} --add-path .local/bin --no-mangle-all

cp .local/bin/gmp.lib ${dest_dir}
(cd ${dest_dir}; wheel unpack --dest . python-gmp-*.whl; mv *.lib python-gmp-*/gmpy2.libs; wheel pack python-gmp-*[0-9]; rm -rf python-gmp-*[0-9])
