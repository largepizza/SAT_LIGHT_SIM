# SAT LIGHT SIM v1.2.0

The biggest update yet. The constellations now have a life cycle: rockets launch them, trains of new satellites
climb to their slots, crews and cargo fly to the stations, old satellites re-enter, and from the late 2010s to
2036 the sky fills up launch by launch. A scripted solar superstorm in July 2037 can set off a Kessler cascade.
Around them: a new weather system with storms, hurricanes, tornadoes, haboobs and wildfires; a night sky with the
real stars, resolved planets, comets, meteor showers and a supernova; a living Earth from the ground to orbit;
satellites you can fly up to and inspect as real 3D models; and a sim clock that runs on real UTC. Accepted photometric error is listed in
`data/benchmarks/KNOWN_RESIDUALS.md`; the full reference is the project wiki (`wiki/` in the source).

## Highlights

- **Launches.** Rockets keep the constellations flying: Falcon 9, Starship, New Glenn, Vulcan, Ariane 6, the
  Long March family, H3 and more, as 3D models on their real pads, flown in full from lift-off through staging,
  deployment, the satellites' climb to their slots and the boosters' landings, with physically lit exhaust,
  billowing staging clouds and launch-made noctilucent clouds. Every launch has milestones you can skip between.
- **Crew and cargo.** Crews rotate to every station twice a year (Crew Dragon, Shenzhou, Soyuz, Gaganyaan) and
  cargo ships resupply them, docking included.
- **The deployment era.** From 2019 to 2036 the sky fills up launch by launch: go back in time and only the
  satellites launched by then are up.
- **The Sun's Fury.** A scripted Carrington-class solar storm on 15 July 2037 brings aurora to the mid-latitudes,
  kills a share of the satellites, takes nineteen in twenty Reflect Orbital mirrors out of control, and can set off a
  Kessler cascade of collisions and debris. The default cascade ships precomputed, so you can jump straight to it.
- **Clouds, rebuilt.** One volumetric cloud field from the ground to orbit, from cumulus with billowing sides to
  stratus decks, cirrus and lenticular clouds on the mountains, lit with physical multiple scattering and the real
  droplet phase function, by the Sun, the Moon, city light, lightning, aurora and the reflector beams.
- **Weather.** Storms as objects with a life (cells, supercells, squall lines), hurricanes with a real eye and
  eyewall that rotate, tornadoes and waterspouts, haboobs, wildfires with smoke; rain, snow, sleet and diamond dust;
  lightning and thunder that lights the clouds and the ground; rainbows and ray-traced ice halos; a Weather window.
- **The night sky.** The real stars (about 62,000), resolved planets (Saturn's rings, Jupiter's moons), comets,
  meteor showers at their real rates, the Moon's real surface, Betelgeuse going supernova on 25 December 2037, a
  physical light-pollution model, refraction, mirages and seeing; a Sky events window with one Watch per event.
- **Real time, real eclipses.** The sim clock is real UTC: eclipses happen when and where they really do, from the
  total lunar eclipse of 31 January 2037 to the total solar eclipse over Australia on 13 July 2037, with eclipse
  glasses.
- **A living Earth.** True 3D terrain with erosion detail, snow and rock; a sea with seamless waves, surf and real
  reflections; procedural cities with streets, lamps and regional styles; farmland and solar parks.
- **Satellites as real 3D models**, with physically computed brightness validated against observation campaigns;
  398 real geostationary satellites by name; SpaceX Starmind orbital data centers.
- **Reflect Orbital beams light the world:** every beam lights the ground, the sea, the clouds and the air with its
  own cloud shadows; Missions > Reflectors lists every lit site with a Watch.
- **Controls and UI.** FPS mouse look (Tab), fly where you look, roll in space; a playable opening; Missions, Sky
  events and Weather windows; Settings search; first-run hints; UI themes.
- **Capture.** Video recording to MP4 with sound, cinematics with a camera-path editor, HQ photos, bookmarks with
  ten defaults, and in-app issue reports.
- **Sound.** Location-aware ambient sound in the music's key, lightning cracks and booms, launches that sound like
  launches, and new music.
- **Performance.** Two frames in flight, temporal anti-aliasing and upscaling, an aerial-perspective volume, a
  rebuilt cloud march that is much faster from orbit and in storms, and the light Potato and Planetarium presets.
- **Platforms.** Windows, Linux and macOS (one universal arm64 + x86_64 build) from the same release workflow.

## Downloads

| Platform | File |
|---|---|
| Windows (x64) | `SAT_LIGHT_SIM_v1.2.0_Windows.zip` |
| Linux (x64) | `SAT_LIGHT_SIM_v1.2.0_Linux.tar.gz` |
| macOS 11+ (universal: Apple Silicon and Intel) | `SAT_LIGHT_SIM_v1.2.0_macOS_universal.tar.gz` |

Unpack and run the executable inside (on macOS, the `.command` launcher). Requires a Vulkan 1.2 GPU (macOS through MoltenVK) with 2 GB of video memory
or more; the graphics presets in Settings > Display (Potato, Planetarium, Low ... Ultra) cover older hardware.
The defaults have changed: see "Changed defaults and behaviour" in the changelog.

## Everything else

The full list of changes, fixes and performance work (about 1,100 lines) is in
[CHANGELOG.md](https://github.com/largepizza/SAT_LIGHT_SIM/blob/v1.2.0/CHANGELOG.md).
