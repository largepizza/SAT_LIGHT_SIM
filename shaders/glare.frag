#version 450
// ── glare.frag — a bright satellite's glare, drawn at full resolution (see glare.vert) ────────
// Built like the sun's corona in sat_sky.frag's lensFlare() (f0), so a glint and the sun read as
// the same optics: a Lorentzian core, 1 / (1 + 1.1·r) with r in pixels, whose angular profile is
// modulated ~20x by a smooth noise of the angle — thin bright rays with dark gaps between them,
// fading as 1/r. On top, for the brightest glints, a thin horizontal (anamorphic) streak. One ray
// pattern for every source (a camera aperture's), additive over the frame after the broad bloom.

layout(location = 0) in vec3  gColor;
layout(location = 1) in float gStrength;
layout(location = 2) in float gRadiusPx;
layout(location = 3) in float gSeed;
layout(location = 4) flat in vec3 gSrc; // xy = the source's screen uv, z = its range (m)

layout(set = 0, binding = 5) uniform sampler2D cloudTargetA;  // a = tCloudOcclude (>= 0: >= 90% opaque)
layout(set = 0, binding = 6) uniform sampler2D cloudTargetB;  // rgb = cloud transmittance
layout(set = 0, binding = 7) uniform sampler2D sceneDepthTex; // terrain/ocean distance

layout(location = 0) out vec4 outColor;

float hash1(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }
// Smooth 1D value noise.
float vnoise(float x)
{
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash1(i), hash1(i + 1.0), f);
}

void main()
{
    vec2  d = (gl_PointCoord - 0.5) * (2.0 * gRadiusPx); // pixels from the source
    float r = length(d);
    if (r > gRadiusPx || gStrength <= 0.0) discard;
    float ang = atan(d.y, d.x);

    // Occlusion at the source, as flare_source.frag: opaque cloud or terrain NEARER than it hide it,
    // the cloud transmittance dims it (the same value for every pixel of the sprite).
    vec4  cA = textureLod(cloudTargetA, gSrc.xy, 0.0);
    vec4  cB = textureLod(cloudTargetB, gSrc.xy, 0.0);
    float vis = (cA.a >= 0.0 && cA.a < gSrc.z) ? 0.0 : clamp(dot(cB.rgb, vec3(1.0 / 3.0)), 0.0, 1.0);
    if (textureLod(sceneDepthTex, gSrc.xy, 0.0).r < gSrc.z) vis = 0.0;
    if (vis <= 0.001) discard;

    // lensFlare()'s angular noise: a smooth, non-monotonic function of the angle sampling a noise
    // curve, then sin(noise·16): many rays of uneven width and brightness.
    float seedA = gSeed * 6.2831853, seedB = gSeed * 3.7;
    float nSeed = sin(ang * 4.0 + seedA) * 4.0 - cos(ang * 3.0 + seedB);
    float angNoise = vnoise(nSeed * 19.0 + 57.0);
    float rays = max(sin(angNoise * 16.0), 0.0);

    float f0   = 1.0 / (1.0 + 1.1 * r);                      // the corona's Lorentzian
    float core = exp(-r * r / (2.0 * 1.6 * 1.6));            // the glint itself, a few pixels
    float edge = 1.0 - smoothstep(0.55 * gRadiusPx, gRadiusPx, r);
    float corona = (f0 * (0.6 + 20.8 * rays) * 0.1 + core * 0.9) * edge;

    // Anamorphic streak: thin and horizontal, only once the glint is well past the threshold.
    float streakOn = smoothstep(0.8, 2.0, gStrength);
    float streak = exp(-(d.y * d.y) / (2.0 * 0.9 * 0.9)) * exp(-abs(d.x) / (0.35 * gRadiusPx)) * edge * streakOn;

    // The glint's own tint for the corona (desaturated toward white at the centre, as a bright
    // source reads), a faint blue for the streak.
    vec3 tint = mix(gColor, vec3(1.0), 0.5 * core + 0.3);
    vec3 col = tint * corona + vec3(0.55, 0.72, 1.0) * streak * 0.35;
    outColor = vec4(col * (gStrength * vis), 1.0);
}
