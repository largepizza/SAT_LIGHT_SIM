---
pages: [using/graphics-settings.md, using/features.md]
code: [src/simulations/SatelliteSimUI.cpp]
---
## Now
Settings tabs were regrouped by what they control. **Sky** now holds everything about the natural sky, as folds: stars,
planets, the Moon, comets, meteors, refraction and mirages, the dark-sky gate, the Milky Way (and its zoomed star clouds),
seeing (twinkle, turbulence warp), scattering (with the orbit colour grade and the aerial-perspective volume), airglow and
zodiacal light, and the aurora. **Photometry** is how satellites and points of light are drawn (exposure, brightness, sky
dimming, point sources, bloom and glare, star trails, satellite models) and now also holds the Reflect-Orbital beams.
**Terrain** holds Night lights and Ocean as folds. The Atmosphere, Beams, Ocean and Night lights tabs are gone from the
strip; every setting keeps its key, and a saved window opens on the tab that now holds it.

Weather's four switches (storm objects, haboobs as cloud, wildfire burn scars, precipitation particles) sit in their
sections instead of above them, and the old "Rebuild experiments" section is gone: those sliders are filed with the
settings they act on (Clouds: quality, lighting, shape; Weather: cloud shadows and god rays; Terrain: surface and lighting).
The explanatory captions under the bulk-export and GEO-list headings are removed.

## History
2026-10-30 (issue 20261029_205641): categories redefined; Sky, Photometry and Terrain absorbed four tabs.
