# Orbits, the GPU orbital pipeline, reflectors and beams

Constellation distributions, the sat_orbit / sat_flare dispatches and buffer layouts, push constants, TargetedReflector targeting, Reflect-Orbital beams and their cloud occlusion.

Part of the subsystem notes (`docs/subsystems/`, map in `AGENTS.md` §4). Moved verbatim from
`CLAUDE.md` on 2026-11-03. Do not read this file whole: `python tools/outline.py docs/subsystems/orbits.md`
lists its sections and topics with line numbers. New notes go next to the section they belong to.

## Subsystem: Orbital Mechanics / Constellations

### Orbit distributions
- **Walker** — `numPlanes × perPlane` satellites, evenly spaced RAAN, random phase per plane
- **RandomShell** — random RAAN, random incl in [0, c.incl], jittered altitude, random tumble axis
- **Disk** — concentric rings in a single orbital plane (incl + raan). `alignTerminator=true` derives incl/raan from sunDirECI at J2000 epoch and precesses RAAN at SSO rate (kSSOPrecRate = 2π/year)
- **Shells** (2026-10-06) — a filed shell table in ONE roster entry (`groups`: alt range, incl range or
  `sun_synchronous`, shells, planes per shell, satellites per plane; `ShellGroup`). Non-SSO groups are Walker
  shells (each shell's planes staggered); SSO groups take each shell's J2 inclination and spread their planes
  from the dusk node (LTAN 18:00), so 2 planes per shell = the 18:00 + 06:00 **X-ring** crossing at the
  equator. One entry rather than one per group (a roster entry is one Show / Highlight bit; see List below for the
  limit, now 256).
- **List** (2026-10-20, issue 20261020_190331 "GEO Satellites") — named MEMBERS, each its own satellite
  (`ConstMember`: name, operator, class, purpose, its own type), inline `members` or a `members_file` next to the
  exe; a member's type is its `type`, else `class_types[class]`, else the entry's. A member with `lon_deg` (no
  `alt_km`) is GEOSTATIONARY: buildOrbits puts it at the radius whose float mean motion is the Earth's rotation
  rate and solves its J2000 phase so that at the build's sim time it is over its slot (u0 = lon + ERA(t) - raan -
  n t; inclination 0.02-0.08 deg hashed unless given, node hashed); collocated members are spread +-0.06 deg in
  their box. Drift < 0.01 deg a year (harness `state` -> `selection.member.lon_now_deg`: GOES-19 -75.200 /
  -75.199 / -75.196 over a year). No J2, no station keeping — nothing else moves a roster record. The roster's
  **GEO belt** is one List entry, APPENDED last (so no earlier entry's random stream, `defIndex` or Kessler
  history moves): 398 real satellites from `data/geo_satellites.json` (written by `tools/make_geo_belt.py`: name,
  operator, slot, class, purpose; approximate mid-2020s slots, see its header), flying four generic models
  (`tools/make_geo_models.py`: `geo_comsat_large` 205, `geo_comsat_medium` 168 incl. "small", `geo_weather` 20,
  `geo_spinner` 5) baked at the 48-lobe budget (`geo_` prefix: ~200 are above any observer's horizon every
  night; 112 exact lobes cost ~0.3 ms of orbit dispatch on the ground). `satLabel()` / the selection panel /
  the info window show a member's own name (`rosterMemberName()`; a one-satellite entry, a station, shows its
  entry's name, no "#0"), with operator, mission and slot (`rosterMember()`). Harness `select const "GEO belt"
  n=<k>`; `state` -> `selection.member`.
- **Show / Highlight bits** (2026-10-20): `GpuSatTypeHeader::constEnabled[8]` / `constHighlight[8]` (bit c & 31 of
  word c >> 5; 256 constellations, the width of a record's constIdx byte: `kMaxConstellationBits`), read in
  `sat_orbit.comp` by `CONST_BIT`. They replaced `SatOrbitPC`'s two 32-bit masks (now `padMask0/1`): every entry
  past the 32nd — and every Kessler / launch record carrying one (constIdx & 31 wrapped onto another entry) — read
  another entry's bit.
- **Clusters** (`cluster_size`; Walker, Disk, Shells): each plane's satellites fly in formation clusters,
  evenly spaced. 1 = off (Walker keeps random phases). `cluster_shape` "line": `cluster_spacing_km` apart along
  track; "ring" (2026-10-06 rework): a PASSIVE formation — every member takes the cluster CENTRE's circular
  orbit (raan/incl/u0/altM; R_sat and meanMot are the centre's) plus the bounded, drift-free Clohessy-Wiltshire
  (Hill) relative orbit about it, in the centre's radial / along-track / cross-track frame, theta = u + delta:
  x = a cos theta, y = -2a sin theta, z = k x (a = rho/2, rho = `cluster_radius_km`; k from
  `cluster_relative_orbit`: circular sqrt 3 = a circle of radius rho tilted 60 deg out of the orbit plane about
  the along-track axis, every member rho from the centre — the default; projected_circular 2; in_plane 0 = the
  2:1 ellipse). Members are evenly spaced in delta from a per-cluster start. A member below the centre runs ahead,
  one above falls behind, and the formation turns once per orbit (the user's "ring rotating on axis"). Stored as
  `SatOrbit::formation/formP/formQ/formK` (P = a cos delta, Q = a sin delta, so x = P cos u - Q sin u and
  y = -2 (P sin u + Q cos u): no trig per satellite), baked into GpuSatOrbit's tumble slots with
  `kGpuFormationBit` in constIdx (see the layout below); `formationOffset()` in sat_orbit.comp, mirrored in
  `satOrbitStateAt()` (elems `formation/formP/formQ/formK`, traces `orbit_formation[_pqk]`). The centre's phase
  keeps `orbitPhase`'s exact two-float product; the offset is added after it. Velocity (attitude) stays the
  centre's (the relative speed is ~n rho < 1 m/s). The first cut (until 2026-10-06) was a rigid polygon held
  together by giving each member its centre's mean motion at its own radius ("magic").
- **Starmind** (SpaceX orbital data centers) follows the 29 May 2026 letter to the FCC, Table 1
  (SAT-LOA-20260108-00016; transcribed in github.com/sdross0/orbital-datacenter-brightness ASSUMPTIONS.md
  rev. 2): SSO 565-585 km (10 shells), 707-744 (22), 967-1002 (22), 2 planes each; 30 deg at 550-568 km
  (10 shells, 26-32 deg), 686-718 (25), 946-978 (25), 30 planes x 333. The rows are per-group MAXIMA summing to
  1,198,120 against a 1,000,000 cap, so `per_plane` is scaled by 1e6 / 1,198,120 and rounded down to whole
  clusters (988,672 flown). Rings of 8 (the user's reading of SpaceX's visualization; SpaceX's text says "10-ish
  satellites" per cluster), circular relative orbits of rho = 0.35 km (700 m across: the formations are
  reportedly under 750 m; neighbours 268 m apart). It was 2 km in the first cut, where each cluster RESOLVED into
  a small ring of points from the ground ("stars drawn as hollow rings"); at 350 m a cluster is about one point
  at a normal FOV. **Per-cluster jitter** (Shells only, `cluster_alt_jitter_km` 0.25, `cluster_phase_jitter`
  0.25 of the gap between clusters (~80 km), `cluster_raan_jitter_deg` 0.15, `cluster_incl_jitter_deg` 0.03;
  deterministic hash of (entry, cluster), not rand()) breaks the perfectly even strings of clusters along each
  plane. Filed RAAN tolerance +-30 deg: modelled
  per plane via `raan_spread_deg` (10 in the roster; `raan_spread` even = golden-ratio sequence | random). It
  replaced a single 1M-satellite dawn-dusk Disk (575-1925 km, 2000 rings). **Why the spread matters:** with
  tight nodes every shell of a node family shares ONE plane, and shells 1.6-1.8 km apart with 2-km rings
  interpenetrate, sliding through each other at their different rates — 13,000 pairs of satellites from
  different clusters within 1 km at any instant (scratch rebuild of the same orbits, KD-tree, 12 instants); +-10
  even: 26; +-30 even: 12 (random +-30: 25, it clumps); the 30 deg shells (stagger only): 30.

### ConstellationConfig field order
```cpp
// Walker:
{ name, altM, incl, numPlanes, perPlane, typeIdx, enabled, OrbitDistribution::Walker }

// Disk (extra trailing fields):
{ name, altM, incl, numPlanes, perPlane, typeIdx, enabled, OrbitDistribution::Disk,
  altJitterM, raan, alignTerminator, numRings, ringSpacingM }
