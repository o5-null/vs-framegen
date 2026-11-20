@echo off
rem Builds rife_native.dll: the native RIFE CUDA kernels (vendored from the-database/
rem animejanai-inference) behind a VapourSynth 4 filter.
rem Needs MSVC x64, the CUDA toolkit (nvcc 13.x) and the VapourSynth R74+ headers.
rem Override with env: CUDA_PATH, VS_INCLUDE, VCVARS.
setlocal
set ROOT=%~dp0

if "%CUDA_PATH%"=="" set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
if "%VS_INCLUDE%"=="" set VS_INCLUDE=E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include
if "%VCVARS%"=="" set VCVARS=C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" goto no_vcvars
if not exist "%VS_INCLUDE%\VapourSynth4.h" goto no_vs
if not exist "%CUDA_PATH%\bin\nvcc.exe" goto no_cuda

call "%VCVARS%" >nul
if not exist "%ROOT%..\bin" mkdir "%ROOT%..\bin"
if not exist "%ROOT%..\obj" mkdir "%ROOT%..\obj"
pushd "%ROOT%"

set INC=/I"%VS_INCLUDE%" /I"%CUDA_PATH%\include" /I"."
rem sm_80 SASS plus compute_80 PTX: Ampere and later run the SASS, newer architectures JIT the PTX.
set NVCC=nvcc -std=c++17 -O3 -gencode arch=compute_80,code=[sm_80,compute_80] -Xcompiler "/MD /EHsc /O2 /utf-8 /W3" -DNDEBUG -I"%VS_INCLUDE%" -I"."
set CLX=cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD /W3 /DNDEBUG %INC%

echo [build] CUDA kernels
%NVCC% -c -o "..\obj\rife_kernels.obj" "native\rife_kernels.cu" || goto build_failed
%NVCC% -c -o "..\obj\gconv.obj" "native\gconv.cu" || goto build_failed
%NVCC% -c -o "..\obj\convert.obj" "convert.cu" || goto build_failed

echo [build] host code
%CLX% /c "/Fo..\obj\\" plugin.cpp engine.cpp native\rife.cpp native\rife_plan.cpp native\pack.cpp native\onnx_graph.cpp || goto build_failed

echo [build] link
cl /nologo /LD /Fe:"..\bin\rife_native.dll" "..\obj\rife_kernels.obj" "..\obj\gconv.obj" ^
   "..\obj\convert.obj" "..\obj\plugin.obj" "..\obj\engine.obj" "..\obj\rife.obj" ^
   "..\obj\rife_plan.obj" "..\obj\pack.obj" "..\obj\onnx_graph.obj" ^
   /link /SUBSYSTEM:WINDOWS /LIBPATH:"%CUDA_PATH%\lib\x64" cudart.lib || goto build_failed

popd
echo [build] ok: %ROOT%..\bin\rife_native.dll
exit /b 0

:build_failed
echo [build] failed
popd
exit /b 1

:no_vcvars
echo [build] vcvars64.bat not found, set VCVARS. Tried: %VCVARS%
exit /b 1

:no_vs
echo [build] VapourSynth headers not found, set VS_INCLUDE. Tried: %VS_INCLUDE%
exit /b 1

:no_cuda
echo [build] nvcc not found, set CUDA_PATH. Tried: %CUDA_PATH%
exit /b 1
