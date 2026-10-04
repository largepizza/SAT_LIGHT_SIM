# GPU conventions

The rules that keep the Vulkan and GLSL code correct: the `VulkanContext` helpers, barrier habits, the
128-byte push-constant limit, the hand-kept mirrors between C++ structs and GLSL blocks, the shared shader
headers, the precision rules for distances and positions, descriptor budgets, and how code structure affects
shader speed. Most of these rules exist because breaking them fails silently: everything compiles and the
image is wrong.

## `VulkanContext`

`VulkanContext` (`src/VulkanContext.h`) holds the core objects simulations read (`device`, `physicalDevice`,
`graphicsQueue`, `commandPool`, `commandBuffer`, `swapExtent`, `swapFormat`, `swapImages`, `renderPass`,
`renderPassLoad`, `renderPassBoot`, `framebuffers`, `depthFormat`, `pipelineCache`) and a few helpers:

| Helper | What it does |
|---|---|
| `loadShader(path)` | a `VkShaderModule` from a `.spv` file |
| `createBuffer(size, usage, props, buf, mem)` | buffer plus bound memory |
| `createImage(w, h, fmt, usage, img, mem, mips = 1, depth = 1)` | 2D or 3D image plus bound memory |
| `findMemoryType(filter, props)` | a memory type index |
| `generateMipmaps(cmd, img, fmt, w, h, mips)` | blits mips 1..n-1 from mip 0 (in `TRANSFER_DST_OPTIMAL` on entry); all mips end in `SHADER_READ_ONLY_OPTIMAL` |
| `bestBlitFilter(fmt)` | `LINEAR` if the format supports linear blit filtering, else `NEAREST` |
| `beginOneTimeCommands()` / `endOneTimeCommands(cmd)` | a one-shot command buffer, submitted and waited on (init-time uploads) |
| `imageBarrier(cmd, img, srcAccess, dstAccess, oldLayout, newLayout, srcStage, dstStage)` | one image memory barrier |

`imageBarrier()` covers the colour aspect, mip 0 and layer 0 only. A barrier on a depth image, a mip chain or
an array needs its own `VkImageMemoryBarrier`.

Linear blit filtering is an optional per-format feature, not a guarantee. `bestBlitFilter()` is used both for
mipmap generation and for the render-scale upscale, which is the path that exists for weak hardware: the
least safe place to assume an optional feature.

`VulkanContext::logDeviceLimits()` logs every device limit the app needs beyond Vulkan's guaranteed minimum,
and `pickPhysicalDevice()` throws a named error when one is short, so a report from an unfamiliar machine says
which limit failed. Device features are checked before `vkCreateDevice` for the same reason: requesting an
unsupported one fails with `VK_ERROR_FEATURE_NOT_PRESENT` and no indication of which. The current margins are
listed on [Weak-hardware tiers](../rendering/hardware-tiers.md).

## Synchronisation

The app has **one command buffer and one frame in flight**. `App::drawFrame()` waits on the frame fence first,
so anything the GPU wrote last frame (timestamps, readback buffers, screenshot copies) can be read on the CPU
afterwards in the same frame. Host-visible buffers the CPU writes each frame and the GPU reads in the same
frame need no barrier for the same reason.

Barrier habits:

- **Compute to compute** on a storage buffer: `SHADER_WRITE` to `SHADER_READ` (or `READ | WRITE`), with
  `COMPUTE_SHADER` on both sides.
- **Compute to vertex shader** on a storage buffer: `SHADER_WRITE` to `SHADER_READ` at `VERTEX_SHADER`, not
  `VERTEX_ATTRIBUTE_READ` (the vertex shaders read the buffers as storage, not as vertex attributes).
- **Indirect arguments** written by a compute shader also need `INDIRECT_COMMAND_READ` at `DRAW_INDIRECT`
  (which also covers indirect dispatches).
- **Transfer fills** before a compute pass (`vkCmdFillBuffer` to clear a histogram): `TRANSFER_WRITE` to
  `SHADER_READ | SHADER_WRITE`, transfer to compute.
- The clouds' screen-space images live permanently in `VK_IMAGE_LAYOUT_GENERAL` and are synchronised with
  memory barriers only.

`onResize()` must recreate graphics pipelines, because the viewport is baked into them; compute pipelines are
independent of the extent. The three main render-pass variants (`renderPass`, `renderPassLoad`,
`renderPassBoot`) differ only in load operations and share one list of subpass dependencies: render-pass
compatibility includes the dependencies, so a variant with different ones would make every draw invalid
against framebuffers and pipelines built for another.

## Push constants: 128 bytes

Vulkan guarantees only 128 bytes of push-constant space, and the old integrated GPUs the weak-hardware tiers
target report exactly that. **No push-constant block may exceed 128 bytes.** The largest blocks
(`SatOrbitPC`, `SatFlarePC`, `SatDrawPC`, `PointDrawPC`, `CloudMarchPC`, `SkyTaaPC`) are exactly 128 and
`static_assert` that size in `SatelliteSim.h`; smaller ones assert their own sizes. The device gate in
`pickPhysicalDevice()` requires 128 and must stay equal to the largest block.

