# Packs bin\xlive_smoke.exe, xlive.dll and steam_api.dll into a zip another Steam user can run.
# Usage: tools\make_probe.ps1 -AppId 209120 [-Out bin\xlive-probe-209120.zip]
param(
	[Parameter(Mandatory = $true)][uint32]$AppId,
	[string]$Out = ""
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "bin"
if (-not $Out) { $Out = Join-Path $bin "xlive-probe-$AppId.zip" }

foreach ($file in @("xlive_smoke.exe", "xlive.dll", "steam_api.dll")) {
	if (-not (Test-Path (Join-Path $bin $file))) {
		throw "missing $file in $bin, build with -DXLS_BUILD_TESTS=ON first"
	}
}

$stage = Join-Path $env:TEMP "xlive-probe-$AppId"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Path $stage | Out-Null

Copy-Item (Join-Path $bin "xlive_smoke.exe") $stage
Copy-Item (Join-Path $bin "xlive.dll") $stage
Copy-Item (Join-Path $bin "steam_api.dll") $stage
Copy-Item (Join-Path $PSScriptRoot "probe\run_probe.bat") $stage
Copy-Item (Join-Path $PSScriptRoot "probe\README.txt") $stage
[IO.File]::WriteAllText((Join-Path $stage "steam_appid.txt"), "$AppId")
$config = '{ "log": { "level": "debug" }, "steam": { "required": false }, "leaderboards": { "create_if_missing": false } }'
[IO.File]::WriteAllText((Join-Path $stage "xlive_steamworks.json"), $config)

if (Test-Path $Out) { Remove-Item -Force $Out }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out
Remove-Item -Recurse -Force $stage
Write-Host "wrote $Out"
