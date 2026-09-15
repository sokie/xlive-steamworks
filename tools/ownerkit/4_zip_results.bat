@echo off
setlocal
cd /d "%~dp0"

if not exist results\probe_report.txt (
	echo No probe report yet. Run 1_run_probe.bat first.
	pause
	exit /b 1
)

set "OUT=%~dp0sfxt-results.zip"
del "%OUT%" 2>nul

echo Packaging results ...
powershell -NoProfile -Command "Compress-Archive -Path 'results\*' -DestinationPath '%OUT%' -Force"

if exist "%OUT%" (
	echo.
	echo Wrote %OUT%
	echo Please send that one file back. Thank you!
) else (
	echo Could not create the zip. Please send the whole 'results' folder instead.
)
pause
