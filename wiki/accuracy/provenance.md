# Model provenance

Every number in a satellite model comes from somewhere: a publication, a calculation from published numbers,
a fit to a benchmark, or an informed guess. A model file says which, part by part, in its `sources` block.
This page describes that block, the rule for reference models, and the confidence of every shipped model. The
geometry and material fields themselves are documented in [Satellite models](../modding/satellite-models.md).

## The `sources` block

`sources` is an array in the model file (`data/satellite_models/<id>.json`). Each entry covers one or more
**subjects**, which are names of the model's attitude groups, components or model-local materials, or
`attitude` for the pointing law as a whole:

```json
{
  "subjects": ["base_metal_side_a", "base_metal_side_b", "bus_top"],
  "status": "sourced",
  "value": "bus 1.3 x 2.8 m footprint, 0.2 m thick",
  "source": "Footprint: McDowell, planet4589.org/astro/starsim (V1.5 row: bus 2.8 x 1.3 m) ..."
}
```

`subject` (a single string) may be used instead of `subjects`. `value` states the number or choice exactly as
the model uses it, and `source` says where it comes from, with enough detail to find it: a URL, a paper with
section and table, or the derivation.

| `status` | Meaning | What `source` must contain |
|---|---|---|
| `sourced` | taken from a cited publication | the citation |
| `derived` | computed from sourced values plus a stated constraint | the inputs and the derivation |
| `estimate` | no source; an informed guess | why this value, and the plausible range where one is known |
| `calibrated` | fitted to a benchmark | the benchmark, the metric, the scan, and which checks stay held out |

Any other status makes the loader warn. Render-only components (decorative parts that do not enter the
photometry) need no entry, since they have no photometric numbers. Material presets shared by every model
(`solar_cell`, `white_paint`, and so on) are documented in the code where they are defined
(`satMaterialPresetsBase()` in `src/simulations/SatModel.cpp`); only materials defined inside a model file need
entries.

### Checking it

`unexplainedModelParts()` lists every group, photometric component and model-local material that no entry
covers. `SatModelTool <model.json>` prints, for each model, the counts by status, every entry that is not
`sourced`, and every unexplained part:

```text
  provenance: 4 sourced, 2 derived, 0 calibrated, 2 estimate; 0 part(s) unexplained
    estimate   antennas           white antenna region 0.9 x 2.2 m centred on the nadir face ...
```

So the estimates in a model are always one command away, which is the point of the exercise: a reader can see
at a glance which numbers a brightness prediction rests on that nobody has measured.

### The rule for reference models

A model that a benchmark is run against must have **zero unexplained parts**. Otherwise a benchmark result
would rest on numbers whose origin nobody can state. The rule is a project policy enforced by review: the
tool reports unexplained parts but does not fail on them. Every model that a benchmark names meets it.

A `calibrated` entry changes what its benchmark means. Once a value is fitted to a dataset, agreement with that
dataset shows only that the fit worked. The entry must therefore say which comparisons remain **held out**:
the ones that still test the model.

## Shipped models

Confidence is a summary of the `sources` blocks:

- **Reference**: built for a benchmark; every dimension sourced or derived; materials partly calibrated.
- **Parity, benchmarked**: built to stand in for a constellation in the roster, and compared against a
  published campaign.
- **Sourced geometry, estimated surfaces**: dimensions and layout sourced, reflectances estimated; no
  benchmark.
- **Low confidence**: a satellite or station that does not exist yet or whose design is unpublished; sizes from
  press and renders, surfaces guessed.
- **Representative**: not a particular object; a plausible example of a class (debris).

Counts are of entries (one entry may cover several parts): S sourced, D derived, C calibrated, E estimate.

