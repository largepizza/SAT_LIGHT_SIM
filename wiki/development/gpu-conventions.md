# GPU conventions

The rules that keep the Vulkan and GLSL code correct: the `VulkanContext` helpers, barrier habits, the
128-byte push-constant limit, the hand-kept mirrors between C++ structs and GLSL blocks, the shared shader
headers, the precision rules for distances and positions, descriptor budgets, and how code structure affects
shader speed. Most of these rules exist because breaking them fails silently: everything compiles and the
image is wrong.

## `VulkanContext`

`VulkanContext` (`src/VulkanContext.h`) holds the core objects simulations read (`device`, `physicalDevice`,
`graphicsQueue`, `commandPool`, `commandBuffer` (the current frame slot's), `swapExtent`, `swapFormat`,
`swapImages`, `renderPass`, `renderPassLoad`, `renderPassBoot`, `framebuffers`, `depthFormat`, `pipelineCache`)
and a few helpers; the helpers for memory the CPU and GPU share are listed under [Frames in flight](#frames-in-flight):

| Helper | What it does |
|---|---|
| `loadShader(path)` | a `VkShaderModule` from a `.spv` file |
| `createBuffer(size, usage, props, buf, mem)` | buffer plus bound memory |
| `createImage(w, h, fmt, usage, img, mem, mips = 1, depth = 1)` | 2D or 3D image plus bound memory |
| `findMemoryType(filter, props)` | a memory type index |
| `generateMipmaps(cmd, img, fmt, w, h, mips)` | blits mips 1..n-1 from mip 0 (in `TRANSFER_DST_OPTIMAL` on entry); all mips end in `SHADER_READ_ONLY_OPTIMAL` |
| `bestBlitFilter(fmt)` | `LINEAR` if the format supports linear blit filtering, else `NEAREST` |
| `beginOneTimeCommands()` / `endOneTimeCommands(cmd)` | a one-shot command buffer, submitted and waited on (init-time uploads; at runtime it stalls the frame, and the harness counts it) |
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

## Frames in flight

The app keeps **two frames in flight**: the CPU records frame *f* while the GPU still runs frame *f* - 1, so a
frame costs max(CPU, GPU) rather than CPU + GPU. Each frame slot (`kMaxFramesInFlight` = 2) has its own command
buffer, upload command buffer, fence and acquire semaphore. `App::drawFrame()` starts with
`ctx.beginFrameSlot()`, which waits only for the previous frame that used this slot (two frames back) and points
`commandBuffer`, `fenceFrame`, every slotted buffer and every readback pointer at the slot. The environment
variable `SATLIGHTSIM_FRAMES_IN_FLIGHT=1` selects the serial single-frame mode at launch, so one build can A/B
both.

**The GPU still runs frames one after another.** Every frame's GPU work opens with a full memory barrier
(`ALL_COMMANDS` to `ALL_COMMANDS`, every write made visible to every read): the first command of the slot's
upload command buffer, and `beginFrameGpu()` at the top of the main one. Device-local resources (cloud and TAA
histories, the depth targets, every compute output) therefore need nothing: frame *f*'s GPU work sees the whole
result of frame *f* - 1, exactly as with one frame. **Only memory the CPU touches needs care**, and it goes
through these helpers:

| Helper | For | How |
|---|---|---|
| `mirrorUpload(dst, mem, size, whole, name)` | a buffer the CPU rewrites (the `CloudParams` and clouds uniform blocks, the satellite type header and tables, the storm buffer, mesh instances, ...) | returns a pointer to a CPU **shadow**; code writes it as it would the mapped memory. `recordFrameUploads()` copies the shadow through a per-slot ring into the device buffer at the start of the frame's GPU work: the whole buffer when `whole`, else only the ranges marked with `mirrorDirty(base, off, len)` (merged; `mirrorDirtyAll()`). With one frame in flight it returns the mapped memory itself |
| `uploadPatch(dst, off, data, len)`, `uploadFill(...)` | a few scattered bytes into a GPU buffer (a probe request, a counter reset, a cleared level) | applied at the start of this frame's GPU work, in both modes |
| `slotted(&buf, &mem, &ptr, size, usage, props, name)` | staging the CPU fills and this frame copies, the UI's vertex and index buffers, host readback targets | one copy per slot; the bound variables are swapped at `beginFrameSlot()` |
| `readbackCreate(size, &ptr, name)` | a GPU result the CPU reads (the glow histogram, the beam list, the clouds' flash buffer head) | one host-cached copy per slot, filled by a copy the frame records (`recordFrameReadbacks()`); after `beginFrameSlot()` the pointer holds the data of `completedFrame()` |
| `stagingAlloc(bytes)` | staging for an image upload recorded into this frame (the radar, the Kessler tracker's globe, the cyclone track map) | slot-owned host memory, reused when the slot comes round |
| `deferDestroy(buf, mem)` | a buffer a frame in flight may still use | destroyed once its last user has finished |

!!! warning "Invariant"
    A CPU write into a mirrored buffer that is not `whole` must be marked with `mirrorDirty()`, or the GPU never
    sees it. A descriptor set must never bind a slotted buffer (the binding would change every frame): slotted
    buffers are copy sources and destinations and vertex input only.

**Readbacks are two frames old.** Picking, the peak magnitude, the exposure meter, the beam list and the other
per-frame readbacks hold the result of the frame that last used this slot. State a readback must be paired with
(the observer basis the beam list was written in, the lightning list's observer direction, the tornado probes'
ids, the glint list's validity, the exposure meter's pending flag) is kept per slot too: stored once, it would
pair the result of frame *f* - 2 with the state of frame *f* - 1. One-shot readbacks (the weather radar, the
harness probe, the photometric check, a screenshot copy) record the frame index they were requested in and read
once `ctx.completedFrame()` reaches it; `finalizeScreenshotCopy(true)` drains instead, for the recorders that need
exactly one rendered frame per recorded frame.

**Descriptor writes and recreation drain.** A descriptor set the previous frame bound must not be rewritten while
that frame runs, so every `vkUpdateDescriptorSets` outside `init()` goes through `frameSafeUpdateDescriptorSets()`,
which drains the frames in flight first (and counts the write). Resource recreation at runtime (the render-scale
targets, the low-resolution sky, the deferred full-sky pipelines, the swapchain) calls `drainFrames()`. A steady
frame does neither: the harness's `perf` reports a `frames_in_flight` block (about 400 KB uploaded a frame, 0
drains, 0 descriptor writes, 0 one-time submits). **A drain or a descriptor write every frame is a bug**: it
silently restores the single frame. The timestamp query pool holds `kTimestampCount` slots per frame slot, and
each frame writes and resolves its own range.

## Synchronisation

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
| `aurora_oval.glsl` | the auroral oval's geometry (a Sun-fixed ring whose edges follow the activity index) |
| `cv2_sunlut.glsl` | reading the clouds' Sun-colour, sky and multiple-scattering table |
| `cv2_cdc.glsl` | the cloud density clipmap (the occupancy the cloud march skips empty space with) |
| `cv2_storms.glsl` | the storm objects' towers and anvil shields and their spatial index |
| `cv2_optics.glsl` | bows, halos and phase lobes, shared by the cloud march and the rain particles |
| `sky_atmos.glsl` | the sky's single-scattering helpers, shared by the per-pixel atmosphere loop and the aerial-perspective volume |
| `sky_ap.glsl` | the aerial-perspective volume's layout and lookup |
| `smoke.glsl` | wildfire smoke's spectral extinction and scattering |
| `rain_particles.glsl`, `lightning_draw.glsl` | the rain particles' and the lightning draw's push constants and shared code |
| `glare_mesh_vert.glsl` | the body of the mesh-glint glare vertex shaders |

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
or be read as a storage image with `imageLoad`, which has its own budget (4 guaranteed; the sky pass and the
cloud march each use all 4). The sky descriptor set has 34 bindings in total; most are storage buffers. A new
small per-frame list for an existing pass can often ride at the end of a buffer the pass already binds (the
launch engine lights travel at the tail of the dust-cloud buffer, for example) rather than take a binding.
`sat_orbit.comp`'s set binds 12 storage buffers, above the guaranteed 4, and the device check asks for that. On macOS, MoltenVK
maps storage buffers, uniform buffers and vertex buffers into Metal's 31 buffer slots per stage, which is
where a storage-buffer limit is most likely to be hit.

## Register pressure in the cloud march

The volumetric cloud march (`cloud_v2_march.comp`) is large, and its speed is governed by how the GPU schedules
it and by register allocation more than by arithmetic.

**Lockstep.** A GPU runs a group of 32 rays together, so every iteration of a loop costs what its most expensive
ray costs. In the march a lit cloud sample costs about ten evaluations of the cloud field (the view sample, seven
light-march steps, a far sample and a sky probe) and an empty step one. With density and lighting in one loop,
rays in empty air wait out their neighbours' light marches, and the time is the longest ray's step count times a
lit sample's cost. The march therefore works in **two loops per batch of 32 samples**: the first walks the ray on
the density field alone and keeps the samples that hold cloud, the second lights only those. The consequences
run through every optimisation of this shader:

- Ending an opaque ray early gains little: its group runs as long as its longest ray.
- Skipping work on alternate samples gains nothing unless every ray in the group skips together.
- Per-ray caches that one ray refreshes stall the whole group; a precomputed table (the clouds' Sun-colour and
  zenith-sky table, `cv2_sunlut.glsl`) is cheaper than a cache recomputed as rays cross levels.
- In the lighting loop, variables named like the density loop's are locals that shadow them: anything the
  lighting needs from the density walk must be stored in the batch's buffers.

**Code size.** Every call site of the cloud field inlines the whole field, so the number of call sites sets the
code size. The light march, sky probe and other secondary samples share one call. Marking the field functions
`DontInline` in the SPIR-V is honoured by the driver and runs several times slower.

Code that never runs still costs: the instructions a step executes are only part of it, and the code merely
present in the inlined field slows every view. A uniform branch does not help; a **specialization constant**
does, because the driver then compiles a variant without the code. The haboob dust wall is the standing example:
the march is built twice (specialization constant 3, `kCv2DustOn` in `clouds_v2.glsl`), and the host binds the
variant without the haboob code whenever the dust grid holds no wall (`dustGpuOn`). A feature that is absent from
most views and lives inside the field should follow the same pattern.

When measuring what a piece of the field costs, run its loop twice **inside** its function (an idempotent repeat,
such as a maximum taken twice) rather than calling the function twice: a second inlined copy of a large function
costs far more than its share (the extra code size is part of the cost), while a duplicate in a branch never taken
costs nothing.

**The register cliff.** On NVIDIA the shader is compiled either to 128 registers (with a small spill) or to roughly
220 to 230 (no spill), and the second is 20 to 60% slower. Trivial edits flip it: an extra per-sample loop bound, a
few debug counters, values cached in globals, an extra member in the field's result struct, a value carried out of
a loop. Edits inside the cloud field's functions flip it far more readily than edits in the march's own file.
Reordering or folding code rarely brings it back; **removing code does**, so dead branches and relics of earlier
designs are worth deleting from this shader even when they never run.

Before timing a variant, check its register count: the harness's `shaders reload` rebuilds the cloud pipelines
from the SPIR-V on disk and reports the driver's statistics (register count, binary size, spill memory) through
`VK_KHR_pipeline_executable_properties`. Compare variants interleaved in one running app; run-to-run noise is
5 to 10%. See [Profiling](profiling.md#ab-testing-a-shader-change).

## CPU access to host-visible memory

Host-visible memory is not ordinary RAM, and two access patterns are slow enough to show in a frame:

- **Never loop over mapped GPU memory on the CPU; copy it out first.** Memory that is `HOST_VISIBLE` and
  `HOST_COHERENT` without `HOST_CACHED` is uncached, so each scalar read is a bus transaction (tens of
  nanoseconds), and device-local host-visible memory is read across PCIe (around a microsecond a read). A loop of
  a few thousand byte or float reads (an exposure meter, a rain map) costs most of a millisecond. `memcpy` the
  block into local memory and loop over that; readbacks made with `readbackCreate()` are host-cached already.
- **Write device-local host-visible memory in one run.** It is write-combined: scattered stores are slow, while a
  sequential copy is fast. Large per-frame structures written slot by slot (the storm buffer's tower, shield and
  envelope indices) are filled into host memory first and copied in one pass.

The clouds' flash buffer (the Sun transmittance volume, the rain map, the god-ray froxels) lives in device-local,
host-visible memory when the device offers it, because every lit cloud sample and the sky pass's god rays read
it and reads of system memory cross PCIe. `SATLIGHTSIM_FLASH_HOSTMEM=1` forces it into system memory.

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
| `src/VulkanContext.h/.cpp` | helpers, `pickPhysicalDevice()`, `logDeviceLimits()`, render passes, validation setup; the frame slots (`beginFrameSlot()`, `beginFrameGpu()`, `recordFrameUploads()`, `drainFrames()`, `completedFrame()`), `mirrorUpload()`, `slotted()`, `readbackCreate()`, `frameSafeUpdateDescriptorSets()` |
| `src/simulations/SatelliteSim.cpp` | `recordFrameReadbacks()`: the per-slot readback copies each frame records |
| `src/simulations/SatelliteSim.h` | every push-constant struct and GPU record mirror, with their assertions |
| `shaders/include/` | the shared headers |
| `tools/check_cloud_params.py` | the `CloudParams` mirror check |
