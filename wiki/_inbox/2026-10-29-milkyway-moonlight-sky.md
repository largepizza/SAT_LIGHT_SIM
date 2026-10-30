---
pages: [rendering/atmosphere-and-sky.md, rendering/moon-and-eclipses.md]
code: [shaders/sat_sky.frag]
---
## Now
Moonlight dims the Milky Way, its star clouds, the zodiacal light and the sea's Milky Way reflection as SKY BRIGHTNESS, not a flat scalar. `moonSkyBgMag` adds the Moon's light to the dark-sky gate's background in linear light: an 18 mag/arcsec^2 sky at the full Moon high up (the model the stars' naked-eye limit uses), scaled by the Moon's phase-law light, (sin elevation / 0.5)^2 and the air's share. The per-texel gate (`darkSkyVis`) then removes the faint arms first and leaves the core. A 23%-of-full Moon cuts the band to a faint core, near full it is gone; the new Moon is unchanged.

## History
Issue 20261029_182916. The old term was a flat (1 - 0.95 k) multiplier (zodiacal 0.9): a Moon at 23% of full light (sky ~16x natural) dimmed the band by only ~20%, and the sea reflection had no moon dimming. SKY_LITE unchanged (230232 B); full / TAA SPVs +1.2 KB. Measured by interleaved SPV reloads at the report's place, Apr 2037: band-region mean luminance old -> new 16.4 -> 11.3 (Moon 0.23), 19.5 -> 15.8 (0.46), 25.7 -> 24.2 (0.85; moonlit sea dominates the crop).
