# SAT LIGHT SIM

A real-time GPU simulation of the night sky with tens of thousands to millions of satellites in it, seen from
anywhere on or around the Earth. Each satellite's brightness comes from a physical reflectance model of its
real shape, checked against published observation campaigns. The sky, clouds, terrain, sea, cities and aurora
around them are rendered physically enough that the satellites are seen in the right context. It runs on
Vulkan on Windows, Linux and macOS.

![SAT LIGHT SIM](assets/title.png)

## Where to start

<div class="grid cards" markdown>

- **[Using the sim](using/index.md)**: install, controls, the features tour, graphics settings.
- **[Modding](modding/index.md)**: add constellations, satellite models and reflector targets with JSON.
- **[Simulation](simulation/index.md)**: the physics: time and frames, orbits, attitude, photometry,
  extinction, the Sun, Moon and planets.
- **[Accuracy](accuracy/index.md)**: how the brightness predictions are benchmarked against published
  observations, and how close they are.
- **[Rendering](rendering/index.md)**: how a frame is made: atmosphere, volumetric clouds, terrain, sea,
  cities, satellites as points and meshes.
- **[Sound](sound/index.md)**: location-aware ambience and the music that drives it.
- **[Development](development/index.md)**: building, code architecture, conventions, the automation
  harness and tools.
- **[History](history/index.md)**: the changelog, design notes and known issues. The only dated part of the wiki.

</div>

## About this wiki

Every page outside [History](history/index.md) describes the project **as it is now**. Pages are updated in
dedicated wiki passes from notes left during development. See [Working on this wiki](development/wiki.md)
and the [style guide](development/style-guide.md).
