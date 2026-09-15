@echo off
setlocal
cd /d "%~dp0"
if not exist results mkdir results

set "GAMEDIR=%~1"
if "%GAMEDIR%"=="" (
	echo Enter the full path to your Street Fighter X Tekken folder
	echo   ^(the folder that contains SFTK.exe^), then press Enter.
	echo You can also drag that folder, or SFTK.exe itself, onto this .bat file.
	set /p "GAMEDIR=Game folder: "
)
set "GAMEDIR=%GAMEDIR:"=%"

rem If given SFTK.exe (or any file) instead of the folder, use its folder.
if exist "%GAMEDIR%\SFTK.exe" goto have
for %%I in ("%GAMEDIR%") do (
	if exist "%%~fI\SFTK.exe" ( set "GAMEDIR=%%~fI" & goto have )
	if /I "%%~nxI"=="SFTK.exe" ( set "GAMEDIR=%%~dpI" & goto trail )
)
echo.
echo Could not find SFTK.exe using: %GAMEDIR%
echo Run again and give the folder that contains SFTK.exe.
pause
exit /b 1

:trail
if "%GAMEDIR:~-1%"=="\" set "GAMEDIR=%GAMEDIR:~0,-1%"
:have
if not exist "%GAMEDIR%\SFTK.exe" (
	echo SFTK.exe not found in: %GAMEDIR%
	pause
	exit /b 1
)
echo.
echo Game folder: %GAMEDIR%

if not exist "%GAMEDIR%\SFTK.exe.cfg" echo WARNING: SFTK.exe.cfg is missing next to SFTK.exe; the game may not start without it.

echo Backing up the original xlive.dll ...
if exist "%GAMEDIR%\xlive.dll" (
	if exist "%GAMEDIR%\xlive.dll.orig-backup" (
		echo   keeping the existing xlive.dll.orig-backup
	) else (
		copy /y "%GAMEDIR%\xlive.dll" "%GAMEDIR%\xlive.dll.orig-backup" >nul
		echo   saved xlive.dll.orig-backup
	)
)

echo Installing the wrapper ...
copy /y "wrapper\xlive.dll" "%GAMEDIR%\xlive.dll" >nul
copy /y "wrapper\steam_api.dll" "%GAMEDIR%\steam_api.dll" >nul
copy /y "wrapper\xlive_steamworks.json" "%GAMEDIR%\xlive_steamworks.json" >nul
> "%GAMEDIR%\steam_appid.txt" echo 209120
del "%GAMEDIR%\xlive_steamworks.log" 2>nul
> results\gamedir.txt echo %GAMEDIR%

echo.
echo Starting the game. Play a little, try the online / ranked / lobby menus,
echo then QUIT the game normally. This window waits until the game closes.
echo.
pushd "%GAMEDIR%"
start /wait "" "SFTK.exe"
popd

echo.
echo Game closed. Collecting the log ...
copy /y "%GAMEDIR%\xlive_steamworks.log" "results\game_xlive_steamworks.log" >nul 2>nul
if exist "results\game_xlive_steamworks.log" (
	echo   saved results\game_xlive_steamworks.log
) else (
	echo   no log was produced; the game may not have started.
)
echo.
echo Next: 4_zip_results.bat to package everything, or 3_restore_game.bat to undo.
pause
