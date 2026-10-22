---
pages: [simulation/photometry.md, rendering/satellite-meshes.md, modding/satellite-models.md, accuracy/results.md]
code: [src/simulations/SatModel.h, src/simulations/SatModel.cpp, shaders/sat_orbit.comp, shaders/sat_mesh.frag, src/simulations/SatMesh.cpp, tools/sat_model_tool/pass_eval.cpp, tools/video/vidkit/photometry.py, data/satellite_models/iridium_block1.json]
---
## Now
**The Sun's disc is a source term, not a fixed roughness.** A disc of fixed angular size maps onto half vectors as an
ellipse, R/2 in the plane of incidence and R/(2 cos θd) across it (θd = the angle between the Sun and the half vector;
dω_o = 4 (o·h) dω_h). Every lobe lit by the Sun therefore takes an anisotropic α² pair in the frame (p, q = s × o, h):
α_p² = α_m² + S·w(α_m²/S) and α_q² = α_m² + (S/c²)·w(α_m² c²/S), with c = s·h and S = kSunAlpha² (0.00208², the centre
of a limb-darkened disc, u = 0.6). w(x) = v + (1 − v)/(1 + k x) makes S·w the disc's share of the convolution's peak:
the disc's centre for a material sharper than the disc, its variance for a broad one (GGX v 1.125, k 4; Beckmann
v 0.5625, k 3). D is the anisotropic GGX / Beckmann of that pair; the masking term keeps α_p. One function per side
(SatModel.cpp `sunDiscA2`, GLSL `sunA2` in sat_orbit.comp and sat_mesh.frag, vidkit `sun_a2`); callers use
`satLobeIntensitySun()` for sunlight and `satLobeIntensity()` (an isotropic source folded into α²) for the Earth.
Measured against the material lobe convolved with a limb-darkened disc by brute force: peak within 0.006 mag (GGX)
and 0.036 mag (Beckmann) for incidence 0-75 deg and α 0-0.3; energy within 0.3% of the previous folding; α ≥ 0.05
changes ≤ 0.004 mag. A flat mirror's reflected beam is round, as the Sun's image is. Its half-maximum radius is
~0.15 deg against the disc's 0.26 deg: the kernel is GGX-shaped, with the peak and the energy matched.

**A material can have a haze**: JSON `"haze": {"fraction": f, "roughness": α}` (`SatMaterial::hazeFraction` /
`hazeRoughness`, α default 0.04). A share f of the specular reflectance goes into a second, broader lobe:
specular = F·((1 − f)·D_core + f·D_haze); diffuse and transmission are unchanged. The bake emits a hazy facet group as
two lobes with the same normal, area, date, stowing and occlusion samples: the core (specular area × (1 − f), all the
diffuse and transmission) and the haze (specular area × f, nothing else). Splitting the specular AREA, not F0, keeps
Schlick exact. A hazy lobe counts twice against the lobe budget. The mesh renderer evaluates both lobes per pixel
(`specCosMat`; the haze is packed into `GpuSatMeshMaterial`'s beckmann / pattern high halves), and only the core may
glare. `iridium_block1.json` uses it (0.3 at α 0.04 on the MMAs' silvered film) instead of splitting each panel by area.

**The pass replay's extinction starts at the observer's real height.** `SatModelTool --pass` places the observer on
WGS84 (vidkit `pass_spec`). The extinction's atmosphere is a 6371-km sphere, so it is evaluated from the observer's
geodetic height above that sphere, not from |r| − 6371 km. The output gains `obs_height_m`.

## History
- The old sun term was one isotropic α 0.0023 added to every lobe's α² (the uniform disc's peak at normal incidence).
  It kept the energy but was 1/cos θ too bright at oblique incidence (−0.26 mag at 50 deg, −0.95 at 70, −1.25 at 75),
  cos θ too narrow across the plane of incidence, and 0.22 mag faint at normal incidence against the limb-darkened
  centre. Found by the Iridium flare validation. Tried: an isotropic area-preserving α (same peak, wrong beam shape)
  and one α_s for both distributions (Beckmann then read 0.15-0.19 mag faint at α 0.001-0.003).
- Accuracy gate before / after: every gated value moved ≤ 0.001 mag (V1.0 sd 0.755 → 0.754). The benchmarks are
  dominated by diffuse and rough lobes. Iridium: low-elevation (oblique) flare centres 0.1-0.3 mag fainter; the
  Heavens-Above off-centre drop rms 0.40 → 0.37; the Lee/John bins and the MMT-9 wing unchanged (≤ 0.03 mag).
- The haze was an area split of each panel (70% core material, 30% haze material) while materials had one lobe:
  the same reflectance, with the occlusion samples spread over two strips.
- The pass extinction put a 54 N observer ~7 km underground (35 mag of air) and added ~0.23 mag at the MMT-9 site
  (2030 m): the three Starlink MMT replays' apparent baselines moved by −0.25 / −0.23 / −0.22 mag (−0.65 → −0.90,
  +0.60 → +0.37, +0.03 → −0.20; median model − observed), the median extinction 0.35 → 0.12 mag. BlueWalker 3's
  visual (Mallama) set: apparent-magnitude scatter 4.5 → 1.0 mag rms. Python's vidkit had no copy of the bug (it
  computes no extinction).
