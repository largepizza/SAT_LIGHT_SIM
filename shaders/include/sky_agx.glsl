// The sky pass's tonemap (sat_sky.frag; moved here unchanged 2026-11-21 so the SKY_LITE variant, which declares only the
// two members of the clouds v2 UBO it reads, shares the one copy). Needs `cv2.tone` in scope.
#ifndef SKY_AGX_GLSL
#define SKY_AGX_GLSL
// AgX (2026-10-26; Troy Sobotka's, in Benjamin Wrensch's polynomial fit): the tonemap. 1 - e^-x per channel saturates
// each channel on its own: bright orange skewed yellow, every sunlit cloud flattened to paper white (the physical
// multiple scattering put lit cloud past that). AgX maps scene light to a log curve in a wider gamut (an inset of the
// primaries), so highlights roll off to white and keep their hue on the way. Mid grey 0.18 -> ~0.18 (the old curve gives
// 0.165 there), so the exposure means what it did. Look: power (contrast) and saturation on the display-encoded curve.
// Out: display-linear (the swapchain encodes sRGB).
vec3 skyAgX(vec3 v)
{
    const mat3 kIn  = mat3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
                           0.0784335999999992, 0.878468636469772, 0.0784336,
                           0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const mat3 kOut = mat3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
                           -0.0980208811401368, 1.15190312990417, -0.0980434501171241,
                           -0.0990297440797205, -0.0989611768448433, 1.15107367264116);
    const float minEv = -12.47393, maxEv = 4.026069;
    // "AgX range" (cv2.tone.w, light3 2026-11-05): the scene's log contrast about mid grey x w before the curve, i.e. the
    // curve spans 16.5 EV / w of scene light. AgX's 16.5 EV is a camera's whole latitude; a sunset cumulus's lit / shaded
    // 4.5 EV sat in its flat middle, so the shaded faces read khaki and the lit ones cream (3.2 : 1 on screen against
    // 23.6 : 1 in the scene). Mid grey stays put, so the exposure means what it did. Below mid grey the extra contrast
    // saturates 1.5 EV down (tanh: the curve stays smooth): at 1.4 x everywhere the ground at a low Sun went black.
    const float kMidEv = -2.473931;   // log2(0.18)
    vec3 dEv = log2(max(kIn * v, vec3(1e-10))) - kMidEv;
    dEv += (cv2.tone.w - 1.0) * mix(1.5 * tanh(dEv / 1.5), dEv, step(0.0, dEv));
    v = clamp(kMidEv + dEv, minEv, maxEv);
    v = (v - minEv) / (maxEv - minEv);
    vec3 v2 = v * v, v4 = v2 * v2;
    v = 15.5 * v4 * v2 - 40.14 * v4 * v + 31.96 * v4 - 6.868 * v2 * v + 0.4298 * v2 + 0.1191 * v - 0.00232;
    float l = dot(v, vec3(0.2126, 0.7152, 0.0722));
    v = pow(max(v, vec3(0.0)), vec3(cv2.tone.z));
    v = vec3(pow(max(l, 0.0), cv2.tone.z)) + cv2.tone.y * (v - vec3(pow(max(l, 0.0), cv2.tone.z)));
    return pow(clamp(kOut * v, 0.0, 1.0), vec3(2.2));
}
#endif
