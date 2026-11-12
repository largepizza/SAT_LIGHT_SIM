# Launch gate serves waiters first come, first served (2026-11-11)

- `tools/harness/launchgate.py` `wait()`: each waiting launcher (run.py, live.py start, run_latest.py) takes a ticket
  file in `%LOCALAPPDATA%/SatLightSimHarness/queue/` (name = arrival time in ns + pid) and refreshes it every 2 s; only
  the oldest live ticket may take the launch lock. Tickets older than 20 s (launcher died) are reaped; the ticket is
  removed on launch or on any exit from wait().
- Why: with one app machine-wide, three QA agents shared the gate and the first to poll after a slot freed won; the
  smalls agent of the 2026-11-11 wave waited ~1 h 45 min while two others alternated.
- Tested: 4 simulated launchers launched in arrival order; a stale ticket ahead is reaped in < 1 s.
- Page: the harness / launch spacing page.
