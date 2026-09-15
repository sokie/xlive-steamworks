xlive-steamworks probe
======================

This checks what a Steam app offers to a Games for Windows LIVE title running on the
xlive-steamworks wrapper. It creates a temporary lobby and reads Steam settings, it does not
create leaderboards, does not unlock achievements and removes the two small files it writes to
your Steam Cloud for the app.

How to run
----------
1. Start Steam and log in with an account that owns the game.
2. Double-click run_probe.bat. It runs for about a minute and prints the results.
3. Send back probe_report.txt (it contains your Steam name and id, your friends' names, and the
   wrapper's log, nothing else).

Options
-------
run_probe.bat 209120                       probe a different app id
run_probe.bat 209120 "C:\Games\SFxT\SFTK.exe"   also read the game's own achievement list

If the first lines say Steam did not initialise, the account does not own that app id (or Steam
is not running), the log at the end of the report shows Steam's exact message.
