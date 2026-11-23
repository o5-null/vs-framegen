@echo off
setlocal
call "C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0"
if not exist "..\bin" mkdir "..\bin"
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD /W3 /D_CRT_SECURE_NO_WARNINGS ^
  /I ..\third_party\nvof ^
  spike_nvof.cpp ^
  /Fe:..\bin\nvof_spike.exe ^
  d3d12.lib dxgi.lib
if errorlevel 1 exit /b 1
echo built ..\bin\nvof_spike.exe
