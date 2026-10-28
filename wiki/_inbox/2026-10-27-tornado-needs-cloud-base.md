---
pages: [simulation/weather.md, rendering/clouds/storms.md]
code: [src/simulations/SatelliteSimWeather.cpp, src/simulations/SatelliteSim.h, src/simulations/SatelliteSimAmbience.cpp]
---
## Now
A tornado or waterspout is drawn and heard only under a real storm base: the GPU probe over its column
(cloud_v2_lightning.comp threads 2-5) must find cloud within 3.5 km of the ground with an optical depth of at least 3
above it. Its presence (`TornadoNow::present`, harness `state` -> `weather.tornado_list[].present`) eases in and out over
~0.25 s and multiplies the funnel, wall cloud, debris cloud and the roar. With no probe (clouds v2 off, no lightning
pass) no tornado is drawn. The catalog still decides when and where a storm has one (`tornadoOf`); the drawn cloud
decides whether it shows.

## History
2026-10-27, issue 20261027_212628 "Tornado formation under absolutely zero cloud cover": a supercell's tornado over the
Argentine plains (-30.29, -62.27) stood in clear sky: the probe found no cloud in its column (only the anvil above 6 km
and rain falling from it), and the funnel hung from the cumulonimbus type's base anyway. A coverage floor at the storm
stamp's rear-flank rain-free base was tried and changed nothing at that spot (reverted).
