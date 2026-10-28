---
pages: [rendering/atmosphere-and-sky.md, using/settings.md]
code: [shaders/sat_sky.frag, tools/make_milky_way.py, src/simulations/SatelliteSim.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
The Milky Way is the ESO/Brunier starless panorama (4096 x 2048, 0.088 deg a texel, an 8-bit JPEG) drawn in the sky
pass, now as an RGBA16F texture: RGB the JPEG decoded to linear, A a denoised glow baked by `tools/make_milky_way.py`
(`assets/textures/milky_way_glow.r16f`). At the default field of view the band is the photograph's texels (the faint,
noise-dominated sky takes the denoised luminance). When a pixel is under ~0.6 texel (vertical FOV below ~30 deg at
1009 px) it is drawn from: the smooth glow (A through a C1 fetch, colour from a blurred mip), sub-texel dust structure
(value noise whose contrast is the local dust depth, plus local contrast about the 8-texel mean), and **star clouds**:
a share of the light carried by unresolved points on an iso-latitude lattice in galactic coordinates (4 input pixels
a cell, two levels cross-faded, 90% of cells, a power-law brightness, a stellar tint, a 0.9-pixel Gaussian), whose mean
is the glow, so the light is conserved (in-app, zoom on vs off: within 1-2.5% on the ground). The faint sky between
the star clouds and the magenta emission nebulae (Carina, North America) keep a smooth glow. The Andromeda galaxy,
missing from the panorama, is drawn analytically (bulge + inclined disk, its light scaled from the panorama's own LMC
by the real magnitudes). Under the air the band takes a rod-vision blue-grey; above it a brighter, contrastier look.
Photometry tab, `photometry.*`: "Milky Way gain (ground)" `milky_way_gain_ground` 1, "... (space)"
`milky_way_gain_space` 2.5, "Milky Way contrast (space)" `milky_way_contrast_space` 1.25, "Milky Way night vision"
`milky_way_night_vision` 0.5, "Milky Way star clouds" `milky_way_star_clouds` 0.7, "Milky Way zoom detail"
`milky_way_zoom_detail` 0.6; `milky_way_zoom` (no UI, true) is the A/B. Not in Planetarium / Potato (no Milky Way) or
in probes and mirrors (the env renders keep the single fetch; Andromeda is drawn there too).

## History
Until 2026-10-28 the panorama was sampled with one bilinear fetch at every zoom: below ~25 deg its texels showed as
diamonds and its 8-bit / JPEG noise as grain, lifted further from orbit. A bilateral denoise was tried first and
posterised; the bake is a median (noisy texels only) + a 1.3-texel Gaussian weighted by the 8-bit step. Points at
0.6 px shimmered under the sky TAA in a pan (registered frame-to-frame p99.9 7.8 levels; 0.9 px: 4.9). Cost (RTX 3070
Ti, 1600x900): sky pass +0.0 at FOV 90, +0.25 ms at FOV 8 (the whole frame zoomed), +0.07 from orbit; sky TAA 168
registers, binary 937 -> 950 KB.
