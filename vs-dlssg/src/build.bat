@echo off
rem Build one DLL per engine: every plugin is a single engine fixed at compile time (profiles.h),
rem so a package never carries a model it cannot run. Needs MSVC x64, NGX SDK, VapourSynth R74+.
rem Override with env: NGX_SDK, VS_INCLUDE, VCVARS.
setlocal
set ROOT=%~dp0

if "%NGX_SDK%"=="" set NGX_SDK=%ROOT%..\..\ngxsdk
if "%VS_INCLUDE%"=="" set VS_INCLUDE=E:\code\VFI\.venv\Lib\site-packages\vapoursynth\include
if "%VCVARS%"=="" set VCVARS=C:\software\msvc\VC\Auxiliary\Build\vcvars64.bat
if "%VULKAN_SDK%"=="" set VULKAN_SDK=C:\software\Vulkan-sdk

if not exist "%NGX_SDK%\include\nvsdk_ngx.h" goto no_ngx
if not exist "%VS_INCLUDE%\VapourSynth4.h" goto no_vs
if not exist "%VCVARS%" goto no_vcvars

call "%VCVARS%" >nul
if not exist "%ROOT%..\bin" mkdir "%ROOT%..\bin"
if not exist "%ROOT%..\obj" mkdir "%ROOT%..\obj"
pushd "%ROOT%"

rem name=profile pairs; the profile macro is what selects the engine. Each engine links only the
rem translation units it needs (the NGX ones pull in D3D12; the VFG one is CUDA-only).
call :build_one vdlssg FG_PLUGIN_VDLSSG "frame_gen.cpp ngx_session.cpp d3d12_ctx.cpp" || goto build_failed
call :build_one vxessfg FG_PLUGIN_VXESSFG "frame_gen.cpp ngx_session.cpp d3d12_ctx.cpp" || goto build_failed
call :build_one vvfg FG_PLUGIN_VVFG "frame_gen.cpp vfg_session.cpp" || goto build_failed
rem FSR4 drives smv_fsrfg_bridge.dll over Vulkan: the bridge hands out its shared textures as raw
rem Vulkan exports (the bundled vkd3d-proton is patched), so the host imports them with Vulkan.
if not exist "%VULKAN_SDK%\Include\vulkan\vulkan.h" goto no_vulkan
call :build_one vfsr4 FG_PLUGIN_VFSR4 "frame_gen.cpp fsr4_session.cpp fsr4_gpu.cpp fsr4_vk.cpp fsr4_vk_device.cpp" "%VULKAN_SDK%\Include" "%VULKAN_SDK%\Lib" || goto build_failed

popd
exit /b 0

:build_one
setlocal
set NAME=%~1
set PROFILE=%~2
set SOURCES=%~3
set EXTRA_INC=%~4
set EXTRA_LIBDIR=%~5
set INC_ARG=
set LIB_ARG=
if not "%EXTRA_INC%"=="" set INC_ARG=/I"%EXTRA_INC%"
if not "%EXTRA_LIBDIR%"=="" set LIB_ARG=/LIBPATH:"%EXTRA_LIBDIR%" vulkan-1.lib
echo [build] %NAME%
if not exist "..\obj\%NAME%" mkdir "..\obj\%NAME%"
cl /nologo /std:c++17 /utf-8 /EHsc /O2 /MD /arch:AVX2 /W3 /DNDEBUG /D%PROFILE% /DVS_USE_LATEST_API /I"%NGX_SDK%\include" /I"%VS_INCLUDE%" %INC_ARG% ^
   /Fo:..\obj\%NAME%\ /Fe:..\bin\%NAME%.dll ^
   %SOURCES% ^
   /link /DLL /SUBSYSTEM:WINDOWS ^
   /LIBPATH:"%NGX_SDK%\lib\Windows_x86_64\x64" ^
   nvsdk_ngx_d.lib d3d12.lib dxgi.lib d3dcompiler.lib %LIB_ARG% ^
   advapi32.lib user32.lib shell32.lib ole32.lib version.lib
endlocal & exit /b %errorlevel%

:build_failed
echo [build] failed
popd
exit /b 1

:no_ngx
echo [build] NGX SDK not found, set NGX_SDK. Tried: %NGX_SDK%
exit /b 1

:no_vs
echo [build] VapourSynth headers not found, set VS_INCLUDE. Tried: %VS_INCLUDE%
exit /b 1

:no_vcvars
echo [build] vcvars64.bat not found, set VCVARS. Tried: %VCVARS%
exit /b 1

:no_vulkan
echo [build] Vulkan headers not found, set VULKAN_SDK. Tried: %VULKAN_SDK%
exit /b 1
