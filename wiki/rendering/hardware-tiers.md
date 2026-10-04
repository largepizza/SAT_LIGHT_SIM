# Weak-hardware tiers

How SAT LIGHT SIM runs on old and integrated GPUs: why the full sky shader cannot, the two stand-in sky tiers
(Potato and SKY_LITE) and what each drops, how the graphics presets select them, and the Vulkan limits the app
needs compared with what the specification guarantees. Player-facing preset advice is on
[Graphics settings](../using/graphics-settings.md).

## Why the full sky shader is too big

`shaders/sat_sky.frag` is about 6000 lines and compiles to one very large fragment function. The reference
weak machine is a 2015 MacBook Pro (AMD Radeon R9 M370X, GCN 1.0) running macOS 12 through MoltenVK, where
the full shader alone measured about 490 ms per frame: the whole frame.

The cause is the shader's **compiled size**, not any one feature:

- The register allocation is sized for the shader's largest path, whether or not that path runs. Peak register
  pressure collapses the number of wavefronts in flight, and with them the GPU's ability to hide texture-fetch
  latency.
- A knockout bit or a quality slider skips **execution**, not **code**, so no setting of the full shader fixes
  it.
- Particular terms are disproportionately expensive on that hardware: the Milky Way's equirect projection
  (`atan2` and `asin`) plus its panorama fetch, per-step noise in the atmosphere loop, and wide blur kernels.

The answer is to compile **smaller shaders**: a separate minimal shader, and a `SKY_LITE` variant of the full
one with whole blocks removed by the preprocessor.

## The two tiers

Both stand-ins bind through the same pipeline layout (`skyBgPipeLayout`) and descriptor set (`skyDescSet`) as
the full shader, each declaring only the bindings it reads, and both write the same unified depth. They are
selected in `recordDraw()` by a `debugDisableMask` bit, as pipeline swaps rather than in-shader branches:

| Tier | Bit | Pipeline | Shader |
|---|---|---|---|
| **Potato** | 262144 | `skyBgMinimalPipeline` | `shaders/sat_sky_minimal.frag`, its own file, about 450 lines |
| **SKY_LITE** | 524288 | `skyBgLitePipeline` | `sat_sky.frag` compiled with `-DSKY_LITE` → `sat_sky_lite.frag.spv` |

Bit 262144 wins if both are set. A log line in `satlight_log.txt` reports `MINIMAL`, `LITE (SKY_LITE)` or
`FULL sat_sky.frag` whenever the choice changes.

### Potato (`sat_sky_minimal.frag`)

A different renderer, not a cut-down one:

- **Atmosphere in closed form**, no raymarch. The optical depth along a ray is approximated as
  scale height × the air density at the ray's lowest point × the Kasten-Young airmass, evaluated at the ray's
  closest approach to the surface. That works from the ground (dense, blue overhead, bright horizon) and from
  space (a thin wash over the sunlit disc, a bright limb, black away from the Earth). Rayleigh and Mie phases
  against the Sun; one 32-tap arithmetic loop. The Sun path's airmass explodes past the horizon, so day,
  twilight and night need no special case.
- **Ground**: the day and night Earth textures across the terminator plus aerial perspective, and the city
  detail textures.
- **Clouds**: one flat drifting shell at 3 km with terminator lighting, read from the same weather cube the
  volumetric clouds use.
- **Ocean**: one noise tap for the slope, Fresnel and a Blinn sun glint.
- **Moon**: a textured disc. **Sun**: disc, corona and the same `lensFlare()` as the full shader.
- **Not drawn**: the Milky Way, volumetric clouds, aurora, airglow, real ocean waves, satellite meshes.

The Milky Way does not fit this tier: a textured panorama drops the reference machine to about 2 FPS, and a
trig-free procedural band looks poor. The discrete star catalogue (point sprites) covers the night sky instead.
Measured on the reference machine: about 60 FPS.

### SKY_LITE (Planetarium)

The full shader with `#ifdef SKY_LITE` cuts. Everything not listed stays, including terrain, the sea and the
Reflect beams' ground spots:

| Cut | Why it matters |
|---|---|
| Milky Way, and its reflection in the sea | the equirect projection and panorama fetch |
| zodiacal light and gegenschein | new post-tonemap sky terms default to being cut here |
| the 64-bin satellite sky-glow loop | 64 iterations with an `acos` on every pixel |
| flat cloud layers 2-3 (cirrus and high decks); only layers 0-1 | |
| the cloud target's joint-bilateral read | single bilinear tap |
| green and sodium airglow in the atmosphere loop | two noise-based coverage masks per step: the dominant cost |
| city-glow upwelling past the first 3 atmosphere steps | the air density makes later steps negligible |
| the zenith sky-ambient integration halved (`N_ZT` 4 → 2) | |
| terrain sun shadows | |

On the reference machine SKY_LITE takes the frame from about 2 FPS (full shader) to tens of FPS.

## Presets

`applyGraphicsPreset()` (`src/simulations/SatelliteSimUI.cpp`) sets a knockout mask, the render scale and
the quality sliders. The two weak-hardware presets:

| | Potato | Planetarium |
|---|---|---|
| sky shader | Potato (262144) | SKY_LITE (524288) |
| terrain relief (1), ocean reflection (8), red airglow (16), aurora (32), beams (128), cloud shadow (256), beam cloud block (512), fog and dust (2048) | off | off |
| volumetric cloud march (32768), cirrus march (16384) | off | off |
| scene depth pass (1024), beam ray loop (8192), sky-glow loop (65536) | off | on (sky-glow is compiled out anyway) |
| cloud coverage | 0 | 0 |
| atmosphere view samples (min / max) | 6 / 20 | 4 / 10 |
| render scale | 1.0 | 1.0 |
| procedural terrain detail, shadows, materials, textures | off | off |

