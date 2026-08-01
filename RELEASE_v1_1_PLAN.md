# SAT LIGHT SIM v1.1 — Release Plan

Scope: turn a well-tuned personal simulator into something a stranger can download, launch on
unknown hardware, and enjoy without reading a manual. Read alongside `TERRAIN_PLAN.md` (rendering
work), `PLANETS_PLAN.md` (planets feature, outside this plan's scope but touches UC2/NEW-6), and
`CLAUDE.md` (architecture).

Status legend: `[ ]` not started · `[~]` partially exists · `[x]` done

---

## Session log

**Session 30 (2026-07-30):** Phases 1 and 2 confirmed already done (code was ahead of this doc's
checkboxes — they've been backfilled below). Phase 3 (S2 stars/pollution, S3 flare ceiling)
completed and documented inline below; `lightPollutionGain` re-tuned in-app by you afterward and
confirmed good. Also shipped, **outside this plan's original scope**: real Keplerian-ephemeris
planets (Mercury–Uranus), clickable/selectable, plus a Moon direction-calc fix found along the way
— full writeup in the new `PLANETS_PLAN.md`, architecture in `CLAUDE.md`'s "Subsystem: Planets".
Not itself a release gate, but worth remembering it's there when scoping QA (NEW-6) and
attributions (UC2 — the JPL/Standish elements and Schlyter magnitude formulas should get a citation
alongside the Yale Bright Star Catalogue).

**Next session: Phase 4** (UC3 intro cinematic + UC1 benchmark hook, UC4 dual-input docs, UC6
screenshots, S1 reflector targets) — see Sequencing below. Phase 4 is the cut line if time runs
short, so it's fine to pick off items individually rather than all-or-nothing.

**Session 31: Phase 4 complete, all four items.**
- **UC3 + UC1 mechanism 2:** the old static-text intro overlay is gone, replaced by a fixed
  camera-path cinematic (`kIntroKeyframes`, `SatelliteSim.h`) — ground/zenith → horizon pan → rise
  through the cloud deck (teaches Q/E, generated live from `keybindings` so a rebind is reflected)
  → LEO → settle back on the ground, with fading captions and a "press any key to skip" hint from
  1.5s in. Deliberately never moves `obsDir`/lat-lon — only altitude/look/FOV/facing-azimuth — so
  no great-circle interpolation was needed. `gpuMsTotalSmoothed` is accumulated across beats 1-4
  and used for a one-shot promote/demote against a 12ms target at natural completion only (not on
  skip, not on replay, not during crash recovery) — `finishIntro()`. `showIntro` is now persisted
  (first-run-only); a "Replay Intro" button was added to Settings > Display.
- **UC4:** the intro's old hardcoded keyboard-only control list is gone (see UC3 above). The
  Controls window now splits into rebindable "digital" rows (including Q/E, which used to be a
  hardcoded string) and a labeled read-only "ANALOG (not rebindable)" section for the true axes
  (WASD/stick move, mouse-drag/stick look, scroll zoom, trigger elevation). `lastInputWasGamepad`
  tracks which device was last active and reorders each row's "Key / Pad" text accordingly. A
  gamepad virtual cursor (right stick moves it, A clicks, active only while a Settings/Controls
  window is open) was added via a new `Simulation::virtualCursor()` hook — no changes needed to any
  existing Clay hover/click code, since App just overrides that frame's mouse position/click state.
- **UC6:** `VulkanContext::createSwapchain` now requests `TRANSFER_SRC` when the surface supports
  it (`screenshotSupported`, near-universal but gated, not assumed). F12 (`KB_SCREENSHOT`) copies
  the swapchain image to a host-visible staging buffer (`vkCmdCopyImageToBuffer`, avoiding the
  linear-tiling row-pitch problem entirely) right after the render pass ends; the copy is read back
  and PNG-encoded (`stb_image_write`, BGRA→RGBA swizzle) at the top of the NEXT frame, once the
  frame fence proves the GPU finished writing it — same "read back what last frame's GPU work
  produced" point `ctx.resolveTimestamps()` already uses. Clean-shot mode (`wantsCleanScreenshot()`)
  skips `ui.record()` for the captured frame. Saved to `<userDataDir>/screenshots/`, confirmation
  toast on completion. PNG `tEXt` metadata (version/observer/sim-time) was scoped as optional and
  **not implemented** — `stb_image_write`'s monolithic writer has no chunk-injection API, and
  hand-rolling PNG chunk manipulation wasn't judged worth it for an optional item.
- **S1:** `data/reflector_targets.json` — ~50 hand-curated real solar installations (no license
  obligations; hand-typed from public knowledge, not a redistributed dataset) plus an
  `"observer_spawn": true` pin at the exact 67°S/67°W spawn point, loaded by
  `SatelliteSim::loadReflectorTargets()` (falls back to the old procedural-random generator on any
  failure). Fixed a real, previously-undiscovered bug along the way: the documented "index 200 is a
  fixed observer-spawn pin" was already broken — the code that wrote it had been removed in an
  earlier session, leaving that slot a degenerate zero-vector (`normalize` → NaN → silently
  invalid) — so there was no working guaranteed-nearby target at all before this session. Also
  implemented the CPU-side compaction the plan called for, in a lower-risk form than originally
  scoped: `updatePositions()` packs only night-side-valid targets to the front of
  `reflectorTargetsMapped` each frame (`GpuReflectorTarget.origIdx` replaces the old 0/1 `.valid`
  flag with the target's original index), and `sat_orbit.comp`'s per-satellite scan is bounded by
  the new `SatOrbitPC::activeTargetCount` instead of a flat 201 — this is the dominant cost the
  plan identified and gets the full win. `beam_cloud_block.comp` was deliberately left
  un-reordered (its dispatch count now scales with `reflectorTargetCount` from the real-coordinates
  change alone, a ~4x win already) rather than also compacting it to the night-side subset — doing
  so safely would need a second per-frame-rewritten buffer and was judged not worth the added risk
  for a pass with no way to runtime-test the result; revisit if `beam_cloud_block.comp` shows up as
  a real cost in a future profiling pass.

**Same-day follow-up bug fix:** stars had no cloud occlusion at any preset (an architectural gap,
not a preset-tuning issue) — most visible at Medium-and-below because those presets' shorter cloud
distance-fade band leaves more of the visible sky in a "looks like cloud, occludes nothing" state.
Fixed by mirroring satellites' existing C12-follow-up-#33 cloud-occlusion sampling onto
`star_point.frag`. Relevant to NEW-6's QA matrix — re-check star/cloud interaction across presets.
Full writeup in `PLANETS_PLAN.md` (shared pipeline with planets).

---

## 0. Headline framing

v1.0 was a planetarium with satellites. v1.1 is a full atmospheric/terrain renderer that happens to
contain a planetarium. That change is the release risk: **the thing that makes v1.1 impressive is
also the thing that will make it unplayable for a chunk of the audience**, and the current default
settings are the author's own tuned values on the author's own GPU.

Three items below carry most of the release risk. Everything else is polish:

1. **P0 — First-run graphics selection** (UC1). A new user on integrated graphics currently gets
   the full terrain march + cloud march + aurora at native resolution.
2. **P0 — It must not hard-crash on unsupported hardware, and must not write to a read-only
   directory** (NEW-2, NEW-4). Both are currently real.
3. **P0 — Licensing/attribution completeness** (UC2). Some of this is a legal obligation, not
   politeness, and it is currently incomplete.

---

## User Comfort

### UC1 — Graphics presets + first-run auto-detection `[x]` **P0** (done, predates session 30)

**The good news: most of the machinery already exists.** `debugDisableMask` (session 29 profiling
knockouts) already disables terrain march, atmosphere loop, `optDepth`, ocean reflection, airglow,
aurora, cloud shadows, and the scene-depth pass — each with "a mathematically-safe zero/no-op
fallback." That is 90% of the feature-disable layer for a preset system, built for a different
reason. `renderScale` and ~46 quality sliders cover the rest.

**Caveat that must not be skipped:** those fallbacks were validated for *A/B profiling*, not for
shipping as a play mode. Terrain-off leaves `tHit=-1` → the sea-level sphere fallback, which is
exactly what a planetarium mode wants; but aurora-off, airglow-off etc. need a visual pass to
confirm they degrade *gracefully* rather than *conspicuously*. `debugDisableMask` is also currently
not persisted. Promoting it to a user-facing feature means persisting it and re-validating each bit.

#### Preset tiers

| Preset | Intent | Key settings |
|---|---|---|
| **Planetarium** | v1.0 experience. Flat textured Earth, stars, satellites, atmosphere. Fast anywhere. | terrain march OFF (sea-level sphere), cloud march OFF, aurora OFF, ocean waves OFF, airglow OFF, renderScale 1.0 |
| **Low** | Integrated graphics / old laptops | + 2D cloud paste only, minimal atmosphere samples, renderScale 0.7 |
| **Medium** | Mainstream discrete GPU | volumetric clouds at reduced steps, terrain march with tight step budget, aurora ON at low steps |
| **High** | Current defaults | today's tuned values |
| **Ultra** | Uncapped for showcase/screenshots | max march steps, renderScale 1.0, all effects |
| **Custom** | Auto-selected the moment any advanced slider moves | — |

**Planetarium is not "Low."** It is a deliberately different product for the audience that came for
satellites, not for weather. Sell it that way in the UI, not as a downgrade.

#### First-run selection — recommended approach

Three mechanisms, layered. Do all three; each covers the others' failure mode.

1. **Seed from device class.** `VkPhysicalDeviceProperties::deviceType` —
   `INTEGRATED_GPU`/`CPU`/`VIRTUAL_GPU` → seed **Low**; `DISCRETE_GPU` → seed **Medium**. Coarse,
   but never catastrophically wrong, and it costs ~10 lines. Do *not* build a GPU-name lookup table;
   it is unmaintainable and is always wrong for someone.

2. **Measure during the intro cinematic (UC3).** This project already has in-app GPU timestamp
   queries and `gpuMsSmoothed[6]`. The intro flies a fixed camera path through ground → cloud deck →
   orbit — which is, coincidentally, a near-perfect benchmark: it samples the ground-level terrain
   march, the cloud deck, and the orbital view in one pass. Run the intro at the seeded preset,
   accumulate GPU frame time, and promote/demote once at the end against a target budget
   (~12 ms for 3 tiers of headroom, tunable). **Fold UC1 and UC3 into one feature — this is the
   single best structural idea in this plan.**

3. **Always tell the user, never silently re-decide.** On completion show a dismissible line:
   `Graphics set to Medium based on your hardware — change in Settings > Display`. Persist the
   result and never auto-adjust again. Silent dynamic quality scaling is worse than a bad fixed
   choice, because the user cannot tell whether they are looking at a bug or a downgrade.

#### Settings-window restructure (required, not optional)

The Clouds / Ocean / Terrain / Aurora tabs are ~46 developer sliders. A new user opening Settings
should see: **Preset selector, Render scale, Window mode, Text scale, Units, FPS cap** — and one
`Show advanced settings` toggle that reveals the existing tabs unchanged. Keep every slider; just
stop making them the front door. Moving any advanced slider sets preset → Custom.

**Effort:** L (preset table + apply/detect + settings restructure + knockout-bit validation pass).

---

### UC2 — Attributions, licenses, AI disclaimer `[~]` **P0**

An Attributions tab exists with three entries: constellation data (planet4589.org), the
peterekepeter lens-flare Shadertoy, and HackerNoon Pixel Icon Library. Everything below is missing.

**Blocking on you:** I cannot determine provenance or license terms for the asset files from the
repo alone. Before this can be finished I need the source and license of each:

| Asset | Likely source | Need |
|---|---|---|
| `8k_earth_daymap/nightmap/clouds/specular/normal.jpg` | Solar System Scope (CC BY 4.0) — attribution is **mandatory** if so | confirm |
| `earth_elevation.png` (21600×10800, land-only, no bathymetry) | unknown | confirm |
| `8k_stars_milky_way.jpg` | Solar System Scope / ESO Gaia sky? | confirm |
| `full_moon.png` | NASA/LRO? (public domain if so) | confirm |
| `city_day_detail.png` / `city_night_detail.png` | unknown | confirm |
| `assets/sound/music/*` | — | confirm license + whether commercial/monetized use is permitted |
| `assets/noise/rgba_noise.png` | Shadertoy default noise? | confirm |
| Any other Shadertoy references beyond peterekepeter | you mentioned "some" | list them |

**Also required:**
- **Yale Bright Star Catalogue** (Hoffleit & Warren 1991, CDS V/50) — cited in `star_catalog.h`
  source comments only, not in the app.
- **Third-party library licenses** — GLFW (zlib), glm (MIT), Clay (zlib), stb (MIT/public domain),
  miniaudio (MIT/public domain), nlohmann/json (MIT). MIT and zlib both require the license text to
  travel with binary distributions. **An in-app tab is not sufficient** — ship
  `THIRD_PARTY_NOTICES.txt` in the zip and reference it from the tab.
- **AI code disclaimer.** Suggested wording, adjust to taste:
  *"Portions of this software were written with AI assistance (Claude). All code was reviewed,
  tested, and integrated by the author."*
- **Non-affiliation disclaimer.** The sim names SpaceX, Amazon, OneWeb, Guowang, and Reflect
  Orbital, and its thesis is unflattering to them. A one-line *"Not affiliated with or endorsed by
  any company named in this simulation. Constellation parameters are drawn from public filings and
  are approximations for illustrative purposes."* is cheap insurance and also improves credibility.
- **Photosensitivity notice** — see NEW-8.

**Effort:** S once the provenance list comes back. Blocked until then.

---

### UC3 — Cinematic intro `[x]` **P1** (fold with UC1) — done session 31

Replaces the current wall-of-text overlay (`buildIntroOverlay`, ~90 lines of hardcoded Clay text).

**The real design problem you identified is correct and is the whole point:** a player who never
finds Q/E never sees the product. Do not solve that with a text line. Solve it by *flying them
there* before handing over control, so the altitude axis is already in their mental model.

#### Proposed beat sheet

| # | Camera | Text (fade in/out) | Teaches |
|---|---|---|---|
| 1 | Ground, night, looking up at zenith. Slow drift. | `SAT LIGHT SIM` / `by papereater` | Milky Way + stars exist |
| 2 | Slow pan to horizon; satellite streaks cross frame | "Every planned major constellation has been built." | the subject |
| 3 | **Rise** through the cloud deck to ~80 km, camera holding on the ground below | `Q / E — or the triggers — change your altitude` | **Q/E, the critical one** |
| 4 | Continue to LEO over the terminator; reflect beams visible below | "Perpetual sunshine lies in sun-synchronous orbit." | the orbital view exists |
| 5 | Settle at a good default ground vantage; controls hint fades in | "We will come to miss the quiet sky." | handoff |

#### Implementation notes

- A `struct IntroKeyframe { float t; float lat, lon, altM, azDeg, elDeg, fovDeg; const char* text; }`
  array with eased (smoothstep) interpolation, driven from `recordCompute` before the normal
  WASD/gamepad handling; the existing camera members are the only state touched. Lat changes must go
  through `setLat()`, not direct assignment.
- Skippable on any input — reuse the existing `showIntro` dismiss path. **Must be skippable from
  frame 1**, and must show `Press any key to skip` from ~1.5 s in.
- Replayable from Settings > Display (`Replay intro`).
- `showIntro` becomes first-run-only (persisted), not every-launch.
- The GPU-timing accumulation for UC1 runs across beats 1–4 and applies at beat 5.
- **Do not** run the benchmark during the skip path; if skipped, keep the device-class seed.

**Effort:** M.

---

### UC4 — Dual-input controls documentation `[x]` **P1** — done session 31

Already good: every `KeyBinding` carries a `gpButton`, `gamepadButtonDisplayName()` exists, and the
Controls window renders `"Key / Pad"` pairs. Gaps:

- `[ ]` **The intro overlay's control list is hardcoded keyboard-only** (`"WASD  =  Move"` etc.).
  Generate it from `keybindings` the way `buildViewControlsBody` does, so a rebind or a gamepad is
  reflected. This is the highest-value fix here — it is the first thing every user reads.
- `[ ]` **Analog axes are documented as hardcoded strings, not bindings.** `"WASD / L stick"`,
  `"Right-click drag / R stick"`, `"Q/E / RT/LT trigger"` — these can't be rebound and can drift out
  of sync with reality. Either bring the axes into the binding system or add a clearly-marked
  read-only "Analog controls" section so it's honest.
- `[ ]` **Show the active input device's column first.** Track last-input-source (key/mouse vs.
  gamepad) and reorder/emphasize accordingly. Showing both always is acceptable; showing keyboard
  only to a controller user is not.
- `[ ]` **Gamepad UI navigation.** Today a controller user still needs a mouse to change any
  setting. Full focus-based UI navigation in Clay is a large job. **Recommend the cheap 90%
  solution:** right-stick drives a virtual cursor, A = left click, B = close/back. ~1 day, versus a
  week for real focus traversal, and it makes the whole existing UI reachable.
- `[ ]` Controller glyphs (Xbox face buttons) as icons rather than text — nice-to-have, skip if tight.

**Effort:** M.

---

### UC5 — Push constants: are they a performance problem? `[x]` (answered)

**No. Do not cull any setting for push-constant reasons.**

The numbers: 14 `vkCmdPushConstants` call sites, largest struct 144 bytes (`SatDrawPC`), plus a
384-byte `CloudParams` UBO written once per frame. Total ≈ 2 KB of constant traffic per frame. Push
constants are delivered through a fast driver path (registers or a small preamble buffer) and are
specifically the cheapest way to get data to a shader. This is not a measurable cost and never will
be at this scale. The Vulkan spec's 128-byte guaranteed minimum has already been exceeded
(`SatDrawPC` is 144, `SatFlarePC` 128) — **that** is worth a compatibility check on low-end drivers
(`maxPushConstantsSize`), but it is a correctness question, not a performance one.

**There is a real adjacent cost, and it is the opposite of what the question assumes.** `CLAUDE.md`
already documents the mechanism in the `optDepth` cautionary tale: a **runtime-variable loop bound
prevents unrolling**. `cloud.marchSteps`, `lightSamples`, `viewSamplesMin/Max`, `oceanSeaOctaves`,
`oceanReflSamples` and friends cost something not because they are *pushed* but because they make
trip counts *dynamic*, which defeats unrolling and forces conservative register allocation.

So the payoff from "baking known values" is real, but it is a *shader-side* change, not a
push-constant-side one: replace the UBO read with a `const int` in GLSL. And it interacts neatly
with UC1 — presets are fixed values, so only the Custom path needs the dynamic version.

**Recommendation:** do not do this for v1.1 as a broad effort. It means shader permutations (two
variants, or specialization constants), which is real complexity for an unmeasured win. Instead:
**run one experiment** — hardcode `cloud.marchSteps` for the Medium preset's value, measure
`gpuMsSmoothed` with the existing profiling tools, and only pursue it if the delta justifies the
machinery. `VkSpecializationInfo` is the clean mechanism if it does (one pipeline per preset,
constants baked at pipeline creation, no source duplication).

**Effort:** S (the experiment). Defer the general case.

---

### UC6 — Screenshots `[x]` **P2** — done session 31 (PNG tEXt metadata scoped out, see session log)

**Real blocker found:** `VulkanContext.cpp:399` sets
`ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` only. Copying from a swapchain image without
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT` is invalid. Fix first, and gate it on
`surfaceCaps.supportedUsageFlags & TRANSFER_SRC` — it is near-universally supported but not
guaranteed, so fall back to disabling the feature rather than failing to create a swapchain.

Then:
- `vkCmdCopyImage` swapchain → host-visible `VK_IMAGE_TILING_LINEAR` image after the frame's submit;
  swizzle BGRA→RGBA on the CPU (swapchain format is almost certainly `B8G8R8A8`).
- `stb_image_write` — `stb` is already vendored via FetchContent and only `stb_truetype` is in use,
  so this is a one-line `#define STB_IMAGE_WRITE_IMPLEMENTATION`.
- Output `screenshots/satlight_YYYY-MM-DD_HH-MM-SS.png`, path per NEW-4.
- F12 hotkey **through the `keybindings` vector** (`KB_SCREENSHOT`, event) per the mandatory
  pipeline in `CLAUDE.md`, plus a toolbar button.
- **Clean-shot mode:** suppress UI for the captured frame. This is what makes the feature actually
  used — nobody shares a screenshot with a settings panel in it. Simplest version: a
  `pendingScreenshot` flag that skips `ui.record()` for one frame.
- On-screen confirmation toast with the filename.
- Optional: embed version + observer lat/lon/alt + sim time in a PNG `tEXt` chunk.

**Effort:** M (S once the swapchain flag is fixed).

---

## Simulation

### S1 — Real reflector target coordinates `[x]` **P1** — done session 31 (see session log for the
compaction scope decision)

Current: `kNumReflectorTargets = 201` uniformly-random ECEF points, index 200 pinned to the observer
spawn (67°S 67°W).

**Realism.** Reflect Orbital's actual proposition is illuminating solar farms after dark, so real
sites are the right data. Sources: Global Energy Monitor's *Global Solar Power Tracker* (free,
CC BY 4.0, includes lat/lon + capacity + status), or OpenStreetMap `plant:source=solar` (ODbL —
attribution + share-alike, check before use). Ship as `data/reflector_targets.json` with
`{lat, lon, name, capacity_mw}`, moddable exactly like `constellations.json`, and keep the observer
pin as an explicit `"observer_spawn": true` flag rather than a hardcoded last index — that pin is
what makes the feature visible on first launch and should not be lost.