| Model | Satellite | S / D / C / E | Confidence | Main sources | Benchmark |
|---|---|---|---|---|---|
| `starlink_v1_0` | Starlink V1.0 (original, pre-visor) | 4 / 2 / 0 / 2 | reference | McDowell's size table (planet4589.org); Cole 2021 (arXiv:2107.06026) for the polished-metal base, white antennas and edge-on array; SpaceX's shark-fin description | Mallama 2020a (held out) |
| `starlink_visorsat` | Starlink VisorSat | 5 / 2 / 1 / 2 | reference | as V1.0, plus Cole 2021's fitted array offset (24° from vertical) and 23° visor cutoff | Mallama 2021; differential |
| `starlink_v1_5` | Starlink V1.5 | 3 / 1 / 1 / 2 | parity, benchmarked | the V1.0 chassis; SpaceX's *Brightness Mitigation Best Practices* (2022) for the gen-1 mirror film and translucent backsheet | Mallama et al. 2025 |
| `starlink_v2_mini` | Starlink V2 Mini, mitigated | 3 / 1 / 0 / 0 | parity, benchmarked | McDowell (bus 4.1 × 2.7 m, arrays 4.1 × 12.8 m); SpaceX 2022 (gen-2 film, black paint, opaque backsheet); Mallama et al. 2023 (arrays edge-on to the limb) | Mallama et al. 2023 |
| `starlink_v2_mini_dtc` | Starlink Direct-to-Cell, mitigated | 2 / 2 / 0 / 2 | parity, benchmarked | as V2 Mini; Mallama et al. 2025 for the 5 × 5 m nadir phased array | Mallama et al. 2025 (held out) |
| `starlink_v3` | Starlink V3 | 1 / 1 / 0 / 2 | low confidence | SpaceX's V3 update and press (about 60 m span, 7–8 m base); the V2 Mini's mitigations at V3 scale | none |
| `oneweb` | OneWeb Gen1 (Airbus Arrow) | 1 / 1 / 1 / 2 | parity, benchmarked | eoPortal (1 × 1 × 1.3 m box, two arrays); span from Gunter's Space Page | Mallama 2020b |
| `amazon_leo` | Amazon Leo | 2 / 2 / 0 / 2 | parity, benchmarked | Gunter's Space Page (2 m bus, 10 m span); Mallama et al. 2026 (nadir antenna panel, tracking array) | Mallama et al. 2026 (held out) |
| `guowang` | Guowang | 1 / 1 / 0 / 2 | low confidence | China in Space and KeepTrack (3 × 1 × 1 m box, 10 m span); attitude and surfaces unpublished | Mallama et al. 2025 (not gated) |
| `iss` | International Space Station | 12 / 3 / 0 / 5 | sourced geometry, estimated surfaces | NASA ATCS overview (radiators, Z-93 coating); module and truss tables; NASA and Wikipedia for the solar wings and iROSAs; NASA body frame and XVV attitude | none |
| `tiangong` | Tiangong (three modules) | 2 / 4 / 0 / 5 | sourced geometry, estimated surfaces | Wikipedia and eoPortal (module sizes, T shape, wing span); China Space Report (two-axis wings); attitude estimated | none |
| `hubble` | Hubble Space Telescope | none | example, no `sources` | tube and array sizes in the model's note; attitude a stand-in (aperture away from the Sun) | none |
| `reflect_orbital` | Reflect Orbital mirror | 1 / 2 / 0 / 3 | low confidence | Earendil-1 (18 m demonstrator: aluminised membrane, ~5 km spot); the quoted 55 × 55 m full-scale mirror | none |
| `spacex_ai_sat` | SpaceX Starmind AI1 | 3 / 3 / 0 / 4 | low confidence | the AI1 specification as given by the project owner (70 m span, edge-on radiators, flat stacking bus); materials borrowed or estimated | none |
| `starship_depot` | Starship HLS propellant depot | 2 / 1 / 0 / 3 | low confidence | Starship's 9 m diameter; HLS's body-mounted solar band; length, attitude and skin estimated | none |
| `haven1` | Vast Haven-1 | 1 / 0 / 0 / 5 | low confidence | Wikipedia (10.1 × 4.4 m module); arrays sized to the roster type's cross-section | none |
| `haven2` | Vast Haven-2 | 0 / 0 / 0 / 6 | low confidence | press (module about 15 m); layout and arrays estimated | none |
| `axiom_station` | Axiom Station | 0 / 0 / 0 / 6 | low confidence | press (11 × 4.2 m first module) | none |
| `orbital_reef` | Orbital Reef | 0 / 0 / 0 / 6 | low confidence | renders (inflatable habitat about 8 m) | none |
| `starlab` | Starlab | 1 / 0 / 0 / 5 | low confidence | Wikipedia (8 m module, 60 kW); arrays sized from the power | none |
| `ross` | Russian Orbital Service Station | 0 / 0 / 0 / 6 | low confidence | press; module sizes after the Zvezda/Nauka class | none |
| `bharatiya_station` | Bharatiya Antariksh Station | 0 / 0 / 0 / 6 | low confidence | ISRO press (BAS-1 about 3.8 × 8 m) | none |
| `debris_fragment` | Debris fragment (tumbling) | 1 / 0 / 0 / 4 | representative | a folded panel and stub sized to 0.05 m² mean cross-section; uncontrolled tumble | none |