```
- `perPlane` is **never** ignored — total = `numPlanes × perPlane` for all distributions
- `incl` is ignored when `alignTerminator=true`

### Adding a new constellation
1. Add a `ConstellationConfig` entry to `constellations.json` (or `loadHardcoded()`) — APPEND it: an entry inserted
   mid-roster moves every later entry's `defIndex` (cluster jitter), the Kessler history and the launches' planes
   (bump the Kessler `kCacheVersion` when the roster changes)
2. Give it a `"category"` (Settings > Constellations groups by it: Broadband, Stations, Orbital data centers,
   Reflectors, GEO, Science, Benchmarks, Debris; any other name makes a new group; absent = `constCategoryOf()`
   infers one from the name). Rows are hover-keyed through `uiHov_`, so there is no per-entry hover state to size
3. `MAX_SATELLITES = 10,000,000` — cap is generous; only relevant for very large test configs

### Current constellation roster (9 total, hardcoded fallback)
| Name | Sats | Alt (km) | Incl | Dist | TypeIdx |
|------|------|----------|------|------|---------|
| Starlink Gen1 | 4,392 | 550 | 53° | Walker | 0 |
| Starlink Gen2 | 30,480 | 525 | 53.2° | Walker | 7 (KnifeEdge) |
| OneWeb | 648 | 1,200 | 87.9° | Walker | 1 |
| Amazon LEO | 7,742 | 630 | 51.9° | Walker | 1 |
| Guowang | 13,920 | 508 | 85° | Walker | 1 |
| ISS | 1 | 408 | 51.6° | Walker | 3 |
| SpaceX AI Sat | 20,000 | 575–1,925 | SSO | Disk+terminator, 10 rings | 4 |
| Reflect Orbital | 1,000 | 500 | SSO | Disk+terminator, 10 rings | 5 |
| Space Junk | 3,000 | ~1,000 | random 0–180° | RandomShell | 6 |

CPU `updatePositions()` is now **O(1)** — it only updates sun/moon/obsECI/eci2enu and uploads `reflectorTargetsBuf`. All orbital mechanics run on GPU via `sat_orbit.comp`.

### SSO precession model (alignTerminator=true)
Inclination from J2 formula: `cos(i) = -kSSOPrecRate / (1.5 × n × kJ2 × (Re/a)²)`
RAAN anchored at **sim-start** using `sunDirECI` (set by `updatePositions()` before `initConstellation()`): `raan_start = atan2(sunDirECI.x, -sunDirECI.y)`. GPU formula: `liveRaan = raan_start + kSSOPrecRate × (simTime − t_start)`. Anchoring at sim-start avoids the ~3° obliquity-driven phase error that accumulates when extrapolating from J2000 to a solstice epoch.

---

## Subsystem: GPU Orbital Pipeline

All per-satellite orbital mechanics, attitude and reflectance computation runs on the GPU. The CPU only manages the small reflector targets buffer and triggers a rebake when needed.

**Lighting overhaul Phase 1 (2026-09-22, `.plans/SAT_LIGHTING_PLAN.md`).** At millions of
satellites the per-satellite cost was memory traffic, not math: ~304 B/satellite/frame (112 B orbit
read + an 80 B `GpuSatInput` written by `sat_orbit.comp` only to be read straight back by
`sat_flare.comp` + 32 B visible). Now 128 B (culled) to 160 B (lit: 64 B orbit + 32 B visible
write + 32 B read + 32 B in-place rewrite): the per-type fields moved out of every orbit record
into `satTypeBuf` (orbit 112 → 64 B), the reflectance model moved into `sat_orbit.comp`, and
`GpuSatInput` was deleted. `sat_flare.comp` is still a separate dispatch **on purpose**: its
photometry reads `beamGlowDome`, which `sat_orbit.comp` builds by `atomicMax` across every
TargetedReflector satellite, so it isn't complete until that whole dispatch is. Merging the two
would need a one-frame-stale (double-buffered) dome.

**Phase 1b — compact visible list (2026-09-23).** Measured at 9.88M satellites on an RTX 3070 Ti
after Phase 1: `sat_orbit` 1.7 ms (≈90% of the card's 608 GB/s — at the memory wall), but
`sat_flare` 1.27 ms + the satellite draws 0.7 ms were spent almost entirely on the ~90% of
satellites below the horizon. Now `sat_orbit.comp` **appends** only surviving satellites to a
compact list (`satVisibleBuf` records + `satVisibleIdxBuf` slot→satellite index), aggregated per
workgroup through shared memory (one global `atomicAdd` per 64 satellites — no subgroup ops, so
nothing to check on MoltenVK). It also maintains the indirect args in `satListBuf`
(`GpuSatListHeader`) by `atomicMax` on each workgroup's append end — slots are contiguous from 0,
so the largest end *is* the count, no separate args pass. `sat_flare.comp` is
`vkCmdDispatchIndirect`, and the satellite point, flare-source and trail draws are
`vkCmdDrawIndirect`, all scaling with the visible count.
Invariants:
- **Slot order is nondeterministic** (atomic append). Fine only because every satellite point
  pipeline blends additively (`ONE`/`ONE`, no depth write). Never key anything per-satellite off
  `gl_VertexIndex` in those shaders — it is a list slot, not a satellite.
- **`satVisibleBuf` is no longer indexed by satellite.** Selection tracking goes through
  `SatFlarePC::selectedSatIdx` → `sat_flare.comp` writes that satellite's final record into
  `GpuSatListHeader::selected`; the whole 80-byte header is copied into `pickedVisibleBuf` every
  frame (it also carries the real `visibleCount` — the snapshot's `visible_count` was a placeholder
  equal to the roster size until this). Picking copies back only `count` entries + their indices.
- **The sun's flare-source point is its own `vkCmdDraw(1 vertex, firstInstance 1)`**;
  `flare_source.vert` tells it apart by `gl_InstanceIndex`, since the satellite count is GPU-side.
- The header is reset each frame with `vkCmdUpdateBuffer` before `sat_orbit`; the post-orbit
  barrier includes `INDIRECT_COMMAND_READ` at `DRAW_INDIRECT`, which also covers the later draws.

### Two-dispatch pattern (recordCompute)
```
header → satTypeBuf       → CPU memcpy of GpuSatTypeHeader (brightnessScale, mirrorBoost)
vkCmdUpdateBuffer(satListBuf) → reset list header {0, dispatch 0,1,1, draw 0,1,0,0}; barrier →compute
sat_orbit.comp dispatch   → reads satOrbitBuf + satTypeBuf + reflectorTargetsECEFBuf; APPENDS
                            visible satellites to satVisibleBuf/satVisibleIdxBuf, args to satListBuf
                            (+ beams, beamGlowDome)
barrier (3 buffers)       → SHADER_WRITE → SHADER_READ|WRITE (+INDIRECT_COMMAND_READ on satListBuf),
                            compute → compute|DRAW_INDIRECT
vkCmdFillBuffer(glowBuf)  → zeros the glow histogram for this frame
barrier glowBuf           → TRANSFER_WRITE → SHADER_READ|SHADER_WRITE, transfer→compute
sat_flare.comp INDIRECT   → finishes the compact list IN PLACE; glowBuf (atomicMax) + ocean glints;
                            selected satellite's record → satListBuf.selected
