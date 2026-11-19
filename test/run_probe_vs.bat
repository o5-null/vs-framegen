@echo off
cd /d E:\code\dlssg-vfi
E:\code\VFI\.venv\Scripts\python.exe test\probe_vs_inproc.py > test\probe_vs_inproc.out 2>&1
type test\probe_vs_inproc.out
