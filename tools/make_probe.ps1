# Packs bin\xlive-probe-<appid>.zip for another owner of the app to run: the probe under probe\,
# 1_run_probe.bat, 2_zip_results.bat and a README. Batch and text files are written CRLF so
# cmd.exe and Notepad handle them, the probe dll logs into results\.
# Usage: tools\make_probe.ps1 -AppId 209120 [-Title "Street Fighter X Tekken"] [-Out path.zip]
# Build with -DXLS_BUILD_TESTS=ON first.
param(
	[Parameter(Mandatory = $true)][uint32]$AppId,
	[string]$Title = "",
	[string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
if (-not $Out) { $Out = Join-Path $bin "xlive-probe-$AppId.zip" }
if (-not $Title) { $Title = "Steam app $AppId" }

foreach ($file in @("xlive_smoke.exe", "xlive.dll", "steam_api.dll")) {
	if (-not (Test-Path (Join-Path $bin $file))) {
		throw "missing $file in $bin, build with -DXLS_BUILD_TESTS=ON first"
	}
}

$stage = Join-Path $env:TEMP "xlive-probe-$AppId"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path (Join-Path $stage "probe") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "results") | Out-Null

Copy-Item (Join-Path $bin "xlive_smoke.exe") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "xlive.dll") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "steam_api.dll") (Join-Path $stage "probe")
[IO.File]::WriteAllText((Join-Path $stage "probe\steam_appid.txt"), "$AppId")
$probeConfig = '{ "log": { "level": "debug", "path": "..\\results\\probe_debug.log" }, "steam": { "required": false }, "leaderboards": { "create_if_missing": false } }'
[IO.File]::WriteAllText((Join-Path $stage "probe\xlive_steamworks.json"), $probeConfig)

Copy-Item (Join-Path $PSScriptRoot "probe\1_run_probe.bat") $stage
Copy-Item (Join-Path $PSScriptRoot "probe\2_zip_results.bat") $stage

$readme = @"
xlive-steamworks probe ($Title, app $AppId)

WHAT IT DOES
  Checks what Steam offers this app to a Games for Windows LIVE title running on
  the xlive-steamworks wrapper: your account, ownership, achievements, Cloud,
  DLC, lobbies and the relay network. It creates nothing permanent on Steam and
  does not need the game installed. The report holds your Steam name and id and
  your friends' names, nothing else.

HOW TO RUN
  1. Start Steam and log in with the account that owns the app.
  2. Double-click  1_run_probe.bat  and wait about a minute.
  3. Double-click  2_zip_results.bat  to make  probe-results.zip .
  4. Send  probe-results.zip  back.

If the report says Steam did not initialise, either Steam is not running or that
account does not own the app.
"@
[IO.File]::WriteAllText((Join-Path $stage "README.txt"), $readme)
[IO.File]::WriteAllText((Join-Path $stage "results\PUT_RESULTS_HERE.txt"), "The scripts write probe_report.txt, probe_debug.log and system_info.txt here.")

# Force CRLF on everything cmd.exe or Notepad reads.
Get-ChildItem $stage -Recurse -Include *.bat, *.txt | ForEach-Object {
	$text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n" -replace "`n", "`r`n"
	[IO.File]::WriteAllText($_.FullName, $text)
}

if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Remove-Item -Recurse -Force $stage
Write-Host "wrote $Out"
