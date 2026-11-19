@echo off
rem Extract 1080p rgb24 frames as triplets (a = n, gt = n+1, b = n+2).
setlocal
set ROOT=%~dp0
set IN=%ROOT%sintel_trailer_1080p.mp4
set OUT=%ROOT%frames
if not exist "%OUT%" mkdir "%OUT%"

call :tri 00 120
call :tri 01 200
call :tri 02 300
call :tri 03 420
call :tri 04 540
call :tri 05 660
call :tri 06 780
call :tri 07 900
call :tri 08 1020
call :tri 09 1140
goto :done

:tri
set TAG=%1
set /a N=%2
set /a M=%2+1
set /a K=%2+2
call :frame %TAG% a %N%
call :frame %TAG% gt %M%
call :frame %TAG% b %K%
goto :eof

:frame
ffmpeg -v error -i "%IN%" -vf "select='eq(n\,%3)'" -vsync 0 -frames:v 1 -pix_fmt rgb24 -f rawvideo "%OUT%\t%1_%2.rgb"
goto :eof

:done
echo done
