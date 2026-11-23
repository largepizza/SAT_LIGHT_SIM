#version 450
// ── Lightning: channels (mode 0, one quad per segment) and the cloud's glow (mode 1: ONE full-screen triangle;
// lightning.frag loops over the flashes' emitters per pixel) ──
// (SatelliteSimLightning.cpp, 2026-10-05.) A channel is drawn at its TRUE luminous width (a few metres for a
// return stroke's main channel, less for branches and tendrils, hundreds of metres for a sprite's tendrils);
// thinner than a pixel it is drawn a pixel wide with its energy kept (dimmer, never fatter), so a far stroke is
// a fine line and a near one saturates into a white core. A faint halo (the light scattered by the rain and air
// around it) widens the main channel. Brightness = the flash's intensity now x the segment's weight: during the
// stepped leader only the revealed part shows, dim with brighter tips; a ground stroke's branches light with the
// first return stroke and fade, the main channel carries the later ones; a spider spreads through its web.
#include "depth.glsl"
#include "lightning_draw.glsl"

layout(location = 0) out vec2 vLocal;          // px: x along the segment from its start, y across
layout(location = 1) flat out vec4 vCore;      // rgb the core's energy (x exposure, pre-tonemap), a its width (px)
layout(location = 2) flat out vec4 vHalo;      // rgb the halo's, a its width (px)
layout(location = 3) flat out vec4 vSeg;       // x length (px), y range (m), z mode, w kind
layout(location = 4) flat out vec4 vGlow;      // a channel's corona sheath: rgb its energy, a 1 / its width^2 (px)
layout(location = 5) flat out vec4 vScat;      // the scattered glow in clear air: rgb its energy, a its core radius^2 (px)
layout(location = 6) flat out vec4 vScatW;     // the same through rain / cloud
layout(location = 7) flat out vec4 vScatK;     // x 1 / the segment's length (px), y / z the clear / wet glow at the quad's edge

void cullV()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0); vLocal = vec2(0.0); vCore = vec4(0.0); vHalo = vec4(0.0); vSeg = vec4(0.0); vGlow = vec4(0.0);
    vScat = vec4(0.0); vScatW = vec4(0.0); vScatK = vec4(0.0);
}
// The scattered glow of one piece of channel L px long, seen D px from it: the integral along the piece of
// 1 / distance^2 (L / D^2 from afar, pi / D beside a long one). Summed over a channel's pieces: pi / D.
float glowK(float D, float L) { return 1.0 / (D * D / L + D * 0.31831); }

vec3 toCam(vec3 p) { return (pc.skyView * vec4(p, 0.0)).xyz; }
vec2 toPx(vec3 c, float tanH, vec2 scr)
{
    return vec2(c.x / (-c.z) / (tanH * pc.aspect), -c.y / (-c.z) / tanH) * 0.5 * scr + 0.5 * scr;
}

