#include "SatTrace.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

namespace
{
constexpr const char *kTraceFormat = "sat-light-sim-trace/1";
constexpr const char *kTraceColumns = "t_j2000,sun_alt_deg,elevation_deg,azimuth_deg,range_m,phase_deg,"
                                      "off_specular_deg,lit_factor,mag,m1000,extinction_mag,mag_apparent,"
                                      "dominant_lobe";

double sinElevationAt(const SatTraceSetup &s, double t)
{
    const glm::dvec3 sat = satOrbitStateAt(s.orbit, t).posEci;
    const glm::dvec3 obs = observerEciAt(s.obsDirEcef, s.obsRadiusM, t);
    return glm::dot(glm::normalize(sat - obs), glm::normalize(obs));
}

// Bisects a horizon crossing between a (below) and b (above), or the reverse, to 0.1 s.
double refineCrossing(const SatTraceSetup &s, double a, double b)
{
    const bool aUp = sinElevationAt(s, a) > 0.0;
    for (int i = 0; i < 40 && std::abs(b - a) > 0.1; ++i)
    {
        const double m = 0.5 * (a + b);
        ((sinElevationAt(s, m) > 0.0) == aUp ? a : b) = m;
    }
    return 0.5 * (a + b);
}

std::string fmtG(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string fmtVec(glm::dvec3 v) { return fmtG(v.x) + " " + fmtG(v.y) + " " + fmtG(v.z); }

bool parseVec(const std::string &s, glm::dvec3 &v)
{
    std::istringstream is(s);
    return (bool)(is >> v.x >> v.y >> v.z);
}

// A magnitude-like field: empty when dark (non-finite).
std::string fmtMag(double v)
{
    if (!std::isfinite(v))
        return "";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

double parseMag(const std::string &s) { return s.empty() ? INFINITY : std::strtod(s.c_str(), nullptr); }
} // namespace

SatTraceRow evalSatTraceRow(const SatTraceSetup &setup, const std::vector<AttitudeGroup> &groups,
                            const std::vector<GpuSatLobe> &lobes, const SatOcclusion *occ, double t)
{
    SatPhotInputs in;
    in.sunDirEci = sunDirEciAt(t);
    in.obsEci = observerEciAt(setup.obsDirEcef, setup.obsRadiusM, t);
    in.flareTiltRad = setup.flareTiltRad;
    const SatPhotResult r =
        evalSatPhotometry(groups, lobes, setup.orbit, t, in, nullptr, setup.occlusion ? occ : nullptr);

    SatTraceRow row;
    row.tJ2000 = t;
    const glm::dvec3 up = glm::normalize(in.obsEci);
    glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
    east = glm::length(east) > 1e-12 ? glm::normalize(east) : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 toSat = r.orbit.posEci - in.obsEci;
    row.sunAltDeg = glm::degrees(std::asin(glm::clamp(glm::dot(in.sunDirEci, up), -1.0, 1.0)));
    row.elevationDeg = glm::degrees(std::asin(glm::clamp(r.sinElevation, -1.0, 1.0)));
    double az = glm::degrees(std::atan2(glm::dot(toSat, east), glm::dot(toSat, north)));
    row.azimuthDeg = az < 0.0 ? az + 360.0 : az;
    row.rangeM = r.rangeM;
    row.phaseDeg = glm::degrees(r.phaseAngleRad);
    row.offSpecularDeg = glm::degrees(r.offSpecularRad);
    row.litFactor = r.litFactor;
    row.mag = r.supported ? r.magnitude : INFINITY;
    row.m1000 = r.supported ? r.magnitude1000 : INFINITY;
    row.extinctionMag = r.sinElevation > 0.0
                            ? atmExtinctionMag(in.obsEci, glm::normalize(toSat), r.rangeM, setup.extinctionK)
                            : INFINITY; // below the horizon: behind the Earth
    row.magApparent = row.mag + row.extinctionMag;
    row.dominantLobe = r.dominantLobe;
    return row;
}

void satTracePassWindow(const SatTraceSetup &s, double t0, double &tStart, double &tEnd)
{
    constexpr double kStep = 10.0;
    const double period = 2.0 * satphot::kPi *
                          std::sqrt(s.orbit.rSatM * s.orbit.rSatM * s.orbit.rSatM / satphot::kGM);
    const double maxPass = 0.5 * period; // no LEO pass is above the horizon for half an orbit
    double tUp = t0;
    if (sinElevationAt(s, t0) <= 0.0)
    {
        bool found = false;
        for (double t = t0 + kStep; t <= t0 + 2.0 * period; t += kStep)
            if (sinElevationAt(s, t) > 0.0)
            {
                tUp = refineCrossing(s, t - kStep, t);
                found = true;
                break;
            }
        if (!found)
        {
            tStart = t0 - 600.0;
            tEnd = t0 + 600.0;
            return;
        }
        tUp += 0.1; // just inside the pass
    }
    tStart = tUp - maxPass;
    for (double t = tUp - kStep; t >= tUp - maxPass; t -= kStep)
        if (sinElevationAt(s, t) <= 0.0)
        {
            tStart = refineCrossing(s, t, t + kStep);
            break;
        }
    tEnd = tUp + maxPass;
    for (double t = tUp + kStep; t <= tUp + maxPass; t += kStep)
        if (sinElevationAt(s, t) <= 0.0)
        {
            tEnd = refineCrossing(s, t - kStep, t);
            break;
        }
}

std::string satTraceFileHash(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return "unreadable";
    uint64_t h = 1469598103934665603ull;
    for (std::istreambuf_iterator<char> it(f), end; it != end; ++it)
    {
        h ^= (unsigned char)*it;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

std::string formatSatTraceRow(const SatTraceRow &r)
{
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%.6f,%.6f,%.6f,%.3f,%.6f,%.6f,%.6f,", r.sunAltDeg, r.elevationDeg,
                  r.azimuthDeg, r.rangeM, r.phaseDeg, r.offSpecularDeg, r.litFactor);
    return std::string(buf) + fmtMag(r.mag) + "," + fmtMag(r.m1000) + "," + fmtMag(r.extinctionMag) + "," +
           fmtMag(r.magApparent) + "," + std::to_string(r.dominantLobe);
}

bool writeSatTraceCsv(const std::string &path, const SatTraceSetup &s, const std::vector<SatTraceRow> &rows,
                      std::string &err)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
    {
        err = "cannot write " + path;
        return false;
    }
    const SatOrbitElems &o = s.orbit;
    f << "# format: " << kTraceFormat << "\n"
      << "# app_version: " << s.appVersion << "\n"
      << "# git_commit: " << s.gitCommit << "\n"
      << "# model: " << s.modelId << "\n"
      << "# model_hash: " << s.modelHash << "\n"
      << "# type: " << s.typeName << "\n"
      << "# satellite_index: " << s.satelliteIndex << "\n"
      << "# lobe_budget: " << s.lobeBudget << "\n"
      << "# occlusion: " << (s.occlusion ? 1 : 0) << "\n"
      << "# orbit_raan: " << fmtG(o.raan) << "\n"
      << "# orbit_incl: " << fmtG(o.incl) << "\n"
      << "# orbit_u0: " << fmtG(o.u0) << "\n"
      << "# orbit_r_sat_m: " << fmtG(o.rSatM) << "\n"
      << "# orbit_mean_motion: " << fmtG(o.meanMot) << "\n"
      << "# orbit_sso: " << (o.sso ? 1 : 0) << "\n"
      << "# orbit_raan_anchor_t: " << fmtG(o.raanAnchorT) << "\n"
      << "# orbit_tumble_rate: " << fmtG(o.tumbleRate) << "\n"
      << "# orbit_tumble_phase: " << fmtG(o.tumblePhase) << "\n"
      << "# orbit_tumble_axis: " << fmtVec(o.tumbleAxis) << "\n"
      << "# observer_dir_ecef: " << fmtVec(s.obsDirEcef) << "\n"
      << "# observer_radius_m: " << fmtG(s.obsRadiusM) << "\n"
      << "# flare_tilt_rad: " << fmtG(s.flareTiltRad) << "\n"
      << "# extinction_k: " << fmtG(s.extinctionK) << "\n"
      << "# note: t_j2000 is sim time (s since J2000); the sim's Earth rotation omits GMST at J2000, so it is "
         "not real UTC. Empty magnitude fields = dark (Earth's shadow) or below the horizon.\n"
      << kTraceColumns << "\n";
    for (const SatTraceRow &r : rows)
        f << fmtG(r.tJ2000) << "," << formatSatTraceRow(r) << "\n";
    if (!f)
    {
        err = "write failed: " + path;
        return false;
    }
    return true;
}

bool readSatTraceCsv(const std::string &path, SatTraceSetup &s, std::vector<SatTraceRow> &rows, std::string &err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    rows.clear();
    std::string line, format;
    bool sawColumns = false;
    int lineNo = 0;
    while (std::getline(f, line))
    {
        ++lineNo;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        if (line[0] == '#')
        {
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            const std::string key = line.substr(2, colon - 2);
            const std::string v = colon + 2 <= line.size() ? line.substr(colon + 2) : "";
            const double d = std::strtod(v.c_str(), nullptr);
            bool ok = true;
            if (key == "format") format = v;
            else if (key == "app_version") s.appVersion = v;
            else if (key == "git_commit") s.gitCommit = v;
            else if (key == "model") s.modelId = v;
            else if (key == "model_hash") s.modelHash = v;
            else if (key == "type") s.typeName = v;
            else if (key == "satellite_index") s.satelliteIndex = (int)d;
            else if (key == "lobe_budget") s.lobeBudget = (int)d;
            else if (key == "occlusion") s.occlusion = d != 0.0;
            else if (key == "orbit_raan") s.orbit.raan = d;
            else if (key == "orbit_incl") s.orbit.incl = d;
            else if (key == "orbit_u0") s.orbit.u0 = d;
            else if (key == "orbit_r_sat_m") s.orbit.rSatM = d;
            else if (key == "orbit_mean_motion") s.orbit.meanMot = d;
            else if (key == "orbit_sso") s.orbit.sso = d != 0.0;
            else if (key == "orbit_raan_anchor_t") s.orbit.raanAnchorT = d;
            else if (key == "orbit_tumble_rate") s.orbit.tumbleRate = d;
            else if (key == "orbit_tumble_phase") s.orbit.tumblePhase = d;
            else if (key == "orbit_tumble_axis") ok = parseVec(v, s.orbit.tumbleAxis);
            else if (key == "observer_dir_ecef") ok = parseVec(v, s.obsDirEcef);
            else if (key == "observer_radius_m") s.obsRadiusM = d;
            else if (key == "flare_tilt_rad") s.flareTiltRad = d;
            else if (key == "extinction_k") s.extinctionK = d;
            if (!ok)
            {
                err = path + ":" + std::to_string(lineNo) + ": bad value for " + key;
                return false;
            }
            continue;
        }
        if (!sawColumns)
        {
            if (line != kTraceColumns)
            {
                err = path + ":" + std::to_string(lineNo) + ": unexpected column header";
                return false;
            }
            sawColumns = true;
            continue;
        }
        std::vector<std::string> fld;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ','))
            fld.push_back(cell);
        if (line.back() == ',')
            fld.push_back("");
        if (fld.size() != 13)
        {
            err = path + ":" + std::to_string(lineNo) + ": expected 13 fields, got " + std::to_string(fld.size());
            return false;
        }
        SatTraceRow r;
        r.tJ2000 = std::strtod(fld[0].c_str(), nullptr);
        r.sunAltDeg = std::strtod(fld[1].c_str(), nullptr);
        r.elevationDeg = std::strtod(fld[2].c_str(), nullptr);
        r.azimuthDeg = std::strtod(fld[3].c_str(), nullptr);
        r.rangeM = std::strtod(fld[4].c_str(), nullptr);
        r.phaseDeg = std::strtod(fld[5].c_str(), nullptr);
        r.offSpecularDeg = std::strtod(fld[6].c_str(), nullptr);
        r.litFactor = std::strtod(fld[7].c_str(), nullptr);
        r.mag = parseMag(fld[8]);
        r.m1000 = parseMag(fld[9]);
        r.extinctionMag = parseMag(fld[10]);
        r.magApparent = parseMag(fld[11]);
        r.dominantLobe = std::atoi(fld[12].c_str());
        rows.push_back(r);
    }
    if (format != kTraceFormat)
    {
        err = path + ": format '" + format + "' is not " + kTraceFormat;
        return false;
    }
    if (s.modelId.empty())
    {
        err = path + ": no model";
        return false;
    }
    return true;
}
