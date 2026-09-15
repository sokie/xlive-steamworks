# Packs a probe-only kit into bin\sfxt-probe.zip: run 1_run_probe.bat, then 4_zip_results.bat.
# Batch and text files are written CRLF so cmd.exe handles them, the probe dll logs into results.
# Build with -DXLS_BUILD_TESTS=ON first.
param(
	[uint32]$AppId = 209120,
	[string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
$kit = Join-Path $PSScriptRoot "ownerkit"
if (-not $Out) { $Out = Join-Path $bin "sfxt-probe.zip" }

foreach ($file in @("xlive_smoke.exe", "xlive.dll", "steam_api.dll")) {
	if (-not (Test-Path (Join-Path $bin $file))) {
		throw "missing $file in $bin, build with -DXLS_BUILD_TESTS=ON first"
	}
}

$stage = Join-Path $env:TEMP "sfxt-probe"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path (Join-Path $stage "probe") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "results") | Out-Null

Copy-Item (Join-Path $bin "xlive_smoke.exe") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "xlive.dll") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "steam_api.dll") (Join-Path $stage "probe")
[IO.File]::WriteAllText((Join-Path $stage "probe\steam_appid.txt"), "$AppId")
# The dll writes its debug log straight into the kit's results folder (relative to the probe exe).
$probeConfig = '{ "log": { "level": "debug", "path": "..\\results\\probe_debug.log" }, "steam": { "required": false }, "leaderboards": { "create_if_missing": false } }'
[IO.File]::WriteAllText((Join-Path $stage "probe\xlive_steamworks.json"), $probeConfig)

Copy-Item (Join-Path $kit "1_run_probe.bat") $stage
Copy-Item (Join-Path $kit "4_zip_results.bat") $stage

$readme = @"
xlive-steamworks probe (Street Fighter X Tekken, app $AppId)

WHAT IT DOES
  Checks what Steam offers this game with the wrapper: your account, whether it
  owns the game, achievements, Cloud, lobbies and the relay network. It creates
  nothing permanent on Steam and does not need the game installed.

HOW TO RUN
  1. Start Steam and log in with the account that owns Street Fighter X Tekken.
  2. Double-click  1_run_probe.bat  and wait about a minute.
  3. Double-click  4_zip_results.bat  to make  sfxt-results.zip .
  4. Send  sfxt-results.zip  back.

The results folder will hold probe_report.txt, probe_debug.log and system_info.txt.
If the report says Steam did not initialise, either Steam is not running or that
account does not own the game.
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
