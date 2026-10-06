# Graphics settings

How to trade picture quality for frame rate: the graphics presets and exactly what each one turns down, the
render scale and the automatic render scale, temporal anti-aliasing (which doubles as an upscaler), the frame
limiter, and the handful of advanced sliders that matter most. How the effects themselves work is in
[Rendering](../rendering/index.md); how to measure costs precisely is on [Profiling](../development/profiling.md).

## Presets

Settings → **Display** → *Graphics* → *Preset* has six buttons: **Potato**, **Planetarium**, **Low**,
**Medium**, **High** and **Ultra**. A preset sets the render scale, switches whole effects off, and sets the
quality sliders of the advanced tabs. A seventh state, **Custom**, has no button: the row reads *Preset (custom)*
when you have changed any slider on an advanced tab by hand, and means "keep what is set".

On a first launch the program picks **Medium** for a discrete GPU and **Planetarium** for anything else
(integrated graphics, a software or virtual device). It then adjusts one step after the intro if the intro plays
through (see [Getting started](getting-started.md#the-first-launch)). After a crash it starts on Planetarium.

### What each tier turns off

| Effect | Potato | Planetarium | Low | Medium | High | Ultra |
|---|---|---|---|---|---|---|
| Sky shader | minimal | lite | full | full | full | full |
| Volumetric clouds | off (one flat deck) | off | on | on | on | on |
| Terrain relief | off | off | on | on | on | on |
| Procedural terrain detail, shadows, materials, close-up textures | off | off | on | on | on | on |
| Sea reflections | off | off | on | on | on | on |
| Aurora | off | off | on | on | on | on |
| Red airglow | off | off | on | on | on | on |
| Mirror beam spots and beam light in clouds | off | off | on | on | on | on |
| Beam pointing rays | off | on | on | on | on | on |
| Cloud shadows on the ground | off | off | on | on | on | on |
| Fog, dust and ice fog | off | off | on | on | on | on |
| Shared scene depth (terrain occlusion for clouds and beams) | off | on | on | on | on | on |
| Temporal anti-aliasing / upscaling | off | off | on | on | on | on |
| Render scale | 100% | 100% | 50% | 67% | 100% | 100% |

- **Potato** draws the sky with a separate, much smaller shader: an analytic atmosphere, the day and night Earth
  textures, one flat drifting cloud deck, a simple sea glint and the textured Moon. No Milky Way, no volumetric
  clouds, no aurora, airglow or real waves. It exists for very weak or translated GPUs (old integrated graphics,
  MoltenVK on a 2015 Mac) where the full sky shader is too large to run well.
- **Planetarium** is the "everything off" tier. It keeps the full shader's look but compiles it without the Milky
  Way, the satellite sky glow, the upper cloud layers, the aurora's glow on the ground and most of the airglow.
  The Earth is flat (no relief), there are no volumetric clouds, and the atmosphere uses fewer samples. It is the
  starting tier on integrated GPUs.
- **Low** is Medium's picture at a 50% render scale: every effect is on, and the clouds, the depth pass and the
  temporal upscaler all follow the lower resolution, so it costs roughly half of Medium's GPU frame.
- **Medium** turns nothing off; it uses smaller cloud and atmosphere budgets than High and a 67% render scale.
- **High** is the tuned default, at full resolution.
- **Ultra** raises every budget to its slider's ceiling and reaches much farther for terrain and clouds. It is
  meant for screenshots and films.

As a guide, on an RTX 3070 Ti at 1600 × 900 in heavy storm views, Low runs at about 10 to 14 ms of GPU time per
frame and Medium at about 14 to 18 ms.

### The quality values a preset sets

| Slider (tab) | `settings.json` key | Potato | Planetarium | Low | Medium | High | Ultra |
|---|---|---|---|---|---|---|---|
| March budget (Clouds) | `clouds_v2.max_iters` | 160 | 160 | 300 | 300 | 400 | 640 |
| Light steps (Clouds) | `clouds_v2.light_steps` | 3 | 3 | 6 | 6 | 6 | 8 |
| Step growth (Clouds) | `clouds_v2.step_growth` | 0.025 | 0.025 | 0.015 | 0.015 | 0.01 | 0.008 |
| View samples (min) (Atmosphere) | `clouds.view_samples_min` | 6 | 4 | 6 | 6 | 6.5 | 16 |
| View samples (max) (Atmosphere) | `clouds.view_samples_max` | 20 | 10 | 96 | 96 | 160 | 256 |
| Light samples (Atmosphere) | `clouds.light_samples` | 2 | 2 | 2 | 2 | 2.4 | 4 |
| Sea octaves / Detail octaves (Ocean) | `clouds.ocean_sea_octaves` / `ocean_detail_octaves` | 3 / 5 | 3 / 5 | 3 / 5 | 3 / 5 | 3 / 5 | 3 / 5 |
| Refl samples (Ocean) | `clouds.ocean_refl_samples` | 3 | 3 | 6 | 6 | 6 | 6 |
| Terrain fade start (m) (Terrain) | `clouds.terrain_dist_fade_start_m` | 50 000 | 50 000 | 50 000 | 50 000 | 50 000 | 900 000 |
| Terrain fade end (m) (Terrain) | `clouds.terrain_dist_fade_end_m` | 100 000 | 100 000 | 600 000 | 600 000 | 900 000 | 3 600 000 |
| Terrain detail, shadows, materials, close-up textures (Terrain) | `clouds.terrain_*_strength` | 0 | 0 | 1 | 1 | 1 | 1 |

The sea's wave octaves stay at full quality on every tier: they are cheap and the sea looks wrong without them.

!!! warning "A named preset is re-applied at every launch"
    When the saved preset is anything but Custom, its values are applied again on the next start. Settings that
    do not switch the preset to Custom are then overwritten, most visibly the **Render scale**: on Medium, a
    render scale raised to 100% comes back as 67% after a restart. To keep a hand-picked render scale, change any
    advanced slider (which makes the preset Custom) or pick High or Ultra, which already run at 100%.

## Render scale

**Render scale** (Display tab, 50% to 100% in 5% steps; key `display.render_scale`) renders the background at a
reduced resolution: the sky, terrain and sea, and, with **Clouds follow render scale** on, the volumetric clouds
and the terrain depth pass as well. Those two always run at half of the background's resolution, so at 50% they
march a quarter of the pixels they march at 100%. The clouds' noise detail is still filtered for the full
resolution pixel, so their texture is kept, only softer.

Satellites, stars, planets, city light sprites, satellite meshes and the interface always render at full
resolution, so the points stay sharp.

With temporal anti-aliasing on (the default), a reduced render scale is **upscaled over time** (next section),
so 50% keeps most of the detail of 100%. With temporal anti-aliasing off, the background is simply stretched to
the window, and terrain edges hide satellites and stars less exactly.

**Clouds follow render scale** (`display.clouds_follow_render_scale`, on by default) can be turned off to keep the
clouds and the depth pass at half the window's resolution whatever the render scale; they then cost the same at
every scale.

### Automatic render scale

**Automatic render scale** (Display tab, off by default) changes the render scale by itself to hold a frame rate.
It lowers the scale in 5% steps when the GPU frame runs over 90% of the target frame time, and raises it one step
at a time when there is clearly room, never below **Lowest render scale**. A step does not restart the clouds:
their history is resampled to the new size.

| Setting | `settings.json` key | Default |
|---|---|---|
| Render scale | `display.render_scale` | 100% (or the preset's) |
| Clouds follow render scale | `display.clouds_follow_render_scale` | on |
| Automatic render scale | `display.dynamic_resolution` | off |
| Target frame rate | `display.dynamic_target_fps` | 60 (30 to 144) |
| Lowest render scale | `display.dynamic_min_scale` | 50% |

## Temporal anti-aliasing

**Temporal anti-aliasing** (Display tab, key `display.sky_taa`, on by default) jitters the background a fraction
of a pixel each frame and blends the result with the previous frames, reprojected for camera and observer motion.
It smooths mountain silhouettes, terrain detail, the sea and city lights, and the edges of the clouds. It costs
about 1 ms at 1600 × 900.

Below 100% render scale it becomes a **temporal upscaler**: the background is rendered at the reduced resolution
with a jitter that cycles through 16 sub-pixel positions, and each full-resolution frame is rebuilt from the
current samples and the reprojected history. Terrain textures and patterns are filtered for the full-resolution
pixel, so the upscaled image keeps far more detail than a plain stretch. It costs about 1 ms more than the
stretch.

While the camera is completely still and time is paused or runs at 1x, the full history is kept, so
lights smaller than a pixel (far city lights, glints on the sea) average out instead of blinking. In motion the
history is limited to what the current frame supports, which avoids ghosting.

It needs the full sky shader, so it is off on Potato and Planetarium.

| Setting | Key | Default |
|---|---|---|
| Temporal anti-aliasing | `display.sky_taa` | on |
| History weight while still | `display.sky_taa_weight` | 0.1 |
| History weight while moving | `display.sky_taa_weight_moving` | 0.35 |

The two weights are the share of each new frame in the blend; they have no sliders and are set in
`settings.json` (range 0.02 to 1).

## Frame limiter and window

| Setting (Display tab) | Choices | Default |
|---|---|---|
| Frame limiter (`display.fps_cap_mode`) | Off (uncapped, tearing allowed), 30, 60, 120, V-Sync | V-Sync |
| Window mode | Windowed, Fullscreen (F11 toggles) | Windowed (maximised) |
| Text scale (`display.ui_scale`) | 0.75 to 2 | 1.5 |
| Units | Metric, Imperial | Metric |

With V-Sync, a frame that misses the display's refresh waits for the next one, so on a 60 Hz display a GPU frame
just over 16.7 ms shows as 30 frames per second. If the frame rate halves suddenly, lowering the cost slightly
often doubles it back; the automatic render scale is built for exactly this.

## The sliders that cost the most

When a preset is close but not quite right, these are the sliders worth adjusting first. Each is on an advanced
tab (turn on *Show advanced settings* in Display).

| Slider | Tab | Effect |
|---|---|---|
| March budget | Clouds | Steps per cloud ray. Lower is cheaper; too low and distant clouds along the horizon thin out |
| Light steps | Clouds | Steps toward the Sun from each cloud sample. The main per-sample cost; below about 4 storm tops shimmer |
| Step growth | Clouds | How fast the cloud steps lengthen with distance. Larger is cheaper and coarser far away |
| Full rate above (km) | Clouds | Above this altitude every cloud pixel is recomputed every frame (sharper from orbit, costlier) |
| Adaptive rate while moving (0/1) | Clouds | While moving, only screen tiles with visible cloud parallax are marched at full rate. On by default; worth keeping |
| View samples (min / max) | Atmosphere | Samples along each sky ray |
| Terrain detail, Terrain shadows | Terrain | The procedural relief on top of the elevation map and its sun shadows: several milliseconds on the ground in mountains, little from aircraft height, nothing from orbit |
| Terrain fade start / end (m) | Terrain | How far the detailed terrain march reaches |
| Fog amount, Dust amount | Weather | Fog and dust have their own small march |
| Lightning rate: storms, Lightning rate: Cb towers | Weather | Both at 0 turn lightning off |

Some photometry options are expensive at the scale of a million satellites:

- **Part occlusion** (Photometry, *Satellite models*): parts of a modelled satellite shadow and hide each other.
  Off by default; with millions of satellites it costs roughly ten times the orbit computation.
- **Full-renderer reflections** and **Sharp mirror reflections** (Photometry, *Satellite models*): what satellite
  meshes reflect. They only cost anything when satellites are drawn as meshes.

## Performance tips

- **Start from the preset**, then adjust. Pick the highest preset that holds your target frame rate in the views
  you care about; the cost depends a lot on where you are. If you are close, the automatic render scale fills the
  gap.
- **Storms and thick cloud are the most expensive views**, especially near the ground looking at the horizon.
  Clear skies and views from orbit are cheap.
- **Mountains on the ground** are the most expensive terrain views.
- **Orbital mirrors** (Reflect Orbital) concentrate many beams over a few sites at night; those views cost more
  near the sites.
- **Turning off constellations** in the Constellations tab removes their satellites from the GPU work.
- **Measure**: Settings → **Performance** (an advanced tab) shows the GPU and CPU cost of each pass, smoothed over
  a few dozen frames. *Save snapshot* (also F9) records the numbers with the location, so a slow view can be
  reported exactly.

## Where in the code

- `src/simulations/SatelliteSimUI.cpp`: `applyGraphicsPreset()` (the preset table),
  `seedGraphicsPresetFromDevice()`, `buildSettingsDisplayTab()`.
- `src/simulations/SatelliteSim.cpp`: the post-intro adjustment in `finishIntro()`, `skyTaaWanted()`,
  `updateDynamicResolution()`, `computeHalfExtent()` and `recreateComputeScaledTargets()` (the clouds following
  the render scale).
- `shaders/sky_taa.comp`: the temporal resolve and upscaler.
- `src/simulations/SatelliteSim.h`: `GraphicsPreset`, `FpsCapMode`.
- The weak-hardware sky shaders are described on [Weak-hardware tiers](../rendering/hardware-tiers.md).
