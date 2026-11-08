# The cumulus cell volume is read in the shape frame (no rows from orbit)

Date: 2026-11-07 (branch weather-v2). Pages: clouds (noise volumes, tiling), from-orbit appearance.

- What changed: the 32-km cumulus cell volume is now read through the same observer-chosen rotation as the low shape
  volume (and the same per-ray fade when that rotation changes). Weather v2 fixed the cloud frame to the Earth, so the tile's
  rows stood still along the equator; from orbit the cumulus field showed straight north-south streaks and a ~100-km
  lattice (issue_20261107_125245, "noise repetition").
- Cost: unchanged (cloud march registers 250, the artifact gate's numbers within noise).
- Also (71dd391): under a raining or snowing base, rain with scud now strides like rain (cloud march 40 -> 33 ms looking up
  from 500 m under a snowing deck); the scud is slightly softer.
