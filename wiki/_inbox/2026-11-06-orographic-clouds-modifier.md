---
pages: [rendering/clouds/layers.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp]
---
## Now
Orographic clouds are a modifier of the clouds the wind carries over the relief, not an object. `cv2OroField` reads the
elevation map per sample (Earth-fixed): the streamlines' lift over a ridge and in its standing lee wave (crests over
the ridge and one wavelength 2 pi U / N downwind, a trough half a wavelength downwind), from the ~5 km relief's excess
over the ~13 km massif, saturating at ~1.5 km. The mid layer follows it: lifted over the crests, thinned in the troughs,
and where the lift passes the air's condensation lift (set by "lens moisture" `oro_moist`, a slow regional wave and the
mid layer's cover) its cloud condenses into a smooth, sharp-rimmed lens, thickest over the crest. Low cloud parts round
massifs: where the relief stands prominent over the ~20 km ground the bases ride on, low cloud below the relief + 900 m
gives way, so a deck lies in the valleys and the peaks stand in clear gaps. Flat land is unchanged. Settings > Clouds,
"Orographic clouds" (`clouds_v2.oro_amount`, 1; 0 = off), "Ridge wind (m/s, 0 = auto)", "Ridge wind from (deg, -1 = auto)",
key `oro_moist` (0.75). The cloud march computes the field once at its main sample (128 registers).

## History
Until 2026-11-06 lenticulars were a lens object (`cv2OroSigma`): a cap and one lee lens built from the DEM excess, with
1-4 stacked plates. From above (issue 20261105_174850, the eastern Himalaya from 13 km) it was a large smooth blob with
parallel stripes and chevrons from the plates, and lighting glitches; the reporter asked for terrain-shaped cloud instead.
