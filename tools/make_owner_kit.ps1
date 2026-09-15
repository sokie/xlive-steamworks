# Packs the Street Fighter X Tekken owner test kit into bin\sfxt-owner-kit.zip.
# The probe and the drop-in wrapper both target app 209120 (the owner's Steam copy) and log at
# debug so the returned results are complete. Build with -DXLS_BUILD_TESTS=ON first.
param(
	[uint32]$AppId = 209120,
	[string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
$kit = Join-Path $PSScriptRoot "ownerkit"
if (-not $Out) { $Out = Join-Path $bin "sfxt-owner-kit.zip" }

foreach ($file in @("xlive_smoke.exe", "xlive.dll", "steam_api.dll")) {
	if (-not (Test-Path (Join-Path $bin $file))) {
		throw "missing $file in $bin, build with -DXLS_BUILD_TESTS=ON first"
	}
}

$stage = Join-Path $env:TEMP "sfxt-owner-kit"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "probe") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "wrapper") | Out-Null
New-Item -ItemType Directory -Path (Join-Path $stage "results") | Out-Null

# Top-level scripts and readme.
Copy-Item (Join-Path $kit "*.bat") $stage
Copy-Item (Join-Path $kit "README.txt") $stage

# The standalone probe.
Copy-Item (Join-Path $bin "xlive_smoke.exe") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "xlive.dll") (Join-Path $stage "probe")
Copy-Item (Join-Path $bin "steam_api.dll") (Join-Path $stage "probe")
[IO.File]::WriteAllText((Join-Path $stage "probe\steam_appid.txt"), "$AppId")
$probeConfig = '{ "log": { "level": "debug" }, "steam": { "required": false }, "leaderboards": { "create_if_missing": false } }'
[IO.File]::WriteAllText((Join-Path $stage "probe\xlive_steamworks.json"), $probeConfig)

# The drop-in wrapper for the game folder.
Copy-Item (Join-Path $bin "xlive.dll") (Join-Path $stage "wrapper")
Copy-Item (Join-Path $bin "steam_api.dll") (Join-Path $stage "wrapper")
[IO.File]::WriteAllText((Join-Path $stage "wrapper\steam_appid.txt"), "$AppId")
$gameConfig = @"
{
	"log": { "level": "debug" },
	"steam": { "app_id": $AppId, "required": false },
	"leaderboards": { "create_if_missing": true },
	"achievements": { "local_fallback": true }
}
"@
[IO.File]::WriteAllText((Join-Path $stage "wrapper\xlive_steamworks.json"), $gameConfig)

# Keep the empty results folder in the zip.
[IO.File]::WriteAllText((Join-Path $stage "results\PUT_RESULTS_HERE.txt"), "The scripts write probe_report.txt, game_xlive_steamworks.log and system_info.txt here.")

if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Remove-Item -Recurse -Force $stage
Write-Host "wrote $Out"