void main()
{
    float tanH = tan(pc.fovYRad * 0.5);
    vec2  scr  = vec2(pc.screenW, pc.screenH);
    float pixAng = 2.0 * tanH / pc.screenH;
    int   vi = gl_VertexIndex;
    float E  = pc.params.x;

    if (pc.params.w > 0.5) {
        // ── The cloud lit around the flashes: one full-screen triangle (lightning.frag loops over the emitters) ──
        vec2 q = vec2(float((vi << 1) & 2), float(vi & 2));
        gl_Position = vec4(q * 2.0 - 1.0, 0.0, 1.0);
        vLocal = vec2(0.0); vCore = vec4(0.0); vHalo = vec4(0.0);
        vSeg  = vec4(0.0, 0.0, pc.params.w, 0.0);   // 1 the flashes' glow, 2 the engine lights
        vGlow = vec4(0.0);
        vScat = vec4(0.0); vScatW = vec4(0.0); vScatK = vec4(0.0);
        return;
    }

    // ── A channel segment ──
    BoltSeg  s = bSeg[gl_InstanceIndex];
    uint     slot  = s.packed & 127u;
    uint     order = (s.packed >> 7u) & 3u;
    float    arcF  = float((s.packed >> 9u) & 4095u) / 4095.0;
    float    f     = float((s.packed >> 21u) & 1023u) / 1023.0;
    BoltFlash F = bFlash[slot];
    float I = F.o.w, kind = F.t1.w;
    float arc = arcF * F.t2.w, reveal = F.m.x;
    float b;
    if (kind > 0.5 && kind < 1.5) {
        if (F.m.z > 0.5) {
            // The stepped leader: the revealed part, faint, its advancing tips brighter.
            if (arc > reveal) { cullV(); return; }
            b = 0.04 * (0.3 + 4.0 * smoothstep(reveal - 400.0, reveal, arc)) * (order == 0u ? 1.0 : order == 1u ? 0.6 : 0.25);
        } else
            b = I * s.w * (order == 0u ? 1.0 : F.m.y);
    } else if (kind > 1.5 && kind < 2.5) {
        b = I * s.w;
    } else {
        if (arc > reveal) { cullV(); return; }
        b = I * s.w * (1.0 + 1.5 * exp(-(reveal - arc) / 3000.0));   // the spreading front is brightest
    }
    if (b <= 1e-3) { cullV(); return; }

    vec3  p0 = F.o.xyz + F.t1.xyz * s.p0.x + F.t2.xyz * s.p0.y + F.u.xyz * s.p0.z;
    vec3  p1 = F.o.xyz + F.t1.xyz * s.p1.x + F.t2.xyz * s.p1.y + F.u.xyz * s.p1.z;
    vec3  c0 = toCam(p0), c1 = toCam(p1);
    const float zN = 1.0;
    if (c0.z > -zN && c1.z > -zN) { cullV(); return; }
    if (c0.z > -zN) c0 = mix(c1, c0, (c1.z + zN) / (c1.z - c0.z));
    if (c1.z > -zN) c1 = mix(c0, c1, (c0.z + zN) / (c0.z - c1.z));
    vec2  P0 = toPx(c0, tanH, scr), P1 = toPx(c1, tanH, scr);
    float range = 0.5 * (length(c0) + length(c1));
    float mpp   = range * pixAng;
    bool  sprite = kind > 1.5 && kind < 2.5;
    // The luminous width (m): a return stroke's channel ~3 m (its glow), branches 1.2, twigs 0.6, tendrils 0.35;
    // a sprite's tendrils hundreds of metres, tapering.
    float wTrue = sprite ? mix(380.0, 120.0, f) * (order == 0u ? 1.0 : 0.6)
                         : (order == 0u ? 3.0 : order == 1u ? 1.2 : order == 2u ? 0.6 : 0.35);
    float wPx = wTrue / mpp;
    float wD  = max(wPx, 1.0);
    float ef  = wPx / wD;                                  // energy kept when thinner than a pixel
    float air = boltAir(0.5 * (p0 + p1), range);
    vec3  col;
    float x;
    // The stroke's colour (2026-11-22, lightning_draw.glsl BoltColour; a sprite keeps its own): `red` what the air
    // leaves of each channel (the blue goes first: a far stroke is warmer), `shS` the corona sheath's share of this
    // piece's light (a main channel a few percent, more per branch order, most of a stepped leader's).
    vec3  red = vec3(1.0);
    float shS = 0.0;
    if (sprite) {
        col = mix(vec3(1.0, 0.06, 0.10), vec3(0.55, 0.08, 0.95), f * f * f);
        x = E * pc.params.y * 1.2 * b * ef * air;
    } else {
        col = bc.core.rgb;
        x = E * pc.params.y * 1.4 * b * ef * air;
        red = boltAirTint(air);
        shS = (kind > 0.5 && kind < 1.5 && F.m.z > 0.5) ? bc.sheathK.z : bc.sheathK.x * pow(bc.sheathK.y, float(order));
        shS = clamp(shS, 0.0, 0.9);
    }
    // The halo: the stroke's light scattered by the rain and air around it (a broad violet-white glow about the
    // main channel, a tight one about the branches). Its energy is a share of the core's, spread over its area.
    float hM  = order == 0u ? 70.0 : (order == 1u ? 22.0 : 8.0);
    float hPx = sprite ? 0.0 : clamp(hM / mpp, 1.5, 22.0);
    float hK  = sprite ? 0.0 : (order == 0u ? 0.35 : 0.2) * wD / max(hPx, 1.0);
    vCore = vec4(col * x * red * (1.0 - shS), wD);
    vHalo = vec4(bc.halo.rgb * x * hK * red, hPx);
    float Lpx = length(P1 - P0);
    // The corona sheath: a Gaussian sPx wide hugging the core, with the share of the core's light it took (a row of
    // pieces shorter than it is wide overlaps: the sum along the channel is kept).
    float sPx = max(bc.sheath.a * pow(bc.sheathK.w, float(order)) / mpp, 0.75);
    vGlow = vec4(bc.sheath.rgb * red * (x * shS * wD * min(1.0 / (sPx * 1.7725), Lpx / (sPx * sPx * 3.1416))), 1.0 / (sPx * sPx));
    // The scattered glow: the halo's energy (its Gaussian over this piece's capsule: pi h^2 + L h sqrt(pi)), spread as
    // glowK from the sheath's radius out to the quad's edge, where it is taken to 0 (2 pi int K r dr = pi L (ln(1 +
    // R^2 / s^2) - 1) about a short piece). Through rain and cloud its core is wider.
    {
        float R   = 2.5 * hPx, L = max(Lpx, 0.05);
        float eH  = x * hK * (3.1416 * hPx * hPx + L * hPx * 1.7725);
        float s2  = sPx * sPx, s2w = s2 * bc.glowWet.a * bc.glowWet.a;
        vScat  = vec4(bc.glow.rgb * red * (eH / (3.1416 * L * max(log(1.0 + R * R / s2) - 1.0, 0.5))), s2);
        vScatW = vec4(bc.glowWet.rgb * red * (eH / (3.1416 * L * max(log(1.0 + R * R / s2w) - 1.0, 0.5))), s2w);
        vScatK = vec4(1.0 / L, glowK(sqrt(R * R + s2), L), glowK(sqrt(R * R + s2w), L), 0.0);
    }
    vec2  ax  = (Lpx > 1e-3) ? (P1 - P0) / Lpx : vec2(1.0, 0.0);
    vec2  ay  = vec2(-ax.y, ax.x);
    float hw  = max(0.5 * wD + 1.0, 2.5 * hPx);
    float u   = (vi & 1) != 0 ? Lpx + hw : -hw;
    float w   = (vi & 2) != 0 ? hw : -hw;
    vec2  px  = P0 + ax * u + ay * w;
    // (harness `lightning color knock=1`: no depth test, the quad at depth 0)
    gl_Position = vec4(px / scr * 2.0 - 1.0, (int(bc.wet.z + 0.5) & 1) != 0 ? 0.0 : sceneDepthFromDistance(range), 1.0);
    vLocal = vec2(u, w);
    vSeg   = vec4(Lpx, range, 0.0, kind);
}
