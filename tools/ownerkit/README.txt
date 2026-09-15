================================================================================
 xlive-steamworks - Street Fighter X Tekken test kit
================================================================================

Thank you for helping test. This runs Street Fighter X Tekken's online code on
Steam instead of Games for Windows LIVE, using your Steam account and your copy
of the game on Steam.

You need:
  - Steam running and logged in.
  - This Steam account must OWN Street Fighter X Tekken (app 209120).
  - Your installed Street Fighter X Tekken (the GFWL build: the folder that has
    SFTK.exe and xlive.dll in it).

Nothing here is installed system-wide. It writes only inside its own folder and,
for the game test, inside your game folder (after backing up the original file).

--------------------------------------------------------------------------------
 STEP 1  -  Run the probe          (about one minute, always do this)
--------------------------------------------------------------------------------
Double-click:   1_run_probe.bat

It checks what Steam gives the game (your account, ownership, achievements,
Cloud, lobbies, the relay network) and writes  results\probe_report.txt .
It does NOT change the game and creates nothing permanent on Steam.

--------------------------------------------------------------------------------
 STEP 2  -  Test the game          (optional, needs the game installed)
--------------------------------------------------------------------------------
Double-click:   2_install_and_play.bat

It will ask for your Street Fighter X Tekken folder (the one with SFTK.exe).
You can also drag that folder onto the .bat file.

It backs up the game's original xlive.dll to  xlive.dll.orig-backup , copies the
new xlive.dll and steam_api.dll in, then starts the game. Play a little, try the
online / ranked / lobby menus, then quit the game normally.

When the game closes it copies the game's log into  results\ .

To put the game back exactly as it was:   3_restore_game.bat

--------------------------------------------------------------------------------
 STEP 3  -  Send the results back
--------------------------------------------------------------------------------
Double-click:   4_zip_results.bat

It makes  sfxt-results.zip  in this folder. Send that one file back.
It contains: the probe report, the game log, and a short system-info text
(Windows version, GPU, whether Steam was running). It does not contain any
password or personal file.

Questions or a crash? Send sfxt-results.zip and describe what you saw on screen.
