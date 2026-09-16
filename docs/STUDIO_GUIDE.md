# Studio guide

The wrapper takes a shipped GFWL title to Steam without touching its online code. This is the
order of work.

## 1. Steamworks setup

- **App id.** The usual: `steam_appid.txt` next to the exe while developing, the Steam client
  launch in release. `steam.app_id` in the config sets the id when neither is present.
- **Achievements.** One per SPA achievement. API name `ACH_<spa id>` unless you prefer your own
  names, in which case list them under `achievements.map`. Display name, description and icons
  come out of `tools/spa2steam.py`.
- **Leaderboards.** One per stats view the title writes. Default name `LB_<view id>`. With
  `leaderboards.create_if_missing` (default on) the wrapper creates missing boards on first write
  with the sort order from the config. Boards created that way have the default display type, so
  creating them on the partner site first is cleaner.
- **Steam Cloud.** Enable it with a quota. The wrapper writes `xlive/profile.bin` (a few KB)
  and `xlive/tms/<item>` for anything the title stored through `XStorage*`.
- **Rich presence.** Add a localization token `#status` whose value is `%status%` so the GFWL
  presence line appears in the friends list. The wrapper also sets `connect` to
  `+connect_lobby <id>` while the player is in a joinable session, which enables "Join Game".
- **DLC.** One DLC app per GFWL content package. Map them in `content.dlc` with the content id
  the title expects (the tool prints the ids it finds in the SPA when they are referenced) and
  the folder the DLC depot installs to.
- **Networking.** Nothing to configure. SteamNetworkingMessages relays through Steam Datagram
  Relay, there are no ports to open.

## 2. Files next to the exe

```
Game.exe
xlive.dll                  (this project)
steam_api.dll              (from the Steamworks SDK, same bitness)
steam_appid.txt            (development only)
xlive_steamworks.json      (optional)
xlive-title-storage\       (optional: per-title files the title used to download from LIVE)
DLC\<appid>\               (auto discovered DLC folders, unless content.dlc points elsewhere)
```

A GFWL `xlive.dll` still installed under `Program Files\Microsoft Games for Windows - LIVE` does
not interfere: the exe finds the dll in its own directory first.

## 3. Configuration

`config/xlive_steamworks.example.json` lists every key with its default. The ones that matter
first:

| Key | Why |
| --- | --- |
| `log.level` | `debug` during bring-up, the log names every export the title calls and every Steam result |
| `achievements.map`, `leaderboards.map` | when the Steamworks names are not `ACH_<id>` / `LB_<id>` |
| `leaderboards.map.<id>.rating_column` | when the view's first column is not the one to rank by |
| `content.dlc` | content ids and paths for DLC |
| `title_servers` | if the title talked to XLSP servers (dedicated master servers), these keep their real IPs |
| `steam.required` | set to false to suppress the "Steam not running" warning in offline-capable builds |

## 4. Probe the app, then run the title

