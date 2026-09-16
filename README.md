# xlive-steamworks

A Games for Windows LIVE `xlive.dll` wrapper that routes to the Steamworks SDK backend.

Titles that were built against GFWL keep calling `XLiveInitialize`, `XSessionCreate`,
`XUserWriteAchievements` and the rest of the 270 xlive exports. This library answers those calls
with Steam users, Steam lobbies, Steam Datagram Relay, Steam achievements, leaderboards and Cloud.
Nothing in the title has to be rewritten, and a studio that wants to move one subsystem to
Steamworks directly can do that piece by piece through the extension API.

Two ways to use it:

- **Drop-in dll.** Ship `xlive.dll` and `steam_api.dll` next to the exe. No source change.
- **SDK.** Link the static library, include `xlive/xfuncs.h` in place of the GFWL headers, and
  call `xlive/xlive_steamworks.h` where you want to reach the Steam objects behind an XUID, a
  session handle or a secure address.

## Why

GFWL is long unsupported, and the titles built on it keep losing online play, achievements and
cloud saves as publishers strip `xlive.dll` from their Steam builds. 

It is a work in progress. The export table is complete and the we have some initial tests, 
but only a few titles have been run with it (see below), and each new title
finds some weird. [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness) is the
reference for how the GFWL API behaves, and the Steamworks SDK is fetched from
[rlabrecque's mirror](https://github.com/rlabrecque/SteamworksSDK). This project is not
affiliated with Microsoft or Valve.

## Titles

Street Fighter X Tekken (the GFWL and the Steam build) and Lost Planet 2 run with the drop-in
dll. Per-title folders, configs and notes live in
[xlive-steamworks-games](https://github.com/sokie/xlive-steamworks-games). A title that imports
nothing but `xlive.dll` needs only the two dlls and a config next to the exe.

## Why

GFWL is long unsupported, and the titles built on it keep losing online play, achievements and
cloud saves as publishers strip `xlive.dll` from their Steam builds. 

It is a work in progress. The export table is complete and the we have some initial tests, 
but only a few titles have been run with it (see below), and each new title
finds some weird. [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness) is the
reference for how the GFWL API behaves, and the Steamworks SDK is fetched from
[rlabrecque's mirror](https://github.com/rlabrecque/SteamworksSDK). This project is not
affiliated with Microsoft or Valve.

## Titles

Street Fighter X Tekken (the GFWL and the Steam build) and Lost Planet 2 run with the drop-in
dll. Per-title folders, configs and notes live in
[xlive-steamworks-games](https://github.com/sokie/xlive-steamworks-games). A title that imports
nothing but `xlive.dll` needs only the two dlls and a config next to the exe.

## Why

GFWL is long unsupported, and the titles built on it keep losing online play, achievements and
cloud saves as publishers strip `xlive.dll` from their Steam builds. 

It is a work in progress. The export table is complete and the we have some initial tests, 
but only a few titles have been run with it (see below), and each new title
finds some weird. [XLiveLessNess](https://gitlab.com/GlitchyScripts/xlivelessness) is the
reference for how the GFWL API behaves, and the Steamworks SDK is fetched from
[rlabrecque's mirror](https://github.com/rlabrecque/SteamworksSDK). This project is not
affiliated with Microsoft or Valve.

## Titles

Street Fighter X Tekken (the GFWL and the Steam build) and Lost Planet 2 run with the drop-in
dll. Per-title folders, configs and notes live in
[xlive-steamworks-games](https://github.com/sokie/xlive-steamworks-games). A title that imports
nothing but `xlive.dll` needs only the two dlls and a config next to the exe.

## Status

| Area | State | Steam side |
| --- | --- | --- |
| Init, render, input, overlapped, enumerators, notifications | implemented | `SteamAPI_RunCallbacks` pump, `GameOverlayActivated_t` -> `XN_SYS_UI` |
| Users, sign-in, privileges, contexts, properties | implemented | `ISteamUser`, `ISteamFriends` (persona), rich presence |
| Achievements (write, enumerate, pictures) | implemented | `ISteamUserStats` achievements, SPA for text and art, per-user record when the app has no Steam achievement schema |
| Profile settings | implemented | one Steam Cloud file, defaults from Steam for the read-only ones |
| Title managed storage (`XStorage*`) | implemented | Steam Cloud per user, shipped folder per title, files next to the exe when the app's Cloud refuses API writes |
| Friends, presence, invites | implemented | `ISteamFriends`, lobby invites, `+connect_lobby` |
| Sessions, search, arbitration | implemented | `ISteamMatchmaking` lobbies and lobby data |
| Stats and leaderboards | implemented | `ISteamUserStats` leaderboards, details array for extra columns |
| Networking (`XNet*`, `XSocket*`, QoS) | implemented, UDP is the exercised path | `ISteamNetworkingMessages` (UDP), `ISteamNetworkingSockets` P2P (TCP), Winsock for title servers |
| Guide UI (`XShow*`) | implemented | Steam overlay pages, native dialogs for keyboard and message box |
| Content and marketplace (DLC) | implemented | `ISteamApps` DLC list, store overlay |
| XLocator server list | implemented for peer hosted | tagged public lobbies |
| Title servers (XLSP) | config driven | real addresses through Winsock |
| Voice (`XHVCreateEngine`) | implemented, basic | Steam voice capture/decode, waveOut playback |
| Protected data, PBuffer, data sections, updates | pass-through | not needed on Steam |
| Guide custom actions, gamer picture awards, TrueSkill service | stubs | no Steam counterpart |

The mapping document has the entry-by-entry table: which exports map one-to-one, which go through
a middle layer, and which are stubs. the SPA notes explains what happens to the title's SPA.

## How identity maps

Every identifier a title sees is derived from Steam's own, so two players never collide:

| GFWL | Value on Steam |
| --- | --- |
| `XUID` | `0x0009000000000000 | SteamAccountID` (LIVE-enabled, `IsOnlineXUID` holds). The inverse is `CSteamID(accountId, Public, Individual)` |
| `XNKID` (session id) | the 64-bit Steam lobby id |
| `XNKEY` | 16 random bytes made by the host, shared through lobby data |
| `XNADDR` | pseudo 10.x.y.z from the account id, MAC from the account id, `abOnline` = SteamID64 + magic |
| secure `IN_ADDR` | a local alias (10.0.0.n) per peer, exactly like GFWL's security associations |
| machine id | `0xFA00000000000000 | SteamAccountID` |
| UDP port | SteamNetworkingMessages channel |
| TCP port | SteamNetworkingSockets virtual port |

## Quick start for a studio

1. Build (see below) or take `bin/xlive.dll` and `bin/steam_api.dll`.
2. Put both next to the exe. For development add `steam_appid.txt` with your app id.
3. On the Steamworks partner site create:
   - one achievement per SPA achievement, API name `ACH_<id>` (or map names in the config)
   - one leaderboard per SPA stats view the title writes, name `LB_<viewId>`
   - Steam Cloud quota (profile file plus whatever the title stores through `XStorage*`)
   - a rich presence token `#status` with value `%status%` if you want the GFWL presence line
     in the friends list
   - one DLC app per GFWL content package, if the title had any.
4. Run the SPA exporter to export the SPA into a CSV of achievements,
   a CSV of leaderboards, the achievement icons and a starting `xlive_steamworks.json`.
5. Copy `config/xlive_steamworks.example.json` to `xlive_steamworks.json` next to the dll and
   fill in what differs from the defaults.
6. Set `log.level` to `debug` for the first runs, the log lands next to the dll.

The studio guide walks through each step with the details.

## Building

```
cmake -B build -G "Visual Studio 16 2019" -A Win32 -DSTEAMWORKS_SDK_DIR=path/to/sdk
cmake --build build --config Release
```

- GFWL titles are 32-bit, so build with `-A Win32`. A 64-bit build works and links
  `steam_api64`.
- Without `STEAMWORKS_SDK_DIR` the rlabrecque mirror of the SDK is fetched at the pinned
  revision (v1.65). The code builds against SDK 1.62 and newer. On Linux, Proton's
  `lsteamclient` bridges the Windows `steam_api.dll` to the native client and knows SDK 1.65
  from Proton 10 on. Proton 9 stops at 1.62, so a title that must run there builds against 1.62.
- `-DXLS_BUILD_STATIC=ON` produces `xlive.lib` for linking into a title. The def file is not used
  in that configuration.
- The runtime is the static CRT. The dll depends on `steam_api.dll`, `ws2_32`, `winmm`,
  `kernel32` and `user32` only.
- `-DXLS_BUILD_TESTS=ON` adds `bin/xlive_smoke.exe`, which drives the dll against a running Steam
  client: sign-in, notifications, achievements, profile and storage on Cloud, friends, sockets,
  a real lobby, a leaderboard write and read. Put `steam_appid.txt` with your app id
  next to it, or `480` (Spacewar) for testing only. `--spa Game.exe` loads a real title's SPA for the achievement list.
  `--filters` checks Steam's lobby filters against a lobby shaped like GTA IV's ranked search,
  `--query "4:0x10000056=9,..."` runs an XLAST query of the loaded SPA and logs the filters sent,
  `--voice` runs the voice engine in loopback, and `--pair-host CODE` / `--pair-join CODE`
  (`--relay-only`) run the two-machine network test that the packer builds.
- Per-title drop-in folders and owner test kits live in the companion repository
  [xlive-steamworks-games](https://github.com/sokie/xlive-steamworks-games).
- `xlive_smoke.exe --probe` reports what a Steam app offers (ownership, achievement schema, Cloud,
  DLC, relay, leaderboards) without creating anything on it. the probe packer
  [-Title "Name"]` packs it as `bin/xlive-probe-<id>.zip` for another owner of the app to run
  and send back `probe-results.zip`.

Layout:

```
tests/smoke/    the Steam smoke test, probe and two-machine pair test
src/xlive/      GFWL public types and prototypes, plus the extension header
src/core/       Steam lifetime and pump, overlapped bridge, enumerators, notifications,
                config, SPA reader, network layer, users, cloud, images
src/api/        one file per export group, the exported functions themselves
config/         example configuration
```

## Known limits

- Host migration keeps the lobby. The elected host claims the lobby through its member data and
  the current owner's wrapper hands it over. Until that happens, or until Steam times out a dead
  owner, searchers still see the old host. Members follow the claim at once.
- `XUserCreateStatsEnumeratorByRating` returns the top of the board since Steam cannot seek a
  leaderboard by score. `XUserEstimateRankForRating` answers rank 1.
- Other players' title managed storage is not readable (Steam Cloud is per account).
- Headless dedicated servers advertised through `XLocator` are out of scope, but a listen server that
  runs under a Steam client works. `XLocator` filter groups and sorters are not applied, so the
  enumerator returns every advertised server of the title.
- `XShowCustomPlayerListUI` opens the overlay's Players page and reports cancel, the title's
  custom buttons cannot be shown.
- Voice playback goes through waveOut with a simple per-talker stream, there is no mixing into
  the title's audio device.

## License

MIT. `src/third-party` carries its own licenses (zlib for puff, public domain for stb_image). The
Steamworks SDK is Valve's and is not redistributed here.