The future stations share one scheme: modules stacked along the velocity vector, flown in the ISS's attitude,
with two solar wings on a cross-track mast tracking the Sun, each sized so the model's cross-section matches
the roster type it replaced. Their brightness is a render-level estimate and should be read as "a station of
roughly this size", not a prediction of any real station.

!!! note "Hubble"
    `hubble.json` is an example model and carries no `sources` block, so every one of its parts is
    unexplained. Its attitude is a stand-in: the simulator has no inertial-pointing law, so the aperture
    points away from the Sun, which respects the telescope's Sun-avoidance limit and lets the arrays face the
    Sun. It is not a reference model.

## What is calibrated, and against what

Three values in the model files are fitted, and one value of a shared material preset:

| Value | Fitted to | Metric | Result | Still held out |
|---|---|---|---|---|
| `solar_cell` preset diffuse albedo: 0.02 | Mallama 2021 (VisorSat) | low-phase bins (40–60°, where the array face dominates) | consistent with anti-reflection-coated cells, whose few percent reflectance is mostly the cover glass's specular | Mallama 2020a mean, the differential |
| VisorSat visor size: inset 0.55 m, gap 0.233 m | Mallama 2021 (VisorSat) | phase-curve RMS, inset scanned 0.2–0.9 m at Cole's 23° cutoff (gap = inset × tan 23°) | RMS minimum about 0.14 mag at 0.55 m | Mallama 2020a mean, the differential |
| OneWeb bus MLI: albedo 0.1, F0 0.25, roughness 0.35 | Mallama 2020b (OneWeb) | mean \( m_{1000} \) | the gold-foil preset reads 6.49, dark MLI 6.97 against 7.18 | nothing |
| V1.5 backsheet diffuse transmission: 0.03 | Mallama & Respler 2022, Post-VisorSat phase function | the 100–160° phase bins | an opaque backsheet leaves them 2.7–3.9 mag too faint; 0.03 brings them within 0.6 | nothing from that paper; the V1.5 campaign of Mallama et al. 2025 is a separate dataset |

Two further choices were made with a benchmark in view but are not fitted values. The dielectric mirror films
use a Beckmann distribution (with GGX the V2 Mini reads 7.43 against 7.87); the polished Starlink bus uses GGX
(Beckmann makes 70–110° phase 0.3–0.5 mag too faint once earthshine is modelled correctly). Both choices are
recorded in the presets' comments and in [Results](results.md#fitted-and-estimated-values).

Everything else in the shared presets is an initial estimate. The held-out comparisons that test the
calibrated materials are therefore: Starlink V1.0 (the VisorSat chassis without the visor), the
Direct-to-Cell campaign (the V2 Mini's materials at another altitude with another antenna) and Amazon Leo.

## Where in the code

- `src/simulations/SatModel.h/.cpp`: `SatModelSource`, the `sources` parser in `loadSatModel()`,
  `unexplainedModelParts()`, `satMaterialPresetsBase()`.
- `tools/sat_model_tool/main.cpp`: the provenance printout.
- `data/satellite_models/*.json`: the models and their `sources` blocks.
