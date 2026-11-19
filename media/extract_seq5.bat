@echo off
rem Adds the fifth frame (tNN_f4.rgb) to every group in media\seq, so --len 5 works: the pair is
rem (f0, f4) and multiFrameCount = 3, which puts index 1/2/3 on f1/f2/f3 - all real frames.
setlocal
set ROOT=%~dp0
set IN=%ROOT%sintel_trailer_1080p.mp4
set OUT=%ROOT%seq
if not exist "%OUT%" mkdir "%OUT%"

call :grp 00 120
call :grp 01 200
call :grp 02 300
call :grp 03 420
call :grp 04 540
call :grp 05 660
call :grp 06 780
call :grp 07 900
call :grp 08 1020
call :grp 09 1140
goto :done

:grp
set TAG=%1
set /a N=%2+4
call :frame %TAG% 4 %N%
goto :eof

:frame
ffmpeg -y -v error -i "%IN%" -vf "select='eq(n\,%3)'" -vsync 0 -frames:v 1 -pix_fmt rgb24 -f rawvideo "%OUT%\t%1_f%2.rgb"
goto :eof

:done
echo done
