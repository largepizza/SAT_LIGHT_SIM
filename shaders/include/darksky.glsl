// ─────────────────────────────────────────────────────────────────────────────
// darksky.glsl — shared exposure/contrast gate for faint diffuse sky features.
//
// Replaces the per-object `× (1 - domeVal * kXPollutionMaxDim)` multipliers that the Milky Way,
// zodiacal light and aurora each carried their own hand-tuned ceiling for. Those had two
// structural problems that no amount of retuning fixes:
//
//  1. A single scalar multiplying a whole feature can only dim it UNIFORMLY. The real
//     rural→suburban transition is not "the Milky Way at 30% everywhere", it is "the galactic
//     core still shows, the faint arms are gone" — a PER-SAMPLE threshold against each sample's
//     own surface brightness. That is what this file's darkSkySurfMag/darkSkyVis pair produces.
//  2. The Milky Way specifically was gated on a single non-directional scalar (the old
//     mwSuppressEased, driven by a MAX over all 16 dome sectors), so a city on the north horizon
//     suppressed the dark southern zenith identically. Everything here is per-direction.
//
// The model is the standard astronomical one, in magnitudes per square arcsecond (LARGER = FAINTER,
// the usual convention): estimate the sky background's surface brightness for this view direction,
// estimate the feature's own surface brightness at this sample, and let visibility fall off with
// the difference. Because both sides are per-sample, a bright feature survives a background that a
// faint one does not — for free, with no per-object ceiling to tune.
//
// Working in magnitudes (rather than a linear contrast ratio) is deliberate: the underlying signal
// spans orders of magnitude, the rest of this codebase already speaks magnitudes for stars,
// satellites and planets (see updateStars()/sat_flare.comp), and Bortle sky classes are themselves
// defined in exactly these units — so the constants below are checkable against reality rather
// than being free-floating fudge factors.
//
// Deliberately NOT driven by the pre-tonemap HDR sky luminance in sat_sky.frag, even though that
// is available there and is angularly finer than the 16-sector dome: it already contains twilight
// and the moon halo, both of which each consumer ALSO multiplies by separately
// (nightFactorEff*, moonBright*), so folding it in would double-count them — and cloud_march.comp
// has no equivalent quantity at all, which would leave aurora on a different sky model from the
// Milky Way. One analytic model, shared by both shaders, keeps them coherent by construction.
//
// All-scalar and loop-free by design: the shared-code hazard documented in CLAUDE.md
// ("Shader #include" → optDepth) is about turning a compile-time loop bound into a runtime
// parameter and losing the unroll. Nothing here has a loop to unroll.
// ─────────────────────────────────────────────────────────────────────────────
// REQUIRES, declared before this file is included:
//   * cloud_params.glsl — darkSkyTwilightMag reads cloud.darkSkyTwilightMag0 / darkSkyTwilightEndDeg /
//     darkSkyTwilightAniso directly rather than taking parameters at every call site.
//   * a `LightDomeBuf` block exposing `float lightDomeEased[16]` (the skyglow's directional excess per sector) and
//     `vec4 skyglowInfo` (x = its zenith ratio) — sat_sky.frag, cloud_march.comp and comet_tail.* declare it above
//     their include of this file.
#ifndef DARKSKY_GLSL
#define DARKSKY_GLSL

// Sky background surface brightness of a pristine site, mag/arcsec^2 — Bortle 1 zenith is
// ~21.9-22.0 in reality (Falchi et al.'s natural sky, 174 ucd/m^2). Artificial skyglow adds to it: darkSkyArtRatio.
const float kSkyMagPristine = 22.0;

// Response curve, in magnitudes of (sky background - feature surface brightness). Positive means
// the feature is intrinsically brighter than the sky it sits on; real naked-eye detection of a
// large diffuse feature survives a fair way below zero (the eye integrates over a big solid
// angle), which is why the knee is negative.
//
// This is a LOGISTIC, deliberately, replacing a smoothstep between a -2.4/-0.4 pair. The
// smoothstep was wrong in two ways that showed up immediately in-app:
//
//  * It CLIPS at both ends. Everything above the window kept its full brightness while everything
//    below was forced to exactly zero — a hard histogram cut. Since a feature's own texel range
//    maps straight onto that window, the gate acted as a maximum-strength contrast STRETCH: the
//    Milky Way got *sharper* as it got dimmer, ending as a hard-edged core on black instead of a
//    soft glow. Measured on the shipped constants, the gate alone multiplied the core/faint ratio
//    by 7.9x at rural and by infinity (a real zero) at suburban, on top of the texture's own 16.7x.
//  * A smoothstep's window is a hard interval, so once the sky background slid ~2 magnitudes the
//    feature went from untouched to fully gone. That is the "cuts off sharply on a marginally
//    brighter skyglow" failure.
//
// A logistic has no clamps at either end and no knee, so brightening the sky always scales the
// whole feature smoothly. kVisSharpness x the caller's `spread` is what sets the residual contrast
// stretch — the two are coupled, do not retune one alone.
// Twilight's own faint end. The twilight term is an ADDITIVE light source on top of the natural
// background, so it must fade to nothing, NOT to kSkyMagPristine — that floor already comes from
// darkSkyPollutionMag, and mixing toward it here both double-counted it and (once the anisotropy
// pushed past it) produced an antisolar sky DARKER than a pristine site, which is not a thing.
// 26 rather than something enormous because the ramp to it is what sets the curve's slope in the
// range that matters. Against real measured zenith twilight — 12 at sunset, ~16 at -6 deg, ~19.5
// at -12, ~21.9 at -18 — the defaults give 12 / 16.7 / 21.3 / ~22: right at both ends, and
// half a magnitude to a magnitude conservative (dark) through nautical twilight, which is the
// safe direction to err given the symptom being fixed.
const float kTwilightFloorMag = 26.0;

