#ifndef SATLIGHTSIM_FLASH_VOLUME_GLSL
#define SATLIGHTSIM_FLASH_VOLUME_GLSL
// ── The cloud around a lightning flash's light: optical depth outward from each emitter (2026-11-21,
// issue_20261121_163317) ────────────────────────────────────────────────────────────────────────────────────────────
// A flash lit the cloud as a glow by distance alone (lightning_draw.glsl boltGlow): no cloud between the light and what
// it lit, no phase function, so a flash behind a tower lit the tower's near side and a flash inside a cloud drew a round
// patch. Each emitter of the nearest flashes (a flash's origin, where a ground stroke leaves the cloud, a spider's
// spreading fronts) now carries a small volume of the cloud's OPTICAL DEPTH from it: kFvA x kFvE directions (azimuth x
// elevation in the flash's own frame: t1, t2, up; 16 x 16 drew its cells on a cloud close to the light) x kFvR distances (r = kFvRmax (k + 0.5)^2 / kFvR^2: ~25 m to
// kFvRmax, fine near the light), one slot of a 3D image per emitter (cloud_v2_flashvol.comp bakes a slot when its
// emitter is new or has moved; SatelliteSim::boltLightAssign hands them out), with the depth of the emitter's own cloud
// along each direction and the factor N that keeps a flash's light inside its own cloud. lightning.frag reads the
// depth from the emitter to the cloud each pixel shows (and a little beyond it: the far side) and lights it as the
// march lights a cloud sample under a reflector beam: the two-lobe phase x e^-od + the slab's diffuse field
// (cv2DiffuseField), x N.
const int   kFvA     = 32;        // azimuths (wraps)
const int   kFvE     = 32;        // elevations, -90 .. 90 deg
const int   kFvR     = 16;        // distances
const int   kFvSlots = 16;        // emitters with a volume
const float kFvRmax  = 24000.0;   // m: beyond it the depth stays what it was there

