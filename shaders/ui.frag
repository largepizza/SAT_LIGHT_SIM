#version 450

layout(location = 0) in vec2  fragUV;
layout(location = 1) in vec4  fragColor;
layout(location = 2) in float fragMode;
layout(location = 3) in vec2  fragLocalPos;
layout(location = 4) in vec2  fragHalfSize;
layout(location = 5) in vec4  fragCornerRadius;

layout(location = 0) out vec4 outColor;

// binding 0 = font atlas (R8_UNORM, alpha mask for text glyphs)
layout(set = 0, binding = 0) uniform sampler2D fontAtlas;

// binding 1 = icon atlas (R8G8B8A8_UNORM, RGBA sprite sheet)
layout(set = 0, binding = 1) uniform sampler2D iconAtlas;

// Signed distance to a box of half-size `halfSize` centered at the origin, with
// per-corner rounding (radius order: topLeft, topRight, bottomLeft, bottomRight —
// screen space, so p.y < 0 is the top half). Negative inside, positive outside.
// Standard technique (Inigo Quilez) extended to support 4 independent radii so
// shapes like a title bar (rounded top, square bottom) render correctly.
float roundedBoxSDF(vec2 p, vec2 halfSize, vec4 radius) {
    float r = (p.x < 0.0)
        ? ((p.y < 0.0) ? radius.x : radius.z)   // left:  topLeft / bottomLeft
        : ((p.y < 0.0) ? radius.y : radius.w);  // right: topRight / bottomRight
    r = min(r, min(halfSize.x, halfSize.y));
    vec2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

void main() {
    if (fragMode < 0.5) {
        // Solid rectangle — rounded per-corner via SDF (radius 0 on every corner
        // reduces exactly to a sharp-edged rect, so this is a no-op for the many
        // callers — borders, unrounded panels — that never set a radius).
        float dist = roundedBoxSDF(fragLocalPos, fragHalfSize, fragCornerRadius);
        float aa = 1.0 - smoothstep(-1.0, 1.0, dist);
        outColor = vec4(fragColor.rgb, fragColor.a * aa);
    } else if (fragMode < 1.5) {
        // Text glyph (2026-10-21): the atlas holds a signed distance field (UIRenderer::loadFont): stored =
        // kSdfOnEdge + distance in atlas texels x kSdfDistScale, inside brighter. The distance is turned into
        // SCREEN pixels by the glyph's own scale (atlas texels per screen pixel, from the UV derivatives —
        // a glyph quad is one primitive with one mode, so its 2x2 pixel quads all take this branch) and the
        // coverage is a one-pixel ramp across the outline: an exact edge at any size, 9 px to titles.
        const float kSdfOnEdge = 128.0 / 255.0;
        const float kSdfDistScale = 16.0 / 255.0;
        vec2 tex = fragUV * vec2(textureSize(fontAtlas, 0));
        vec2 dx = dFdx(tex), dy = dFdy(tex);
        float texPerPx = max(sqrt(0.5 * (dot(dx, dx) + dot(dy, dy))), 1e-3);
        float distTex = (texture(fontAtlas, fragUV).r - kSdfOnEdge) / kSdfDistScale;
        // Very small text (under 13 px, the sizes only the automatic UI scale reaches on short windows): a
        // slight emboldening, up to 0.1 px at 9 px, so sub-pixel stems keep their weight. None at 13 px and up
        // (at 16-18 px the plain ramp already matches the bitmap's ink within a few percent).
        float pxPerEm = 48.0 / texPerPx;
        float bold = 0.10 * clamp((13.0 - pxPerEm) / 4.0, 0.0, 1.0);
        float alpha = clamp(distTex / texPerPx + 0.5 + bold, 0.0, 1.0);
        outColor = vec4(fragColor.rgb, fragColor.a * alpha);
    } else if (fragMode < 2.5) {
        // Icon sprite: RGBA sample, tinted by fragColor
        vec4 tex = texture(iconAtlas, fragUV);
        outColor = tex * fragColor;
    } else {
        // Rounded border ring (mode 3): a stroke of width fragUV.x traced along the
        // same rounded-box boundary the fill uses, so the outline actually follows
        // the curve instead of a straight-strip border squaring off the corners.
        // Stroke width rides in fragUV (all 4 corners share the same value, so it's
        // constant after interpolation) rather than growing UIVertex for one field.
        float bw = fragUV.x;
        float dist = roundedBoxSDF(fragLocalPos, fragHalfSize, fragCornerRadius);
        float outerAA = 1.0 - smoothstep(-1.0, 1.0, dist);
        float innerAA = 1.0 - smoothstep(-1.0, 1.0, dist + bw);
        float ringAlpha = clamp(outerAA - innerAA, 0.0, 1.0);
        outColor = vec4(fragColor.rgb, fragColor.a * ringAlpha);
    }
}
