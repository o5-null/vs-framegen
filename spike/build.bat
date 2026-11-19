@echo off
setlocal
set ROOT=%~dp0
set SDK=%ROOT%..\ngxsdk
call "C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%ROOT%"
cl /nologo /std:c++17 /EHsc /O2 /MD /W3 /DNDEBUG ^
  /I"%SDK%\include" ^
  /Fe:spike.exe ^
  main.cpp gpu.cpp motion.cpp ^
  /link /SUBSYSTEM:CONSOLE ^
  /LIBPATH:"%SDK%\lib\Windows_x86_64\x64" ^
  nvsdk_ngx_d.lib d3d12.lib dxgi.lib d3dcompiler.lib advapi32.lib user32.lib shell32.lib ole32.lib version.lib
set RC=%ERRORLEVEL%
popd
exit /b %RC%
