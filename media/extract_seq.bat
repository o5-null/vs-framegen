@echo off
rem Extract consecutive frame groups for the multi-frame probe: tNN_f0 .. tNN_f3 (4 in a row).
rem With 4 frames the pair is (f0, f3) and multiFrameCount = 2, so index 1 should be f1 and
rem index 2 should be f2 - both have an exact real ground truth.
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
set /a N=%2
call :frame %TAG% 0 %N%
set /a N1=%N%+1
call :frame %TAG% 1 %N1%
set /a N2=%N%+2
call :frame %TAG% 2 %N2%
set /a N3=%N%+3
call :frame %TAG% 3 %N3%
goto :eof

:frame
ffmpeg -v error -i "%IN%" -vf "select='eq(n\,%3)'" -vsync 0 -frames:v 1 -pix_fmt rgb24 -f rawvideo "%OUT%\t%1_f%2.rgb"
goto :eof

:done
echo done
