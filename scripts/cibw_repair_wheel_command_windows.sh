#!/bin/bash

set -e -x

wheel=$WHEELNAME
dest_dir=$WHEELHOUSE

delvewheel repair ${wheel} -w ${dest_dir} \
    --add-path .local/bin --no-mangle-all --include-imports
