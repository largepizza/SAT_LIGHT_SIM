// ── reflect_beam.glsl ─────────────────────────────────────────────────────────────────────────
// The ReflectBeam record and the buffer's capacity constant, shared by every stage that touches
// the Reflect-Orbital beam list: sat_orbit.comp writes most fields, beam_self_march.comp (added
// 2026-08-09) overwrites blockAltM/blockOpacity right after, cloud_march.comp draws the pointing
// ray + volumetric glow, sat_sky.frag draws the ground spot.
//
// Previously declared three times. Identical layout in all three, but the field DOCS had already
// drifted (debugPad was described as "unused" in one copy and as the originating satellite's
// dispatch index in another), which is exactly how a shared binary layout starts to rot.
//
// The buffer DECLARATION stays per-file: consumers bind it at different indices and with
// different access qualifiers (sat_orbit.comp and beam_self_march.comp write, the rest readonly).
//
// BEAM_MAX_ACTIVE must match kMaxActiveBeams in src/simulations/SatelliteSim.h.
const uint BEAM_MAX_ACTIVE = 2048u;

// GROUND_BEAM_MAX must match kMaxGroundBeams in src/simulations/SatelliteSim.h. Capacity of the
// CPU-compacted, observer-range-culled beam list (GroundBeamsBuf in sat_sky.frag) — see that
// buffer's comment and GpuGroundBeams in SatelliteSim.h for why this exists.
const uint GROUND_BEAM_MAX = 256u;

struct ReflectBeam {
    vec3  satENU;       // meters, observer-relative (East, North, Up)
    float intensity;    // groundIrradiance * beamGain
    vec3  targetENU;    // meters, observer-relative
    float footprintRadM;
    vec3  reflectDirENU; // unit direction — mirror's ACTUAL current reflected-sunlight direction
                          // (debug pointing-ray visualization, C12 follow-up #12)
    float debugPad;       // repurposed (follow-up #20): originating satellite's stable dispatch
                          // index, for stable downsampling — see the write site below.
    float blockAltM;       // Written by beam_self_march.comp (2026-08-09), a per-beam slant march
    float blockOpacity;    // through the beam's own real path — see that shader's own header.
    float mirrorRadiusM;   // C12 follow-up #34: repurposed from padding — equivalent-circle radius
                          // of the physical mirror (sqrt(mirrorAreaM2/PI)), used by cloud_march.comp
                          // (sky tube radius) and sat_sky.frag (ground-spot core radius).
    float aimErrorRad;     // Repurposed from padding (2026-08-06): radians remaining in the
                          // mirror's rate-limited slew toward its current target this frame — 0
                          // once converged. See GpuReflectBeam's comment in SatelliteSim.h.
    uint  targetIdx;       // 2026-08-12: this beam's resolved ground-target ORIGINAL index
                          // (sat_orbit.comp's own `bestIdx`, always >= 0 wherever a beam is
                          // written). A STABLE INTEGER IDENTITY — the whole point of carrying it.
                          // The CPU cloud-light build keys its per-target clusters on this instead
                          // of epsilon-matching targetENU positions against a seed beam, which is
                          // what made the old partition order-dependent and discontinuous. See
                          // GpuBeamCloudLights / TrackedBeamLight in SatelliteSim.h.
    uint  pad0, pad1, pad2; // std430 struct alignment is 16 (vec3 members), so the record is 80
                          // bytes with or without these. They are DECLARED EXPLICITLY so the C++
                          // mirror (GpuReflectBeam — glm::vec3 is 4-aligned, so C++ does NOT insert
                          // this tail padding on its own) matches byte-for-byte rather than relying
                          // on the total happening to be a multiple of 16. Same hazard class as
                          // GpuCloudParams: a silent permutation, no compile error.
};