**Performance — the important nuance.** Swapping random points for real ones does *not* by itself
save anything: a real list is ~100–300 sites, the same order as today, and `sat_orbit.comp` still
scans every target per satellite while `beam_cloud_block.comp` still dispatches one thread each.

**But real sites cluster** — US Southwest, Rajasthan, Xinjiang/Qinghai, Spain, Atacama — and the
night-side validity test already runs on the CPU every frame in `updatePositions()`. So the actual
win is compaction: **build a compacted night-side-only active list on the CPU and upload just that**,
typically 20–40 entries instead of 201. That shrinks the per-satellite scan by ~5–10× and the
`beam_cloud_block` dispatch proportionally. It is a bigger win with real (clustered) data than with
random (uniform) data, because uniform points are ~50% night-side by construction while clustered
ones swing much further.

Do both, in that order: compaction is the perf item, real coordinates are the realism item, and they
compose.

**Effort:** M.

---

### S2 — Light pollution, dimmer stars, Milky Way `[x]` **P1** — *highest visual value in the plan*

Three findings, one of which is a concrete bug.

#### (a) The star catalogue is the easy, huge win

`star_catalog.h` holds **287 stars at Vmag ≤ 3.5**. Naked-eye limit in a genuinely dark sky is
~6.5, which is ~9,100 stars — a **32× increase**. BSC5 is complete to ~6.5, `parse_bsc.py` already
downloads and parses it, and the threshold is a literal in that script. Cost: 9,100 × 32 B = 291 KB
of host-visible buffer and 9,100 point sprites — negligible against 78,000 satellites.

