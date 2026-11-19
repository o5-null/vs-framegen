@echo off
rem Runs the spike with the dlssg_for_sm86 proxy beside it. The NGX core (_nvngx.dll) is left to
rem NGX's own loader, which finds the driver-store copy - same as a game without a local one.
setlocal
set ROOT=%~dp0
set WORK=%ROOT%..\work
if not exist "%WORK%" mkdir "%WORK%"
copy /y "%ROOT%..\dist\version.dll" "%WORK%\" >nul
copy /y "%ROOT%..\dist\dlssg_sm86.ini" "%WORK%\" >nul
xcopy /y /e /i /q "%ROOT%..\dist\dlssg_sm86" "%WORK%\dlssg_sm86\" >nul
copy /y "%ROOT%spike.exe" "%WORK%\" >nul
pushd "%WORK%"
spike.exe --out "%WORK%" %*
set RC=%ERRORLEVEL%
popd
exit /b %RC%