A new per-frame value therefore goes into a uniform buffer, normally the `CloudParams` block below, which
every major shader binds. Push constants carry only what genuinely differs between draws in one frame: for
example `PointDrawPC::noTwinkle` (1 only on the planet draw) and `manualTerrainTest` (set on the trail draws).
If a block would grow past 128, the fix is the uniform buffer, not a larger gate.

## The `CloudParams` uniform block

`CloudParams` is the per-frame uniform block shared by most shaders (despite the name, it carries far more
than clouds: terrain, sea, sky, beams, debug flags, screen size). It is declared once in GLSL, in
`shaders/include/cloud_params.glsl`, and each consumer includes it after defining `CLOUD_PARAMS_BINDING`. Its
C++ mirror is `GpuCloudParams` in `SatelliteSim.h`, which the CPU fills each frame.

GLSL and C++ cannot share a declaration, so **`GpuCloudParams` is a hand-kept mirror**. Its `static_assert`
checks the total size, but the dangerous failure is a *permutation*: add a field at different positions in the
two files and the size is unchanged, everything compiles, and every field after the divergence point reads its
neighbour's value.

!!! warning "Invariant"
    After touching `cloud_params.glsl` or `GpuCloudParams`, run `python tools/check_cloud_params.py`. It extracts
    both field lists and fails on any difference in names or order. Prefer appending new fields at the end of
    both.

Several fields of the block are unread leftovers from earlier renderers; a later compaction can reclaim them.

`GpuCloudV2Params` (the volumetric clouds' own uniform block, mirrored from `CloudV2Params` in
`clouds_v2.glsl`) is built entirely of `vec4` and `mat4` members, so std140 packing cannot shift anything, and
`offsetof` assertions pin its layout. Keep the order identical in both.

## Shared struct layouts

Several storage-buffer records are shared between C++ and GLSL (`GpuSatOrbit`, `GpuSatVisible`,
`GpuReflectBeam`, `GpuSatType`, the list headers, and others). The rules:

- Prefer scalar fields (`float`, `uint`) and `vec4`; a C++ struct of plain floats matches std430 exactly.
- **A `vec3` aligns to 16 bytes in GLSL std430 and to 4 in C++** (`glm::vec3`). A struct ending in a `vec3`, or
  an array of structs whose size is not a multiple of 16, packs differently on the two sides. Add explicit pad
  fields in **both** declarations so the layout does not depend on either compiler's rounding.
- Every shared struct has a `sizeof` assertion, and the larger ones `offsetof` assertions. As with
  `CloudParams`, a size assertion does not catch a reordering, so keep field order identical and change both
  files in the same edit.
- Where one record type is used by several shaders, it lives in one header (`reflect_beam.glsl` for the beam
  record, `sat_mesh_common.glsl` for the mesh descriptor set, `glint_list.glsl`), and each shader declares its
  own buffer binding with its own access qualifiers.

## Shader headers