const float kVisSharpness = 1.40;
const float kVisKnee      = -0.60;

// ── Artificial sky brightness (2026-10-28, the skyglow model) ────────────────
// The CPU (SatelliteSim::updateLightPollutionDome, CLAUDE.md "Light Pollution Dome") sums every lit texel of the night
// map within 300 km with a Walker's-law kernel and gives, as the ratio of ARTIFICIAL to natural sky brightness (Falchi
// et al. 2016's convention): skyglowInfo.x = Z, the zenith; lightDomeEased[16] = D, each compass sector's directional
// excess toward its horizon (the share of the glow from sources far enough to be seen as a dome in their direction).
// Both eased, both x "Skyglow" (photometry.skyglow_gain). In a direction:
//     ratio = Z x iso(el) + D(az) x dome(el)
// iso: the isotropic part brightens toward the horizon as the air path grows (1 at the zenith, 1.8 at the horizon: the
// old 0.4 + 0.6 elevFalloff shape, normalised to the zenith); dome: the horizon-hugging part, 0 at the zenith (so the
// zenith has no azimuth seam), 1 at the horizon. Calibrated against 16 sites with published zenith sky brightness
// (Paranal .. Manhattan, 0.28 mag rms; the table is in CLAUDE.md). Sky = natural x (1 + ratio).
float darkSkyArtRatio(vec3 dir) {
    float az      = mod(atan(dir.x, dir.y) + 6.283185307, 6.283185307);
    float secF    = az * (16.0 / 6.283185307) - 0.5;
    int   sec0    = int(floor(secF));
    float secFrac = secF - float(sec0);
    int   sec0w   = ((sec0 % 16) + 16) % 16;
    int   sec1w   = (sec0w + 1) % 16;
    float D       = mix(lightDomeEased[sec0w], lightDomeEased[sec1w], secFrac);
    float ef      = 0.35 / (max(dir.z, 0.0) + 0.35);           // 1 at the horizon, 0.259 at the zenith
    float iso     = (0.4 + 0.6 * ef) * (1.0 / 0.5556);
    float dome    = max(ef - 0.2593, 0.0) * (1.0 / 0.7407);
    return max(skyglowInfo.x, 0.0) * iso + max(D, 0.0) * dome;
}
// Its sky brightness alone, mag/arcsec^2.
float darkSkyPollutionMagPhys(vec3 dir) {
    return kSkyMagPristine - 2.5 * log(1.0 + darkSkyArtRatio(dir)) / log(10.0);
}

// ── Twilight ─────────────────────────────────────────────────────────────────
// The sun's own contribution to sky background brightness. Until this existed, the only solar
// gate on the Milky Way was nightFactorEffSky's clamp(-sunDirENU.w * 5, 0, 1), which saturates
// at sin(el) = -0.2 — the sun only 11.5 deg down, short of even the end of nautical twilight —
// so the Milky Way reached full brightness against a sky that was still visibly bright.
//
// Two parts, and the directional one is the point:
//   * base — brightness at the sun's current depression, linear in DEGREES of depression, which
//     is the standard shape for twilight (each degree the sun sinks costs a roughly fixed number
//     of magnitudes). Reaches pristine at cloud.darkSkyTwilightEndDeg, 18 deg by default: the real
//     end of astronomical twilight, not a fudge factor.
//   * anisotropy — real twilight is an ARCH low on the sun's side while the antisolar sky is
//     already dark, which is exactly the "terminator crossing the zenith" the old non-directional
//     ramp could not express. Keyed on dot(dir, sunDir), so it falls out for free: with the sun
//     12 deg down at a pristine site, the western sky sits near 19.9 mag/arcsec^2, the zenith
//     near 20.9 and the eastern horizon near 21.4 — so the Milky Way emerges in the EAST while
//     the sunset side is still washed out, instead of everywhere at once.
//
// Nothing here is gated on the sun being below the horizon: in daylight it simply returns a very
// bright sky, which is the correct answer, and nightFactorEff* already zeroes these features then.
float darkSkyTwilightMag(vec3 dir, vec3 sunDirENU, float sunSinEl) {
    float sunElDeg = degrees(asin(clamp(sunSinEl, -1.0, 1.0)));
    float t = clamp(-sunElDeg / max(cloud.darkSkyTwilightEndDeg, 0.1), 0.0, 1.0);
    float base = mix(cloud.darkSkyTwilightMag0, kTwilightFloorMag, t);
    // CENTERED on prox = 0.5 (perpendicular to the sun, i.e. roughly the zenith while the sun is
    // near the horizon), so `base` is a zenith brightness and the anisotropy brightens the solar
    // side and darkens the antisolar side by half its value each. Anchoring at prox = 1 instead
    // made `base` the brightest point in the sky and pushed the ZENITH a magnitude and a half
    // darker than the curve was calibrated for, which delayed the whole effect.
    float prox = 0.5 + 0.5 * dot(dir, sunDirENU);
    return base + cloud.darkSkyTwilightAniso * (0.5 - prox);
}

