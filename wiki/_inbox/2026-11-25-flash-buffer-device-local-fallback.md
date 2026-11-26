---
pages: [development/gpu-conventions.md, development/profiling.md]
code: [src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The clouds' flash buffer (the Sun shadow lattices every lit cloud sample and god-ray pixel reads, plus the flash
list, rain map and eval counters) is always in device-local memory. The preferred type is device-local +
host-visible (the BAR heap, or all of VRAM with Resizable BAR), which keeps a CPU mapping for the harness's
`state sun_volume=1` readout. Where no such type exists, it is plain device-local memory zeroed by
`vkCmdFillBuffer`, with no mapping. The CPU's writes are transfer patches / fills, and its reads are a per-slot copy of the buffer's
head, so neither placement changes them. The startup log line names the placement: `clouds v2: flash buffer in
device-local host-visible memory` / `device-local memory (not mapped)` / `host memory`.
`SATLIGHTSIM_FLASH_DEVONLY=1` forces the unmapped placement and `SATLIGHTSIM_FLASH_HOSTMEM=1` the old system-RAM one (A/B).

## History
Until 2026-11-25 the fallback was host memory. A GTX 970M on driver 441 (Vulkan 1.1) had no device-local +
host-visible type, so it got host memory and ran Low at 1-2 fps. The cloud passes there were 76x the 3070 Ti's,
against 5-9x for the rest of the frame (issues 20261125_171559 / _172129). On the 3070 Ti, typhoon view, Low:
cloud bucket 6.9-7.2 ms BAR, 6.5-6.7 ms device-only, 10.6-10.8 ms host; the device-only frame is bit-identical.
