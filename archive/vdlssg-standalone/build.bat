@echo off
setlocal
set ROOT=%~dp0
set SDK=%ROOT%..\ngxsdk
set VSINCLUDE=E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include
call "C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat" >nul
pushd "%ROOT%"
cl /nologo /std:c++17 /EHsc /O2 /MD /arch:AVX2 /W3 /DNDEBUG /I"%SDK%\include" /I"%VSINCLUDE%" ^
   /Fe:vdlssg.dll ^
   vdlssg.cpp ngx_session.cpp d3d12_ctx.cpp ^
   /link /DLL /SUBSYSTEM:WINDOWS ^
   /LIBPATH:"%SDK%\lib\Windows_x86_64\x64" ^
   nvsdk_ngx_d.lib d3d12.lib dxgi.lib d3dcompiler.lib ^
   advapi32.lib user32.lib shell32.lib ole32.lib version.lib
popd
