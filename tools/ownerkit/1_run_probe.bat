@echo off
setlocal
cd /d "%~dp0"
if not exist results mkdir results

echo Removing the "downloaded from the internet" mark so Windows does not block the files ...
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0.' -Recurse -File | Unblock-File" 2>nul

if not exist "probe\xlive_smoke.exe" (
	> results\probe_report.txt echo probe\xlive_smoke.exe is missing - your antivirus most likely removed it.
	echo ERROR: probe\xlive_smoke.exe is missing. Antivirus probably removed it.
	echo Restore it from the zip or allow it, then run this again.
	pause
	exit /b 1
)

echo Writing system info ...
> results\system_info.txt echo ===== system info =====
ver >> results\system_info.txt
powershell -NoProfile -Command "$o=Get-CimInstance Win32_OperatingSystem; ('OS: {0} (build {1}, {2})' -f $o.Caption,$o.BuildNumber,$o.OSArchitecture); 'GPU:'; Get-CimInstance Win32_VideoController | ForEach-Object { ('  {0} | driver {1} | {2}x{3} @ {4}Hz' -f $_.Name,$_.DriverVersion,$_.CurrentHorizontalResolution,$_.CurrentVerticalResolution,$_.CurrentRefreshRate) }; ('Steam running: {0}' -f [bool](Get-Process steam -ErrorAction SilentlyContinue))" >> results\system_info.txt 2>&1

echo.
echo Running the Steam probe for app 209120 ...
echo Steam must be running, and this account must own Street Fighter X Tekken.
echo This takes about a minute. Please wait.
echo.
del results\probe_debug.log 2>nul
> results\probe_report.txt echo ===== xlive-steamworks probe (console) =====
rem Call the exe by full path, some systems do not search the current folder.
"%~dp0probe\xlive_smoke.exe" --probe >> "%~dp0results\probe_report.txt" 2>&1
set "RC=%ERRORLEVEL%"
>> results\probe_report.txt echo.
>> results\probe_report.txt echo (probe exit code %RC%)

rem The dll writes its own debug log straight into results via its config, keep a copy if it landed elsewhere.
if exist probe\xlive_steamworks.log copy /y probe\xlive_steamworks.log results\probe_debug.log >nul 2>nul

type results\probe_report.txt
echo.
echo ------------------------------------------------------------------
echo Probe done. Everything is in the 'results' folder:
echo   probe_report.txt, probe_debug.log, system_info.txt
echo Next: run 4_zip_results.bat to package them, or 2_install_and_play.bat to test the game.
echo ------------------------------------------------------------------
pause
