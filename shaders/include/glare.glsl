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
    float nearGain;     // glareNearGain: the size multiplier at zero range (1 = off)
    float nearRangeM;   // glareNearRangeKm in metres: at/beyond it the size is exactly as tuned
    // Clusters (2026-10-20, GlareCluster below): the screen grid every glaring source is summed into
    // (glare_cluster.comp), and how strongly a crowded source gives up its spikes (0 = off).
    float tilePx;       // the grid's tile, in pixels of the main view
    uint  gridW, gridH; // tiles across / down (0 = no grid: every source keeps its spikes)
    float cluster;      // "Glare cluster merge" (0..1)
} gpc;

float glareHash(float n) { return fract(sin(n * 12.9898 + 4.1414) * 43758.5453); }

// Proximity (2026-09-26): the SAME satellite glares far wider when the camera is near it — the viewer
// resolves it from metres away, and a mesh resolved in the main view is a close-up too, while its
// point sprite from the ground carries the glare the distance tuning was chosen for. So the sprite's
// size scales by 1 → nearGain over range nearRangeM → 0, smoothly; at nearRangeM and beyond it is
// exactly 1.0, leaving every distant/ground source untouched. Cost: two ALU ops off a range the
// record already carries (a sprite's rangeM, a glint's glintPos.w), no extra pass or buffer.
// A range of 0 means "unknown" (a record without one) and gets no scaling.
float glareNearScale(float rangeM)
{
    if (rangeM <= 0.0 || gpc.nearRangeM <= 0.0 || gpc.nearGain <= 1.0) return 1.0;
    float t = clamp(1.0 - rangeM / gpc.nearRangeM, 0.0, 1.0);
    return mix(1.0, gpc.nearGain, t * t * (3.0 - 2.0 * t));
}

// d: pixels from the source; radius: the sprite's radius in pixels; spikeW: the share of its spikes it keeps
// (glareSpikeWeight: < 1 in a cluster). Returns the corona's weight (x) and the core's (y), both already
// windowed. A crowded source's spikes give way to a slightly stronger halo and shorter rays: a cluster
// of lights reads as one soft glow, not a sea urchin of overlapping stars.
vec2 glareProfile(vec2 d, float radius, float spikeW)
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
        float len = (0.45 + 0.55 * glareHash(kk * 3.71 + 2.0)) * mix(0.55, 1.0, spikeW); // of the radius
        float bright = 0.35 + 0.65 * glareHash(kk * 5.19 + 3.0);
        float along = max(1.0 - x / len, 0.0);
        spikes += bright * exp(-p * p / (2.0 * 0.75 * 0.75)) * pow(along, gpc.falloff);
    }
    return vec2(((0.25 + 0.15 * (1.0 - spikeW)) * halo + 0.6 * spikeW * spikes) * win, core * win);
}

// ── Glare clusters (2026-10-20, issue 20261019_222938 "Glares") ──────────────────────────────────────────
// One glare per source drew a cluster of bright lights (a launch pad's floods, a Starlink train, the Reflect
// ring, a mesh's glints) as overlapping stars: "spidery, sea-urchin-y". glare_cluster.comp sums every glaring
// source into a coarse screen grid (per tile: the weights, and the weighted position inside the tile, fixed
// point), each weighted u = (its flare / the threshold's)^2 x its visibility. A source then keeps the share
// w = u_self / (u_self + the others' u within ~its own glare radius) of its spikes, squared: a lone source
// keeps them all (w = 1, exactly as before), the brightest of a group most of them, N equal neighbours each
// 1/N^2 (a soft glow). Continuous in the sources' brightness and position (the tile's own centroid, and the
// window's edge a smoothstep), so a moving train does not flicker. The core and halo are never touched.
#ifdef GLARE_GRID_SET
#ifndef GLARE_GRID_ACCESS
#define GLARE_GRID_ACCESS readonly
#endif
layout(set = GLARE_GRID_SET, binding = GLARE_GRID_BINDING, std430) GLARE_GRID_ACCESS buffer GlareGrid {
    uint glareGrid[]; // per tile: sum q, sum q fx, sum q fy (q = uint(u x kGlareQ), f the position in the tile x 64)
};
const float kGlareQ = 16.0;
// A glaring source's weight in the grid: (flare / the threshold flare)^2, capped at 2^12.
float glareClusterU(float s) { return exp2(4.0 * clamp(s, 0.0, 3.0)); }
uint glareClusterQ(float s, float vis) { return uint(glareClusterU(s) * clamp(vis, 0.0, 1.0) * kGlareQ + 0.5); }

// px: the source's pixel; q: its own grid weight (glareClusterQ, as glare_cluster.comp wrote it); radius: its
// glare's radius in pixels. 1 = keep every spike.
float glareSpikeWeight(vec2 px, uint q, float radius)
{
    if (gpc.cluster <= 0.0 || gpc.gridW == 0u || gpc.gridH == 0u || q == 0u)
        return 1.0;
    float T = max(gpc.tilePx, 1.0);
    float R = clamp(0.8 * radius, 1.5 * T, 12.0 * T);
    ivec2 g  = ivec2(gpc.gridW, gpc.gridH);
    ivec2 a  = clamp(ivec2(floor((px - R) / T)), ivec2(0), g - 1);
    ivec2 b  = clamp(ivec2(floor((px + R) / T)), ivec2(0), g - 1);
    ivec2 own = ivec2(floor(px / T));
    float sum = 0.0, kOwn = 1.0;
    for (int y = a.y; y <= b.y; ++y)
        for (int x = a.x; x <= b.x; ++x) {
            uint i  = uint(y * g.x + x) * 3u;
            uint qs = glareGrid[i];
            if (qs == 0u) continue;
            vec2 c  = (vec2(x, y) + vec2(float(glareGrid[i + 1u]), float(glareGrid[i + 2u])) / (64.0 * float(qs))) * T;
            float k = 1.0 - smoothstep(0.5 * R, R, length(c - px));
            sum += float(qs) * k;
            if (x == own.x && y == own.y) kOwn = k;
        }
    float qf = float(q);
    float others = max(sum - qf * kOwn, 0.0);
    float w = qf / (qf + others);
    return mix(1.0, w * w, clamp(gpc.cluster, 0.0, 1.0));
}
#endif
