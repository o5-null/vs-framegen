#!/bin/bash
# Local MSYS2 CLANG64 build for the forked mpv with vf_playkit.
#
# vf_playkit's frame generation runs through vf vapoursynth, so the
# VapourSynth bridge must be part of the build. It is a meson feature option
# that defaults to 'auto'; the option is pinned to 'enabled' below so the
# configure step fails loudly instead of silently producing an mpv without
# vapoursynth support.
#
# NOTE: build-msys also holds manually placed playkit artifacts (dlssg2f.exe,
# vdlssg.dll, nvngx_dlssg.dll, ...) that are NOT ninja outputs. Never run
# `meson setup --wipe` here: reconfigure in place instead.
set -e

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH
export PKG_CONFIG_PATH=/clang64/lib/pkgconfig:/clang64/share/pkgconfig

cd /e/code/dlssg-vfi/repos/mpv-src

MESON_ARGS=(
  --prefix=/clang64
  -Dlibmpv=true
  -Dd3d11=enabled
  -Dvulkan=enabled
  -Dshaderc=enabled
  -Dvapoursynth=enabled
)

if [ ! -f build-msys/build.ninja ]; then
  meson setup build-msys "${MESON_ARGS[@]}"
else
  # Re-runs dependency detection, so newly installed packages (e.g. vapoursynth)
  # are picked up without throwing away the object files.
  meson setup build-msys --reconfigure "${MESON_ARGS[@]}"
fi

meson compile -C build-msys
