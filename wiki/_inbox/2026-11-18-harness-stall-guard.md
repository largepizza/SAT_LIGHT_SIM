---
pages: [development/harness.md, development/tools.md]
code: [src/Harness.cpp, src/simulations/SatelliteSimHarness.cpp, src/simulations/SatelliteSimKessler.cpp, tools/make_kessler_prebake.py, tools/harness/selftest.py]
---
## Now
Every harness command has a wall-clock budget when it starts (`Runner::tick`, src/Harness.cpp). One still Pending past
it fails with an error naming the command, its wall time, its frames and its progress (`budget_exceeded`, `budget_s`,
`wall_s`, `progress` in the result; summary.json `budget_failures`), and the script runs on. `maxs=<s>` on any command
is its budget (0 = none); `limit cmd=<s>` sets the default for the rest of a script (120 s); the long commands have
their own defaults (photo and video stop 600 s, path play / bulkexport / sweep 1800 s, cine play|export 3600 s, perf
30 s + 0.5 s a frame). `wait N` and `wait settle` keep their own soft cap (`maxs=`, default 2 s).

`kessler wait days=<d>` reports `prebake_hit`, `world_hash`, `cache_source`, `eta_s`, `live_s_per_day`; after 4 s of
live computation (past the cache, ~1 s per simulated day) it fails at once when the ETA exceeds the budget left, naming
the world hash and the shipped prebake's. `kessler prebake [require=1]` is the staleness check: the world hash against
the prebakes in `<exe>/kessler_prebake/`; the self-test runs it at the built-in defaults, and the app logs
`kessler: WARNING the shipped prebake ... is not this world's` once per world. `results.jsonl` `wall_ms` is the
command's own time (it used to count from the start of the frame).

## History
2026-11-18: event_windows.satcmd took 16 min, 989 s of it in one `kessler wait days=1100` with the clock paused: the
shipped prebake (36752c04, 2026-11-04) no longer matched the world (984125cb), moved by the Reflect Orbital commits of
2026-11-04 (5bbfa392, 90e775b6, 59bf9104), so the cascade was computed live. The prebake was rebuilt (see the
kessler.md note for its hash, size and rebuild time).
