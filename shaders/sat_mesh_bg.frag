#version 450
// Model viewer background: the full sky renderer seen from the viewer's camera (SatEnvProbes'
// SKY_ENV target, HDR) — or, before that exists, the camera's ray into earth_env.glsl — plus the
// Sun's disc, through the same exposure as the mesh. Markers on the Earth: where the OBSERVER stands
// (with a faint line from the satellite to them: the direction it would have to send light to be
// seen) and, for a ground-site mirror, the site it is aiming at.

#include "common.glsl"
#include "terrain.glsl"
#include "sat_mesh_common.glsl"
#include "earth_env.glsl"

layout(set = 0, binding = 9) uniform sampler2D viewerBgTex; // SatEnvProbes::viewerBgView (HDR)

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

// Screen position (px) of a world point, and whether it is in front of the camera.
bool projectPx(vec3 p, out vec2 px)
{
    vec4 c = frame.viewProj * vec4(p, 1.0);
    px = vec2(0.0);
    if (c.w <= 1e-6) return false;
    px = (c.xy / c.w * 0.5 + 0.5) * frame.bgParams.zw;
    return true;
}

// A ground marker: a filled dot with a dark rim, hidden once the point is over the Earth's limb.
vec4 groundMarker(vec4 m, vec3 color, vec2 frag)
{
    if (m.w < 0.5) return vec4(0.0);
    vec3 up = normalize(m.xyz - frame.earthCenter.xyz);
    if (dot(up, frame.camPos.xyz - m.xyz) <= 0.0) return vec4(0.0); // behind the Earth from here
    vec2 px;
    if (!projectPx(m.xyz, px)) return vec4(0.0);
    float r = frame.bgParams.y, d = length(frag - px);
    float fill = 1.0 - smoothstep(r - 1.0, r, d);
    float rim  = 1.0 - smoothstep(r + 1.0, r + 2.5, d);
    return vec4(mix(vec3(0.0), color, fill), rim);
}

// Distance (px) from `frag` to the screen segment a-b.
float segDist(vec2 frag, vec2 a, vec2 b)
{
    vec2 ab = b - a;
    float t = clamp(dot(frag - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    return length(frag - (a + t * ab));
}

void main()
{
    vec4 far = frame.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - frame.camPos.xyz);
    vec3 ro  = frame.camPos.xyz - frame.earthCenter.xyz;
    vec3 L;
    if (frame.bgParams.x > 0.5)
        L = texture(viewerBgTex, vNdc * 0.5 + 0.5).rgb; // the full renderer, from this camera
    else
        L = earthEnv(ro, dir, frame.sunDir.xyz, 0.0, frame.earthCenter.w);

    // Sun disc (0.27° radius, softened), only where the Earth does not hide it (the env renderer
    // leaves it out: on a mesh the GGX sun lobe is the glint).
    if (frame.sunDir.w > 0.5 && raySphere(ro, dir, R_EARTH).x < 0.0) {
        float c = dot(dir, frame.sunDir.xyz);
        L += vec3(1.0, 0.96, 0.88) * 40.0 * smoothstep(0.99998, 0.999992, c);
        L += vec3(1.0, 0.94, 0.82) * 0.08 * pow(max(c, 0.0), 800.0);
    }
    vec3 col = vec3(1.0) - exp(-frame.camPos.w * L);

    // Markers, drawn over the sky and under the mesh.
    vec2 frag = gl_FragCoord.xy;
    if (frame.marker0.w > 0.5) {
        // Line from the satellite (the frame origin) toward the observer, faint and dashed.
        vec2 a, b;
        if (projectPx(vec3(0.0), a) && projectPx(frame.marker0.xyz, b)) {
            float d = segDist(frag, a, b);
            float along = length(frag - a);
            float dash = step(0.5, fract(along / 14.0));
            col = mix(col, vec3(0.35, 0.95, 1.0), (1.0 - smoothstep(0.6, 1.6, d)) * 0.55 * dash);
        }
    }
    vec4 t = groundMarker(frame.marker1, vec3(1.0, 0.62, 0.18), frag); // a mirror's ground site
    col = mix(col, t.rgb, t.a);
    vec4 o = groundMarker(frame.marker0, vec3(0.35, 0.95, 1.0), frag); // you
    col = mix(col, o.rgb, o.a);
    outColor = vec4(col, 1.0);
}
