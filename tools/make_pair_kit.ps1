# Packs the two-PC network test into bin\xlive-pair-kit.zip. Both testers run it under app 480
# (Spacewar), which is for testing only and needs no game ownership.
# Build with -DXLS_BUILD_TESTS=ON first.
param(
	[uint32]$AppId = 480,
	[string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
$kit = Join-Path $PSScriptRoot "pairkit"
if (-not $Out) { $Out = Join-Path $bin "xlive-pair-kit.zip" }

foreach ($file in @("xlive_smoke.exe", "xlive.dll", "steam_api.dll")) {
	if (-not (Test-Path (Join-Path $bin $file))) {
		throw "missing $file in $bin, build with -DXLS_BUILD_TESTS=ON first"
	}
}

$stage = Join-Path $env:TEMP "xlive-pair-kit"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path (Join-Path $stage "probe") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "results") | Out-Null

Copy-Item (Join-Path $bin "xlive_smoke.exe") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "xlive.dll") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "steam_api.dll") (Join-Path $stage "probe")
[IO.File]::WriteAllText((Join-Path $stage "probe\steam_appid.txt"), "$AppId")
$probeConfig = '{ "log": { "level": "debug", "path": "..\\results\\pair.log" }, "steam": { "required": false }, "leaderboards": { "create_if_missing": false } }'
[IO.File]::WriteAllText((Join-Path $stage "probe\xlive_steamworks.json"), $probeConfig)

Copy-Item (Join-Path $kit "*.bat") $stage
Copy-Item (Join-Path $kit "README.txt") $stage
[IO.File]::WriteAllText((Join-Path $stage "results\PUT_RESULTS_HERE.txt"), "The scripts write pair_*.txt and pair_*.log here.")

# Force CRLF on everything cmd.exe or Notepad reads.
Get-ChildItem $stage -Recurse -Include *.bat, *.txt | ForEach-Object {
	$text = [IO.File]::ReadAllText($_.FullName) -replace "`r`n", "`n" -replace "`n", "`r`n"
	[IO.File]::WriteAllText($_.FullName, $text)
}

if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Remove-Item -Recurse -Force $stage
Write-Host "wrote $Out"