Atmosphere scattering (bit 2) stays on in both: on the reference machine it costs a few milliseconds, and
without it there is no sky gradient at all.

**Render scale stays 1.0 on these tiers.** Below 1 the background takes an extra offscreen render pass and a
`vkCmdBlitImage` every frame. On MoltenVK each is another command-encoder boundary, which costs more than the
pixels it saves. The render-scale prepass therefore has no Lite or Minimal variant: it never runs for them.
Sky TAA is also off on both tiers (`skyTaaWanted()` excludes them).

Low, Medium, High and Ultra use the full shader. Medium runs at render scale 0.85, which also turns sky TAA off
there.

## The hardware floor

Several of the app's needs sit above the Vulkan specification's **guaranteed minimum**.
`VulkanContext::logDeviceLimits()` logs the relevant limits at every launch and throws a named error when one is
short, so a report from a machine nobody has access to is answerable from its log.

| Limit | Guaranteed minimum | The app needs | Why |
|---|---|---|---|
| `maxPushConstantsSize` | 128 | **128** | the main push-constant blocks are exactly 128 bytes (checked in `pickPhysicalDevice()`) |
| `maxImageDimension2D` | 4096 | **14999** | the DEM is 14999 × 7500; GCN 1.0 and Metal allow 16384, so there is little headroom |
| `maxImageDimension3D` | 256 | **1024** | `aurora_noise.comp` bakes a 1024 × 16 × 256 volume |
| `maxComputeWorkGroupInvocations` | 128 | **256** | `local_size 16 × 16` in the cloud, scene-depth and blur passes |
| `maxComputeSharedMemorySize` | 16 KB | 6 KB (checked) | tile-cull lists |
| `maxPerStageDescriptorSampledImages` / `Samplers` | 16 | **16** | `sat_sky.frag` binds 16 combined image samplers: at the floor |
| `maxPerStageDescriptorStorageBuffers` | 4 | **11** | `sat_orbit.comp`'s set |
| `maxPerStageDescriptorStorageImages` | 4 | 4 (not checked) | the sky set declares four fragment storage images (mesh radiance, mesh distance, reflection G-buffer, far cloud layer) |
| `pointSizeRange[1]` | 64 (with `largePoints`) | up to 1024 | glare sprites clamp to it, so they are smaller on hardware with a low limit |

!!! warning "Invariant: the sky set is at its sampler floor"
    `sat_sky.frag` already uses all 16 sampled images the specification guarantees per stage. A new texture for
    the sky shader must merge into an existing one (an array layer, a channel of another texture) or be read as
    a storage image. The newer full-resolution inputs (meshes, far cloud layer) are storage images read with
    `imageLoad` for this reason, and that budget is now also at its floor of 4.

The storage-buffer limit is the realistic one to hit under MoltenVK, which maps storage buffers, uniform
buffers and vertex buffers into Metal's 31 per-stage buffer slots.

### The 128-byte push constant rule

Old AMD integrated parts report exactly the guaranteed 128 bytes of push-constant space. `SatDrawPC`,
`PointDrawPC`, `CloudMarchPC`, `SatOrbitPC`, `SatFlarePC` and `SkyTaaPC` each `static_assert` to exactly 128 bytes;
smaller blocks (`GlarePC`, `FlareSourcePC`, 112 bytes) assert their own sizes.

!!! warning "Invariant"
    No push-constant block may grow past 128 bytes. When one needs another field, move per-frame-uniform data
    into the `CloudParams` UBO (its "push-constant relief" block) or another UBO, and keep the gate in
    `pickPhysicalDevice()` equal to 128. Raising that number would reject exactly the hardware the limit exists
    for. Fields that differ **between draws** within a frame (the point draws' `noTwinkle` and
    `manualTerrainTest`) are the only ones that must stay in a push constant.

### Device features

Two features the app uses are near-universal but not guaranteed: `largePoints` (point sprites larger than one
pixel) and `shaderStorageImageExtendedFormats` (RGBA8 storage images). They are checked before
`vkCreateDevice`, which would otherwise fail with `VK_ERROR_FEATURE_NOT_PRESENT` and no indication of which
feature is missing.

**Linear blit filtering** is a per-format optional feature. `VulkanContext::bestBlitFilter()` picks LINEAR or
NEAREST per format for both mipmap generation and the render-scale upscale; the upscale exists for weak
hardware, the least safe place to assume the feature. The sky TAA path additionally needs the swapchain to
support transfer-destination use (`swapTransferDstSupported`) and is skipped without it.

### VRAM

Video memory is the one limit not checked. The shipped textures need roughly 500 MB of GPU memory before
mipmaps (8K day, night, cloud and Milky Way maps, plus the 14999 × 7500 DEM at about 112 MB on its own). Nothing
streams or downsamples them, against 2 GB on the reference machine. The per-satellite buffers are sized to the
loaded roster, about 100 bytes per satellite: on the order of 130 MB for the default roster of about
1.4 million satellites.

## Where in the code

- `shaders/sat_sky_minimal.frag`; the `SKY_LITE` blocks in `shaders/sat_sky.frag`; `CMakeLists.txt` (the
  `SKY_LITE` compile).
- `src/simulations/SatelliteSimUI.cpp`: `applyGraphicsPreset()`.
- `src/simulations/SatelliteSim.cpp`: the sky pipeline choice in `recordDraw()`, `skyTaaWanted()`.
- `src/VulkanContext.cpp`: `pickPhysicalDevice()`, `logDeviceLimits()`, the feature check, `bestBlitFilter()`.
- `src/simulations/SatelliteSim.h`: the push-constant structs and their `static_assert`s.