// ── Combined sky background ──────────────────────────────────────────────────
// City glow and twilight are independent light sources, so they ADD as brightnesses — which in magnitudes means going
// through linear and back, not min() or a mix(). The constant is 0.4*ln(10).
//
// ONE physical sky for every dark-sky feature (2026-10-28): the Milky Way, the zodiacal light, the aurora and the
// comets all read darkSkySkyMag. Until then the Milky Way and zodiacal light read a LOG ramp of the dome from 1e-6 of
// the night map's brightness toward an artistic 13 mag/arcsec^2 (`darkSkyDomeT`, deleted): a raw of 1e-3 was already
// 61% of the way to "city", so a dark site within tens of km of a village (La Palma's observatory, a Bortle 4 field
// south-west of Santiago) read 16-18 mag/arcsec^2 and the Milky Way was a ghost or gone. darkSkySkyMagLinear (the
// aurora's name for it since 2026-10-14) is the same function.
float darkSkySkyMag(vec3 dir, vec4 sunDirENU) {
    float a = darkSkyPollutionMagPhys(dir);
    float b = darkSkyTwilightMag(dir, sunDirENU.xyz, sunDirENU.w);
    const float kM = 0.4 * 2.302585093;
    return -(1.0 / kM) * log(exp(-kM * a) + exp(-kM * b));
}
float darkSkySkyMagLinear(vec3 dir, vec4 sunDirENU) {
    return darkSkySkyMag(dir, sunDirENU);
}


// ── Feature surface brightness ───────────────────────────────────────────────
// Converts a sample's own linear luminance to mag/arcsec^2 against a (refLum -> refMag) anchor.
// `spread` is the magnitudes-per-decade slope. The physical value is 2.5, but these features are
// artistic/tuned rather than radiometric (the Milky Way panorama in particular is a stretched
// photograph whose faint regions sit relatively brighter than the real sky), so a shallower slope
// keeps a texture's internal dynamic range mapped onto the real feature's much narrower one.
//
// It is also the OTHER half of the contrast-stretch control: what the gate does to a feature's
// internal contrast is governed by kVisSharpness * spread, so these two are coupled and must be
// retuned together. Callers pass 0.85, down from an original 1.6 — with the old smoothstep that
// value put a feature's whole texel range across the response window at once, which is what made
// the Milky Way sharpen as it dimmed.
float darkSkySurfMag(float objLum, float refLum, float refMag, float spread) {
    return refMag - spread * (log(max(objLum, 1e-9) / refLum) / log(10.0));
}

// ── Visibility ───────────────────────────────────────────────────────────────
// The whole point: this is a function of the FEATURE's own per-sample brightness, so as the sky
// brightens the faint parts of a feature fade faster than the bright parts, and the feature thins
// from the outside in instead of dimming as a block.
//
// Normalized by the response this same sample would get under a PRISTINE sky, which is what makes
// the result read as "how much of this feature's dark-sky appearance survives the actual sky."
// Two properties follow, and both matter:
//   * At skyBgMag == kSkyMagPristine it returns exactly 1.0 for EVERY sample, so a genuinely dark
//     sky is bit-identical to having no gate at all. A bare logistic asymptotes to 1 without
//     reaching it, which dimmed and flattened the Milky Way even at a perfect site.
//   * It bounds the contrast stretch. Bright samples have a larger pristine response, so dividing
//     by it holds back exactly the samples that would otherwise run away — the core/faint ratio
//     the gate contributes now goes 1.0x (pristine) -> 1.5x (rural) -> 2.2x (suburban) -> 3.1x
//     (city) instead of 1.2x -> 7.9x -> infinity.
// The clamp only guards skies BRIGHTER than pristine (none exist today) and costs nothing.
float darkSkyVis(float objMag, float skyBgMag) {
    float resp    = 1.0 / (1.0 + exp(-kVisSharpness * ((skyBgMag - objMag) - kVisKnee)));
    float respRef = 1.0 / (1.0 + exp(-kVisSharpness * ((kSkyMagPristine - objMag) - kVisKnee)));
    return clamp(resp / max(respRef, 1e-4), 0.0, 1.0);
}

#endif // DARKSKY_GLSL
