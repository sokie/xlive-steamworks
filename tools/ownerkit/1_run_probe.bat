@echo off
setlocal
cd /d "%~dp0"
if not exist results mkdir results

echo Writing system info ...
call :sysinfo > results\system_info.txt 2>&1

echo.
echo Running the Steam probe for app 209120 ...
echo (Steam must be running and this account must own Street Fighter X Tekken.)
echo.
del "%~dp0probe\xlive_steamworks.log" 2>nul
pushd probe
xlive_smoke.exe --probe > "%~dp0results\probe_report.txt" 2>&1
popd
echo. >> results\probe_report.txt
echo ===== xlive_steamworks.log (probe) ===== >> results\probe_report.txt
type "%~dp0probe\xlive_steamworks.log" >> results\probe_report.txt 2>nul

type results\probe_report.txt
echo.
echo ------------------------------------------------------------------
echo Probe done. Report saved to results\probe_report.txt
echo Next: 2_install_and_play.bat to test the game, or 4_zip_results.bat to send this back.
echo ------------------------------------------------------------------
pause
exit /b

:sysinfo
echo ===== system info =====
ver
echo.
wmic os get Caption,Version,OSArchitecture /value 2>nul | findstr "="
echo.
echo GPU:
wmic path win32_VideoController get Name,DriverVersion,CurrentHorizontalResolution,CurrentVerticalResolution,CurrentRefreshRate /value 2>nul | findstr "="
echo.
echo Steam process:
tasklist /fi "imagename eq steam.exe" 2>nul | findstr /i steam.exe || echo steam.exe NOT running
exit /b