#ifdef FV_SAMPLER
// The optical depth from slot's emitter to the point v from it (v in the light's frame: x t1, y t2, z up; r the
// distance to read at). One read for both kinds of light (2026-11-22: an engine light's, issue_20261122_105107,
// lightning.frag engineLightAt; a flash's since issue_20261122_213848, boltLightAt, which read the depth bilinearly and
// drew the directions as facets and a starburst round the emitter). Every slot holds, beside the depth (r), the
// TRANSMITTANCE e^-depth (g) and where along each distance step the step's depth is centred (b, 0 .. 1);
// texel k is the end of step k, r = kFvRmax ((k + 0.5) / kFvR)^2 (cloud_v2_flashvol.comp's u1).
//   meanDepth (a flash's read) blends the DEPTHS over the directions, linear in sqrt(r) between the steps' ends, as
// its bilinear read did (blended as transmittances, a flash inside a tower read the open directions round it and lit
// the whole tower near white).
//   Otherwise (an engine light's) the transmittances are blended, not the depths (the depth returned is -ln of the blend): the
// directions are 5.6 / 11 deg apart, and where one passes over a deck and the next has been inside it for kilometres
// (a light 1 km above a deck, the deck 10 km off) the mean of their depths is a shadow that is not there (from 300 km
// the lit patch ended in a dark band 9-13 km out), while the light that reaches the point is the share of the
// directions that are open. The blend is a cubic B-spline (four filtered taps a slice): with the four nearest
// directions weighted bilinearly the blend had a crease along every line between two directions' centres, and a steady
// light drew them, straight wedges and patch outlines on a deck lit through from the other side.
//   Along the distance the depth rises from the step's start to its end over the stretch about its centre (the whole
// step for 0.5, its last fifth for 0.9), eased in where the cloud begins inside the step and out where it ends there.
float fvOdFine(uint slot, vec3 v, float r, bool meanDepth)
{
    vec3  d  = v / max(length(v), 1.0);   // (r may be short of |v|: the caller's)
    vec2  p  = vec2(fract(atan(d.y, d.x) * (1.0 / 6.2831853)) * float(kFvA),
                    clamp((asin(clamp(d.z, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5) * float(kFvE), 0.5, float(kFvE) - 0.5));
    vec2  tc = floor(p - 0.5) + 0.5, f = p - tc, f2 = f * f, f3 = f2 * f;
    vec2  w1 = (4.0 - 6.0 * f2 + 3.0 * f3) * (1.0 / 6.0), w3 = f3 * (1.0 / 6.0);
    vec2  g0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) * (1.0 / 6.0) + w1, g1 = 1.0 - g0;
    vec2  h0 = (tc - 1.0 + w1 / g0) / vec2(float(kFvA), float(kFvE)), h1 = (tc + 1.0 + w3 / g1) / vec2(float(kFvA), float(kFvE));
    float sb = sqrt(min(r / kFvRmax, 1.0)) * float(kFvR);
    int   k  = clamp(int(sb + 0.5), 0, kFvR - 1);
    float zn = 1.0 / float(kFvSlots * kFvR), z1 = (float(int(slot) * kFvR + k) + 0.5) * zn;
    vec4  q  = g0.x * g0.y * textureLod(FV_SAMPLER, vec3(h0.x, h0.y, z1), 0.0) + g1.x * g0.y * textureLod(FV_SAMPLER, vec3(h1.x, h0.y, z1), 0.0) +
               g0.x * g1.y * textureLod(FV_SAMPLER, vec3(h0.x, h1.y, z1), 0.0) + g1.x * g1.y * textureLod(FV_SAMPLER, vec3(h1.x, h1.y, z1), 0.0);
    vec2  a  = vec2(meanDepth ? q.r : q.g, q.b);
    float t0 = meanDepth ? 0.0 : 1.0;
    if (k > 0) {
        float z0 = z1 - zn;
        vec4  q0 = g0.x * g0.y * textureLod(FV_SAMPLER, vec3(h0.x, h0.y, z0), 0.0) + g1.x * g0.y * textureLod(FV_SAMPLER, vec3(h1.x, h0.y, z0), 0.0) +
                   g0.x * g1.y * textureLod(FV_SAMPLER, vec3(h0.x, h1.y, z0), 0.0) + g1.x * g1.y * textureLod(FV_SAMPLER, vec3(h1.x, h1.y, z0), 0.0);
        t0 = meanDepth ? q0.r : q0.g;
    }
    float od0 = meanDepth ? t0 : -log(clamp(t0, 1e-7, 1.0));
    float od1 = max(meanDepth ? a.x : -log(clamp(a.x, 1e-7, 1.0)), od0);
    float uA = max(float(k) - 0.5, 0.0) / float(kFvR), uB = (float(k) + 0.5) / float(kFvR);
    float x  = clamp((r - kFvRmax * uA * uA) / (kFvRmax * (uB * uB - uA * uA)), 0.0, 1.0);
    float xa = max(2.0 * a.y - 1.0, 0.0), xb = min(2.0 * a.y, 1.0);
    float xn = clamp((x - xa) / max(xb - xa, 0.05), 0.0, 1.0);
    float pr = mix(mix(xn, xn * xn * (2.0 - xn), smoothstep(0.5, 0.6, a.y)), xn * (1.0 + xn - xn * xn), smoothstep(0.5, 0.4, a.y));
    // (a flash: linear in sqrt(r) between the steps' ends, as its bilinear read was; eased about the step's centre of
    // depth, the steps' ends drew rings round an emitter seen through its own cloud)
    if (meanDepth) pr = clamp(sb - float(k) + 0.5, 0.0, 1.0);
    return od0 + (od1 - od0) * pr;
}
// The emitter's N (cloud_v2_flashvol.comp writes it to the alpha of its slot's first texel).
float fvN(uint slot)
{
    return texelFetch(FV_SAMPLER, ivec3(0, 0, int(slot) * kFvR), 0).a;
}
#endif
#endif
