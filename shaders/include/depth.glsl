// Unified scene depth (lighting overhaul Phase 4, .plans/SAT_RENDERER_PHASE4.md).
//
// Every pass that writes the main render pass's depth attachment writes the SAME function of the
// TRUE distance along its view ray, so terrain, ocean, opaque cloud, satellite meshes and satellite
// points all depth-test against one another at any range, from the ground or from orbit:
//
//     depth = log2(t / near) / log2(far / near),   near 1 cm, far 1e9 m (clamped to kDepthFar)
//
// A logarithmic encoding because the scene spans 1 cm (a mesh in follow mode) to ~1e7 m (Earth's
// limb from orbit): in the D32 float attachment it resolves distance to ~1.5e-6 of itself
// everywhere. Sky with no surface writes 1.0; anything at infinity (stars, planets) draws at
// kDepthFar, just in front of it, so any surface occludes it. Compare op: LESS, cleared to 1.0.
//
// Replaced the 150 km-capped `t / 300 km` encoding with points pinned at 0.5 — which existed so
// that satellites in front of the distant Earth survived in orbital views, and which could not
// express a satellite behind a nearer one.
const float kDepthNearM = 0.01;
const float kDepthFarM  = 1.0e9;
const float kDepthFar   = 0.99999;

float sceneDepthFromDistance(float tM)
{
    const float kInvLogRange = 1.0 / log2(kDepthFarM / kDepthNearM);
    return clamp(log2(max(tM, kDepthNearM) * (1.0 / kDepthNearM)) * kInvLogRange, 0.0, kDepthFar);
}