**One thing must change with it:** `initStars` sizes sprites as
`angSize = (1.5 + min(rawInt, 4.0)) * 4.0` — a mag 6 star and a mag 3 star both get ~6 px, because
`rawInt` is tiny for both and the `1.5` floor dominates. Push 9,100 stars through that curve and the
sky turns to porridge. Needs a magnitude-driven size curve that floors near 1 px for the faintest
stars, so the added ones read as a dense fine dust rather than a second layer of blobs.

#### (b) Star brightness *does* feed the pollution model correctly — the shape is right

`rawInt = 10^(-vmag/2.5)`, then multiplied by `(1 - domeVal × 0.99)`. A multiplicative intensity
scale *is* a uniform magnitude shift, which is exactly how limiting magnitude works physically. So
the model naturally produces "only the brightest survive downtown." The shape is not the problem.

#### (c) The actual Milky Way bug — this is why it's still visible near cities

In both `updateStars()` and `sat_sky.frag`:

```
elevFalloff = 0.35 / (max(skyDir.z, 0) + 0.35)   // 1.0 at horizon, ~0.26 at zenith
domeVal     = clamp(domeAz * elevFalloff, 0, 1)
```

`kMWPollutionMaxDim = 0.99` therefore cannot dim the *zenith* Milky Way by more than
**~26%**, no matter how bright the city or how high `lightPollutionGain` goes. The Milky Way is a
large, mostly-high-in-the-sky feature, so it lives almost entirely in the region where the dome is
structurally incapable of suppressing it.

