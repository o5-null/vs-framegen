#!/bin/bash
set -e

export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH

cd /e/code/dlssg-vfi/mpv-src/build-msys

./mpv.com "/e/code/dlssg-vfi/测试数据/阿哈角色PV 1080p.mkv" \
  --no-config --no-audio \
  --hwdec=d3d11va \
  --vo=gpu \
  --gpu-context=d3d11 \
  --gpu-api=d3d11 \
  --vf=playkit=multi=6:log=yes \
  --frames=18 \
  --msg-level=all=info
