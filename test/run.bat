@echo off
cd /d E:\code\dlssg-vfi
E:\code\VFI\.venv\Scripts\python.exe test\%1.py > test\%1.out 2>&1
echo RC=%ERRORLEVEL%
