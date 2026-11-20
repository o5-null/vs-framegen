@echo off
rem Fast syntax-only pass over every translation unit (no link, no SASS): catches includes,
rem typos and API mismatches before the real build.
setlocal
set ROOT=%~dp0
if "%CUDA_PATH%"=="" set CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.2
if "%VS_INCLUDE%"=="" set VS_INCLUDE=E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include
if "%VCVARS%"=="" set VCVARS=C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat
call "%VCVARS%" >nul
pushd "%ROOT%"

set INC=/I"%VS_INCLUDE%" /I"%CUDA_PATH%\include" /I"."
set CLX=cl /nologo /std:c++17 /utf-8 /EHsc /MD /W3 /Zs %INC%
set NVCC=nvcc -std=c++17 -ptx -o nul -I"%VS_INCLUDE%" -I"%CUDA_PATH%\include" -I"."

echo [check] host
%CLX% plugin.cpp engine.cpp native\rife.cpp native\rife_plan.cpp native\pack.cpp native\onnx_graph.cpp || goto bad
echo [check] cuda
%NVCC% convert.cu || goto bad
%NVCC% native\rife_kernels.cu || goto bad
%NVCC% native\gconv.cu || goto bad
popd
echo [check] ok
exit /b 0

:bad
echo [check] failed
popd
exit /b 1
