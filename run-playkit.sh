#!/bin/bash
set -e

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH

cd /e/code/dlssg-vfi/repos/mpv-src/build-msys

./mpv.com "/e/code/dlssg-vfi/娴嬭瘯鏁版嵁/闃垮搱瑙掕壊PV 1080p.mkv" \
  --no-config --no-audio \
  --hwdec=d3d11va \
  --vo=gpu \
  --gpu-context=d3d11 \
  --gpu-api=d3d11 \
  --vf=playkit=multi=6:log=yes \
  --frames=18 \
  --msg-level=all=info
