@echo off
rem Runs the multi-frame probe with the dlssg_for_sm86 proxy beside it. Same layout as run.bat:
rem the proxy (version.dll) and its runtime folder live in ..\work, the NGX core comes from the
rem driver store on its own.
setlocal
set ROOT=%~dp0
set WORK=%ROOT%..\work
set SEQ=%ROOT%..\media\seq
if not exist "%WORK%" mkdir "%WORK%"
copy /y "%ROOT%..\dist\version.dll" "%WORK%\" >nul
copy /y "%ROOT%..\dist\dlssg_sm86.ini" "%WORK%\" >nul
xcopy /y /e /i /q "%ROOT%..\dist\dlssg_sm86" "%WORK%\dlssg_sm86\" >nul
copy /y "%ROOT%multiframe.exe" "%WORK%\" >nul
rem The proxy writes here; without the folder it fails silently.
if not exist "%WORK%\dlssg_sm86\logs" mkdir "%WORK%\dlssg_sm86\logs"
pushd "%WORK%"
multiframe.exe --seq "%SEQ%" --out "%WORK%" %*
set RC=%ERRORLEVEL%
popd
exit /b %RC%
