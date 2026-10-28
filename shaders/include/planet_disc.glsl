// Resolved planet discs (2026-10-27, planet_disc.vert / .frag; CPU side SatelliteSimPlanets.cpp, GpuPlanetDiscUBO in
// SatelliteSim.h — keep the two in step: std140, vec4s only). A planet whose true angular radius passes ~1 px is drawn
// here instead of by the star pipeline: a quad round it, every pixel ray-traced against the planet's oblate spheroid
// (and Saturn's ring plane) in a LOCAL frame: x = the sky's right, y = the sky's up, z = toward the viewer, lengths in
// equatorial radii. Additive over the frame like the stars, at kDepthFar (terrain, opaque cloud and the Moon hide it).
#ifndef PLANET_DISC_GLSL
#define PLANET_DISC_GLSL

struct PlanetDisc {
    vec4 c;     // xyz = the planet's apparent direction (ENU, refracted), w = its equatorial angular radius (rad)
    vec4 bx;    // xyz = the local frame's x (sky right, ENU), w = the quad's half extent (equatorial radii)
    vec4 by;    // xyz = the local frame's y (sky up, ENU), w = one pixel in equatorial radii
    vec4 pole;  // xyz = the north pole (local), w = polar / equatorial radius
    vec4 x0;    // xyz = longitude 0 (local; Jupiter: System II), w = the map layer (-1 = none)
    vec4 sun;   // xyz = toward the Sun (local), w = the law (0 Lommel-Seeliger, 1 Minnaert)
    vec4 tint;  // rgb = the albedo colour (x the map), w = Minnaert k
    vec4 lin;   // rgb = linear light at the tonemap's input per unit law / <law> (exposure, extinction, hand-off in it),
                //  w = the disc's share (0 = all point, 1 = all disc)
    vec4 pt;    // x = the point's peak x (1 - share), y = its sigma (px), z = the suppression (day, Moon, light pollution), w = map u offset
    vec4 ring;  // x = the rings' outer radius (0 = none), y = linear light per unit I/F (rings, Venus's rim), z = the rim's I/F, w = unused
    vec4 misc;  // x = seeing blur (px), yz = the turbulence's jitter (equatorial radii), w = the body (PlanetId)
    vec4 pad;
};

layout(std140, set = 0, binding = 0) uniform PlanetDiscUBO {
    uvec4 head;        // x = count, y = Jupiter's disc index (or 99: its moons' shadows), z = the rings' map layer
    vec4  tone;        // x = AgX on, y = saturation, z = contrast, w = highlight roll-off (the old curve)
    vec4  moons[4];    // Galilean moons in Jupiter's local frame (equatorial radii), w = their radius
    PlanetDisc disc[6];
} pd;

#endif
