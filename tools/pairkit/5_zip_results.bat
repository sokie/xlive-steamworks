@echo off
setlocal
cd /d "%~dp0"
if exist pair-results.zip del pair-results.zip
powershell -NoProfile -Command "Compress-Archive -Path '%~dp0results\*' -DestinationPath '%~dp0pair-results.zip' -Force"
echo Wrote pair-results.zip - send this file back.
pause
