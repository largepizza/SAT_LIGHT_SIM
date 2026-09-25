// ── glare.glsl — the sharp glare of a bright point source (glare.frag, glare_mesh.frag) ─────────
// Drawn at full resolution over the broad bloom (flare_blur.comp). One pattern for every source, as a
// camera's aperture gives every point light the same spikes:
//   - the glint itself: a Gaussian a pixel or two across;
//   - a small, tight halo, brighter towards the centre;
//   - kSpikes thin spikes of their own length and brightness (a fixed hash per spike), each a line of
//     constant pixel width (so it stays sharp and anti-aliased at any radius; the first cut used the
//     sun corona's angular noise, whose hundreds of rays aliased into a cluttered centre), fading out
//     along its length as (1 − r/length)^falloff — brighter centre, dimmer tips;
//   - all of it windowed smoothly to zero at the sprite's edge, so overlapping glares add up to a soft
//     sum, never a disc with an edge.
// The 2026-09-24 first cut also had a blue anamorphic streak; the sun's flare has none, so it went.

layout(push_constant) uniform GlarePC {
    mat4  skyView;
    float fovYRad;
    float aspect;
    float gain;         // glare brightness (Settings: "Glare gain") x the night eye-adaptation
    float sizePx;       // sprite radius per unit of the bloom's log response
    vec2  screenSizePx; // the main view
    float maxPointSize; // device limit
    float threshold;    // the log response a glint needs before it glares
    float falloff;      // spike brightness along its length: (1 - r/length)^falloff
    float spikes;       // number of spikes
    float pad0, pad1;
} gpc;

float glareHash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// d: pixels from the source; radius: the sprite's radius in pixels. Returns the corona's weight (x)
// and the core's (y), both already windowed.
vec2 glareProfile(vec2 d, float radius)
{
    float r = length(d);
    float x = r / max(radius, 1.0);
    if (x >= 1.0) return vec2(0.0);
    float win = 1.0 - x * x;
    win *= win;

    float core = exp(-r * r / (2.0 * 1.5 * 1.5));
    float halo = exp(-r / (0.05 * radius + 1.5));

    const float TWO_PI = 6.2831853;
    float n = max(floor(gpc.spikes + 0.5), 1.0);
    float t = (atan(d.y, d.x) + 3.14159265) / TWO_PI * n; // [0, n): spike cells around the circle
    float k0 = floor(t);
    float spikes = 0.0;
    for (int j = -1; j <= 1; ++j) {
        float k = k0 + float(j);
        float kk = mod(k, n);                                     // wraps across the -pi seam
        float centre = k + 0.5 + 0.35 * (glareHash(kk * 7.13 + 1.0) - 0.5);
        float dTheta = (t - centre) * (TWO_PI / n);               // radians off this spike
        float p = r * sin(clamp(dTheta, -1.5, 1.5));              // pixels off its line
        float len = 0.45 + 0.55 * glareHash(kk * 3.71 + 2.0);     // its length, of the radius
        float bright = 0.35 + 0.65 * glareHash(kk * 5.19 + 3.0);
        float along = max(1.0 - x / len, 0.0);
        spikes += bright * exp(-p * p / (2.0 * 0.75 * 0.75)) * pow(along, gpc.falloff);
    }
    return vec2((0.25 * halo + 0.6 * spikes) * win, core * win);
}
