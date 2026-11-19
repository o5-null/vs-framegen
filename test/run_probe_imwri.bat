@echo off
cd /d E:\code\dlssg-vfi
E:\code\VFI\.venv\Scripts\python.exe test\probe_imwri.py > test\probe_imwri.out 2>&1
type test\probe_imwri.out
