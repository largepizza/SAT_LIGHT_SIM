// ── reflect_beam.glsl ─────────────────────────────────────────────────────────────────────────
// The ReflectBeam record and the buffer's capacity constant, shared by every stage that touches
// the Reflect-Orbital beam list: sat_orbit.comp writes it, cloud_march.comp marches the sky tube,
// sat_sky.frag draws the ground spot.
//
// Previously declared three times. Identical layout in all three, but the field DOCS had already
// drifted (debugPad was described as "unused" in one copy and as the originating satellite's
// dispatch index in another), which is exactly how a shared binary layout starts to rot.
//
// The buffer DECLARATION stays per-file: the three consumers bind it at different indices and
// with different access qualifiers (sat_orbit.comp writes, the other two are readonly).
//
// BEAM_MAX_ACTIVE must match kMaxActiveBeams in src/simulations/SatelliteSim.h.
const uint BEAM_MAX_ACTIVE = 2048u;

struct ReflectBeam {
    vec3  satENU;       // meters, observer-relative (East, North, Up)
    float intensity;    // groundIrradiance * beamGain
    vec3  targetENU;    // meters, observer-relative
    float footprintRadM;
    vec3  reflectDirENU; // unit direction — mirror's ACTUAL current reflected-sunlight direction
                          // (debug pointing-ray visualization, C12 follow-up #12)
    float debugPad;       // repurposed (follow-up #20): originating satellite's stable dispatch
                          // index, for stable downsampling — see the write site below.
    float blockAltM;       // C12 follow-up #33: copied from beamCloudBlockBuf[bestIdx] at write
    float blockOpacity;    // time below — see beam_cloud_block.comp.
    float mirrorRadiusM;   // C12 follow-up #34: repurposed from padding — equivalent-circle radius
                          // of the physical mirror (sqrt(mirrorAreaM2/PI)), used by cloud_march.comp
                          // (sky tube radius) and sat_sky.frag (ground-spot core radius).
    float pad1;            // std430 16-byte alignment
};
