#!/bin/bash
set -e
export MSYSTEM=CLANG64
export PATH=/clang64/bin:/usr/bin:/bin:$PATH
cd /e/code/dlssg-vfi/mpv-src/build-msys
./mpv.exe --no-config --ao=null --vo=gpu --gpu-context=d3d11 --d3d11-adapter=#1 \
  --hwdec=d3d11va \
  --vf=playkit=multi=6:log:dll=dlss45/vdlssg.dll:proxy_dir=dlss45 \
  --frames=60 --log-file=playkit_test.log \
  'E:/code/dlssg-vfi/测试数据/ah_1080p10.mkv'