barrier satVisibleBuf/satListBuf → SHADER_WRITE → SHADER_READ (vertex) / TRANSFER_READ
point / flare-source / trail draws → vkCmdDrawIndirect(satListBuf.draw*)
end of recordCompute      → copy satListBuf header → pickedVisibleBuf (host, read next frame)
```

### Buffers
| Buffer | Memory | Lifetime | Updated by |
|--------|--------|----------|------------|
| `satOrbitBuf` | device-local | uploaded once; rebaked every 7 sim-days | `uploadSatOrbits()` |
| `satTypeBuf` | host-coherent, mapped | header per-frame; types on upload/rebake | `recordCompute()` / `uploadSatOrbits()` |
| `satVisibleBuf` | device-local | per-frame | compact list: `sat_orbit.comp` appends, `sat_flare.comp` finishes in place |
| `satVisibleIdxBuf` | device-local | per-frame | `sat_orbit.comp` (slot → satellite index) |
| `satListBuf` | device-local, INDIRECT | per-frame | reset by `vkCmdUpdateBuffer`; `sat_orbit.comp` (count/args), `sat_flare.comp` (`selected`) |
| `pickedVisibleBuf` | host-coherent, mapped | per-frame | copy of `satListBuf`'s 80-byte header |
| `reflectorTargetsECEFBuf` | host-visible, mapped | uploaded once at target-generation time | `loadReflectorTargets()`/fallback |
| `glowBuf` | host-coherent, mapped | per-frame | `sat_flare.comp` write; App reads back |

`satOrbitBuf`/`satTypeBuf`/`satVisibleBuf`/`satVisibleIdxBuf`/`satListBuf` are created by
`createSatBuffers()`, sized to the loaded roster — which is why `init()` builds the constellation
**before** `createDescriptors()`. `uploadSatOrbits()` bakes straight into a reused 64 MB staging
chunk (it used to hold two full-size copies — 1.3 GB at 10M) and `buildOrbits()` reserves the exact
total, so a 10M roster's startup no longer spikes host memory.

`mirrorNormalsBuf` (persistent per-satellite mirror lock/slew state) and the old per-frame
CPU-compacted `reflectorTargetsBuf` were both removed 2026-08-06 — see "Subsystem: TargetedReflector
/ Mirror Ground Targets" below. `sat_orbit.comp` now reads target data from a static
`reflectorTargetsECEFBuf` (uploaded once at target-generation time), and carries no persisted GPU
state of its own: every frame's TargetedReflector selection and orientation is a pure function of
that frame's push constants.

### Orbit rebake
`kOrbitRebakeDays = 7`. Each `GpuSatOrbit` bakes `u0 = fmod(orig_u0 + meanMot × epochT0, 2π)` so the shader only adds `meanMot × deltaT` where deltaT < 7×86400 s. `uploadSatOrbits()` auto-triggers in `recordCompute()` when `|simDayJ2000 - orbitEpochDay| >= 7`.

**That product is ~700 rad by day 7, and in plain float it was off by a median 60 m and up to
~770 m along-track** — invisible as a point from the ground, fatal for a mesh next to its own
sprite (Phase 4). `orbitPhase()` (`sat_orbit.comp`) now computes it as an exact two-float product:
`deltaT` arrives as `pc.deltaT` + `GpuSatTypeHeader::deltaTLo` (the low half of the CPU's double),
Dekker's two-product over a Veltkamp split gives `meanMot·dtHi` exactly, and 2π is subtracted as a
14-significant-bit `TWO_PI_A` + remainder so every step is exact. **It must stay `precise`** (SPIR-V
`NoContraction`, 26 decorations): a compiler that contracts or reassociates it silently loses
everything — and it deliberately does not use `fma()`, which Vulkan does not guarantee is fused.
Verified by float32 emulation over 200k satellites × random deltaT in 0-7 days: median 1.1 m, p99
6 m, max 9 m (the float32 floor of the final angle). The header grew 16 → 32 B for `deltaTLo`;
`kSatTypeArrayOffset` follows it automatically.

### simTime representation
Split into `simDayJ2000` (int64_t days) + `simSecInDay` (double, re-based to [0, 86400) each frame). Avoids accumulated float precision loss when a large J2000 base is added to a small per-frame delta. The shader receives `deltaT = float((dDays × 86400) + dSec)` where dDays < 7 (ensured by rebake).

### GpuSatOrbit layout (64 bytes, std430)
All plain floats/uints — no vec3 — so C++ struct packing matches GLSL std430 with no padding.
Must match `SatOrbit` in `sat_orbit.comp` exactly. Only genuinely per-satellite data belongs here —
anything that is constant per type goes in `GpuSatType`.
```
[ 0] raan, u0, R_sat, meanMot
[16] cosI, sinI, cosRaan, sinRaan
[32] tumbleRate, tumblePhase, tumbleAxisX, tumbleAxisY
[48] tumbleAxisZ, alignTerminator, typeIdx (uint), constIdx (uint)
```
`static_assert(sizeof(GpuSatOrbit) == 64)` — do not change field order without updating both structs.
**Formation ring members** (2026-10-06): `constIdx` carries `kGpuFormationBit` (0x80000000; the shader masks
the index with 31 for the enabled/highlight masks), the tumble slots hold the Clohessy-Wiltshire coefficients
(tumbleAxisX/Y/Z = formP, formQ, formK; tumbleRate/Phase 0), and the Tumble attitude law reads +Z for them —
the CPU's `SatOrbit` keeps its own (unused) tumble fields. `offsetof` asserts pin the slot positions. Since 2026-10-07 `constIdx`
bits 8-15 are the member's cluster size and 16-23 its slot in it (the formation merge, below).

### GpuSatType layout (416 bytes, std430) — `satTypeBuf`
One per `satTypes[]` entry, indexed by `GpuSatOrbit::typeIdx`, at `kSatTypeArrayOffset`: after a
112-byte `GpuSatTypeHeader` (`brightnessScale`, `mirrorBoost`, `occlusionFluxFloor`, `occlusionOn`,
`deltaTLo`, the mesh / formation / Kessler pool fields, and since 2026-10-20 the per-constellation
`constEnabled[8]` / `constHighlight[8]` bits —
rewritten every frame; `SatOrbitPC` is full) and the 448 KB `GpuEarthshineLut` (`earthLut` +
`earthShA/B/C` in the shader, written once by `createSatBuffers()`). Must match
`SatType`/`AttGroup`/`SatTypeBuf` in `sat_orbit.comp` (`offsetof` static_asserts guard the C++ side).
```
[ 0] baseColorR, baseColorG, baseColorB, crossSection
[16] specExp0, specExp1, w1, diffuse
[32] mirrorFrac, groupCount (uint), firstLobe (uint), lobeCount (uint — 0 = legacy path)
[48] surfNormalT0 (vec3, triad coords), surfGroup0 (uint)
[64] surfNormalT1 (vec3, triad coords), surfGroup1 (uint)
[80] groups[4] — GpuAttGroup, 64 B each:
     law, primaryTarget, secondaryTarget, jointMode (uint×4)
     jointAxisT (vec3), jointTarget (uint)
     jointVectorT (vec3), jointLimitRad
     jointAngleRad, parent (uint, 0xFFFFFFFF = root), pad×2
