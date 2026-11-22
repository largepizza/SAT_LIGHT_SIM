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
// x the optical depth from slot's emitter to the point v from it (v in the flash's frame: x t1, y t2, z up; r = |v|),
// y the depth of the emitter's own cloud along that direction. (The emitter's N: fvN.)
vec3 fvAt(uint slot, vec3 v, float r)
{
    vec3  d = v / max(r, 1.0);
    float u = atan(d.y, d.x) * (1.0 / 6.2831853);                                   // (the sampler wraps it)
    float e = clamp(asin(clamp(d.z, -1.0, 1.0)) * (1.0 / 3.14159265) + 0.5, 0.5 / float(kFvE), 1.0 - 0.5 / float(kFvE));
    float s = clamp(sqrt(min(r / kFvRmax, 1.0)) * float(kFvR), 0.5, float(kFvR) - 0.5);
    return textureLod(FV_SAMPLER, vec3(u, e, (float(slot) * float(kFvR) + s) / float(kFvSlots * kFvR)), 0.0).rgb;
}
// The emitter's N (cloud_v2_flashvol.comp writes it to the alpha of its slot's first texel).
float fvN(uint slot)
{
    return texelFetch(FV_SAMPLER, ivec3(0, 0, int(slot) * kFvR), 0).a;
}
#endif
#endif
