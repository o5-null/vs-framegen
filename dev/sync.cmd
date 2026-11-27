@echo off
rem ---------------------------------------------------------------------------
rem Sync the K7sfunc working tree into dev\mpv (the local test player).
rem
rem dev\mpv is a one-off copy of a mpv-playkit stage; this script refreshes the
rem files that come from k7sfunc, so edits take effect without rebuilding a pack:
rem   k7sfunc\mpv\portable_config\scripts\k7f_*.lua   -> portable_config\scripts
rem   k7sfunc\mpv\portable_config\script-opts\*.conf  -> portable_config\script-opts
rem   k7sfunc\mpv\portable_config\vs\MEMC_*.vpy       -> portable_config\vs
rem   k7sfunc\src\k7sfunc\*                          -> Lib\site-packages\k7sfunc
rem
rem Usage: dev\sync.cmd
rem ---------------------------------------------------------------------------
setlocal
set "ROOT=%~dp0.."
set "PLR=%ROOT%\dev\mpv"
set "KF=%ROOT%\k7sfunc"

if not exist "%PLR%\mpv.exe" (
	echo [sync] dev\mpv not found - copy a mpv-playkit stage into dev\mpv first.
	exit /b 1
)

robocopy "%KF%\mpv\portable_config\scripts" "%PLR%\portable_config\scripts" k7f_*.lua /NJH /NJS /NP /R:1 /W:1 /NFL /NDL
if errorlevel 8 goto fail
robocopy "%KF%\mpv\portable_config\script-opts" "%PLR%\portable_config\script-opts" *.conf /NJH /NJS /NP /R:1 /W:1 /NFL /NDL
if errorlevel 8 goto fail
robocopy "%KF%\mpv\portable_config\vs" "%PLR%\portable_config\vs" MEMC_*.vpy /NJH /NJS /NP /R:1 /W:1 /NFL /NDL
if errorlevel 8 goto fail
rem mirror instead of copy: deleted/renamed modules in src\k7sfunc must go away too
robocopy "%KF%\src\k7sfunc" "%PLR%\Lib\site-packages\k7sfunc" /MIR /XD __pycache__ /NJH /NJS /NP /R:1 /W:1 /NFL /NDL
if errorlevel 8 goto fail

echo [sync] dev\mpv updated from k7sfunc working tree.
exit /b 0

:fail
echo [sync] FAILED - see robocopy output above.
exit /b 1
