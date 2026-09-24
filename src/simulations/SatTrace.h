#pragma once
// ── SatTrace — one satellite's magnitude over a time window (benchmarking milestone M9) ───────────
// The in-app "Trace" instrument and its CSV export, and SatModelTool --replay-trace, which re-runs an
// exported CSV and checks every row comes out the same. Everything here is the CPU photometric
// evaluator (SatPhotometry) in double precision — valid because orbit and attitude are closed-form
// functions of sim time — so nothing reads back from the GPU.
//
// A trace file is self-describing: its header carries every input the evaluator used (model id and
// file hash, lobe budget, occlusion, the orbit elements exactly as the app baked them, the observer,
// the flare-mitigation tilt, the extinction coefficient), so a replay needs nothing from the app.
// Rows follow the design page's export convention: m (physical, above the atmosphere), m1000, the
// apparent magnitude after extinction, the off-specular angle B, the phase angle θ, range and
// elevation. A dark satellite (Earth's shadow) leaves its magnitude fields empty.

#include "SatPhotometry.h"

#include <string>
#include <vector>

struct SatTraceSetup
{
    std::string modelId;          // satellite_models/<modelId>.json
    std::string modelHash;        // FNV-1a of that file (satTraceFileHash), to catch a changed model
    std::string typeName;         // informational
    int satelliteIndex = -1;      // informational (the app's roster index)
    int lobeBudget = 48;          // the budget the app baked this type with (roster-size dependent)
    bool occlusion = false;       // Phase 3b occlusion between parts applied (exactly, no flux floor)
    SatOrbitElems orbit;
    glm::dvec3 obsDirEcef{0.0, 0.0, 1.0}; // observer's Earth-fixed unit direction
    double obsRadiusM = satphot::kEarthRadiusM;
    double flareTiltRad = 0.0;
    double extinctionK = 0.0;     // sea-level zenith extinction, mag (the "Extinction" slider)
    std::string appVersion, gitCommit; // informational
};

struct SatTraceRow
{
    double tJ2000 = 0.0;
    double sunAltDeg = 0.0;       // Sun altitude at the observer
    double elevationDeg = 0.0;    // satellite above the observer's geocentric horizon
    double azimuthDeg = 0.0;      // clockwise from north
    double rangeM = 0.0;
    double phaseDeg = 0.0;
    double offSpecularDeg = 0.0;
    double litFactor = 0.0;       // 1 = full sunlight
    double mag = 0.0;             // above-atmosphere apparent magnitude (+inf: dark)
    double m1000 = 0.0;           // reduced to 1000 km
    double extinctionMag = 0.0;   // along the line of sight to the satellite
    double magApparent = 0.0;     // mag + extinctionMag
    int dominantLobe = -1;
};

// Evaluates one row. `lobes`/`occ` are the type's baked lobes and occlusion data (occ may be null
// when setup.occlusion is false).
SatTraceRow evalSatTraceRow(const SatTraceSetup &setup, const std::vector<AttitudeGroup> &groups,
                            const std::vector<GpuSatLobe> &lobes, const SatOcclusion *occ, double tJ2000);

// The pass around t0: if the satellite is above the horizon at t0, from its rise to its set;
// otherwise the next pass within two orbital periods; failing both, t0 ± 10 minutes. Edges are
// refined to 0.1 s.
void satTracePassWindow(const SatTraceSetup &setup, double t0, double &tStart, double &tEnd);

// FNV-1a 64 of a file's bytes as 16 hex digits ("unreadable" if it cannot be opened) — the same
// hash SatBench reports carry for model files.
std::string satTraceFileHash(const std::string &path);

// CSV format "sat-light-sim-trace/1": '#'-prefixed "key: value" header lines, then a column header
// and one row per sample. Inputs are written with full precision (%.17g), outputs with %.6f.
bool writeSatTraceCsv(const std::string &path, const SatTraceSetup &setup, const std::vector<SatTraceRow> &rows,
                      std::string &err);
bool readSatTraceCsv(const std::string &path, SatTraceSetup &setup, std::vector<SatTraceRow> &rows,
                     std::string &err);
// One row's output fields exactly as the CSV writes them (the replay compares these strings).
std::string formatSatTraceRow(const SatTraceRow &r);
