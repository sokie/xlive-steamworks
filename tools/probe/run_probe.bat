@echo off
setlocal
cd /d "%~dp0"

rem Usage: run_probe.bat [steam app id] [path\to\Game.exe]
rem Needs the Steam client running and logged in to an account that owns the app.

if not "%~1"=="" (
	<nul set /p =%~1> steam_appid.txt
)
set /p APPID=<steam_appid.txt
set SPA=
if not "%~2"=="" set SPA=--spa "%~2"

echo Probing Steam app %APPID% ...
del xlive_steamworks.log 2>nul
xlive_smoke.exe --probe %SPA% > probe_report.txt 2>&1
type probe_report.txt
echo. >> probe_report.txt
echo ===== xlive_steamworks.log ===== >> probe_report.txt
type xlive_steamworks.log >> probe_report.txt 2>nul
echo.
echo Report written to probe_report.txt - send that file back.
pause