That falloff curve is right for its original purpose — city glow does hang low. It is wrong as the
*only* term, because real urban skyglow raises zenith brightness enormously (Bortle 8 zenith is
roughly 50× brighter than Bortle 1). Fix by splitting the dome into two components:

```
domeVal = clamp(domeAz * (kIsotropicFrac + (1 - kIsotropicFrac) * elevFalloff), 0, 1)
```

with `kIsotropicFrac` ≈ 0.35–0.5. Horizon behaviour is unchanged; the zenith now gets a real floor.
Apply identically to stars, satellites, and the Milky Way so they stay coherent (the "same array,
coherent by construction" property `CLAUDE.md` calls out is worth preserving). Expect to re-tune
`lightPollutionGain` afterwards.

The Milky Way texture being dust/gas with no stars is a genuine asset here, not a limitation — once
(a) lands, the composite gets substantially more real for free.

**Target outcome, stated as an acceptance test:** at 67°S 67°W (spawn, effectively Bortle 1) the sky
should be overwhelming — faint Milky Way structure plus thousands of stars. Over a major city the
Milky Way should be *gone* and only ~10–30 stars visible. Over a small town, constellations only.

**Effort:** M. Do (a) and (c) together — (c) is what makes (a) look right.

**Done (session 30):**
- (a) `parse_bsc.py`'s `VMAG_LIMIT` raised 3.5 → 6.5; regenerated `star_catalog.h` from BSC5 —
  8404 entries (up from 287). `initStars()`'s point-sprite size curve replaced: the old
  `1.5 + min(rawInt, 4.0)` let its additive floor dominate every faint star, so mag 6 and mag 3
  both landed at ~6 px; now `0.25 + 2.5 × sqrt(rawInt)` (pre-`starScale`), which floors near 1 px
  for the faintest new stars while keeping Sirius at roughly its old ~21 px.
- (b) No code change — verified the multiplicative model already produces the right shape.
- (c) Isotropic-floor fix applied identically at all four `elevFalloff` consumers: `sat_flare.comp`
  (satellites), `sat_sky.frag` (Milky Way), `cloud_march.comp` (aurora), `updateStars()` (CPU).
  `domeVal = clamp(domeAz * (kIsotropicFrac + (1-kIsotropicFrac) * elevFalloff), 0, 1)`,
  `kIsotropicFrac = 0.4`. Beam-glow dome copies (`beamDomeVal`) deliberately left untouched — a
  Reflect-Orbital beam flash is a genuinely horizon-hugging point source, not city skyglow. See
  `CLAUDE.md`'s "Subsystem: Light Pollution Dome" for the full writeup.
- **Not done / needs your eyes:** `lightPollutionGain`'s default almost certainly needs re-tuning
  now that the dome can actually reach zenith — the acceptance test above should be re-run in-app.

---

### S3 — Flare brightness ceiling `[x]` **P1**

**Root cause is concrete.** The flare-source pass injects the sun as a virtual point at a fixed
`sunFlareRefIntensity = 40.0` (`SatelliteSim.h:1330`), and satellite `effectFlare` is unbounded, so
a reflector at favourable geometry simply exceeds 40 and out-shines the sun. Three fixes, in order:

1. **Soft ceiling relative to the sun.** Reinhard-style rolloff in `sat_flare.comp`:
   `f' = sunRef × f / (f + sunRef)`. Nothing can ever exceed the sun, ordering between satellites is
   preserved, and the low end is untouched (at `f ≪ sunRef`, `f' ≈ f`). One line, no new tunables.

2. **Decouple brightness from size — this is the direct answer to "bright point source becomes a
   white blob."** The blob is a *size* problem masquerading as a brightness problem. A real
   point source grows on a sensor because of PSF spill and core saturation, and that growth is
   **logarithmic in flux, not linear**. Currently `angularSize` scales roughly linearly. Change to
   `angSize = base + k × log(1 + f)` and a 100× brighter satellite gets a modestly bigger core plus
   much longer streaks — which is what the eye actually reports as "blindingly bright."

3. **Keep the night drama in the glow, not the core.** Scale `flareStreakGain` /
   `flareGlowGain` with darkness (eye adaptation) rather than letting the core blow out. Both are
   already members; this is a mapping, not new machinery.

Together: the sun stays the brightest object in every frame, and satellites get *more* visually
striking at night rather than merely wider.

**Effort:** S. Highest ratio of visual payoff to code changed in this plan.

**Done (session 30):**
1. Implemented as specified — `SatFlarePC`'s `pad1` repurposed to `sunRefIntensity` (mirrors
   `sunFlareRefIntensity`, already sent to the flare-source pass), applied in `sat_flare.comp`
   right before the visibility cull.
