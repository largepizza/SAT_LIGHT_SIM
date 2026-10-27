---
pages: [simulation/kessler.md, simulation/weather.md, development/profiling.md, development/gpu-conventions.md]
code: [src/simulations/SatelliteSimKessler.cpp, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimWeather.cpp]
---
## Now
A Kessler GPU pool growth during a frame never blocks it. The five descriptor sets that bind the pool's buffers
(descSet, orbitDescSet, satRebakeDescSet, citySpriteDescSet, kFireDescSet) are re-pointed by copying them into
fresh sets from a pool of their own, writing the new buffers there and swapping the handles
(`swapSatRosterDescriptors`); the old sets stay valid for the frames still in flight and their pool is destroyed a few
frames later. The mirror's sync stops at the first new record past the GPU pool's capacity while the buffers allocated
ahead are not ready (`kesslerGrowReady`), so sim time holds at the mirrored frontier instead of the frame waiting.
Harness `kessler state` -> `pool_capacity_gpu`, `grow_holds`. Profiling sections `kessler.growWaitPrealloc` (the
allocation only) and `kessler.growDescriptors`.

The weather bake's envelope-index job is waited for at most 1 ms at the frame's end (`kStormEnvJoinWaitMs`, a spin:
MSVC's timed wait oversleeps). Still running, the frame's bake reads the previous index; the next frame joins it,
uploads it and starts no new job. Harness `state` -> `weather.cube.env_late`, section `storms.envJoinWait`.

## History
hitch_check D at 1 d/s after the CME: `kessler.growWaitPrealloc` 24-62 ms (122 ms worst frame). The allocation was
not the cost (0.5-2.6 ms on the worker); the in-place descriptor update drained both frames in flight (~50 ms of
GPU each at that warp). D 1d worst frame 123 -> 79 ms (now the GPU's fence wait). The env join had waited 11.7 ms
once at 1 mo/s; after: at most 1.0 ms, late on 3-7 of 180 frames at 1 w/s and 1 mo/s.
