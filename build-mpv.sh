#!/bin/bash
# Local MSYS2 CLANG64 build for the forked mpv with vf_playkit.
set -e

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH
export PKG_CONFIG_PATH=/clang64/lib/pkgconfig:/clang64/share/pkgconfig

cd /e/code/dlssg-vfi/mpv-src

if [ ! -d build-msys ]; then
  meson setup build-msys \
    --prefix=/clang64 \
    -Dlibmpv=true \
    -Dd3d11=enabled \
    -Dvulkan=enabled \
    -Dshaderc=enabled
fi

meson compile -C build-msys
