# Heavy-rain lightning: rain only, default 6

- Change: the heavy-rain lightning zones (`cv2RainZoneProbe`, cloud_v2_lightning.comp) skip any bin whose air 500 m over
  the ground is under 2.5 C: the Weather window radar's own rule for its snow / mixed colours. No strikes on snow.
- `clouds_v2.lightning_rain_rate` default 4.5 -> 6.0 to keep about twice the storms-alone flash count (x 1.98).
- Numbers: Andes 400-km radar 2127 -> 1131 flashes at 4.5 (snow side bare); globe x 1.74 at 4.5, x 1.98 at 6;
  lightning pass at 3780 km over the Andes +0.27 ms vs rate 0. Details: docs/subsystems/weather.md "Lattice 2".
- Pages: the lightning / weather page's heavy-rain section.
