@echo off
rem host_probe: link the RIFE engine outside VapourSynth to see where it dies.
rem Produces ..\bin\host_probe.exe from ..\obj (plugin.obj excluded).
setlocal
set "HERE=%~dp0"
set "SRC=%HERE%..\src"
set "OBJ=%HERE%..\obj"
set "BIN=%HERE%..\bin"
if "%CUDA_PATH%"=="" set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2"
set "VCVARS=C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul

cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD /W3 /DNDEBUG /I"%SRC%" /I"%CUDA_PATH%\include" ^
   "%HERE%host_probe.cpp" ^
   "%OBJ%\rife.obj" "%OBJ%\rife_plan.obj" "%OBJ%\rife_kernels.obj" "%OBJ%\gconv.obj" ^
   "%OBJ%\pack.obj" "%OBJ%\onnx_graph.obj" ^
   /Fe:"%BIN%\host_probe.exe" /link /SUBSYSTEM:CONSOLE /LIBPATH:"%CUDA_PATH%\lib\x64" cudart.lib
if errorlevel 1 exit /b 1
echo [probe] built %BIN%\host_probe.exe
