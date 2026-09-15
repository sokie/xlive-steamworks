@echo off
setlocal
cd /d "%~dp0"

set "GAMEDIR="
if exist results\gamedir.txt set /p GAMEDIR=<results\gamedir.txt
if "%GAMEDIR%"=="" (
	set /p GAMEDIR="Game folder to restore: "
)
set "GAMEDIR=%GAMEDIR:"=%"

if not exist "%GAMEDIR%\xlive.dll.orig-backup" (
	echo No backup found at "%GAMEDIR%\xlive.dll.orig-backup".
	echo Nothing to restore.
	pause
	exit /b 1
)

echo Restoring the original xlive.dll ...
copy /y "%GAMEDIR%\xlive.dll.orig-backup" "%GAMEDIR%\xlive.dll" >nul
del "%GAMEDIR%\xlive.dll.orig-backup" >nul
del "%GAMEDIR%\steam_api.dll" 2>nul
del "%GAMEDIR%\steam_appid.txt" 2>nul
del "%GAMEDIR%\xlive_steamworks.json" 2>nul
del "%GAMEDIR%\xlive_steamworks.log" 2>nul
echo Done. The game is back to its original files.
pause
