// SatelliteSimStars.cpp — the star catalogue's metadata, selectable stars and the scripted 2037 Betelgeuse supernova
// (2026-10-27; CLAUDE.md "Stars").
//
// The catalogue is assets/stars/star_catalog.bin (tools/make_star_catalog.py: the HYG database v4.1 to V 8.5, ~62k
// stars, brightest first, with IAU proper names, Bayer / Flamsteed designations, spectral types, constellations and
// distances); without it the compiled-in BSC5 set (star_catalog.h, 8404 stars to 6.5, no names). updateStars
// (SatelliteSim.cpp) draws a PREFIX of it each frame: the stars that can show at all under this frame's sky.
//
// A selected star is selectedPlanetIndex = kPlanetCount + its index, so everything that clears a planet selection
// clears a star's too; the panel, the harness and the Sky events read it through skyObjectEnu.
//
// The supernova: Betelgeuse (168 pc, Joyce et al. 2020; A_V ~0.5) explodes as a Type II-P on 2037-12-25 03:00 UTC.
// Its V light curve (supernovaMagAt) is a pure function of sim time: shock breakout (minutes, to -6), the cooling
// envelope (-7 by 6 h), the rise to a peak of -11.2 a week later (M_V ~ -17.8, the bright end of II-P), the
// hydrogen-recombination plateau slowly fading to -10.2 over ~3 months, the drop off the plateau (3.2 mag in 25 days)
// and the 56Co tail (0.98 mag / 100 days, steeper after ~400 days as dust forms); B-V from -0.35 at breakout to ~1.4
// on the tail (H-alpha). While it is brighter than mag -1 it is drawn as a kFireBuf point (point, bloom, glare: the
// path the Moon-base glints use) and the star record is dark; it is visible by day near its peak, dims faint stars
// like moonlight, and adds its light to the night's ground and cloud ambient ("Night sky light").
#include "../FrameProfile.h"
#include "../Harness.h"
#include "../Log.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "star_catalog.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
std::string lowerStr(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}
double ss01(double x)
{
    x = std::clamp(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
const char *colourWord(float bv)
{
    if (bv < -0.1f) return "blue";
    if (bv < 0.15f) return "blue-white";
    if (bv < 0.45f) return "white";
    if (bv < 0.7f) return "yellow-white";
    if (bv < 1.0f) return "yellow";
    if (bv < 1.4f) return "orange";
    return "orange-red";
}
} // namespace

// ── The catalogue ──────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::loadStarCatalog()
{
    starInfo_.clear();
    starRaDec_.clear();
    starStrings_.assign(1, '\0');
    starCatalogFromFile = false;
    std::ifstream f("assets/stars/star_catalog.bin", std::ios::binary);
    if (f)
    {
        char magic[4];
        uint32_t hdr[3] = {};
        f.read(magic, 4);
        f.read(reinterpret_cast<char *>(hdr), sizeof(hdr));
        if (f && std::memcmp(magic, "SLSC", 4) == 0 && hdr[0] == 1u && hdr[1] > 0u && hdr[1] < 10000000u)
        {
            struct Rec
            {
                float ra, dec, vmag, bv, dist;
                uint32_t hip, hr, name, desig, spect;
                char con[4];
                uint32_t conName;
            };
            static_assert(sizeof(Rec) == 48, "star_catalog.bin record");
            std::vector<Rec> recs(hdr[1]);
            f.read(reinterpret_cast<char *>(recs.data()), (std::streamsize)(recs.size() * sizeof(Rec)));
            std::string blob(hdr[2], '\0');
            f.read(blob.data(), (std::streamsize)blob.size());
            if (f && !blob.empty())
            {
                starStrings_ = std::move(blob);
                starStrings_.push_back('\0');
                starInfo_.reserve(recs.size());
                starRaDec_.reserve(recs.size());
                for (const Rec &r : recs)
                {
                    StarInfo s;
                    s.vmag = r.vmag;
                    s.bv = r.bv;
                    s.distPc = r.dist;
                    s.hip = r.hip;
                    s.hr = r.hr;
                    s.name = r.name;
                    s.desig = r.desig;
                    s.spect = r.spect;
                    s.conName = r.conName;
                    std::memcpy(s.con, r.con, 3);
                    s.con[3] = '\0';
                    s.variable = (r.con[3] & 1) != 0;
                    starInfo_.push_back(s);
                    starRaDec_.emplace_back(r.ra, r.dec);
                }
                starCatalogFromFile = true;
            }
        }
    }
    if (!starCatalogFromFile)
    {
        Log::line("stars: assets/stars/star_catalog.bin missing or unreadable: the compiled-in BSC5 set (no names)");
        starStrings_.assign(1, '\0');
        for (const StarEntry &e : kStarCatalog)
        {
            StarInfo s;
            s.vmag = e.vmag;
            s.bv = e.bv;
            starInfo_.push_back(s);
            starRaDec_.emplace_back(e.ra_deg, e.dec_deg);
        }
    }
    starBetelgeuse = -1;
    for (size_t i = 0; i < starInfo_.size(); ++i)
        if (starInfo_[i].hip == 27989u)
            starBetelgeuse = (int)i;
    size_t named = 0;
    for (const StarInfo &s : starInfo_)
        named += s.name != 0;
    Log::line("stars: " + std::to_string(starInfo_.size()) + " stars to mag " +
              std::to_string(starInfo_.empty() ? 0.0f : starInfo_.back().vmag).substr(0, 4) + ", " + std::to_string(named) +
              " named" + (starBetelgeuse >= 0 ? ", Betelgeuse #" + std::to_string(starBetelgeuse) : std::string()));
    return starCatalogFromFile;
}

std::string SatelliteSim::starDisplayName(int i) const
{
    if (i < 0 || i >= (int)starInfo_.size())
        return "?";
    const StarInfo &s = starInfo_[(size_t)i];
    if (s.name) return starStr(s.name);
    if (s.desig) return starStr(s.desig);
    if (s.hip) return "HIP " + std::to_string(s.hip);
    return "Star #" + std::to_string(i);
}

int SatelliteSim::starIndexByName(const std::string &nameIn) const
{
    const std::string n = lowerStr(nameIn);
    if (n.empty()) return -1;
    auto numAfter = [&](const char *pre, uint32_t &v) {
        const size_t L = std::strlen(pre);
        if (n.size() <= L || n.compare(0, L, pre) != 0) return false;
        v = (uint32_t)std::strtoul(n.c_str() + L, nullptr, 10);
        return v != 0;
    };
    uint32_t num = 0;
    const bool hip = numAfter("hip ", num) || numAfter("hip", num);
    uint32_t hrN = 0;
    const bool hr = !hip && (numAfter("hr ", hrN) || numAfter("hr", hrN));
    for (size_t i = 0; i < starInfo_.size(); ++i)
    {
        const StarInfo &s = starInfo_[i];
        if ((hip && s.hip == num) || (hr && s.hr == hrN))
            return (int)i;
        if (s.name && lowerStr(starStr(s.name)) == n) return (int)i;
        if (s.desig && lowerStr(starStr(s.desig)) == n) return (int)i;
    }
    return -1;
}

// A planet (0 .. kPlanetCount - 1) or a star (kPlanetCount + i): its drawn direction (ENU, refracted) and its drawn
// intensity this frame (0 = not drawn). A star outside this frame's drawn prefix: its true direction, intensity 0.
bool SatelliteSim::skyObjectEnu(int obj, glm::vec3 &enu, float *flare) const
{
    if (obj < 0) return false;
    if (obj < kPlanetCount)
    {
        if (!planetMapped) return false;
        const GpuSatVisible &v = static_cast<const GpuSatVisible *>(planetMapped)[obj];
        enu = v.skyDir;
        if (flare) *flare = v.flareIntensity;
        return true;
    }
    const int i = obj - kPlanetCount;
    if (i >= (int)starRecords.size()) return false;
    if (i == starBetelgeuse && snFlare > 0.0)
    {
        enu = snEnu;
        if (flare) *flare = 1.0f;
        return true;
    }
    if ((uint32_t)i < starDrawCount && starMapped)
    {
        const GpuSatVisible &v = static_cast<const GpuSatVisible *>(starMapped)[i];
        enu = v.skyDir;
        if (flare) *flare = v.flareIntensity;
        return true;
    }
    const glm::vec3 &e = starRecords[(size_t)i].eciDir;
    enu = glm::vec3(glm::dot(e, glm::vec3(eci2enuX)), glm::dot(e, glm::vec3(eci2enuY)), glm::dot(e, glm::vec3(eci2enuZ)));
    if (flare) *flare = 0.0f;
    return true;
}

// The selectable stars: named, or brighter than 4.5 (the first few hundred of the sorted list), drawn this frame.
int SatelliteSim::pickStarAt(float clickX, float clickY, float screenW, float screenH, float *distPx) const
{
    constexpr float kMinHitRadiusPx = 8.0f;
    int best = -1;
    float bestDist = 0.0f;
    for (size_t i = 0; i < starInfo_.size() && i < starRecords.size(); ++i)
    {
        const StarInfo &s = starInfo_[i];
        if (s.vmag > 4.5f && s.name == 0) { if (s.vmag > 6.5f) break; continue; }
        glm::vec3 d;
        float fl = 0.0f;
        if (!skyObjectEnu(kPlanetCount + (int)i, d, &fl) || fl <= 0.0f) continue;
        float sx, sy;
        if (!projectSkyDirToScreen(d, screenW, screenH, sx, sy)) continue;
        const float dist = std::hypot(sx - clickX, sy - clickY);
        float hitR = kMinHitRadiusPx;
        if (i < starDrawCount && starMapped && !((int)i == starBetelgeuse && snFlare > 0.0))
            hitR = std::max(hitR, static_cast<const GpuSatVisible *>(starMapped)[i].angularSize * 0.5f);
        if (dist <= hitR && (best < 0 || dist < bestDist))
        {
            best = (int)i;
            bestDist = dist;
        }
    }
    if (distPx) *distPx = bestDist;
    return best;
}

void SatelliteSim::formatSelectedStarInfo(int i)
{
    for (auto &line : planetInfoLine) line[0] = '\0';
    if (i < 0 || i >= (int)starInfo_.size()) return;
    const StarInfo &s = starInfo_[(size_t)i];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    float bv = s.bv;
    float vNow = s.vmag;
    const bool sn = i == starBetelgeuse && supernovaEnabled && t >= kSupernovaT0;
    if (i == starBetelgeuse) vNow = betelgeuseMagAt(t, &bv);
    snprintf(planetInfoLine[0], sizeof(planetInfoLine[0]), "%s%s", starDisplayName(i).c_str(), sn ? " (supernova)" : "");
    const char *spect = starStr(s.spect), *con = starStr(s.conName);
    snprintf(planetInfoLine[1], sizeof(planetInfoLine[1]), "%s%s%s%s%s", sn ? "Supernova (Type II-P)" : "Star",
             spect[0] && !sn ? ", " : "", sn ? "" : spect, con[0] ? ", in " : "", con);
    if (sn)
        snprintf(planetInfoLine[2], sizeof(planetInfoLine[2]), "Mag: %.1f (day %.0f since breakout)", vNow,
                 std::floor((t - kSupernovaT0) / 86400.0) + 1.0);
    else
        snprintf(planetInfoLine[2], sizeof(planetInfoLine[2]), "Mag: %.2f%s", vNow, s.variable || i == starBetelgeuse ? " (variable)" : "");
    if (s.distPc > 0.0f)
        snprintf(planetInfoLine[3], sizeof(planetInfoLine[3]), "Distance: %.0f ly (%.0f pc)", s.distPc * 3.26156f, s.distPc);
    else
        snprintf(planetInfoLine[3], sizeof(planetInfoLine[3]), "Distance: unknown");
    std::string ids = s.desig ? starStr(s.desig) : "";
    if (s.hr) ids += (ids.empty() ? "" : ", ") + std::string("HR ") + std::to_string(s.hr);
    if (s.hip) ids += (ids.empty() ? "" : ", ") + std::string("HIP ") + std::to_string(s.hip);
    snprintf(planetInfoLine[4], sizeof(planetInfoLine[4]), "%s", ids.c_str());
    snprintf(planetInfoLine[5], sizeof(planetInfoLine[5]), "Colour: %s (B-V %.2f)", colourWord(bv), bv);
}

// ── The supernova ──────────────────────────────────────────────────────────────────────────────────────────────────
double SatelliteSim::supernovaMagAt(double t, float *bv)
{
    const double dt = t - kSupernovaT0;
    if (dt < 0.0)
    {
        if (bv) *bv = 1.5f;
        return 99.0;
    }
    const double h = dt / 3600.0, d = dt / 86400.0;
    double m, c;
    if (h < 0.4) // shock breakout: the flash (mostly UV; its visual light within minutes)
    {
        m = 0.45 + (-6.0 - 0.45) * ss01(h / 0.4);
        c = -0.35;
    }
    else if (h < 6.0) // the cooling envelope
    {
        m = -6.0 - 1.0 * ss01((h - 0.4) / 5.6);
        c = -0.3;
    }
    else if (d < 7.0) // the rise to the peak
    {
        const double x = (d - 0.25) / 6.75;
        m = -7.0 - 4.2 * (1.0 - (1.0 - x) * (1.0 - x));
        c = -0.3 + 0.25 * x;
    }
    else if (d < 14.0)
    {
        m = -11.2 + 0.4 * ss01((d - 7.0) / 7.0);
        c = -0.05 + 0.25 * (d - 7.0) / 7.0;
    }
    else if (d < 100.0) // the plateau (hydrogen recombination), cooling and reddening
    {
        m = -10.8 + 0.6 * (d - 14.0) / 86.0;
        c = 0.2 + 0.9 * std::min(1.0, (d - 14.0) / 60.0);
    }
    else if (d < 125.0) // the drop off the plateau
    {
        const double x = ss01((d - 100.0) / 25.0);
        m = -10.2 + 3.2 * x;
        c = 1.1 + 0.3 * x;
    }
    else // the radioactive tail (56Co, 0.98 mag / 100 d), faster once dust forms
    {
        m = -7.0 + 0.0098 * (d - 125.0) + (d > 400.0 ? 0.004 * (d - 400.0) : 0.0);
        c = 1.4 + 0.1 * std::min(1.0, (d - 125.0) / 300.0);
    }
    if (bv) *bv = (float)c;
    return m;
}

double SatelliteSim::supernovaEndT()
{
    double lo = kSupernovaT0 + 200.0 * 86400.0, hi = kSupernovaT0 + 3000.0 * 86400.0;
    for (int i = 0; i < 60; ++i)
    {
        const double m = 0.5 * (lo + hi);
        if (supernovaMagAt(m) < 0.5) lo = m; else hi = m;
    }
    return hi;
}

// Betelgeuse's V now: the supernova from its breakout; before it the semiregular variable (a ~416-day pulsation and
// the ~2100-day long secondary period; V 0.0 .. 1.6 in reality, +-0.35 here about the catalogue's 0.5).
float SatelliteSim::betelgeuseMagAt(double t, float *bv) const
{
    if (supernovaEnabled && t >= kSupernovaT0)
        return (float)supernovaMagAt(t, bv);
    const float base = starBetelgeuse >= 0 ? starInfo_[(size_t)starBetelgeuse].vmag : 0.45f;
    if (bv) *bv = starBetelgeuse >= 0 ? starInfo_[(size_t)starBetelgeuse].bv : 1.5f;
    if (!betelgeuseVariable) return base;
    const double d = t / 86400.0;
    constexpr double kTwoPi = 6.283185307179586;
    return base + (float)(0.22 * std::sin(kTwoPi * (d / 416.0 + 0.31)) + 0.13 * std::sin(kTwoPi * (d / 2100.0 + 0.62)));
}

// kesslerFillDust's finish (after the Moon-base glints): the supernova as a kFireBuf point (bloom and glare like a
// satellite's), at the direction / effectFlare updateStars worked out this frame.
void SatelliteSim::supernovaFill()
{
    if (snFlare <= 0.0 || !kFireMapped || kFireCount >= kFireballMax) return;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    // Range: past the Moon (0.97 x its distance is the glints') so the Moon's disc hides it, inside the depth range.
    fo[kFireCount++] = {glm::vec4(snEnu, (float)snFlare), glm::vec4(snColor, 9.0e8f)};
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

// ── Harness: stars [state] | select <name> | find <name> ──────────────────────────────────────────────────────────
harness::Status SatelliteSim::harnessStars(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string("state") : lowerStr(c.pos[0]);
    std::string rest;
    for (size_t i = 1; i < c.pos.size(); ++i) rest += (i > 1 ? " " : "") + c.pos[i];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (sub == "state")
    {
        size_t named = 0;
        for (const StarInfo &s : starInfo_) named += s.name != 0;
        r["catalogue"] = starCatalogFromFile ? "assets/stars/star_catalog.bin" : "star_catalog.h (BSC5)";
        r["count"] = (int)starInfo_.size();
        r["named"] = (int)named;
        r["faintest_mag"] = starInfo_.empty() ? 0.0f : starInfo_.back().vmag;
        r["drawn_prefix"] = starDrawCount;
        r["drawn_visible"] = starVisibleCount;
        r["limit_mag_frame"] = starLimitMagFrame;
        r["nelm_zenith"] = starNelmZenith;
        r["point_limit_mag"] = pointEffLimitMag();
        r["update_ms"] = starUpdateMs;
        r["sky_limit"] = starSkyLimit;
        float bv = 0.0f;
        r["betelgeuse"] = {{"index", starBetelgeuse},
                           {"v_now", starBetelgeuse >= 0 ? betelgeuseMagAt(t, &bv) : 99.0f},
                           {"bv_now", bv},
                           {"supernova", supernovaEnabled && t >= kSupernovaT0},
                           {"sn_day", (t - kSupernovaT0) / 86400.0},
                           {"sn_mag_seen", snMagSeen},
                           {"sn_flare", snFlare},
                           {"sn_above", snAbove},
                           {"sn_diffuse_moon_frac", snDiffuseMoonFrac},
                           {"breakout_utc", fmtUtcReadable(kSupernovaT0)},
                           {"fades_below_0_5_utc", fmtUtcReadable(supernovaEndT())}};
        if (selectedPlanetIndex >= kPlanetCount)
            r["selected_star"] = starDisplayName(selectedPlanetIndex - kPlanetCount);
        char b[200];
        snprintf(b, sizeof(b), "%zu stars, %u drawn (%u lit), limit %.2f, NELM zenith %.2f, %.3f ms", starInfo_.size(),
                 starDrawCount, starVisibleCount, starLimitMagFrame, starNelmZenith, starUpdateMs);
        r["message"] = b;
        return harness::Status::Done;
    }
    if (sub == "find" || sub == "select")
    {
        const int i = starIndexByName(rest);
        if (i < 0) throw std::runtime_error("stars " + sub + ": no star '" + rest + "' (a proper name, a designation, HIP n, HR n)");
        const StarInfo &s = starInfo_[(size_t)i];
        glm::vec3 d;
        float fl = 0.0f;
        skyObjectEnu(kPlanetCount + i, d, &fl);
        r = {{"index", i}, {"name", starDisplayName(i)}, {"vmag", s.vmag}, {"bv", s.bv}, {"dist_pc", s.distPc},
             {"hip", s.hip}, {"hr", s.hr}, {"spect", starStr(s.spect)}, {"con", starStr(s.conName)},
             {"az_deg", glm::degrees(std::atan2(d.x, d.y))}, {"el_deg", glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)))},
             {"drawn_intensity", fl}};
        if (sub == "select")
        {
            selectedSatIndex = -1;
            selectedPlanetIndex = kPlanetCount + i;
            formatSelectedStarInfo(i);
            stopTrack();
        }
        r["message"] = (sub == "select" ? "selected " : "found ") + starDisplayName(i);
        return harness::Status::Done;
    }
    throw std::runtime_error("stars: state | find <name> | select <name>");
}
