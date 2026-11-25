#!/bin/bash
set -e

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH

cd /e/code/dlssg-vfi/mpv-src/build-msys
./mpv.com --version
echo "--- filters ---"
./mpv.com --vf=help 2>&1 | grep -i playkit || true