Shared GLSL lives in `shaders/include/*.glsl`, included with `#include` (`glslc -I`). Headers are never
compiled on their own, and every shader is rebuilt when any header changes
([Building](building.md#shaders)).

| Header | Holds |
|---|---|
| `common.glsl` | constants (Earth and atmosphere radii, scattering coefficients and scale heights, `kNoSurfaceT`), `raySphere`, `remap`, rotation helpers, phase functions |
| `cloud_params.glsl` | the `CloudParams` uniform block (define `CLOUD_PARAMS_BINDING` first) |
| `terrain.glsl` | DEM decoding, direction to UV, terrain height lookups, the observer frame (`enuBasis`, `observerPos`, `observerEffHeight`), the water map |
| `terrain_detail.glsl` | the procedural terrain detail and the shared terrain march (needs `common`, `cloud_params`, `terrain` first) |
| `atmosphere.glsl` | atmospheric extinction along any line of sight, and the Sun's transmittance |
| `depth.glsl` | the unified scene depth encoding |
| `clouds_v2.glsl` | the cloud density field and everything that samples it |
| `cloud_occlusion.glsl` | how a point source is hidden by the cloud composite |
| `cloud_lightning.glsl` | the lightning flash list |
| `beam_cloud_lights.glsl` | the beam light list, its tile cull and the beam lighting of cloud |
| `reflect_beam.glsl` | the Reflect Orbital beam record and buffer capacities |
| `point_style.glsl` | magnitude to point-spread mapping for satellites, stars and planets |
| `glare.glsl` | the glare sprite profile |
| `glint_list.glsl` | the mesh glint list |
| `darksky.glsl` | the exposure and contrast gate for faint diffuse sky features |
| `eclipse.glsl` | the Moon's shadow per point during a solar eclipse |
| `earth_env.glsl` | an analytic Earth and atmosphere along any ray (reflection fallback) |
| `sat_mesh_common.glsl` | the satellite mesh pipelines' descriptor set |

Headers whose GLSL cannot be shared because of different bindings or parameter types are duplicated by hand
and must be kept in step; the comments at each copy say so.

### De-duplication and code generation

Moving a pure *declaration* into a header (a uniform block, a struct, a constant) costs nothing and prevents
drift. Moving a *function body* into a shared header is not always free. When the shared version has to take a
loop count as a parameter where one copy had a compile-time constant, the loop cannot be unrolled at
compile time, and the driver does not reliably recover it. The atmosphere's `optDepth` is the standing example:
the cloud pass hard-codes its sample count while the sky pass reads a tunable one, and sharing the two copies
measured slower. Measure before and after unifying such code, at altitude and near the ground.

## Precision rules

These rules come from 32-bit floats on a planet 6.4 million metres in radius.

**Never store a distance in a half float.** An `RGBA16F` channel tops out at 65 504, which a near-horizon
cloud distance in metres exceeds; the overflow to infinity then silently disables whatever compares against
it. Distances go in `R32F` targets (the shared scene depth is half-resolution `R32_SFLOAT`, metres along the
view ray), or in kilometres where a half float is unavoidable (the cloud composite's alpha stores a signed
distance in km).

**Never form an absolute ECEF position in a float in a shader.** A float has about 0.5 m of resolution at the
Earth's radius. Positions are made relative to the observer on the CPU in double precision, and lattices for
procedural noise are anchored to the observer's sea-level point as an integer cell plus a fractional offset.
For the same reason a displacement, or anything multiplied by a large factor, is never read from a
hardware-filtered 8-bit texture: its sub-texel weights have only 8 bits.

**Every pass that produces or consumes a distance uses the same observer height.** `observerEffHeight()` in
`terrain.glsl` (or the detailed ground height the depth pass writes) defines where the eye is. One pass using a
CPU-computed height and another a GPU lookup is harmless while each only compares against itself, and wrong the
moment one produces a depth the other reads.

Time is handled the same way: absolute sim time is kept on the CPU as integer days plus seconds, and shaders
receive only short, already-reduced values. Every Earth-rotation (ECI to ECEF) conversion goes through one
function, `earthRotationAngle()`; see [Time and reference frames](../simulation/time-and-frames.md).

## Descriptor budgets

Vulkan guarantees 16 sampled images (and 16 samplers) per shader stage. `sat_sky.frag` uses all 16. A new
texture for the sky pass must merge into an existing one (as two maps into an array, or as extra channels),
or be read as a storage image with `imageLoad`, which has its own budget (4 guaranteed; the sky pass uses
some). The sky descriptor set has about 30 bindings in total; most are storage buffers. On macOS, MoltenVK
maps storage buffers, uniform buffers and vertex buffers into Metal's 31 buffer slots per stage, which is
where a storage-buffer limit is most likely to be hit.

## Register pressure in the cloud march

The volumetric cloud march (`cloud_v2_march.comp`) is large, and its speed is governed by code size and
register allocation more than by arithmetic:

- Every call site of the cloud field inlines the whole field, so the number of call sites sets the code size.
  The light march, sky probe and other secondary samples share one call.
- On NVIDIA the shader is compiled either to 128 registers (with a small spill) or to about 227 (no spill), and
  the second is 20 to 60% slower. Trivial edits flip it: an extra per-sample loop bound, a few debug counters,
  values cached in globals, an extra member in the field's result struct.

Before timing a variant, check its register count: the harness's `shaders reload` rebuilds the cloud pipelines
from the SPIR-V on disk and reports the driver's statistics (register count, binary size, spill memory) through
`VK_KHR_pipeline_executable_properties`. Compare variants interleaved in one running app; run-to-run noise is
5 to 10%. See [Profiling](profiling.md#ab-testing-a-shader-change).

## Validation layers

Debug builds enable `VK_LAYER_KHRONOS_validation` automatically. For a Release build, set the environment
variable when launching through the harness:

```bash
VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation python tools/harness/run.py my.satcmd
```

The messages land in the run folder's `app_stdout.txt`. Run with validation after touching render passes,
image uploads, barriers or anything near the photo target; it is how a mip-size mismatch (Vulkan mip sizes
round down) and a render-pass dependency mismatch were found.

## Where in the code

| File | What |
|---|---|
| `src/VulkanContext.h/.cpp` | helpers, `pickPhysicalDevice()`, `logDeviceLimits()`, render passes, validation setup |
| `src/simulations/SatelliteSim.h` | every push-constant struct and GPU record mirror, with their assertions |
| `shaders/include/` | the shared headers |
| `tools/check_cloud_params.py` | the `CloudParams` mirror check |
