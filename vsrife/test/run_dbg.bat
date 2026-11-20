@echo off
set RIFE_TRACE=1
"E:\code\VFI\.venv\Scripts\vspipe.exe" -i "%~dp0debug.vpy" > "%~dp0dbg2.log" 2>&1
echo EXIT=%errorlevel% >> "%~dp0dbg2.log"