Before touching the title, run the probe under your app id: `tools/make_probe.ps1 -AppId <id>`
packs `bin/xlive-probe-<id>.zip`. Unzip it anywhere on a machine whose Steam account owns the
app and run `1_run_probe.bat`. It reports ownership, the achievement schema, Cloud and its API
quota, DLC, the relay network and lobbies, creates nothing persistent on the app, and writes
its report into `results\` and `2_zip_results.bat` packs that folder to send back.
`tools/make_pair_kit.ps1` packs `bin/xlive-pair-kit.zip`, a two-PC test that proves
filtered discovery, UDP and TCP traffic through the GFWL socket API, the transport Steam chose
(direct or relay, and a forced relay-only run), voice frames and host migration in both
directions between two real networks. It runs as Steam app 480 (Spacewar), which is for testing
only.

What the probe tells you:

- `Steam did not initialise`: the account does not own the app (Steam answers
  `ConnectToGlobalUser failed`), or Steam is not running.
- `steam achievements defined for this app: 0`: no schema, unlocks go to the per-user file until
  one exists (`achievements.local_fallback`).
- `cloud: account 1, app 1` but `cloud quota: not reported`: Cloud is on for auto-cloud folders
  only, the API refuses writes and the wrapper keeps profile and storage files next to the exe.
- `leaderboard LB_1 does not exist`: create the boards on the partner site or leave
  `leaderboards.create_if_missing` on.

With `log.level` at `debug`:

1. `steam: initialised app <id> as <name>` appears. If not, Steam is not running or the app id is
   wrong.
2. `spa: title 0x.... "<title>": N achievements ...` appears. If not, the exe has no `SPAFILE`
   resource (or a packer hides it), and achievement text falls back to Steam.
3. `users: user 0 is <gamertag>`.
4. Unlock an achievement in game: `achievements: unlocked <id> as ACH_<id>`. An error here means
   the API name does not exist on Steamworks.
5. Host a match: `session: hosting lobby <id>`. Join from a second account: `session: joined
   lobby <id>` and `net: alias 10.0.0.2 -> <steamid>` on both sides.
6. Write stats at the end of a match: `stats: view <id> -> leaderboard LB_<id>` and
   `stats: view <id> score <n> uploaded`.

## 5. Linking as an SDK

Build with `-DXLS_BUILD_STATIC=ON`, link `xlive.lib` and `steam_api.lib`, include
`xlive/xfuncs.h` in place of the GFWL SDK headers (the types in `xlive/xdefs.h` match the GFWL
layouts). Everything the dll exports is a plain function call now.

Then `xlive/xlive_steamworks.h` lets you move subsystems over one at a time:

- **You initialise Steam yourself.** Call `XlsSetSteamOwnedByTitle(TRUE)` before
  `XLiveInitialize`. If you also run `SteamAPI_RunCallbacks`, `XlsSetRunSteamCallbacks(FALSE)`.
  The wrapper shares the same Steam pipe and callbacks.
- **You replace matchmaking but keep the game networking.** Find lobbies with
  `ISteamMatchmaking`, then `XlsSessionInfoFromLobby(lobbyId, &info)` and hand `info` to
  `XSessionCreate`. The `XSocket*` traffic keeps working, addressed by the aliases
  `XlsSecureAddrFromSteamId` gives you.
- **You replace networking but keep sessions.** `XlsSteamIdFromSecureAddr` turns the
  `sockaddr_in` the session code passes around into a `CSteamID` you can send to with
  `ISteamNetworkingMessages` yourself. `XlsLobbyIdFromSession` gives the lobby for member lists.
- **You keep the GFWL notification loop.** `XlsPostNotification(XN_..., param)` feeds the
  title's existing `XNotifyGetNext` handling from your own Steam callbacks.
- **You rename achievements at run time** (per region, per build): `XlsSetAchievementName`.

The extension functions are also exported by the dll (ordinals 46000+) for a title that stays
with the drop-in dll but loads them with `GetProcAddress`.

## 6. Behaviours worth knowing

- `XShowKeyboardUI` and `XShowMessageBoxUI` open the Steam gamepad text input in Big Picture and
  on Steam hardware. On the desktop they open a small native dialog. An exclusive full-screen title
  can hide it behind the game window, borderless windowed avoids that.
- A session whose create flags carry `USES_PRESENCE` is the one invites go to.
- Session searches return every public lobby of the app that the filters accept. The wrapper
  hides lobbies this client is a member of. A result carries exactly the attributes the XLAST
  query declares, in the declared order, as Live did. The host's gamertag and XUID travel as
  the system properties `X_PROPERTY_GAMER_HOSTNAME` and `X_PROPERTY_GAMER_PUID`, which most
  titles declare and show in their lobby list.
- Host migration keeps the lobby id, so the XNKID and every registered key stay valid. Only the
  Steam lobby owner can write lobby data, so the member the title elects publishes a host claim
  in its member data. Whichever wrapper owns the lobby hands it over on seeing the claim, and the
  new host then publishes its data. Members follow the claim at once. Searchers see the new host
  once the hand-over is done.
- Peer connections go direct when both NATs allow it and through Steam's relay network
  otherwise, no port needs forwarding. `"network": { "relay_only": true }` (or
  `XlsSetP2PTransport`) forces the relay path, which is useful to test what players behind
  strict NATs get. `XlsPeerConnectionInfo` and `XlsSocketConnectionInfo` report which path a
  connection took.
- `XLiveUninitialize` calls `SteamAPI_Shutdown` only when the process never addressed a peer.
  After online play the Steam API stays up until the process exits. Closed peer connections keep
  handshaking on Steam's networking thread for a few seconds, and shutting the API down under
  them crashes inside Steam. Process exit ends that thread first. `steam.shutdown_api` = false
  skips the call in every case.
- Voice needs no setup. Remote talkers play through waveOut, one stream each. The wrapper does
  not feed the decoded PCM into the title's own audio graph.
- Steam Cloud is used only when the app reports an API quota. An app with Cloud switched on but
  no quota accepts writes that are gone on the next start, so such apps keep every file in
  `xlive-storage\<account id>\` next to the exe from the first write.
- `XStorage*` reads of other players' files fail with file-not-found since Steam Cloud is per
  account. Titles that shared ghosts or replays that way need Steam UGC, outside this wrapper.
- Linux runs through Proton unchanged: the wrapper depends on Win32 and `steam_api.dll` only,
  and Proton's `lsteamclient` bridges that dll to the native Steam client (SDK 1.65 from Proton
  10 on, 1.62 in Proton 9, the README covers choosing the SDK). The overlay is Steam's own and
  is not shipped with the wrapper. Launch the title through Steam so the app id and the overlay
  come from the client.

## 7. Building

```
cmake -B build -G "Visual Studio 16 2019" -A Win32 -DSTEAMWORKS_SDK_DIR=path/to/sdk
cmake --build build --config Release
```

Output is `bin/xlive.dll` (or `bin/xlive.lib`), with `steam_api.dll` and the example config copied
next to it.
