---
pages: [simulation/orbits.md, simulation/time-and-frames.md, modding/constellations.md, development/architecture.md]
code: [shaders/sat_rebake.comp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The orbit buffer's epoch still moves every 7 sim-days (deltaT stays small for float), but the rebake is a
GPU dispatch (`sat_rebake.comp`): it rewrites each satellite's u0, tumble phase and SSO RAAN from
per-satellite epoch-free bases (`satPhaseBaseBuf`, 12 B each) in exact fixed-point turns. Each float
becomes a 64-bit fixed-point number of turns built from its own bits, and rate × epoch is taken
mod one turn as an integer product with the epoch in whole seconds. The result matches the CPU's
double formula to float rounding at any date (GPU vs CPU mirror within ~2.4 m of range at 1995, 2037
and 2061). The Performance tab and harness `perf` report its CPU cost as `orbit_rebake`.

The roster can change at runtime: `rebuildRoster()` rebuilds every satellite and the per-satellite GPU
buffers. Harness `roster list | remove "<name>" | add "<name>"` drives it; there is no UI yet, and a
removal lasts only for the session. Each constellation draws its random numbers from its own stream
(`SatRng`, an emulation of MSVC's rand() LCG). The stream state at its start is recorded on the first
build, and a constellation added later seeds from a hash of its name. The per-cluster jitter is keyed
by load order and terminator planes by the Sun at the sim's start. So a constellation keeps exactly the
same satellites when others are removed or re-added; only satellite indices move. The selection,
follow, track and viewer are dropped on a rebuild. The layout is now the same on every platform.

## History
Until 2026-10-15 the rebake was a CPU loop in double plus a full synchronous upload of the orbit buffer.
At the shipped 1.37M satellites that was ~27 ms per rebake, which runs every other frame at 1 yr/s:
13.7 ms a frame on average, a 28.6 ms frame against a 12.5 ms GPU (RTX 3070 Ti). It is now 16.7 ms
(vsync). buildOrbits() used the global CRT rand() stream. That reshuffled every later constellation
whenever an earlier one changed, and Linux/macOS drew a different sky than Windows. The new stream
reproduces the Windows layout bit for bit. This work is the groundwork for the Kessler / "The Sun's
Fury" scenario (branch kessler).
