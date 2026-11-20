@echo off
rem Full pipeline run: decode, interpolate, discard. Writes stderr to run.log.
set RIFE_TRACE=
"E:\code\VFI\.venv\Scripts\vspipe.exe" -p "%~dp0test.vpy" - > nul 2> "%~dp0run.log"
echo EXIT=%errorlevel% >> "%~dp0run.log"
type "%~dp0run.log"