2. **Already done, pre-existing.** `angSizeBase`/`glowSigmaPx` in `sat_flare.comp` already use
   `log`/`log2` of `effectFlare`, not a linear scale — this plan item's premise (linear size) no
   longer matched the code by the time this phase started.
3. Implemented — `flareStreakGain`/`flareGlowGain` scaled by a darkness factor (`sunDirENU.w`,
   same formula as `updateStars()`'s `nightFactor`) with a `0.35` day floor rather than a hard
   on/off, at both the blur pass (`recordCompute`) and the composite pass (`recordDraw`).

---

### S4 — Previously-planned, never-completed work `[x]` (audited)

| Item | Status | v1.1 call |
|---|---|---|
| **Terrain march altitude/distance fade** | **`[x]` Done, predates session 30** (`TERRAIN_PLAN.md` session 31 log — `tCap`/step-budget fade implemented). Originally: open, and listed in `TERRAIN_PLAN.md`'s own "NEXT SESSION" as a *measured* dominant cost. | **IN — P0.** This was the single biggest low-end-hardware win available. Mirrors the cloud `cloudDistFadeStartM/EndM` change; degrades to the existing sea-level sphere, so it degrades to "smooth textured Earth," not to nothing. |
| **Step 7 / C10 — city light upwelling into sky glow** | Partially superseded. The session-26 pollution dome *dims what's behind the glow*; Step 7 spec'd *adding glow to the sky itself*. Cities currently never brighten the sky. | **IN — P1.** It is the other half of S2 and directly serves the S2 acceptance test. **Done** — found already implemented in `sat_sky.frag` ("Step 7 / C10" comment block) by the time this phase started; not touched this session beyond the S2(c) isotropic-dome fix above, which also improves it. |
| **C9 — composite & performance** | Effectively delivered by sessions 23 (half-res cloud compute) and 29 (profiling + fixes). | **Close as done.** Update `TERRAIN_PLAN.md`. |
| **C14 — Anvil height-profile spread** | Not started; deferred 4+ times. | **CUT.** Cosmetic cloud shaping with no user-visible gap. Your instinct that "there is probably a good reason" is right here. |
| **Step 10 — orbital camera / coordinate decoupling** | Not started; large rework. | **OUT of v1.1**, but do a QA pass on "what visibly breaks at 400 km" (see NEW-6) — users *will* fly up there, especially after UC3 teaches them to. |
| **Noise repetition cleanup** | Backlog | **OUT.** Quality polish, not a release gate. |
| **Satellite-reflected light onto terrain; aurora ground-cast light** | Raised, never designed | **OUT.** |
| **Moon halo / directional `moonBright`** | Explicitly deferred, session 28 | **OUT.** |

---

## New items for a public release

Not in your list; each is either a real hazard found in the code or a standard shipping requirement.

### NEW-1 — Version + commit stamp `[x]` **P0** *(you asked for this)* (done, predates session 30 — `src/version.h.in`)

`VERSION` currently feeds only `EXE_BASENAME` in CMake; **no version string exists anywhere in the
C++**. Add:

- `src/version.h.in` → `configure_file` → `APP_VERSION`, `GIT_COMMIT` (`git rev-parse --short HEAD`,
  with a `unknown` fallback for source-zip builds), `BUILD_DATE`.
- Displayed in the intro, in the settings window title bar, and in the About/Attributions tab.
- Written into `settings.json`, `perf_profiles/profile_log.jsonl`, screenshot metadata, and the log
  file (NEW-2). You will get bug reports; every one of them should carry a build id.
- Tag releases (`git tag v1.1.0`) — `git describe` currently finds nothing, so `release.bat`'s
  macOS/GitHub-Actions path is untested in practice.
- Bump `VERSION` → `1.1.0`. Note `dist/` already contains a `SAT_LIGHT_SIM_v1.1.0_Light_Beams_*.zip`
  built while `VERSION` still said `1.0.0` — clean that up so the naming is trustworthy.

### NEW-2 — Graceful failure + log file `[x]` **P0** (done, predates session 30)

Right now, on a machine with no Vulkan driver or a driver missing a required feature, the app
almost certainly hard-crashes or silently exits. For a public release this is the highest-value
robustness item, because it converts "the game doesn't work" into an actionable report.

- Wrap instance/device creation; on failure show a native message box (Win32 `MessageBoxA`, no new
  dependency) naming what was missing and linking to GPU-driver update instructions.
- Write `satlight_log.txt` (path per NEW-4): version/commit, GPU name and driver version, selected
  preset, resolution, and any Vulkan validation or init errors.
- Check `maxPushConstantsSize ≥ 144` at startup (see UC5) and fail loudly rather than mysteriously.

### NEW-3 — Crash-safe mode `[x]` **P1** (done, predates session 30)

Write a sentinel file at startup, delete it on clean exit. If it is present at launch, come up in
**Planetarium** preset with a one-line notice. Roughly 30 lines of code, and it converts the worst
possible new-user outcome (launch → hang/crash → uninstall) into a recoverable one. Pairs directly
with UC1.

### NEW-4 — User-data paths `[x]` **P0** (done, predates session 30 — `src/Paths.cpp`)

`settings.json`, `perf_profiles/`, and (as proposed) `screenshots/` all write to `exeDir_`. **If the
game is installed anywhere read-only — Program Files, or an itch.io app-managed directory — every
one of these silently fails**, and settings never persist. Move writes to
`%APPDATA%/SatLightSim/` (Windows) / `~/.local/share/SatLightSim/` (Linux) /
`~/Library/Application Support/SatLightSim/` (macOS), keeping a portable-mode fallback: if a
`portable.txt` sits next to the exe, use `exeDir_` as today. Read-only data
(`constellations.json`, `assets/`, `shaders/`) stays next to the exe — only writes move.

### NEW-5 — Settings schema versioning + reset `[x]` **P0** (done, predates session 30 — `kSettingsSchemaVersion`)

A v1.0 `settings.json` will be read by v1.1, which now has presets, new keys, and re-tuned defaults.
Silent partial loads produce configs no user could have chosen and no one can diagnose. Add a
`"schema_version"` field; on mismatch, keep camera/audio/keybindings and reset the graphics section
to the auto-detected preset. Also add a **Reset to defaults** button per settings tab — cheap, and
it is the first thing you will ask a bug reporter to try.

### NEW-6 — QA pass matrix `[ ]` **P1**

You test at your spawn point at your altitude. Users will not. Minimum matrix, each preset × each:
sea level, 10 km, 100 km, 400 km (LEO) · day, twilight, night · over ocean, over desert, over a
major city, at the aurora oval · time-warp min and max · window resize and fullscreen toggle mid-run
· gamepad-only and mouse-only. `CLAUDE.md` already documents several altitude-dependent
approximations (`renderScale < 1` disables depth-based satellite occlusion; cloud/terrain distance
fades) — this is where those get characterized rather than discovered by a stranger.

### NEW-7 — Frame pacing / FPS cap `[x]` **P1** (done, predates session 30)

`VulkanContext.cpp:366` prefers `MAILBOX` over `FIFO`. On a laptop that means the GPU runs flat out
forever: fans, heat, thermal throttling, and battery drain — a genuine comfort issue for exactly the
low-end audience this release targets. Add a **V-Sync / FPS cap** option (Off / 30 / 60 / 120 /
V-Sync), defaulting to V-Sync (`FIFO`). Put it in the top-level Display tab next to Render scale.

### NEW-8 — Photosensitivity + comfort `[ ]` **P2**

The content includes bright flares, aurora shimmer, and (at high time-warp) rapid day/night
strobing. Add a startup notice line and a **Reduce flashing** option that caps flare peak intensity,
slows aurora shimmer, and limits maximum time-warp. Cheap, and standard practice for anything with
this kind of luminance dynamic range. Fold the notice into the intro (UC3) or the first-run dialog.

### NEW-9 — Distribution & docs `[ ]` **P1**

- `README.md` fit for the itch.io / GitHub Releases page: what it is, screenshots, controls,
  **minimum spec** (Vulkan 1.x, "Intel Iris Xe or better at Low", "GTX 1060 / RX 580 class for
  High"), and a known-issues list.
- `CHANGELOG.md` — v1.0 → v1.1 is an enormous delta and it is worth showing off.
- Verify `release.bat` copies `data/reflector_targets.json` (S1) and
  `THIRD_PARTY_NOTICES.txt` (UC2) into both platform stagings — it currently enumerates data files
  by name, so new ones must be added by hand.
- The macOS GitHub Actions path is untested (no tags exist). Either test it or drop the macOS claim
  from the release notes.

---

## Sequencing

Each phase ends in a buildable, testable state. Phases 1 and 2 are the release gates; 4 is cuttable.

| Phase | Contents | Why here |
|---|---|---|
| **1 — Foundation** `[x] DONE` | NEW-1 version stamp · NEW-4 user-data paths · NEW-5 settings schema · NEW-2 graceful failure + log | Everything downstream writes files and reports versions. Doing these first means every later test run produces diagnosable output. |
| **2 — Performance & presets** `[x] DONE` | S4 terrain fade (P0) · UC1 presets + auto-detect · NEW-7 FPS cap · NEW-3 safe mode | The release-blocking accessibility work. Terrain fade goes **before** presets so the presets are calibrated against the fixed cost, not the current one. |
| **3 — Visual truth** `[x] DONE (session 30)` | S2 stars + pollution (a & c) · S3 flare ceiling · Step 7 city sky glow | The "this looks real now" phase. S2(c) and Step 7 are both light-pollution work and share testing; do them together. |
| **4 — Experience** `[x] DONE (session 31)` | UC3 intro cinematic (+ UC1 benchmark hook) · UC4 dual-input docs · UC6 screenshots · S1 reflector targets | User-facing polish. UC3 depends on UC1's preset system existing. |
| **5 — Release** `[ ]` | UC2 attributions · NEW-8 comfort · NEW-6 QA matrix · NEW-9 docs & packaging · tag v1.1.0 | UC2 is last only because it is blocked on your provenance answers — **start gathering those now**, it is P0 and it is the one item that cannot be rushed at the end. |

*(Bonus, not in this sequencing: real-ephemeris planets — see Session log above and `PLANETS_PLAN.md`.)*

### Recommended cut line

Ship-blocking: **Phase 1, Phase 2, S3, UC2, NEW-6, NEW-9.**
Everything else improves the release but does not gate it.

**As of session 31:** Phases 1, 2, and 4 are done, plus S3. The only remaining ship-blockers are
**UC2 (blocked on your provenance answers), NEW-6 (QA matrix), and NEW-9 (docs & packaging)** —
i.e. all of Phase 5. UC2 is still the long pole and still worth starting now.

---

## Open questions for you

1. **Asset provenance** (UC2) — the table above. Blocking, and the long pole.
2. **Reflector target data source** (S1) — Global Energy Monitor (CC BY 4.0) vs. OpenStreetMap
   (ODbL, share-alike) vs. a hand-curated list of ~50 famous sites. The hand-curated list has no
   license complications and is arguably better for a simulation, since you control the geographic
   spread.
3. **Gamepad UI navigation** (UC4) — virtual cursor (cheap, ~1 day) or real focus traversal (~1
   week)? My recommendation is the virtual cursor.
4. **Distribution target** — itch.io, Steam, GitHub Releases? Steam adds hard requirements
   (Steamworks init, cloud saves, controller config, store assets) that would change several items
   above. The plan as written assumes itch.io + GitHub Releases.
5. **Is Planetarium preset a *preset* or a *mode*?** If some users are expected to live in it
   permanently, it may deserve its own front-page presentation on the intro screen ("Full
   simulation" vs. "Planetarium") rather than being buried in Settings.
