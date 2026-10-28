#pragma once
// SkyEvents.h — the Sky events window's data (2026-10-27, SatelliteSimSkyEvents.cpp; CLAUDE.md "Sky events window").
//
// A celestial event is one record of a KIND. Each kind is one row of the kind table in SatelliteSimSkyEvents.cpp
// (kSkyKinds): an id, a card title, an icon, a SEARCH (a pure function of a time range: it appends the events it finds
// in [t0, t1]) and a WATCH (lands on the event already happening, in frame, at 1x). The window, the worker that runs
// the searches, the cards, the upcoming list and the harness only ever go through that table, so a new kind (the
// Moon-base solar-array glints, the 2037-12-25 Betelgeuse supernova, meteor showers) is a new enum value + a new row.
#include <glm/glm.hpp>
#include <atomic>
#include <cstdint>
#include <vector>

namespace sky
{
// The kinds, in the cards' order. APPEND new kinds before kKindCount (the cards and the harness names follow the
// table; nothing is persisted by index).
enum Kind : int
{
    kSolarEclipse = 0,
    kLunarEclipse,
    kConjunction,
    kOccultation,
    kMoonGlint,     // a lunar base's solar array mirroring the Sun to the Earth (LunarBases.h, 2026-10-27)
    kMeteorShower,  // a meteor shower's peak (Meteors.h, 2026-10-27)
    kKindCount
};

// Bodies an event names: 0..5 are PlanetId (Mercury .. Uranus), then the Moon and the Sun.
constexpr int kBodyMoon = 10;
constexpr int kBodySun = 11;
// A lunar base's glint point on the drawn Moon (a = kBodyMoonBase0 + the base's index in lunar_bases.json).
constexpr int kBodyMoonBase0 = 20;

struct Event
{
    int kind = 0;
    double t = 0.0;            // the moment of the event (s since J2000): greatest eclipse / closest approach
    double t0 = 0.0, t1 = 0.0; // while it is happening: a Watch lands in [t0, t1) (never before t0)
    int a = -1, b = -1;        // the bodies (an eclipse: the Sun or the Moon in a; a conjunction / occultation: a, b)
    double sepDeg = 0.0;       // the closest separation (geocentric; an occultation: topocentric at the site)
    float magA = 99.0f, magB = 99.0f;
    double score = 0.0;        // ranking within its kind (brighter and closer = higher)
    bool total = false;        // eclipses: total; solar: annular sets `annular` instead
    bool annular = false;
    bool hasSite = false;      // `site` is where the search found it best seen (an occultation: the only place)
    glm::dvec3 site{0.0, 0.0, 1.0}; // Earth-fixed unit direction
    double watchT = 0.0;       // a kind's preferred moment to land on, when it has one (an occultation: 1 min before egress)
    double tAux = 0.0;         // kind-specific (an occultation: the geocentric closest approach its sites come from)
    char label[48] = {};       // a kind's own name for its subject (a Moon base's)
};

// The range a search covers, and its cancel flag (a restart while it runs).
struct SearchCtx
{
    double t0 = 0.0, t1 = 0.0;
    float moonSizeScale = 2.5f;           // the drawn Moon is this x its true size (occultation contacts as drawn)
    float moonBaseJitter = 1.0f;          // x the bases' row tilt / azimuth errors (moon_bases.tilt_jitter_scale)
    float moonBaseMagLimit = 3.0f;        // a Moon-base glint is an event when brighter than this (moon_bases.event_mag_limit)
    const std::atomic<bool> *cancel = nullptr;
    bool cancelled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};
} // namespace sky

// SatelliteSim.cpp: planet p's geocentric ECI direction, distance (AU) and apparent magnitude at t, as updatePositions.
void skyPlanetGeoAt(int p, double t, glm::dvec3 &dirEci, double &distAU, float &vmag);
