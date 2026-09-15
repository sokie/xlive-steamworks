xlive-steamworks two-PC network test (runs as Steam app 480, Spacewar, for testing only)

WHAT IT PROVES
  Two PCs on different networks find each other through a filtered session search,
  exchange UDP and TCP traffic through the GFWL socket API, report whether Steam
  connected them directly or through a Steam relay, swap voice frames, and run host
  migration in both directions. A second run forces every connection through a
  relay, which is the path players behind strict NATs get.

  Nothing needs to be opened or forwarded on any router or firewall.

HOW TO RUN (both PCs, Steam running and logged in)
  1. Agree on a 4-digit code with the other tester (for example 4242).
  2. PC A: double-click  1_host.bat  and type the code.
     PC B: double-click  2_join.bat  and type the same code.
     Wait until both windows say "PAIR RESULT". About 2 minutes.
  3. Repeat with  3_host_relay_only.bat  and  4_join_relay_only.bat .
  4. Each PC: double-click  5_zip_results.bat  and send back  pair-results.zip .

  Start the host first. The joiner waits up to 5 minutes for the host to appear.

NOTES
  The test creates a temporary lobby under app 480 and deletes it. It writes nothing
  to your Steam profile. A microphone is optional, without one the voice step
  reports 0 frames sent, which is expected.
