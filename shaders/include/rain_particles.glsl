#ifndef SATLIGHTSIM_RAIN_PARTICLES_GLSL
#define SATLIGHTSIM_RAIN_PARTICLES_GLSL
// Rain particles (rain_particles.vert / .frag): the push constants (== RainDrawPC in SatelliteSim.h, 128 bytes).
// Set 0: 0 CloudParams, 1 CloudV2Params, 2 the flash buffer (rain map + the light at the eye), 3 the scene depth.
layout(push_constant) uniform RainPC {
    mat4  skyView;
    float fovYRad, aspect, screenW, screenH;
    vec4  eyeVel;       // xyz the eye's velocity (ENU, m/s; capped), w the shutter time (s)
    vec4  params;       // x drops in level 0, y levels, z visibility gain ("Drops at the eye"), w the sky's exposure
    vec4  obsECEFDir;   // xyz the observer's direction (ECEF), w mode bits: 1 diamond dust, 2 manual depth test
} pc;

const float kRpL0 = 4.0;   // level 0's box (m); level k's is 2^k x it. Divides the rain frame's 1024-m wrap.
#endif
