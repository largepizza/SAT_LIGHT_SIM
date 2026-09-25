#version 450
// Model viewer markers (2026-09-25), drawn OVER the mesh (sat_mesh_bg.vert's fullscreen triangle,
// alpha-blended, depth-tested, no depth write):
//   - the line from the satellite toward the observer (dashed cyan) and, for a ground-site mirror, to the
//     site its beam is on (solid orange). Each fragment takes the depth of the line where it crosses
//     that pixel, so the model hides the part of a line behind it and the part in front shows. They
//     were drawn in the background pass until this, so the model always covered them.
//   - the dots where those lines end on the Earth, on top of everything (depth 0) so you can always
//     find yourself; hidden only when the Earth itself is in the way. Their "You" / "Target" labels are
//     UI text placed from the same projection on the CPU (SatelliteSim::recordModelViewer).
// The segments are clipped to the view frustum (with a margin) in clip space before the divide: the
// first cut only clipped at w = 1e-4, so a line ending near or behind the camera projected to
// coordinates in the millions of pixels and the distance test lost all precision — the lines jumped
// and flickered as the camera orbited close to the model.

#include "common.glsl"
#include "sat_mesh_common.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

const vec3 kYouColor  = vec3(0.35, 0.95, 1.0);
const vec3 kSiteColor = vec3(1.0, 0.62, 0.18);

// Clips the clip-space segment c0-c1 to w >= 1e-3 and |x|, |y| <= 1.5 w. False if nothing is left.
bool clipSegment(inout vec4 c0, inout vec4 c1)
{
    float t0 = 0.0, t1 = 1.0;
    vec4 d = c1 - c0;
    // Planes as f(c) = dot(n, c) >= k.
    vec4 n[5] = vec4[5](vec4(0, 0, 0, 1), vec4(1, 0, 0, 1.5), vec4(-1, 0, 0, 1.5), vec4(0, 1, 0, 1.5),
                        vec4(0, -1, 0, 1.5));
    float k[5] = float[5](1e-3, 0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < 5; ++i) {
        float f0 = dot(n[i], c0) - k[i];
        float fd = dot(n[i], d);
        if (abs(fd) < 1e-20) {
            if (f0 < 0.0) return false;
            continue;
        }
        float t = -f0 / fd;
        if (fd > 0.0) t0 = max(t0, t);
        else          t1 = min(t1, t);
        if (t0 > t1) return false;
    }
    vec4 a = c0 + d * t0, b = c0 + d * t1;
    c0 = a;
    c1 = b;
    return true;
}

// Coverage (0..1) of the world segment p0-p1 at this fragment, its depth there, and the distance along
// it from p0's end in pixels (for dashes).
float segment(vec3 p0, vec3 p1, vec2 frag, float halfWidth, out float depth, out float along)
{
    vec4 c0 = frame.viewProj * vec4(p0, 1.0), c1 = frame.viewProj * vec4(p1, 1.0);
    depth = 1.0;
    along = 0.0;
    if (!clipSegment(c0, c1)) return 0.0;
    vec2 a = (c0.xy / c0.w * 0.5 + 0.5) * frame.bgParams.zw;
    vec2 b = (c1.xy / c1.w * 0.5 + 0.5) * frame.bgParams.zw;
    vec2 ab = b - a;
    float s = clamp(dot(frag - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    float d = length(frag - (a + s * ab));
    // z/w is affine in screen space, so the line's depth at this pixel is the endpoints' mixed.
    depth = clamp(mix(c0.z / c0.w, c1.z / c1.w, s), 0.0, 1.0);
    along = s * length(ab);
    return 1.0 - smoothstep(halfWidth - 0.6, halfWidth + 0.6, d);
}

// A ground marker: a filled dot with a dark rim, hidden once the point is over the Earth's limb.
vec4 groundMarker(vec4 m, vec3 color, vec2 frag)
{
    if (m.w < 0.5) return vec4(0.0);
    vec3 up = normalize(m.xyz - frame.earthCenter.xyz);
    if (dot(up, frame.camPos.xyz - m.xyz) <= 0.0) return vec4(0.0); // behind the Earth from here
    vec4 c = frame.viewProj * vec4(m.xyz, 1.0);
    if (c.w <= 1e-6) return vec4(0.0);
    vec2 px = (c.xy / c.w * 0.5 + 0.5) * frame.bgParams.zw;
    float r = frame.bgParams.y, d = length(frag - px);
    float fill = 1.0 - smoothstep(r - 1.0, r, d);
    float rim  = 1.0 - smoothstep(r + 1.0, r + 2.5, d);
    return vec4(mix(vec3(0.0), color, fill), rim);
}

void main()
{
    vec2 frag = gl_FragCoord.xy;
    // Dots first: on top of everything.
    vec4 t = groundMarker(frame.marker1, kSiteColor, frag);
    vec4 o = groundMarker(frame.marker0, kYouColor, frag);
    if (t.a > 0.0 || o.a > 0.0) {
        vec4 c = o.a > 0.0 ? o : t;
        outColor = c;
        gl_FragDepth = 0.0;
        return;
    }
    // Lines: whichever is nearer at this pixel.
    float bestA = 0.0, bestD = 1.0;
    vec3  bestC = vec3(0.0);
    if (frame.marker0.w > 0.5) {
        float depth, along;
        float cov = segment(vec3(0.0), frame.marker0.xyz, frag, 1.0, depth, along);
        cov *= step(0.5, fract(along / 14.0)) * 0.6; // faint and dashed: the direction you would be
        if (cov > 0.0) { bestA = cov; bestD = depth; bestC = kYouColor; }
    }
    if (frame.marker1.w > 0.5) {
        float depth, along;
        float cov = segment(vec3(0.0), frame.marker1.xyz, frag, 1.3, depth, along) * 0.85; // the beam
        if (cov > 0.0 && (bestA <= 0.0 || depth < bestD)) { bestA = cov; bestD = depth; bestC = kSiteColor; }
    }
    if (bestA <= 0.0) discard;
    outColor = vec4(bestC, bestA);
    gl_FragDepth = bestD;
}
