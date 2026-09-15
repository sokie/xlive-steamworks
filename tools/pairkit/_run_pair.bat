@echo off
rem Shared by the numbered scripts: %1 = host|join, %2 = extra flags, %3 = result name.
setlocal
cd /d "%~dp0"
if not exist results mkdir results
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -LiteralPath '%~dp0.' -Recurse -File | Unblock-File" 2>nul

if not exist "probe\xlive_smoke.exe" (
	echo ERROR: probe\xlive_smoke.exe is missing. Antivirus probably removed it.
	pause
	exit /b 1
)

set "CODE="
set /p CODE=Enter the 4-digit code agreed with the other tester [4242]:
if "%CODE%"=="" set "CODE=4242"

echo.
echo Running the %1 side with code %CODE% %2 ...
echo Keep this window open until it says PAIR RESULT. Steam must be running.
echo.
del "results\%3.log" 2>nul
del "results\pair.log" 2>nul
> "results\%3.txt" echo ===== xlive-steamworks pair test: %1 %2 code %CODE% =====
"%~dp0probe\xlive_smoke.exe" --pair-%1 %CODE% %2 >> "%~dp0results\%3.txt" 2>&1
set "RC=%ERRORLEVEL%"
>> "results\%3.txt" echo.
>> "results\%3.txt" echo (exit code %RC%)
rem The dll logs to results\pair.log (set in probe\xlive_steamworks.json), keep one log per run.
if exist "results\pair.log" move /y "results\pair.log" "results\%3.log" >nul
type "results\%3.txt"
echo.
echo ------------------------------------------------------------------
echo Done. Results are in the 'results' folder: %3.txt and %3.log
echo ------------------------------------------------------------------
pause