[336] originT[4] (vec4 — each group's rest hinge point, triad coords; Phase 3b)
[400] firstOccluder, occluderCount (uint), pad×2
```
Geometry-model lobes live in `satLobeBuf` (binding 8 of the `sat_orbit` set, host-coherent,
`GpuSatLobe` 64 B: normalT + group, area, diffArea, albedoD, f0, alpha2Mat, sampleFirst,
sampleCount, occluderMask, distribution, transmission, occluderMaskHi, pad), packed per type at `firstLobe`; their occluders and sample points in
`satOccluderBuf`/`satLobeSampleBuf` (bindings 9/10), `sampleFirst` rebased at upload.

### GpuSatVisible layout (32 bytes, std430)
A **compact list** entry: appended by `sat_orbit.comp`, finished in place by `sat_flare.comp`, read
by `sat_point.vert`, `flare_source.vert` and the trail splat pass via indirect draws.
```
[ 0] skyDir (vec3) + flareIntensity (float)  — ENU unit vector + intensity [0,1+]
[16] color (uint, packUnorm4x8), angularSize (float, sprite px), rangeM (float), pad
```
Between the two dispatches it is a **pre-photometry** record (culled satellites are not in the
list at all): each carries the raw flux, with `color` already eclipse-tinted (its ALPHA = the type's area fill,
`GpuSatType::meshFill`) and `angularSize` = +-(1 + the formation merge factor), negative for a highlighted
constellation (2026-10-07; it was -1 / 0). Highlight mode lights satellites normally and raises them to
at least `highlightFlare`; until 2026-09-24 it skipped the lighting and drew every one at that
~mag 5.4 dot, so highlighting a sunlit ISS made it FAINTER. After `sat_flare.comp`, a satellite below
`visThresh` stays in the list as a zero record. Stars and planets use the same record
(`rangeM` = 0, at infinity; `packVisibleColor()` on the CPU). **Phase 4 (2026-09-23):** the tint
was a `vec3`; packing it freed the slot for `rangeM`, which the point draws write as their depth
(see "Unified scene depth").

### GpuSatListHeader layout (80 bytes) — `satListBuf`
```
[ 0] count                                        — append counter = list length
[ 4] dispatchX, dispatchY, dispatchZ              — VkDispatchIndirectCommand (sat_flare)
[16] vertexCount, instanceCount, firstVertex, firstInstance — VkDrawIndirectCommand (3 point draws)
[32] selected (GpuSatVisible)                     — selected satellite's final record, or zeros
[64] selectedRawFlux, selectedRangeM, selectedFound, pad — the selection's PRE-photometry values
                                                    (sat_flare.comp copies them before rewriting)
```
The `selected*` raw fields feed the **GPU-parity readout** (benchmarking M2): where `SatOrbitPC` is
built, `parityPending` records that dispatch's exact inputs (sim time, Sun, observer, tilt,
brightnessScale, mirrorBoost); next frame `updateSelectedPhotometry()` re-evaluates the selection with
`evalSatPhotometry()` at those inputs, shows the physical magnitude in the selection panel, and
computes the GPU−CPU gap (Δmag, a mismatch above `kParityWarnMag` = 0.02 is logged with a 120-frame
cooldown). The gap's readout line was dropped from the panel (2026-09-24): the check and its log
remain, the UI no longer shows it.
Legacy types are compared through the evaluator's `legacyFlux()` mirror (`LegacyReflectance`), in
their own display units. The gap is float-vs-double arithmetic. Until Phase 4 it was chiefly
`u0 + meanMot·deltaT` in float (median 60 m, up to ~770 m along-track by day 7 of a rebake); since
2026-09-23 `orbitPhase()` forms that product exactly (see "Orbit rebake"), leaving ~1 m median /
~9 m max from float32 rounding of the final angle and position.
Mirrored as `SatListBuf` in both `sat_orbit.comp` and `sat_flare.comp`; `offsetof` static_asserts
guard the C++ side.
`static_assert(sizeof(GpuSatVisible) == 32)`

### Push constants

**SatOrbitPC** (128 bytes) — sat_orbit.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — ECI→ENU basis, offsets 0/16/32
sunDirECI (vec3), deltaT (float)       — offset 48/60
obsECI (vec3), satCount (uint)         — offset 64/76
padMask0, padMask1 (uint; the old highlight / enabled masks, 2026-10-20: GpuSatTypeHeader), simDt (float), elevCutoff (float) — offset 80/84/88/92
beamGain (float), reflectorLockWindowS (float), targetCount (uint),
minBeamElevSin (float)                 — offsets 96/100/104/108
gmstNow (float), windowFrac (float), mirrorMaxRateDegPerSec (float), pad2 — offset 112/116/120,
padded to 128
```
2026-08-06 reversibility rework repurposed the fields at offset 100-124 in place (same total size,
no growth): `mirrorSlewDegPerSec` → `reflectorLockWindowS` (a duration instead of a rate — see
below), `activeTargetCount` → `targetCount` (now the full loaded count, not a per-frame
night-side-compacted subset), `mirrorSnap` → `gmstNow` (current-frame GMST, for rotating a target's
static ECEF entry to its live ECI position), `minBeamElevSinRelease` → `windowFrac` (fractional
position within the current lock window, `fract(simTimeAbs / reflectorLockWindowS)`), and (same-day
follow-up) `pad1` → `mirrorMaxRateDegPerSec` — a real angular-rate cap, added once the original
fixed-fraction-of-window crossfade turned out to read as satellites snapping to target (it only
covered one of several transition cases and wasn't derived from actual angular distance — see
"Orientation" below). All of `deltaT`, `gmstNow`, and `windowFrac` are pure functions of absolute
sim time, computed on the CPU in double precision and narrowed to float only after the relevant
periodic reduction — so `sat_orbit.comp` can extrapolate exactly to a lock window's start instant
(see below) with no persisted GPU state anywhere in the pipeline.

**SatFlarePC** (128 bytes) — sat_flare.comp:
```
enuX (vec4), enuY (vec4), enuZ (vec4)  — offsets 0/16/32
sunDirECI (vec3), satCount (uint)      — offset 48/60
obsECI (vec3), selectedSatIdx (uint)  — offset 64/76
pad3, daySuppression, pad4, visThresh, highlightFlare,
extinctionCoeff, moonSuppression, pad0 — offsets 80–108
moonDirECI (vec3), sunRefIntensity     — offset 112/124
```
`pad3`/`pad4` were `brightnessScale`/`mirrorBoost` until the lighting overhaul's Phase 1 moved the
reflectance model into `sat_orbit.comp`; offset 76 was `elevCutoff`, then a pad, and since Phase 1b
is `selectedSatIdx` (`UINT32_MAX` = none).
`pad2` was `lightPollution` — superseded (session 26) by the directional `lightDomeBuf` SSBO
(binding 3 in the sat_flare.comp descriptor set, 8 floats, host-visible/mapped), which doesn't
need push-constant space. See "Subsystem: Light Pollution Dome" below.

**SatDrawPC** (128 bytes) — `sat_sky.vert`/`.frag` (+ `_lite`/`_minimal`) via `skyBgPipeLayout`:
```
skyView (mat4)                          — offset 0
fovYRad, aspect, gmst, waveTime         — offsets 64/68/72/76
sunDirENU (vec4) — xyz=dir, w=sin(el)  — offset 80
moonDirENU (vec4) — xyz=dir, w=illum   — offset 96
obsECEFDir (vec4) — xyz=ECEF, w=obsHeightOffset — offset 112
```
Exactly the 128-byte `maxPushConstantsSize` floor (oldest AMD integrated parts). Everything that
used to trail past offset 128 — `debugDisableMask`, `screenSizePx`, `skyGlareVisibility`, the four
`beam*` scalars, `mwSuppressEased` — was per-frame-uniform and moved into the **CloudParams UBO**
(`cloud_params.glsl` / `GpuCloudParams` "Push-constant relief" block): `sat_sky.frag` reads them as
`cloud.dbgDisableMask` / `vec2(cloud.skyScreenW, cloud.skyScreenH)` / `cloud.skyGlareVisibility` /
`cloud.beam*` / `cloud.mwSuppressEased`. `buildSkyDrawPC()` fills it.

**PointDrawPC** (128 bytes) — `sat_point.vert`/`.frag` (`drawPipeLayout`) + `star_point.vert`/`.frag`
(`starPipeLayout`, also planets/trail):
```
skyView (mat4)                          — offset 0
fovYRad, aspect, waveTime, noTwinkle    — offsets 64/68/72/76
moonDirENU (vec4)                       — offset 80
obsECEFDir (vec4) — w=obsHeightOffset   — offset 96
screenSizePx (vec2)                     — offset 112   (always ctx.swapExtent — point draws never scale)
debugDisableMask (uint)                 — offset 120   (only sat_point.frag's knockout bit 4096)
manualTerrainTest (float)               — offset 124   (1 on the trail draws only)
```
Split from `SatDrawPC` so both point pipeline layouts fit 128 bytes while still carrying the two
per-draw flags that genuinely can't be frame-UBO'd: `noTwinkle`=1 on the planet draw, and
`manualTerrainTest`=1 on the trail draws — both differ between draws within one frame.
`buildPointDrawPC()` fills it; callers set the two flags. Each point shader declares only the
prefix it reads.

---

## Subsystem: TargetedReflector / Mirror Ground Targets

Mirrors in `TargetedReflector` mode aim at a ground target chosen by each satellite. All
per-satellite selection and orientation computation runs in `sat_orbit.comp`.

**2026-08-06 reversibility rework.** The previous design (lock + acquire/release hysteresis in
`mirrorNormalsBuf`, rate-limited slew integrated frame-by-frame) was persistent, history-dependent
GPU state: which target a satellite held and how far its mirror had slewed toward it were both a
function of the *sequence of frames* used to reach the current sim time, not of that sim time
alone. Playing forward to an instant and reaching the same instant by reversing time therefore
accumulated different lock/slew histories and could show different satellite/target pairings —
structural, not a tunable-away bug, since hysteresis is deliberately not time-symmetric. Separately,
the per-(satellite,target) preference hash (`hash11(float) `on a large combined index) silently
collapsed to a constant `0.0` for every candidate once a satellite's global dispatch index exceeded
~15,000 float32 mantissa range — Reflect Orbital satellites sit at index ~57,000+ given the
constellation upload order, so EVERY score was tied and argmax degenerated to "first eligible
candidate in scan order," meaning only the lowest-original-index site among a satellite's
simultaneously-eligible set ever won and other co-eligible sites were never picked.

Both are fixed together: `mirrorNormalsBuf` and the old per-frame CPU-compacted
`reflectorTargetsBuf` are gone. TargetedReflector selection and orientation are now pure functions
of the current frame's push constants (themselves pure functions of absolute sim time), so forward
and reverse playback of the same instant produce bit-identical results, and the hash is an
all-integer mix (`pairScore`, `sat_orbit.comp`) immune to magnitude collapse.

### Target generation (once at init) — S1, RELEASE_v1_1_PLAN.md
`SatelliteSim::loadReflectorTargets()` reads `reflector_targets.json` (next to the exe, moddable
exactly like `constellations.json`) — a hand-curated list of ~50 real, publicly-known solar
installations (`{name, lat, lon, capacity_mw}`), with the first entry flagged
`"observer_spawn": true` at the exact fixed spawn point (67°S, 67°W — see "Fixed Simulation
State"). Falls back to `generateReflectorTargetsRandomFallback()` (uniformly-random ECEF points,
still with a real fixed entry at index 0 for the observer-spawn pin) if the file is missing,
malformed, or empty. `kNumReflectorTargets = 201` is a **capacity** (buffer sizing), not the real
count — `reflectorTargetCount` (≤ capacity) holds how many actually loaded.
`reflectorObserverSpawnIdx` records which loaded index is the pin (informational/logging).

Per-target ground radius (`reflectorTargetsRadiusM[]`, real terrain elevation via a 3×3-max
`earthElevCpu` lookup) is computed by the shared `computeReflectorTargetElevationRadius(ti)`
helper — used by both the JSON path and the fallback. Both xyz (unit ECEF direction) and this
radius are uploaded ONCE, at generation time, into `reflectorTargetsECEFBuf` (host-visible,
`vec4` per target: xyz=ECEF dir, w=radius) — read by `sat_orbit.comp` for TargetedReflector target
search. There is no per-frame CPU rotation step any more; `sat_orbit.comp` rotates ECEF→ECI itself,
on demand, for whichever instant it needs (see below).

### Target kinds and solar PV parks (2026-10-02)

Every target carries a `"kind"`: `solar` (58 PV parks, Casa Grande included), `agriculture` (the two
agrivoltaics points), `daylight` (the seven "polar illumination" towns) or `none` (the spawn pin);
`reflectorKind[]`. A file without it is inferred from the name and `capacity_mw`, so old and modded
lists still load. Only `solar` changes anything so far: the park is drawn on the terrain.
- **Data:** area = `capacity_mw` x 2.2 ha/MW (or `"area_km2"`), mount = single-axis N-S trackers in the
  Americas, India and Australia, fixed tilt facing the equator elsewhere (or `"mount"`). Ivanpah, Noor
  Ouarzazate and Cerro Dominador carry `"tech": "csp"` and are still drawn as PV.
- **CPU (`fillSolarSites`, end of the beam readback):** the 8 nearest solar sites within 2500 km, in
  double, as `GpuSolarSite` (80 B) appended to `GpuGroundBeams` (`solarCount` = the old pad0) — no new
  binding (`sat_sky.frag` is at its sampled-image floor and has many SSBOs). The centre is relative to
  the observer's SEA-LEVEL point, the frame of `terrainQ`, so the shader's difference is float-exact and
  the rows are world-fixed. Trackers turn toward the Sun while it is up at the site, else toward the
  strongest beam landing on the park, else stow flat, eased over ~8 s, clamped +-55 deg.
- **Shader (`solarSiteAt`, before the farm block, which gives way inside a park's plots):** plots of
  4 x 4 blocks in or out of a wobbly outline, blocks of whole rows with 6-m roads, rows and segments as
  exactly box-filtered pulse trains (`solarPulseCover`: no aliasing at any footprint, a dark patch from
  orbit). Coverage is AS SEEN: a panel of width W tilted to n covers W |nz + nx dx/dz| of ground along the
  view (viewed across the rows the panels hide the ground), and casts the same projection along the Sun
  as shadow. Glass (`solarGlint`, Beckmann + Schlick F0 0.03 + Smith, the sea glint's form): sky
  reflection and Sun glint in the terrain lighting; in the ground-spot loop each beam's light
  (`GpuGroundBeam::dirOct`, the old pad0: ground -> satellite, octahedral snorm16) glints off the panels,
  and the spot's diffuse light now follows the surface's brightness (`solarDiffK`; it was the same on
  every surface). Seen from the mirror direction, each beam's glint lands on other rows, so the park
  shows the ring of beam satellites as a glitter band. Per-row tilt jitter +-1.5 deg; unresolved, it
  widens the lobe. Backs are a grey backsheet with no glass reflection. Debug view 27 (`debugview
  solar`): R coverage, G park ground, B normal east. Setting `clouds.solar_arrays` (bool, no UI yet).
  Cost within noise (harness `perf`, Topaz from 800 m). `scripts/solar_parks.satcmd`.
- **Rooftop PV where a park overlaps a city (2026-10-03):** the site evaluation runs BEFORE the city
  day pattern. With the city's presence `solarUrbanK` (cityLum, after the terrain limit), ground blocks
  drop out block by block (a hash per block below urbanK) and the park's smooth radial density
  (`radK`) x urbanK goes to the city pattern as `gCityPvK` (globals beside `gCityViewE`). `cityDayGrid`
  then puts panels on its own roofs, per lot by hash: commercial / perimeter / slab roofs rows of tilted
  panels at a 2.2-m pitch over most of the roof, houses one array on one slope, off-centre. It writes
  `gCityPvCov` (footprint-filtered; expectation `kCityPvMean` x K once unresolved, so it holds to orbit)
  plus a per-roof tilt jitter; after the farms the panels darken the albedo and join the park's glass
  (normal ~15 deg toward the equator, +-8 deg per roof; a broad lobe when unresolved). Reference: the
  user's Odessa snapshot (Roadrunner's coordinates sit on the city; the real plant is in Upton County).
  Cost within noise there.
- **Ground spots land on the target's ground (fixed the same day):** the CPU traced every beam to the
  sea-level sphere, so a beam arriving at 11-20 deg on a site 1.7 km up (Villanueva) lit the ground ~5 km
  past it. The ground spot and the cloud lights now intersect the sphere of the target's own radius
  (`reflectorTargetsRadiusM` minus its 75-m margin). The sky shafts still end at R_EARTH.

### Per-satellite selection (GPU, sat_orbit.comp) — deterministic lock windows
Target IDENTITY is chosen per fixed-width **sim-time window**
(`SatOrbitPC::reflectorLockWindowS`, default 90s, settings-window "Target lock window (s)"), not by
a persisted per-satellite lock.

**Per-satellite phase offset (2026-08-06 same-day follow-up).** `SatOrbitPC::windowFrac` is one
GLOBAL value, identical for every satellite dispatched this frame — without correction, every
TargetedReflector satellite's window boundary lands at the exact same sim-time instant, so all of
them ease toward a new target simultaneously: reported as one large synchronized wave of motion
regardless of how `reflectorLockWindowS`/`mirrorMaxRateDegPerSec` were tuned (a short window reads
as constant chaos, a long one as periodic mass movement). Fixed with a per-satellite hash offset —
`windowFracI = fract(pc.windowFrac + hashU(i × 0x2545F491u)/2³²)` — which is exactly the fractional
part of `(simTimeAbs + offsetSeconds_i) / W` (dropping the integer part doesn't care which multiple
of `W` `offsetSeconds_i` came from), i.e. this satellite's OWN window fraction, needing no extra
CPU-side work or push-constant fields. Every use of `windowFrac` in this section below is really
`windowFracI` in the shader; still a pure function of (sim time, satellite index), so still fully
reversible.

`sat_orbit.comp` extrapolates this frame's `deltaT`/`gmstNow` back to the CURRENT window's own
START instant — `toWinStart = -windowFracI × reflectorLockWindowS`, then
`evalDeltaT = deltaT + toWinStart` and `gmstEval = gmstNow + K_OMEGA_EARTH × toWinStart` (and one
more `reflectorLockWindowS` further back for the PREVIOUS window's start). **This extrapolation is
exact, not approximate**, for both quantities in this sim's model: orbital phase
(`u = u0 + meanMot×deltaT`) and GMST are both exactly linear in time, so evaluating at a shifted
`deltaT` is algebraically identical to the CPU having computed `deltaT` relative to a different
reference instant — no precision or physical approximation beyond what `deltaT`/`gmstNow` already
carry. The one real approximation is treating `sunDirECI` as constant across one window (true to
within the sun's ~1°/day drift against a ~90s window).

At each eval instant, `findWinner()` re-derives the satellite's own ECI position (`satEciAt`, same
closed-form math as the real position, evaluated at `evalDeltaT`) and scans **every** loaded target
(`pc.targetCount`, not a pre-filtered subset — the old per-frame CPU night-side compaction is gone),
rotating each target's static ECEF entry by `gmstEval`, rejecting day-side-at-that-instant and
anything below `pc.minBeamElevSin` (local elevation of the satellite *as seen from the target*), and
taking the `pairScore(satIdx, ORIGINAL target index)` argmax among survivors. Called once for the
CURRENT window's start (`bestIdx`) and once for the PREVIOUS window's (`bestIdxPrev`) — both
constant across their own whole window, since they only depend on that window's own start instant,
not on which frame within it asks. `pairScore` doesn't depend on the eval instant at all, only on
which targets are eligible — so a satellite's top-scoring target keeps winning unprompted across
consecutive windows as long as it stays eligible; a window boundary only actually changes the
winner when the incumbent drops out (day-side, or below `minBeamElevSin`).

`pairScore` is a pure function of the `(satellite, target)` pair (all-integer hash, see the block
comment above `hashU`/`pairScore` in `sat_orbit.comp`), independent of scan order and of how many
other candidates exist — the same load-spreading property the old `hash11`-based score was designed
to have, minus the magnitude-collapse bug.

**Beams need sunlight (2026-10-04):** a beam's intensity is x the satellite's Earth-shadow `litFactor` (the
soft umbra/penumbra, now computed before the ground-site block): until then a mirror in the Earth's shadow beamed
at full strength (user snap: Reflect satellites lighting the Antarctic winter night).

### Orientation — a rate-limited ease, not integrated slew
2026-08-06 same-day follow-up: the first cut of this rework smoothed only ONE transition case (both
the current and a look-AHEAD "next window" valid and different) via a fixed-fraction-of-window
crossfade, unrelated to how far the mirror actually had to swing. Every other transition (acquiring
a target from nothing, losing one, falling back to the nearest night-valid site) snapped instantly,
and even the smoothed case could look abrupt for a wide swing compressed into a fixed ~13.5s —
reported as satellites visibly snapping to target. Replaced with a genuine angular-rate cap
(`SatOrbitPC::mirrorMaxRateDegPerSec`, settings-window "Mirror max slew rate (deg/s)") applied via a
closed-form ease, covering every transition uniformly:
- `bestIdxPrev` (previous window's winner — a one-window lookback, not unbounded history, so still
  a pure function of current sim time) gives `startAim`: the ideal aim direction toward whatever the
  mirror was presumably doing right as the CURRENT window began, evaluated AT the current window's
  own start instant (`idealTowards(satEciCur, targetPosAt(bestIdxPrev, gmstEvalCur), ...)`).
  `nearFallbackIdeal()` (nearest night-valid target, no elevation gate, or `FlatMirror45` if truly
  nothing night-valid exists) substitutes whenever the relevant index is `< 0`, used identically for
  "previous window had nothing" and "current window has nothing."
- `destAtStart` is the same computation for `bestIdx` (current window's own winner) at that same
  start instant — directly comparable to `startAim` since both are evaluated at the identical
  moment, only the target differs. `angle0 = acos(dot(startAim, destAtStart))` is therefore a
  single stable angle for the whole window, not something that recomputes every frame.
- `slewDuration = clamp(angle0 / radians(mirrorMaxRateDegPerSec), ~0, reflectorLockWindowS)` — the
  real time a slew at the configured max rate would take, capped at one window so a huge swing
  can't bleed into the NEXT window's own (independently computed) ease.
- The mirror eases from `startAim` toward `liveIdeal` (the target's true LIVE, unquantized position
  right now — this is what makes it track exactly once caught up, not toward a stale window-start
  snapshot) via `smoothstep(0, slewDuration, windowFrac × reflectorLockWindowS)`. When
  `bestIdxPrev == bestIdx` (the common case — nothing actually changed), `angle0` is exactly 0, the
  ease completes within a clamped instant, and the mirror simply tracks `liveIdeal` for the whole
  window, matching the previous design's "once slew has caught up" steady state.
- `bestIdx` (for beam bookkeeping — footprint, `beamCloudBlock` lookup) is always the CURRENT
  window's winner, using its live position, regardless of ease progress — only ORIENTATION lags
  during a transition, never where the beam is drawn. `aimErrorRad` is the residual angle between
  the eased orientation and `liveIdeal` (0 once caught up) — same field, same downstream consumer
  (`cloud_march.comp`'s beam debug ray fade) as before, just driven by the rate-limited ease instead
  of window-crossfade progress.

Because every one of these quantities — `windowFrac`, `evalDeltaT`/`gmstEval` (current AND
previous), `bestIdx`/`bestIdxPrev`, `angle0`, the ease itself — is a pure function of the current
frame's push constants (themselves pure functions of absolute sim time computed in double precision
on the CPU), the whole pipeline remains reversible by construction: there is nothing to accumulate
differently depending on playback direction, even though the visual result now has a genuine
physically-motivated slew rate.

---

## Subsystem: Reflect-Orbital Beam Cloud Occlusion

See `.plans/BEAM_CLOUD_PLAN.md` for the full session-by-session history — this section is the
current-architecture summary only.

**The visible beam is not called "the debug ray" despite its name.** `showBeamDebugRays`
(`SatelliteSim.h`) started as a literal debug-only visualization (a green line, C12 follow-up #12)
back when a separate volumetric "tube" was the real beam visual. Once that tube was thrown out for
graphics/performance reasons, this ray was reworked in a later session (realistic color, altitude
attenuation) into the actual production beam visual — it defaults to `true` and is what a player
sees, not a diagnostic overlay. The shader-side header comment in `cloud_march.comp` calling it
"Opt-in and off by default" is itself the stale artifact of that history, not a bug — a same-session
pass mistakenly "fixed" the default to `false` reasoning from that comment before this was
clarified (2026-08-09); do not repeat that mistake. This ray was, until 2026-08-09, occluded by
terrain ONLY — genuinely no cloud awareness at all — which is almost certainly what a string of
"beams pass through clouds no matter what" reports were actually seeing, independent of whatever
was fixed in the occlusion math underneath it.

**`beam_self_march.comp`** (2026-08-09) computes real per-BEAM cloud occlusion, replacing
`beam_cloud_block.comp`'s per-TARGET vertical-column approximation (deleted). Dispatched over
`BEAM_MAX_ACTIVE` (2048) threads right after `sat_orbit.comp` (needs the `satENU`/`reflectDirENU`
that shader just wrote), fixed-size every frame (`beamCount` is a GPU atomic counter, not known at
command-buffer record time — inactive slots return immediately). For each active beam it
reconstructs the satellite's true ECEF position from `ReflectBeamsBuf`'s observer-relative ENU
offset (via `terrain.glsl`'s `observerPos()`/`enuBasis()`), then marches the SEGMENT from the
satellite to the mirror's **actual current ground intersection** — `reflectDirENU` traced to
`R_EARTH` via `raySphere`, NOT the chosen target's fixed `targetENU` position — through the cloud
shell, overwriting `blockAltM`/`blockOpacity` on that same `ReflectBeam` entry in place.

**Marching toward `targetENU` instead of the real ray was a second, subtler bug**, found in-app
the same day after the ground-intersection fix above already shipped: occlusion looked correct for
a beam locked onto its target (where `reflectDirENU` and the target direction coincide) but did
nothing for a beam still slewing between two targets (where they genuinely diverge — see
`aimErrorRad`/the TargetedReflector orientation section above). The march was checking cloud along
a path the beam wasn't physically following. Fixed by using `raySphere(satECEF, reflectDirECEF,
R_EARTH)`'s hit point as the near endpoint instead of `targetECEF` — same shell-crossing math
otherwise (the hit point is still guaranteed inside both cloud-base/cloud-top spheres, same as
before), just anchored to reality instead of intent.

**Why per-beam, when "Deliberately NOT per-satellite" was the standing design rule** (see
`beam_cloud_block.comp`'s own retired header, and `TERRAIN_PLAN.md` follow-ups #14-#16): that rule
was about a DIFFERENT failure shape. Follow-up #14's cost blowup was a real `cloudDensity()` march
evaluated per SCREEN PIXEL near each beam's line, once per satellite — cost multiplied by
(satellites × pixels). Follow-up #16's flicker came from a dedup fix that picked one "winning"
satellite's geometry per target per frame, and the winner's identity flipped frame-to-frame among
near-equal candidates. `beam_self_march.comp` is neither: a single bounded march per beam (same
shape `beam_cloud_block.comp` itself already proved cheap at 201 threads — beam_self_march is
~10x the thread count along a segment instead of straight up, still well under 1ms against
`cloud_march.comp`'s own ~16ms), with zero arbitration — every beam computes its own value from its
own stable geometry, so there is nothing to flicker between.

**`blockAltM`/`blockOpacity` keep their pre-existing meaning** (altitude where the path's
transmittance first drops below 50%, and overall 0-1 opacity — including the weighted-mean-
absorption-altitude fallback for a column that never crosses that threshold, so a thin/scattered
cloud doesn't produce a fake cutoff edge pinned to the shell's nominal ceiling). `sat_sky.frag`'s
ground-spot `shadowAtten = 1.0 - groundBeams[bi].blockOpacity` needed zero changes and automatically
reads more physically-accurate per-beam data.

**The visible pointing ray and the volumetric cloud glow were reworked together, same day, per
explicit user direction, once the march above was marching the real path.** Both previously read
NOTHING from `blockAltM`/`blockOpacity` (the ray) or read it through a per-TARGET CPU aggregation
with no directionality at all (the old `beamCloudLighting()`/`BeamCloudLightBuf` glow — a purely
horizontal Gaussian around the target's ground position, gated by a height cutoff averaged/argmaxed
across every satellite servicing that site). The visible ray and the cloud glow ended up on two
DIFFERENT architectures, after an intermediate design that didn't work out:
- **The ray** stayed in `cloud_march.comp`'s `main()`, per-pixel, folded into its existing
  closest-approach loop (`showBeamDebugRays`-gated). It applies a height-cutoff `smoothstep`/`mix`
  shape at its own closest-approach altitude (`qAltM`, already computed there for the vacuum/
  altitude fade), ADDITIONALLY multiplied with the pre-existing `cloudGate` term — the two answer
  different questions and neither substitutes for the other: `cloudGate` gates whether the
  CAMERA's own line of sight to this point is blocked by unrelated cloud; the height-cutoff term
  gates whether the BEAM's own sunlight survives to reach this point along its real path. Both
  must be open for the ray to render there.
- **The cloud glow went through a second design that shipped and was reverted the same day.**
  First attempt folded it into that same per-pixel ray loop, reusing `perpDist` (the CAMERA's
  view-ray-to-beam-line distance) for proximity — cheap, but geometrically wrong: that distance's
  iso-contours are not soft blobs, they're hyperbola-like curves, and it rendered as large white
  rings intersecting the beams. Explicit user correction: this needs to work exactly like the
  sun/moon terms already do, evaluated PER CLOUD SAMPLE inside the volumetric march, not as a
  screen-space effect. Reverted outright (not patched) back to the ray-only per-pixel loop.
- **The glow's current (third) design restores the per-sample shape** (`beamCloudLighting()`,
  called from `cloudMarchCS`'s own loop and added into `inScatter` right alongside
  `sunColorCloud`/`moonContrib` — the "feed forward" the user asked for), fed from a small
  CPU-built list (`SatelliteSim.cpp`, `kMaxCloudBeamLights`=**512**, `GpuBeamCloudLights`; this
  doc said 16 for a long time and was simply wrong — the cap was raised for coverage and the
  number here was never updated, which is exactly why that function's real cost went unnoticed) — the only
  shape proven affordable at per-march-sample call frequency (TERRAIN_PLAN.md follow-ups #14/#16).
  Each list entry is now ONE individual real beam (no per-target aggregation, so nothing to
  average/argmax and nothing to flicker between): `posENU` is that beam's REAL ground intersection
  (`satENU + reflectDirENU` traced to `R_EARTH`, via the same rotation-invariant local-frame
  `raySphere` trick the GPU shaders use — valid on the CPU too, no ECEF conversion needed), and
  `dirToSource` is its REAL direction (ground toward satellite), driving `phaseCloud()` per light
  instead of a shared `normalize(p)` local-zenith stand-in the old per-target version used.
  (**"ONE individual real beam per entry" is history, not current behaviour** — clustering was added
  2026-08-09 and reworked 2026-08-12. The per-beam GEOMETRY described here is still exactly what
  feeds the list; what an entry represents is not. See "Cloud-light identity and cross-frame easing"
  below.)
- `sat_sky.frag`'s ground spot separately anchors at the same real ray-ground intersection
  (`raySphere(satWorldPos, reflectDirENU, R_EARTH)`) instead of `targetENU` — this part of the fix
  was correct from the start (not implicated in the ring bug, a genuinely different computation)
  and was left alone through the glow's redesign. The range-cutoff fade (`beamMaxRangeM`)
  deliberately stays keyed to the fixed target site position, not the transient ray-ground point —
  that's "is the observer close enough to this SITE," which shouldn't flicker as a mid-slew ray
  briefly touches down elsewhere.
- That pre-existing gap — knockout bit 128 gates `sat_sky.frag`'s ground-spot loop and
  `beamCloudLighting()`'s per-sample glow, but NOT `cloud_march.comp`'s per-pixel ray loop, which
  had only ever been gated by `showBeamDebugRays` — **was closed 2026-08-10 by knockout bit 8192**,
  and measuring it is what found the cost below.

### Cloud-light identity and cross-frame easing (2026-08-12)

`GpuBeamCloudLights`, the GPU-side struct and its 512 cap, is **unchanged** — so is every consumer
(`beamCloudLighting()`, `cullCloudLightsForTile`). What changed is how the CPU builds it. Full
session history in `.plans/BEAM_CLOUD_PLAN.md`; this is the architecture summary.

The list used to be rebuilt from scratch every frame, and a cluster's identity was **emergent**: it
was seeded by whichever beam the scan reached first at a target (a 2 m `targetENU` epsilon match)
and admitted members by comparing them against a running partial average that changed as members
were added. That partition is discontinuous in its own inputs — one satellite dropping out
repartitions the survivors, changing cluster count, every direction and every summed intensity in a
single frame. Since `posENU`/`dirToSource`/`blockAltM`/`blockOpacity` are all intensity-weighted
means and `hgG≈0.99`, that reads as lights popping and re-aiming instantly. **A 2026-08-11 attempt
to fix this by adding a fade on top was reverted** (still flickery, 20 FPS): easing a slot whose
*meaning* changes underneath it doesn't help, and recovering identity by proximity-searching a pool
cost O(rawClusters × 256) per frame.

Identity is now **declared**, which is possible because `sat_orbit.comp` carries its `bestIdx`
through as `ReflectBeam::targetIdx` (**this is why the record grew 64 → 80 bytes** — the three pads
next to it are load-bearing, see below):

| | key | why it's stable |
|---|---|---|
| cluster | `(targetIdx, direction bucket)` | bucket is a FIXED quantization of the beam's direction in the **target site's own** local ENU frame (`reflectorSiteEnu*[]`, computed once in `computeReflectorTargetElevationRadius()`) — a pure function of that beam's geometry, independent of scan order, of the cluster's contents, and of the observer |
| individual (transiting) | originating satellite dispatch index (`debugPad`) | already guaranteed stable by `sat_orbit.comp`, unlike the atomic-append slot `s` |

`TrackedBeamLight` (`SatelliteSim.h`) holds the persistent state: two pools with the same reserved
budgets as before (256 + 256 = `kMaxCloudBeamLights`, so the emit can't truncate), each with a
power-of-two open-addressed key→slot index **rebuilt from live slots every frame** — O(live) ≤ 256,
and it avoids tombstones entirely. Values are eased with the `1 - exp(-dt/τ)` idiom
(`mwSuppressEased`'s): intensity asymmetric (`beamClusterFadeInS`/`OutS`, Beams tab), geometry on a
shorter fixed `kTrackedLightGeomEaseS`.

**Three invariants:**
1. **Geometry is stored in Earth-fixed ECEF, in `glm::dvec3` — never observer-relative ENU.** A
   ground site is stationary in ECEF, so an entry that goes unmatched for its whole fade-out cannot
   drift however far or fast the observer moves. `rebase()` still applies to the raw per-beam fields
   (genuinely one frame stale) and must NOT be applied to tracked state: it is rotation-only, built
   for exactly one frame of lag, and the 2026-08-11 revert burned three rounds relearning that.
2. **`ReflectBeam`'s three explicit `uint` pads must exist in BOTH `reflect_beam.glsl` and
   `GpuReflectBeam`.** std430 rounds the GLSL struct up to its 16-byte alignment; C++ does not,
   because `glm::vec3` is 4-aligned. The pre-`targetIdx` total agreed at 64 only by luck. Same
   silent-permutation hazard as `GpuCloudParams`, and the `static_assert` only catches a size change.
3. **Nothing in the readback loop may depend on scan order again.**
4. **The raw per-beam positions are rebased RIGIDLY (review 7):** `satENU`/`targetENU` are positions
   relative to last frame's EYE, so `rebasePos` adds the eye's own move (double, `lastBeamObsRadius`)
   to the basis turn; `rebase` (rotation only) is right for `reflectDirENU` alone. Rotation-only
   positions lagged the observer's per-frame displacement — ~2 km a frame flying at the height-scaled
   speed from 116 km, and the ground spots visibly swung while strafing.
5. **One eye for satellites and shaders:** `obsECI` (and every CPU evaluator: trace, selection
   readout, harness Sun search) uses `obsEyeRadiusM()` = R + max(ground, height offset) + 2 m, the
   shaders' `observerEffHeight`. The ground is the GPU's detailed ground (`obsGroundM()`: terrainFrameBuf .y, last frame's),
   the CPU DEM only before the first readback or while following (2026-10-22: the 18-km DEM read 450 m above the real
   ground in issue 20261022_174155, which pinned the eye there during a climb and ghosted the sky TAA). It was R + ground + offset: over the Sierra a ~3 km different eye, so
   beams landed km off the drawn scene and jumped as the CPU ground under the observer changed. The `std::sort` by `debugPad`
   that used to enforce determinism is deleted — the partition is order-independent now (sums and a
   max), `groundTopK` ranks on intensity, and `nearest`/the opacity diagnostics are commutative.

`beamClusterDirThresholdDeg` kept its slider, label, range and settings key, but its **meaning
changed**: it was the merge tolerance against a running-average direction, it is now the angular
size of a fixed bucket. Settings → Beams also shows live pool occupancy, which is the instrument for
the failure mode that killed the previous attempt — a count pinned at 256 means entries are
respawning instead of matching; a count near the real active-site count means keying is working.

Accepted: the eased list is history-dependent, so it is **not** bit-reversible under time reversal
the way the orbital pipeline is. Same class and precedent as `skyGlareEased`/`mwSuppressEased`.

### Beam pointing-ray tile culling (2026-08-10)

The Anchorage worst-case sweep measured that per-pixel ray loop at **7.54 ms at Medium — 28% of the
whole frame**, and 4.72 ms of Planetarium's ~10.9 ms (43%). It ran `min(beamCount, 2048)` iterations
on every half-res texel (571 beams × 484,800 texels = **277M iterations/frame**) and rejected on
distance only at the END, after ~7 SSBO loads, a `raySphere` with a sqrt and the full
closest-approach solve. Beam rendering in total (this loop + bit 128's two consumers + the
`beam_self_march.comp` dispatch) was **14.0 ms, 52% of the Medium frame** — more than the volumetric
cloud march.

`cullBeamsForTile()` is the standard Forward+ light-cull answer, and it fits for free because this
shader is already dispatched at `local_size 16x16`: one workgroup owns a 16×16-texel screen tile, so
its 256 threads cooperatively test every beam ONCE against the tile's bounding view cone and leave a
short shared-memory list (`sTileBeamIdx`/`RayLen`/`Fade`, cap `kTileBeamMax`=384, ~4.6 KB shared)
that each thread walks. Per-thread iteration count becomes "beams crossing this tile" rather than
"beams that exist", so **cost stops scaling with the observer's location** — Anchorage concentrating
beams was the entire problem.

It also does the **Tier-1 hoist**: `rayLen` (a `raySphere` against `R_EARTH`), `targetDistM`/
`rangeFade` and `aimFade` are per-beam constants the old loop recomputed once per texel — 484,800
times each. They are now computed once per workgroup and passed through shared memory. Doing the
hoist here rather than in `beam_self_march.comp` needs no new `ReflectBeam` fields and no
producer-side plumbing, and — the deciding reason — does **not** make bit 512 (that dispatch's
knockout) an unsafe fallback.

**Three invariants, all easy to break:**
1. **`kDebugRayRadiusM`/`kDebugRayMinAngRad`/`kDebugRayMaxLenM`/`kDebugRayAimMaxRad`/`kSkyBeamFadeM`
   moved to file scope** precisely so the cull and the per-texel loop cannot disagree. If they
   diverge the cull stops being conservative and beams pop at tile boundaries.
2. **The conservatism bound.** Two rays sharing an origin and diverging by at most the tile
   half-angle are separated by at most `tFar * tileHalfAngle` anywhere within `tFar`; the closest
   point on the view ray to any point of the beam segment lies within `tFar` of the origin. So the
   accept test uses `radiusMax*4 + tFar*tileHalfAngle` with `radiusMax` derived from `tFar` (not the
   centre ray's own `t`). The failure mode is unmistakable: beams appearing/disappearing on a
   16×16-texel grid.
3. **The barriers must stay in uniform control flow.** `cullBeamsForTile` is called BEFORE `main()`'s
   out-of-bounds early return, so every thread of an edge workgroup reaches both `barrier()` calls;
   the scan itself sits in a push-constant-only (workgroup-uniform) branch, and the barriers are at
   the function's top level, outside it. `obsEffH`/`obsPos` are therefore also resolved above that
   early return — safe, since `observerEffHeight` depends only on `pc.obsECEFDir`, not on `coord`.

Overflow past `kTileBeamMax` sets `sTileOverflow` and falls back to scanning the whole buffer,
recomputing exactly what the cull would have supplied — a pathological tile gets slow, never wrong.
**Knockout bit 131072 forces that same fallback**, which makes it the cull's correctness A/B (image
must be pixel-identical with it on) and the way to measure what the cull bought — its sweep
`cost_ms` is the SAVING, reported with the opposite sign to every other row.

**Measured:** beam pointing rays 7.54 ms → **1.08 ms** at Medium; the in-sweep A/B says the cull
saves **6.78 ms** there and **3.29 ms** at Planetarium (which dropped 10.9 → 6.85 ms total).

### Cloud-light tile culling (`cullCloudLightsForTile`, 2026-08-10)

Same workgroup, same pattern, different list. `beamCloudLighting()` is called from `cloudMarchCS`'s
innermost loop — once per in-cloud SAMPLE — and walked all `beamLightCount` entries every time,
against a cap of **512**. That was 3.48 ms of the `cloud_march` bucket at Medium. The cull reduces
512 lights to the handful whose influence cylinder can reach any ray in the tile; the per-sample
loop walks that shared list instead.

Its conservatism argument differs from the ray cull's in one place worth understanding:
`tRangeMax` is the tile-centre ray's own far crossing of the **cloud-top sphere**, which
upper-bounds where `cloudMarchCS` can march (`tExit` is min'd against exactly that shell exit, and
both `tScene` and `maxRenderDistM` only shorten it), scaled by `kTileRangeMargin` = 1.25 so a
tile-edge ray whose own shell crossing runs slightly longer is still covered. `kBeamCutoffSigma`
moved to file scope for the same reason the ray constants did — the cull and the per-sample test
must bound against the identical radius. Bit 131072 disables this cull too.

### Beam ground-spot CPU hoist (2026-08-10)

`sat_sky.frag`'s ground-spot loop measured 1.59 ms at Medium, on a `GroundBeamsBuf` sitting at its
full `kMaxGroundBeams` = 256 cap, so every ground-hit pixel paid all 256 iterations at full
resolution. Almost the entire body was view-INDEPENDENT: a `length(targetENU)` + smoothstep range
fade, an `obsPos + satENU` and a `raySphere` (two sqrts) for the real ray/ground intersection, the
elevation fade, and the shadow attenuation. Only the horizontal distance to the landing spot and
the two Gaussians built from it genuinely vary per pixel.

All of it moved to the CPU loop that already builds this buffer each frame, into a new packed
`GpuGroundBeam` (32 bytes, mirrored as `GroundBeam` in `sat_sky.frag` — hand-mirrored, same
convention and same hazard as `GpuCloudParams`): `weight` (= intensity × rangeFade × elevFade ×
shadowAtten) plus `invFootprintSq`/`invCoreSq`/`cutoffSq`. The shader's per-beam work is now a 2D
subtract, a dot, a squared-distance reject and two exps — **with the reject first rather than
last**, and no sqrt anywhere. `intensity` is still carried, unread by the shader, purely so the
CPU top-K eviction ranks on exactly the quantity it did before (ranking stability is load-bearing —
see the flicker history at the insertion site). Entries that fail the elevation/range/ground-hit
tests keep their slot with `weight = 0` rather than being skipped, so top-K membership stays a
function of intensity alone and doesn't churn frame to frame.

---
