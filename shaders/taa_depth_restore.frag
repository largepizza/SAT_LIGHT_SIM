#version 450
// Sky TAA (sky_taa.comp): the background was rendered offscreen and blitted into the swapchain, so the
// main pass's depth attachment starts cleared. This first draw of the main pass writes the background's
// unified depth back (from sat_sky.frag -DSKY_TAA's R32F copy; colour writes are masked off), so the
// stars, planets and satellites that follow are occluded by terrain, sea, cloud and meshes as before.
layout(set = 0, binding = 0) uniform sampler2D skyDepth;
void main() {
    gl_FragDepth = texelFetch(skyDepth, ivec2(gl_FragCoord.xy), 0).r;
}
