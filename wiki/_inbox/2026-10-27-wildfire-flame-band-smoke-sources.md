---
pages: [rendering/clouds/storms.md]
code: [src/simulations/SatelliteSimWildfire.cpp]
---
## Now
Close to a fire (its NEAREST front point within 40 km, faded 25-40 km) the active front is drawn as a band of flame:
emissive tongues ~9 px apart on screen, each at the level of detail its own range asks (64 base segments of the
front, nearest first, down to 2^16 points round, levels cross-faded), at most 256 a frame (the spacing widens when
that binds). A tongue the eye resolves keeps its shape (0.66 x the flame height tall) and gets two narrow licks that
sway and flicker; an unresolved one becomes a ribbon segment of front, at least a pixel tall, carrying the flame light
of the stretch it stands for. Where the front runs along the view the tongues' radiance is divided by their overlap
on screen (flames are near opaque). The points keep half their flame light there, for their glare and bloom.
`clouds_v2.wildfire_flame_band` (no UI, default 1; 0 = the old sparse tongues). Cost ~+0.7-1.4 ms GPU 500 m from a front.

Smoke streaks: the low layer and the pall leave from the middle-rank section of the front (ranks are labelled by
bearing, so the same stretch of front at every emission), and the main streak is that rank too. Trail segments are
planned in two passes: every segment in view gets one Gaussian first, segments off screen up to a quarter of what is
left, and the pieces (at most 32 a segment) are shared by a water fill over what is on screen.
`clouds_v2.wildfire_smoke_alloc` (no UI, default 1; 0 = the old share).

## History
2026-10-27, issues 20261027_212337 ("Flame particles") and 20261027_212509 ("Smoke trails"), a three-week-old fire with
a 126-km front in Thailand: the flames, their share of the points and the glow were gated on the range to the ignition
point (125 km while the eye was 44 m over the front: no flames at all), and the tongues stood >= 40 px apart at their
true few-metre size. The low layer left from the strongest section, which flipped between emissions minutes apart on
such a front: its knots jumped ~25 km, drawn as a star of needles ~20 km long, new every frame at 1 h/s. The heat
centroid was tried as the source and dropped (on a curved front it lies in the burnt area). The old piece share went
to zero whenever the fires in reach had more segments than the cap.
