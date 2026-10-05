// The auroral oval's geometry (2026-10-03). Include after cloud_params.glsl.
//
// The oval is a RING around the geomagnetic pole whose shape is fixed relative to the SUN, not the
// ground: it is thickest and furthest equatorward at magnetic midnight (~23 MLT), thin and close to the
// pole at noon, and the polar cap inside it is dark. The Earth turns under it. Its boundaries follow the
// activity index (src/simulations/SpaceWeather.cpp: auroraOvalBounds, computed on the CPU each frame from
// a deterministic space-weather history), and a substorm pushes a bulge toward the pole around its onset
// MLT and brightens it. The CPU mirror is auroraOvalWeightCpu() — keep the two in step.
//
// It replaced a fixed band at 20 deg colatitude (+8 deg x storm) that faded out over TWICE its width:
// at the default storm level it was lit from ~5 deg off the pole outward, so from orbit it read as a cap.
//
// UBO: auroraMidnight.xyz = toward magnetic midnight in the magnetic equatorial plane (ECEF), .w = Kp;
// auroraOval = colatitudes (deg) of the equatorward edge at midnight / noon, the poleward edge at midnight /
// noon; auroraSub = substorm intensity, cos / sin of its MLT angle, 1 / half-width^2; auroraOval2 = x the
// brightness from activity, y the largest colatitude that can be lit (a cheap early out), z the edge ripple
// (deg), w the substorm's poleward push (deg).

const vec3 kAuroraAxisECEF = vec3(0.0481, -0.1543, 0.9868); // the dipole axis (== kGeomagPoleECEF)

struct AuroraGeom {
    float cosMlt;  // cos of the angle from the oval's widest point (~23 MLT): 1 there, -1 near 11 MLT
    float eqDeg;   // equatorward edge here (colatitude, deg)
    float polDeg;  // poleward edge here
    float boost;   // the substorm's local intensity (0 = none)
};

AuroraGeom auroraOvalGeom(vec3 dirECEF) {
    vec3  m = cloud.auroraMidnight.xyz;
    vec3  e = cross(kAuroraAxisECEF, m);                    // toward dawn (MLT increases eastward)
    float c = dot(dirECEF, m), s = dot(dirECEF, e);
    float r = max(length(vec2(c, s)), 1e-4);
    c /= r; s /= r;                                         // cos / sin of the MLT angle (0 = midnight)
    AuroraGeom g;
    // The widest point sits ~1 h before midnight (dusk side): cos(phi + 0.26).
    g.cosMlt = c * 0.9664 - s * 0.2571;
    float w = 0.5 + 0.5 * g.cosMlt;
    g.eqDeg  = mix(cloud.auroraOval.y, cloud.auroraOval.x, w);
    g.polDeg = mix(cloud.auroraOval.w, cloud.auroraOval.z, w);
    // Substorm bulge: a Gaussian in MLT angle around the onset (1 - cos d ~ d^2 / 2).
    float cd = c * cloud.auroraSub.y + s * cloud.auroraSub.z;
    float b  = exp((cd - 1.0) * cloud.auroraSub.w);
    g.boost  = cloud.auroraSub.x * b;
    g.polDeg = max(g.polDeg - cloud.auroraOval2.w * b, 2.0);
    g.eqDeg += 0.8 * g.boost;
    return g;
}

// The band's brightness at a colatitude (deg) shifted by the edge ripple (deg): a sharp poleward edge
// (discrete arcs end there), a soft equatorward one (the diffuse aurora), a dim dayside, the activity's
// brightness and the substorm's brightening.
float auroraOvalBand(AuroraGeom g, float colatDeg, float rippleDeg) {
    float cl    = colatDeg - rippleDeg;
    float inner = smoothstep(g.polDeg - 0.8, g.polDeg + 1.2, cl);
    float outer = 1.0 - smoothstep(g.eqDeg - 2.0, g.eqDeg + 3.5, cl);
    float mltB  = mix(0.3, 1.0, smoothstep(-0.2, 0.7, g.cosMlt));
    return inner * outer * mltB * cloud.auroraOval2.x * (1.0 + 1.5 * g.boost);
}

// The share of the band's light in DISCRETE arcs (the sheets): full in the poleward part of the oval,
// a quarter in its equatorward part, which is the diffuse aurora. From orbit a storm's oval was ~60
// evenly spaced sheets across 17 degrees: tree rings.
float auroraDiscreteShare(AuroraGeom g, float colatDeg) {
    float f = (colatDeg - g.polDeg) / max(g.eqDeg - g.polDeg, 1.0);
    return mix(0.25, 1.0, 1.0 - smoothstep(0.3, 0.75, f));
}
