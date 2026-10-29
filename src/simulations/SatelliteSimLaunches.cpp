// Launches, the sim side (2026-10-18, branch kessler; plan .plans/LAUNCHES_PLAN.md; data/launches.json).
//
// A roster satellite is a POSITION in its constellation; its occupant changes. Every orbital plane of a constellation
// that a program keeps flying is refreshed once per design life, in batches of a launch's payload a few days apart.
// For batch b of refresh k of plane p the arrival time is t_A = (k + phase_p) L + b gap (phase_p hashed): a rocket
// lifts off from a site the plane passes over shortly before t_A - raise (the launch window), deploys its train low,
// the train climbs to its slots and arrives at t_A; there each new satellite takes over its roster slot (which never
// changes on screen) and the satellite it replaces starts down, re-entering weeks later. So the roster's size never
// changes and launches balance re-entries one for one.
//
// Everything is a pure function of sim time: no history. Trains and retirees are drawn as satellites: Keplerian
// records in kesslerBuf's launch block (sat_orbit.comp: LAUNCH_CAP), osculating elements of their state rewritten
// as they move (a few tens of thousands at most). Rockets are point sprites in kFireBuf, their flames emissive
// Gaussians and their exhaust scattering Gaussians in the dust list (cloud_march.comp).
#include "../FrameProfile.h"
#include <chrono>
#include "SatelliteSim.h"
#include "Launches.h"
#include "../Log.h"
#include "../Paths.h"
#include "../VulkanContext.h"
#include "../UIRenderer.h"
#include "UIPalette.h"
#include "clay.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cctype>
#include <atomic>
#include <limits>
#include <future>
#include <thread>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <filesystem>

namespace launchsim
{
using launch::Track;
constexpr uint32_t kChunk = 8;
constexpr uint32_t kLaunchCapL = 131072; // == SatelliteSim::kLaunchCap (asserted in launchInitBlock)
constexpr uint32_t kNumChunks = kLaunchCapL / kChunk;
constexpr double kDay = 86400.0;
constexpr double kYear = 365.25 * 86400.0;
constexpr double kR = satphot::kEarthRadiusM;
constexpr double kEndAltM = 200000.0;   // a retiree is lowered to here, then re-enters
// How long the exhaust is drawn (2026-10-26, issue 20261026_210053: it fades out over the last 60% of this, in
// launch::makeParcel; it used to end here with a third to a half of its optical depth left).
constexpr double kPlumeAgeLow = 2400.0;
constexpr double kPlumeAgeHigh = 3600.0;

glm::dvec3 rotZ(const glm::dvec3 &v, double th)
{
    const double c = std::cos(th), s = std::sin(th);
    return glm::dvec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}
glm::dvec3 ecefToEci(const glm::dvec3 &v, double t) { return rotZ(v, earthRotationAngle(t)); }
glm::dvec3 eciToEcef(const glm::dvec3 &v, double t) { return rotZ(v, -earthRotationAngle(t)); }
double wrap2pi(double a)
{
    a = std::fmod(a, glm::two_pi<double>());
    return a < 0.0 ? a + glm::two_pi<double>() : a;
}
double h01(uint64_t seed, uint64_t idx, uint32_t ch) { return launch::hash01(seed, idx, ch); }
// A program's refresh period (s): the design life x the setting, or for a crew rotation its own cadence (unscaled).
double progLifeS(const launch::Program &pg, double scale)
{
    return pg.crew ? std::max(0.05, pg.lifeYears) * kYear : std::max(0.25, pg.lifeYears * scale) * kYear;
}

struct LPlane
{
    int prog = -1;
    uint32_t constIdx = 0;
    std::vector<uint32_t> sats; // by argument of latitude
    double phase = 0.0;         // [0, 1)
    uint64_t seed = 0;
    int nb = 1;                 // batches per refresh
    double gap = 2.0 * kDay;
    double inclRad = 0.0;
    // The deployment era (2026-10-20): the arrival of each batch's FIRST fill (refresh kFillK), its cohort's S-curve
    // order; empty = the plane has always been there (lattice refreshes only).
    int cohort = -1;
    std::vector<double> fillT;
    uint32_t shell = 0; // its shell (alt / inclination group within the constellation): the fill order's stagger
    // Visiting vehicles (2026-10-21): a station served by more than one visiting program (crews, then cargo) has a
    // plane per program; the extra ones are appended after every other plane (no other plane's index moves).
    // crewPlane: the station's crew plane (a program that shares the crews' ports reads its rotation).
    bool extra = false;
    int crewPlane = -1;
};
// The first fill's refresh number in batch keys (k + 2^19 must fit 20 bits).
constexpr int64_t kFillK = -400000;

struct LMember
{
    uint32_t sat = 0;
    double rSlot = 0.0, nSlot = 0.0;
    double sRaise = 0.0;  // the climb starts (s, J2000)
    double tRamp = 0.0;   // the climb lasts
    double eps = 0.0;     // phasing residual (rad), spread over the raise
    double tEnd = 0.0;    // the retiree's re-entry
    glm::dvec3 depOff{0.0}; // at its release, the dispenser's position less the member's (radial, along, cross): eased out
    double tRel = 0.0;      // its release from the dispenser (s, J2000): the train leaves one after another
    glm::dvec3 sepDir{0.0}; // its separation drift (radial, along, cross; m/s) from the dispenser, eased out with depOff
    glm::dvec3 depVel{0.0}; // at its release, the dispenser's velocity less the member's (same axes, rotating frame)
    // Its FREE FLIGHT (2026-10-21, mission-ops suite): every member of a train flies about ONE reference orbit, the
    // upper stage's parking circle (Clohessy-Wiltshire, curvilinear: x radial, y arc length along it, z cross-track),
    // from its place at release (ffP: the dispenser / stack slot) with its separation velocity (ffV, relative to the
    // circle). ff: set (memberState flies it for kFreeFlightS, then eases into the member's own path).
    bool ff = false;
    glm::dvec3 ffP{0.0}, ffV{0.0};
    // Its place in the payload stack (2026-10-20, launches.json "stack"; stackSlot): in the upper stage's body frame,
    // the satellite's origin (slotO), its body axes (slotR: columns = its X, Y, Z in the stage's), the direction it
    // leaves in (exitDir). stacked: the program has a stack — the satellite is drawn on the stage until its release
    // (a fairing vehicle, once the fairing is gone), leaves it in the stack's attitude (q0, body -> ECI at tRel) with
    // a little tip-off rate (omega, ECI), and slews to its attitude law from slewT0 over slewDur.
    bool stacked = false;
    glm::dvec3 slotO{0.0}, exitDir{0.0, 0.0, 1.0};
    glm::dmat3 slotR{1.0};
    glm::dquat q0{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 omega{0.0};
    double slewT0 = 0.0, slewDur = 0.0;
};

// A fairing half falling away (2026-10-20): its centre of mass sampled every kFairDt s from the separation (ECI,
// position and velocity), its attitude at the separation and its tumble rate. The model's origin is the stage's
// (the half was modelled in place); cb is its centre in that frame.
constexpr double kFairDt = 2.0;
struct LFairing
{
    std::vector<glm::dvec3> c, cv;
    glm::dquat q0{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 w{0.0};
    double tEnd = 0.0; // gone (below ~60 km, or the table's end)
};

// A crew spacecraft's flight (2026-10-20, issue 20261020_214759; LaunchImpl::buildCrew / crewState): separated from
// its upper stage at tSep it coasts in the parking orbit, raises to a phasing orbit (Hohmann, burns at tB1 / tB1e),
// waits there for the station to come round, raises again to the station's altitude 5 km behind it (tB2 .. tArr),
// then flies the approach in the station's frame (segs: to 1 km, round onto the port's axis, holds at 400 / 200 /
// 20 m, contact), stays docked to tUndock, backs out, departs onto a slightly lower orbit, deorbits (tDeorb), enters
// (tEI .. tChute), comes down under parachutes and splashes down / lands (tSplash). Everything a closed form of t.
enum CrewPhase : int
{
    CP_NONE = 0, CP_SEPARATED, CP_RAISE1, CP_PHASING, CP_RAISE2, CP_FAR, CP_TO_AXIS, CP_TO_400, CP_HOLD_400, CP_TO_200,
    CP_HOLD_200, CP_TO_20, CP_HOLD_20, CP_FINAL,
    // (2026-10-21) Berthing: the R-bar approach to the capture point, the arm's capture and berthing
    CP_TO_250, CP_HOLD_250, CP_TO_30, CP_HOLD_30, CP_TO_CAPTURE, CP_CAPTURE, CP_ARM_BERTH, CP_ARM_INSTALL,
    CP_DOCKED, CP_BERTHED, CP_UNBERTH, CP_RELEASE,
    CP_BACKOUT, CP_DEPART, CP_DRIFT, CP_DEORBIT, CP_ENTRY,
    CP_DROGUES, CP_MAINS, CP_DOWN,
    CP_GLIDE, CP_FINAL_GLIDE, CP_ROLLOUT, // (2026-10-21c) a runway landing: Dream Chaser
    CP_ABORT, CP_ESCAPE, CP_COUNT
};
const char *const kCrewPhaseNames[CP_COUNT] = {
    "none", "separated", "phasing burn: raising", "phasing orbit", "raising to the station's altitude",
    "approach: closing to 1 km", "approach: onto the docking axis", "approach: closing to 400 m", "hold at 400 m",
    "approach: closing to 200 m", "hold at 200 m (go / no-go)", "final approach: closing to 20 m", "hold at 20 m",
    "final approach: contact",
    "approach: closing to 250 m (R-bar)", "hold at 250 m", "approach: closing to 30 m", "hold at 30 m (go for capture)",
    "approach: closing to the capture point", "free drift at the capture point (the arm grapples it)",
    "on the arm: moved to the berthing port", "on the arm: berthing (bolts driving)",
    "docked", "berthed", "unberthed: on the arm to the release point", "released: free drift at the release point",
    "undocked: backing out", "departure", "drifting away, deorbit prep",
    "deorbit burn, falling", "entry (plasma)", "drogue parachutes", "main parachutes", "down",
    "gliding to the runway", "final approach and flare", "rollout on the runway",
    "the station is lost: rendezvous called off", "the station is lost: emergency undocking"};
struct LCrewSeg
{
    double t0 = 0.0, t1 = 0.0;
    int kind = 0;               // 0 Hermite a -> b (end velocities va / vb, m/s in the station's frame), 1 arc about the port
    glm::dvec3 a{0.0}, b{0.0};  // station body frame, relative to the station's origin (m); an arc's start / end directions
    glm::dvec3 va{0.0}, vb{0.0};
    double rArc = 0.0;          // an arc's radius about the port
    int phase = 0;              // CrewPhase while in it
    int att = 0;                // 0 toward the port, 1 on the axis (-port axis), 2 the axis turning to prograde (departure)
    double tCut = 1e300;        // (2026-10-21) cut short here (an approach the station's loss called off)
    int attFrom = 1;            // a departure (att 2) turns from this attitude (1; 0 = an approach turned away)
};
struct LCrew
{
    bool on = false;
    uint32_t sat = 0;            // the station (roster index)
    SatOrbitElems st;            // its orbit (rosterElemsOf at build)
    int port = 0, nPorts = 1;
    std::string portName;
    glm::dvec3 portPos{10.0, 0.0, 0.0}, portAx{1.0, 0.0, 0.0}; // station body frame
    glm::dvec3 Us{1, 0, 0}, As{0, 1, 0}, Ns{0, 0, 1};          // the station's orbit plane at tSep (ECI)
    double rS = 0.0, nS = 0.0, r0 = 0.0, n0 = 0.0, rP = 0.0, nP = 0.0;
    double vSep = 0.4;            // the capsule drifts ahead of its stage after separation (m/s)
    glm::dvec3 fix0{0.0};         // at tSep: the capsule on its stage less the plane model (faded out by tB1)
    double th0 = 0.0, th1 = 0.0, th2 = 0.0, th3 = 0.0;
    double tSep = 0.0, tB1 = 0.0, tB1e = 0.0, tB2 = 0.0, tArr = 0.0, tDock = 0.0, tUndock = 0.0, tDepEnd = 0.0,
           tDeorb = 0.0, tCm = 0.0, tEI = 0.0, tChute = 0.0, tMains = 0.0, tSplash = 0.0, tGone = 0.0;
    std::vector<LCrewSeg> segs;   // tArr .. tDepEnd, in time order
    // After departure: a circle rD at angle thDep + nD (t - tDepEnd); the deorbit ellipse (apoapsis at tDeorb).
    double rD = 0.0, nD = 0.0, thDep = 0.0;
    double aE = 0.0, eE = 0.0, nE = 0.0, thApo = 0.0, thEI = 0.0, vEI = 7800.0, vrEI = -200.0, deorbDur = 600.0;
    glm::dvec3 chuteEcef{1, 0, 0}; // unit, Earth-fixed: where the parachutes open
    glm::dvec3 chuteV{0.0};        // its velocity over the ground there (Earth-fixed, m/s, horizontal): eased out under the drogues
    double groundM = 0.0;          // the ground (land) or 0 (sea) there
    bool lands = false;            // comes down on land
    double hold[3] = {0.0, 0.0, 0.0}; // arrivals at the 400 / 200 / 20 m holds
    double tAxis = 0.0;             // on the docking axis 1 km out
    bool skipRaise1 = false;        // the phasing orbit is the parking orbit (no first raise)
    // Visiting vehicles (2026-10-21): a fast-track rendezvous (short coast and approach), a cargo ship (no people),
    // one destroyed on re-entry (a fireball from tEI, tGone = tEI). The station's loss (its dead / end second from the
    // event log, tDeath; buildCrew): before arrival the rendezvous is called off (abort 1: it waits on the circle it
    // reaches and comes home), during the approach it turns away (2), docked a crew undocks in an emergency (3), a
    // cargo ship docked is destroyed with the station (4: gone at tDeath).
    bool fast = false, cargo = false, burns = false;
    // Berthing (2026-10-21): captured by the station's arm capM below the port (tGrapple), moved onto the port
    // (tBerth0 .. tDock), unberthed (tUndock ..) to the release point and released there (tRelease); its grapple
    // fixture (gfB, body frame) and the fixture's outward normal (gfN). The arm (stationArmPose) is with this vehicle
    // over [armA0, armA3) on arrival and [armD0, armD3) at departure. tDepBurn: its departure burn (both kinds).
    bool berth = false;
    double capM = 9.0, tCapArr = 0.0, tGrapple = 0.0, tBerth0 = 0.0, tRelease = 0.0, tDepBurn = 0.0;
    glm::dvec3 gfB{0.0}, gfN{1.0, 0.0, 0.0};
    double armA0 = 0.0, armA2 = 0.0, armA3 = 0.0, armD0 = 0.0, armD3 = 0.0;
    int abort = 0;
    double tDeath = 1e300;
    double massKg = 7000.0;
    // A runway landing (2026-10-21c, Dream Chaser; crewRunwayEcef): the entry ends (tGlide, 10 km) over (x0, y0) of
    // the runway's tangent plane (rwC its centre, unit Earth-fixed; rwE / rwN east / north), moving vx0; it glides
    // to the touchdown point (pTD, along rwDir at vTD) at tTD, rolls out lRoll to rest at tStop. Heights above the
    // runway (rwElev).
    bool runway = false;
    glm::dvec3 rwC{1, 0, 0}, rwE{0, 1, 0}, rwN{0, 0, 1};
    glm::dvec2 rwDir{1, 0}, p0{0.0}, v0{0.0}, pTD{0.0};
    glm::dvec3 rwDv{0.0};
    bool rwDvOn = false;
    double rwElev = 0.0, h0 = 10000.0, tGlide = 1e300, Tg = 600.0, tTD = 1e300, tStop = 1e300, vTD = 95.0, lRoll = 2400.0;
};

// A station arm's pose (2026-10-21; LaunchImpl::stationArmPose), station body frame.
struct ArmPose
{
    glm::dvec3 S{0.0}, E{0.0}, W{0.0}, G{0.0}, nrm{0.0, 1.0, 0.0};
    const LCrew *with = nullptr; // the vehicle it is working with (nullptr: parked or moving to / from the park)
    bool attached = false;       // holding it
    double reachShort = 0.0;     // > 0: the wrist point was out of reach by this much (m)
};

struct LBatch
{
    bool first = false; // the plane's first fill (the deployment era): its slots were empty, nobody retires
    bool crewFast = false; // a visiting vehicle on a fast-track rendezvous (candidate's dockAfter, 2026-10-21)
    bool resid = false; // a member has a phasing residual (rewritten every 2 s while raising)
    int plane = -1, b = 0;
    int64_t k = 0;
    int prog = -1, veh = -1, site = -1;
    bool valid = false;
    std::string why;
    double tA = 0.0, tL = 0.0, tD = 0.0;
    double rPark = 0.0, nPark = 0.0, thIns = 0.0, t2 = 0.0;
    glm::dvec3 U0{1, 0, 0}, A0{0, 1, 0}, N{0, 0, 1}; // ECI at tL: the pad in the plane, prograde, normal
    glm::dvec3 G0{1, 0, 0}, Eaz{0, 1, 0};             // Earth-fixed: the pad, the ground azimuth
    double azDeg = 0.0;
    uint64_t seed = 0;
    std::vector<LMember> mem;
    std::vector<uint32_t> chunks[2]; // GPU chunks: 0 the train, 1 the retirees
    double tw[2] = {-1e300, -1e300}; // when each kind's records were last all written
    uint32_t nUp[2] = {0, 0};        // members up then
    std::vector<double> ends;        // the retirees' re-entries, sorted
    uint32_t touched = 0;
    bool inChunked = false;          // listed in LaunchState::chunked (it holds GPU chunks)
    uint32_t liveIdx = UINT32_MAX;   // its index in the live list it was last put in
    uint32_t inWalk = UINT32_MAX;    // the frame of the window walk that last listed it (== walkFrame: listed now)
    double tDeorbBurn = 0.0, tStageRE = 0.0, tStageGone = 0.0;
    // The upper stage's deorbit (every stage burns, expendable or returning): a burn of burnDur from tDeorbBurn that
    // puts it on an ellipse with its apoapsis there (angle thApo in the plane, from U0); it reaches the entry interface
    // (100 km, angle thEI, speed vEI) at tStageGone. Before the burn it turns retrograde (tTurn on).
    double burnDur = 0.0, aE = 0.0, eE = 0.0, nE = 0.0, thApo = 0.0, thEI = 0.0, vEI = 7800.0, vrEI = 0.0;
    bool vents = false;
    bool parcelsBuilt = false;
    float plumeGrow = 1.0f; // "Plume growth" (launches.plume_growth) the parcels were / are being built with
    float plumeDisp = 1.0f; // launches.plume_dispersal, likewise
    double plumeResPri = 0.0; // (2026-10-26) last frame's weakest priority in its reserved share (0: all reserved)
    std::vector<launch::Parcel> parcels;
    double tVisEnd = 0.0;
    int shifts = 0, rivals = 0; // pad deconfliction: passes skipped, candidates checked
    // The pad's height (2026-10-19): its plateau (the DEM there, the terrain flattened to it) + the vehicle's mount.
    // Added to the track near the pad (trackEcef; gone by ~40 km downrange).
    double padBase = 0.0;
    // A returning ship's landing (2026-10-19): after its entry (B.tStageGone .. + kShipEntryS) it descends belly
    // first to ~1.2 km over a Starship pad (tShipDesc .. tShipFlip), flips, burns and is caught by the tower
    // (tShipCatch), held there until tShipEnd. landPad < 0: no pad in reach, a controlled splashdown at the same
    // point of its descent (the sea under it: catchH 0). Descent in the landing frame (the pad's or the splash
    // point's gnomonic plane: x east, y north, metres; h above sea level): from (dX0, dH0) with velocity (dV0, dVz0)
    // to F = (dXF, dHF) with (dVF, dVzF) over T1 (cubic Hermite per axis), then to the catch point (0, catchH) at rest
    // over T2.
    int landPad = -1;
    bool shipLands = false;
    double tShipDesc = 0.0, tShipFlip = 0.0, tShipCatch = 0.0, tShipEnd = 0.0;
    glm::dvec3 LG{1, 0, 0}, LE{0, 1, 0}, LN{0, 0, 1}; // landing frame (Earth-fixed): up, east, north at the target
    double catchH = 0.0;
    glm::dvec2 dX0{0.0}, dV0{0.0}, dXF{0.0}, dVF{0.0};
    double dH0 = 0.0, dVz0 = 0.0, dHF = 0.0, dVzF = 0.0, T1 = 1.0, T2 = 1.0;
    // The entry model's start vs the deorbit ellipse at the entry interface (ECI, m): faded out over the entry's
    // first 60 s (shipEntry), so the ship's track is continuous there (it jumped ~0.5 km).
    glm::dvec3 entryOff{0.0};
    // The payload fairing (2026-10-20): its halves separate at tFair (-1: no fairing), then fall (built lazily by
    // ensureFairing; fairBuilt). deployInterval: the members' spacing in time (the stack's, else the vehicle's).
    double tFair = -1.0;
    bool fairBuilt = false;
    LFairing fair[2];
    double deployInterval = 2.0;
    // The booster's recovery for THIS launch (2026-10-20, droneships): rec 0 expendable, 1 back to the pad (a tower
    // catch, or a Falcon pad's landing zone), 2 downrange (a droneship / landing ship, or the ground where the arc comes
    // down on land). A reusable vehicle with rtls_max_kg flies a light payload (payloadKg, the program's mass x a hashed
    // +-10%) back; heavier ones go downrange. landKind: where it comes to rest (kLand*), landEcef its point (sea level
    // for a ship: the deck is deckM above; for the ground its height in landBase).
    int rec = 0;
    int landKind = 0;
    double payloadKg = -1.0;
    double xLand = 0.0;           // downrange of the touchdown (m, the booster track's frame)
    glm::dvec3 lzOff{0.0};        // a landing zone's offset from the pad (Earth-fixed, m; rec 1 on a Falcon pad)
    double lzUp = 0.0;            // ... and its ground less the pad's (m)
    double landBase = 0.0;        // a downrange ground landing: the ground's height less the track's touchdown height
    // The ship (landKind kLandShip): on station at shipG (unit, Earth-fixed) from tShipOn, the booster down on its deck
    // at tLand; from tShipDepart it steams home toward the launch site at kShipSpeed (eased in), gone at tShipGone.
    int ship = -1;                // index into the vehicle's landing ships
    double deckM = 3.0;
    glm::dvec3 shipG{1, 0, 0}, shipHome{0, 1, 0}; // its station and the horizontal direction home (unit, there)
    double tShipOn = 0.0, tLand = 0.0, tShipDepart = 0.0, tShipGone = 0.0;
    // The sea state at the ship (2026-10-21; 1 = the default sea, the drawn waves' rule: SatelliteSim::shipSeaState),
    // eased; set each frame by launchShipJobs while the ship is out (-1 = not yet: the default). Its swell (shipSwell).
    mutable float seaK = -1.0f;
    LCrew crew; // a crew spacecraft's own flight (a crew program with a spacecraft model)
};

// A launch pad on the ground (2026-10-19): one per distinct place among the catalog's sites (the Jiuquan crewed
// and uncrewed entries share a pad). Drawn by sat_sky.frag (the flat parts: launchFillPadParams), as a static mesh
// (the tower, the mount, the tanks: launchMeshJobs) and as lights at night (launchFillPadLights).
struct LPad
{
    int site = -1;                 // the first catalog site here
    int kind = 0;                  // 0 generic, 1 Falcon (fixed service structure, a landing zone), 2 Starship (catch tower)
    glm::dvec3 g{1, 0, 0};         // unit, Earth-fixed
    glm::dvec3 u{0, 1, 0}, v{0, 0, 1}; // Earth-fixed horizontal axes: u the layout's heading, v = up x u (to its left)
    double elevM = 0.0;            // plateau height (m above sea level)
    uint32_t seed = 0;
    double exclR = 3000.0;         // no city grid, glitter or sprites inside this (m)
    double flatR = 400.0;          // graded flat to elevM inside this (blending out to twice it)
    bool lz = false;               // a landing zone (Falcon pads) ...
    double lzAng = 0.0;            // ... kLzDistM out at this angle from u (toward v), on land
    double lzElevM = 0.0;          // ... its ground (m above sea level)
    std::string name;
    double onlineT = -1e300;       // the deployment era: built from this date (the earliest of its sites)
    bool fictional = false;
    // What stands here by date (2026-10-21): (from J2000 s, kind; -1 = nothing), sorted; `kind` above is the last
    // (the 2036 pad, drawn at every date with the deployment era off). Built by buildPads from the sites'
    // pad_history or their vehicles' online dates (39A: Shuttle pad, Falcon from 2017, its Starship tower 2027-03).
    std::vector<std::pair<double, int>> kindHist;
};
// The pad's kind at t (-1 = not there), with its time-dependent radii. era false: the 2036 pad always.
static int padKindAt(const LPad &p, double t, bool era)
{
    if (!era)
        return p.kind;
    int k = -1;
    for (const auto &h : p.kindHist)
    {
        if (h.first > t)
            break;
        k = h.second;
    }
    return k;
}
static double padExclRFor(int kind) { return kind == 1 ? 3200.0 : 3000.0; }
static double padFlatRFor(int kind) { return kind == 2 ? 450.0 : 350.0; }
// A copy of the pad as it stands at t (kind, exclusion and graded radii); kind -1 = not there.
static LPad padAtTime(const LPad &p, double t, bool era)
{
    LPad q = p;
    q.kind = padKindAt(p, t, era);
    if (q.kind >= 0)
    {
        q.exclR = padExclRFor(q.kind);
        q.flatR = padFlatRFor(q.kind);
    }
    return q;
}
// Where things stand on a pad, in its (u, v) frame (metres): shared by the ground pattern (sat_sky.frag padGround,
// which must agree), the meshes and the lights.
constexpr double kPadTowerV = -32.0;   // the tower's centre (u = 0); its arms reach +v over the mount at the centre
constexpr double kShipCatchAboveGround = 55.0; // a caught ship's aft end (the chopsticks hold it under its flaps)
constexpr double kShipEntryS = 480.0;  // the entry glow (shipEntry) lasts this long
constexpr double kCaughtHoldS = 1800.0;
// A train's free flight (2026-10-21): each member flies free from its release for this long, then its own propulsion
// takes it into its own path (eased out by two days after the release).
constexpr double kFreeFlightS = 6.0 * 3600.0;
// The least distance two members of a train come to in free flight (from a minute after the later one leaves).
constexpr double kReleaseGapM = 30.0;
// A train's separation velocity in the parking circle's plane (radial, along; m/s): the SAME for every member, so
// their relative motion is one Clohessy-Wiltshire trajectory shifted in time and never closes below the chord it
// leaves with (members differ only across the plane, which never brings two together). Mostly up out of the stack,
// a little back for a dispenser point; forward off the nose for a column (its satellites' lateral directions go
// across the plane). (A pure along-track push collides with itself 0.41 orbit later: release spans stay short.)
// Never a purely radial push (2026-10-26, issue 20261026_194340): that is a CLOSED 2:1 ellipse, so every member came
// back to the door it left an orbit later, and every orbit after (until kFreeFlightS). The along-track share (-0.24:
// a little retrograde) drifts each orbit ~3 x 0.24 x sp x T ahead (~2.8 km at Starship's 0.7 m/s) and costs no
// member-to-member separation (releaseMinRate stays sp).
static glm::dvec2 releaseInPlane(int layout, double sp)
{
    if (layout == launch::Stack::Column)
        return sp * glm::dvec2(0.25, 0.968);
    return sp * glm::dvec2(0.97, -0.24);
}
// The least speed of that trajectory relative to the stage (min over an orbit of |dX/dt|): two members released dt
// apart stay at least this x dt apart.
static double releaseMinRate(const glm::dvec2 &v)
{
    double m = 1e30;
    for (int k = 0; k < 128; ++k)
    {
        const double th = glm::two_pi<double>() * (double)k / 128.0, c = std::cos(th), s = std::sin(th);
        const double xp = v.x * c + 2.0 * v.y * s, yp = -2.0 * v.x * s + v.y * (4.0 * c - 3.0);
        m = std::min(m, std::sqrt(xp * xp + yp * yp));
    }
    return m;
}
constexpr double kLzDistM = 2300.0;
// Where a booster comes to rest (LBatch::landKind; harness `launch state` -> recovery, and the landing effects key off
// it: dust on a pad or the ground, spray off a ship's deck).
constexpr int kLandNone = 0;  // expendable: breaks up / falls in the sea
constexpr int kLandTower = 1; // caught by the tower's chopsticks at its pad
constexpr int kLandLz = 2;    // a landing zone beside its pad (Falcon RTLS)
constexpr int kLandShip = 3;  // a droneship / landing ship's deck, at sea downrange
constexpr int kLandGround = 4; // the ground downrange (an inland site's arc comes down on land)
constexpr double kShipOnStationS = 8.0 * 3600.0;  // a ship is on station this long before the launch
constexpr double kShipSecureS = 1.5 * 3600.0;     // ... then the booster is secured before it heads home
constexpr double kShipSpeed = 4.0;                // under tow / its own power (m/s, ~8 knots)
constexpr double kShipHomeS = 12.0 * 3600.0;      // drawn until this long after the landing (some 150 km on)    // a landing zone's distance from its pad (sat_sky.frag must agree) // a caught booster or ship is drawn in the tower this long

// A pre-decoded R8 map's value at a place (0..255, bilinear on texel centres as the GPU filters it): the halves
// (tools/make_raw_textures.py: "SLR8", width, total height, first row) are read four texels at a time.
double rawR8At(const char *const *paths, int nParts, double latDeg, double lonDeg)
{
    struct Part
    {
        FILE *f = nullptr;
        uint32_t w = 0, h = 0, first = 0;
    };
    Part parts[2];
    nParts = std::clamp(nParts, 1, 2);
    bool ok = true;
    for (int i = 0; i < nParts && ok; ++i)
    {
        parts[i].f = std::fopen(paths[i], "rb");
        char magic[4];
        uint32_t hdr[3];
        ok = parts[i].f && std::fread(magic, 1, 4, parts[i].f) == 4 && std::memcmp(magic, "SLR8", 4) == 0 &&
             std::fread(hdr, 4, 3, parts[i].f) == 3;
        if (ok)
        {
            parts[i].w = hdr[0];
            parts[i].h = hdr[1];
            parts[i].first = hdr[2];
        }
    }
    double val = -1.0;
    if (ok)
    {
        const uint32_t W = parts[0].w, H = parts[0].h;
        auto texel = [&](int x, int y) -> double {
            x = ((x % (int)W) + (int)W) % (int)W;
            y = std::clamp(y, 0, (int)H - 1);
            const Part &p = (nParts > 1 && (uint32_t)y >= parts[1].first) ? parts[1] : parts[0];
            std::fseek(p.f, (long)(16 + ((size_t)(y - (int)p.first) * W + (size_t)x)), SEEK_SET);
            unsigned char b = 0;
            return std::fread(&b, 1, 1, p.f) == 1 ? (double)b : 0.0;
        };
        const double u = (glm::radians(lonDeg) + glm::pi<double>()) / glm::two_pi<double>();
        const double vv = (0.5 * glm::pi<double>() - glm::radians(latDeg)) / glm::pi<double>();
        const double tx = u * W - 0.5, ty = vv * H - 0.5;
        const int x0 = (int)std::floor(tx), y0 = (int)std::floor(ty);
        const double fx = tx - x0, fy = ty - y0;
        val = (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
              (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
    }
    for (Part &p : parts)
        if (p.f)
            std::fclose(p.f);
    return val;
}
// The DEM's height at a place (m above sea level), decoded as the GPU does (terrain.glsl: v / 255 x 9000 - 15 / 255
// x 9000, floored at 0; bilinear of the decoded texels ~ of the raw ones above the baseline).
double demElevCpu(double latDeg, double lonDeg)
{
    static const char *kParts[2] = {"assets/textures/earth_elevation_0.r8", "assets/textures/earth_elevation_1.r8"};
    const double v = rawR8At(kParts, 2, latDeg, lonDeg);
    return v < 0.0 ? 0.0 : std::max(0.0, v / 255.0 * 9000.0 - 15.0 / 255.0 * 9000.0);
}
// The water map's signed distance to the shore there (m, > 0 in water; terrain.glsl kShoreSdfMaxM), or 0 unknown.
double shoreDistCpu(double latDeg, double lonDeg)
{
    static const char *kSdf[1] = {"assets/textures/earth_water_sdf.r8"};
    const double v = rawR8At(kSdf, 1, latDeg, lonDeg);
    const double kShoreSdfMaxM = 4.0 * glm::pi<double>() * 6371000.0 / 4096.0;
    return v < 0.0 ? 0.0 : (v / 255.0 - 0.5) * 2.0 * kShoreSdfMaxM;
}

// A coast patch around coastal launch sites (2026-10-20, data/launch_coasts.json baked by tools/make_pad_coasts.py;
// terrain_detail.glsl tdCoastPatch): a kCoastGridN^2 grid of the shore's signed distance (bytes, > 127.5 water) over
// [-R, R]^2 metres east / north of its centre, rows south to north.
struct LCoast
{
    std::string name;
    glm::dvec3 g{1, 0, 0}, e{0, 1, 0}, n{0, 0, 1}; // centre (unit, Earth-fixed) and its east / north axes
    double R = 6000.0;
    glm::vec3 col{0.14f, 0.16f, 0.10f};
    float floorM = 2.5f;
    std::vector<uint8_t> grid;
};
constexpr double kCoastSdfMaxM = 3000.0;
constexpr double kTwoPi1p5 = 15.749609945722419; // (2 pi)^1.5: a 3D Gaussian's normalisation // == tools/make_pad_coasts.py SDF_MAX_M (terrain_detail.glsl kCoastSdfMaxM)

static std::vector<uint8_t> base64Decode(const std::string &in)
{
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in)
    {
        const int v = val(c);
        if (v < 0)
            continue;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back((uint8_t)((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

// The coast patches' shore at an Earth-fixed unit direction u: their signed distance (m, > 0 water) and weight k
// (the GPU's: 1 inside, fading out over the outer quarter; 0 = no patch). C1 bilinear as tdCoastPatch.
static double coastPatchAt(const std::vector<LCoast> &cs, const glm::dvec3 &u, double &k)
{
    k = 0.0;
    double dOut = 0.0;
    for (const LCoast &c : cs)
    {
        if (glm::dot(u, c.g) < std::cos(1.5 * c.R / kR))
            continue;
        const glm::dvec3 d = (u - c.g) * kR;
        const double px = glm::dot(d, c.e), py = glm::dot(d, c.n);
        const double ax = std::abs(px) / c.R, ay = std::abs(py) / c.R;
        const double ki = (1.0 - glm::smoothstep(0.72, 0.97, ax)) * (1.0 - glm::smoothstep(0.72, 0.97, ay));
        if (ki <= k)
            continue;
        const int N = kCoastGridN;
        const double gx = (px / c.R * 0.5 + 0.5) * N - 0.5, gy = (py / c.R * 0.5 + 0.5) * N - 0.5;
        const double fx0 = std::floor(gx), fy0 = std::floor(gy);
        double fx = gx - fx0, fy = gy - fy0;
        fx = fx * fx * (3.0 - 2.0 * fx);
        fy = fy * fy * (3.0 - 2.0 * fy);
        auto at = [&](int x, int y) {
            x = std::clamp(x, 0, N - 1);
            y = std::clamp(y, 0, N - 1);
            return (double)c.grid[(size_t)y * N + x];
        };
        const int x0 = (int)fx0, y0 = (int)fy0;
        const double b = glm::mix(glm::mix(at(x0, y0), at(x0 + 1, y0), fx), glm::mix(at(x0, y0 + 1), at(x0 + 1, y0 + 1), fx), fy);
        dOut = (b - 127.5) / 127.5 * kCoastSdfMaxM;
        k = ki;
    }
    return dOut;
}

struct SlotInfo
{
    uint64_t owner = 0; // batch key * 2 + kind, 0 = free
    uint16_t m = 0;
    bool live = false;  // a record is up (or due)
    bool shown = false; // the GPU holds a live record
    double tw = -1e300; // when it was written (sim time)
};

uint64_t batchKey(int plane, int64_t k, int b)
{
    return ((uint64_t)(uint32_t)plane << 40) | ((uint64_t)(k + (1 << 19)) & 0xFFFFFull) << 20 | (uint64_t)(b & 0xFFFFF);
}
} // namespace launchsim
using namespace launchsim;

struct SatelliteSim::LaunchState
{
    // Plume parcels built on worker threads (2026-10-20: buildParcels integrates every parcel over its life, 80-190 ms
    // a launch, and ran on the main thread the frame a launch first mattered: the hitches at 1 min/s - 1 h/s). Keyed by
    // batch; a std::async future's destructor waits, so clearing the map joins them: launchInitBlock does (the batches
    // they were made for go), and the destructor does before the catalog and pads they read are destroyed.
    std::unordered_map<uint64_t, std::future<std::vector<launch::Parcel>>> parcelJobs;
    ~LaunchState()
    {
        joinJob();
        parcelJobs.clear();
    }
    launch::Catalog cat;
    // Station arms (2026-10-21, Canadarm2): (the station's roster index, its arm in cat.arms), set by buildPlanes.
    std::vector<std::pair<uint32_t, int>> armSat;
    const launch::StationArm *armOf(uint32_t sat) const
    {
        for (const auto &a : armSat)
            if (a.first == sat)
                return &cat.arms[(size_t)a.second];
        return nullptr;
    }
    uint32_t nArmParts = 0;                 // arm parts drawn this frame (launchArmJobs)
    ArmPose armPoseNow;    // the last drawn arm's pose (harness `launch arm`)
    uint32_t armPoseSat = UINT32_MAX;
    bool tried = false, loaded = false;
    std::string err;
    bool planesBuilt = false;
    std::vector<LPlane> planes;
    // The deployment era (2026-10-20): one cohort per program deployment block (its constellations' satellites in
    // first-fill order along its S-curve). deployOn: the era as the planes were built.
    struct Cohort
    {
        int prog = -1, dep = -1;
        std::string name;
        double N = 0.0;      // satellites
        launch::SCurve curve;
        uint32_t nBatches = 0;
    };
    std::vector<Cohort> cohorts;
    bool deployOn = false;
    std::vector<Track> booster; // per vehicle (its own recovery)
    std::vector<Track> boosterRtls; // per vehicle: back to the pad (a droneship vehicle with rtls_max_kg; else empty)
    std::map<std::pair<int, int>, Track> upper; // (vehicle, park km)
    std::unordered_map<uint64_t, LBatch> batches;
    // A batch's launch before pad deconfliction (vehicle, site, the plane's next pass over it), cached by batch key.
    struct Cand
    {
        int veh = -1, site = -1;
        double tL = -1.0, az = 0.0;
        double dockAfter = -1.0; // a fast-track visiting vehicle: lift-off to docking (s); < 0 the ~1-day profile
    };
    std::unordered_map<uint64_t, Cand> cands;
    int buildCap = 100; // batches built a frame at most (launchUpdate's walk; follows their measured cost)
    double buildMsEach = 0.1; // (2026-10-21) the wall time of one batch build in the parallel pieces (eased)
    // (2026-10-21b) The background build: after a time jump (a bookmark, `time set`, Missions Go to) thousands of
    // batches are missing at once, and the walk's ~8-ms-a-frame budget took 300-500 frames to build them (busy
    // 2032: never within 500). They are built on worker threads (below normal priority) in order of urgency, in
    // chunks; each frame the main thread moves the finished prefix into `batches` (applyBuildJob). The builds read
    // the planes, the catalog, the roster and the candidate / rival caches (both locked) and write only `out`:
    // anything that changes those (launchInitBlock, a crewed station's fate in crewSync) joins or discards the job
    // (buildGen). The same batches as a build on the main thread (buildBatch is a pure function of the batch).
    struct BuildItem
    {
        uint64_t key;
        int pi;
        int64_t k;
        int b;
        double life;
    };
    struct BuildJob
    {
        std::vector<BuildItem> items;
        std::vector<LBatch> out;
        std::atomic<size_t> done{0};    // items [0, done) are built (release)
        std::atomic<bool> cancel{false};
        std::atomic<bool> finished{false};
        std::thread th;
        uint64_t gen = 0;
        size_t applied = 0;
        std::chrono::steady_clock::time_point t0;
    };
    std::unique_ptr<BuildJob> job;
    std::unordered_set<uint64_t> jobKeys; // the keys the job holds (not built on the main thread meanwhile)
    uint64_t buildGen = 0;
    uint32_t jobsRun = 0;
    double plumeCutPrev = 0.0;   // (2026-10-21b) last frame's plume priority cut (launchFillVisuals)
    double plumeResEased = 48.0; // (2026-10-26) each launch's reserved share of the plume budget, eased (ranks)
    uint32_t plumeSkipped = 0, plumeSkippedLast = 0;
    double lastJobMs = 0.0;
    size_t lastJobN = 0;
    // (perf4-cpu 2026-10-24) The Missions window's own builds (launchUpcoming's non-forced refresh) on a worker of their
    // own: after a jump its 2- and 14-day searches need tens to hundreds of batches whose days' rival buckets are cold
    // (5-25 ms a build on the main thread: 25-30 ms a frame for seconds). The rows wait for it; a forced refresh (a
    // click, the harness) still builds on the main thread.
    struct UpJob
    {
        std::vector<BuildItem> items;
        std::vector<LBatch> out;
        std::atomic<bool> finished{false};
        std::thread th;
        uint64_t gen = 0;
    };
    std::unique_ptr<UpJob> upJob;
    std::unordered_set<uint64_t> upKeys;
    void joinJob()
    {
        if (job)
        {
            job->cancel.store(true);
            if (job->th.joinable())
                job->th.join();
            job.reset();
        }
        if (upJob)
        {
            if (upJob->th.joinable())
                upJob->th.join();
            upJob.reset();
        }
        jobKeys.clear();
        upKeys.clear();
        ++buildGen;
    }
    std::shared_mutex candsMx; // (2026-10-20) batches are built in parallel (launchUpdate's walk): candidate() is shared
    // Pad deconfliction's rivals by (site, day of their candidate launch): every batch whose candidate is at the site
    // that day (2026-10-20: each build enumerated every plane flying from its site, ~1 ms a build, 100+ ms a frame at
    // load and at a fast warp). Pure functions of the batches, shared by the parallel builds.
    struct RivalEntry
    {
        double tL, tA, pri;
        uint64_t key;
        int plane;
    };
    std::unordered_map<uint64_t, std::shared_ptr<const std::vector<RivalEntry>>> rivalIdx;
    std::mutex rivalMx;
    std::vector<std::vector<int>> planesBySite; // the planes that may launch from each site
    std::vector<uint32_t> freeChunks;
    std::vector<SlotInfo> slots;
    std::vector<SatOrbitElems> elems;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    void *stageMapped = nullptr;
    static constexpr uint32_t kStageRecs = 16384;
    uint32_t gpuGen = UINT32_MAX;
    bool clearGpu = true;         // zero the GPU block before anything is written (a reset)
    float lifeScale = -1.0f;      // the planes were built for this "life_scale"
    int afterStorm = -1;          // ... and this "after_storm"
    uint32_t frame = 0;
    bool active = false; // anything up on the GPU
    std::vector<uint64_t> live; // batch keys touched this frame (in a stable order)
    std::vector<LBatch *> livePtr; // ... and the batches (valid until the next launchUpdate: only it erases batches)
    size_t cursor = 0;
    // The settings section's readouts (refreshed every few seconds).
    double uiListAt = -1e300, uiListSim = -1e300;
    std::string uiNext;
    int uiCount24 = 0;
    // Readouts (this frame)
    struct PlumeDbg
    {
        double rangeKm, altKm, sigKm, Sigma, tauPeak, age;
        glm::dvec3 col;
        double amb;
        int ground = 0;          // 0 exhaust, 1 lift-off cloud, 2 landing cloud
        glm::dvec3 drawn{0.0};   // the colour handed to the shader (Sun + sky)
        double isoShare = -1.0;  // the share of it lit isotropically (the sky); < 0: none
    };
    std::vector<PlumeDbg> plumeDbg;
    // (2026-10-26, issue 20261026_210053) per launch drawing plumes this frame: its candidates, how many were drawn,
    // their summed cross-section (before / after the budget's cut and fade), parcels alive / built, the largest
    // decimation step (launch state -> plume_groups).
    struct PlumeGrpDbg
    {
        uint64_t key = 0;
        double tPlus = 0.0;
        int cand = 0, drawn = 0, parcels = 0, alive = 0, maxDec = 1;
        double sigC = 0.0, sigD = 0.0, rangeKm = 0.0;
    };
    std::vector<PlumeGrpDbg> plumeGrpDbg;
    // (2026-10-21) The entry plasma Gaussians drawn this frame (launch state -> entry_plasma): range, the resolved share,
    // the face-on surface brightness asked for and as capped x the sky exposure (displayed), the weight, the view hold.
    struct PlasmaDbg
    {
        double rangeM, resK, sAsked, sDispCapped, weight, hold, sigMin, sigMax;
    };
    std::vector<PlasmaDbg> plasmaDbg;
    std::unordered_map<int, glm::dvec3> engineGeom; // per rocket type: (nozzle exit z, exit radius, length) in metres
    // Per plane: the crewed station's dead second (Kessler's roster word) the plane's batches were built against; a
    // change (a breakup, the cascade reaching it, a rollback) rebuilds them (crewSync).
    std::vector<int32_t> crewDead;
    std::vector<LPad> pads;          // the launch pads (ensureCatalog)
    std::vector<LCoast> coasts;      // the coast patches around coastal sites (data/launch_coasts.json)
    // Roster satellite -> its plane and place in it (missionRosterBatches; rebuilt with the planes).
    std::vector<int32_t> satPlane;
    std::vector<uint32_t> satPos;
    std::vector<int> sitePad;        // per catalog site: its pad
    uint64_t padMeshKey[16] = {};    // the pads drawn as meshes, per reserved slot (pad index + 1; 0 = free)
    uint32_t nRockets = 0, nTrain = 0, nRetire = 0, nPlumes = 0, nReentries = 0, nWrites = 0, nBatches = 0;
    // Upper stages carrying a visible payload stack this frame (launchFillVisuals -> launchMeshJobs): the stage's
    // state at the list's time and its body frame.
    struct StackDraw
    {
        uint64_t key = 0;
        double t = 0.0;
        glm::dvec3 P{0.0}, V{0.0}, A{0.0};
        glm::dmat3 S{1.0};
        int type = -1; // the stage's model (its mesh size: which stack is nearest)
    };
    std::vector<StackDraw> stackDraws;
    uint64_t stackMeshKey[SatelliteSim::kStackMeshSlots] = {}; // per reserved slot: batch key ^ (member + 1) << 48
    uint32_t nStacked = 0, nFairings = 0, nCrew = 0;
    uint64_t shipMeshKey[SatelliteSim::kShipMeshSlots] = {}; // the landing ships drawn, per reserved slot (batch key)
    uint32_t nShips = 0, nShipRobots = 0;
    double lastFullFreeT = -1e300; // the last full pass over the batches (launchUpdate's freeing)
    // (2026-10-20, CPU) The window walk's result, reused while sim time stays within kWalkMarginS (300 s) of walkT:
    // every plane and a hash lookup per batch each frame was ~0.33 ms with Starmind's ~5300 live batches.
    std::vector<std::pair<uint64_t, LBatch *>> walkList; // sorted by key; invalid batches too (they are touched)
    double walkT = -1e300;
    bool walkOk = false;            // complete (no build budget hit) and current
    uint64_t batchEpoch = 0;        // bumped whenever a batch that may be listed is erased
    uint64_t walkEpoch = ~0ull;
    double freeLo = 0.0, freeHi = -1.0; // launchUpdate's freeing: no chunked batch's need changes for t in [lo, hi)
    bool freeForce = true;
    uint32_t walkFrame = UINT32_MAX; // the frame of the walk the live list comes from (batches touched then)
    // The batches holding GPU chunks (the freeing pass looks at these only).
    std::vector<LBatch *> chunked;
    std::vector<uint8_t> upScratch;
    // Per live batch (parallel to livePtr, 2026-10-20): its raising and falling times, and whether it holds chunks.
    struct LiveTimes
    {
        double tD, tA, tEnd; // tEnd = the last retiree's re-entry + 1 s
    };
    std::vector<LiveTimes> liveT;
    // ... and the interval of t in which its last look found nothing due, with its counts then (2026-10-20, CPU).
    struct LiveSkip
    {
        double lo = 1.0, hi = 0.0; // empty
        uint32_t addT = 0, addR = 0;
    };
    std::vector<LiveSkip> liveSkip;
    std::vector<uint8_t> liveCh;
    std::vector<std::pair<uint64_t, LBatch *>> visList; // the live batches with something to draw near now
    std::vector<std::pair<uint64_t, LBatch *>> crewList; // the live crew batches whose spacecraft flies or is docked now // launchUpdate's per-batch "member up now" flags (reused)
    // CPU time per part (ms, eased over ~30 frames; harness `launch state` -> cpu_split_ms, 2026-10-20): 0 window walk,
    // 1 live list, 2 freeing, 3 record writes, 4 the rest of launchUpdate, 5 pad lights, 6 the visuals' batch loop.
    double prof[12] = {};
    uint32_t nDueEval = 0, nFinds = 0; // this frame: batches whose members were tested for a write; window lookups
    void profAdd(int i, std::chrono::steady_clock::time_point &t0)
    {
        const auto t1 = std::chrono::steady_clock::now();
        prof[i] += (std::chrono::duration<double, std::milli>(t1 - t0).count() - prof[i]) / 30.0;
        t0 = t1;
    }

    const Track &upperTrackFor(int veh, double parkKm)
    {
        const std::pair<int, int> key{veh, (int)std::lround(parkKm)};
        auto it = upper.find(key);
        if (it != upper.end())
            return it->second;
        return upper.emplace(key, launch::upperTrack(cat.vehicles[veh], parkKm * 1000.0)).first->second;
    }
};

void SatelliteSim::LaunchStateDelete::operator()(LaunchState *s) const { delete s; }

// ── Setup ───────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::launchInitBlock(bool keepPlanes)
{
    static_assert(kLaunchCap == kLaunchCapL, "the launch block's size");
    // Every launch-block entry empty (no constellation: the per-constellation scans skip it).
    {
        FPROF_SCOPE("launch.initBlock.orbits");
        for (uint32_t s = 0; s < kLaunchCap && activeSatCount + s < satOrbits.size(); ++s)
        {
            SatOrbit &o = satOrbits[activeSatCount + s];
            o = SatOrbit{};
            o.constIdx = UINT32_MAX;
            o.launchSlot = (int32_t)s;
        }
    }
    if (!launchState)
        launchState.reset(new LaunchState());
    LaunchState &L = *launchState;
    {
        FPROF_SCOPE("launch.initBlock.joinBuildJob");
        L.joinJob(); // (2026-10-21b: the background build reads the planes and the roster)
    }
    // The catalog stays; the roster may have changed (planes, batches, slots go).
    {
        FPROF_SCOPE("launch.initBlock.parcelJobs");
        L.parcelJobs.clear(); // (waits for plume builds in flight: their batches go)
    }
    // (2026-10-21) A Kessler restart keeps the planes, their sites and the candidates / rival day index: they are
    // functions of the roster and the catalog alone (the batches read the Kessler state and go). Rebuilding them cost
    // ~75 ms the frame after a player breakup before the CME.
    keepPlanes = keepPlanes && L.planesBuilt;
    if (!keepPlanes)
    {
        L.planesBuilt = false;
        L.planes.clear();
    }
    if (keepPlanes)
    {
        // (2026-10-21) Thousands of batches (their members, crews, tracks) freed off the main thread: ~20 ms here.
        FPROF_SCOPE("launch.initBlock.disposeBatches");
        auto *old = new std::unordered_map<uint64_t, LBatch>(std::move(L.batches));
        L.batches = std::unordered_map<uint64_t, LBatch>();
        std::thread([old] { delete old; }).detach();
    }
    else
        L.batches.clear();
    L.walkOk = false;
    L.walkList.clear();
    L.visList.clear();
    L.chunked.clear();
    ++L.batchEpoch;
    if (!keepPlanes)
    {
        L.cands.clear();
        {
            std::lock_guard<std::mutex> lk(L.rivalMx);
            L.rivalIdx.clear();
        }
        L.planesBySite.clear();
    }
    {
        FPROF_SCOPE("launch.initBlock.slotsElems");
        L.slots.assign(kLaunchCap, SlotInfo{});
        L.elems.assign(kLaunchCap, SatOrbitElems{});
    }
    L.freeChunks.clear();
    // The last chunk holds the rocket meshes and the transient selection's slots, the two before it the pads' meshes,
    // the six before those the payload stacks' (2026-10-20).
    // The landing ships' (2026-10-20) one more below the stacks'.
    for (uint32_t c = kNumChunks - 3 - kStackMeshSlots / kChunk - kShipMeshSlots / kChunk - kArmMeshSlots / kChunk; c-- > 0;)
        L.freeChunks.push_back(c);
    static_assert(kPadMeshSlots == 2 * kChunk, "the pads' mesh slots: two chunks");
    static_assert(kStackMeshSlots % kChunk == 0, "the stacks' mesh slots: whole chunks");
    static_assert(kShipMeshSlots == kChunk, "the ships' mesh slots: one chunk");
    std::fill(std::begin(L.shipMeshKey), std::end(L.shipMeshKey), 0ull);
    std::fill(std::begin(L.stackMeshKey), std::end(L.stackMeshKey), 0ull);
    if (activeSatCount + kLaunchCap - 1 < satOrbits.size())
        satOrbits[activeSatCount + kLaunchCap - 1].typeIdx = UINT32_MAX; // no model: Track only
    if (selIsTransient())
    {
        selectedSatIndex = -1;
        selTransientKey = 0;
    }
    L.gpuGen = UINT32_MAX;
    L.clearGpu = true; // whatever the GPU block holds goes (records of the state just dropped)
    L.active = false;
    L.live.clear();
    L.livePtr.clear();
}

void SatelliteSim::launchDestroy(VkDevice device)
{
    if (!launchState)
        return;
    LaunchState &L = *launchState;
    L.joinJob();
    if (L.stage && ctx_)
        ctx_->slottedDestroy(&L.stage); // every slot's copy
}

struct LaunchImpl
{
static void ensureCatalog(SatelliteSim::LaunchState &L)
{
    if (L.tried)
        return;
    L.tried = true;
    const std::string path = (std::filesystem::path(Paths::exeDir()) / "launches.json").string();
    L.loaded = L.cat.load(path, L.err);
    Log::line(L.loaded ? "launches: " + std::to_string(L.cat.programs.size()) + " programs, " +
                             std::to_string(L.cat.vehicles.size()) + " vehicles, " +
                             std::to_string(L.cat.sites.size()) + " sites"
                       : "launches: " + L.err);
    if (L.loaded)
        for (const launch::Vehicle &v : L.cat.vehicles)
        {
            L.booster.push_back(launch::boosterTrack(v));
            L.boosterRtls.push_back(v.recovery == 2 && v.rtlsMaxKg > 0.0 ? launch::boosterTrack(v, 1, 0.0) : Track{});
        }
    loadCoasts(L);
    buildPads(L);
}

// The coast patches (data/launch_coasts.json next to the exe): optional; without the file the pads stand on the map.
static void loadCoasts(SatelliteSim::LaunchState &L)
{
    L.coasts.clear();
    const std::string path = (std::filesystem::path(Paths::exeDir()) / "launch_coasts.json").string();
    std::ifstream f(path);
    if (!f)
    {
        Log::line("launches: no launch_coasts.json (the pads' coasts are the water map's)");
        return;
    }
    try
    {
        const nlohmann::json j = nlohmann::json::parse(f);
        for (const auto &p : j.at("patches"))
        {
            LCoast c;
            c.name = p.value("name", std::string());
            const double la = glm::radians(p.at("lat").get<double>()), lo = glm::radians(p.at("lon").get<double>());
            c.g = glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
            c.e = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c.g));
            c.n = glm::cross(c.g, c.e);
            c.R = p.at("radius_m").get<double>();
            const auto col = p.value("land_color", std::vector<float>{0.14f, 0.16f, 0.10f});
            if (col.size() >= 3)
                c.col = glm::vec3(col[0], col[1], col[2]);
            c.floorM = p.value("land_floor_m", 2.5f);
            c.grid = base64Decode(p.at("sdf_b64").get<std::string>());
            if (p.value("grid_n", 0) != kCoastGridN || c.grid.size() != (size_t)kCoastGridN * kCoastGridN ||
                std::abs(p.value("sdf_max_m", 0.0) - kCoastSdfMaxM) > 1e-6)
            {
                Log::line("launches: coast patch " + c.name + " has another grid size: skipped (re-run tools/make_pad_coasts.py)");
                continue;
            }
            L.coasts.push_back(std::move(c));
        }
        Log::line("launches: " + std::to_string(L.coasts.size()) + " coast patches");
    }
    catch (const std::exception &e)
    {
        Log::line(std::string("launches: launch_coasts.json: ") + e.what());
        L.coasts.clear();
    }
}

// The water map's shore distance at a place, with the coast patches' shore mixed in (as tdDemAt does).
static double shoreDistAt(const SatelliteSim::LaunchState &L, double latDeg, double lonDeg)
{
    const double map = shoreDistCpu(latDeg, lonDeg);
    const double la = glm::radians(latDeg), lo = glm::radians(lonDeg);
    double k = 0.0;
    const double d = coastPatchAt(L.coasts, glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)), k);
    return glm::mix(map, d, k);
}

// The pads: one per distinct place (sites within 150 m share one), its kind from the vehicles that fly from it (a
// tower-caught vehicle with a mount: a Starship pad; Falcon: a fixed service structure and a landing zone), its
// layout turned to the middle of its launch azimuths, its ground height from the DEM.
static void buildPads(SatelliteSim::LaunchState &L)
{
    L.pads.clear();
    L.sitePad.assign(L.cat.sites.size(), -1);
    for (size_t i = 0; i < L.cat.sites.size(); ++i)
    {
        launch::Site &st = L.cat.sites[i];
        int at = -1;
        for (size_t p = 0; p < L.pads.size(); ++p)
            if (glm::dot(L.pads[p].g, st.ecef) > std::cos(150.0 / kR))
                at = (int)p;
        if (at < 0)
        {
            LPad pd;
            pd.site = (int)i;
            pd.name = st.name;
            pd.g = st.ecef;
            int kind = 0;
            for (const launch::Vehicle &vh : L.cat.vehicles)
                if (std::find(vh.sites.begin(), vh.sites.end(), (int)i) != vh.sites.end())
                {
                    if (vh.recovery == 1 && vh.mountM > 0.0)
                        kind = 2;
                    else if (vh.id.rfind("falcon9", 0) == 0)
                        kind = std::max(kind, 1);
                }
            pd.kind = st.padKind >= 0 ? std::min(st.padKind, 2) : kind;
            double head = st.padHeadingDeg;
            if (std::isnan(head))
            {
                double a0 = st.azMinDeg, a1 = st.azMaxDeg;
                if (a1 < a0)
                    a1 += 360.0;
                head = 0.5 * (a0 + a1);
            }
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), pd.g));
            const glm::dvec3 north = glm::cross(pd.g, east);
            // The water map around it (most pads are on a coast): the layout's back (-u: the access road, the
            // trench, the tanks) turns to the most land 1.5-3 km out when the heading's own back is water there.
            auto shoreAt = [&](double az, double dist) {
                const glm::dvec3 dir = std::cos(az) * north + std::sin(az) * east;
                const glm::dvec3 q = glm::normalize(pd.g + dir * (dist / kR));
                return shoreDistAt(L, glm::degrees(std::asin(glm::clamp(q.z, -1.0, 1.0))), glm::degrees(std::atan2(q.y, q.x)));
            };
            double hr = glm::radians(head);
            if (std::isnan(st.padHeadingDeg) && shoreAt(hr + glm::pi<double>(), 1500.0) > -200.0)
            {
                double best = 1e30;
                for (int k = 0; k < 24; ++k)
                {
                    const double az = glm::two_pi<double>() * k / 24.0;
                    const double d = shoreAt(az, 1500.0) + shoreAt(az, 3000.0);
                    if (d < best)
                    {
                        best = d;
                        hr = az + glm::pi<double>();
                    }
                }
                head = glm::degrees(hr);
            }
            pd.u = std::cos(hr) * north + std::sin(hr) * east;
            pd.v = glm::cross(pd.g, pd.u);
            // The ground: never below 3 m (a pad at the sea's level would be drawn as the sea).
            pd.elevM = std::max(demElevCpu(st.latDeg, st.lonDeg), 3.0);
            uint64_t h = 1469598103934665603ull;
            for (char c : st.id)
                h = (h ^ (uint64_t)(unsigned char)c) * 1099511628211ull;
            pd.seed = (uint32_t)(h & 0xFFFFu);
            pd.exclR = pd.kind == 1 ? 3200.0 : 3000.0;
            pd.flatR = pd.kind == 2 ? 450.0 : 350.0;
            // A landing zone (Falcon pads): on the most land kLzDistM out, if any (none in the water).
            pd.lz = false;
            if (pd.kind == 1)
            {
                double best = -300.0;
                for (int k = 0; k < 24; ++k)
                {
                    const double a = glm::two_pi<double>() * (k + 0.5) / 24.0;
                    const glm::dvec3 dir = std::cos(a) * pd.u + std::sin(a) * pd.v;
                    const glm::dvec3 q = glm::normalize(pd.g + dir * (kLzDistM / kR));
                    const double la = glm::degrees(std::asin(glm::clamp(q.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(q.y, q.x));
                    const double d = shoreDistAt(L, la, lo);
                    // Dry land: not water, and above the sea's level (land at 0 m is drawn as water) or in a coast
                    // patch (its land is lifted, tdDemAt).
                    double ck = 0.0;
                    coastPatchAt(L.coasts, q, ck);
                    if (d < best && (demElevCpu(la, lo) > 1.0 || ck > 0.99))
                    {
                        best = d;
                        pd.lz = true;
                        pd.lzAng = a;
                        pd.lzElevM = std::max(demElevCpu(la, lo), 1.0);
                    }
                }
            }
            pd.onlineT = st.onlineT;
            pd.fictional = st.fictional;
            at = (int)L.pads.size();
            L.pads.push_back(pd);
            Log::line("launches: pad " + st.id + " kind " + std::to_string(pd.kind) + ", heading " +
                      std::to_string((int)std::lround(head)) + " deg, ground " + std::to_string((int)std::lround(pd.elevM)) + " m");
        }
        L.sitePad[i] = at;
        L.pads[at].onlineT = std::min(L.pads[at].onlineT, st.onlineT);
        st.elevM = L.pads[at].elevM;
    }
    // Each pad's history (2026-10-21): per site its pad_history, else a pad from its online date (kind 0) raised to
    // each vehicle's kind from the date that vehicle flies there (onlineFor: 39A's Starship tower); the pad is the
    // largest of its sites' kinds at each change point. The 2036 kind (`kind`) is unchanged.
    auto siteKindAt = [&](size_t si, double t) {
        const launch::Site &st = L.cat.sites[si];
        if (!st.padHistory.empty())
        {
            int k = -1;
            for (const auto &h : st.padHistory)
                if (h.first <= t)
                    k = h.second;
            return k;
        }
        if (st.onlineT > t)
            return -1;
        int k = 0;
        for (const launch::Vehicle &vh : L.cat.vehicles)
            if (std::find(vh.sites.begin(), vh.sites.end(), (int)si) != vh.sites.end() && st.onlineForVehicle(vh.id) <= t)
            {
                if (vh.recovery == 1 && vh.mountM > 0.0)
                    k = 2;
                else if (vh.id.rfind("falcon9", 0) == 0)
                    k = std::max(k, 1);
            }
        if (st.padKind >= 0)
            k = std::min(st.padKind, 2);
        return k;
    };
    for (size_t p = 0; p < L.pads.size(); ++p)
    {
        LPad &pd = L.pads[p];
        std::vector<double> ts{-1e300};
        for (size_t si = 0; si < L.cat.sites.size(); ++si)
        {
            if (L.sitePad[si] != (int)p)
                continue;
            const launch::Site &st = L.cat.sites[si];
            for (const auto &h : st.padHistory)
                ts.push_back(h.first);
            ts.push_back(st.onlineT);
            for (const auto &f : st.onlineFor)
                ts.push_back(std::max(f.second, st.onlineT));
        }
        std::sort(ts.begin(), ts.end());
        ts.erase(std::unique(ts.begin(), ts.end()), ts.end());
        pd.kindHist.clear();
        for (double t : ts)
        {
            int k = -1;
            for (size_t si = 0; si < L.cat.sites.size(); ++si)
                if (L.sitePad[si] == (int)p)
                    k = std::max(k, siteKindAt(si, t));
            if (pd.kindHist.empty() || pd.kindHist.back().second != k)
                pd.kindHist.push_back({t, k});
        }
        // The 2036 pad keeps the kind buildPads gave it (and its landing zone); the era draws the history.
        pd.onlineT = 1e300;
        for (const auto &h : pd.kindHist)
            if (h.second >= 0)
            {
                pd.onlineT = h.first;
                break;
            }
        std::string hs;
        for (const auto &h : pd.kindHist)
            hs += " " + (h.first < -1e299 ? std::string("always") : std::to_string((int)std::lround(2000.0 + h.first / 31557600.0))) +
                  ":" + std::to_string(h.second);
        if (pd.kindHist.size() > 1 || (pd.kindHist.size() == 1 && pd.kindHist[0].second != pd.kind))
            Log::line("launches: pad " + L.cat.sites[pd.site].id + " history" + hs);
    }
}

// ── Mass of a satellite type (for its re-entry) ─────────────────────────────────────────────────
static double launchTypeMass(const SatelliteType &ty)
{
    if (ty.massKg > 0.0f)
        return ty.massKg;
    const std::string &m = ty.modelId;
    if (m.find("spacex_ai") != std::string::npos)
        return 2500.0;
    if (m.find("starlink_v3") != std::string::npos)
        return 1900.0;
    if (m.find("starlink") != std::string::npos)
        return 800.0;
    if (m.find("oneweb") != std::string::npos)
        return 150.0;
    return 600.0;
}

// Groups each programme's constellations into planes (raan, inclination, altitude), each sorted by phase.
static void buildPlanes(SatelliteSim::LaunchState &L, const std::vector<SatOrbit> &orbits, uint32_t rosterCount,
                        const std::vector<ConstellationConfig> &consts, float lifeScale, bool deployOn)
{
    FPROF_SCOPE("launch.buildPlanes");
    L.planes.clear();
    L.cohorts.clear();
    L.deployOn = deployOn;
    L.satPlane.clear();
    L.satPos.clear();
    for (size_t c = 0; c < consts.size(); ++c)
    {
        int prog = -1;
        for (size_t p = 0; p < L.cat.programs.size() && prog < 0; ++p)
            for (const std::string &m : L.cat.programs[p].match)
                if (consts[c].name.find(m) != std::string::npos)
                {
                    prog = (int)p;
                    break;
                }
        if (prog < 0 || consts[c].orbitCount < (L.cat.programs[prog].crew ? 1u : 2u))
            continue;
        const launch::Program &pg = L.cat.programs[prog];
        std::map<uint64_t, std::vector<uint32_t>> groups;
        const uint32_t i0 = consts[c].orbitStart, i1 = std::min(i0 + consts[c].orbitCount, rosterCount);
        for (uint32_t i = i0; i < i1; ++i)
        {
            const SatOrbit &o = orbits[i];
            const int64_t ra = (int64_t)std::lround(glm::degrees(wrap2pi((double)o.raan))) % 360;
            const int64_t inc = (int64_t)std::lround(glm::degrees((double)o.incl) * 4.0);
            const int64_t alt = (int64_t)std::lround(o.altM / 5000.0);
            const uint64_t key = ((uint64_t)ra << 40) | ((uint64_t)inc << 20) | (uint64_t)(alt & 0xFFFFF);
            groups[key].push_back(i);
        }
        const int cluster = std::max(1, consts[c].clusterSize);
        const int payload = std::max(cluster, pg.payload / cluster * cluster);
        const double life = progLifeS(pg, (double)lifeScale);
        for (auto &g : groups)
        {
            LPlane pl;
            pl.prog = prog;
            pl.constIdx = (uint32_t)c;
            pl.sats = std::move(g.second);
            std::stable_sort(pl.sats.begin(), pl.sats.end(),
                             [&](uint32_t a, uint32_t b) { return orbits[a].u0 < orbits[b].u0; });
            pl.seed = (uint64_t)c * 0x9E3779B97F4A7C15ull ^ g.first * 0xC2B2AE3D27D4EB4Full;
            pl.phase = h01(pl.seed, 1, 1);
            pl.nb = (int)((pl.sats.size() + (size_t)payload - 1) / (size_t)payload);
            pl.gap = std::min(pg.batchGapDays * kDay, life / std::max(pl.nb, 1));
            pl.inclRad = orbits[pl.sats[0]].incl;
            // The deployment era: its cohort (the program's first deployment block whose match takes this
            // constellation; none = always there).
            if (deployOn && !pg.crew)
                for (size_t d = 0; d < pg.deploy.size() && pl.cohort < 0; ++d)
                {
                    const launch::Deployment &dp = pg.deploy[d];
                    bool m = dp.match.empty();
                    for (const std::string &s : dp.match)
                        m = m || consts[c].name.find(s) != std::string::npos;
                    if (!m)
                        continue;
                    int ci = -1;
                    for (size_t q = 0; q < L.cohorts.size(); ++q)
                        if (L.cohorts[q].prog == prog && L.cohorts[q].dep == (int)d)
                            ci = (int)q;
                    if (ci < 0)
                    {
                        SatelliteSim::LaunchState::Cohort co;
                        co.prog = prog;
                        co.dep = (int)d;
                        co.name = dp.match.empty() ? pg.name : dp.match[0];
                        ci = (int)L.cohorts.size();
                        L.cohorts.push_back(co);
                    }
                    pl.cohort = ci;
                }
            L.planes.push_back(std::move(pl));
        }
    }
    // Visiting vehicles (2026-10-21): every other visiting program (cargo) matching a station gets a plane of its own,
    // appended here so the index of no other plane (batch keys, the pad rivals' priorities) moves.
    for (size_t c = 0; c < consts.size(); ++c)
    {
        if (consts[c].orbitCount < 1u || consts[c].orbitStart >= rosterCount)
            continue;
        int first = -1;
        std::vector<int> visits;
        for (size_t p = 0; p < L.cat.programs.size(); ++p)
            for (const std::string &m : L.cat.programs[p].match)
                if (consts[c].name.find(m) != std::string::npos)
                {
                    if (first < 0)
                        first = (int)p;
                    else if (L.cat.programs[p].crew)
                        visits.push_back((int)p);
                    break;
                }
        if (first < 0 || !L.cat.programs[first].crew)
            continue;
        int crewPl = -1;
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
            if (L.planes[pi].constIdx == (uint32_t)c && L.planes[pi].prog == first)
                crewPl = (int)pi;
        if (crewPl < 0)
            continue;
        for (int prog : visits)
        {
            const launch::Program &pg = L.cat.programs[prog];
            const LPlane &cp = L.planes[(size_t)crewPl];
            LPlane pl;
            pl.prog = prog;
            pl.constIdx = (uint32_t)c;
            pl.sats = cp.sats;
            pl.seed = cp.seed ^ ((uint64_t)(prog + 1) * 0xD6E8FEB86659FD93ull);
            pl.phase = h01(pl.seed, 1, 1);
            pl.extra = true;
            pl.crewPlane = L.cat.programs[first].cargo ? -1 : crewPl;
            // Sharing the crews' ports: the visits come half a rotation after the crews' (the same cadence).
            if (pg.sharePorts && pl.crewPlane >= 0)
                pl.phase = std::fmod(cp.phase + 0.5, 1.0);
            // (2026-10-21c) "alternate_with": half a cadence after that program's visits here (one berthing port).
            if (!pg.alternateWith.empty())
                for (size_t q = 0; q < L.planes.size(); ++q)
                    if (L.planes[q].extra && L.planes[q].constIdx == (uint32_t)c &&
                        L.cat.programs[(size_t)L.planes[q].prog].name == pg.alternateWith)
                    {
                        pl.phase = std::fmod(L.planes[q].phase + 0.5, 1.0);
                        break;
                    }
            pl.nb = 1;
            pl.gap = progLifeS(pg, (double)lifeScale);
            pl.inclRad = cp.inclRad;
            L.planes.push_back(std::move(pl));
        }
    }
    // The deployment era's first fills (2026-10-20): per cohort, every batch of its planes in fill order — shells one
    // after another (by altitude within each constellation; constellations side by side), overlapping by the block's
    // "shell_stagger"; within a shell batch 0 of every plane, then batch 1 ... (planes fill in parallel) — and the
    // cumulative share of satellites mapped through the cohort's S-curve gives each batch's arrival.
    for (size_t ci = 0; ci < L.cohorts.size(); ++ci)
    {
        SatelliteSim::LaunchState::Cohort &co = L.cohorts[ci];
        const launch::Program &pg = L.cat.programs[co.prog];
        const launch::Deployment &dp = pg.deploy[co.dep];
        std::vector<int> pls;
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
            if (L.planes[pi].cohort == (int)ci)
                pls.push_back((int)pi);
        // Shells per constellation, by altitude (1 km) and inclination (0.25 deg).
        std::map<uint32_t, std::vector<std::pair<int64_t, int64_t>>> shellsOf; // constIdx -> sorted (alt, inc)
        auto shellKey = [&](const LPlane &pl) {
            const SatOrbit &o = orbits[pl.sats[0]];
            return std::make_pair((int64_t)std::lround(o.altM / 1000.0), (int64_t)std::lround(glm::degrees((double)o.incl) * 4.0));
        };
        for (int pi : pls)
        {
            auto &v = shellsOf[L.planes[pi].constIdx];
            const auto k = shellKey(L.planes[pi]);
            if (std::find(v.begin(), v.end(), k) == v.end())
                v.push_back(k);
        }
        for (auto &kv : shellsOf)
            std::sort(kv.second.begin(), kv.second.end());
        struct FB
        {
            double key;
            int plane, b;
            uint32_t n;
        };
        std::vector<FB> fb;
        co.N = 0.0;
        for (int pi : pls)
        {
            LPlane &pl = L.planes[pi];
            const auto &sv = shellsOf[pl.constIdx];
            const size_t rank = (size_t)(std::find(sv.begin(), sv.end(), shellKey(pl)) - sv.begin());
            pl.shell = (uint32_t)rank;
            const double sk = sv.size() > 1 ? (double)rank / (double)(sv.size() - 1) : 0.0;
            const int cluster = std::max(1, consts[pl.constIdx].clusterSize);
            const int payload = std::max(cluster, pg.payload / cluster * cluster);
            const double jit = 1e-7 * h01(pl.seed, 3, 7);
            pl.fillT.assign((size_t)pl.nb, 0.0);
            for (int b = 0; b < pl.nb; ++b)
            {
                const size_t m0 = (size_t)b * payload, m1 = std::min(pl.sats.size(), m0 + (size_t)payload);
                fb.push_back({((double)b + 0.5) / (double)pl.nb + dp.stagger * sk + jit, pi, b, (uint32_t)(m1 - m0)});
                co.N += (double)(m1 - m0);
            }
        }
        std::sort(fb.begin(), fb.end(), [](const FB &a, const FB &b) {
            return a.key < b.key || (a.key == b.key && (a.plane < b.plane || (a.plane == b.plane && a.b < b.b)));
        });
        co.curve.fit(dp.startT, dp.completeT, dp.steep, L.cat.referenceT, co.N > 0.0 ? dp.present / co.N : 0.0);
        double cum = 0.0;
        for (const FB &f : fb)
        {
            const double mid = (cum + 0.5 * f.n) / std::max(co.N, 1.0);
            cum += f.n;
            L.planes[f.plane].fillT[(size_t)f.b] = co.curve.inv(mid);
        }
        co.nBatches = (uint32_t)fb.size();
        char m[240];
        snprintf(m, sizeof(m), "launches: deployment %s: %.0f satellites in %zu planes, %u first-fill launches, %.0f%% at the reference date",
                 co.name.c_str(), co.N, pls.size(), co.nBatches, 100.0 * co.curve.frac(L.cat.referenceT));
        Log::line(m);
    }
    L.planesBySite.assign(L.cat.sites.size(), {});
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        std::vector<bool> seen(L.cat.sites.size(), false);
        for (int v : L.cat.programs[L.planes[pi].prog].vehicles)
            for (int st : L.cat.vehicles[v].sites)
                if (!seen[st])
                {
                    seen[st] = true;
                    L.planesBySite[st].push_back((int)pi);
                }
    }
    // Every (vehicle, parking orbit) upper track a program can fly, built here: candidate() and buildBatch() run in
    // parallel and only READ the map then (2026-10-21; a build that met a missing track inserted into it concurrently).
    for (const launch::Program &pg : L.cat.programs)
    {
        for (int v : pg.vehicles)
            L.upperTrackFor(v, pg.parkKm);
        for (const launch::Deployment &dp : pg.deploy)
            for (int v : dp.earlyVehicles)
                L.upperTrackFor(v, pg.parkKm);
    }
    // (2026-10-21) The stations with an arm.
    L.armSat.clear();
    for (size_t ai = 0; ai < L.cat.arms.size(); ++ai)
        for (size_t c = 0; c < consts.size(); ++c)
            for (const std::string &m : L.cat.arms[ai].match)
                if (consts[c].name.find(m) != std::string::npos && consts[c].orbitCount > 0 &&
                    consts[c].orbitStart < rosterCount)
                {
                    L.armSat.push_back({consts[c].orbitStart, (int)ai});
                    break;
                }
    L.planesBuilt = true;
    L.lifeScale = lifeScale;
    char m[160];
    snprintf(m, sizeof(m), "launches: %zu planes in %zu programs", L.planes.size(), L.cat.programs.size());
    Log::line(m);
}

// The next time after t0 that the plane (normal N, ECI) passes over the site: both passes a day; the one whose
// prograde ground azimuth the site allows. Returns < 0 if none within two days.
static double nextCrossing(const launch::Site &site, const std::function<glm::dvec3(double)> &normalAt, double t0,
                           double &azOut)
{
    const double w = satphot::kOmegaEarth;
    for (double tb = t0; tb < t0 + 2.0 * kDay; tb += 0.5 * kDay)
    {
        glm::dvec3 N = normalAt(tb);
        // N . Rz(th) s = c (Nx sx + Ny sy) + s (Ny sx - Nx sy) + Nz sz = Rr cos(th - ph) + C
        const glm::dvec3 s = site.ecef;
        const double A = N.x * s.x + N.y * s.y, B = N.y * s.x - N.x * s.y, C = N.z * s.z;
        const double Rr = std::sqrt(A * A + B * B);
        if (Rr < 1e-9 || std::abs(C) > Rr)
            return -1.0; // the plane never passes over this latitude
        const double ph = std::atan2(B, A), dth = std::acos(std::clamp(-C / Rr, -1.0, 1.0));
        double best = -1.0;
        for (int sgn = -1; sgn <= 1; sgn += 2)
        {
            const double thS = ph + sgn * dth;
            const double th0 = earthRotationAngle(tb);
            double t = tb + wrap2pi(thS - th0) / w;
            // Refine with the normal at that time (an SSO plane turns ~1 deg a day).
            for (int it = 0; it < 2; ++it)
            {
                const glm::dvec3 Nt = normalAt(t);
                const double At = Nt.x * s.x + Nt.y * s.y, Bt = Nt.y * s.x - Nt.x * s.y, Ct = Nt.z * s.z;
                const double Rt = std::sqrt(At * At + Bt * Bt);
                if (Rt < 1e-9 || std::abs(Ct) > Rt)
                    break;
                const double ths = std::atan2(Bt, At) + sgn * std::acos(std::clamp(-Ct / Rt, -1.0, 1.0));
                double d = std::remainder(ths - earthRotationAngle(t), glm::two_pi<double>());
                t += d / w;
                N = Nt;
            }
            if (t < t0)
                continue;
            // Prograde at the pad: N x U; its azimuth on the ground.
            const glm::dvec3 U = ecefToEci(s, t);
            const glm::dvec3 Ap = glm::normalize(glm::cross(normalAt(t), U));
            const glm::dvec3 Ae = eciToEcef(Ap, t);
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), s));
            const glm::dvec3 north = glm::cross(s, east);
            double az = glm::degrees(std::atan2(glm::dot(Ae, east), glm::dot(Ae, north)));
            if (az < 0.0)
                az += 360.0;
            double lo = site.azMinDeg, hi = site.azMaxDeg;
            double a = az;
            if (lo < 0.0 && a > 180.0)
                a -= 360.0;
            if (a < lo || a > hi)
                continue;
            if (best < 0.0 || t < best)
            {
                best = t;
                azOut = az;
            }
        }
        if (best >= 0.0)
            return best;
    }
    return -1.0;
}

// ── Batches ─────────────────────────────────────────────────────────────────────────────────────
static double secOfKessler(const SatelliteSim &s, double t) { return std::floor(t - s.kesslerEpoch()); }

static uint64_t batchSeed(const LPlane &pl, int64_t k, int b)
{
    return pl.seed ^ ((uint64_t)(k + 77) * 0x2545F4914F6CDD1Dull) ^ ((uint64_t)b * 0x9E3779B97F4A7C15ull);
}

// The deployment era (2026-10-20). A batch's arrival: its first fill's (k == kFillK) or the lattice's.
static double arrivalOf(const LPlane &pl, int64_t k, int b, double life)
{
    if (k == kFillK)
        return (size_t)b < pl.fillT.size() ? pl.fillT[(size_t)b] : -1e300;
    return (k + pl.phase) * life + b * pl.gap;
}
// Whether a lattice refresh flies: never before the plane's first fill (+ the shortest first life); a first fill does
// when the plane has one.
static bool batchExists(const SatelliteSim::LaunchState &L, const LPlane &pl, int64_t k, int b, double life)
{
    if (k == kFillK)
        return (size_t)b < pl.fillT.size();
    if (pl.fillT.empty() || (size_t)b >= pl.fillT.size())
        return true;
    return arrivalOf(pl, k, b, life) >= pl.fillT[(size_t)b] + L.cat.minFirstLifeFrac * life;
}
// (2026-10-21d) The launches' stand-down at the Sun's Fury ("after the storm: stand down", the default) as an ARRIVAL
// boundary: a batch arriving at or after it lifts off after the CME for certain, so it is stood down whatever its pad
// and rivals. Its lift-off is never earlier than arrival - raiseDays[1] - its vehicle's lead (candidate() searches
// forward from tA - raise - lead; pad ranks only move it later, the fast-docking pass search too). +inf when nothing
// stands down for good (Kessler off, or "resume", whose end depends on the ranks). maxLeadS: the largest t2 +
// deployAfter of the catalog's vehicles (maxVehicleLeadS).
static double maxVehicleLeadS(const SatelliteSim::LaunchState &L)
{
    double lead = 0.0;
    for (const launch::Vehicle &v : L.cat.vehicles)
        lead = std::max(lead, v.t2 + v.deployAfter);
    return lead;
}
// (SATLIGHTSIM_LAUNCH_STANDDOWN_SKIP=0 turns the shortcut off: the A/B for its cost and its schedule identity.)
static bool standDownSkipOn()
{
    static const bool on = [] {
        const char *e = std::getenv("SATLIGHTSIM_LAUNCH_STANDDOWN_SKIP");
        return !(e && e[0] == '0');
    }();
    return on;
}
static double standDownArrival(const SatelliteSim &sim, const launch::Program &pg, double maxLeadS)
{
    if (!sim.kesslerEnabled || sim.launchAfterStorm != 0 || !standDownSkipOn())
        return std::numeric_limits<double>::infinity();
    return sim.kesslerEpoch() + pg.raiseDays[1] * kDay + maxLeadS + 1.0;
}
static bool standsDownAt(const SatelliteSim &sim, double tL)
{
    return sim.kesslerEnabled && sim.launchAfterStorm == 0 && tL >= sim.kesslerEpoch() && standDownSkipOn();
}
// Every batch of a plane whose arrival is in [lo, hi]: fn(k, b) — the lattice refreshes that exist, then the first
// fills (a plane's first fills arrive in b order, fillT increasing).
template <class F>
static void forBatches(const SatelliteSim::LaunchState &L, const LPlane &pl, double life, double lo, double hi, F &&fn)
{
    const int64_t k0 = (int64_t)std::floor((lo - (pl.nb - 1) * pl.gap) / life - pl.phase);
    const int64_t k1 = (int64_t)std::floor(hi / life - pl.phase);
    for (int64_t k = k0; k <= k1; ++k)
    {
        const double base = (k + pl.phase) * life;
        const int b0 = std::max(0, (int)std::ceil((lo - base) / pl.gap));
        const int b1 = std::min(pl.nb - 1, (int)std::floor((hi - base) / pl.gap));
        for (int b = b0; b <= b1; ++b)
            if (batchExists(L, pl, k, b, life))
                fn(k, b);
    }
    if (!pl.fillT.empty() && hi >= pl.fillT.front() && lo <= pl.fillT.back())
    {
        auto it = std::lower_bound(pl.fillT.begin(), pl.fillT.end(), lo);
        for (; it != pl.fillT.end() && *it <= hi; ++it)
            fn(kFillK, (int)(it - pl.fillT.begin()));
    }
}
// Is a site / vehicle there at t (the deployment era; always when the era is off)?
static bool onlineAt(const SatelliteSim::LaunchState &L, double onlineT, double t) { return !L.deployOn || onlineT <= t; }

// The plane's normal at t (from the batch's first member's slot).
static glm::dvec3 planeNormal(const SatelliteSim &sim, const LPlane &pl, int b, int payload, double t)
{
    const size_t m0 = std::min(pl.sats.size() - 1, (size_t)b * (size_t)payload);
    const SatOrbitState st = satOrbitStateAt(sim.rosterElemsOf(sim.satOrbits[pl.sats[m0]]), t);
    return glm::normalize(glm::cross(st.posEci, st.velEci));
}

static int payloadOf(const SatelliteSim &sim, const SatelliteSim::LaunchState &L, const LPlane &pl)
{
    const launch::Program &pg = L.cat.programs[pl.prog];
    const int cluster = std::max(1, sim.constellations[pl.constIdx].clusterSize);
    return std::max(cluster, pg.payload / cluster * cluster);
}

// ── Visiting vehicles' phasing (2026-10-21) ─────────────────────────────────────────────────────────────
// The far phases of a spacecraft's rendezvous (buildCrew): a coast c1 in the parking orbit r0, a Hohmann to a phasing
// orbit rP, a wait W there, a Hohmann to the station's orbit rS arriving 5 km behind it. crewCatchup = the angle the
// spacecraft gains on the station over D (separation to that arrival); the station's lead at separation (+ whole turns)
// must equal it. One copy, used by the launch-time choice (candidate) and the flight (buildCrew).
struct CrewPhaseGeom
{
    double r0 = 0.0, n0 = 0.0, rS = 0.0, nS = 0.0, c1 = 0.0, vs = 0.0, rPmax = 0.0;
};
static double crewHohmannT(double ra, double rb)
{
    const double a = 0.5 * (ra + rb);
    return glm::pi<double>() * std::sqrt(a * a * a / launch::kGM);
}
static double crewCatchup(const CrewPhaseGeom &g, double rP, double D, double *Wout)
{
    const double T1 = crewHohmannT(g.r0, rP), T2 = crewHohmannT(rP, g.rS);
    const double W = D - g.c1 - T1 - T2;
    if (Wout)
        *Wout = W;
    return (g.n0 + g.vs - g.nS) * g.c1 + (glm::pi<double>() - g.nS * T1) + (launch::meanMotion(rP) - g.nS) * W +
           (glm::pi<double>() - g.nS * T2);
}
// The approach's legs in the station's frame (buildCrew): 5 km behind -> 1 km out, the arc onto the port's axis, to
// 400 m, hold, to 200 m, hold, to 20 m, hold, contact. The fast track flies them in ~65 min (Soyuz / Progress /
// Shenzhou / Tianzhou automated approaches), the ~1-day profile in ~2.7 h.
static constexpr double kCrewApproachLegs[2][9] = {{1800.0, 1500.0, 900.0, 900.0, 600.0, 900.0, 1200.0, 600.0, 400.0},
                                                   {900.0, 600.0, 420.0, 240.0, 300.0, 240.0, 600.0, 300.0, 300.0}};
// (2026-10-21) A berthing vehicle's legs after the axis (buildCrew): to 250 m, hold, to 30 m, hold (go for capture),
// to the capture point, free drift (grappled after kBerthGrappleS), on the arm to 1.5 m, berthing (the last 1.5 m).
static constexpr double kBerthLegs[8] = {1500.0, 1200.0, 1800.0, 900.0, 900.0, 1500.0, 3000.0, 1200.0};
static constexpr double kBerthGrappleS = 300.0;     // in free drift at the capture / release point until the grapple / release
static constexpr double kBerthReleaseHoldS = 600.0; // the release point: held, released at + kBerthGrappleS, drifting
static constexpr double kArmMoveS = 1800.0;         // the arm from its park to the vehicle (or back)
static constexpr double kArmOpenS = 120.0;          // its end effector closing / opening (0.6 m along the fixture's normal)
static constexpr double kArmBoltS = 3600.0;         // berthed: the arm holds on while the bolts drive
static double crewApproachS(bool fast, bool berth = false)
{
    double s = 0.0;
    const double *lg = kCrewApproachLegs[fast ? 1 : 0];
    for (int i = 0; i < (berth ? 2 : 9); ++i)
        s += lg[i];
    if (berth)
        for (double d : kBerthLegs)
            s += d;
    return s;
}
static double crewC1(uint64_t seed, bool fast)
{
    return fast ? 900.0 + 600.0 * h01(seed, 40, 1) : 2400.0 + 1200.0 * h01(seed, 40, 1);
}
// Is there a far-phase duration D in [Dlo, Dhi] that closes the lead lag0 (in [0, 2 pi): no extra turns beyond the
// first that the phasing orbit's range allows)? The middle of the first feasible stretch (margin either side).
static bool crewFastD(const CrewPhaseGeom &g, double lag0, double Dlo, double Dhi, double &Dout)
{
    const double Dmin = g.c1 + std::max(crewHohmannT(g.r0, g.r0) + crewHohmannT(g.r0, g.rS),
                                        crewHohmannT(g.r0, g.rPmax) + crewHohmannT(g.rPmax, g.rS)) + 60.0;
    Dlo = std::max(Dlo, Dmin);
    double a = -1.0, b = -1.0;
    for (double D = Dlo; D <= Dhi; D += 20.0)
    {
        double lag = lag0;
        while (lag < crewCatchup(g, g.rPmax, D, nullptr))
            lag += glm::two_pi<double>();
        const bool ok = lag <= crewCatchup(g, g.r0, D, nullptr);
        if (ok)
        {
            if (a < 0.0)
                a = D;
            b = D;
        }
        else if (a >= 0.0)
            break;
    }
    if (a < 0.0)
        return false;
    Dout = 0.5 * (a + b);
    return true;
}

// A batch's launch before deconfliction: a hashed vehicle and site, the first the plane passes over after the nominal
// time (arrival - raise - the flight to deployment). Cached.
static SatelliteSim::LaunchState::Cand candidate(const SatelliteSim &sim, SatelliteSim::LaunchState &L, int plane,
                                                 int64_t k, int b)
{
    const uint64_t key = batchKey(plane, k, b);
    {
        std::shared_lock<std::shared_mutex> lk(L.candsMx);
        auto it = L.cands.find(key);
        if (it != L.cands.end())
            return it->second;
    }
    SatelliteSim::LaunchState::Cand c;
    const LPlane &pl = L.planes[plane];
    const launch::Program &pg = L.cat.programs[pl.prog];
    const double lifeS = progLifeS(pg, (double)sim.launchLifeScale);
    const uint64_t seed = batchSeed(pl, k, b);
    const double tA = arrivalOf(pl, k, b, lifeS);
    const double raise = (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(seed, 2, 1)) * kDay;
    const double tNom = tA - raise;
    const int payload = payloadOf(sim, L, pl);
    auto normalAt = [&](double t) { return planeNormal(sim, pl, b, payload, t); };
    // The deployment era: before a block's early_until its early vehicles (Falcon 9 before Starship); only vehicles
    // and sites already flying.
    const std::vector<int> *vehList = &pg.vehicles;
    if (L.deployOn && pl.cohort >= 0)
    {
        const launch::Deployment &dp = pg.deploy[(size_t)L.cohorts[(size_t)pl.cohort].dep];
        if (!dp.earlyVehicles.empty() && tNom < dp.earlyUntilT)
            vehList = &dp.earlyVehicles;
    }
    const size_t nv = vehList->size();
    for (size_t vi = 0; vi < nv && c.tL < 0.0; ++vi)
    {
        const int veh = (*vehList)[(vi + (size_t)(h01(seed, 3, 1) * nv)) % nv];
        const launch::Vehicle &vh = L.cat.vehicles[veh];
        if (!onlineAt(L, vh.onlineT, tNom))
            continue;
        const double lead = vh.t2 + vh.deployAfter;
        const size_t ns = vh.sites.size();
        for (size_t si = 0; si < ns && c.tL < 0.0; ++si)
        {
            const int site = vh.sites[(si + (size_t)(h01(seed, 4, 1) * ns)) % ns];
            if (!onlineAt(L, L.cat.sites[site].onlineForVehicle(vh.id), tNom - lead))
                continue;
            double a = 0.0;
            const double t = nextCrossing(L.cat.sites[site], normalAt, tNom - lead, a);
            if (t >= 0.0)
            {
                c.tL = t;
                c.az = a;
                c.veh = veh;
                c.site = site;
            }
        }
    }
    // A fast-track visiting vehicle (2026-10-21): Soyuz / Progress / Shenzhou / Tianzhou launch on the pass that puts
    // the station at the right phase, so they dock 2-7 h after lift-off; the passes of the next 30 days are tried in
    // order (each a pure function of the batch) and the first that allows one of the program's windows is taken.
    if (pg.crew && !pg.fastDockH.empty() && c.tL >= 0.0 && !pl.sats.empty() && pl.sats[0] < sim.activeSatCount)
    {
        const launch::Vehicle &vh = L.cat.vehicles[c.veh];
        const launch::Site &site = L.cat.sites[c.site];
        const SatOrbitElems se = sim.rosterElemsOf(sim.satOrbits[pl.sats[0]]);
        const double t2 = std::max(vh.t2, vh.t1 + 60.0), sepAfter = t2 + vh.deployAfter;
        const double r0 = kR + pg.parkKm * 1000.0;
        const double thCap = L.upperTrackFor(c.veh, pg.parkKm).at(t2).x / kR + launch::meanMotion(r0) * vh.deployAfter;
        const double appr = crewApproachS(true);
        double t = c.tL, a = c.az;
        double best[4] = {-1.0, -1.0, -1.0, -1.0}, bestAz[4] = {0.0, 0.0, 0.0, 0.0}, bestDock[4] = {0.0, 0.0, 0.0, 0.0};
        double firstAny = -1.0;
        for (int j = 0; j < 64 && t >= 0.0 && t < c.tL + 30.0 * kDay; ++j)
        {
            const glm::dvec3 N = normalAt(t);
            const glm::dvec3 S = ecefToEci(site.ecef, t);
            const glm::dvec3 U0 = glm::normalize(S - glm::dot(S, N) * N), A0 = glm::cross(N, U0);
            const SatOrbitState so = satOrbitStateAt(se, t + sepAfter);
            CrewPhaseGeom g;
            g.r0 = r0;
            g.n0 = launch::meanMotion(r0);
            g.rS = glm::length(so.posEci);
            g.nS = se.meanMot > 0.0 ? se.meanMot : launch::meanMotion(g.rS);
            g.c1 = crewC1(seed, true);
            g.vs = 0.4 / r0;
            g.rPmax = std::max(r0, g.rS - 8000.0);
            const double thSt = std::atan2(glm::dot(so.posEci, A0), glm::dot(so.posEci, U0));
            const double lag0 = wrap2pi(thSt - thCap - 5000.0 / g.rS);
            // The program's windows in order of preference (Soyuz / Progress: 2-orbit, then 4-orbit): the first pass that
            // allows the first window is taken if it comes within 14 days of the first pass allowing any.
            for (size_t wi = 0; wi < pg.fastDockH.size() && wi < 4; ++wi)
            {
                const auto &w = pg.fastDockH[wi];
                double D = 0.0;
                if (best[wi] < 0.0 &&
                    crewFastD(g, lag0, w.first * 3600.0 - sepAfter - appr, w.second * 3600.0 - sepAfter - appr, D))
                {
                    best[wi] = t;
                    bestAz[wi] = a;
                    bestDock[wi] = sepAfter + D + appr;
                }
            }
            if (firstAny < 0.0)
                for (size_t wi = 0; wi < 4; ++wi)
                    if (best[wi] >= 0.0)
                        firstAny = t;
            if (best[0] >= 0.0 || (firstAny >= 0.0 && t > firstAny + 14.0 * kDay))
                break;
            t = nextCrossing(site, normalAt, t + 3600.0, a);
        }
        int pick = -1;
        if (best[0] >= 0.0 && best[0] <= firstAny + 14.0 * kDay)
            pick = 0;
        else
            for (int wi = 0; wi < 4; ++wi)
                if (best[wi] >= 0.0 && (pick < 0 || best[wi] < best[pick]))
                    pick = wi;
        if (pick >= 0)
        {
            c.tL = best[pick];
            c.az = bestAz[pick];
            c.dockAfter = bestDock[pick];
        }
    }
    std::unique_lock<std::shared_mutex> lk(L.candsMx);
    if (L.cands.size() > 200000)
        L.cands.clear();
    L.cands.emplace(key, c);
    return c;
}

// The rivals at a site in one day of candidate launch times (LaunchState::rivalIdx): every batch of the planes flying
// from the site whose arrival is in the widest window any query that day can ask about, kept when its candidate is at
// the site that day. Built once, shared (a race builds it twice, identically).
static constexpr double kRivalBucketS = 86400.0;
// (2026-10-21b) Set on the background build's threads (LaunchState::BuildJob): their helpers run below normal
// priority and leave two cores to the frame.
static inline thread_local bool gBuildBg = false;
static std::shared_ptr<const std::vector<SatelliteSim::LaunchState::RivalEntry>>
rivalBucket(const SatelliteSim &sim, SatelliteSim::LaunchState &L, int site, int64_t beta, bool parallel = false)
{
    const uint64_t ik = ((uint64_t)(uint32_t)site << 40) ^ ((uint64_t)beta & 0xFFFFFFFFFFull);
    {
        std::lock_guard<std::mutex> lk(L.rivalMx);
        auto it = L.rivalIdx.find(ik);
        if (it != L.rivalIdx.end())
            return it->second;
    }
    auto v = std::make_shared<std::vector<SatelliteSim::LaunchState::RivalEntry>>();
    const double b0 = (double)beta * kRivalBucketS, b1 = b0 + kRivalBucketS;
    const std::vector<int> &sitePlanes = L.planesBySite[(size_t)site];
    auto planeEntries = [&](int q, std::vector<SatelliteSim::LaunchState::RivalEntry> &out) {
        const LPlane &pq = L.planes[(size_t)q];
        const launch::Program &gq = L.cat.programs[pq.prog];
        const double lifeQ = progLifeS(gq, (double)sim.launchLifeScale);
        const double lo = b0 - 5400.0 + (gq.raiseDays[0] - 1.0 - gq.earlyDays()) * kDay,
                     hi = b1 + 5400.0 + (gq.raiseDays[1] + 3.0) * kDay;
        forBatches(L, pq, lifeQ, lo, hi, [&](int64_t k, int b) {
            const SatelliteSim::LaunchState::Cand co = candidate(sim, L, q, k, b);
            if (co.site != site || co.tL < b0 || co.tL >= b1)
                return;
            const uint64_t key = batchKey(q, k, b);
            out.push_back({co.tL, arrivalOf(pq, k, b, lifeQ), h01(key, 9, 9), key, q});
        });
    };
    // (2026-10-21) A bucket wanted on its own (at 1 mo/s - 1 yr/s, one new day a frame) is split over the planes in
    // parallel: ~40 ms on one thread at a Starship pad (~2000 planes). Same entries in the same (plane) order.
    const size_t hw = buildThreads();
    const size_t workers = parallel ? std::min(hw, sitePlanes.size() / 64) : 1;
    if (workers <= 1)
    {
        for (int q : sitePlanes)
            planeEntries(q, *v);
    }
    else
    {
        std::vector<std::vector<SatelliteSim::LaunchState::RivalEntry>> per(sitePlanes.size());
        std::atomic<size_t> next{0};
        const bool bg = gBuildBg;
        auto run = [&, bg] {
            if (bg)
            {
                gBuildBg = true;
                kessler::lowerThreadPriority();
            }
            for (size_t i = next.fetch_add(1); i < sitePlanes.size(); i = next.fetch_add(1))
                planeEntries(sitePlanes[i], per[i]);
        };
        std::vector<std::thread> th;
        for (size_t w = 1; w < workers; ++w)
            th.emplace_back(run);
        run();
        for (std::thread &x : th)
            x.join();
        for (const auto &pv : per)
            v->insert(v->end(), pv.begin(), pv.end());
    }
    std::lock_guard<std::mutex> lk(L.rivalMx);
    if (L.rivalIdx.size() > 20000)
        L.rivalIdx.clear();
    return L.rivalIdx.emplace(ik, v).first->second;
}

// A returning ship's landing (2026-10-19; the user: SpaceX's 2036 plan is full reuse, the ship caught by the tower).
// Its deorbit burn is timed so that its entry ends 120-450 km short of a Starship pad along its ground track (and
// within 200 km of it across): the first such moment in the 72 h after deployment (30-s steps over the burn time,
// then 2-s steps around the best), a pure function of the batch. The entry itself is the same closed form as before;
// the descent (shipDescentEcef) then bends from wherever it ends onto the pad. With no pad in reach in 72 h, the old
// burn time and a touchdown on the sea 250 km past the entry's end.
template <class F> static void planShipLanding(SatelliteSim &sim, SatelliteSim::LaunchState &L, LBatch &B, F &&setDeorbit)
{
    FPROF_SCOPE("launch.planShipLanding");
    (void)sim;
    const double tD0 = B.tDeorbBurn;
    const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
    // The entry's end (te = kShipEntryS) at the current burn: its angle in the plane past the entry interface.
    const glm::dvec3 P480 = shipEntry(B, kShipEntryS, nullptr, nullptr);
    double dTh = std::atan2(glm::dot(P480, B.A0), glm::dot(P480, B.U0)) - B.thEI;
    dTh -= glm::two_pi<double>() * std::floor(dTh / glm::two_pi<double>() + 0.5);
    const double hEnd = glm::length(P480) - kR;
    double vEnd = 0.0;
    shipEntry(B, kShipEntryS, &vEnd, nullptr);
    // The end (Earth-fixed unit position, direction over the ground) for a burn at Td: only the phase moves.
    auto endAt = [&](double Td, glm::dvec3 &e, glm::dvec3 &dirT) {
        const double th = B.thEI + B.nPark * (Td - tD0) + dTh;
        const double tE = B.tStageGone + (Td - tD0) + kShipEntryS;
        const glm::dvec3 r = (kR + hEnd) * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
        const glm::dvec3 vI = vEnd * (-std::sin(th) * B.U0 + std::cos(th) * B.A0);
        const glm::dvec3 rE = eciToEcef(r, tE), vE = eciToEcef(vI, tE) - glm::cross(omega, rE);
        e = glm::normalize(rE);
        dirT = vE - glm::dot(vE, e) * e;
        dirT = glm::length(dirT) > 1e-6 ? glm::normalize(dirT) : glm::dvec3(1, 0, 0);
    };
    std::vector<int> pads;
    for (size_t p = 0; p < L.pads.size(); ++p)
        if (L.pads[p].kind == 2)
            pads.push_back((int)p);
    auto score = [&](double Td, int &best) {
        glm::dvec3 e, dT;
        endAt(Td, e, dT);
        const glm::dvec3 c = glm::cross(e, dT);
        double sBest = 1e30;
        best = -1;
        for (int p : pads)
        {
            const glm::dvec3 g = L.pads[p].g;
            if (glm::dot(g, e) < 0.98 || padKindAt(L.pads[p], Td, L.deployOn) != 2) // (a catch tower there by then)
                continue;
            const double a = kR * glm::dot(g - e, dT), x = kR * glm::dot(g - e, c);
            if (a < 120e3 || a > 450e3 || std::abs(x) > 200e3)
                continue;
            const double sc = std::abs(x) + 0.3 * std::abs(a - 260e3);
            if (sc < sBest)
            {
                sBest = sc;
                best = p;
            }
        }
        return sBest;
    };
    double TdBest = -1.0;
    int padBest = -1;
    // (2026-10-20, hitches at a high warp: this scan was ~0.5 ms a ship, ~100 ms a frame at 1 yr/s) Exact skip: score()
    // needs a pad within acos(0.98) of the entry's end e(Td), and e moves over the ground at most |nPark| + the Earth's
    // rate, so from a step where the nearest pad is farther, the steps that cannot reach it are passed over (the same
    // 30-s grid, the same additions: the same answer).
    const double eRate = std::abs(B.nPark) + 7.2921159e-5;
    const double angLim = std::acos(0.98);
    for (double Td = tD0; Td < tD0 + 72.0 * 3600.0 && TdBest < 0.0; Td += 30.0)
    {
        {
            glm::dvec3 e, dT;
            endAt(Td, e, dT);
            double maxDot = -1.0;
            for (int pp : pads)
                maxDot = std::max(maxDot, glm::dot(L.pads[pp].g, e));
            if (maxDot < 0.98)
            {
                const double ang = std::acos(glm::clamp(maxDot, -1.0, 1.0));
                const double gap = (ang - angLim) / eRate - 1e-3;
                for (int j = 0, nSkip = (int)std::floor(gap / 30.0); j < nSkip; ++j)
                    Td += 30.0;
                continue;
            }
        }
        int p;
        if (score(Td, p) < 1e29)
        {
            double sMin = 1e30;
            for (double T2 = Td; T2 < Td + 600.0; T2 += 2.0)
            {
                int q;
                const double sc = score(T2, q);
                if (sc < sMin)
                {
                    sMin = sc;
                    TdBest = T2;
                    padBest = q;
                }
            }
        }
    }
    glm::dvec3 tgt;
    if (TdBest >= 0.0)
    {
        B.tDeorbBurn = TdBest;
        setDeorbit();
        B.landPad = padBest;
        tgt = L.pads[padBest].g;
        B.catchH = L.pads[padBest].elevM + kShipCatchAboveGround;
    }
    else
    {
        glm::dvec3 e, dT;
        endAt(tD0, e, dT);
        const double a = 250e3 / kR;
        tgt = std::cos(a) * e + std::sin(a) * dT;
        B.landPad = -1;
        B.catchH = 0.0;
    }
    B.LG = glm::normalize(tgt);
    B.LE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), B.LG));
    B.LN = glm::cross(B.LG, B.LE);
    // Where the entry hands over: its state at te = kShipEntryS (Earth-fixed, from its last second).
    B.tShipDesc = B.tStageGone + kShipEntryS;
    const glm::dvec3 Q1 = eciToEcef(shipEntry(B, kShipEntryS, nullptr, nullptr), B.tShipDesc);
    const glm::dvec3 Q0 = eciToEcef(shipEntry(B, kShipEntryS - 1.0, nullptr, nullptr), B.tShipDesc - 1.0);
    glm::dvec2 x1, x0;
    double h1, h0;
    landCoords(B, Q1, x1, h1);
    landCoords(B, Q0, x0, h0);
    B.dX0 = x1;
    B.dH0 = h1;
    B.dV0 = x1 - x0;
    B.dVz0 = h1 - h0;
    // F: 1.1 km over the catch point and 400 m back along the approach, falling belly first at ~75 m/s.
    const glm::dvec2 appr = glm::length(x1) > 1.0 ? -glm::normalize(x1) : glm::dvec2(1.0, 0.0);
    B.dXF = -400.0 * appr;
    B.dHF = B.catchH + 1100.0;
    B.dVF = 20.0 * appr;
    B.dVzF = -75.0;
    // Durations: the horizontal Hermite's start tangent stays under 3x the chord (no loop back) at 2x; the landing
    // burn's likewise.
    B.T1 = glm::clamp(2.0 * glm::length(B.dXF - B.dX0) / std::max(glm::length(B.dV0), 200.0), 150.0, 1000.0);
    const double LF = std::sqrt(glm::dot(B.dXF, B.dXF) + (B.dHF - B.catchH) * (B.dHF - B.catchH));
    B.T2 = glm::clamp(2.0 * LF / std::sqrt(glm::dot(B.dVF, B.dVF) + B.dVzF * B.dVzF), 15.0, 60.0);
    B.tShipFlip = B.tShipDesc + B.T1;
    B.tShipCatch = B.tShipFlip + B.T2;
    B.tShipEnd = B.tShipCatch + (B.landPad >= 0 ? kCaughtHoldS : 30.0);
    B.shipLands = true;
}

// Builds a batch: where and when it launches, its trajectory frame, each member's climb and its predecessor's fall.
static void buildBatch(SatelliteSim &sim, SatelliteSim::LaunchState &L, LBatch &B, const LPlane &pl, double lifeS)
{
    FPROF_SCOPE("launch.buildBatch");
    const launch::Program &pg = L.cat.programs[pl.prog];
    B.prog = pl.prog;
    B.seed = batchSeed(pl, B.k, B.b);
    B.first = B.k == kFillK;
    B.tA = arrivalOf(pl, B.k, B.b, lifeS);
    if (!batchExists(L, pl, B.k, B.b, lifeS))
    {
        B.why = B.k == kFillK ? "no such first fill" : "before the plane's first deployment";
        return;
    }
    // Members: a contiguous run of the plane's slots.
    const int cluster = std::max(1, sim.constellations[pl.constIdx].clusterSize);
    const int payload = std::max(cluster, pg.payload / cluster * cluster);
    const size_t m0 = (size_t)B.b * (size_t)payload, m1 = std::min(pl.sats.size(), m0 + (size_t)payload);
    if (m0 >= m1)
    {
        B.why = "empty";
        return;
    }
    // Stood down at the Sun's Fury for certain (its lift-off is after the CME whatever its pad): no candidate, no rivals
    // (2026-10-21d: after the CME, at 1 week/s, these were 4-8 ms of the walk's builds a frame, every one stood down).
    if (B.tA >= standDownArrival(sim, pg, maxVehicleLeadS(L)))
    {
        B.why = "launches stand down at the Sun's Fury";
        return;
    }
    const SatOrbitElems e0 = sim.rosterElemsOf(sim.satOrbits[pl.sats[m0]]);
    auto normalAt = [&](double t) {
        const SatOrbitState st = satOrbitStateAt(e0, t);
        return glm::normalize(glm::cross(st.posEci, st.velEci));
    };
    // Vehicle, site and time: the candidate (the plane's first pass over a site its vehicle flies from), then pad
    // deconfliction: a launch whose pad another launch (of higher priority, by hash) takes within 90 min takes the
    // plane's next pass instead. The others' times are their candidates, never their deconflicted times: no recursion,
    // still a pure function of time.
    const SatelliteSim::LaunchState::Cand c0 = candidate(sim, L, B.plane, B.k, B.b);
    if (c0.tL < 0.0)
    {
        B.why = "no site reaches the plane";
        return;
    }
    B.veh = c0.veh;
    B.site = c0.site;
    double tL = c0.tL, az = c0.az;
    // (2026-10-21d) A candidate after the CME stands down whatever its rank (ranks only move it later): skip the rivals.
    if (standsDownAt(sim, c0.tL))
    {
        B.why = "launches stand down at the Sun's Fury";
        return;
    }
    {
        // The launches competing for this pad around this pass (their candidates): this one waits as many passes as
        // there are rivals of higher priority (a hash), so the cluster goes out on successive passes (~a day apart).
        FPROF_SCOPE("launch.rivals");
        const uint64_t self = batchKey(B.plane, B.k, B.b);
        const double myPri = h01(self, 9, 9);
        int rank = 0;
        // (The batches of the planes flying from this site whose arrival is in [tL + raise0 - 1 d, tL + raise1 + 3 d]
        // and whose candidate is at this site within 90 min; from the day index, rivalBucket. B.rivals counts those
        // within the 90 min, since 2026-10-20: it was every batch looked at.)
        const int64_t be0 = (int64_t)std::floor((c0.tL - 5400.0) / kRivalBucketS);
        const int64_t be1 = (int64_t)std::floor((c0.tL + 5400.0) / kRivalBucketS);
        for (int64_t be = be0; be <= be1; ++be)
        {
            const auto bucket = rivalBucket(sim, L, B.site, be);
            for (const SatelliteSim::LaunchState::RivalEntry &e : *bucket)
            {
                if (e.key == self || !(std::abs(e.tL - c0.tL) < 5400.0))
                    continue;
                const launch::Program &gq = L.cat.programs[L.planes[(size_t)e.plane].prog];
                const double lo = tL + (gq.raiseDays[0] - 1.0 - gq.earlyDays()) * kDay,
                             hi = tL + (gq.raiseDays[1] + 3.0) * kDay;
                if (e.tA < lo || e.tA > hi)
                    continue;
                ++B.rivals;
                if (e.pri > myPri)
                    ++rank;
            }
        }
        // A rank first moves the launch to the vehicle's next pad that reaches the plane, then (past the pads) to
        // later passes.
        // (a fast-track visiting vehicle keeps its pass: its phasing to the station needs it; the others move)
        if (rank > 0 && c0.dockAfter < 0.0)
        {
            const launch::Vehicle &vh0 = L.cat.vehicles[B.veh];
            const double lead = vh0.t2 + vh0.deployAfter;
            const double tNom = B.tA - (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(B.seed, 2, 1)) * kDay;
            std::vector<std::pair<int, double>> pads; // (site, its first pass), the candidate's pad first
            std::vector<double> pAz;
            pads.push_back({B.site, c0.tL});
            pAz.push_back(c0.az);
            // (2026-10-20, the deployment era's ~90 Starship launches a day) the alternatives in the vehicle's site list
            // order starting AFTER the candidate's pad: the launches pushed off one pad (distinct ranks) land on distinct
            // pads, and those pushed off different pads start from different places. (From the list's head they all
            // piled onto its first pads: 20 a day on Louisiana A-1.)
            const size_t ns0 = vh0.sites.size();
            size_t at0 = 0;
            for (size_t q = 0; q < ns0; ++q)
                if (vh0.sites[q] == B.site)
                    at0 = q;
            for (size_t q = 1; q < ns0; ++q)
            {
                const int st = vh0.sites[(at0 + q) % ns0];
                if (st == B.site || !onlineAt(L, L.cat.sites[st].onlineForVehicle(vh0.id), tNom - lead))
                    continue;
                double a2 = 0.0;
                const double t2 = nextCrossing(L.cat.sites[st], normalAt, tNom - lead, a2);
                if (t2 >= 0.0)
                {
                    pads.push_back({st, t2});
                    pAz.push_back(a2);
                }
            }
            const int np = (int)pads.size();
            B.site = pads[rank % np].first;
            tL = pads[rank % np].second;
            az = pAz[rank % np];
            for (int r = 0; r < rank / np; ++r)
            {
                double a2 = 0.0;
                const double t2 = nextCrossing(L.cat.sites[B.site], normalAt, tL + 5400.0, a2);
                if (t2 < 0.0)
                {
                    B.why = "pad taken";
                    return;
                }
                tL = t2;
                az = a2;
            }
            B.shifts = rank;
        }
    }
    // Kessler: launches stand down at the storm. By default for good: the cascade's shells are being ground into
    // debris, and no operator would send replacements into them (a slot the storm or the cascade kills is not refilled
    // either way). "After the storm: resume" brings them back three weeks on (the trains are not engine objects: they
    // do not collide, whatever the debris).
    if (sim.kesslerEnabled && tL >= sim.kesslerEpoch() &&
        (sim.launchAfterStorm == 0 || tL < sim.kesslerEpoch() + 21.0 * kDay))
    {
        B.why = sim.launchAfterStorm == 0 ? "launches stand down at the Sun's Fury" : "the stand-down after the Sun's Fury";
        return;
    }
    // A crewed station the cascade or the player killed (dead, destroyed or re-entered) before lift-off gets no more
    // crews (issue 20261019_231126). A flight already up keeps flying (launchOngoing reports the station lost). Read
    // from the roster word mirrored from the event log, so a pure function of time and the log; crewSync rebuilds the
    // plane's batches when the word changes.
    if (pg.crew && !pl.sats.empty() && pl.sats[0] < sim.activeSatCount && 2ull * pl.sats[0] + 1 < sim.kRosterWords.size())
    {
        // (lost = dead or ended, whichever first: a breakup ends a satellite without a dead second)
        const int32_t dead = std::min(sim.kRosterWords[2ull * pl.sats[0]], sim.kRosterWords[2ull * pl.sats[0] + 1]);
        if (sim.kesslerEnabled && dead != kessler::kNever && (double)dead <= secOfKessler(sim, tL))
        {
            B.why = "station lost";
            return;
        }
    }
    // The deployment era: no crew before the station is up (or after it is retired).
    if (pg.crew && !pl.sats.empty() && pl.sats[0] < sim.activeSatCount && sim.rosterFateAt(pl.sats[0], tL) >= 4)
    {
        B.why = "station not there yet";
        return;
    }
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Site &st = L.cat.sites[B.site];
    B.tL = tL;
    B.azDeg = az;
    B.padBase = (B.site >= 0 && B.site < (int)L.sitePad.size() && L.sitePad[B.site] >= 0
                     ? L.pads[L.sitePad[B.site]].elevM
                     : 0.0) +
                vh.mountM;
    B.t2 = std::max(vh.t2, vh.t1 + 60.0);
    B.tD = tL + B.t2 + vh.deployAfter;
    if (pg.crew)
    {
        // A crew flight: docking raise_days after lift-off, or (a fast track) when the candidate's phasing says;
        // buildCrew's rendezvous sets the real docking.
        B.crewFast = c0.dockAfter > 0.0 && tL == c0.tL;
        B.tA = B.crewFast ? tL + c0.dockAfter
                          : std::max(tL + (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(B.seed, 2, 1)) * kDay,
                                     B.tD + 600.0);
    }
    else if (B.tD >= B.tA - 0.5 * kDay)
    {
        B.why = "launch window too late";
        return;
    }
    // Frames.
    B.N = normalAt(tL);
    const glm::dvec3 S = ecefToEci(st.ecef, tL);
    B.U0 = glm::normalize(S - glm::dot(S, B.N) * B.N);
    B.A0 = glm::normalize(glm::cross(B.N, B.U0));
    B.G0 = st.ecef;
    {
        const glm::dvec3 Ae = eciToEcef(B.A0, tL);
        B.Eaz = glm::normalize(Ae - glm::dot(Ae, B.G0) * B.G0);
    }
    planRecovery(L, B, pg);
    B.rPark = kR + pg.parkKm * 1000.0;
    B.nPark = launch::meanMotion(B.rPark);
    const Track &up = L.upperTrackFor(B.veh, pg.parkKm);
    B.thIns = up.at(B.t2).x / kR;
    const double thD = B.thIns + B.nPark * vh.deployAfter;
    // Members.
    const double T = B.tA - B.tD;
    B.mem.clear();
    for (size_t m = m0; m < m1 && !pg.crew; ++m) // a crew flight carries no train
    {
        LMember mb;
        mb.sat = pl.sats[m];
        const SatOrbitElems e = sim.rosterElemsOf(sim.satOrbits[mb.sat]);
        mb.rSlot = e.rSatM;
        mb.nSlot = e.meanMot > 0.0 ? e.meanMot : launch::meanMotion(e.rSatM);
        // A formation (Starmind's rings of 8: one ship = one cluster) raises as ONE group (2026-10-20, mission-ops suite):
        // every member of a cluster takes its leader's schedule (the wait, the ramp, the residual), so their paths are
        // the cluster's own path + each one's ring offset, and once the release offsets have eased out (two days) the
        // eight fly in ring formation all the way up. With a schedule each (a hashed ramp per member) they spread
        // thousands of km along the orbit during the climb and met only at the arrival, each from its own side.
        if (cluster > 1 && !gPerMemberRaise && (m - m0) % (size_t)cluster != 0 && !B.mem.empty())
        {
            const LMember &ld = B.mem[(m - m0) / (size_t)cluster * (size_t)cluster];
            mb.tRamp = ld.tRamp;
            mb.sRaise = ld.sRaise;
            mb.eps = ld.eps;
            const uint64_t ms = B.seed ^ ((uint64_t)m * 0x9E3779B97F4A7C15ull);
            mb.tEnd = B.first ? B.tA
                              : B.tA + (pg.deorbitDays[0] + (pg.deorbitDays[1] - pg.deorbitDays[0]) * h01(ms, 6, 1)) * kDay;
            B.mem.push_back(mb);
            continue;
        }
        const glm::dvec3 r = satOrbitStateAt(e, B.tD).posEci;
        const double thSlot = std::atan2(glm::dot(r, B.A0), glm::dot(r, B.U0));
        const uint64_t ms = B.seed ^ ((uint64_t)m * 0x9E3779B97F4A7C15ull);
        mb.tRamp = T * (0.55 + 0.25 * h01(ms, 5, 1));
        const double W = T - mb.tRamp;
        // (the climb's rate ends at the slot's own mean motion, not Kepler's at its radius: 2026-10-20)
        const double kj = mb.nSlot / launch::meanMotion(mb.rSlot);
        const double rampLag = launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, mb.tRamp, 1.0, kj) - mb.nSlot * mb.tRamp;
        const double dn = B.nPark - mb.nSlot;
        // The train leaves the stage as a tight string (~150 m apart), then each member waits its own time in the
        // parking orbit so that the climb ends exactly on its slot.
        const double lag0 = wrap2pi(thSlot - thD + 2.2e-5 * (double)(m - m0));
        const double lo = dn * 0.25 * kDay + rampLag;
        double lag = lag0;
        if (dn > 0.0)
            while (lag < lo)
                lag += glm::two_pi<double>();
        const double sNeeded = dn > 1e-12 ? (lag - rampLag) / dn : 0.0;
        if (dn > 1e-12 && sNeeded >= 0.0 && sNeeded <= W)
        {
            mb.sRaise = B.tD + sNeeded;
            mb.eps = 0.0;
        }
        else
        {
            const double s = dn > 1e-12 ? std::clamp(sNeeded, 0.0, W) : 0.0;
            mb.sRaise = B.tD + s;
            mb.eps = lag - (dn * s + rampLag);
        }
        // (a first fill replaces nobody: its "retiree" ends as it starts, so it is never up)
        mb.tEnd = B.first ? B.tA
                          : B.tA + (pg.deorbitDays[0] + (pg.deorbitDays[1] - pg.deorbitDays[0]) * h01(ms, 6, 1)) * kDay;
        B.mem.push_back(mb);
    }
    // The upper stage after deployment: a deorbit burn 30-90 min later onto an ellipse whose perigee is in the air,
    // its apoapsis at the burn; it reaches the entry interface (100 km) about half an orbit on. Every stage does it:
    // an expendable one re-enters as a fireball, a returning one (Starship's ship) flies its entry glow. Until
    // 2026-10-19 only the expendable ones burned (40 s, while the stage sank 30 km on its circle) and Starship's ship
    // jumped from its parking orbit to 100 km at the entry interface 40 min after deploying.
    // (not before the last member has left the dispenser and drifted clear: a Starship train takes ~20-30 min)
    // (the deployment era: an early vehicle — Falcon 9 before Starship — flies the program's satellites without its
    // stack: they leave from the dispenser point, stowed)
    static const launch::Stack kNoStack{};
    const bool earlyVeh = std::find(pg.vehicles.begin(), pg.vehicles.end(), B.veh) == pg.vehicles.end();
    const launch::Stack &stk = earlyVeh ? kNoStack : pg.stack;
    B.deployInterval = stk.intervalS >= 0.0 ? stk.intervalS : vh.deployIntervalS;
    // (2026-10-21) Far enough apart that two members never come within kReleaseGapM in free flight: they follow one
    // trajectory, deployInterval apart (a Falcon 9 dispenser point: ~67 s; Starship's door: its 45 s).
    const double relSp = stk.sepMps >= 0.0 ? stk.sepMps : vh.sepSpeedMps;
    const glm::dvec2 relV = releaseInPlane(stk.layout, relSp);
    if (B.mem.size() > 1 && relSp > 0.0)
        B.deployInterval = std::max(B.deployInterval, kReleaseGapM / std::max(releaseMinRate(relV), 1e-3));
    B.tDeorbBurn = std::max(B.tD + 1800.0 + 3600.0 * h01(B.seed, 7, 1),
                            B.tD + B.deployInterval * (double)B.mem.size() + 900.0);
    auto setDeorbit = [&]()
    {
        const double ra = B.rPark, rp = kR + (vh.upperDeorbit ? 20000.0 : 45000.0); // the ship enters shallower
        B.aE = 0.5 * (ra + rp);
        B.eE = (ra - rp) / (ra + rp);
        B.nE = launch::meanMotion(B.aE);
        const double dv = std::sqrt(launch::kGM / ra) - std::sqrt(launch::kGM * (2.0 / ra - 1.0 / B.aE));
        // One engine, throttled (~12% of the stage's full thrust; never more): a burn of 12 s or more.
        const double acc = 0.12 * vh.flow2 * vh.vex2 / std::max(vh.upperMassKg, 1000.0);
        B.burnDur = std::clamp(dv / std::max(acc, 0.1), 12.0, 240.0);
        // (The ellipse is the impulsive equivalent of the finite burn: its apoapsis at the burn's middle, deorbitApoT.)
        B.thApo = B.thIns + B.nPark * (deorbitApoT(B) - tL - B.t2);
        const double rEI = kR + 100000.0;
        const double cE = std::clamp((1.0 - rEI / B.aE) / std::max(B.eE, 1e-6), -1.0, 1.0);
        const double E = glm::two_pi<double>() - std::acos(cE); // after the apoapsis, falling toward the perigee
        const double M = E - B.eE * std::sin(E);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + B.eE) * std::sin(0.5 * E), std::sqrt(1.0 - B.eE) * std::cos(0.5 * E));
        B.thEI = B.thApo + (nu < 0.0 ? nu + glm::two_pi<double>() : nu) - glm::pi<double>();
        B.vEI = std::sqrt(launch::kGM * (2.0 / rEI - 1.0 / B.aE));
        B.vrEI = std::sqrt(launch::kGM / (B.aE * (1.0 - B.eE * B.eE))) * B.eE * std::sin(nu); // < 0: falling
        B.tStageRE = deorbitApoT(B) + (M - glm::pi<double>()) / B.nE;
        B.tStageGone = B.tStageRE;
        B.entryOff = glm::dvec3(0.0);
        if (!vh.upperDeorbit)
            B.entryOff = deorbitPos(B, B.tStageGone) - shipEntry(B, 0.0, nullptr, nullptr);
    };
    setDeorbit();
    if (!vh.upperDeorbit)
        planShipLanding(sim, L, B, setDeorbit);
    B.vents = h01(B.seed, 8, 1) < vh.ventChance;
    // (After the deorbit plan, 2026-10-20 mission-ops suite: rocketPos returns nothing for the upper stage at or past
    // B.tStageGone, which was still 0 here, so the search never found the altitude and every fairing went at MECO + 30 s,
    // ~60-100 km instead of ~110.)
    // The payload fairing (2026-10-20): its halves go once the upper stage passes vh.fairingSepKm (~110 km: the
    // heating has fallen off), found on the upper stage's track and refined to 0.1 s.
    B.tFair = -1.0;
    B.fairBuilt = false;
    if (vh.fairingSepKm > 0.0)
    {
        const double hSep = vh.fairingSepKm * 1000.0;
        auto alt = [&](double tau) {
            glm::dvec3 P;
            return rocketPos(L, B, 1, tau, P) ? glm::length(P) - kR : -1.0;
        };
        double lo = vh.t1 + 5.0, hi = -1.0;
        if (alt(lo) >= hSep)
            hi = lo;
        else
            for (double tau = lo; tau <= vh.t2; tau += 2.0)
                if (alt(tau) >= hSep)
                {
                    hi = tau;
                    lo = tau - 2.0;
                    break;
                }
        if (hi < 0.0)
            hi = lo = std::min(vh.t1 + 30.0, vh.t2);
        while (hi - lo > 0.1)
        {
            const double m = 0.5 * (lo + hi);
            (alt(m) >= hSep ? hi : lo) = m;
        }
        B.tFair = B.tL + hi;
    }
    const double boosterEnd = tL + boosterTr(L, B).tEnd + boosterHoldS(L, B);
    B.tVisEnd = std::max({boosterEnd + kPlumeAgeLow, B.tStageGone + kPlumeAgeHigh, tL + B.t2 + kPlumeAgeHigh,
                          B.tStageGone + kShipEntryS, B.shipLands ? B.tShipEnd : 0.0});
    B.ends.clear();
    B.resid = false;
    for (const LMember &mb : B.mem)
    {
        B.ends.push_back(mb.tEnd);
        B.resid = B.resid || std::abs(mb.eps) > 1e-9;
    }
    std::sort(B.ends.begin(), B.ends.end());
    // Each member leaves the dispenser in turn (deployIntervalS apart): at its release it is AT the dispenser — in the
    // payload bay's door, near the nose on the zenith side (the stage flies nose prograde, belly to nadir) — and drifts
    // away from it at a fraction of a m/s, mostly upward; memberState eases that offset out over two days. (Until
    // 2026-10-19 the whole train appeared at once at deployment, a string 120 m apart behind the stage.)
    {
        const bool big = vh.upperMassKg > 50000.0; // Starship's ship: a 9-m body, the bay door high on the side
        // (5.3 m up: a stowed satellite's underside clears the 4.5-m hull)
        const double noseOff = big ? 31.0 : 9.0, sideOff = big ? 5.3 : 1.6;
        for (size_t mi = 0; mi < B.mem.size(); ++mi)
        {
            LMember &mb = B.mem[mi];
            mb.tRel = B.tD + B.deployInterval * (double)mi;
            // Its slot in the stack (2026-10-20): it leaves from there, in the stack's attitude.
            mb.stacked = stackSlot(vh, stk, mi, B.mem.size(), mb.slotO, mb.slotR, mb.exitDir);
            glm::dvec3 Pst, Pb;
            if (!rocketPos(L, B, 1, mb.tRel - B.tL, Pst) || !rocketPos(L, B, 1, mb.tRel - B.tL - 1.0, Pb))
                continue;
            mb.depOff = glm::dvec3(0.0);
            mb.depVel = glm::dvec3(0.0);
            mb.ff = false; // (the offsets below are measured from the member's own path)
            // The dispenser less the member's own path, in the member's (radial, along, cross) axes, at tt.
            auto offsetAt = [&](double tt, glm::dvec3 &o) {
                glm::dvec3 P1, P0, r, v;
                if (!rocketPos(L, B, 1, tt - B.tL, P1) || !rocketPos(L, B, 1, tt - B.tL - 1.0, P0) ||
                    !memberState(sim, B, mb, 0, tt, r, v))
                    return false;
                const glm::dvec3 nose = glm::normalize(P1 - P0), up = glm::normalize(P1);
                const glm::dvec3 disp = mb.stacked ? P1 + upperFrame(L, B, tt - B.tL) * mb.slotO
                                                   : P1 + noseOff * nose + sideOff * up;
                const glm::dvec3 U = r / glm::length(r);
                const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
                const glm::dvec3 A = glm::cross(Cn, U);
                const glm::dvec3 d = disp - r;
                o = glm::dvec3(glm::dot(d, U), glm::dot(d, A), glm::dot(d, Cn));
                return true;
            };
            glm::dvec3 o0, oA, oB;
            // (one-sided, second order: the first member's own path starts at its release)
            if (!offsetAt(mb.tRel, o0) || !offsetAt(mb.tRel + 0.5, oA) || !offsetAt(mb.tRel + 1.0, oB))
                continue;
            mb.depOff = o0;
            // The member leaves with the dispenser's velocity (+ its push): relative to its own plane and phase that is
            // tens to a hundred m/s across (the stage's plane differs from the slot's by up to a degree of node).
            mb.depVel = -3.0 * o0 + 4.0 * oA - oB; // (/ 2h, h = 0.5 s)
            const uint64_t ms = B.seed ^ ((uint64_t)mi * 0xD1B54A32D192ED03ull);
            // Mostly up, a little back, + the dispenser's spread (Starship's: a few cm/s, so the members leave as
            // one string along the same free-flight arc, sepSpeed x interval apart; a stack's: ~0.1 m/s).
            const double sp = stk.sepMps >= 0.0 ? stk.sepMps : vh.sepSpeedMps;
            const double sd = stk.spreadMps >= 0.0 ? stk.spreadMps : vh.sepSpreadMps;
            mb.sepDir = sp * glm::dvec3(0.97, -0.22, 0.0) +
                        sd * glm::dvec3(2.0 * h01(ms, 30, 1) - 1.0, 2.0 * h01(ms, 31, 1) - 1.0, 2.0 * h01(ms, 32, 1) - 1.0);
            if (mb.stacked)
            {
                // Out of its slot (2026-10-20): along the stack's exit direction (Starship: out of the door, up; a
                // flat stack: forward along the stage; a column: radially out), in the member's own axes at tRel.
                glm::dvec3 r, v;
                const glm::dmat3 S = upperFrame(L, B, mb.tRel - B.tL);
                if (memberState(sim, B, mb, 0, mb.tRel, r, v))
                {
                    const glm::dvec3 U = glm::normalize(r), Cn = glm::normalize(glm::cross(r, v)), A = glm::cross(Cn, U);
                    const glm::dvec3 e = S * mb.exitDir;
                    mb.sepDir = sp * glm::dvec3(glm::dot(e, U), glm::dot(e, A), glm::dot(e, Cn)) +
                                sd * glm::dvec3(2.0 * h01(ms, 30, 1) - 1.0, 2.0 * h01(ms, 31, 1) - 1.0,
                                                2.0 * h01(ms, 32, 1) - 1.0);
                }
                // It leaves in the stack's attitude with a little tip-off rate (0.2-0.8 deg/s about a hashed axis),
                // and its own control turns it to its attitude law 1-2 min later (over 3 min), before it unfolds.
                mb.q0 = glm::normalize(glm::quat_cast(S * mb.slotR));
                const double a1 = glm::two_pi<double>() * h01(ms, 33, 1), c1 = 2.0 * h01(ms, 34, 1) - 1.0;
                const glm::dvec3 ax(std::sqrt(1.0 - c1 * c1) * std::cos(a1), std::sqrt(1.0 - c1 * c1) * std::sin(a1), c1);
                mb.omega = ax * glm::radians(0.2 + 0.6 * h01(ms, 35, 1)) * (stk.layout == launch::Stack::Pez ? 0.5 : 1.0);
                mb.slewT0 = mb.tRel + 60.0 + 60.0 * h01(ms, 36, 1);
                mb.slewDur = 180.0;
            }
            // Its free flight about the parking circle (2026-10-21, replacing the drift about each member's own path:
            // their planes differ by up to a degree, and the members' linearised drifts disagreed by metres: pairs
            // closed to 0.8-1.6 m in the first minutes and again about an orbit later; a random spread of +-0.04-0.12
            // m/s per axis did the rest). The dispenser in the circle's curvilinear frame at tRel:
            {
                const double th0 = B.thIns + B.nPark * (mb.tRel - B.tL - B.t2);
                const glm::dvec3 Ur = std::cos(th0) * B.U0 + std::sin(th0) * B.A0;
                const glm::dvec3 Ar = -std::sin(th0) * B.U0 + std::cos(th0) * B.A0;
                const glm::dvec3 nose = glm::normalize(Pst - Pb), up = glm::normalize(Pst);
                const glm::dvec3 disp = mb.stacked ? Pst + upperFrame(L, B, mb.tRel - B.tL) * mb.slotO
                                                   : Pst + noseOff * nose + sideOff * up;
                const double z = glm::dot(disp, B.N);
                const glm::dvec3 rin = disp - z * B.N;
                mb.ffP = glm::dvec3(glm::length(rin) - B.rPark, B.rPark * std::atan2(glm::dot(rin, Ar), glm::dot(rin, Ur)), z);
                // Its velocity relative to the circle: the train's common in-plane push, across the plane its own
                // (a column's satellites leave sideways; + a small spread, golden-ratio stepped so neighbours differ),
                // and an along-track trim of -2 n x0 that cancels the secular drift its radial place would give it.
                const double sp = stk.sepMps >= 0.0 ? stk.sepMps : vh.sepSpeedMps;
                const double sd = stk.spreadMps >= 0.0 ? stk.spreadMps : vh.sepSpreadMps;
                const glm::dvec2 vin = releaseInPlane(mb.stacked ? stk.layout : (int)launch::Stack::None, sp);
                double vc = sd * (2.0 * std::fmod(0.6180339887498949 * (double)mi + 0.5 * h01(B.seed, 37, 1), 1.0) - 1.0);
                if (mb.stacked && stk.layout == launch::Stack::Column)
                    vc += sp * mb.exitDir.x; // (the stage's X is the circle's normal at deployment)
                mb.ffV = glm::dvec3(vin.x, vin.y - 2.0 * B.nPark * mb.ffP.x, vc);
                mb.ff = true;
            }
        }
    }
    // A crew flight's spacecraft (2026-10-20): its own flight from separation to splashdown; docking sets tA.
    if (pg.crew && !buildCrew(sim, L, B, pl, lifeS))
    {
        B.why = "no free docking port";
        return;
    }
    B.valid = true;
}

// Diagnostics only (harness `launch jitter`): build records and drift the old way, for a before / after comparison.
static inline bool gLegacyRecords = false;
// launchOrbitElems: the CPU's elements exact at the current sim time (default; harness `launch exact off` = the
// written record, for an A/B).
static inline bool gExactElems = true;
// Diagnostics only (harness `launch formation ... before=1`): each formation member on its own raise schedule, as
// before 2026-10-20.
static inline bool gPerMemberRaise = false;

// A member's state: the train climbing (kind 0, tD <= t < tA) or the retiree falling (kind 1, tA <= t < tEnd).
static bool memberState(const SatelliteSim &sim, const LBatch &B, const LMember &mb, int kind, double t, glm::dvec3 &r,
                        glm::dvec3 &v)
{
    SatOrbitElems e = sim.rosterElemsOf(sim.satOrbits[mb.sat]);
    e.meanMot = mb.nSlot; // the slot's rate: the phase offsets below are relative to it
    double a = mb.rSlot, du = 0.0;
    if (kind == 0)
    {
        if (t < B.tD || t >= B.tA)
            return false;
        const double tA = B.tA;
        // The residual spread over the raise as a smoothstep (2026-10-20, mission-ops suite): linear, it was a constant
        // drift that stopped dead at the arrival (Starlink DTC: an 18 m/s along-track jump onto the slot).
        const double sE = glm::clamp((t - B.tD) / std::max(tA - B.tD, 1.0), 0.0, 1.0);
        const double epsK = mb.eps * (1.0 - sE * sE * (3.0 - 2.0 * sE));
        const double sEnd = mb.sRaise + mb.tRamp; // == tA
        double lag;
        const double kj = mb.nSlot / launch::meanMotion(mb.rSlot);
        const double full = launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, mb.tRamp, 1.0, kj);
        if (t < mb.sRaise)
        {
            a = B.rPark;
            lag = (B.nPark - mb.nSlot) * (mb.sRaise - t) + full - mb.nSlot * mb.tRamp;
        }
        else
        {
            const double tau = std::min(t - mb.sRaise, mb.tRamp);
            a = B.rPark + (mb.rSlot - B.rPark) * tau / mb.tRamp;
            lag = (full - launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, tau, 1.0, kj)) - mb.nSlot * (sEnd - mb.sRaise - tau);
        }
        du = -(lag + epsK);
    }
    else
    {
        if (t < B.tA || t >= mb.tEnd)
            return false;
        const double Td = mb.tEnd - B.tA, tau = t - B.tA;
        const double rEnd = kR + kEndAltM;
        a = mb.rSlot + (rEnd - mb.rSlot) * tau / Td;
        const double kj = mb.nSlot / launch::meanMotion(mb.rSlot);
        du = launch::rampPhase(mb.rSlot, rEnd, Td, tau, kj, kj) - mb.nSlot * tau;
    }
    e.u0 += du;
    e.rSatM = a;
    const SatOrbitState st = satOrbitStateAt(e, t);
    r = st.posEci;
    v = st.velocity * std::sqrt(launch::kGM / glm::length(r));
    // Leaving the stage: the member starts where the stage is and drifts into its own plane and phase over two days
    // (members of one "plane" differ by up to a degree of node, a Starmind cluster by its jitter).
    // It leaves the dispenser at tRel with the dispenser's velocity + its push sepDir (m/s) and moves away as a FREE
    // body does (Clohessy-Wiltshire about the parking circle: a radial push makes it rise and fall back behind, an
    // along-track one drift), and the offset eases out over two days (the satellite's own thrusters). (Until 2026-10-19 the drift was a straight line in the
    // rotating frame, which no orbit follows: the GPU's Kepler record of it was off by metres after 2 s and by up
    // to hundreds of metres after the 20-s rewrites — the jitter at 10x.)
    // Free flight (2026-10-21): about the train's ONE reference orbit, the stage's parking circle, from the member's
    // place at release with its separation velocity (LMember::ffP / ffV), so the members' relative motion is exactly
    // consistent; before its release it rides in its slot. kFreeFlightS after the release its own propulsion takes
    // over: the offset from its own path eases out by two days, as before.
    if (kind == 0 && mb.ff && !gLegacyRecords)
    {
        const double age = t - mb.tRel;
        double f = 1.0;
        if (age > kFreeFlightS)
        {
            const double s = glm::clamp((age - kFreeFlightS) / (2.0 * 86400.0 - kFreeFlightS), 0.0, 1.0);
            f = 1.0 - s * s * (3.0 - 2.0 * s);
        }
        if (f <= 0.0)
            return true;
        glm::dvec3 X = mb.ffP, Xv(0.0);
        if (age > 0.0)
            launch::cwState(mb.ffP, mb.ffV, B.nPark, age, X, Xv);
        const double th = B.thIns + B.nPark * (t - B.tL - B.t2) + X.y / B.rPark;
        const glm::dvec3 Ur = std::cos(th) * B.U0 + std::sin(th) * B.A0;
        const glm::dvec3 Ar = -std::sin(th) * B.U0 + std::cos(th) * B.A0;
        const glm::dvec3 rf = (B.rPark + X.x) * Ur + X.z * B.N;
        const glm::dvec3 vf = Xv.x * Ur + (B.rPark + X.x) * (B.nPark + Xv.y / B.rPark) * Ar + Xv.z * B.N;
        if (f >= 1.0)
        {
            r = rf;
            v = vf;
            return true;
        }
        // Easing into its own path: the free position's offset from it (radius, angle along it, across) scaled by f.
        const glm::dvec3 U = r / glm::length(r);
        const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
        const glm::dvec3 A = glm::cross(Cn, U);
        const double z = glm::dot(rf, Cn);
        const glm::dvec3 rin = rf - z * Cn;
        const double rl = glm::length(r), dx = glm::length(rin) - rl;
        const double ang = f * std::atan2(glm::dot(rin, A), glm::dot(rin, U));
        const double ca = std::cos(ang), sa = std::sin(ang);
        const glm::dvec3 U2 = ca * U + sa * A, A2 = ca * A - sa * U;
        const double vu = glm::dot(v, U), va = glm::dot(v, A), vc = glm::dot(v, Cn);
        r = (rl + f * dx) * U2 + f * z * Cn;
        v = (1.0 - f) * (vu * U2 + va * A2 + vc * Cn) + f * vf;
        return true;
    }
    if (kind == 0 && glm::dot(mb.depOff, mb.depOff) > 0.0)
    {
        const double age = std::max(t - mb.tRel, 0.0);
        const double s = glm::clamp(age / (2.0 * 86400.0), 0.0, 1.0);
        const double f = 1.0 - s * s * (3.0 - 2.0 * s);
        if (f > 0.0)
        {
            const glm::dvec3 U = r / glm::length(r);
            const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
            const glm::dvec3 A = glm::cross(Cn, U);
            // The whole offset from its own path flies free from the release: the dispenser's position and velocity
            // relative to that path (+ its push), as Clohessy-Wiltshire motion about the parking circle. (Holding the
            // dispenser's offset fixed in the rotating frame — up to ~100 km across, the planes' difference — took a
            // constant push no orbit has: 0.07 m/s^2 across, 14 m off a Kepler record after 20 s.)
            glm::dvec3 o, cwV;
            launch::cwState(mb.depOff, mb.depVel + mb.sepDir, B.nPark, age, o, cwV);
            if (gLegacyRecords) // (harness `launch jitter`: the pre-2026-10-19 straight drift, for comparison)
            {
                o = mb.depOff + mb.sepDir * age;
                cwV = mb.sepDir;
            }
            // The along-track offset is ARC length (Clohessy-Wiltshire's y): turned along the orbit about Cn, not added
            // as a straight line (2026-10-20). A late release out of a long dispenser run (Starmind: 36 x 45 s) leaves
            // the stage ~60 km behind its path at ~-40 m/s, whose secular drift reaches thousands of km within a day;
            // as a chord that put the member 1800-2000 km UP, and its record was refused (never drawn; selftest
            // launches_schedule_and_phasing: 2 of 33000 slots).
            if (gLegacyRecords)
                r += f * (o.x * U + o.y * A + o.z * Cn);
            else
            {
                // (The release offset itself stays the straight line it was measured as, so the member starts exactly
                // at the dispenser; only the drift since is turned along the orbit.)
                const double ang = f * (o.y - mb.depOff.y) / glm::length(r);
                const double ca = std::cos(ang), sa = std::sin(ang);
                const glm::dvec3 U2 = ca * U + sa * A, A2 = ca * A - sa * U;
                const double rl = glm::length(r);
                r = (rl + f * o.x) * U2 + f * mb.depOff.y * A2 + f * o.z * Cn;
                const double vu = glm::dot(v, U), va = glm::dot(v, A), vc = glm::dot(v, Cn);
                v = vu * U2 + va * A2 + vc * Cn;
                if (t >= mb.tRel)
                    v += f * (cwV.x * U2 + cwV.y * A2 + cwV.z * Cn);
            }
            if (gLegacyRecords && t >= mb.tRel)
                v += f * (cwV.x * U + cwV.y * A + cwV.z * Cn);
        }
    }
    return true;
}

// A member's unfolding (2026-10-19): a train member leaves its dispenser stowed and its model's stowable parts
// (arrays, radiators: SatComponent::stow) unfold over SatModel::unfoldDurS, starting unfoldDelayS (+-15% per member)
// after its release; a retiree is deployed. t0 / dur as SatOrbitElems::unfoldT0 / unfoldDur (dur 0 = deployed).
static void memberUnfold(const SatelliteSim &sim, const LMember &mb, int kind, double &t0, double &dur)
{
    t0 = 0.0;
    dur = 0.0;
    if (kind != 0)
        return;
    const SatOrbit &so = sim.satOrbits[mb.sat];
    if (so.typeIdx >= sim.satTypes.size() || !sim.satTypes[so.typeIdx].model)
        return;
    const SatModel &m = *sim.satTypes[so.typeIdx].model;
    if (m.unfoldDurS <= 0.0)
        return;
    const double j = h01((uint64_t)mb.sat * 0x9E3779B97F4A7C15ull, 41, 1);
    t0 = mb.tRel + m.unfoldDelayS * (0.85 + 0.3 * j);
    dur = m.unfoldDurS;
}

// The Kepler record a member's GPU slot carries from t (and the CPU's copy of it, ce): its OSCULATING orbit at t.
// The GPU propagates it as a Kepler orbit until the next rewrite, so it must be the orbit the member is really on
// (2026-10-19, the 10x jitter): the velocity is memberState's own derivative (its drift and every eased offset
// included; the analytic one was the bare circle's), an SSO plane's precession is the record's node rate (removed
// from the velocity first, so it is not counted twice), and the epoch phase uses the mean motion as the GPU's float
// holds it (with the double one the float's rounding, x up to 7 days, put a satellite up to ~140 m off, a different
// amount at every rewrite).
static bool memberRecord(const SatelliteSim &sim, const LBatch &B, const LMember &mb, int kind, double t,
                         double tEpoch, GpuKepOrbit &g, SatOrbitElems &ce)
{
    glm::dvec3 r, v, rA, rB, vT;
    if (!memberState(sim, B, mb, kind, t, r, v))
        return false;
    if (gLegacyRecords)
    {
        // The pre-2026-10-19 record (harness `launch jitter` only): memberState's analytic velocity, no node rate,
        // the double mean motion.
        const kessler::Elems el = kessler::elemsFromState({r, v});
        if (!(el.a > kR) || el.e >= 0.9)
            return false;
        const double n = launch::meanMotion(el.a);
        g = GpuKepOrbit{};
        g.M = (float)wrap2pi(el.M - n * (t - tEpoch));
        g.argp = (float)wrap2pi(el.argp);
        g.raan = (float)wrap2pi(el.raan);
        g.n = (float)n;
        g.a = (float)el.a;
        g.e = (float)el.e;
        g.cosI = (float)std::cos(el.i);
        g.sinI = (float)std::sin(el.i);
        ce = SatOrbitElems{};
        ce.kepler = true;
        ce.u0 = wrap2pi(el.M - n * t);
        ce.raan = el.raan;
        ce.argp = el.argp;
        ce.meanMot = n;
        ce.rSatM = el.a;
        ce.ecc = el.e;
        ce.incl = el.i;
        return true;
    }
    const double h = 0.25;
    const bool okA = memberState(sim, B, mb, kind, t + h, rA, vT), okB = memberState(sim, B, mb, kind, t - h, rB, vT);
    if (okA && okB)
        v = (rA - rB) / (2.0 * h);
    else if (okA)
        v = (rA - r) / h;
    else if (okB)
        v = (r - rB) / h;
    const SatOrbit &so = sim.satOrbits[mb.sat];
    const SatOrbitElems re = sim.rosterElemsOf(so);
    const double raanDot = re.sso ? satphot::kSSOPrecRate : 0.0;
    const glm::dvec3 vRel = v - glm::cross(glm::dvec3(0.0, 0.0, raanDot), r);
    const kessler::Elems el = kessler::elemsFromState({r, vRel});
    if (!(el.a > kR) || el.e >= 0.9)
        return false;
    const float nf = (float)launch::meanMotion(el.a);
    const double n = (double)nf;
    g = GpuKepOrbit{};
    g.M = (float)wrap2pi(el.M - n * (t - tEpoch));
    g.argp = (float)wrap2pi(el.argp);
    g.raan = (float)wrap2pi(el.raan - raanDot * (t - tEpoch));
    g.n = nf;
    g.a = (float)el.a;
    g.e = (float)el.e;
    g.cosI = (float)std::cos(el.i);
    g.sinI = (float)std::sin(el.i);
    g.raanDot = (float)raanDot;
    g.flux = 1.0f;
    g.bornSec = (int32_t)0x80000001;
    g.deathSec = kessler::kNever;
    // A train raising flies "open book" (bit 25: every joint at rest, arrays flat with the bus, edge into the
    // flow): the low-drag configuration that makes fresh trains bright. On station it is the roster satellite.
    // Bit 26: still unfolding — the tumble words hold the progress (rate, value at the orbit epoch).
    double uT0 = 0.0, uDur = 0.0;
    memberUnfold(sim, mb, kind, uT0, uDur);
    const bool unfolding = uDur > 0.0 && t < uT0 + uDur;
    g.typeConst = (uint32_t)so.typeIdx | ((so.constIdx & 0xFFu) << 16) | (kind == 0 ? (1u << 25) : 0u) |
                  (unfolding ? (1u << 26) : 0u);
    if (unfolding)
    {
        g.tumbleRate = (float)(1.0 / uDur);
        g.tumblePhase = (float)((tEpoch - uT0) / uDur);
    }
    ce = SatOrbitElems{};
    ce.kepler = true;
    ce.u0 = el.M - n * t;
    ce.raan = el.raan - raanDot * t;
    ce.raanDot = raanDot;
    ce.argp = el.argp;
    ce.meanMot = n;
    ce.rSatM = el.a;
    ce.ecc = el.e;
    ce.incl = el.i;
    ce.restJoints = kind == 0;
    if (unfolding)
    {
        ce.unfoldT0 = uT0;
        ce.unfoldDur = uDur;
    }
    if (kind == 0 && mb.stacked && t < mb.slewT0 + mb.slewDur)
    {
        // Just released from its stack (2026-10-20): the stack's attitude, tumbling slowly, until its control acquires
        // its law (CPU only: the GPU's point keeps the law).
        ce.attFix = true;
        ce.attQ = mb.q0;
        ce.attOmega = mb.omega;
        ce.attT0 = mb.tRel;
        ce.attSlewT0 = mb.slewT0;
        ce.attSlewDur = mb.slewDur;
    }
    return true;
}

// Whether a member is drawn: Kessler may have killed its slot before the swap (no replacement, no retiree).
static bool memberAllowed(const SatelliteSim &sim, const LBatch &B, const LMember &mb)
{
    if (!sim.kesslerEnabled || 2ull * mb.sat >= sim.kRosterWords.size())
        return true;
    const int32_t dead = sim.kRosterWords[2ull * mb.sat];
    return dead == kessler::kNever || (double)dead > secOfKessler(sim, B.tA);
}

// ── The rocket ──────────────────────────────────────────────────────────────────────────────────
// Earth-fixed position on the ground track: x downrange along the great circle (G0, Eaz), h up.
static glm::dvec3 trackEcef(const LBatch &B, double x, double h)
{
    const double a = x / kR;
    // Near the pad the track stands on its plateau and mount (a booster coming home lands on them); downrange the
    // offset is gone (a droneship's deck is at the sea's level).
    const double base = B.padBase * (1.0 - glm::smoothstep(5000.0, 40000.0, std::abs(x)));
    return (kR + base + h) * (std::cos(a) * B.G0 + std::sin(a) * B.Eaz);
}

// The booster's recovery for this launch (2026-10-20, droneships; a pure function of the batch): a reusable booster
// that can come back (Vehicle::rtlsMaxKg) does so with a light payload (the program's mass x a hashed 0.9-1.1),
// else it lands downrange. A downrange touchdown at sea is on a landing ship (one of the vehicle's, picked by hash
// among those serving the site) waiting on station from kShipOnStationS before lift-off; on land it is the ground.
static void planRecovery(const SatelliteSim::LaunchState &L, LBatch &B, const launch::Program &pg)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    B.rec = vh.recovery;
    B.payloadKg = pg.payloadMassKg >= 0.0 ? pg.payloadMassKg * (0.9 + 0.2 * h01(B.seed, 31, 1)) : -1.0;
    if (vh.recovery == 2 && vh.rtlsMaxKg > 0.0 && B.payloadKg >= 0.0 && B.payloadKg <= vh.rtlsMaxKg &&
        (size_t)B.veh < L.boosterRtls.size() && !L.boosterRtls[B.veh].p.empty())
        B.rec = 1;
    B.landKind = kLandNone;
    B.ship = -1;
    B.lzOff = glm::dvec3(0.0);
    B.lzUp = 0.0;
    B.landBase = 0.0;
    const Track &tr = boosterTr(L, B);
    if (B.rec == 0 || !tr.landed)
        return;
    const int pi = (B.site >= 0 && B.site < (int)L.sitePad.size()) ? L.sitePad[B.site] : -1;
    if (B.rec == 1)
    {
        B.landKind = kLandTower;
        // A droneship vehicle back home (Falcon RTLS): its pad's landing zone (the zone's ground, 2.3 km aside).
        if (vh.recovery == 2)
        {
            B.landKind = kLandLz;
            if (pi >= 0 && L.pads[pi].lz)
            {
                const LPad &pd = L.pads[pi];
                B.lzOff = kLzDistM * (std::cos(pd.lzAng) * pd.u + std::sin(pd.lzAng) * pd.v);
                B.lzUp = pd.lzElevM - pd.elevM;
            }
        }
        return;
    }
    B.xLand = tr.p.back().x;
    B.tLand = B.tL + tr.tEnd;
    const double a = B.xLand / kR;
    const glm::dvec3 g = std::cos(a) * B.G0 + std::sin(a) * B.Eaz;
    const double la = glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(g.y, g.x));
    // At sea (the water map, a few km off any shore): a ship. Else the ground (its height under the touchdown).
    std::vector<int> ok;
    for (size_t k = 0; k < vh.ships.size(); ++k)
        if (vh.ships[k].sites.empty() ||
            std::find(vh.ships[k].sites.begin(), vh.ships[k].sites.end(), B.site) != vh.ships[k].sites.end())
            ok.push_back((int)k);
    if (!ok.empty() && shoreDistCpu(la, lo) > 3000.0)
    {
        B.landKind = kLandShip;
        B.ship = ok[std::min((size_t)(h01(B.seed, 32, 1) * ok.size()), ok.size() - 1)];
        B.deckM = vh.ships[B.ship].deckM;
        B.shipG = g;
        // Home: toward the launch site (the ship's bow points there the whole time).
        glm::dvec3 h = B.G0 - glm::dot(B.G0, g) * g;
        B.shipHome = glm::length(h) > 1e-9 ? glm::normalize(h) : -B.Eaz;
        B.tShipOn = B.tL - kShipOnStationS;
        B.tShipDepart = B.tLand + kShipSecureS;
        B.tShipGone = B.tLand + kShipHomeS;
        return;
    }
    B.landKind = kLandGround;
    // The track touches down at the deck's height (the vehicle's first ship, else 3 m): the ground's height less it.
    // (the track ends vh.legM above it: the legs' feet on the ground)
    const double hTrack = tr.p.back().y;
    B.landBase = std::max(demElevCpu(la, lo), 0.0) + vh.legM - hTrack;
}

// Where this launch's booster comes to rest (for the landing effects: dust on a pad or the ground, spray off a ship's
// deck; harness `launch state` -> boosters[].land_kind). Returns its kind (kLand*: kLandNone for an expendable one),
// the touchdown time tTouch and E = the point (Earth-fixed AT t: a ship's moves) where the booster comes to rest:
// a tower's catch point (its aft end), a landing zone's ground, a ship's deck at its landing target, the ground
// downrange (its legs' feet: the aft end stands Vehicle::legM above them).
static int boosterLandingAt(const SatelliteSim::LaunchState &L, const LBatch &B, double t, glm::dvec3 &E, double &tTouch)
{
    const Track &tr = boosterTr(L, B);
    tTouch = B.tL + tr.tEnd;
    E = glm::dvec3(0.0);
    if (B.landKind == kLandNone || !tr.landed)
        return kLandNone;
    if (B.landKind == kLandShip)
    {
        E = shipDeckEcef(B, t, 0.0, 0.0, B.deckM);
        return kLandShip;
    }
    const glm::dvec2 p = tr.p.back();
    E = trackEcef(B, p.x, p.y);
    if (B.landKind == kLandLz)
        E += B.lzOff + B.lzUp * glm::normalize(E);
    else if (B.landKind == kLandGround)
        E += B.landBase * glm::normalize(E);
    if (B.landKind == kLandLz || B.landKind == kLandGround)
        E -= L.cat.vehicles[B.veh].legM * glm::normalize(E);
    return B.landKind;
}

// The booster's track for this launch (its recovery: back to the pad, or the vehicle's own).
static const Track &boosterTr(const SatelliteSim::LaunchState &L, const LBatch &B)
{
    if (B.rec == 1 && (size_t)B.veh < L.boosterRtls.size() && !L.boosterRtls[B.veh].p.empty())
        return L.boosterRtls[B.veh];
    return L.booster[B.veh];
}
// How long a landed booster stays drawn after touchdown: in the tower or on its landing zone half an hour, on a ship
// until the ship is gone (it rides home on the deck).
static double boosterHoldS(const SatelliteSim::LaunchState &L, const LBatch &B)
{
    if (!boosterTr(L, B).landed)
        return 0.0;
    if (B.landKind == kLandShip)
        return std::max(B.tShipGone - B.tLand, 0.0);
    return B.landKind == kLandNone ? 0.0 : kCaughtHoldS;
}
// The ship's place at t (Earth-fixed, on the sea-level sphere) and its bow (unit, horizontal): on station until
// tShipDepart, then steaming home along the great circle toward the launch site (eased in over 10 min).
static glm::dvec3 shipEcef(const LBatch &B, double t, glm::dvec3 *bow = nullptr)
{
    const double T = 600.0, dt = t - B.tShipDepart;
    const double d = dt <= 0.0 ? 0.0 : dt < T ? kShipSpeed * dt * dt / (2.0 * T) : kShipSpeed * (dt - 0.5 * T);
    const double a = d / kR;
    if (bow)
        *bow = -std::sin(a) * B.shipG + std::cos(a) * B.shipHome;
    return kR * (std::cos(a) * B.shipG + std::sin(a) * B.shipHome);
}

// The ship riding the swell (2026-10-21): heave (m), pitch (bow up) and roll (to starboard) in radians, each a sum of
// three sinusoids with periods of ~6-14 s (hashed per ship) x the sea state there (seaK). A pure function of sim time
// and the batch. Jacklyn, a ship four times the barge's displacement, moves less.
static void shipSwell(const LBatch &B, double t, double &heave, double &pitch, double &roll)
{
    const double s = (B.seaK > 0.0f ? (double)B.seaK : 1.0) * (B.deckM > 5.0 ? 0.6 : 1.0);
    heave = pitch = roll = 0.0;
    static const double kPer[3] = {7.0, 9.5, 12.5};
    for (int k = 0; k < 3; ++k)
    {
        const double w = glm::two_pi<double>() / (kPer[k] * (0.9 + 0.2 * h01(B.seed, 340 + k, 1)));
        const double a = std::fmod(w * t, glm::two_pi<double>()) + glm::two_pi<double>() * h01(B.seed, 340 + k, 2);
        heave += 0.3 * s * std::sin(a);
        pitch += glm::radians(0.35) * s * std::sin(a + 1.3 + 0.9 * k);
        roll += glm::radians(0.7) * s * std::sin(a + 2.1 + 2.3 * k);
    }
}
// The ship's frame at t (Earth-fixed): its origin (the landing target at the waterline) and axes, riding the swell.
struct ShipFrame
{
    glm::dvec3 O, bow, port, up;
};
static ShipFrame shipFrame(const LBatch &B, double t)
{
    glm::dvec3 bow0;
    const glm::dvec3 S = shipEcef(B, t, &bow0);
    const glm::dvec3 up0 = S / kR;
    bow0 = glm::normalize(bow0 - glm::dot(bow0, up0) * up0);
    const glm::dvec3 port0 = glm::cross(up0, bow0);
    double hv, p, r;
    shipSwell(B, t, hv, p, r);
    ShipFrame F;
    F.bow = std::cos(p) * bow0 + std::sin(p) * up0;
    const glm::dvec3 up1 = -std::sin(p) * bow0 + std::cos(p) * up0;
    F.port = std::cos(r) * port0 + std::sin(r) * up1;
    F.up = -std::sin(r) * port0 + std::cos(r) * up1;
    F.O = S + hv * up0;
    return F;
}
// A point on the ship (its body frame: u toward the bow, v to port, h above the waterline), Earth-fixed.
static glm::dvec3 shipDeckEcef(const LBatch &B, double t, double u, double v, double h)
{
    const ShipFrame F = shipFrame(B, t);
    return F.O + h * F.up + u * F.bow + v * F.port;
}

// The landing frame's gnomonic plane (x east, y north at the target, metres) and back.
static glm::dvec3 landPoint(const LBatch &B, const glm::dvec2 &x, double h)
{
    return (kR + h) * glm::normalize(B.LG + (x.x / kR) * B.LE + (x.y / kR) * B.LN);
}
static void landCoords(const LBatch &B, const glm::dvec3 &P, glm::dvec2 &x, double &h)
{
    const double r = glm::length(P);
    const glm::dvec3 d = P / r;
    const double c = std::max(glm::dot(d, B.LG), 1e-3);
    x = kR * glm::dvec2(glm::dot(d, B.LE), glm::dot(d, B.LN)) / c;
    h = r - kR;
}
static double herm(double s, double p0, double m0, double p1, double m1)
{
    const double s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
}
// A returning ship after its entry (Earth-fixed): the belly-flop descent to F, the flip and landing burn to the catch
// point, then held in the tower (or, with no pad in reach, at rest on the sea).
static glm::dvec3 shipDescentEcef(const LBatch &B, double t, uint8_t *burn)
{
    glm::dvec2 x;
    double h;
    uint8_t b = launch::BURN_NONE;
    if (t < B.tShipFlip)
    {
        const double s = glm::clamp((t - B.tShipDesc) / B.T1, 0.0, 1.0);
        x = glm::dvec2(herm(s, B.dX0.x, B.dV0.x * B.T1, B.dXF.x, B.dVF.x * B.T1),
                       herm(s, B.dX0.y, B.dV0.y * B.T1, B.dXF.y, B.dVF.y * B.T1));
        h = herm(s, B.dH0, B.dVz0 * B.T1, B.dHF, B.dVzF * B.T1);
    }
    else if (t < B.tShipCatch)
    {
        const double s = glm::clamp((t - B.tShipFlip) / B.T2, 0.0, 1.0);
        x = glm::dvec2(herm(s, B.dXF.x, B.dVF.x * B.T2, 0.0, 0.0), herm(s, B.dXF.y, B.dVF.y * B.T2, 0.0, 0.0));
        h = herm(s, B.dHF, B.dVzF * B.T2, B.catchH, 0.0);
        b = launch::BURN_LANDING;
    }
    else
    {
        x = glm::dvec2(0.0);
        h = B.catchH;
    }
    if (burn)
        *burn = b;
    return landPoint(B, x, h);
}

// The booster (stage 0) or the upper stage (1) at tau s after lift-off, ECI. False when it is not flying.
static bool rocketPos(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau, glm::dvec3 &P,
                      uint8_t *burn = nullptr)
{
    const double t = B.tL + tau;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    if (tau < -600.0)
        return false;
    tau = std::max(tau, 0.0);
    if (stage == 0)
    {
        const Track &tr = boosterTr(L, B);
        // A landed booster stays: in the tower's chopsticks or on its landing zone (half an hour), on the ship's deck
        // while the ship is drawn (2026-10-20).
        if (tau > tr.tEnd + boosterHoldS(L, B))
            return false;
        uint8_t b = 0;
        const glm::dvec2 p = tr.at(tau, nullptr, &b);
        if (burn)
            *burn = (tau <= 0.0) ? launch::BURN_NONE : b;
        // Before staging the two are one vehicle: the booster carries the flame.
        glm::dvec3 E = trackEcef(B, p.x, p.y);
        if (tau > vh.t1)
        {
            // Coming home to a landing zone beside the pad (Falcon RTLS): the track's touchdown is the pad's centre;
            // over its last 150 s it slides to the zone (2.3 km aside) and the zone's ground.
            // (2026-10-21: both blended in over the last 150 s of the descent by TIME, the last 15 s purely vertical; by
            // downrange distance they were crossed in ~20 s at ~1.5 km/s, a 1.5-km height change that peaked the proper
            // acceleration at 51-53 m/s^2 for Pallas-1 / Zhuque-3 to the ground and a Falcon 9 to its zone)
            const double wT = glm::smoothstep(tr.tEnd - 150.0, tr.tEnd - 15.0, tau);
            if (B.landKind == kLandLz)
                E += wT * (B.lzOff + B.lzUp * glm::normalize(E));
            // Down on the ground downrange: the ground's height there.
            else if (B.landKind == kLandGround)
                E += wT * B.landBase * glm::normalize(E);
            // On the ship's deck once down (standing on its legs), riding it home (and its swell: over the last seconds
            // of the landing burn the booster takes up the deck's motion, so it touches down on the moving deck).
            if (B.landKind == kLandShip && tau >= tr.tEnd)
                E = shipDeckEcef(B, t, 0.0, 0.0, B.deckM + vh.legM);
            else if (B.landKind == kLandShip && tau > tr.tEnd - 6.0)
                E += glm::smoothstep(tr.tEnd - 6.0, tr.tEnd, tau) *
                     (shipDeckEcef(B, t, 0.0, 0.0, B.deckM + vh.legM) - (kR + B.deckM + vh.legM) / kR * shipEcef(B, t));
        }
        P = ecefToEci(E, t);
        return true;
    }
    if (B.tL + tau >= B.tStageGone)
    {
        // A returning ship: drawn by its entry glow (shipEntry) until tShipDesc, then here to the catch.
        if (!B.shipLands || t < B.tShipDesc || t >= B.tShipEnd)
            return false;
        P = ecefToEci(shipDescentEcef(B, t, burn), t);
        return true;
    }
    if (burn)
        *burn = launch::BURN_NONE;
    const Track &up = const_cast<SatelliteSim::LaunchState &>(L).upperTrackFor(B.veh, L.cat.programs[B.prog].parkKm);
    if (tau <= B.t2)
    {
        const glm::dvec2 p = up.at(tau);
        glm::dvec3 Pg = ecefToEci(trackEcef(B, p.x, p.y), t);
        const double a = p.x / kR;
        const glm::dvec3 Pi = (kR + p.y) * (std::cos(a) * B.U0 + std::sin(a) * B.A0);
        // The blend into the inertial plane starts AT staging (2026-10-19): it started 60 s before, so at separation
        // the upper stage was already ~4 km from the stack it came out of, a jump Go to and the selection carried.
        const double s = glm::clamp((tau - vh.t1) / std::max(B.t2 - vh.t1, 1.0), 0.0, 1.0);
        const double sm = s * s * (3.0 - 2.0 * s);
        // The upper track is integrated on its own and stands ~1 km from the booster's at separation: it starts from the
        // stack's point (the booster track's at t1, Earth-fixed) and the difference fades out with the blend.
        const Track &bt = boosterTr(L, B);
        if (vh.t1 <= bt.tEnd)
        {
            const glm::dvec2 pb = bt.at(vh.t1), pu = up.at(vh.t1);
            const glm::dvec3 d = trackEcef(B, pb.x, pb.y) - trackEcef(B, pu.x, pu.y);
            Pg += ecefToEci(d, t) * (1.0 - sm);
        }
        P = glm::mix(Pg, Pi, sm);
        if (burn && tau > vh.t1 + 4.0)
            *burn = launch::BURN_UPPER;
        return true;
    }
    const double th = B.thIns + B.nPark * (tau - B.t2);
    P = B.rPark * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
    if (t > B.tDeorbBurn)
    {
        // The deorbit burn (2026-10-21): a constant thrust with 1-s start-up and shut-down ramps. Off the parking
        // circle by Q(tau) / Q(D) x the entry ellipse's offset from it at the burn's end, Q the thrust profile's
        // double integral: the displacement a finite burn makes, its acceleration the profile's (dv / D, ~flat), and at
        // the end exactly on the ellipse at its speed, the ellipse being the impulsive burn at the profile's centroid
        // (the middle: deorbitApoT). The smoothstep blend it replaced peaked at ~4x dv / D (Qianfan: ~90 m/s^2).
        const double D = std::max(B.burnDur, 1.0), tau2 = std::min(t - B.tDeorbBurn, D);
        if (t - B.tDeorbBurn >= D)
            P = deorbitPos(B, t);
        else
        {
            const double te = B.tDeorbBurn + D, the = B.thIns + B.nPark * (te - B.tL - B.t2);
            const glm::dvec3 Ce = B.rPark * (std::cos(the) * B.U0 + std::sin(the) * B.A0);
            P += deorbitBurnShape(tau2, D) * (deorbitPos(B, te) - Ce);
        }
    }
    if (burn && t >= B.tDeorbBurn && t < B.tDeorbBurn + B.burnDur)
        *burn = launch::BURN_UPPER;
    (void)vh;
    return true;
}

// The deorbit ellipse's apoapsis: the impulsive equivalent of the finite burn, at its middle (the thrust profile below
// is symmetric).
static double deorbitApoT(const LBatch &B)
{
    return B.tDeorbBurn + 0.5 * std::max(B.burnDur, 1.0);
}
// The deorbit burn's displacement profile at tau of D s, normalised to 1 at the end: Q(tau) / Q(D), Q = the double
// integral of the thrust (a trapezoid: 1-s ramps at start-up and shut-down, flat between).
static double deorbitBurnShape(double tau, double D)
{
    const double r = std::min(1.0, 0.25 * D);
    auto Q = [&](double x) {
        if (x <= r)
            return x * x * x / (6.0 * r);
        const double qr = r * r / 6.0, vr = 0.5 * r;
        if (x <= D - r)
            return qr + vr * (x - r) + 0.5 * (x - r) * (x - r);
        const double y = D - r, s = x - y;
        const double qy = qr + vr * (y - r) + 0.5 * (y - r) * (y - r), vy = vr + (y - r);
        return qy + vy * s + 0.5 * s * s - s * s * s / (6.0 * r);
    };
    return Q(glm::clamp(tau, 0.0, D)) / Q(D);
}
// The stage on its entry ellipse (apoapsis at the deorbit burn's middle, deorbitApoT), ECI.
static glm::dvec3 deorbitPos(const LBatch &B, double t)
{
    const double M = glm::pi<double>() + B.nE * (t - deorbitApoT(B));
    double E = M;
    for (int i = 0; i < 8; ++i)
        E -= (E - B.eE * std::sin(E) - M) / (1.0 - B.eE * std::cos(E));
    const double r = B.aE * (1.0 - B.eE * std::cos(E));
    double nu = 2.0 * std::atan2(std::sqrt(1.0 + B.eE) * std::sin(0.5 * E), std::sqrt(1.0 - B.eE) * std::cos(0.5 * E));
    // nu - pi, continuous across the apoapsis (E = pi): E's own unwrap.
    const double dnu = nu - glm::pi<double>() + glm::two_pi<double>() * std::round((E - nu) / glm::two_pi<double>());
    const double th = B.thApo + dnu;
    return r * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
}

static glm::dvec3 rocketEcef(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau, bool &ok)
{
    glm::dvec3 P;
    ok = rocketPos(L, B, stage, tau, P);
    return ok ? eciToEcef(P, B.tL + std::max(tau, 0.0)) : glm::dvec3(0.0);
}

// The booster's nose as a pitch angle in its trajectory's vertical plane (from downrange toward up; branch
// [-90, 270) degrees, so nothing it flies wraps). Ascent: along its velocity. After staging: engines first (along -v),
// and in its burns along its thrust (the track's acceleration less gravity): a boostback toward the pad, the landing
// burn upright. Every change between these (the flip after MECO, the flip back after an RTLS boostback) passes
// through the vertical, and averaging the ANGLE (stageNose) turns each into a slew; averaging vectors could not: the
// ones after an RTLS boostback point opposite ways, and their mean spun the booster ~340 degrees/s.
static double boosterPitchRaw(const SatelliteSim::LaunchState &L, const LBatch &B, double tau)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const Track &tr = boosterTr(L, B);
    auto ang = [](glm::dvec2 d) {
        double a = std::atan2(d.y, d.x);
        if (a < -0.5 * glm::pi<double>())
            a += glm::two_pi<double>();
        return a;
    };
    tau = glm::clamp(tau, 0.0, tr.tEnd);
    glm::dvec2 v;
    uint8_t b = 0;
    tr.at(tau, &v, &b);
    if (tau <= vh.t1 + 1.0 || glm::length(v) < 1.0)
        return glm::length(v) < 1.0 ? 0.5 * glm::pi<double>() : ang(v);
    if (b == launch::BURN_BOOSTBACK || b == launch::BURN_ENTRY || b == launch::BURN_LANDING)
    {
        glm::dvec2 va, vc;
        tr.at(tau - 0.5, &va);
        tr.at(tau + 0.5, &vc);
        const glm::dvec2 acc = (vc - va) + glm::dvec2(0.0, 9.81);
        if (glm::length(acc) > 1.0)
            return ang(acc);
    }
    return ang(-v);
}

// The way the upper stage's nose points (ECI, unit), before smoothing: along its velocity to orbit and in orbit,
// then it pitches over to retrograde in the two minutes before its deorbit burn (through the vertical, never a snap)
// and holds it; a returning ship then pitches up to its entry attitude, nose 60 degrees above the velocity, belly into
// the flow (what its entry glow starts with).
static glm::dvec3 upperNoseRaw(const SatelliteSim::LaunchState &L, const LBatch &B, double tau, bool &ok)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const double t = B.tL + tau;
    ok = false;
    if (tau < B.t2)
    {
        // To orbit: along the Earth-fixed velocity (in ECI the pad's rotation would tilt a rising rocket).
        bool o1 = false, o0 = false;
        const glm::dvec3 E1 = rocketEcef(L, B, 1, tau, o1);
        const glm::dvec3 E0 = rocketEcef(L, B, 1, tau - 1.0, o0);
        if (!o1)
            return glm::dvec3(0.0);
        ok = true;
        const glm::dvec3 vE = (o0 && tau > 0.5) ? E1 - E0 : glm::dvec3(0.0);
        return ecefToEci(glm::length(vE) > 0.5 ? glm::normalize(vE) : glm::normalize(E1), t);
    }
    glm::dvec3 P, P1;
    if (!rocketPos(L, B, 1, tau, P))
        return glm::dvec3(0.0);
    if (!rocketPos(L, B, 1, tau - 1.0, P1))
        P1 = P - glm::dvec3(0, 0, 1);
    ok = true;
    if (B.shipLands && t >= B.tShipDesc)
    {
        // The ship coming home: from its entry attitude (nose 60 degrees above the inertial velocity, as the entry
        // glow's listing draws it) to the belly flop (level, nose along its path over the ground) over 90 s; the
        // flip to upright (2 s before the landing burn to 5 s into it); upright in the burn and in the tower.
        auto sm = [](double a, double b, double x) { const double s = glm::clamp((x - a) / (b - a), 0.0, 1.0); return s * s * (3.0 - 2.0 * s); };
        const glm::dvec3 up = glm::normalize(P);
        if (t >= B.tShipFlip + 5.0)
            return up;
        const glm::dvec3 vI = glm::normalize(P - P1);
        glm::dvec3 upp = up - glm::dot(up, vI) * vI;
        upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
        const glm::dvec3 entryAx = glm::normalize(0.5 * vI + 0.866 * upp);
        // Over the ground (Earth-fixed velocity), level.
        const glm::dvec3 E1 = shipDescentEcef(B, t, nullptr), E0 = shipDescentEcef(B, t - 1.0, nullptr);
        glm::dvec3 hz = ecefToEci(E1 - E0, t);
        hz -= glm::dot(hz, up) * up;
        if (glm::length(hz) < 1e-3)
            hz = ecefToEci(-(B.dXF.x * B.LE + B.dXF.y * B.LN), t); // (F lies back along the approach)
        hz = glm::normalize(hz - glm::dot(hz, up) * up);
        glm::dvec3 ax = glm::normalize(glm::mix(entryAx, hz, sm(B.tShipDesc, B.tShipDesc + 90.0, t)));
        ax = glm::normalize(glm::mix(ax, up, sm(B.tShipFlip - 2.0, B.tShipFlip + 5.0, t)));
        return ax;
    }
    const glm::dvec3 vh1 = glm::normalize(P - P1), up = glm::normalize(P);
    glm::dvec3 upp = up - glm::dot(up, vh1) * vh1;
    upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
    auto sm = [](double a, double b, double x) { const double s = glm::clamp((x - a) / (b - a), 0.0, 1.0); return s * s * (3.0 - 2.0 * s); };
    const double tb = B.tDeorbBurn, te = tb + B.burnDur;
    double ph = glm::pi<double>() * sm(tb - 150.0, tb - 20.0, t);
    if (!vh.upperDeorbit)
        ph -= (glm::pi<double>() - glm::radians(60.0)) * sm(te + 60.0, te + 360.0, t);
    return std::cos(ph) * vh1 + std::sin(ph) * upp;
}

// The nose (ECI, unit), smoothed (the booster over 10 s, the upper stage 6 s): no snap at staging, at a burn's start or end, or into orbit.
// Zero when the stage is not flying.
static glm::dvec3 stageNose(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau)
{
    if (stage == 0)
    {
        glm::dvec3 Pe;
        if (!rocketPos(L, B, 0, tau, Pe))
            return glm::dvec3(0.0);
        double sum = 0.0, wsum = 0.0;
        for (int k = 0; k <= 20; ++k)
        {
            const double w = (k == 0 || k == 20) ? 0.5 : 1.0;
            sum += w * boosterPitchRaw(L, B, tau - 0.5 * (double)k);
            wsum += w;
        }
        const double psi = sum / wsum;
        const Track &tr = boosterTr(L, B);
        // Landed (in the tower, on its zone, on a ship's deck riding home): upright where it stands.
        if (tau >= tr.tEnd && tr.landed)
            return glm::normalize(Pe);
        const glm::dvec2 p = tr.at(glm::clamp(tau, 0.0, tr.tEnd));
        const double a = p.x / kR;
        const glm::dvec3 up = std::cos(a) * B.G0 + std::sin(a) * B.Eaz;  // Earth-fixed, at the booster
        const glm::dvec3 dr = -std::sin(a) * B.G0 + std::cos(a) * B.Eaz; // downrange there
        return ecefToEci(std::cos(psi) * dr + std::sin(psi) * up, B.tL + std::max(tau, 0.0));
    }
    glm::dvec3 sum(0.0);
    for (int k = 0; k < 7; ++k)
    {
        bool ok = false;
        const glm::dvec3 n = upperNoseRaw(L, B, tau - (double)k, ok);
        if (ok)
            sum += (k == 0 || k == 6 ? 0.5 : 1.0) * n;
        else if (k == 0)
            return glm::dvec3(0.0);
    }
    return glm::length(sum) > 1e-9 ? glm::normalize(sum) : glm::dvec3(0.0);
}

// A returning stage's entry, te s after the entry interface (B.tStageGone): ~7.4 km/s down to ~65 km, then slowing to
// ~1.2 km/s by 45 km over 8 minutes, along its orbit's plane. Position ECI; speed and altitude out.
// The upper stage's body frame at tau (body -> ECI, columns = its X, Y, Z): +Z the nose (stageNose), -Y as near the
// nadir as it can be — the rocket models' attitude law (primary +Z on the "velocity" the transient hands in,
// secondary -Y on the nadir), so a stack posed in it sits where the stage's mesh is drawn. (2026-10-20)
static glm::dmat3 upperFrame(const SatelliteSim::LaunchState &L, const LBatch &B, double tau)
{
    glm::dvec3 P;
    if (!rocketPos(L, B, 1, tau, P))
        return glm::dmat3(1.0);
    const glm::dvec3 up = glm::normalize(P);
    glm::dvec3 Z = stageNose(L, B, 1, tau);
    if (glm::length(Z) < 0.5)
        Z = up;
    Z = glm::normalize(Z);
    glm::dvec3 Y = up - glm::dot(up, Z) * Z;
    if (glm::length(Y) < 1e-6)
        Y = std::abs(Z.z) < 0.9 ? glm::dvec3(0, 0, 1) - Z.z * Z : glm::dvec3(1, 0, 0) - Z.x * Z;
    Y = glm::normalize(Y);
    return glm::dmat3(glm::cross(Y, Z), Y, Z);
}

// Member i of n's place in its program's stack (2026-10-20; tools/make_rocket_models.py lays the stacks out and checks
// them against every fairing): in the upper stage's body frame (+Z the nose, the payload adapter's top at
// vh.payloadBaseM), the satellite's origin o, its body axes R (columns: its X, Y, Z in the stage's frame) and the
// direction it leaves in. Released top tier first. Pez (Starship): every satellite from the door's slot, flat across
// the ship (its nadir face aft, its X out of the door), leaving through the door (+Y, the leeward side: zenith while
// the ship flies belly down). Flat: nadir face down, one or two a tier, leaving forward along the stage. Column:
// around the dispenser column, nadir face out, the long axis (X) along the stage, leaving radially.
static bool stackSlot(const launch::Vehicle &vh, const launch::Stack &st, size_t i, size_t n, glm::dvec3 &o,
                      glm::dmat3 &R, glm::dvec3 &exitDir)
{
    if (st.layout == launch::Stack::None)
        return false;
    glm::dvec3 slot;
    if (st.layout == launch::Stack::Column)
    {
        const size_t per = (size_t)std::max(st.perTier, 1);
        const size_t tiers = (n + per - 1) / per;
        const size_t k = tiers - 1 - i / per, j = i % per;
        const double phi = glm::two_pi<double>() * (double)j / (double)per +
                           ((k % 2) ? glm::pi<double>() / (double)per : 0.0);
        const glm::dvec3 er(std::cos(phi), std::sin(phi), 0.0), zs(0.0, 0.0, 1.0);
        R = glm::dmat3(zs, glm::cross(er, zs), er);
        slot = er * st.offsetM + zs * (vh.payloadBaseM + st.baseM + ((double)k + 0.5) * st.pitchM);
        exitDir = er;
    }
    else
    {
        R = glm::dmat3(glm::dvec3(0, 1, 0), glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1));
        if (st.layout == launch::Stack::Pez)
        {
            slot = glm::dvec3(0.0, 0.0, vh.doorM);
            exitDir = glm::dvec3(0.0, 1.0, 0.0);
        }
        else
        {
            const size_t per = (size_t)std::clamp(st.perTier, 1, 2);
            const size_t tiers = (n + per - 1) / per;
            const size_t k = tiers - 1 - i / per, j = i % per;
            // (two side by side along the stage's Y, the zenith: the upper one leaves first, both upward)
            const double s = per == 2 ? ((j == 0) == st.sideX ? 1.0 : -1.0) : 0.0;
            // The satellite's X lies along the stage's Y, its Y along the stage's X.
            slot = (st.sideX ? glm::dvec3(0.0, s * st.offsetM, 0.0) : glm::dvec3(s * st.offsetM, 0.0, 0.0)) +
                   glm::dvec3(0.0, 0.0, vh.payloadBaseM + st.baseM + ((double)k + 0.5) * st.pitchM);
            // Up, sliding off the stack's side (2026-10-21; it left forward along the stage, and a pure along-track push
            // brings a train's members back together ~0.4 orbit later: releaseInPlane).
            exitDir = glm::dvec3(0.0, 1.0, 0.0);
        }
    }
    o = slot - R * st.center;
    return true;
}

// The upper stage's model at t (2026-10-20): with its fairing until the halves separate, bare after (the payload on its
// adapter), and a crewed stage empty once its spacecraft has left. Falls back to "_upper" when a variant is missing.
static int upperStageType(const SatelliteSim &sim, const SatelliteSim::LaunchState &L, const LBatch &B, double t)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    auto find = [&](const char *part) {
        auto it = sim.rocketTypes.find("rocket_" + vh.id + part);
        return it != sim.rocketTypes.end() ? it->second : -1;
    };
    // A crewed stage once its spacecraft has separated (with or without a shroud: Dragon has none).
    if (L.cat.programs[B.prog].crew && t >= B.tD)
    {
        const int e = find("_upper_empty");
        if (e >= 0)
            return e;
    }
    if (B.tFair > 0.0 && t >= B.tFair)
    {
        const int b = find("_upper_bare");
        if (b >= 0)
            return b;
    }
    return find("_upper");
}

// The fairing halves' fall (2026-10-20), built the first time they are drawn: each half leaves the stage at tFair with
// the stage's velocity + 1.8 m/s outward (the separation springs), its top tipping outward at ~11 deg/s, and falls
// under gravity and drag (ballistic coefficient 60 kg/m^2, air ~1.2 exp(-h / 8 km) moving with the Earth); its centre
// is sampled every kFairDt s until it is below 60 km on the way down (it would burn or parachute). cb = the +X half's
// centre on the stage (stage frame: launches.json fairing_center_m, the model's origin).
static void ensureFairing(const SatelliteSim::LaunchState &L, LBatch &B, const glm::dvec3 &cb)
{
    if (B.fairBuilt || B.tFair < 0.0)
        return;
    B.fairBuilt = true;
    const double tau = B.tFair - B.tL;
    glm::dvec3 P, Pa, Pb;
    if (!rocketPos(L, B, 1, tau, P) || !rocketPos(L, B, 1, tau + 0.5, Pa) || !rocketPos(L, B, 1, tau - 0.5, Pb))
        return;
    const glm::dvec3 V = Pa - Pb;
    const glm::dmat3 S0 = upperFrame(L, B, tau);
    const glm::dvec3 wE(0.0, 0.0, 7.2921159e-5);
    for (int h = 0; h < 2; ++h)
    {
        LFairing &F = B.fair[h];
        F.c.clear();
        F.cv.clear();
        const double ca = h ? -1.0 : 1.0; // the second half: the first turned 180 deg about the stage's axis
        const glm::dmat3 Rh = S0 * glm::dmat3(glm::dvec3(ca, 0, 0), glm::dvec3(0, ca, 0), glm::dvec3(0, 0, 1));
        F.q0 = glm::normalize(glm::quat_cast(Rh));
        const uint64_t hs = B.seed ^ (0xFA1Bull + (uint64_t)h * 0x9E3779B97F4A7C15ull);
        F.w = Rh * glm::dvec3(0.02 * (h01(hs, 1, 1) - 0.5), glm::radians(9.0 + 4.0 * h01(hs, 2, 1)),
                              0.05 * (h01(hs, 3, 1) - 0.5));
        glm::dvec3 r = P + Rh * cb;
        glm::dvec3 v = V + Rh * glm::dvec3(1.8, 0.0, 0.0);
        auto acc = [&](const glm::dvec3 &x, const glm::dvec3 &u) {
            const double rr = glm::length(x);
            const glm::dvec3 g = -launch::kGM * x / (rr * rr * rr);
            const double rho = 1.225 * std::exp(-(rr - kR) / 8000.0);
            const glm::dvec3 vr = u - glm::cross(wE, x);
            return g - (0.5 * rho * glm::length(vr) / 60.0) * vr;
        };
        bool rising = true;
        for (int k = 0; k <= 1200; ++k)
        {
            if (k % (int)kFairDt == 0)
            {
                F.c.push_back(r);
                F.cv.push_back(v);
            }
            const double hgt = glm::length(r) - kR;
            if (glm::dot(r, v) < 0.0)
                rising = false;
            if (!rising && hgt < 60000.0)
                break;
            // RK4, 1 s
            const glm::dvec3 k1v = acc(r, v), k1r = v;
            const glm::dvec3 k2v = acc(r + 0.5 * k1r, v + 0.5 * k1v), k2r = v + 0.5 * k1v;
            const glm::dvec3 k3v = acc(r + 0.5 * k2r, v + 0.5 * k2v), k3r = v + 0.5 * k2v;
            const glm::dvec3 k4v = acc(r + k3r, v + k3v), k4r = v + k3v;
            r += (k1r + 2.0 * k2r + 2.0 * k3r + k4r) / 6.0;
            v += (k1v + 2.0 * k2v + 2.0 * k3v + k4v) / 6.0;
        }
        F.tEnd = B.tFair + kFairDt * (double)(F.c.size() - 1);
    }
}

// A fairing half at t: its centre = its model's origin (ECI), its velocity, attitude (body -> ECI). False outside
// [tFair, tEnd].
static bool fairingAt(const LBatch &B, int h, double t, glm::dvec3 &pos, glm::dvec3 &vel, glm::dmat3 &R)
{
    const LFairing &F = B.fair[h];
    if (F.c.size() < 2 || t < B.tFair || t > F.tEnd)
        return false;
    const double x = (t - B.tFair) / kFairDt;
    const size_t i = std::min((size_t)x, F.c.size() - 2);
    const double s = x - (double)i, d = kFairDt;
    // Cubic Hermite on the samples and their velocities.
    const double s2 = s * s, s3 = s2 * s;
    const glm::dvec3 c = (2 * s3 - 3 * s2 + 1) * F.c[i] + (s3 - 2 * s2 + s) * d * F.cv[i] + (-2 * s3 + 3 * s2) * F.c[i + 1] +
                         (s3 - s2) * d * F.cv[i + 1];
    vel = glm::mix(F.cv[i], F.cv[i + 1], s);
    const double wl = glm::length(F.w);
    glm::dquat q = F.q0;
    if (wl > 1e-12)
        q = glm::angleAxis(wl * (t - B.tFair), F.w / wl) * q;
    R = glm::mat3_cast(glm::normalize(q));
    pos = c;
    return true;
}

// A transient's full attitude (a fairing half, 2026-10-20) into the elements its mesh slot is posed from.
static void transientAtt(SatOrbitElems &e, const SatelliteSim::SelTransient &s)
{
    if (!s.hasAtt)
        return;
    e.attFix = true;
    e.attQ = s.attQ;
    e.attOmega = s.attOmega;
    e.attT0 = s.attT0;
    e.attSlewDur = 0.0;
}

// A booster's landing legs (2026-10-20, issue 20261020_213739): folded up against the body ("fold" parts in the rocket
// models) from lift-off, swung down kLegDeployS seconds before touchdown over kLegDeployDurS (the unfolding progress of
// its mesh slot, satUnfoldAt). Every other rocket transient keeps the default (deployed: nothing of theirs folds).
static void transientLegs(const SatelliteSim::LaunchState &L, SatOrbitElems &e, const SatelliteSim::SelTransient &s)
{
    // Starship's payload door (2026-10-21, issue 20261021_174219: "the hatch is always open"): the upper stage's
    // "payload_door" panel is a stowable part (tools/make_rocket_models.py) that slides up off its dark slot as it
    // stows. Closed (unfolding progress 1, the rest pose) except from kDoorLeadS before the first release to
    // kDoorLagS after the last, opening / closing over kDoorMoveS. The progress here is not a ramp in time, so it is
    // written as a frame-constant value: unfoldT0 = now - P x a duration far longer than any frame. Upper stages
    // without stowable parts (every other vehicle) are unaffected.
    if (s.kind == 1 && s.stage == 1)
    {
        auto it = L.batches.find(s.a);
        if (it == L.batches.end() || it->second.mem.empty())
            return;
        const LBatch &B = it->second;
        constexpr double kDoorLeadS = 90.0, kDoorLagS = 60.0, kDoorMoveS = 20.0, kHold = 1.0e9;
        const double t = B.tL + s.tau;
        const double tOpen = B.tD - kDoorLeadS;
        const double tShut = B.tD + B.deployInterval * (double)(B.mem.size() - 1) + kDoorLagS;
        const double open = std::min(glm::clamp((t - tOpen) / kDoorMoveS, 0.0, 1.0),
                                     glm::clamp((tShut + kDoorMoveS - t) / kDoorMoveS, 0.0, 1.0));
        const double P = 1.0 - open * open * (3.0 - 2.0 * open);
        e.unfoldT0 = t - P * kHold;
        e.unfoldDur = kHold;
        return;
    }
    if (s.kind != 1 || s.stage != 0)
        return;
    auto it = L.batches.find(s.a);
    if (it == L.batches.end())
        return;
    const LBatch &B = it->second;
    const Track &tr = boosterTr(L, B);
    constexpr double kLegDeployS = 7.0, kLegDeployDurS = 2.5;
    e.unfoldT0 = tr.landed ? B.tL + tr.tEnd - kLegDeployS : B.tL + 1.0e9; // expendable: never
    e.unfoldDur = kLegDeployDurS;
}

static glm::dvec3 shipEntry(const LBatch &B, double te, double *vOut, double *hOut)
{
    te = std::clamp(te, 0.0, 480.0);
    const double v0 = B.vEI; // the entry ellipse's speed at 100 km (continuous with rocketPos)
    auto vAt = [v0](double u) { const double s = glm::clamp((u - 0.35) / 0.65, 0.0, 1.0); return v0 - (v0 - 1200.0) * s * s * (3.0 - 2.0 * s); };
    // (+ a term that starts it falling at the ellipse's own rate, vrEI, and is gone by u = 0.45)
    const double vr0 = B.vrEI;
    auto hAt = [vr0](double u) {
        const double a = glm::clamp(u / 0.45, 0.0, 1.0), b = glm::clamp((u - 0.45) / 0.55, 0.0, 1.0);
        const double w = u < 0.45 ? u * (1.0 - a) * (1.0 - a) : 0.0;
        return 100000.0 - 35000.0 * a * a * (3.0 - 2.0 * a) - 20000.0 * b * b * (3.0 - 2.0 * b) + vr0 * 480.0 * w;
    };
    // Along-track distance: the speed integrated (Simpson, 16 intervals).
    double sAlong = 0.0;
    const int N = 16;
    for (int i = 0; i <= N; ++i)
    {
        const double w = (i == 0 || i == N) ? 1.0 : (i % 2 ? 4.0 : 2.0);
        sAlong += w * vAt(te * i / N / 480.0);
    }
    sAlong *= te / (3.0 * N);
    const double u = te / 480.0;
    if (vOut)
        *vOut = vAt(u);
    if (hOut)
        *hOut = hAt(u);
    // From the entry interface on the deorbit ellipse (angle per metre at the ellipse's radius there), plus the
    // ellipse's own offset there, eased out over the first minute (continuity at the hand-over).
    const double th = B.thEI + sAlong / (kR + 100000.0);
    const double fo = 1.0 - glm::smoothstep(0.0, 60.0, te);
    return (kR + hAt(u)) * (std::cos(th) * B.U0 + std::sin(th) * B.A0) + fo * B.entryOff;
}

// When the upper stage vents what is left. An expendable stage: spinning, after its deorbit burn (the Falcon 9 dump
// spirals). A returning ship (2026-10-26, issue 20261026_194151): a CONTROLLED dump out of its aft end along its own
// axis, 20 min before its deorbit burn — hours after deployment, km from its train (it was 10 min after deploying,
// among the satellites, and spinning like an out-of-control stage).
static double ventStart(const SatelliteSim::LaunchState &L, const LBatch &B)
{
    return L.cat.vehicles[B.veh].upperDeorbit ? B.tDeorbBurn + 50.0 : std::max(B.tDeorbBurn - 1200.0, B.tD + 3.0 * 3600.0);
}

// A rocket model's engines (tools/make_rocket_models.py): the exit plane's height on the body axis (the origin is the
// aft end), the radius the engines' exits span, and the vehicle's length. From the components named engines /
// upper_nozzle / side_nozzle (cones, the exit at their bottom); a model without them: the aft end, 0.7 x the body.
static glm::dvec3 engineGeomOf(const SatelliteType &ty)
{
    double zExit = 1e30, rExit = 0.0, zMin = 1e30, zMax = -1e30, rBody = 0.0;
    if (ty.model)
        for (const SatComponent &c : ty.model->components)
        {
            const bool revolved = c.prim == PrimitiveKind::Cone || c.prim == PrimitiveKind::Cylinder ||
                                  c.prim == PrimitiveKind::Profile;
            const double hz = revolved ? 0.5 * c.height : 0.0;
            zMin = std::min(zMin, (double)c.position.z - hz);
            zMax = std::max(zMax, (double)c.position.z + hz);
            const double rxy = std::sqrt((double)c.position.x * c.position.x + (double)c.position.y * c.position.y);
            if (c.prim == PrimitiveKind::Cylinder)
                rBody = std::max(rBody, rxy + c.radius);
            if (c.prim == PrimitiveKind::Profile)
                for (const glm::vec2 &q : c.profile)
                    rBody = std::max(rBody, rxy + (double)q.y);
            const bool eng = c.name.rfind("engines", 0) == 0 || c.name.rfind("upper_nozzle", 0) == 0 ||
                             c.name.rfind("side_nozzle", 0) == 0;
            if (eng && (c.prim == PrimitiveKind::Cone || c.prim == PrimitiveKind::Profile)) // the exit at the bottom
            {
                zExit = std::min(zExit, (double)c.position.z - hz);
                rExit = std::max(rExit, rxy + c.radius);
            }
        }
    if (zMin > zMax)
        return glm::dvec3(0.0, 2.0, 40.0);
    if (rExit <= 0.0)
    {
        zExit = 0.0;
        rExit = std::max(0.7 * rBody, 0.5);
    }
    return glm::dvec3(zExit, rExit, zMax - zMin);
}

// The exhaust's parcels, from the whole flight (built once, when the launch first matters to the view).
static void buildParcels(const SatelliteSim::LaunchState &L, LBatch &B)
{
    FPROF_SCOPE("launch.buildParcels");
    B.parcels.clear();
    B.parcelsBuilt = true;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const Track &bt = boosterTr(L, B);
    // The launch's wind: a hashed direction, Earth-fixed at the pad.
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), B.G0)), north = glm::cross(B.G0, east);
    const double wa = glm::two_pi<double>() * h01(B.seed, 20, 1);
    const glm::dvec3 windDir = std::cos(wa) * east + std::sin(wa) * north;
    const double windGain = 0.5 + 1.0 * h01(B.seed, 21, 1);
    // Scattering cross-section per kg of exhaust: low down the visible trail (condensing vapour and soot: an opaque
    // column ~50-100 m wide near the pad); high up the exhaust's water freezes into sub-micron ice as it expands
    // (~3000 m^2/kg of ice, ~10% of the exhaust condensing): the bright, sunlit bell seen at twilight.
    auto kappaAt = [&](double h) { return glm::mix(10.0, 1000.0, glm::smoothstep(25000.0, 90000.0, h)); };
    auto albedoAt = [&](double h) { return glm::mix(0.95 - 0.55 * vh.soot, 0.98, glm::smoothstep(15000.0, 40000.0, h)); };
    const double windPhase = glm::two_pi<double>() * h01(B.seed, 24, 1);
    const int padI = (B.site >= 0 && (size_t)B.site < L.sitePad.size()) ? L.sitePad[(size_t)B.site] : -1;
    const LPad *pad = padI >= 0 ? &L.pads[(size_t)padI] : nullptr;
    const double padGround = pad ? pad->elevM : std::max(B.padBase - vh.mountM, 3.0);
    int stream = -1;           // a burn: its parcels are drawn as one polyline (consecutive in B.parcels)
    double lastEmit = -1e30;   // a gap in the emission starts a new stream
    int lastStage = -1, lastMode = -1;
    double kappaMul = 1.0; // (a landing burn's exhaust: 0.5, set below)
    auto emit = [&](int stage, double tau, double dtE, double flow, double vex, int mode) {
        bool ok0, ok1;
        const glm::dvec3 p = rocketEcef(L, B, stage, tau, ok0);
        const glm::dvec3 pb = rocketEcef(L, B, stage, tau - 0.25, ok1);
        bool ok2;
        const glm::dvec3 pf = rocketEcef(L, B, stage, tau + 0.25, ok2);
        if (!ok0 || !ok1 || !ok2)
            return;
        const glm::dvec3 vel = (pf - pb) / 0.5;
        const double spd = glm::length(vel);
        const glm::dvec3 up = glm::normalize(p);
        glm::dvec3 ex; // the exhaust's direction
        if (mode == 0)
            ex = spd > 5.0 ? -vel / spd : -up; // thrust along the velocity
        else if (mode == 1)
        {
            // Boostback / entry / landing: the thrust from the track's acceleration less gravity.
            bool okA, okB;
            const glm::dvec3 pa = rocketEcef(L, B, stage, tau - 1.0, okA), pc = rocketEcef(L, B, stage, tau + 1.0, okB);
            const glm::dvec3 acc = (pc - 2.0 * p + pa) + 9.81 * up;
            ex = glm::length(acc) > 1.0 ? -glm::normalize(acc) : (spd > 5.0 ? vel / spd : -up);
        }
        else if (mode == 2)
            ex = spd > 5.0 ? vel / spd : up; // retrograde burn: the exhaust goes forward
        else if (mode == 4)
        {
            // A controlled dump: out of the aft end, along the stage's axis (stageNose, ECI -> ECEF).
            const glm::dvec3 nz = stageNose(L, B, stage, tau);
            ex = glm::length(nz) > 0.5 ? -glm::normalize(eciToEcef(nz, B.tL + tau)) : (spd > 5.0 ? -vel / spd : -up);
        }
        else
        {
            // Venting while spinning: the jet turns about the velocity (a spiral seen from the ground).
            const glm::dvec3 f = spd > 5.0 ? vel / spd : glm::dvec3(1, 0, 0);
            glm::dvec3 s1 = glm::cross(f, up);
            s1 = glm::length(s1) > 1e-9 ? glm::normalize(s1) : glm::dvec3(0, 1, 0);
            const glm::dvec3 s2 = glm::cross(f, s1);
            const double ang = glm::two_pi<double>() * tau / (8.0 + 10.0 * h01(B.seed, 22, 1));
            ex = std::cos(ang) * s1 + std::sin(ang) * s2;
        }
        const double h = glm::length(p) - kR;
        const double m = flow * dtE;
        const double len = std::max(spd * dtE, 10.0);
        const double maxAge = h > 40000.0 ? kPlumeAgeHigh : kPlumeAgeLow;
        // Vented propellant: droplets freezing to ice, a large cross-section, spreading slowly (the dump spirals).
        const bool vent = mode >= 3;
        // (A vent's parcel is a stretch of its jet, not of the stage's path: the stage carries the jet with it.)
        // The low trail (2026-10-20): a turbulent column that billows and thickens as it entrains the air (it grows
        // ~0.05x its speed through the air, fading out by 6 km), resting on the pad's ground near the pad.
        launch::ParcelOpts O;
        O.growK = vent ? 0.0 : 0.05 * (double)B.plumeGrow * (1.0 - glm::smoothstep(1500.0, 6000.0, h - padGround));
        O.groundM = glm::length(glm::normalize(p) - B.G0) * kR < 30000.0 ? padGround : 3.0;
        // (2026-10-26, issue 20261026_210053) and it keeps spreading with age (turbulence, shear): Richardson growth,
        // ~1 km wide after 10 min and ~5 km by the end of its life in the troposphere (it was ~250 m), half the rate in the stable
        // stratosphere, none in the bells above 60-90 km: already hundreds of km wide, widened further they became a
        // grey veil over the whole sky (and cost the cloud composite ~2.7 ms, every pixel inside their cones).
        O.richK = vent ? 0.0
                       : 0.18 * (double)(B.plumeGrow * B.plumeDisp) * glm::mix(1.0, 0.5, glm::smoothstep(12000.0, 20000.0, h)) *
                             (1.0 - glm::smoothstep(60000.0, 90000.0, h));
        B.parcels.push_back(launch::makeParcel(tau, p, vel + vex * ex, m, vent ? 2000.0 : kappaAt(h) * kappaMul, albedoAt(h),
                                               vent ? ex : (spd > 5.0 ? vel / spd : up), vent ? vex * dtE : len,
                                               maxAge, windDir, windGain, vent ? 30.0 : 2000.0, windPhase, &O));
        B.parcels.back().thin = vent;
        if (!vent)
        {
            if (stage != lastStage || mode != lastMode || tau - lastEmit > 1.6 * dtE)
                ++stream;
            B.parcels.back().stream = stream;
            lastEmit = tau;
            lastStage = stage;
            lastMode = mode;
        }
        B.parcels.back().stage = stage;
        B.parcels.back().dt = dtE;
    };
    // ── Ground clouds (2026-10-20, the user: "smoke trail effects (volumetric) on launch, dust kicking up from
    // landing"). Parcels on the ground, drawn as lumpy round puffs (cloud_march.comp's billows + self-shadow), lit by
    // the flame at night (launchFillVisuals' flame light). All from hashes of the batch: a pure function of sim time.
    auto groundCloud = [&](int kind, double t0, const glm::dvec3 &G, double gnd, const glm::dvec3 &dir, double spd,
                           double m, double kappa, double albedo, const glm::vec3 &tint, double evap, double sig0,
                           double buoy, double growK) {
        launch::ParcelOpts O;
        O.groundM = gnd;
        O.evapS = evap;
        O.growK = growK * (double)B.plumeGrow;
        O.buoy = buoy;
        O.buoyS = 90.0;
        O.sig0 = sig0;
        const glm::dvec3 p = G * (kR + gnd + 0.6 * sig0);
        B.parcels.push_back(launch::makeParcel(t0, p, spd * dir + 6.0 * G, m, kappa, albedo, dir, 0.6 * sig0,
                                               kPlumeAgeLow, windDir, windGain, 300.0, windPhase, &O));
        B.parcels.back().ground = kind;
        B.parcels.back().tint = tint;
    };
    // (1) Lift-off: the exhaust hits the pad while the stack is low (within hImp of the ground, longer for more thrust)
    // and rolls out along the ground with the deluge's steam: Starship's mount sprays water up through its steel
    // deflector, so its cloud billows out all round; the other pads' flame trench throws most of it out of the trench's
    // mouth behind the pad (-u), a little spilling round the sides. It rises (hot) and drifts for minutes.
    {
        const glm::dvec3 G = B.G0;
        const glm::dvec3 u = pad ? pad->u : north, v = pad ? pad->v : east;
        const bool deluge = pad ? padKindAt(*pad, B.tL, L.deployOn) == 2 : vh.mountM > 0.0;
        const double hImp = 60.0 * std::pow(vh.flow1 / 1000.0, 0.4);
        const double dtG = 0.75;
        int k = 0;
        for (double tau = 0.25; tau < 20.0; tau += dtG, ++k)
        {
            bool okR;
            const glm::dvec3 P = rocketEcef(L, B, 0, tau, okR);
            if (!okR)
                break;
            const double hAbove = glm::length(P) - kR - padGround - vh.mountM;
            const double impK = std::pow(glm::clamp(1.0 - hAbove / hImp, 0.0, 1.0), 1.5);
            if (impK <= 0.0 && tau > 3.0)
                break;
            const double mStep = vh.flow1 * dtG * (deluge ? 1.3 : 0.9) * std::max(impK, 0.15);
            const int nRad = deluge ? 3 : 1;
            for (int j = 0; j < nRad; ++j)
            {
                const double ang = glm::two_pi<double>() * (h01(B.seed, 230 + k, j) + (double)j / nRad) + 2.39996 * k;
                const glm::dvec3 dir = std::cos(ang) * u + std::sin(ang) * v;
                const double spd = (70.0 + 80.0 * h01(B.seed, 231 + k, j)) * (0.4 + 0.6 * impK);
                groundCloud(1, tau, G, padGround, dir, spd, mStep * (deluge ? 1.0 / nRad : 0.3), 8.0,
                            deluge ? 0.97 : 0.93 - 0.2 * vh.soot, glm::vec3(1.0f), 360.0, 22.0, 0.25, 0.12);
            }
            if (!deluge)
            {
                // Out of the trench's mouth, ~70 m behind the pad, spreading as a fan.
                const double sw = (h01(B.seed, 232 + k, 0) - 0.5) * 0.9;
                const glm::dvec3 dir = glm::normalize(-u + sw * v);
                const glm::dvec3 Gm = glm::normalize(G - u * (70.0 / kR));
                groundCloud(1, tau, Gm, padGround, dir, (90.0 + 70.0 * h01(B.seed, 233 + k, 0)) * (0.5 + 0.5 * impK),
                            mStep * 0.7, 8.0, 0.92 - 0.25 * vh.soot, glm::vec3(1.0f), 420.0, 18.0, 0.2, 0.12);
            }
        }
    }
    // (3) Landings: in a landing burn's last stretch the exhaust reaches the ground and kicks up a cloud spreading
    // radially. What it is follows the surface under it: a pad (a tower catch, a landing zone) mostly steam and
    // exhaust with some grit (grey-white); bare land dust (tan, lingering); the sea spray (white, gone in a minute; a
    // droneship's deck sits in it).
    auto landingCloud = [&](int stage, double tau0, double tau1, double flowL, int landPadI, int surfForce = -1,
                            double gndForce = 0.0) {
        if (tau1 <= tau0)
            return;
        bool okE;
        const glm::dvec3 PE = rocketEcef(L, B, stage, tau1 - 0.05, okE);
        if (!okE)
            return;
        const glm::dvec3 GE = glm::normalize(PE);
        const double la = glm::degrees(std::asin(glm::clamp(GE.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(GE.y, GE.x));
        int surf; // 0 pad, 1 land, 2 sea, 3 a ship's deck
        double gnd;
        if (surfForce >= 0)
        {
            surf = surfForce;
            gnd = gndForce;
        }
        else if (landPadI >= 0)
        {
            surf = 0;
            gnd = L.pads[(size_t)landPadI].elevM;
        }
        else
        {
            const double dem = demElevCpu(la, lo);
            surf = (dem > 1.0 && shoreDistCpu(la, lo) < 0.0) ? 1 : 2;
            gnd = surf == 1 ? dem : 0.0;
        }
        const double hImp = 80.0 * std::pow(flowL / 1000.0, 0.4);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), GE)), nE = glm::cross(GE, eE);
        const double dtL = 0.5;
        int k = 0;
        for (double tau = tau0; tau < tau1; tau += dtL, ++k)
        {
            bool okR;
            const glm::dvec3 P = rocketEcef(L, B, stage, tau, okR);
            if (!okR)
                continue;
            const double hAbove = glm::length(P) - kR - gnd;
            const double impK = std::pow(glm::clamp(1.0 - hAbove / hImp, 0.0, 1.0), 1.5);
            if (impK <= 0.0)
                continue;
            const glm::dvec3 G = glm::normalize(P);
            for (int j = 0; j < 3; ++j)
            {
                const double ang = glm::two_pi<double>() * (h01(B.seed, 260 + 7 * stage + k, j) + j / 3.0) + 2.39996 * k;
                const glm::dvec3 dir = std::cos(ang) * eE + std::sin(ang) * nE;
                const double spd = (40.0 + 50.0 * h01(B.seed, 261 + 7 * stage + k, j)) * (0.4 + 0.6 * std::sqrt(impK));
                const double mL = flowL * dtL * impK / 3.0;
                if (surf == 3) // a ship's steel deck: the exhaust deflected sideways, a low, thin skirt (2026-10-21)
                    groundCloud(2, tau, G, gnd, dir, spd * 1.2, mL * 1.2, 9.0, 0.9, glm::vec3(1.0f, 0.97f, 0.92f), 50.0, 6.0,
                                0.05, 0.05);
                else if (surf == 2) // sea spray: fine droplets, evaporating within a minute or two
                    groundCloud(2, tau, G, gnd, dir, spd * 1.3, mL * 6.0, 14.0, 0.96, glm::vec3(1.0f), 90.0, 14.0, 0.0, 0.12);
                else if (surf == 1) // dust from bare ground
                    groundCloud(2, tau, G, gnd, dir, spd, mL * 1.6, 9.0, 0.6, glm::vec3(1.0f, 0.84f, 0.64f), 480.0, 10.0, 0.08, 0.12);
                else // a pad: steam and exhaust, a little grit
                    groundCloud(2, tau, G, gnd, dir, spd, mL, 8.0, 0.85, glm::vec3(1.0f, 0.96f, 0.9f), 300.0, 10.0, 0.15, 0.12);
            }
        }
    };
    // The booster: ascent (every 1 s low, 1.5 s above ~20 km), then its burns. The parcels are the polyline's knots:
    // the trail between them is drawn as segments (launchFillVisuals), so the spacing sets how finely it bends.
    for (double tau = 1.0; tau < vh.t1; )
    {
        const double dtE = tau < 60.0 ? 1.0 : 1.5;
        emit(0, tau, dtE, vh.flow1, vh.vex1, 0);
        tau += dtE;
    }
    for (double tau = vh.t1 + 1.0; tau < bt.tEnd; tau += 2.0)
    {
        uint8_t b = 0;
        bt.at(tau, nullptr, &b);
        // (A landing burn's exhaust at half the trail's cross-section, 2026-10-21: one deeply throttled engine's gas,
        // with no deluge steam in it; at the full value the parcels piled up round a booster nearing the deck were an
        // opaque ball its flame lit from inside, the halo of issue 20261020_213739.)
        kappaMul = b == launch::BURN_LANDING ? 0.5 : 1.0;
        if (b == launch::BURN_BOOSTBACK || b == launch::BURN_ENTRY || b == launch::BURN_LANDING)
            emit(0, tau, 2.0, vh.flow1 * (b == launch::BURN_LANDING ? 0.12 : 0.35), vh.vex1, 1);
    }
    kappaMul = 1.0;
    if (vh.recovery != 0)
    {
        // The landing burn's span (its last seconds reach the ground).
        double l0 = -1.0, l1 = -1.0;
        for (double tau = vh.t1 + 1.0; tau <= bt.tEnd; tau += 0.5)
        {
            uint8_t b = 0;
            bt.at(tau, nullptr, &b);
            if (b == launch::BURN_LANDING)
            {
                if (l0 < 0.0)
                    l0 = tau;
                l1 = tau;
            }
        }
        if (l0 >= 0.0)
        {
            // Where the booster really lands (the recovery plan: a tower, a landing zone, a ship's deck, the ground).
            glm::dvec3 EL;
            double tTouch;
            const int lk = boosterLandingAt(L, B, B.tL + bt.tEnd, EL, tTouch);
            const double gL = glm::length(EL) - kR;
            // On a ship's deck (2026-10-21, issue 20261020_213739: the old sea spray, 6x the exhaust's mass as droplets
            // growing at 7 m/s, was a round milky dome ~50 m across over the deck, lit white by the burn): the exhaust
            // the steel deck deflects, a thin skirt that spreads low and is gone in under a minute.
            if (lk == kLandShip)
                landingCloud(0, l0, std::min(l1 + 0.5, bt.tEnd), vh.flow1 * 0.12, -1, 3, gL);
            else if (lk == kLandLz)
                landingCloud(0, l0, std::min(l1 + 0.5, bt.tEnd), vh.flow1 * 0.12, -1, 0, gL);
            else if (lk == kLandGround)
                landingCloud(0, l0, std::min(l1 + 0.5, bt.tEnd), vh.flow1 * 0.12, -1, 1, gL);
            else if (lk != kLandNone)
                landingCloud(0, l0, std::min(l1 + 0.5, bt.tEnd), vh.flow1 * 0.12, padI);
        }
    }
    // The upper stage to orbit: every 1 s. In near vacuum consecutive parcels separate at the exhaust speed (~3.4 km
    // a second of burn) and each spreads at ~2 km/s: at 2.5 s the youngest stood 8.5 km apart and swelled into a
    // string of separate puffs before they merged (the user's "poofs").
    for (double tau = vh.t1 + 1.0; tau < B.t2; tau += 1.0)
        emit(1, tau, 1.0, vh.flow2, vh.vex2, 0);
    // Its deorbit burn, and maybe a spinning vent of what is left (the spirals seen from Norway, New Zealand...).
    {
        // (every stage burns to deorbit since 2026-10-19, for B.burnDur; part of its engines)
        const double tb = B.tDeorbBurn - B.tL;
        for (double tau = tb; tau < tb + B.burnDur; tau += 1.0)
            emit(1, tau, 1.0, vh.flow2 * 0.3, vh.vex2, 2); // knots every second, like the ascent's
    }
    // A returning ship's landing burn (2026-10-19): its exhaust over the pad.
    if (B.shipLands)
        for (double tau = B.tShipFlip - B.tL + 0.5; tau < B.tShipCatch - B.tL; tau += 2.0)
            emit(1, tau, 2.0, vh.flow2 * 0.5, vh.vex2, 1);
    if (B.shipLands)
        landingCloud(1, B.tShipFlip - B.tL, B.tShipCatch - B.tL, vh.flow2 * 0.5, B.landPad);
    if (B.vents)
    {
        const double tv = ventStart(L, B) - B.tL;
        if (vh.upperDeorbit)
            for (double tau = tv; tau < tv + 120.0; tau += 0.5)
                emit(1, tau, 0.5, 80.0, 700.0, 3); // ~10 t of residual propellant over two minutes
        else
            for (double tau = tv; tau < tv + 180.0; tau += 0.5)
                emit(1, tau, 0.5, 55.0, 350.0, 4); // ~10 t over three minutes through the vents, slower
    }
}

// ── Crew spacecraft (2026-10-20, issue 20261020_214759) ────────────────────────────────────────────────────────
// After separation the spacecraft is its own object: a pure function of t built per batch (buildCrew), drawn and
// listed as a transient (kind 8) by launchFillVisuals, docked to its station's port (the station model's
// "docking_ports") until it leaves. See LCrew for the timeline. (The transfer time: crewHohmannT, above.)
// A Hohmann transfer from ra up to rb (rb >= ra), tau s after its start: the radius and the angle swept (0 .. pi).
static void hohmannAt(double ra, double rb, double tau, double &r, double &nu)
{
    const double a = 0.5 * (ra + rb), e = std::abs(rb - ra) / std::max(ra + rb, 1.0);
    const double n = std::sqrt(launch::kGM / (a * a * a));
    const double M = glm::clamp(n * tau, 0.0, glm::pi<double>());
    double E = M;
    for (int i = 0; i < 12; ++i)
        E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
    r = a * (1.0 - e * std::cos(E));
    nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(0.5 * E), std::sqrt(1.0 - e) * std::cos(0.5 * E));
}
// The station's position and body frame (columns X along its velocity, Y = Z x X, Z nadir: the stations' two-vector
// law, so a port in its model's frame lands where the station's mesh draws it).
static void crewStation(const LCrew &C, double t, glm::dvec3 &Ps, glm::dmat3 &R)
{
    const SatOrbitState s = satOrbitStateAt(C.st, t);
    Ps = s.posEci;
    const glm::dvec3 Z = glm::normalize(s.nadir);
    glm::dvec3 X = s.velocity - glm::dot(s.velocity, Z) * Z;
    X = glm::length(X) > 1e-9 ? glm::normalize(X) : glm::normalize(glm::cross(Z, glm::dvec3(0, 0, 1)));
    R = glm::dmat3(X, glm::cross(Z, X), Z);
}
// Unit direction from a to b by u in [0, 1]; nearly opposite ones go round below the station (+Z, nadir: the R-bar
// side a Dragon comes up from).
static glm::dvec3 crewSlerpDir(const glm::dvec3 &a, const glm::dvec3 &b, double u)
{
    auto sl = [](const glm::dvec3 &p, const glm::dvec3 &q, double w) {
        const double c = glm::clamp(glm::dot(p, q), -1.0, 1.0), om = std::acos(c);
        if (om < 1e-6)
            return p;
        return glm::normalize((std::sin((1.0 - w) * om) * p + std::sin(w * om) * q) / std::sin(om));
    };
    if (glm::dot(a, b) > -0.85)
        return sl(a, b, u);
    glm::dvec3 m = a + b;
    if (glm::length(m) < 0.5)
    {
        m = glm::dvec3(0, 0, 1) - glm::dot(glm::dvec3(0, 0, 1), a) * a;
        if (glm::length(m) < 1e-3)
            m = glm::dvec3(1, 0, 0) - glm::dot(glm::dvec3(1, 0, 0), a) * a;
    }
    m = glm::normalize(m);
    return u < 0.5 ? sl(a, m, 2.0 * u) : sl(m, b, 2.0 * u - 1.0);
}
// The approach / departure leg flying at t (its cut, when the station's loss called it off, ends it early).
static const LCrewSeg *crewSegAt(const LCrew &C, double t)
{
    if (C.segs.empty())
        return nullptr;
    const LCrewSeg *g = &C.segs.back();
    for (const LCrewSeg &q : C.segs)
        if (t < std::min(q.t1, q.tCut))
        {
            g = &q;
            break;
        }
    return g;
}
static glm::dvec3 crewSegPos(const LCrew &C, const LCrewSeg &g, double t)
{
    const double T = std::max(g.t1 - g.t0, 1e-3), s = glm::clamp((t - g.t0) / T, 0.0, 1.0);
    if (g.kind == 2)
        return g.a;
    if (g.kind == 1)
    {
        const double u = s * s * s * (s * (6.0 * s - 15.0) + 10.0);
        return C.portPos + g.rArc * crewSlerpDir(g.a, g.b, u);
    }
    const double s2 = s * s, s3 = s2 * s;
    return (2.0 * s3 - 3.0 * s2 + 1.0) * g.a + (s3 - 2.0 * s2 + s) * T * g.va + (-2.0 * s3 + 3.0 * s2) * g.b +
           (s3 - s2) * T * g.vb;
}
// In the station's orbit plane (Us, As): the capsule from separation to its arrival behind the station (the far
// phases). Each phasing burn (2026-10-21) is spread over kCrewBurnS about its impulse time: the path before it and the
// Kepler path after it (both defined either side) cross-fade by a smoothstep, ~0.5-1.5 m/s^2 of thrust for the ~30-60
// m/s burns (the first cut changed the velocity in one step: ~80 m/s^2 in a 1-s second difference). The last burn,
// the circularisation at tArr, blends into the approach's first leg (or, the rendezvous called off, the circle it
// waits on). raw: without that last blend (where the approach starts).
static constexpr double kCrewBurnS = 240.0;
static glm::dvec3 crewPlanePt(const LCrew &C, double r, double th)
{
    return r * (std::cos(th) * C.Us + std::sin(th) * C.As);
}
// A Hohmann-type transfer ellipse up from ra (its periapsis, at angle thStart and time ts) to rb, at any t.
static glm::dvec3 crewTransfer(const LCrew &C, double ra, double rb, double thStart, double ts, double t)
{
    const double a = 0.5 * (ra + rb), e = std::abs(rb - ra) / std::max(ra + rb, 1.0);
    const double n = std::sqrt(launch::kGM / (a * a * a));
    const double M = n * (t - ts);
    double E = M;
    for (int i = 0; i < 12; ++i)
        E -= (E - e * std::sin(E) - M) / (1.0 - e * std::cos(E));
    const double r = a * (1.0 - e * std::cos(E));
    double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(0.5 * E), std::sqrt(1.0 - e) * std::cos(0.5 * E));
    nu += glm::two_pi<double>() * std::round((E - nu) / glm::two_pi<double>());
    return crewPlanePt(C, r, thStart + nu);
}
static glm::dvec3 crewFarPos(const LCrew &C, double t, bool raw = false)
{
    const double h = 0.5 * kCrewBurnS;
    auto piece = [&](int k, double tt) -> glm::dvec3 {
        switch (k)
        {
        case 0:
            return crewPlanePt(C, C.r0, C.th0 + (C.n0 + C.vSep / C.r0) * (tt - C.tSep)) +
                   C.fix0 * (1.0 - glm::smoothstep(C.tSep, C.tB1, tt));
        case 1:
            return crewTransfer(C, C.r0, C.rP, C.th1, C.tB1, tt);
        case 2:
            return crewPlanePt(C, C.rP, C.th2 + C.nP * (tt - C.tB1e));
        default:
            return crewTransfer(C, C.rP, C.rS, C.th3, C.tB2, tt);
        }
    };
    const double T[4] = {-1e300, C.tB1, C.tB1e, C.tB2};
    // The last transition that has fully happened, then the ones in progress.
    int k0 = 0;
    for (int k = 1; k < 4; ++k)
        if (T[k] < C.tArr - 1e-6 && t >= T[k] + h)
            k0 = k;
    glm::dvec3 P = piece(k0, t);
    for (int k = k0 + 1; k < 4; ++k)
        if (T[k] < C.tArr - 1e-6 && t > T[k] - h)
            P = glm::mix(P, piece(k, t), glm::smoothstep(T[k] - h, T[k] + h, t));
    if (raw || t <= C.tArr - h)
        return P;
    glm::dvec3 A;
    if (C.abort == 1 || C.segs.empty())
        A = crewPlanePt(C, C.rD, C.thDep + C.nD * (t - C.tDepEnd));
    else
    {
        glm::dvec3 Ps;
        glm::dmat3 R;
        crewStation(C, t, Ps, R);
        A = Ps + R * crewSegPos(C, C.segs[0], t);
    }
    return glm::mix(P, A, glm::smoothstep(C.tArr - h, C.tArr + h, t));
}
static glm::dvec3 crewReturnPos(const LCrew &C, double t)
{
    const double thD = C.thDep + C.nD * (t - C.tDepEnd);
    const glm::dvec3 circ = C.rD * (std::cos(thD) * C.Us + std::sin(thD) * C.As);
    if (t <= C.tDeorb)
        return circ;
    // The deorbit ellipse, apoapsis at the burn (as the upper stages' deorbitPos), blended in over the burn.
    const double M = glm::pi<double>() + C.nE * (t - C.tDeorb);
    double E = M;
    for (int i = 0; i < 10; ++i)
        E -= (E - C.eE * std::sin(E) - M) / (1.0 - C.eE * std::cos(E));
    const double r = C.aE * (1.0 - C.eE * std::cos(E));
    const double nu = 2.0 * std::atan2(std::sqrt(1.0 + C.eE) * std::sin(0.5 * E), std::sqrt(1.0 - C.eE) * std::cos(0.5 * E));
    const double dnu = nu - glm::pi<double>() + glm::two_pi<double>() * std::round((E - nu) / glm::two_pi<double>());
    const double th = C.thApo + dnu;
    const glm::dvec3 ell = r * (std::cos(th) * C.Us + std::sin(th) * C.As);
    const double s = glm::clamp((t - C.tDeorb) / std::max(C.deorbDur, 1.0), 0.0, 1.0);
    return glm::mix(circ, ell, s * s * (3.0 - 2.0 * s));
}
static constexpr double kCrewEntryS = 540.0;   // the entry interface (100 km) to the drogues (10 km)
// Its height through the entry (s = 0 .. 1): from 100 km falling as the deorbit ellipse does there (vrEI) to 10 km at
// rest vertically (cubic Hermite: no jump in the fall rate at the interface).
static double crewEntryH(const LCrew &C, double s)
{
    const double s2 = s * s, s3 = s2 * s;
    const double m0 = std::clamp(C.vrEI, -400.0, -20.0) * kCrewEntryS;
    return (2.0 * s3 - 3.0 * s2 + 1.0) * 100000.0 + (s3 - 2.0 * s2 + s) * m0 + (-2.0 * s3 + 3.0 * s2) * 10000.0;
}
static glm::dvec3 crewEntryPos(const LCrew &C, double t)
{
    const double te = glm::clamp(t - C.tEI, 0.0, kCrewEntryS), s = te / kCrewEntryS;
    const double x = C.vEI * kCrewEntryS * (1.0 - std::pow(1.0 - s, 2.5)) / 2.5 + 60.0 * te * s;
    const double h = crewEntryH(C, s);
    const double th = C.thEI + x / (kR + 100000.0); // (the speed at the interface: no jump)
    const glm::dvec3 P = (kR + h) * (std::cos(th) * C.Us + std::sin(th) * C.As);
    if (!C.rwDvOn)
        return P;
    // (2026-10-21c) A runway lander ends its entry flying on over the ground (rwDv, Earth-fixed: what its speed over
    // the ground at the entry's end lacks): T rwDv (s^3 - s^2) is 0 at both ends, its rate 0 at the interface and
    // rwDv at the end. The capsules' entry ends at ~120 m/s inertial, i.e. drifting west with the Earth's rotation.
    return ecefToEci(eciToEcef(P, t) + C.rwDv * (kCrewEntryS * (s * s * s - s * s)), t);
}
// Under parachutes, dt s after the drogues open at 10 km (the entry ends there falling ~0 m/s): the fall eases up to
// ~70 m/s, the mains (at 3 km over the ground, C.tMains) ease it down to ~6.5 m/s.
static constexpr double kCrewDriftTau = 25.0; // its speed over the ground fades out over this (s)
static double crewChuteH(const LCrew &C, double dt)
{
    const double tau1 = 8.0, tau2 = 5.0, vD = 70.0, vM = 6.5;
    const double dtM = C.tMains - C.tChute;
    if (dt < dtM)
        return 10000.0 - vD * (dt - tau1 * (1.0 - std::exp(-dt / tau1)));
    const double hM = 10000.0 - vD * (dtM - tau1 * (1.0 - std::exp(-dtM / tau1)));
    const double u = dt - dtM;
    return hM - (vM * u + (vD - vM) * tau2 * (1.0 - std::exp(-u / tau2)));
}
// A runway landing (2026-10-21c): Earth-fixed position after the entry. The glide is a cubic Hermite in the runway's
// tangent plane (from the entry's end, at its speed over the ground, to the touchdown point along the runway) and in
// height (10 km falling at 0 to the wheels at -1.5 m/s); the rollout decelerates evenly to rest.
static constexpr double kRunwayAxisH = 2.6; // the model's axis above the runway, on its wheels (m)
static glm::dvec3 crewRunwayEcef(const LCrew &C, double t, int *phase)
{
    glm::dvec2 p;
    double h;
    int ph;
    if (t < C.tTD)
    {
        const double s = glm::clamp((t - C.tGlide) / C.Tg, 0.0, 1.0), s2 = s * s, s3 = s2 * s;
        const double a0 = 2.0 * s3 - 3.0 * s2 + 1.0, b0 = s3 - 2.0 * s2 + s, a1 = -2.0 * s3 + 3.0 * s2, b1 = s3 - s2;
        p = a0 * C.p0 + b0 * C.Tg * C.v0 + a1 * C.pTD + b1 * C.Tg * (C.vTD * C.rwDir);
        h = a0 * C.h0 + a1 * kRunwayAxisH + b1 * C.Tg * -1.5;
        ph = t < C.tTD - 90.0 ? CP_GLIDE : CP_FINAL_GLIDE;
    }
    else
    {
        const double tau = std::min(t, C.tStop) - C.tTD, a = C.vTD * C.vTD / (2.0 * C.lRoll);
        p = C.pTD + (C.vTD * tau - 0.5 * a * tau * tau) * C.rwDir;
        h = kRunwayAxisH;
        ph = t < C.tStop ? CP_ROLLOUT : CP_DOWN;
    }
    if (phase)
        *phase = ph;
    const glm::dvec3 u = glm::normalize(kR * C.rwC + p.x * C.rwE + p.y * C.rwN);
    return u * (kR + C.rwElev + h);
}
// The capsule's position (ECI) at t, its phase and whether a burn (thrusters / the deorbit burn) is lit. False before
// separation and after it is gone.
static bool crewPos(const LCrew &C, double t, glm::dvec3 &P, int *phase = nullptr, uint8_t *burn = nullptr)
{
    if (!C.on || t < C.tSep || t >= C.tGone)
        return false;
    int ph = CP_NONE;
    bool lit = false;
    auto burning = [&](double tb, double dur) { return t >= tb && t < tb + dur; };
    if (t < C.tArr + 0.5 * kCrewBurnS)
    {
        // (the far phases, and the last burn's blend into the approach or the waiting circle)
        P = crewFarPos(C, t);
        ph = t < C.tB1 ? CP_SEPARATED : t < C.tB1e ? (C.skipRaise1 ? CP_PHASING : CP_RAISE1) : t < C.tB2 ? CP_PHASING
             : t < C.tArr ? CP_RAISE2 : (C.abort == 1 ? CP_ABORT : CP_FAR);
        const double hb = 0.5 * kCrewBurnS;
        lit = (!C.skipRaise1 && (burning(C.tB1 - hb, kCrewBurnS) || burning(C.tB1e - hb, kCrewBurnS))) ||
              (C.abort != 1 && (burning(C.tB2 - hb, kCrewBurnS) || burning(C.tArr - hb, kCrewBurnS)));
    }
    else if (t < C.tDepEnd)
    {
        glm::dvec3 Ps;
        glm::dmat3 R;
        crewStation(C, t, Ps, R);
        if (t >= C.tDock && t < C.tUndock)
        {
            P = Ps + R * C.portPos;
            ph = C.berth ? CP_BERTHED : CP_DOCKED;
        }
        else
        {
            const LCrewSeg *g = crewSegAt(C, t);
            if (!g)
                return false;
            P = Ps + R * crewSegPos(C, *g, t);
            ph = g->phase;
        }
        lit = burning(C.tArr - 30.0, 40.0) || burning(C.tDepBurn, 30.0);
    }
    else if (t < C.tEI)
    {
        P = crewReturnPos(C, t);
        ph = t < C.tDeorb ? ((C.abort == 1 || C.abort == 2) ? CP_ABORT : CP_DRIFT) : CP_DEORBIT;
        lit = burning(C.tDeorb, C.deorbDur);
    }
    else if (C.runway && t >= C.tGlide)
    {
        P = ecefToEci(crewRunwayEcef(C, t, &ph), t);
    }
    else if (t < C.tChute)
    {
        P = crewEntryPos(C, t);
        ph = CP_ENTRY;
    }
    else
    {
        // Under parachutes over an Earth-fixed point: its speed over the ground eased out, the fall easing up to ~70 m/s
        // under the drogues (to 3 km) and down to ~6.5 m/s under the mains, then on the sea / the ground.
        double h;
        if (t < C.tSplash)
        {
            h = crewChuteH(C, t - C.tChute);
            ph = t < C.tMains ? CP_DROGUES : CP_MAINS;
        }
        else
        {
            h = C.groundM + (C.lands ? 0.0 : -1.2); // afloat, the heat shield a metre under
            ph = CP_DOWN;
        }
        const double dt = std::min(t, C.tSplash) - C.tChute;
        const glm::dvec3 drift = C.chuteV * (kCrewDriftTau * (1.0 - std::exp(-dt / kCrewDriftTau)));
        P = ecefToEci(C.chuteEcef * (kR + h) + drift, t);
    }
    if (phase)
        *phase = ph;
    if (burn)
        *burn = lit ? launch::BURN_UPPER : launch::BURN_NONE;
    return true;
}
// Its model at t: 0 the whole spacecraft, 1 the crew / descent module alone, 2 under its main parachutes.
static int crewModelPart(const LCrew &C, double t)
{
    if (t < C.tCm)
        return 0;
    if (t >= C.tMains && t < C.tSplash + 90.0)
        return 2;
    return 1;
}
static glm::dmat3 crewFrame(const glm::dvec3 &Z0, const glm::dvec3 &yRef)
{
    const glm::dvec3 Z = glm::normalize(Z0);
    glm::dvec3 Y = yRef - glm::dot(yRef, Z) * Z;
    if (glm::length(Y) < 1e-6)
        Y = std::abs(Z.z) < 0.9 ? glm::dvec3(0, 0, 1) - Z.z * Z : glm::dvec3(1, 0, 0) - Z.x * Z;
    Y = glm::normalize(Y);
    return glm::dmat3(glm::cross(Y, Z), Y, Z);
}
// From attitude A to Bm by w in [0, 1]: a turn about the axis of Bm A^T. Near a half turn (a forward port's capsule
// turning prograde, prograde to retrograde for the deorbit burn) that axis's sign is fixed by `ref` (the local up),
// or the shortest way would flip from one side to the other with the frames' smallest change (178 deg/s, `launch check`).
static glm::dmat3 crewSlerp(const glm::dmat3 &A, const glm::dmat3 &Bm, double w, const glm::dvec3 &ref)
{
    w = glm::clamp(w, 0.0, 1.0);
    glm::dquat q = glm::normalize(glm::quat_cast(Bm * glm::transpose(A)));
    if (q.w < 0.0)
        q = -q;
    const double ang = 2.0 * std::acos(glm::clamp(q.w, -1.0, 1.0));
    const double s = std::sqrt(std::max(0.0, 1.0 - q.w * q.w));
    if (s < 1e-9)
        return A;
    glm::dvec3 ax = glm::dvec3(q.x, q.y, q.z) / s;
    double th = ang;
    if (ang > glm::radians(150.0) && glm::dot(ax, ref) < 0.0)
    {
        ax = -ax;
        th = glm::two_pi<double>() - ang; // the same end, about the referenced side (a hair the long way at 180 deg)
    }
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), w * th, ax)) * A;
}
// Its attitude at t (body -> ECI: +Z out of the docking port, the heat shield at -Z). Prograde in the free-flying
// phases (as it sat on its stage: +Z the nose, -Y down), its port toward the station's port on the approach and
// docked, the heat shield into the flow from the deorbit burn on, upright under the parachutes.
static glm::dmat3 crewAttitude(const LCrew &C, double t)
{
    if (C.runway && t > C.tStop - 2.0 && t < C.tGone)
        t = C.tStop - 2.0; // (at rest on the runway: as it rolled to a stop)
    glm::dvec3 P, P0, P1;
    if (!crewPos(C, t, P))
        return glm::dmat3(1.0);
    auto velDir = [&](double tt) {
        glm::dvec3 a, b;
        if (!crewPos(C, tt + 0.5, a))
            a = P;
        if (!crewPos(C, tt - 0.5, b))
            b = P;
        const glm::dvec3 v = a - b;
        return glm::length(v) > 1e-6 ? glm::normalize(v) : glm::normalize(glm::cross(C.Ns, P));
    };
    auto prograde = [&](double tt) {
        glm::dvec3 Q;
        crewPos(C, tt, Q);
        return crewFrame(velDir(tt), glm::normalize(Q));
    };
    auto stationFrame = [&](double tt, int att) {
        glm::dvec3 Ps, Q;
        glm::dmat3 R;
        crewStation(C, tt, Ps, R);
        crewPos(C, tt, Q);
        glm::dvec3 Z = -(R * C.portAx);
        if (att == 0)
        {
            const glm::dvec3 d = Ps + R * C.portPos - Q;
            if (glm::length(d) > 1.0)
                Z = glm::normalize(d);
        }
        // Its "up" (-Y down) follows the station's zenith, or its velocity axis for a zenith / nadir port.
        const double k = glm::smoothstep(0.7, 0.95, std::abs(glm::dot(Z, R[2])));
        return crewFrame(Z, glm::normalize(glm::mix(-R[2], R[0], k)));
    };
    if (t < C.tArr)
        return prograde(t);
    if (t < C.tDepEnd)
    {
        if (t >= C.tDock && t < C.tUndock)
            return stationFrame(t, 1);
        const LCrewSeg *g = crewSegAt(C, t);
        if (!g)
            return prograde(t);
        if (g->att == 2)
        {
            const double w = glm::smoothstep(g->t0 + 60.0, g->t0 + 900.0, t);
            return crewSlerp(stationFrame(t, g->attFrom), prograde(t), w, glm::normalize(P));
        }
        // (2026-10-21) Round onto the docking axis (the arc about the port): the frame facing the port is carried round
        // the arc (rotated with the direction, about the arc's axis), not rebuilt from an "up" reference each moment:
        // that reference passed through the facing direction on some arcs (a 180-deg roll in a second). Its roll then
        // eases into the docking axis's own frame over the first minutes of the next leg.
        auto rotTo = [](const glm::dvec3 &p, const glm::dvec3 &q, double w) {
            const glm::dvec3 ax = glm::cross(p, q);
            const double sn = glm::length(ax);
            if (sn < 1e-9)
                return glm::dmat3(1.0);
            const double ang = std::atan2(sn, glm::dot(p, q));
            return glm::dmat3(glm::rotate(glm::dmat4(1.0), w * ang, ax / sn));
        };
        auto formulaB = [](const glm::dvec3 &Z) {
            const double k = glm::smoothstep(0.7, 0.95, std::abs(Z.z));
            return crewFrame(Z, glm::normalize(glm::mix(glm::dvec3(0, 0, -1), glm::dvec3(1, 0, 0), k)));
        };
        auto arcFrameB = [&](const LCrewSeg &q, double tt) {
            const double T = std::max(q.t1 - q.t0, 1e-3), sa = glm::clamp((tt - q.t0) / T, 0.0, 1.0);
            const double u = sa * sa * sa * (sa * (6.0 * sa - 15.0) + 10.0);
            const glm::dmat3 F0 = formulaB(-q.a);
            if (glm::dot(q.a, q.b) > -0.85)
                return glm::dmat3(rotTo(q.a, q.b, u) * F0);
            // (crewSlerpDir's way round: through m, below the station)
            glm::dvec3 m = q.a + q.b;
            if (glm::length(m) < 0.5)
            {
                m = glm::dvec3(0, 0, 1) - glm::dot(glm::dvec3(0, 0, 1), q.a) * q.a;
                if (glm::length(m) < 1e-3)
                    m = glm::dvec3(1, 0, 0) - glm::dot(glm::dvec3(1, 0, 0), q.a) * q.a;
            }
            m = glm::normalize(m);
            return u < 0.5 ? glm::dmat3(rotTo(q.a, m, 2.0 * u) * F0)
                           : glm::dmat3(rotTo(m, q.b, 2.0 * u - 1.0) * rotTo(q.a, m, 1.0) * F0);
        };
        const LCrewSeg *arc = nullptr;
        for (const LCrewSeg &q : C.segs)
            if (q.kind == 1)
            {
                arc = &q;
                break;
            }
        glm::dvec3 PsA;
        glm::dmat3 RA;
        crewStation(C, t, PsA, RA);
        if (arc && g == arc && g->att == 0)
        {
            const glm::dmat3 Fa = RA * arcFrameB(*arc, t);
            if (t < C.tArr + 300.0)
                return crewSlerp(prograde(t), Fa, glm::smoothstep(C.tArr, C.tArr + 300.0, t), glm::normalize(P));
            return Fa;
        }
        if (arc && g->att == 1 && t >= arc->t1 && t < arc->t1 + 300.0)
            return crewSlerp(RA * arcFrameB(*arc, arc->t1), stationFrame(t, 1), glm::smoothstep(arc->t1, arc->t1 + 300.0, t),
                             glm::normalize(P));
        // Turning from prograde to face the port over the approach's first 5 min.
        const glm::dmat3 F = stationFrame(t, g->att);
        if (t < C.tArr + 300.0)
            return crewSlerp(prograde(t), F, glm::smoothstep(C.tArr, C.tArr + 300.0, t), glm::normalize(P));
        return F;
    }
    if (t < C.tChute)
    {
        // Prograde while drifting; turned heat shield forward (+Z retrograde) over the 10 min before the burn.
        const glm::dmat3 pro = prograde(t);
        const glm::dmat3 retro = crewFrame(-velDir(t), glm::normalize(P));
        glm::dmat3 R = crewSlerp(pro, retro, glm::smoothstep(C.tDeorb - 900.0, C.tDeorb - 300.0, t), glm::normalize(P));
        if (C.runway)
        {
            // (2026-10-21) A spaceplane enters NOSE-HIGH, its tiled belly into the flow (Dream Chaser, like the
            // Shuttle, ~40 deg angle of attack): pitched up about its wing axis (body X) from 10 min before the
            // entry interface, held through the entry, and lowered over the last 3 min of it into the glide.
            const double aoa = glm::radians(40.0) * glm::smoothstep(C.tEI - 600.0, C.tEI - 120.0, t) *
                               (1.0 - glm::smoothstep(C.tGlide - 180.0, C.tGlide, t));
            if (aoa > 1e-6)
            {
                const double ca = std::cos(aoa), sa = std::sin(aoa);
                const glm::dvec3 Y = R[1], Z = R[2];
                R[1] = ca * Y + sa * Z; // (the nose, -Z, turns toward +Y: up)
                R[2] = ca * Z - sa * Y;
            }
        }
        return R;
    }
    const glm::dmat3 up = crewFrame(glm::normalize(P), glm::cross(C.Ns, glm::normalize(P)));
    if (t < C.tChute + 60.0)
        return crewSlerp(crewFrame(-velDir(C.tChute - 1.0), glm::normalize(P)), up, (t - C.tChute) / 60.0, glm::normalize(P));
    return up;
}

static bool buildCrew(const SatelliteSim &sim, SatelliteSim::LaunchState &L, LBatch &B, const LPlane &pl, double lifeS)
{
    LCrew &C = B.crew;
    C = LCrew{};
    const launch::Program &pg = L.cat.programs[B.prog];
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    if (!pg.crew || pg.crewModel.empty() || pl.sats.empty() || pl.sats[0] >= sim.activeSatCount)
        return true;
    C.sat = pl.sats[0];
    C.st = sim.rosterElemsOf(sim.satOrbits[C.sat]);
    C.fast = B.crewFast;
    C.cargo = pg.cargo;
    C.burns = pg.burnsUp;
    C.berth = pg.berth;
    C.capM = pg.captureM;
    C.massKg = pg.payloadMassKg > 0.0 ? pg.payloadMassKg : (pg.cargo ? 7000.0 : 10000.0);
    // The station's loss (2026-10-21): its dead / end second from the event log (the roster words mirrored from the
    // Kessler world; crewSync rebuilds the batch when they change).
    if (sim.kesslerEnabled && 2ull * C.sat + 1 < sim.kRosterWords.size())
    {
        const int32_t d = std::min(sim.kRosterWords[2ull * C.sat], sim.kRosterWords[2ull * C.sat + 1]);
        if (d != kessler::kNever)
            C.tDeath = sim.kesslerEpoch() + (double)d;
    }
    // Its port: the station model's docking ports of its class ("crew" / "cargo"). Crews alternate between two (a
    // hand-over); a program sharing the crews' ports (Cargo Dragon) takes the one the current crew is not on.
    {
        const uint32_t ti = sim.satOrbits[C.sat].typeIdx;
        const SatModel *m = ti < sim.satTypes.size() ? sim.satTypes[ti].model.get() : nullptr;
        std::vector<int> ports;
        if (m)
            for (size_t q = 0; q < m->dockPorts.size(); ++q)
                if (m->dockPorts[q].use == pg.portUse)
                    ports.push_back((int)q);
        if (!ports.empty())
        {
            // A station built module by module (2026-10-21, the era on): only ports on modules there at docking (a
            // crew falls back to every port of its class when none is yet; a cargo ship does not fly then).
            if (L.deployOn)
            {
                std::vector<int> ok;
                for (int q : ports)
                    if (m->dockPorts[(size_t)q].sinceDays * 86400.0 <= B.tA)
                        ok.push_back(q);
                if (!ok.empty())
                    ports = ok;
                else if (pg.cargo)
                    return false;
            }
            C.nPorts = (int)ports.size();
            int k = (int)(((B.k % C.nPorts) + C.nPorts) % C.nPorts);
            if (pg.sharePorts && pl.crewPlane >= 0)
            {
                if (C.nPorts < 2)
                    return false; // nothing free between the crews
                const LPlane &cp = L.planes[(size_t)pl.crewPlane];
                const double lifeC = progLifeS(L.cat.programs[cp.prog], (double)sim.launchLifeScale);
                const int64_t kc = (int64_t)std::floor(arrivalOf(pl, B.k, B.b, lifeS) / lifeC - cp.phase);
                k = (int)(((kc + 1) % C.nPorts + C.nPorts) % C.nPorts);
            }
            const SatModel::DockPort &dp = m->dockPorts[(size_t)ports[(size_t)k]];
            C.port = ports[(size_t)k];
            C.portPos = dp.pos;
            C.portAx = dp.axis;
            // The model's port id ("ida2_harmony_fwd") as a player reads it ("IDA-2 Harmony forward"): it shows in the
            // follow bar, the Missions window and the milestones (2026-10-21 QA).
            C.portName.clear();
            {
                std::string tok;
                auto flush = [&]() {
                    if (tok.empty())
                        return;
                    std::string w = tok;
                    if (w == "fwd")
                        w = "forward";
                    else if (w == "stbd")
                        w = "starboard";
                    else if (w.size() == 4 && (w.compare(0, 3, "ida") == 0 || w.compare(0, 3, "pma") == 0) && isdigit((unsigned char)w[3]))
                        w = std::string(1, (char)toupper(w[0])) + (char)toupper(w[1]) + (char)toupper(w[2]) + "-" + w[3];
                    else if (w == "harmony" || w == "zvezda" || w == "tianhe" || w == "nauka" || w == "unity" || w == "destiny")
                        w[0] = (char)toupper(w[0]);
                    C.portName += (C.portName.empty() ? "" : " ") + w;
                    tok.clear();
                };
                for (char ch : dp.name)
                    if (ch == '_')
                        flush();
                    else
                        tok += ch;
                flush();
            }
        }
        else if (pg.portUse != "crew")
        {
            static bool logged = false;
            if (!logged)
                Log::line(std::string("crew: station type '") + (ti < sim.satTypes.size() ? sim.satTypes[ti].name : "?") +
                          "' has no '" + pg.portUse + "' docking port: " + pg.spacecraft + " does not fly there");
            logged = true;
            return false;
        }
        else
        {
            C.nPorts = 1;
            C.portName = "forward port";
            static bool logged = false;
            if (!logged)
                Log::line(std::string("crew: station type '") + (ti < sim.satTypes.size() ? sim.satTypes[ti].name : "?") +
                          "' has no docking ports (model " + (m ? "loaded" : "missing") + ")");
            logged = true;
        }
    }
    // Separation: the spacecraft on top of its stage (vh.spacecraftDockM up the stage's +Z), which flies prograde.
    C.tSep = B.tD;
    glm::dvec3 Pst;
    if (!rocketPos(L, B, 1, C.tSep - B.tL, Pst))
        return true;
    const glm::dvec3 Pc0 = Pst + upperFrame(L, B, C.tSep - B.tL) * glm::dvec3(0.0, 0.0, vh.spacecraftDockM);
    {
        const SatOrbitState s0 = satOrbitStateAt(C.st, C.tSep);
        C.rS = glm::length(s0.posEci);
        C.Us = s0.posEci / C.rS;
        C.Ns = glm::normalize(glm::cross(s0.posEci, s0.velEci));
        C.As = glm::cross(C.Ns, C.Us);
        C.nS = C.st.meanMot > 0.0 ? C.st.meanMot : launch::meanMotion(C.rS);
    }
    C.r0 = B.rPark;
    C.n0 = launch::meanMotion(C.r0);
    C.th0 = std::atan2(glm::dot(Pc0, C.As), glm::dot(Pc0, C.Us));
    C.fix0 = Pc0 - C.r0 * (std::cos(C.th0) * C.Us + std::sin(C.th0) * C.As);
    // Phasing: wait in a phasing orbit rP (between the parking orbit and 8 km under the station) for as long as it
    // takes the station's lead (lag0, + whole turns) to close to dFar, so that the arrival comes as near the
    // program's raise_days (a fast track: the candidate's docking time) as the geometry lets it.
    const double dFar = 5000.0, lamF = dFar / C.rS;
    CrewPhaseGeom pgm;
    pgm.r0 = C.r0;
    pgm.n0 = C.n0;
    pgm.rS = C.rS;
    pgm.nS = C.nS;
    pgm.c1 = crewC1(B.seed, C.fast);
    pgm.vs = C.vSep / C.r0;
    pgm.rPmax = std::max(C.r0, C.rS - 8000.0);
    const double c1 = pgm.c1, rPmax = pgm.rPmax, vs = pgm.vs;
    auto catchup = [&](double rP, double D, double *Wout) { return crewCatchup(pgm, rP, D, Wout); };
    // The far phases' time (separation to arrival 5 km behind), from the arrival less the approach.
    const double kApproachS = crewApproachS(C.fast, C.berth);
    double D = std::max(B.tA - C.tSep - kApproachS,
                        c1 + crewHohmannT(C.r0, C.rS) * 2.0 + (C.fast ? 60.0 : 1200.0));
    double lag = 0.0 - C.th0 - lamF;
    lag = lag - glm::two_pi<double>() * std::floor(lag / glm::two_pi<double>());
    while (lag < catchup(rPmax, D, nullptr))
        lag += glm::two_pi<double>();
    double rP = C.r0;
    const double cMax = catchup(C.r0, D, nullptr);
    if (lag > cMax)
        D += (lag - cMax) / std::max(C.n0 - C.nS, 1e-9); // wait longer in the parking orbit
    else
    {
        double lo = C.r0, hi = rPmax;
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (catchup(mid, D, nullptr) > lag ? lo : hi) = mid;
        }
        rP = 0.5 * (lo + hi);
    }
    C.rP = rP;
    C.nP = launch::meanMotion(rP);
    C.skipRaise1 = rP - C.r0 < 500.0;
    double W = 0.0;
    catchup(rP, D, &W);
    W = std::max(W, 0.0);
    C.tB1 = C.tSep + c1;
    C.tB1e = C.tB1 + crewHohmannT(C.r0, C.rP);
    C.tB2 = C.tB1e + W;
    C.tArr = C.tB2 + crewHohmannT(C.rP, C.rS);
    C.th1 = C.th0 + (C.n0 + vs) * c1;
    C.th2 = C.th1 + glm::pi<double>();
    C.th3 = C.th2 + C.nP * W;
    // The approach, in the station's frame: from where the far phases leave it (exactly: no jump) to 1 km out on the
    // line toward the port, round onto the port's axis at 1 km, then along it with holds at 400, 200 and 20 m.
    C.on = true; // (crewPos is used below)
    glm::dvec3 Ps, Pc;
    glm::dmat3 R;
    crewStation(C, C.tArr, Ps, R);
    Pc = crewFarPos(C, C.tArr, true);
    const glm::dvec3 W0 = glm::transpose(R) * (Pc - Ps);
    const glm::dvec3 ax = glm::normalize(C.portAx), pp = C.portPos;
    glm::dvec3 d0 = W0 - pp;
    d0 = glm::length(d0) > 1.0 ? glm::normalize(d0) : -ax;
    double t = C.tArr;
    auto seg = [&](int kind, glm::dvec3 a, glm::dvec3 b, double dur, int phase, int att) {
        LCrewSeg g;
        g.t0 = t;
        g.t1 = t + dur;
        g.kind = kind;
        g.a = a;
        g.b = b;
        g.phase = phase;
        g.att = att;
        g.rArc = 1000.0;
        C.segs.push_back(g);
        t += dur;
    };
    const double *lg = kCrewApproachLegs[C.fast ? 1 : 0];
    seg(0, W0, pp + 1000.0 * d0, lg[0], CP_FAR, 0);
    seg(1, d0, ax, lg[1], CP_TO_AXIS, 0);
    C.tAxis = t;
    if (C.berth)
    {
        // (2026-10-21) Berthing: up the R-bar (a nadir port) with holds at 250 m and 30 m to the capture point capM
        // out, free drift there while the arm grapples it, then on the arm onto the port (the last 1.5 m: the CBM's
        // bolts). Every leg a smootherstep / Hermite with stops at its ends (no jump at a seam). Legs in s.
        const glm::dvec3 cap = pp + C.capM * ax, pre = pp + 1.5 * ax;
        seg(0, pp + 1000.0 * ax, pp + 250.0 * ax, kBerthLegs[0], CP_TO_250, 1);
        C.hold[0] = t;
        seg(2, pp + 250.0 * ax, pp + 250.0 * ax, kBerthLegs[1], CP_HOLD_250, 1);
        seg(0, pp + 250.0 * ax, pp + 30.0 * ax, kBerthLegs[2], CP_TO_30, 1);
        C.hold[1] = t;
        seg(2, pp + 30.0 * ax, pp + 30.0 * ax, kBerthLegs[3], CP_HOLD_30, 1);
        seg(0, pp + 30.0 * ax, cap, kBerthLegs[4], CP_TO_CAPTURE, 1);
        C.hold[2] = C.tCapArr = t;
        C.tGrapple = t + kBerthGrappleS;
        seg(2, cap, cap, kBerthLegs[5], CP_CAPTURE, 1);
        C.tBerth0 = t;
        seg(0, cap, pre, kBerthLegs[6], CP_ARM_BERTH, 1);
        seg(0, pre, pp, kBerthLegs[7], CP_ARM_INSTALL, 1);
        // The arm leaves its park ~kArmMoveS before the capture point is reached, and is back after the bolts drive.
        C.armA0 = C.tCapArr - kArmMoveS;
        C.armA2 = t + kArmBoltS;
        C.armA3 = C.armA2 + kArmOpenS + kArmMoveS;
        // Its grapple fixture: on its side toward the arm's shoulder (its attitude docked: +Z into the port), gR out,
        // gZ below the port.
        {
            glm::dvec3 toS = (L.armOf(C.sat) ? L.armOf(C.sat)->base : glm::dvec3(0.0)) - pp;
            toS -= glm::dot(toS, ax) * ax;
            if (glm::length(toS) < 1e-3)
                toS = glm::dvec3(-1.0, 0.0, 0.0) - glm::dot(glm::dvec3(-1.0, 0.0, 0.0), ax) * ax;
            toS = glm::normalize(toS);
            // The body frame docked in the station frame (crewAttitude's stationFrame, att 1, with R = identity).
            const glm::dvec3 Zb = -ax;
            const double kz = glm::smoothstep(0.7, 0.95, std::abs(Zb.z));
            const glm::dmat3 F = crewFrame(Zb, glm::normalize(glm::mix(glm::dvec3(0, 0, -1), glm::dvec3(1, 0, 0), kz)));
            C.gfN = glm::transpose(F) * toS;
            C.gfB = pg.grappleR * C.gfN + glm::dvec3(0.0, 0.0, -pg.grappleZ);
        }
    }
    else
    {
    seg(0, pp + 1000.0 * ax, pp + 400.0 * ax, lg[2], CP_TO_400, 1);
    C.hold[0] = t;
    seg(2, pp + 400.0 * ax, pp + 400.0 * ax, lg[3], CP_HOLD_400, 1);
    seg(0, pp + 400.0 * ax, pp + 200.0 * ax, lg[4], CP_TO_200, 1);
    C.hold[1] = t;
    seg(2, pp + 200.0 * ax, pp + 200.0 * ax, lg[5], CP_HOLD_200, 1);
    seg(0, pp + 200.0 * ax, pp + 20.0 * ax, lg[6], CP_TO_20, 1);
    C.hold[2] = t;
    seg(2, pp + 20.0 * ax, pp + 20.0 * ax, lg[7], CP_HOLD_20, 1);
    seg(0, pp + 20.0 * ax, pp, lg[8], CP_FINAL, 1);
    }
    C.tDock = t;
    B.tA = C.tDock;
    // Docked until the next visit of the same program has arrived (two ports: a hand-over of a few days), or a little
    // before it comes (one port); a program's stay_days caps it; one sharing the crews' ports leaves before the next
    // crew needs its port. The next visit's nominal arrival is the plane's next refresh.
    const double tNext = arrivalOf(pl, B.k + 1, B.b, lifeS);
    C.tUndock = C.nPorts >= 2 && !pg.sharePorts ? tNext + (5.0 + 2.0 * h01(B.seed, 41, 1)) * kDay
                                                : tNext - (2.0 + h01(B.seed, 41, 1)) * kDay;
    if (pg.stayDays[0] > 0.0)
        C.tUndock = std::min(C.tUndock,
                             C.tDock + (pg.stayDays[0] + (pg.stayDays[1] - pg.stayDays[0]) * h01(B.seed, 43, 1)) * kDay);
    if (pg.sharePorts && pl.crewPlane >= 0)
    {
        const LPlane &cp = L.planes[(size_t)pl.crewPlane];
        const double lifeC = progLifeS(L.cat.programs[cp.prog], (double)sim.launchLifeScale);
        const int64_t kc = (int64_t)std::floor(arrivalOf(pl, B.k, B.b, lifeS) / lifeC - cp.phase);
        C.tUndock = std::min(C.tUndock, arrivalOf(cp, kc + 1, 0, lifeC) - 3.0 * kDay);
    }
    C.tUndock = std::max(C.tUndock, C.tDock + 2.0 * kDay);
    C.tDepBurn = C.tUndock + 900.0;
    // The departure: from (posFrom, velFrom: station frame) at tFrom onto a circle 2 km under the station and 3 km
    // ahead of it (attFrom: the attitude it turns to prograde from).
    auto depart = [&](const glm::dvec3 &posFrom, const glm::dvec3 &velFrom, double tFrom, double dur, int attFrom) {
        C.tDepEnd = tFrom + dur;
        C.rD = C.rS - 2000.0;
        C.nD = launch::meanMotion(C.rD);
        glm::dvec3 Ps2;
        glm::dmat3 R2;
        crewStation(C, C.tDepEnd, Ps2, R2);
        C.thDep = std::atan2(glm::dot(Ps2, C.As), glm::dot(Ps2, C.Us)) + 3000.0 / C.rS;
        auto relAt = [&](double tt) {
            glm::dvec3 Pa;
            glm::dmat3 Ra;
            crewStation(C, tt, Pa, Ra);
            const double th = C.thDep + C.nD * (tt - C.tDepEnd);
            return glm::dvec3(glm::transpose(Ra) * (C.rD * (std::cos(th) * C.Us + std::sin(th) * C.As) - Pa));
        };
        const glm::dvec3 E = relAt(C.tDepEnd), vE = 0.5 * (relAt(C.tDepEnd + 1.0) - relAt(C.tDepEnd - 1.0));
        LCrewSeg g;
        g.t0 = tFrom;
        g.t1 = C.tDepEnd;
        g.kind = 0;
        g.a = posFrom;
        g.va = velFrom;
        g.b = E;
        g.vb = vE;
        g.phase = CP_DEPART;
        g.att = 2;
        g.attFrom = attFrom;
        C.segs.push_back(g);
    };
    bool destroyed = false;
    if (C.tDeath < C.tArr)
    {
        // The station is lost before the spacecraft gets there: the rendezvous is called off. It finishes the coast or
        // transfer it is in, stays on the circle that leaves it on and comes home from there (crewReturnPos).
        C.abort = 1;
        double tc, rc, thc, nc;
        if (C.tDeath < C.tB1)
        {
            tc = C.tB1;
            rc = C.r0;
            thc = C.th1;
            nc = C.n0 + vs;
        }
        else if (C.tDeath < C.tB1e)
        {
            tc = C.tB1e;
            rc = C.rP;
            thc = C.th2;
            nc = C.nP;
        }
        else if (C.tDeath < C.tB2)
        {
            tc = C.tDeath;
            rc = C.rP;
            thc = C.th2 + C.nP * (C.tDeath - C.tB1e);
            nc = C.nP;
        }
        else
        {
            tc = C.tArr;
            rc = C.rS;
            thc = C.th3 + glm::pi<double>();
            nc = launch::meanMotion(C.rS);
        }
        C.segs.clear();
        C.tArr = C.tDock = C.tUndock = C.tDepEnd = tc;
        C.rD = rc;
        C.nD = nc;
        C.thDep = thc;
        C.tAxis = C.hold[0] = C.hold[1] = C.hold[2] = tc;
        B.tA = tc;
    }
    else if (C.tDeath < C.tDock)
    {
        // Lost during the approach: it turns away from where it is, onto the departure circle.
        C.abort = 2;
        const double tc = C.tDeath;
        size_t gi = C.segs.size() - 1;
        for (size_t q = 0; q < C.segs.size(); ++q)
            if (tc < C.segs[q].t1)
            {
                gi = q;
                break;
            }
        LCrewSeg &g = C.segs[gi];
        const glm::dvec3 Wc = crewSegPos(C, g, tc);
        const glm::dvec3 Wv = 0.5 * (crewSegPos(C, g, tc + 1.0) - crewSegPos(C, g, tc - 1.0));
        const int attNow = g.att;
        g.tCut = tc;
        C.segs.erase(C.segs.begin() + (std::ptrdiff_t)gi + 1, C.segs.end());
        C.tDock = C.tUndock = tc;
        B.tA = tc;
        depart(Wc, Wv, tc, 2400.0, attNow);
    }
    else if (C.tDeath < C.tUndock && C.cargo)
    {
        // A cargo ship docked to a station that breaks up goes with it.
        C.abort = 4;
        C.tUndock = C.tDeath;
        C.tGone = C.tDeath;
        destroyed = true;
    }
    else if (C.tDeath < C.tUndock)
    {
        // A crew docked when the station is lost undocks at once (they shelter in their spacecraft) and backs off fast.
        C.abort = 3;
        C.tUndock = C.tDeath + 90.0;
        C.tDepBurn = C.tUndock + 300.0;
        t = C.tUndock;
        seg(0, pp, pp + 20.0 * ax, 60.0, CP_ESCAPE, 1);
        seg(0, pp + 20.0 * ax, pp + 200.0 * ax, 240.0, CP_ESCAPE, 1);
        depart(pp + 200.0 * ax, glm::dvec3(0.0), t, 1500.0, 1);
    }
    else if (C.berth)
    {
        // (2026-10-21) Unberthing: the arm takes it (from armD0), lifts it off the port (bolts released) and moves it
        // to the release point, holds, opens (tRelease); it drifts off, a departure burn, then the usual departure.
        t = C.tUndock;
        const glm::dvec3 rel = pp + C.capM * ax, pre = pp + 1.5 * ax;
        seg(0, pp, pre, kBerthLegs[7], CP_UNBERTH, 1);
        seg(0, pre, rel, kBerthLegs[6], CP_UNBERTH, 1);
        C.tRelease = t + kBerthGrappleS;
        seg(2, rel, rel, kBerthReleaseHoldS, CP_RELEASE, 1);
        C.armD0 = C.tUndock - kArmMoveS - kArmOpenS;
        C.armD3 = C.tRelease + kArmOpenS + kArmMoveS;
        C.tDepBurn = t + 120.0;
        depart(rel, glm::dvec3(0.0), t, 2400.0, 1);
    }
    else
    {
        // Departure: back out along the axis, then away below and ahead onto a circle 2 km under the station, 3 km ahead.
        t = C.tUndock;
        seg(0, pp, pp + 20.0 * ax, 300.0, CP_BACKOUT, 1);
        seg(0, pp + 20.0 * ax, pp + 200.0 * ax, 600.0, CP_BACKOUT, 1);
        depart(pp + 200.0 * ax, glm::dvec3(0.0), t, 2400.0, 1);
    }
    if (destroyed)
    {
        C.tDepEnd = C.tDeorb = C.tEI = C.tCm = C.tChute = C.tMains = C.tSplash = C.tDeath;
        return true;
    }
    // The deorbit: a few hours later, a ~10 min burn onto an ellipse with its perigee 35 km up; the entry interface
    // (100 km), the trunk / modules gone 15 min before it.
    C.deorbDur = 600.0;
    // The burn is timed (from 3 h after the departure, every 12 min for a day) to bring it down at sea (a splashdown:
    // Dragon, Gaganyaan) or on land (Soyuz, Shenzhou), as the recovery forces plan it; a cargo ship that burns up
    // (Progress, Tianzhou) is aimed at the open ocean.
    auto planReturn = [&](double tDeorb) {
        C.tDeorb = tDeorb;
        const double ra = C.rD, rp = kR + 35000.0;
        C.aE = 0.5 * (ra + rp);
        C.eE = (ra - rp) / (ra + rp);
        C.nE = launch::meanMotion(C.aE);
        C.thApo = C.thDep + C.nD * (C.tDeorb - C.tDepEnd);
        const double rEI = kR + 100000.0;
        const double cE = std::clamp((1.0 - rEI / C.aE) / std::max(C.eE, 1e-6), -1.0, 1.0);
        const double E = glm::two_pi<double>() - std::acos(cE);
        const double M = E - C.eE * std::sin(E);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + C.eE) * std::sin(0.5 * E), std::sqrt(1.0 - C.eE) * std::cos(0.5 * E));
        C.thEI = C.thApo + (nu < 0.0 ? nu + glm::two_pi<double>() : nu) - glm::pi<double>();
        C.vEI = std::sqrt(launch::kGM * (2.0 / rEI - 1.0 / C.aE));
        C.vrEI = std::sqrt(launch::kGM / (C.aE * (1.0 - C.eE * C.eE))) * C.eE * std::sin(nu); // < 0: falling
        C.tEI = C.tDeorb + (M - glm::pi<double>()) / C.nE;
        // (the ellipse is blended in over the burn: the entry starts where the blend has ended)
        C.tEI = std::max(C.tEI, C.tDeorb + C.deorbDur + 60.0);
        C.tCm = C.tEI - 900.0;
        C.tChute = C.tEI + kCrewEntryS;
        const glm::dvec3 Pch = crewEntryPos(C, C.tChute);
        C.chuteEcef = glm::normalize(eciToEcef(Pch, C.tChute));
        const double la = glm::degrees(std::asin(glm::clamp(C.chuteEcef.z, -1.0, 1.0)));
        const double lo = glm::degrees(std::atan2(C.chuteEcef.y, C.chuteEcef.x));
        return shoreDistCpu(la, lo); // > 0 at sea
    };
    if (pg.runway && !C.burns)
    {
        // (2026-10-21c) A runway landing (Dream Chaser at the KSC Shuttle Landing Facility): the burn is timed (every
        // 30 s from 3 h after the departure, for six days) so the entry ends 60-160 km short of the runway, then a
        // glide to it (crewRunwayEcef). A pure function of the batch, like everything here.
        C.runway = true;
        const double la = glm::radians(pg.runwayLat), lo = glm::radians(pg.runwayLon);
        C.rwC = glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
        C.rwE = glm::normalize(glm::dvec3(-std::sin(lo), std::cos(lo), 0.0));
        C.rwN = glm::cross(C.rwC, C.rwE);
        C.rwElev = pg.runwayElevM;
        const double hd = glm::radians(pg.runwayHdgDeg);
        const glm::dvec2 D(std::sin(hd), std::cos(hd));
        auto plane = [&](const glm::dvec3 &u) {
            const double c = std::max(glm::dot(u, C.rwC), 1e-3);
            return glm::dvec2(kR * glm::dot(u, C.rwE) / c, kR * glm::dot(u, C.rwN) / c);
        };
        const double t0r = C.tDepEnd + 3.0 * 3600.0;
        double tBestR = t0r, bestErr = 1e300;
        bool found = false;
        // err of a burn at tc: the entry's end 110 km short of the runway, heading for it (its speed over the ground
        // pointing within 75 deg of it); far ones are skipped cheaply.
        auto evalAt = [&](double tc, bool &hit) {
            hit = false;
            planReturn(tc);
            const double d = kR * std::acos(glm::clamp(glm::dot(C.chuteEcef, C.rwC), -1.0, 1.0));
            if (d > 1.0e6)
                return 1e300;
            const glm::dvec3 Pi1 = crewEntryPos(C, C.tChute), Pi0 = crewEntryPos(C, C.tChute - 1.0);
            const glm::dvec3 E1 = eciToEcef(Pi1, C.tChute);
            const glm::dvec2 q1 = plane(glm::normalize(E1));
            // the direction it flies on over the ground: its inertial track, turned into the Earth's frame
            const glm::dvec2 v = plane(glm::normalize(E1 + eciToEcef(Pi1 - Pi0, C.tChute) * 1000.0)) - q1;
            const bool toward = glm::length(v) > 1.0 && glm::dot(v, -q1) > 0.25 * glm::length(v) * glm::length(q1);
            hit = toward && d > 60000.0 && d < 160000.0;
            return std::abs(d - 110000.0) + (toward ? 0.0 : 400000.0);
        };
        // Coarse steps of 30 s (the entry's end moves ~230 km a step), each pass near the runway refined at 3 s.
        for (int j = 0; j < 17280 && !found; ++j)
        {
            const double tc = t0r + 30.0 * j;
            bool hit = false;
            const double err = evalAt(tc, hit);
            if (err > 0.9e300)
                continue;
            for (int r = -10; r <= 10 && !found; ++r)
            {
                const double tr = tc + 3.0 * r;
                const double er = evalAt(tr, hit);
                if (er < bestErr)
                {
                    bestErr = er;
                    tBestR = tr;
                }
                found = hit;
            }
        }
        planReturn(tBestR);
        C.tGlide = C.tChute; // (the entry's end, 10 km)
        {
            // Its speed over the ground at the entry's end: 180 m/s along its track (rwDv makes up the difference).
            const glm::dvec3 Pi1 = crewEntryPos(C, C.tGlide), Pi0 = crewEntryPos(C, C.tGlide - 0.5);
            const glm::dvec3 E1 = eciToEcef(Pi1, C.tGlide), E0 = eciToEcef(Pi0, C.tGlide - 0.5);
            const glm::dvec3 up = glm::normalize(E1);
            glm::dvec3 dir = eciToEcef(Pi1 - Pi0, C.tGlide);
            dir = glm::normalize(dir - glm::dot(dir, up) * up);
            const glm::dvec3 vNow = (E1 - E0) / 0.5;
            C.rwDv = 180.0 * dir - (vNow - glm::dot(vNow, up) * up);
            C.rwDvOn = true;
        }
        {
            const glm::dvec3 E1 = eciToEcef(crewEntryPos(C, C.tGlide), C.tGlide);
            const glm::dvec3 E0 = eciToEcef(crewEntryPos(C, C.tGlide - 0.5), C.tGlide - 0.5);
            C.p0 = plane(glm::normalize(E1));
            C.v0 = (C.p0 - plane(glm::normalize(E0))) / 0.5;
            C.h0 = glm::length(E1) - kR - C.rwElev;
        }
        // Land along the runway the glide arrives from (its approach end nearer the entry's end).
        C.rwDir = glm::dot(C.p0, D) < 0.0 ? D : -D;
        C.pTD = -C.rwDir * (0.5 * pg.runwayLenM - 450.0);
        const double dist = glm::length(C.pTD - C.p0);
        C.Tg = std::max(300.0, dist / 115.0);
        C.tTD = C.tGlide + C.Tg;
        C.lRoll = std::min(2400.0, pg.runwayLenM - 900.0);
        C.tStop = C.tTD + 2.0 * C.lRoll / C.vTD;
        C.tChute = C.tMains = 1e300;
        C.tSplash = C.tTD;
        C.lands = true;
        C.groundM = C.rwElev;
        C.tGone = C.tStop + 2400.0; // towed off the runway
        return true;
    }
    const bool wantLand = pg.crewLands && !C.burns;
    const double t0 = C.tDepEnd + (C.abort ? 1.5 : 3.0) * 3600.0;
    double tBest = t0 + 7200.0 * h01(B.seed, 42, 1);
    for (int j = 0; j < 120; ++j)
    {
        const double tc = t0 + 720.0 * j;
        const double sd = planReturn(tc);
        if (wantLand ? sd < -15000.0 : sd > 15000.0) // (the water map saturates at ~19.5 km)
        {
            tBest = tc;
            break;
        }
    }
    {
        const double sd = planReturn(tBest);
        const double la = glm::degrees(std::asin(glm::clamp(C.chuteEcef.z, -1.0, 1.0)));
        const double lo = glm::degrees(std::atan2(C.chuteEcef.y, C.chuteEcef.x));
        C.lands = sd < 0.0;
        C.groundM = C.lands ? demElevCpu(la, lo) : 0.0;
    }
    if (C.lands != wantLand && !C.burns)
        Log::line("crew: " + pg.spacecraft + " found no " + (wantLand ? "land" : "sea") + " to come down on within a day");
    if (C.burns)
    {
        // It breaks up and burns in the entry (the re-entry fireball from the entry interface, launchFillVisuals):
        // nothing comes down.
        C.tCm = C.tChute = C.tMains = C.tSplash = 1e300;
        C.tGone = C.tEI;
        return true;
    }
    // Its speed over the ground where the drogues open (the entry's, less the Earth's rotation, horizontal): eased out.
    {
        const glm::dvec3 E1 = eciToEcef(crewEntryPos(C, C.tChute), C.tChute);
        const glm::dvec3 E0 = eciToEcef(crewEntryPos(C, C.tChute - 0.5), C.tChute - 0.5);
        glm::dvec3 v = (E1 - E0) / 0.5;
        v -= glm::dot(v, C.chuteEcef) * C.chuteEcef;
        C.chuteV = v;
    }
    // The mains at 3 km over the ground, then down (both by bisection on crewChuteH's closed forms).
    C.tMains = C.tChute + 1000.0; // (crewChuteH's drogue branch for the first search)
    {
        double lo = 0.0, hi = 1000.0;
        for (int i = 0; i < 50; ++i)
        {
            const double m = 0.5 * (lo + hi);
            (crewChuteH(C, m) > 3000.0 + C.groundM ? lo : hi) = m;
        }
        C.tMains = C.tChute + 0.5 * (lo + hi);
        const double dtM = C.tMains - C.tChute;
        lo = dtM;
        hi = dtM + 3000.0;
        for (int i = 0; i < 50; ++i)
        {
            const double m = 0.5 * (lo + hi);
            (crewChuteH(C, m) > C.groundM ? lo : hi) = m;
        }
        C.tSplash = C.tChute + 0.5 * (lo + hi);
    }
    C.tGone = C.tSplash + 2400.0;
    return true;
}

// The spacecraft as a transient (kind 8, key batch << 3 ^ 3) at t: its state from the model (velocity and acceleration
// by differences), its attitude and turn rate, its model for the phase. False when it does not exist then.
static bool crewFill(const SatelliteSim &sim, const SatelliteSim::LaunchState &L, const LBatch &B, uint64_t key, double t,
                     SatelliteSim::SelTransient &s)
{
    const LCrew &C = B.crew;
    glm::dvec3 P, P1, P2;
    uint8_t burn = 0;
    if (!crewPos(C, t, P, nullptr, &burn))
        return false;
    if (!crewPos(C, t - 0.5, P1))
        P1 = P;
    if (!crewPos(C, t - 1.0, P2))
        P2 = P1;
    s.key = (key << 3) ^ 3ull;
    s.kind = 8;
    s.stage = 0;
    s.a = key;
    s.tau = t - C.tSep;
    s.burn = burn;
    s.pos = P;
    s.vel = (P - P1) / 0.5;
    s.acc = (P - 2.0 * P1 + P2) / 0.25;
    const int part = crewModelPart(C, t);
    const std::string id = "rocket_" + L.cat.programs[B.prog].crewModel + (part == 1 ? "_cm" : part == 2 ? "_chute" : "");
    auto ti = sim.rocketTypes.find(id);
    s.type = ti != sim.rocketTypes.end() ? ti->second : -1;
    const glm::dmat3 R = crewAttitude(C, t), R0 = crewAttitude(C, t - 1.0);
    s.axis = R[2];
    s.hasAtt = true;
    s.attQ = glm::normalize(glm::quat_cast(R));
    // Its turn rate (ECI): R = Rd R0 over one second.
    const glm::dquat dq = glm::normalize(s.attQ * glm::inverse(glm::normalize(glm::quat_cast(R0))));
    const double ang = 2.0 * std::acos(glm::clamp(std::abs(dq.w), 0.0, 1.0));
    glm::dvec3 axq(dq.x, dq.y, dq.z);
    s.attOmega = glm::length(axq) > 1e-12 ? glm::normalize(axq) * ang * (dq.w < 0.0 ? -1.0 : 1.0) : glm::dvec3(0.0);
    s.attT0 = t;
    return true;
}

// ── The station's arm (2026-10-21, Canadarm2 berthing) ─────────────────────────────────────────────────────────────
// Its pose in the station's body frame: the shoulder S (the base + its housing), the elbow E, the wrist W and the end
// effector's tip G; nrm = the normal of the plane the booms bend in. A two-link solution (equal booms) of the wrist
// point, the elbow toward the arm's pole. The tip follows a target that is a pure function of t: the park, or a
// berthing vehicle's grapple fixture (approached and left 0.6 m out along the fixture's normal), blended by
// smootherstep between targets that are themselves continuous: no jump at any step.
static void armSolve(const launch::StationArm &A, const glm::dvec3 &G, const glm::dvec3 &e, ArmPose &o)
{
    o.S = A.base + A.shoulderM * A.baseN;
    o.G = G;
    glm::dvec3 W = G - A.wristM * e;
    glm::dvec3 d = W - o.S;
    double dl = glm::length(d);
    const double reach = 2.0 * A.boomM * 0.999;
    o.reachShort = std::max(0.0, dl - reach);
    if (dl > reach)
    {
        W = o.S + d * (reach / dl);
        dl = reach;
    }
    o.W = W;
    const glm::dvec3 u = dl > 1e-6 ? d / dl : A.baseN;
    glm::dvec3 v = A.pole - glm::dot(A.pole, u) * u;
    if (glm::length(v) < 1e-6)
        v = glm::cross(u, std::abs(u.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
    v = glm::normalize(v);
    const double a = 0.5 * dl, h = std::sqrt(std::max(A.boomM * A.boomM - a * a, 0.0));
    o.E = o.S + a * u + h * v;
    o.nrm = glm::normalize(glm::cross(u, v));
}
// The station's position and body frame from its elements (crewStation's frame).
static void stationFrameAt(const SatOrbitElems &st, double t, glm::dvec3 &Ps, glm::dmat3 &R)
{
    const SatOrbitState s = satOrbitStateAt(st, t);
    Ps = s.posEci;
    const glm::dvec3 Z = glm::normalize(s.nadir);
    glm::dvec3 X = s.velocity - glm::dot(s.velocity, Z) * Z;
    X = glm::length(X) > 1e-9 ? glm::normalize(X) : glm::normalize(glm::cross(Z, glm::dvec3(0, 0, 1)));
    R = glm::dmat3(X, glm::cross(Z, X), Z);
}
// A berthing vehicle's grapple fixture at t, in the station's frame: its point G and outward normal n.
static bool berthFixture(const LCrew &C, double t, glm::dvec3 &G, glm::dvec3 &n)
{
    glm::dvec3 P;
    if (!crewPos(C, t, P))
        return false;
    glm::dvec3 Ps;
    glm::dmat3 R;
    crewStation(C, t, Ps, R);
    const glm::dmat3 Rv = crewAttitude(C, t);
    const glm::dmat3 Rt = glm::transpose(R);
    G = Rt * (P + Rv * C.gfB - Ps);
    n = glm::normalize(Rt * (Rv * C.gfN));
    return true;
}
// The arm at t, given the berthing vehicles visiting its station (any order). Parked unless one is in its arm window.
static void stationArmPose(const launch::StationArm &A, double t, const std::vector<const LCrew *> &visits, ArmPose &o)
{
    const glm::dvec3 Gp = A.parkW + A.wristM * A.parkN, ep = A.parkN;
    auto ss = [](double x) {
        x = glm::clamp(x, 0.0, 1.0);
        return x * x * x * (x * (6.0 * x - 15.0) + 10.0);
    };
    for (const LCrew *Cp : visits)
    {
        const LCrew &C = *Cp;
        if (!C.on || !C.berth || C.abort == 1 || C.abort == 2 || t >= C.tDeath)
            continue;
        // Fixed targets (the vehicle is still in the station's frame at each: the capture hold, berthed, the release
        // hold): pre = 0.6 m out along the fixture's normal.
        auto at = [&](double tt, double out, glm::dvec3 &G, glm::dvec3 &e) {
            glm::dvec3 g, n;
            if (!berthFixture(C, tt, g, n))
                return false;
            G = g + out * n;
            e = -n;
            return true;
        };
        auto blend = [&](const glm::dvec3 &G0, const glm::dvec3 &e0, const glm::dvec3 &G1, const glm::dvec3 &e1, double w) {
            const glm::dvec3 e = glm::normalize(glm::mix(e0, e1, w) + 1e-9 * e1);
            armSolve(A, glm::mix(G0, G1, w), e, o);
        };
        glm::dvec3 G0, e0, G1, e1;
        const double tA1 = C.tCapArr;
        if (t >= C.armA0 && t < C.armA3)
        {
            o.with = &C;
            if (t < tA1)
            {
                if (!at(tA1, 0.6, G1, e1))
                    break;
                blend(Gp, ep, G1, e1, ss((t - C.armA0) / (tA1 - C.armA0)));
            }
            else if (t < C.tGrapple)
            {
                if (!at(tA1, 0.6, G0, e0) || !at(tA1, 0.0, G1, e1))
                    break;
                blend(G0, e0, G1, e1, ss((t - (C.tGrapple - kArmOpenS)) / kArmOpenS));
            }
            else if (t < C.armA2)
            {
                if (!at(t, 0.0, G1, e1))
                    break;
                armSolve(A, G1, e1, o);
                o.attached = true;
            }
            else if (t < C.armA2 + kArmOpenS)
            {
                if (!at(C.armA2, 0.0, G0, e0) || !at(C.armA2, 0.6, G1, e1))
                    break;
                blend(G0, e0, G1, e1, ss((t - C.armA2) / kArmOpenS));
            }
            else
            {
                if (!at(C.armA2, 0.6, G0, e0))
                    break;
                blend(G0, e0, Gp, ep, ss((t - C.armA2 - kArmOpenS) / kArmMoveS));
            }
            return;
        }
        if (t >= C.armD0 && t < C.armD3)
        {
            o.with = &C;
            const double tD1 = C.armD0 + kArmMoveS;
            if (t < tD1)
            {
                if (!at(C.tUndock - 1.0, 0.6, G1, e1))
                    break;
                blend(Gp, ep, G1, e1, ss((t - C.armD0) / kArmMoveS));
            }
            else if (t < C.tUndock)
            {
                if (!at(C.tUndock - 1.0, 0.6, G0, e0) || !at(C.tUndock - 1.0, 0.0, G1, e1))
                    break;
                blend(G0, e0, G1, e1, ss((t - tD1) / kArmOpenS));
            }
            else if (t < C.tRelease)
            {
                if (!at(t, 0.0, G1, e1))
                    break;
                armSolve(A, G1, e1, o);
                o.attached = true;
            }
            else if (t < C.tRelease + kArmOpenS)
            {
                if (!at(C.tRelease, 0.0, G0, e0) || !at(C.tRelease, 0.6, G1, e1))
                    break;
                blend(G0, e0, G1, e1, ss((t - C.tRelease) / kArmOpenS));
            }
            else
            {
                if (!at(C.tRelease, 0.6, G0, e0))
                    break;
                blend(G0, e0, Gp, ep, ss((t - C.tRelease - kArmOpenS) / kArmMoveS));
            }
            return;
        }
    }
    o.with = nullptr;
    o.attached = false;
    armSolve(A, Gp, ep, o);
}

// The berthing vehicles visiting a station now (the live crew list; any order).
static std::vector<const LCrew *> berthVisits(const SatelliteSim::LaunchState &L, uint32_t sat)
{
    std::vector<const LCrew *> v;
    for (const auto &cv : L.crewList)
        if (cv.second->crew.on && cv.second->crew.berth && cv.second->crew.sat == sat)
            v.push_back(&cv.second->crew);
    return v;
}

// Crewed stations: rebuild a plane's cached batches when its station's dead second changes (a breakup, a rollback,
// the worker reaching it), so the schedule follows the event log (buildBatch's "station lost").
static void crewSync(const SatelliteSim &sim, SatelliteSim::LaunchState &L)
{
    FPROF_SCOPE("launch.crewSync");
    if (!L.planesBuilt)
        return;
    if (L.crewDead.size() != L.planes.size())
        L.crewDead.assign(L.planes.size(), kessler::kNever);
    std::vector<int> changed;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        if (!L.cat.programs[pl.prog].crew || pl.sats.empty())
            continue;
        int32_t d = kessler::kNever;
        if (sim.kesslerEnabled && 2ull * pl.sats[0] + 1 < sim.kRosterWords.size())
            d = std::min(sim.kRosterWords[2ull * pl.sats[0]], sim.kRosterWords[2ull * pl.sats[0] + 1]);
        if (d != L.crewDead[pi])
        {
            L.crewDead[pi] = d;
            changed.push_back((int)pi);
        }
    }
    if (changed.empty())
        return;
    L.joinJob(); // (a background build may hold batches built against the old fate)
    for (auto it = L.batches.begin(); it != L.batches.end();)
    {
        const LBatch &B = it->second;
        // (a crew batch carries no train and no retirees: no GPU chunks to release)
        if (B.chunks[0].empty() && B.chunks[1].empty() &&
            std::find(changed.begin(), changed.end(), B.plane) != changed.end())
        {
            it = L.batches.erase(it);
            ++L.batchEpoch; // (it may be in the window walk's list)
        }
        else
            ++it;
    }
}


// (2026-10-21) Builds the given batches (not yet in L.batches) in parallel: their candidates, the rivals' day buckets
// they will ask for, then the builds (buildBatch is a pure function of the batch; candidate()'s cache is under
// L.candsMx). The walk (launchUpdate, a frame's cap of them) and the load (launchPrewarm, all of them) share it.
using ToBuild = SatelliteSim::LaunchState::BuildItem;
static size_t buildThreads()
{
    // The background build (gBuildBg) leaves two cores to the frame and its other workers.
    const size_t hw = std::max(1u, std::thread::hardware_concurrency());
    return gBuildBg ? std::max<size_t>(1, hw > 4 ? hw - 2 : hw - 1) : hw;
}
static void buildMissing(SatelliteSim &sim, SatelliteSim::LaunchState &L, const std::vector<ToBuild> &toBuild,
                         std::vector<LBatch> &built)
{
    auto par = [](size_t n, const auto &fn) {
        const size_t hw = buildThreads();
        const size_t workers = std::min(hw, n / 2);
        if (workers <= 1)
        {
            for (size_t i = 0; i < n; ++i)
                fn(i);
            return;
        }
        std::atomic<size_t> next{0};
        const bool bg = gBuildBg;
        auto run = [&, bg] {
            if (bg)
            {
                gBuildBg = true;
                kessler::lowerThreadPriority();
            }
            for (size_t i = next.fetch_add(1); i < n; i = next.fetch_add(1))
                fn(i);
        };
        std::vector<std::thread> th;
        for (size_t w = 1; w < workers; ++w)
            th.emplace_back(run);
        run();
        for (std::thread &x : th)
            x.join();
    };
    const size_t n = toBuild.size();
    // Their candidates, then the rivals' day buckets they will ask for, each built once (in parallel).
    {
        std::vector<SatelliteSim::LaunchState::Cand> c0(n);
        par(n, [&](size_t i) { c0[i] = candidate(sim, L, toBuild[i].pi, toBuild[i].k, toBuild[i].b); });
        std::vector<std::pair<int, int64_t>> need;
        for (const SatelliteSim::LaunchState::Cand &c : c0)
            if (c.tL >= 0.0 && c.site >= 0 && !standsDownAt(sim, c.tL)) // (stood down: buildBatch asks no rivals)
                for (int64_t be = (int64_t)std::floor((c.tL - 5400.0) / kRivalBucketS);
                     be <= (int64_t)std::floor((c.tL + 5400.0) / kRivalBucketS); ++be)
                    need.push_back({c.site, be});
        std::sort(need.begin(), need.end());
        need.erase(std::unique(need.begin(), need.end()), need.end());
        // Few buckets: each one split over the threads; many: one bucket a thread.
        const size_t hwB = buildThreads();
        if (need.size() * 2 < hwB)
            for (const auto &nd : need)
                rivalBucket(sim, L, nd.first, nd.second, true);
        else
            par(need.size(), [&](size_t i) { rivalBucket(sim, L, need[i].first, need[i].second); });
    }
    built.assign(n, LBatch{});
    par(n, [&](size_t i) {
        LBatch &nb = built[i];
        nb.plane = toBuild[i].pi;
        nb.k = toBuild[i].k;
        nb.b = toBuild[i].b;
        buildBatch(sim, L, nb, L.planes[(size_t)toBuild[i].pi], toBuild[i].life);
    });
}

// (2026-10-21b) The background build (LaunchState::BuildJob): the items in order (most urgent first), in chunks
// (64 while the first ones are wanted, then 256), each published by `done`.
static void startBuildJob(SatelliteSim &sim, SatelliteSim::LaunchState &L, std::vector<ToBuild> items)
{
    auto J = std::make_unique<SatelliteSim::LaunchState::BuildJob>();
    J->items = std::move(items);
    J->out.resize(J->items.size());
    J->gen = L.buildGen;
    J->t0 = std::chrono::steady_clock::now();
    for (const ToBuild &it : J->items)
        L.jobKeys.insert(it.key);
    SatelliteSim::LaunchState::BuildJob *jp = J.get();
    J->th = std::thread([&sim, &L, jp] {
        gBuildBg = true;
        kessler::lowerThreadPriority();
        const size_t n = jp->items.size();
        size_t done = 0;
        std::vector<LBatch> built;
        while (done < n && !jp->cancel.load(std::memory_order_relaxed))
        {
            const size_t e = std::min(n, done + (done < 512 ? (size_t)64 : (size_t)256));
            const std::vector<ToBuild> part(jp->items.begin() + (ptrdiff_t)done, jp->items.begin() + (ptrdiff_t)e);
            buildMissing(sim, L, part, built);
            for (size_t q = 0; q < part.size(); ++q)
                jp->out[done + q] = std::move(built[q]);
            done = e;
            jp->done.store(done, std::memory_order_release);
        }
        jp->finished.store(true, std::memory_order_release);
    });
    ++L.jobsRun;
    L.job = std::move(J);
}

// Moves the background build's finished prefix into the batches (within ~maxMs); joins it when all is in.
static void applyBuildJob(SatelliteSim &sim, SatelliteSim::LaunchState &L, double maxMs)
{
    (void)sim;
    if (!L.job)
        return;
    FPROF_SCOPE("launch.applyBuildJob");
    SatelliteSim::LaunchState::BuildJob &J = *L.job;
    const size_t d = J.done.load(std::memory_order_acquire);
    const auto a0 = std::chrono::steady_clock::now();
    if (J.applied == 0 && d > 0)
        L.batches.reserve(L.batches.size() + J.items.size()); // (one rehash for the job, not several mid-apply)
    while (J.applied < d)
    {
        const size_t e = std::min(d, J.applied + 16); // (2026-10-21c: 256 a look at the clock overran to 9-14 ms at 1 yr/s)
        for (; J.applied < e; ++J.applied)
        {
            const ToBuild &it = J.items[J.applied];
            auto ins = L.batches.emplace(it.key, std::move(J.out[J.applied]));
            ins.first->second.touched = std::max(ins.first->second.touched, L.frame); // (not erased before it is asked for)
            L.jobKeys.erase(it.key);
        }
        if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a0).count() > maxMs)
            break;
    }
    if (J.finished.load(std::memory_order_acquire) && J.applied >= J.done.load(std::memory_order_acquire))
    {
        if (J.th.joinable())
            J.th.join();
        L.lastJobMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - J.t0).count();
        L.lastJobN = J.items.size();
        char msg[160];
        snprintf(msg, sizeof(msg), "launches: background build of %zu batches in %.0f ms", J.items.size(), L.lastJobMs);
        Log::lineDeferred(msg);
        L.job.reset();
        L.jobKeys.clear();
    }
}
};

// ── Per frame ───────────────────────────────────────────────────────────────────────────────────
// (2026-10-21) During the loading screen (setAudio, after init): the planes and every batch the first frame's walk
// will want, built in parallel with no per-frame cap. The first frame after the load spent ~25 ms building them (and
// the next dozen frames 6-12 ms each under the cap). The walk itself still runs on the first frame and finds them all.
// The same batches the walk would build (a pure function of the batch), so nothing drawn changes.
void SatelliteSim::launchJoinBuildJob()
{
    if (launchState)
        launchState->joinJob();
}

void SatelliteSim::launchPrewarm()
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!launchesEnabled || !L.loaded || activeSatCount == 0)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    if (L.planesBuilt && (L.lifeScale != launchLifeScale || L.deployOn != launchDeployEra))
        launchInitBlock();
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    LaunchImpl::crewSync(*this, L);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<LaunchImpl::ToBuild> toBuild;
    const double leadS = LaunchImpl::maxVehicleLeadS(L);
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        const double life = progLifeS(pg, (double)launchLifeScale);
        // (the walk's window, launchUpdate)
        const double lo = t - (pg.crew ? life / kDay + 14.0 : pg.deorbitDays[1] + 1.0) * kDay,
                     hi = std::min(t + (pg.raiseDays[1] + 1.5) * kDay, LaunchImpl::standDownArrival(*this, pg, leadS));
        if (hi < lo)
            continue;
        LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
            const uint64_t key = batchKey((int)pi, k, b);
            if (L.batches.find(key) == L.batches.end())
                toBuild.push_back({key, (int)pi, k, b, life});
        });
    }
    std::vector<LBatch> built;
    LaunchImpl::buildMissing(*this, L, toBuild, built);
    for (size_t q = 0; q < toBuild.size(); ++q)
        L.batches.emplace(toBuild[q].key, std::move(built[q]));
    ++L.batchEpoch; // (no walk list is held yet; a walk is due)
    char msg[160];
    snprintf(msg, sizeof(msg), "launches: prewarmed %zu batches in %.0f ms", toBuild.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    Log::line(msg);
}

void SatelliteSim::launchUpdate(VulkanContext &ctx, VkCommandBuffer cmd)
{
    FPROF_SCOPE("launchUpdate");
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    L.nRockets = L.nTrain = L.nRetire = L.nWrites = L.nBatches = 0;
    L.nDueEval = L.nFinds = 0;
    ++L.frame;
    LaunchImpl::ensureCatalog(L);
    if (kesslerBuf == VK_NULL_HANDLE || activeSatCount == 0)
        return;
    if (L.stage == VK_NULL_HANDLE)
    {
        // One per frame slot (frames in flight): the frame's record copies read this slot's staging.
        ctx.slotted(&L.stage, &L.stageMem, &L.stageMapped, 64ull * LaunchState::kStageRecs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "launchStage");
    }
    // kesslerBuf recreated: the GPU block is zero (all hidden); everything live must be sent again.
    if (L.gpuGen != kesslerBufGen)
    {
        L.gpuGen = kesslerBufGen;
        for (SlotInfo &s : L.slots)
        {
            s.shown = false;
            s.tw = -1e300;
        }
        for (auto &kv : L.batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300; // every batch due again
        std::fill(L.liveSkip.begin(), L.liveSkip.end(), LaunchState::LiveSkip{}); // (their skip intervals with them)
    }
    // A new design-life scale reshuffles every refresh: start over (and the deployment era switched on / off).
    if (L.planesBuilt && (L.lifeScale != launchLifeScale || (L.afterStorm >= 0 && L.afterStorm != launchAfterStorm) ||
                          L.deployOn != launchDeployEra))
    {
        launchInitBlock();
        L.gpuGen = kesslerBufGen;
    }
    // The deployment era's roster table (who is there when): computed once (and on a change), uploaded after the
    // Kessler roster words.
    rosterLifeEnsure();
    if (!rosterLifeUploaded)
        rosterLifeUpload(ctx);
    L.afterStorm = launchAfterStorm;
    if (L.clearGpu)
    {
        L.clearGpu = false;
        vkCmdFillBuffer(cmd, kesslerBuf, 4ull * kLaunchWord0, 64ull * kLaunchCap, 0u);
        VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.buffer = kesslerBuf;
        fb.offset = 4ull * kLaunchWord0;
        fb.size = 64ull * kLaunchCap;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb,
                             0, nullptr);
        for (SlotInfo &si : L.slots)
            si.shown = false;
        for (auto &kv : L.batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300;
        std::fill(L.liveSkip.begin(), L.liveSkip.end(), LaunchState::LiveSkip{}); // (their skip intervals with them)
    }
    const bool on = launchesEnabled && L.loaded;
    if (on && !L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    LaunchImpl::crewSync(*this, L);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<std::pair<uint64_t, LBatch *>> touchedP;
    auto pt = std::chrono::steady_clock::now();
    constexpr double kWalkMarginS = 300.0;
    const bool reuseWalk = on && L.walkOk && L.walkEpoch == L.batchEpoch && std::abs(t - L.walkT) < kWalkMarginS;
    // Reused, nothing is touched again: "listed now" is B.inWalk == L.walkFrame (the frame of the current walk).
    if (reuseWalk)
    {
    }
    else if (on)
    {
        FPROF_SCOPE("launch.walk");
        // Batches that matter now: from before the launch to the last retiree's re-entry. The list is reused while t
        // stays within kWalkMarginS of this walk: the window's edges are days from any launch or re-entry, so a batch
        // entering or leaving it a few minutes late changes nothing drawn (and the batches built stay the same).
        // (2026-10-20) The batches not built yet are collected, then built in parallel (buildBatch is a pure function of
        // the batch; its only shared write is candidate()'s cache, under L.candsMx): built one by one, the first frames
        // after a load and every frame at a fast warp spent 100-170 ms here. At most kMaxBuilds a frame.
        // The cap follows the measured cost of a build (with the candidates and the rivals' day buckets it needs, which
        // are the expensive part the first time): ~8 ms of building a frame. Not every batch is built at once after a
        // load or under a fast warp; the walk is then redone each frame until it is complete (walkOk).
        // (2026-10-21b) Thousands missing at once (a time jump) go to the background build (LaunchState::BuildJob),
        // most urgent first; its finished prefix is moved in before the walk (applyBuildJob). A few are built here.
        LaunchImpl::applyBuildJob(*this, L, 2.0);
        using ToBuild = LaunchImpl::ToBuild;
        std::vector<ToBuild> toBuild;
        std::vector<std::pair<uint64_t, LBatch *>> found; // in walk order; null = built below
        bool incomplete = false;
        L.walkList.clear();
        // (2026-10-21d) The window ends where the launches stand down for good at the Sun's Fury (standDownArrival):
        // the batches past it are all stood down (never valid, nothing to draw or write), so they are neither built nor
        // walked. After the CME at 1 week/s they were the walk's 4-8 ms of builds every frame; once the window has passed
        // the boundary a plane walks nothing (crew planes, whose window reaches back a refresh period, the longest).
        const double leadS = LaunchImpl::maxVehicleLeadS(L);
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
        {
            const LPlane &pl = L.planes[pi];
            const launch::Program &pg = L.cat.programs[pl.prog];
            const double life = progLifeS(pg, (double)launchLifeScale);
            // (a crew flight's spacecraft stays docked about a refresh period and then comes home: 2026-10-20)
            const double lo = t - (pg.crew ? life / kDay + 14.0 : pg.deorbitDays[1] + 1.0) * kDay,
                         hi = std::min(t + (pg.raiseDays[1] + 1.5) * kDay, LaunchImpl::standDownArrival(*this, pg, leadS));
            if (hi < lo)
                continue;
            LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
                const uint64_t key = batchKey((int)pi, k, b);
                auto it = L.batches.find(key);
                ++L.nFinds;
                if (it == L.batches.end())
                {
                    if (!L.jobKeys.empty() && L.jobKeys.count(key))
                    {
                        incomplete = true; // the background build has it
                        return;
                    }
                    toBuild.push_back({key, (int)pi, k, b, life});
                    found.push_back({key, nullptr});
                    return;
                }
                found.push_back({key, &it->second});
            });
        }
        if (!toBuild.empty())
        {
            FPROF_SCOPE("launch.buildBatches");
            // Most urgent first: the launches nearest now (a batch's arrival less its mean raise).
            if (toBuild.size() > 1)
            {
                std::vector<std::pair<double, size_t>> ord(toBuild.size());
                for (size_t i = 0; i < toBuild.size(); ++i)
                {
                    const ToBuild &tb = toBuild[i];
                    const LPlane &pl = L.planes[(size_t)tb.pi];
                    const launch::Program &pg = L.cat.programs[pl.prog];
                    const double tL = LaunchImpl::arrivalOf(pl, tb.k, tb.b, tb.life) -
                                      0.5 * (pg.raiseDays[0] + pg.raiseDays[1]) * kDay;
                    ord[i] = {std::abs(tL - t), i};
                }
                std::sort(ord.begin(), ord.end());
                std::vector<ToBuild> sorted(toBuild.size());
                for (size_t i = 0; i < ord.size(); ++i)
                    sorted[i] = toBuild[ord[i].second];
                toBuild.swap(sorted);
            }
            // Many: the most urgent few here (this frame shows them), the rest to the background build.
            // Past 1 h/s of warp (a new stretch of days every frame, nothing lingers to be seen) none here: the first
            // builds of a day ask for its rival buckets, 20-40 ms on the main thread at 1 yr/s.
            // (SATLIGHTSIM_LAUNCH_BG=0 turns the background build off: the A/B for its schedule identity.)
            static const bool bgOn = [] {
                const char *e = std::getenv("SATLIGHTSIM_LAUNCH_BG");
                return !(e && e[0] == '0');
            }();
            const size_t kBgMin = bgOn ? 48 : SIZE_MAX;
            const bool fastWarp = !timePaused && kTimeScales[timeScaleIdx] > 3600.0f;
            // A jump (a Watch, a bookmark, a typed time: sim time an hour or more from the last walk) hands every build to
            // the job: one build whose day's rival buckets are cold is up to ~7 ms, and a handful of them were 20-30 ms of
            // the landing frame (perf4-cpu 2026-10-24). The rockets of the moment appear a few frames later.
            const bool jumped = std::abs(t - L.walkT) > 3600.0 && L.walkT > -1e299;
            if ((toBuild.size() > kBgMin || (bgOn && (fastWarp || jumped) && toBuild.size() > (jumped ? 0u : 4u))) && !L.job)
            {
                // (4 here: after a jump the first builds of a day ask for its rivals' buckets, and 16 of them were ~20 ms of
                // the landing frame — a Watch's jump; perf4-cpu 2026-10-24. The rest arrive from the job a few frames on.)
                const size_t syncN = (fastWarp || jumped) ? 0 : 4;
                std::vector<ToBuild> rest(toBuild.begin() + (ptrdiff_t)syncN, toBuild.end());
                LaunchImpl::startBuildJob(*this, L, std::move(rest));
                toBuild.resize(syncN);
                incomplete = true;
            }
            else if (L.job && (fastWarp || toBuild.size() > 8))
            {
                // The job holds the cores: these wait for the next one (the walk stays incomplete meanwhile).
                toBuild.clear();
                incomplete = true;
            }
            struct CapTimer
            {
                LaunchState &L;
                size_t n;
                std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
                ~CapTimer()
                {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    const double per = std::max(ms / (double)std::max<size_t>(n, 1), 1e-3);
                    L.buildCap = (int)std::clamp(8.0 / per, 16.0, 400.0);
                }
            } capTimer{L, toBuild.size()};
            // (2026-10-21) ... and a wall-time budget: the cap follows the LAST frame's cost per build, and a frame of
            // cheap builds (cached candidates) let it reach 400, so the next frame's 400 expensive ones took 300-600 ms
            // (1 week/s after the CME). Built in pieces until ~8 ms are spent; the rest wait for the next frame (the walk
            // is then incomplete and redone, as under the cap).
            std::vector<LBatch> built;
            size_t done = 0;
            const auto b0 = std::chrono::steady_clock::now();
            // Each piece sized by the measured cost of a build (L.buildMsEach, eased), so the last piece cannot overrun
            // the budget by much: a piece of 128 expensive builds alone took ~40 ms (1 yr/s, the user's settings).
            // The frame's first piece is at most 16 (the cost of a build varies by an order of magnitude: the first
            // builds of a day ask for the rivals' day buckets), then each piece takes this frame's measured cost.
            // With the background build running (it shares the cores) a smaller budget here.
            const double budgetMs = L.job ? 4.0 : 8.0;
            double eachNow = -1.0;
            while (done < toBuild.size())
            {
                const double spent = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count();
                const size_t piece =
                    eachNow < 0.0 ? (size_t)std::clamp(6.0 / std::max(L.buildMsEach, 0.005), 4.0, 16.0)
                                  : (size_t)std::clamp((budgetMs - spent) / std::max(eachNow, 0.005), 4.0, 256.0);
                const size_t e = std::min(toBuild.size(), done + piece);
                const auto p0 = std::chrono::steady_clock::now();
                const std::vector<LaunchImpl::ToBuild> part(toBuild.begin() + (ptrdiff_t)done, toBuild.begin() + (ptrdiff_t)e);
                LaunchImpl::buildMissing(*this, L, part, built);
                for (size_t q = 0; q < part.size(); ++q)
                    L.batches.emplace(part[q].key, std::move(built[q]));
                const double each =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count() / (double)part.size();
                L.buildMsEach = 0.5 * L.buildMsEach + 0.5 * each;
                eachNow = eachNow < 0.0 ? each : std::max(each, 0.5 * (eachNow + each));
                done = e;
                if (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count() > budgetMs)
                    break;
            }
            capTimer.n = done;
            if (done < toBuild.size())
                incomplete = true; // redone next frame
            size_t w = 0;
            for (auto &f : found)
            {
                if (!f.second)
                {
                    auto it = L.batches.find(f.first);
                    if (it == L.batches.end())
                        continue; // not built this frame: left out of the walk
                    f.second = &it->second;
                }
                found[w++] = f;
            }
            found.resize(w);
        }
        for (const auto &f : found)
        {
            LBatch &B = *f.second;
            B.touched = L.frame;
            B.inWalk = L.frame;
            L.walkList.push_back({f.first, &B});
            if (B.valid)
                touchedP.push_back({f.first, &B});
        }
        std::sort(L.walkList.begin(), L.walkList.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        L.walkOk = !incomplete && !L.job;
        L.walkT = t;
        L.walkFrame = L.frame;
        L.walkEpoch = L.batchEpoch;
    }
    else
        L.walkOk = false;
    L.profAdd(0, pt);
    // (2026-10-20: the batches by pointer: Starmind's eight-a-launch trains made ~5000 live batches, and the hash
    // lookups over them each frame cost ~0.5 ms.)
    if (!reuseWalk) // (reused: the live list stands)
    {
        if (!on)
            L.walkFrame = UINT32_MAX; // nothing touched now
        std::sort(touchedP.begin(), touchedP.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
        L.live.resize(touchedP.size());
        L.livePtr.resize(touchedP.size());
        L.liveT.resize(touchedP.size());
        L.liveSkip.assign(touchedP.size(), LaunchState::LiveSkip{});
        L.liveCh.resize(touchedP.size());
        for (size_t i = 0; i < touchedP.size(); ++i)
        {
            L.live[i] = touchedP[i].first;
            LBatch *pb = touchedP[i].second;
            L.livePtr[i] = pb;
            pb->liveIdx = (uint32_t)i;
            L.liveT[i] = {pb->tD, pb->tA, pb->ends.empty() ? pb->tA : pb->ends.back() + 1.0};
            L.liveCh[i] = pb->inChunked ? 1 : 0;
        }
    }
    const std::vector<uint64_t> &touched = L.live;
    // The batches launchFillVisuals has anything to draw for around now (2026-10-20, CPU: it looked at all ~5300):
    // their rocket, plumes and stages (tL - 600 s .. tVisEnd) or retirees' re-entries (tA .. the last + 200 s), with the
    // walk's margin, rebuilt with the walk.
    if (!reuseWalk)
    {
        L.visList.clear();
        for (size_t i = 0; i < touched.size(); ++i)
        {
            const LBatch &B = *L.livePtr[i];
            const bool rocket = t + kWalkMarginS >= B.tL - 600.0 && t - kWalkMarginS <= B.tVisEnd;
            const bool re = !B.ends.empty() && t + kWalkMarginS >= std::max(B.tA, B.ends.front()) &&
                            t - kWalkMarginS <= B.ends.back() + 200.0;
            if (rocket || re)
                L.visList.push_back({touched[i], L.livePtr[i]});
        }
        // (2026-10-20) The crew spacecraft flying or docked now (a few at most).
        L.crewList.clear();
        for (size_t i = 0; i < touched.size(); ++i)
        {
            const LCrew &C = L.livePtr[i]->crew;
            if (C.on && t + kWalkMarginS >= C.tSep && t - kWalkMarginS < C.tGone + (C.burns ? 200.0 : 0.0))
                L.crewList.push_back({touched[i], L.livePtr[i]});
        }
    }
    L.nBatches = (uint32_t)touched.size();
    L.profAdd(1, pt);
    // GPU chunks: a batch holds the train's while any member climbs, the retirees' while any falls.
    std::vector<VkBufferCopy> copies;
    char *stage = static_cast<char *>(L.stageMapped);
    uint32_t nStage = 0;
    auto putRecord = [&](uint32_t slot, const GpuKepOrbit &g) {
        if (nStage >= LaunchState::kStageRecs)
            return false;
        memcpy(stage + 64ull * nStage, &g, 64);
        copies.push_back({64ull * nStage, 4ull * kLaunchWord0 + 64ull * slot, 64});
        ++nStage;
        return true;
    };
    auto hideSlot = [&](uint32_t slot) {
        SlotInfo &si = L.slots[slot];
        if (si.shown)
        {
            GpuKepOrbit z{};
            if (!putRecord(slot, z))
                return false;
            si.shown = false;
        }
        si.live = false;
        si.tw = -1e300;
        if (selectedSatIndex == (int)(activeSatCount + slot) || viewerSatIndex == (int)(activeSatCount + slot))
            launchSlotEndedSelection(slot); // to its roster slot, or (a retiree) to its re-entry once listed
        // A followed record that ends is handed over by followCheckTarget (a train member to its roster slot, a retiree
        // to its re-entry) or left in place there; it is not snapped home here.
        return true;
    };
    auto releaseChunks = [&](std::vector<uint32_t> &ch) {
        for (uint32_t c : ch)
        {
            for (uint32_t s = c * kChunk; s < (c + 1) * kChunk; ++s)
            {
                hideSlot(s);
                L.slots[s].owner = 0;
                satOrbits[activeSatCount + s].constIdx = UINT32_MAX;
            }
            L.freeChunks.push_back(c);
        }
        ch.clear();
    };
    // Free what no longer matters (a batch out of the window, retirees all down, launches off). The whole map every 8th
    // frame (and after a jump), else only the live batches (2026-10-20: ~6000 batches a frame cost ~0.3 ms).
    // freeLo / freeHi (2026-10-20, CPU): the interval of t in which no chunked batch's need changes; the pass is skipped
    // inside it while the walk stands.
    double freeLo = -1e300, freeHi = 1e300;
    bool freeBlocked = false;
    auto freeBatch = [&](LBatch &B) {
        for (int kind = 0; kind < 2; ++kind)
        {
            if (B.chunks[kind].empty())
                continue;
            const double b0 = kind == 0 ? B.tD : B.tA;
            const double b1 = kind == 0 ? B.tA : B.tA + 400.0 * kDay;
            const bool need = on && B.inWalk == L.walkFrame && B.valid && t >= b0 && t < b1;
            if (t < b0)
                freeHi = std::min(freeHi, b0);
            else if (t < b1)
            {
                freeLo = std::max(freeLo, b0);
                freeHi = std::min(freeHi, b1);
            }
            else
                freeLo = std::max(freeLo, b1);
            if (!need)
            {
                if (nStage + kChunk * (uint32_t)B.chunks[kind].size() <= LaunchState::kStageRecs)
                {
                    releaseChunks(B.chunks[kind]);
                    B.tw[kind] = -1e300;
                    B.nUp[kind] = 0;
                    if (B.liveIdx < L.livePtr.size() && L.livePtr[B.liveIdx] == &B)
                        L.liveSkip[B.liveIdx] = LaunchState::LiveSkip{};
                }
                else
                    freeBlocked = true;
            }
        }
    };
    // (2026-10-20, CPU) Every frame over the batches that hold chunks (LaunchState::chunked), whether listed this frame
    // or not; the batches no longer touched are erased in a pass every 64th frame. It walked the live list each frame
    // and the whole map every 8th (~0.14 ms with ~5300 live batches); a batch out of the window now gives its chunks
    // back the frame it leaves (up to 8 frames sooner).
    // (Only while the staging can take every hide: a freed slot whose record stayed up would be a ghost.)
    if (!reuseWalk || !on || L.freeForce || t < L.freeLo || t >= L.freeHi)
    {
        size_t w = 0;
        for (size_t i = 0; i < L.chunked.size(); ++i)
        {
            LBatch *pb = L.chunked[i];
            freeBatch(*pb);
            if (pb->chunks[0].empty() && pb->chunks[1].empty())
            {
                pb->inChunked = false;
                if (pb->liveIdx < L.livePtr.size() && L.livePtr[pb->liveIdx] == pb)
                    L.liveCh[pb->liveIdx] = 0;
            }
            else
                L.chunked[w++] = pb;
        }
        L.chunked.resize(w);
        L.freeLo = freeLo;
        L.freeHi = freeHi;
        L.freeForce = freeBlocked;
    }
    const bool fullFree = (L.frame % 64u) == 0u || !on || std::abs(t - L.lastFullFreeT) > 600.0;
    if (fullFree)
    {
        FPROF_SCOPE("launch.fullFree");
        L.lastFullFreeT = t;
        // (2026-10-21c) The batches dropped are freed on a detached thread (their vectors: members, chunks, the crew's
        // legs): at 1 yr/s this pass runs every frame and the destructors were ~3-4 ms of it.
        std::vector<std::remove_reference_t<decltype(L.batches)>::node_type> dead;
        for (auto it = L.batches.begin(); it != L.batches.end();)
        {
            const LBatch &B = it->second;
            if (B.touched + 600 < L.frame && B.inWalk != L.walkFrame && B.chunks[0].empty() && B.chunks[1].empty())
            {
                auto nx = std::next(it);
                dead.push_back(L.batches.extract(it));
                it = nx;
            }
            else
                ++it;
        }
        if (dead.size() > 64)
            std::thread([d = std::move(dead)]() mutable { d.clear(); }).detach();
    }
    L.profAdd(2, pt);
    // Writes: members coming up first, then the stalest. A record is rewritten every ~20 s of sim time (the GPU
    // carries it on as a Kepler orbit meanwhile: the climb's drift over 20 s is under a metre).
    const double tEpoch = (double)orbitEpochDay * 86400.0 + orbitEpochSec;
    uint32_t budget = 1200;
    double writeMs = 0.0;
    auto writeMember = [&](LBatch &B, int kind, uint32_t mi, uint32_t slot) {
        struct WT
        {
            double &acc;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~WT() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
        } wt{writeMs};
        const LMember &mb = B.mem[mi];
        GpuKepOrbit g{};
        SatOrbitElems ce{};
        if (!LaunchImpl::memberRecord(*this, B, mb, kind, t, tEpoch, g, ce))
            return false;
        if (!putRecord(slot, g))
            return false; // (the frame's staging is full: the batch stays due, 2026-10-20)
        const SatOrbit &so = satOrbits[mb.sat];
        SlotInfo &si = L.slots[slot];
        si.shown = true;
        si.live = true;
        si.tw = t;
        L.elems[slot] = ce;
        const double elA = ce.rSatM, elI = ce.incl, n = ce.meanMot;
        SatOrbit &o = satOrbits[activeSatCount + slot];
        o.typeIdx = so.typeIdx;
        o.constIdx = so.constIdx;
        o.incl = (float)elI;
        o.altM = (float)(elA - kR);
        o.R_sat = (float)elA;
        o.meanMot = (float)n;
        o.launchSlot = (int32_t)slot;
        ++L.nWrites;
        if (budget > 0)
            --budget;
        return true;
    };
    if (on && LaunchImpl::gExactElems)
    {
        // (2026-10-26, issue 20261026_205544) The members drawn as meshes last frame first, every frame: a record is a
        // Kepler orbit and the path leaves it (the climb's ramp, the ease from free flight into the slot's path: up to
        // ~0.05 m/s^2), and under time warp the 1200-a-frame budget lets records age 30 s (1 min/s) to 250 s (5 min/s)
        // of sim time — 14 m to 940 m off — so a mesh's own point, bloom and glare left it and snapped back at each
        // rewrite. The mesh itself is placed from the exact path (launchOrbitElems).
        for (uint32_t slot : meshLaunchSlots)
        {
            if (slot >= L.slots.size())
                continue;
            const SlotInfo &si = L.slots[slot];
            if (!si.live || !si.shown || si.owner == 0 || si.tw == t)
                continue;
            auto it = L.batches.find(si.owner / 2);
            if (it == L.batches.end() || si.m >= it->second.mem.size())
                continue;
            writeMember(it->second, (int)(si.owner % 2), si.m, slot);
        }
    }
    if (on)
    {
        // A batch's records are rewritten together, when due: never written, 20 s of sim time since, or a retiree's
        // re-entry passed. Idle batches cost nothing. The write budget runs round-robin from a moving start.
        uint32_t nT = 0, nR = 0;
        const size_t nb = touched.size();
        for (size_t q = 0; q < nb; ++q)
        {
            const size_t bi = (L.cursor + q) % nb;
            // (2026-10-20, CPU) From the compact copy, without touching the batch: outside its train's raising and its
            // retirees' fall and holding no chunks, there is nothing to write or hide (the loop below would only
            // find nobody up).
            {
                const LaunchState::LiveTimes &c = L.liveT[bi];
                if (!(t >= c.tD && t < c.tA) && !(t >= c.tA && t < c.tEnd) && !L.liveCh[bi])
                    continue;
                // ... and inside the interval its last look found it not due (nothing changes there but t).
                const LaunchState::LiveSkip &sk = L.liveSkip[bi];
                if (t >= sk.lo && t < sk.hi)
                {
                    nT += sk.addT;
                    nR += sk.addR;
                    continue;
                }
            }
            LBatch &B = *L.livePtr[bi];
            // The interval of t around now in which this batch stays exactly as found (no kind due, none entering or
            // leaving its phase), and what it adds to the counts meanwhile; cache = false when it cannot be known.
            double skLo = -1e300, skHi = 1e300;
            bool skOk = true;
            uint32_t skT = 0, skR = 0;
            auto notDueSpan = [&](int kind, double tw, double every, double relEnd) {
                skLo = std::max(skLo, tw - every);
                skHi = std::min(skHi, tw + every);
                if (kind == 0 && every > 2.5)
                    skLo = std::max(skLo, relEnd + 600.0); // (below it, every 2 s)
                if (kind == 1)
                {
                    auto it = std::upper_bound(B.ends.begin(), B.ends.end(), tw);
                    if (it != B.ends.end())
                        skHi = std::min(skHi, *it);     // the next re-entry after tw makes it due
                    if (it != B.ends.begin())
                        skLo = std::max(skLo, *(it - 1)); // going back, the last one at or before tw does
                }
            };
            for (int kind = 0; kind < 2; ++kind)
            {
                const double pb0 = kind == 0 ? B.tD : B.tA, pb1 = kind == 0 ? B.tA : B.tA + 400.0 * kDay;
                const bool phase = t >= pb0 && t < pb1;
                if (!phase)
                {
                    if (t < pb0)
                        skHi = std::min(skHi, pb0);
                    else
                        skLo = std::max(skLo, pb1);
                    continue;
                }
                skLo = std::max(skLo, pb0);
                skHi = std::min(skHi, pb1);
                std::vector<uint32_t> &ch = B.chunks[kind];
                // While the train leaves the dispenser (and its first minutes, drifting from the stage at a fraction
                // of a m/s) every 2 s, so each member appears at its release and the separation stays smooth.
                // (2026-10-19) Also every 2 s for a train with a phasing residual (LMember::eps): its slow drift is no
                // free orbit (a circle flown faster than circular), and a Kepler record of it was off by 5-15 m after 20 s.
                const double relEnd = B.mem.empty() ? B.tD : B.mem.back().tRel;
                const double every = (kind == 0 && (t < relEnd + 600.0 || B.resid)) ? 2.0 : 20.0;
                bool due = ch.empty() || std::abs(t - B.tw[kind]) > every;
                if (!due && kind == 1)
                {
                    const double lo = std::min(B.tw[1], t), hi = std::max(B.tw[1], t);
                    auto it = std::upper_bound(B.ends.begin(), B.ends.end(), lo);
                    due = it != B.ends.end() && *it <= hi;
                }
                if (!due || (budget == 0 && !ch.empty()))
                {
                    (kind == 0 ? nT : nR) += B.nUp[kind];
                    if (due)
                        skOk = false;
                    else
                    {
                        notDueSpan(kind, B.tw[kind], every, relEnd);
                        (kind == 0 ? skT : skR) += B.nUp[kind];
                    }
                    continue;
                }
                // Who is up now (Kessler may have taken a slot; a retiree is gone at its re-entry).
                // Nobody can be up (before the first release; after the last re-entry): nothing to test (2026-10-20,
                // CPU: ~200 batches a frame rebuilt this answer member by member).
                if (ch.empty() && (B.mem.empty() || (kind == 0 ? t < B.mem.front().tRel : t >= B.ends.back())))
                {
                    B.nUp[kind] = 0;
                    if (!B.mem.empty())
                    {
                        if (kind == 0)
                            skHi = std::min(skHi, B.mem.front().tRel);
                        else
                            skLo = std::max(skLo, B.ends.back());
                    }
                    continue;
                }
                ++L.nDueEval;
                std::vector<uint8_t> &upNow = L.upScratch;
                upNow.assign(B.mem.size(), 0);
                uint32_t up = 0;
                for (size_t m = 0; m < B.mem.size(); ++m)
                {
                    const LMember &mb = B.mem[m];
                    // (2026-10-20) the same intervals memberState accepts: a train member is up until arrival (tA), a retiree
                    // from it — with t >= tRel / t < tEnd alone, a batch arriving that frame counted members up that
                    // memberState refused (no record written, never shown: selftest launches_schedule_and_phasing).
                    upNow[m] = (kind == 0 ? (t >= mb.tRel && t < B.tA) : (t >= B.tA && t < mb.tEnd)) &&
                               LaunchImpl::memberAllowed(*this, B, mb);
                    up += upNow[m];
                }
                if (up == 0 && ch.empty())
                {
                    B.nUp[kind] = 0;
                    skOk = false; // (nobody allowed: the Kessler roster, not t, decides)
                    continue;
                }
                const uint32_t need = (uint32_t)((B.mem.size() + kChunk - 1) / kChunk);
                if (ch.size() < need)
                {
                    if (L.freeChunks.size() < need - ch.size())
                    {
                        skOk = false;
                        continue; // the block is full: this batch waits
                    }
                    while (ch.size() < need)
                    {
                        ch.push_back(L.freeChunks.back());
                        L.freeChunks.pop_back();
                    }
                    if (!B.inChunked)
                    {
                        B.inChunked = true;
                        L.chunked.push_back(&B);
                    }
                    L.liveCh[bi] = 1;
                    // Its need interval (freeBatch): the skipped freeing pass must still see it end.
                    {
                        const double b0 = kind == 0 ? B.tD : B.tA;
                        const double b1 = kind == 0 ? B.tA : B.tA + 400.0 * kDay;
                        L.freeLo = std::max(L.freeLo, b0);
                        L.freeHi = std::min(L.freeHi, b1);
                    }
                    const uint64_t owner = touched[bi] * 2 + (uint64_t)kind;
                    for (uint32_t m = 0; m < (uint32_t)B.mem.size(); ++m)
                    {
                        const uint32_t slot = ch[m / kChunk] * kChunk + m % kChunk;
                        L.slots[slot].owner = owner;
                        L.slots[slot].m = (uint16_t)m;
                    }
                }
                bool complete = true;
                for (uint32_t m = 0; m < (uint32_t)B.mem.size(); ++m)
                {
                    const uint32_t slot = ch[m / kChunk] * kChunk + m % kChunk;
                    SlotInfo &si = L.slots[slot];
                    if (!upNow[m])
                    {
                        if (si.shown || si.live)
                            complete &= hideSlot(slot);
                        continue;
                    }
                    if (si.shown && std::abs(t - si.tw) <= every)
                        continue;
                    if (budget == 0)
                    {
                        complete = false;
                        continue;
                    }
                    if (!writeMember(B, kind, m, slot))
                        complete = false; // not written this frame: the batch stays due (it had counted it as done,
                                          // and the member stayed hidden until its next 20-s rewrite)
                }
                B.nUp[kind] = up;
                if (complete)
                {
                    B.tw[kind] = t;
                    notDueSpan(kind, t, every, relEnd);
                    (kind == 0 ? skT : skR) += up;
                }
                else
                    skOk = false;
                (kind == 0 ? nT : nR) += up;
            }
            L.liveSkip[bi] = skOk ? LaunchState::LiveSkip{skLo, skHi, skT, skR} : LaunchState::LiveSkip{};
        }
        L.nTrain = nT;
        L.nRetire = nR;
        if (nb)
            L.cursor = (L.cursor + 97) % nb;
    }
    L.profAdd(3, pt);
    L.prof[8] += (writeMs - L.prof[8]) / 30.0;
    L.active = std::any_of(L.slots.begin(), L.slots.end(), [](const SlotInfo &s) { return s.shown; });
    L.profAdd(4, pt);
    if (copies.empty())
        return;
    vkCmdCopyBuffer(cmd, L.stage, kesslerBuf, (uint32_t)copies.size(), copies.data());
    VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = kesslerBuf;
    bmb.offset = 4ull * kLaunchWord0;
    bmb.size = 64ull * kLaunchCap;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &bmb, 0, nullptr);
}

uint64_t SatelliteSim::launchSlotOwner(uint32_t slot, uint32_t &m) const
{
    m = 0;
    if (!launchState || slot >= launchState->slots.size())
        return 0;
    const SlotInfo &si = launchState->slots[slot];
    m = si.m;
    return si.live ? si.owner : 0;
}

// A launch record's elements on the CPU (meshes, follow, the selection's readouts). (2026-10-26, issue 20261026_205544)
// For a train member or a retiree they are its OSCULATING orbit at the current sim time, rebuilt from its exact path
// (memberRecord), not the record last written: that is a Kepler orbit, and the path (CW free flight eased into its
// own, the climb's ramp, the fall) leaves it between the rewrites — every 2 s / 20 s of SIM time, i.e. 3-30 times a
// second of wall time at 1 min/s, so the meshes up close jumped at every rewrite. The GPU's point keeps the written
// record (sub-pixel at any range it is drawn as a point).
SatOrbitElems SatelliteSim::launchOrbitElems(uint32_t slot) const
{
    if (!launchState || slot >= launchState->elems.size())
        return SatOrbitElems{};
    const LaunchState &L = *launchState;
    if (LaunchImpl::gExactElems && slot < L.slots.size())
    {
        const SlotInfo &si = L.slots[slot];
        if (si.live && si.owner != 0)
        {
            auto it = L.batches.find(si.owner / 2);
            if (it != L.batches.end() && si.m < it->second.mem.size())
            {
                const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
                GpuKepOrbit g{};
                SatOrbitElems ce{};
                if (LaunchImpl::memberRecord(*this, it->second, it->second.mem[si.m], (int)(si.owner % 2), t, t, g, ce))
                    return ce;
            }
        }
    }
    return L.elems[slot];
}

void SatelliteSim::launchExactElems(bool on)
{
    LaunchImpl::gExactElems = on;
}

std::string SatelliteSim::launchLabel(uint32_t slot) const
{
    if (slot == kLaunchCap - 1)
        return transientLabel();
    if (slot == kLaunchCap - 2)
        return followTrKey ? transientLabelOf(followTrCur) : std::string("Re-entering object");
    if (!launchState || slot >= launchState->slots.size() || launchState->slots[slot].owner == 0)
        return "?";
    const LaunchState &L = *launchState;
    const SlotInfo &si = L.slots[slot];
    auto it = L.batches.find(si.owner / 2);
    if (it == L.batches.end() || si.m >= it->second.mem.size())
        return "?";
    const LBatch &B = it->second;
    const uint32_t sat = B.mem[si.m].sat;
    const SatOrbit &o = satOrbits[sat];
    char b[200];
    const char *cn = o.constIdx < constellations.size() ? constellations[o.constIdx].name.c_str() : "Satellite";
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (si.owner % 2 == 0)
        snprintf(b, sizeof(b), "%s #%d (%s, raising; %s, arrives in %.1f d)", cn,
                 (int)sat - (int)constellations[o.constIdx].orbitStart, B.first ? "first deployment" : "new",
                 L.cat.vehicles[B.veh].name.c_str(), (B.tA - t) / kDay);
    else
        snprintf(b, sizeof(b), "%s #%d (retired, re-enters in %.1f d)", cn,
                 (int)sat - (int)constellations[o.constIdx].orbitStart, (B.mem[si.m].tEnd - t) / kDay);
    return b;
}

// The selection panel's second line for a launch record (formatSelectedSatInfo, refreshed every frame while one is
// selected): the satellite's type and what it is doing now, e.g. "Starlink V2 Mini - raising to slot, 412/565 km".
// Empty when the slot is not a train member or a retiree (the panel then shows the type alone).
std::string SatelliteSim::launchStatus(uint32_t slot) const
{
    if (!launchState || slot >= kLaunchCap - 2 || slot >= launchState->slots.size() || launchState->slots[slot].owner == 0)
        return {};
    const LaunchState &L = *launchState;
    const SlotInfo &si = L.slots[slot];
    auto it = L.batches.find(si.owner / 2);
    if (it == L.batches.end() || si.m >= it->second.mem.size())
        return {};
    const LBatch &B = it->second;
    const LMember &m = B.mem[si.m];
    const SatOrbit &o = satOrbits[m.sat];
    const char *tn = o.typeIdx < satTypes.size() ? satTypes[o.typeIdx].name.c_str() : "Satellite";
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double hKm = (glm::length(satOrbitStateAt(launchOrbitElems(slot), t).posEci) - kR) / 1000.0;
    char b[200];
    if (si.owner % 2 == 1)
        snprintf(b, sizeof(b), "%s - deorbiting, %.0f km", tn, hKm);
    else if (t < m.tRel)
        snprintf(b, sizeof(b), "%s - in the dispenser, %.0f km", tn, hKm);
    else if (t < m.sRaise)
        snprintf(b, sizeof(b), "%s - parking orbit, %.0f km", tn, hKm);
    else
        snprintf(b, sizeof(b), "%s - %sraising to slot, %.0f/%.0f km", tn, B.first ? "first deployment, " : "", hKm,
                 (m.rSlot - kR) / 1000.0);
    return b;
}

// A launch record that ends (hideSlot) while it is the selection or in the 3D view: a train member that arrived IS its
// roster slot now (same place, same model), so both move there; a retiree's selection waits for its re-entry fireball
// (selWant*, picked up in selTransientUpdate). Anything else ends the selection, as before.
void SatelliteSim::launchSlotEndedSelection(uint32_t slot)
{
    const int idx = (int)(activeSatCount + slot);
    const bool sel = selectedSatIndex == idx, view = viewerSatIndex == idx;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The owner as the slot holds it (hideSlot has just cleared `live`, so launchSlotOwner would say "free").
    const uint64_t owner = launchState && slot < launchState->slots.size() ? launchState->slots[slot].owner : 0;
    const uint32_t m = owner ? launchState->slots[slot].m : 0;
    uint32_t sat = UINT32_MAX;
    const LBatch *B = nullptr;
    if (owner != 0 && launchState)
    {
        auto it = launchState->batches.find(owner / 2);
        if (it != launchState->batches.end() && m < it->second.mem.size())
        {
            B = &it->second;
            sat = B->mem[m].sat;
        }
    }
    char lb[200];
    snprintf(lb, sizeof(lb), "launch slot %u ended (selected %d, viewed %d): owner kind %d, %.0f s from arrival", slot,
             (int)sel, (int)view, owner ? (int)(owner % 2) : -1, B ? t - B->tA : 0.0);
    Log::line(lb);
    int to = -1;
    if (B && owner % 2 == 0 && sat < activeSatCount && t >= B->tA - 60.0)
        to = (int)sat;
    else if (B && owner % 2 == 1 && sat < activeSatCount && sel)
    {
        selWantKind = 3;
        selWantA = sat;
        selWantUntil = t + transientGraceS() + 5.0;
        selWantViewer = view;
    }
    if (sel)
    {
        selectedSatIndex = to;
        formatSelectedSatInfo();
    }
    if (view && to >= 0)
    {
        viewerSatIndex = to;
        viewerLastSelected = selectedSatIndex;
        viewerSyncModel();
        snprintf(viewerTitle, sizeof(viewerTitle), "%s", satLabel(to).c_str());
        viewerObsNextWall = 0.0;
    }
}

// ── Visuals: rockets, flames, plumes, re-entries ────────────────────────────────────────────────
void SatelliteSim::launchFillVisuals(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    FPROF_SCOPE("launchFillVisuals");
    auto pt = std::chrono::steady_clock::now();
    launchFillPadLights(); // the pads' lights at night, whether or not anything flies (2026-10-19)
    if (!launchState || !launchesEnabled || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    L.profAdd(5, pt);
    struct ProfEnd
    {
        LaunchState &L;
        std::chrono::steady_clock::time_point &pt;
        ~ProfEnd() { L.profAdd(6, pt); }
    } profEnd{L, pt};
    L.nRockets = L.nPlumes = L.nReentries = 0;
    L.nFairings = 0;
    L.plumeDbg.clear();
    L.plumeGrpDbg.assign(L.visList.size(), LaunchState::PlumeGrpDbg{});
    L.plasmaDbg.clear();
    L.stackDraws.clear();
    if (!L.loaded)
        return;
    launchFillShipLights(); // the landing ships' lights at night (2026-10-20)
    launchFillShipGlow(out, n, toSky, pixFull); // ... and their floodlit decks
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const glm::dvec3 sunD = glm::normalize(glm::dvec3(sunDirECI));
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    auto earthBlocks = [&](const glm::dvec3 &P) {
        const glm::dvec3 rel = P - eye;
        const double range = glm::length(rel);
        const double b = glm::dot(eye, rel / range), c = glm::dot(eye, eye) - kR * kR;
        const double disc = b * b - c;
        return disc > 0.0 && (-b - std::sqrt(disc)) > 0.0 && (-b - std::sqrt(disc)) < range - 50.0;
    };
    auto sunlit = [&](const glm::dvec3 &P) {
        const double sp = glm::dot(P, sunD);
        return sp > 0.0 || glm::length(P - sp * sunD) > kR + 15000.0;
    };
    struct Cand
    {
        double pri;
        GpuDustCloud d;
        int grp = -1;      // the launch (visList index) it belongs to
        double key = 0.0;  // what the budget sorts by: pri, boosted for each launch's reserved share (below)
        double res = 0.0;  // its weight as part of that share (0 .. 1: the share's last ranks fade)
    };
    // (2026-10-26, issue 20261026_193912) each launch's count of candidates this frame, for the reserved share
    static std::vector<int> grpCount;
    grpCount.assign(L.visList.size(), 0);
    int curGrp = -1;
    // The view cone a Gaussian (centre relENU from the eye, extent R m) can touch: the shader skips it elsewhere.
    auto coneOf = [&](const glm::dvec3 &relEnu, double R) {
        const double d = glm::length(relEnu);
        if (R >= 0.95 * d)
            return glm::vec4(0.0f, 0.0f, 0.0f, -2.0f);
        return glm::vec4(glm::vec3(relEnu / d), (float)std::sqrt(1.0 - (R / d) * (R / d)));
    };
    // Whether the Earth hides a sphere of radius R about P entirely from the eye.
    auto earthHides = [&](const glm::dvec3 &P, double R) {
        const glm::dvec3 rel = P - eye;
        const double d = glm::length(rel);
        const glm::dvec3 u = rel / d;
        const double tc = -glm::dot(eye, u);
        if (tc <= 0.0 || tc >= d)
            return false;
        const double miss = glm::length(eye + tc * u);
        return miss < kR - R;
    };
    static std::vector<Cand> plumes; // (kept: its capacity is reused frame to frame; main thread only)
    plumes.clear();
    L.plumeSkipped = 0;
    double plumeMs = 0.0;
    double reMs = 0.0, buildMs = 0.0;
    // The plumes' light (cloud_march.comp: phases per steradian) in the sky's units: the sky and the clouds use
    // phase functions normalised to a MEAN of 1 (phaseR / phaseM / cv2HG: 4 pi per steradian), so a plume's light is
    // x 4 pi to stand beside them (2026-10-20: by day a thick sunlit trail or steam cloud was ~13x too dim, as dim as
    // the haze behind it - invisible from a few km). The night factor (10) was tuned on the twilight bells before
    // this; it is kept, as the night value of the same factor.
    const double nightBoost = glm::mix(10.0, 4.0 * glm::pi<double>(), dayness);
    const double plumeUnitK = 4.0 * glm::pi<double>();
    const double pixA = ctx_ ? 2.0 * std::tan(0.5 * glm::radians((double)camera.fovYDeg)) /
                                   (double)std::max(1u, ctx_->swapExtent.height)
                             : 1e-3;
    // ── Entry plasma close up (2026-10-21, the validation review: a capsule's glow from ~40 m was a large saturated
    // round blob at its heat shield, Starship's hull a dark silhouette in front of its sheath, Dream Chaser's glow on
    // its body axis). One emitter for the ship, the capsules and the spaceplane: an emissive Gaussian whose light is,
    // unresolved, the point's on-screen energy (`Jpt`, toSky units: what the point carried), and resolved (its middle
    // sigma past ~1.5-8 px), a SURFACE BRIGHTNESS in the sky's units (pi x radiance per unit solar irradiance: a sunlit
    // white face is 1), geometric blend between the two as the flames do. The resolved brightness is
    //   - held at its face-on value whatever the view: seen edge-on a thin layer's column is its length, and seen along
    //     a wake its whole length adds up (flames: "held at its side-on surface brightness");
    //   - capped at kPlasmaMaxDisp of displayed light (x the sky exposure): the shock layer's ~4x a white face is
    //     hundreds of times past white under the night exposure, where a Gaussian saturates out to several sigma (the
    //     round disc). Capped, the layer saturates only in its own thin core;
    //   - faded as the eye comes inside it (3 -> 1.5 sigma), like the flames (a camera behind a capsule sits in its wake).
    // `holdAxis` = the axis whose face-on (column along it) brightness is held: the layer's thin axis, or for a wake one
    // of its cross axes (its side-on value).
    const double plasmaExpo = (double)skyExposure();
    constexpr double kPlasmaMaxDisp = 2.0;
    auto plasmaGauss = [&](const glm::dvec3 &cEci, const glm::dvec3 (&ax3)[3], const double (&s3)[3], int holdAxis,
                           double Jpt, double Sres, glm::vec3 col, double rel) {
        if (n >= kMaxDustClouds || !(Jpt > 0.0) || !(Sres > 0.0))
            return;
        const glm::dvec3 relC = cEci - eye;
        const double rangeC = std::max(glm::length(relC), 1e-3);
        const double sMin = 0.6 * rangeC * 2.0 * pixFull;
        glm::dvec3 A[3];
        double s[3], sr[3];
        for (int i = 0; i < 3; ++i)
        {
            A[i] = toEnu(ax3[i]);
            s[i] = std::max(s3[i], sMin);
            sr[i] = s3[i];
        }
        std::sort(sr, sr + 3);
        const double resK = glm::smoothstep(1.5, 8.0, 2.0 * sr[1] / std::max(rangeC * pixA, 1e-12));
        const glm::dvec3 ce = toEnu(relC);
        auto quad = [&](const glm::dvec3 &v) {
            double q = 0.0;
            for (int i = 0; i < 3; ++i)
                q += glm::dot(v, A[i]) * glm::dot(v, A[i]) / (s[i] * s[i]);
            return q;
        };
        const double inK = glm::smoothstep(1.5, 3.0, std::sqrt(quad(ce)));
        if (inK <= 0.0)
            return;
        const glm::dvec3 dv = -ce / std::max(glm::length(ce), 1e-9); // from the centre toward the eye
        const double hold = std::min(1.0, s[holdAxis] * std::sqrt(std::max(quad(dv), 1e-30)));
        double sFace = 1.0; // the face-on area factor: 2 pi x the two sigmas across the held axis
        for (int i = 0; i < 3; ++i)
            if (i != holdAxis)
                sFace *= s[i];
        // (rel: a wake's brightness relative to the layer's, the cap taken on the layer's: their ratio kept)
        const double Sc = rel * std::min(Sres, kPlasmaMaxDisp / std::max(plasmaExpo, 1e-6));
        const double cRes = Sc * 2.0 * glm::pi<double>() * sFace * hold;
        const double w = std::exp(glm::mix(std::log(Jpt), std::log(std::max(cRes, 1e-30)), resK)) * inK;
        glm::dmat3 M(0.0);
        for (int i = 0; i < 3; ++i)
            M += glm::outerProduct(A[i], A[i]) / (s[i] * s[i]);
        if (L.plasmaDbg.size() < 64)
            L.plasmaDbg.push_back({rangeC, resK, Sres, Sc * plasmaExpo, w, hold, sr[0], sr[2]});
        GpuDustCloud &d = out[n++];
        d.c = glm::vec4(glm::vec3(ce), (float)w);
        d.ia = glm::vec4((float)M[0][0], (float)M[1][1], (float)M[2][2], (float)(1.0 / (kTwoPi1p5 * s[0] * s[1] * s[2])));
        d.ib = glm::vec4((float)M[0][1], (float)M[0][2], (float)M[1][2], 0.0f);
        d.col = glm::vec4(col, 1.0f);
        d.cone = coneOf(ce, 5.5 * std::max(s[0], std::max(s[1], s[2])));
    };
    // The plasma's light on its own hull (sat_mesh.frag's local lights), physical (not lifted at night, not adapted):
    // the irradiance the layer gives the face under it is ~ its surface brightness (a bright layer filling half the
    // sky above the face: S / 2 .. S), capped like the layer so the lit hull displays at most ~as bright as the glow.
    auto plasmaHullLight = [&](const glm::dvec3 &posEci, double Sres, double dSurf, double rs, glm::vec3 col, double share) {
        const double Sc = std::min(Sres, kPlasmaMaxDisp / std::max(plasmaExpo, 1e-6));
        const double E = 1.5 * Sc * share;
        if (E > 1e-6)
            meshLightAdd(posEci, E * (dSurf * dSurf + rs * rs), col, rs, nullptr, 180.0, true);
    };
    // Crew spacecraft (2026-10-20): flying, docked or coming home. Listed (selectable, drawn as meshes up close) within
    // 4000 km or when selected / followed; sunlit and free-flying, a faint point that gives way to its mesh; a small
    // flame in its burns; the plasma's glow and wake through its entry.
    L.nCrew = 0;
    for (const auto &cv : L.crewList)
    {
        const uint64_t key = cv.first;
        const LBatch &B = *cv.second;
        const LCrew &C = B.crew;
        const uint64_t ckey = (key << 3) ^ 3ull;
        // (2026-10-21) A cargo ship that burns up (Progress, Tianzhou): its re-entry fireball from the entry interface,
        // along its entry's ground track (the retirees' and stages' model, fbDrawReentry; selectable as kind 9).
        if (C.burns && C.abort != 4 && t >= C.tEI && t < C.tEI + 200.0)
        {
            const glm::dvec3 P1 = LaunchImpl::crewEntryPos(C, C.tEI), P0 = LaunchImpl::crewReturnPos(C, C.tEI - 2.0);
            const glm::dvec3 U0 = glm::normalize(P1);
            if (glm::length(kR * U0 - eye) < 3.0e6 || transientWanted(9, key))
            {
                glm::dvec3 A = (P1 - P0) - glm::dot(P1 - P0, U0) * U0;
                A = glm::length(A) > 1.0 ? glm::normalize(A) : C.As;
                const double m0 = -4.0 - 2.5 * std::log10(std::max(C.massKg, 100.0) / 100.0);
                fbDrawReentry(U0, A, m0, (int)std::clamp(3.0 + 40.0 * std::sqrt(C.massKg / 2500.0), 3.0, 80.0),
                              B.seed ^ 0xC4A6011ull, t - C.tEI, out, n, toSky, pixFull, 9, key);
                ++L.nReentries;
            }
            continue;
        }
        // (2026-10-21c) A runway lander's jettisoned cargo module (Dream Chaser's Shooting Star) burns up a little
        // behind it: its fireball from the entry interface (the same ellipse, 25 s later).
        if (C.runway && C.abort != 4 && t >= C.tEI + 25.0 && t < C.tEI + 225.0)
        {
            const glm::dvec3 P1 = LaunchImpl::crewEntryPos(C, C.tEI), P0 = LaunchImpl::crewReturnPos(C, C.tEI - 2.0);
            const glm::dvec3 U0 = glm::normalize(P1);
            if (glm::length(kR * U0 - eye) < 3.0e6 || transientWanted(9, key))
            {
                glm::dvec3 A = (P1 - P0) - glm::dot(P1 - P0, U0) * U0;
                A = glm::length(A) > 1.0 ? glm::normalize(A) : C.As;
                const double mMod = 3500.0; // Shooting Star with its trash (~3.5 t)
                const double m0 = -4.0 - 2.5 * std::log10(mMod / 100.0);
                fbDrawReentry(U0, A, m0, (int)std::clamp(3.0 + 40.0 * std::sqrt(mMod / 2500.0), 3.0, 80.0),
                              B.seed ^ 0x5C0071Aull, t - C.tEI - 25.0, out, n, toSky, pixFull, 9, key);
                ++L.nReentries;
            }
        }
        glm::dvec3 P;
        int ph = 0;
        uint8_t burn = 0;
        if (!LaunchImpl::crewPos(C, t, P, &ph, &burn))
            continue;
        const glm::dvec3 rel = P - eye;
        const double range = std::max(glm::length(rel), 1.0);
        if (range > 4.0e6 && ckey != selTransientKey && ckey != followTrKey)
            continue;
        SelTransient st;
        if (!LaunchImpl::crewFill(*this, L, B, key, t, st))
            continue;
        st.dirEnu = glm::vec3(toEnu(rel) / range);
        st.flare = 0.0f;
        const size_t idx = selTransients.size();
        addSelTransient(st);
        ++L.nCrew;
        if (earthBlocks(P))
            continue;
        const double ext = atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
        double flare = 0.0;
        glm::vec3 col(0.95f, 0.95f, 0.98f);
        glm::dvec3 ptRel = rel; // where its point is drawn (the entry: the heat shield)
        if (ph == CP_ENTRY)
        {
            const double te = t - C.tEI, s = glm::clamp(te / LaunchImpl::kCrewEntryS, 0.0, 1.0);
            const double v = C.vEI * std::pow(1.0 - s, 1.5) + 120.0 * s;
            const double h = LaunchImpl::crewEntryH(C, s);
            const double I = std::min(std::pow(v / 7500.0, 3.0) * std::exp(-(h - 70000.0) / 8000.0), 3.0) *
                             glm::smoothstep(0.0, 20.0, te) * (1.0 - glm::smoothstep(0.7, 1.0, s));
            if (I > 1e-3)
            {
                const double m = -1.5 - 2.5 * std::log10(I) + 5.0 * std::log10(range / 1.0e5) + ext;
                if (!earthBlocks(P))
                    ambUpwellSource(2, toEnu(rel), range, m);
                const double fl = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.95 * dayness);
                col = glm::mix(glm::vec3(1.0f, 0.62f, 0.38f), glm::vec3(1.0f, 0.55f, 0.75f), (float)glm::clamp(v / 7500.0, 0.0, 1.0));
                // (2026-10-21) Where the plasma is: a capsule's shock layer stands just ahead of its heat shield
                // (heat_shield_m below the port along -Z, radius heat_shield_r_m); a spaceplane's (belly_half_len_m > 0:
                // Dream Chaser, flying nose-high) along its tiled belly. The point sits there and gives way, as the mesh
                // resolves, to the layer (thin, hugging the face), a short hot wake behind the body and the layer's light
                // on the hull (plasmaGauss / plasmaHullLight above). It was a 3.5-m round Gaussian at the shield in the
                // point's units: from ~40 m a saturated disc several times the capsule (and the spaceplane's sat on its
                // body axis).
                const launch::Program &pgE = L.cat.programs[B.prog];
                const glm::dmat3 Ra = LaunchImpl::crewAttitude(C, t);
                glm::dvec3 vdirE = -Ra[2];
                {
                    glm::dvec3 Pv;
                    if (LaunchImpl::crewPos(C, t - 0.5, Pv) && glm::length(P - Pv) > 1e-3)
                        vdirE = glm::normalize(P - Pv);
                }
                const bool belly = pgE.bellyHalfLen > 0.0;
                const double Rs = std::max(pgE.heatShieldR, 0.3);
                const glm::dvec3 nW = belly ? -Ra[1] : -Ra[2]; // the windward face's normal (out into the flow)
                const glm::dvec3 Fc = belly ? P - Ra[2] * pgE.heatShieldM - Ra[1] * Rs : P - Ra[2] * pgE.heatShieldM;
                const double standoff = belly ? 0.35 : 0.12 * Rs + 0.1;
                const glm::dvec3 Ph = Fc + nW * standoff;
                ptRel = Ph - eye;
                const double keep = 1.0 - glm::smoothstep(2.0, 8.0, rocketMeshPx(selTransients[idx], pixA, eye));
                flare = fl * keep;
                const double dayKE = std::max(1.0 - 0.95 * dayness, 1e-3);
                const double Jpt = fl / dayKE * range * range / K * toSky * dayKE; // the point's on-screen energy
                const double Sres = 4.0 * I; // the layer's surface brightness: ~4x a sunlit white face at peak heating
                if (keep < 1.0)
                {
                    const glm::dvec3 ax3[3] = {Ra[0], belly ? Ra[2] : Ra[1], nW};
                    const double s3[3] = {belly ? 1.2 * Rs : 0.55 * Rs, belly ? 0.42 * pgE.bellyHalfLen : 0.55 * Rs,
                                          belly ? 0.3 : 0.06 * Rs + 0.05};
                    plasmaGauss(Ph, ax3, s3, 2, 0.6 * Jpt * (1.0 - keep), Sres, col, 1.0);
                    // The hot wake close behind the body: the gas the shock heated, flowing round it (an eighth of the
                    // layer's brightness: the first cut at 0.3 drew a lit sheet several capsules long).
                    const double bodyL = belly ? 2.0 * pgE.bellyHalfLen : pgE.heatShieldM + Rs;
                    glm::dvec3 u1 = glm::cross(vdirE, Ra[0]);
                    u1 = glm::length(u1) > 1e-6 ? glm::normalize(u1) : Ra[1];
                    const glm::dvec3 u2 = glm::cross(vdirE, u1);
                    const double sw = belly ? 1.2 * Rs : 0.8 * Rs, sl = 0.8 * bodyL + Rs;
                    const glm::dvec3 axW[3] = {vdirE, u1, u2};
                    const double sW[3] = {sl, sw, sw};
                    plasmaGauss(Fc - vdirE * (0.5 * bodyL + sl), axW, sW, 1, 0.25 * Jpt * (1.0 - keep), Sres,
                                glm::mix(col, glm::vec3(1.0f, 0.45f, 0.3f), 0.5f), 0.12);
                    // Its light on the hull: the face under the layer and what stands round it.
                    if (belly)
                        for (int k = -1; k <= 1; k += 2)
                            plasmaHullLight(Fc + nW * 1.5 + Ra[2] * (0.45 * pgE.bellyHalfLen * k), Sres, 1.5, 1.6, col,
                                            0.6 * (1.0 - keep));
                    else
                        plasmaHullLight(Fc + nW * (0.8 * Rs), Sres, 0.8 * Rs, Rs, col, 1.0 - keep);
                }
                // Its glowing wake: the last 4 s of path. Resolved (it is ~80 m wide) a dim trail at its side-on surface
                // brightness, capped like the layer; unresolved the point's energy as before.
                glm::dvec3 Pp;
                if (fl > 5e-3 && LaunchImpl::crewPos(C, t - 4.0, Pp))
                {
                    const glm::dvec3 axis = P - Pp;
                    const double Lw = glm::length(axis);
                    const glm::dvec3 a = axis / std::max(Lw, 1.0);
                    glm::dvec3 b2 = glm::cross(a, glm::normalize(P));
                    b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                    const glm::dvec3 c2 = glm::cross(a, b2);
                    const glm::dvec3 axF[3] = {a, b2, c2};
                    // (centred 0.6 of the path back, sigma 0.22: at 0.55 / 0.28 its 3-sigma tail reached ~a third of the
                    // path AHEAD of the capsule)
                    const double sF[3] = {0.22 * Lw, 80.0, 80.0};
                    plasmaGauss(Ph - 0.6 * axis, axF, sF, 1, 0.6 * Jpt, Sres, col, 0.15);
                }
            }
        }
        else if (burn != launch::BURN_NONE)
        {
            // Its thrusters / engine: a small warm point (a few hundred newtons to a few kN).
            const double m = 2.0 + 5.0 * std::log10(range / 1.0e5) + ext;
            flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.9 * dayness);
            col = glm::vec3(1.0f, 0.82f, 0.62f);
        }
        else if (ph != CP_DOCKED && ph != CP_BERTHED && sunlit(P))
        {
            // Sunlit: a white body of some 20-30 m^2, giving way to its mesh as that resolves.
            double m = 3.8 + 5.0 * std::log10(range / 1.0e6) + ext;
            const double keep = 1.0 - glm::smoothstep(2.0, 8.0, rocketMeshPx(selTransients[idx], pixA, eye));
            if (keep > 0.0)
            {
                m -= 2.5 * std::log10(keep);
                flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.97 * dayness);
            }
        }
        if (flare > 2e-4)
        {
            selTransients[idx].flare = (float)flare;
            const double pr = std::max(glm::length(ptRel), 1.0);
            if (kFireCount < kFireballMax)
                fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(ptRel) / pr), (float)flare), glm::vec4(col, (float)pr)};
        }
    }
    for (size_t li = 0; li < L.visList.size(); ++li)
    {
        const uint64_t key = L.visList[li].first;
        LBatch &B = *L.visList[li].second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        curGrp = (int)li;
        // Retirees re-entering: their fireballs.
        bool anyRe = false;
        if (t >= B.tA && !B.first)
        {
            auto it = std::lower_bound(B.ends.begin(), B.ends.end(), t - 200.0);
            anyRe = it != B.ends.end() && *it <= t;
        }
        if (anyRe)
        {
        const auto reT0 = std::chrono::steady_clock::now();
            for (size_t mi = 0; mi < B.mem.size(); ++mi)
            {
                const LMember &mb = B.mem[mi];
                const double tau = t - mb.tEnd;
                if (tau < 0.0 || tau > 200.0 || !LaunchImpl::memberAllowed(*this, B, mb))
                    continue;
                glm::dvec3 r, v;
                if (!LaunchImpl::memberState(*this, B, mb, 1, mb.tEnd - 1.0, r, v))
                    continue;
                const glm::dvec3 U0 = glm::normalize(r);
                if (glm::length(kR * U0 - eye) > 3.0e6 && !transientWanted(3, mb.sat))
                    continue;
                glm::dvec3 A = v - glm::dot(v, U0) * U0;
                A = glm::length(A) > 1.0 ? glm::normalize(A) : glm::dvec3(1, 0, 0);
                const SatOrbit &so = satOrbits[mb.sat];
                const double mass = so.typeIdx < satTypes.size() ? LaunchImpl::launchTypeMass(satTypes[so.typeIdx]) : 600.0;
                const double m0 = -4.0 - 2.5 * std::log10(std::max(mass, 1.0) / 100.0);
                const int N = (int)std::clamp(3.0 + 40.0 * std::sqrt(std::max(mass, 1.0) / 2500.0), 3.0, 80.0);
                fbDrawReentry(U0, A, m0, N, B.seed ^ ((uint64_t)mb.sat * 0x9E3779B97F4A7C15ull), tau, out, n, toSky, pixFull,
                              3, mb.sat);
                ++L.nReentries;
            }
            reMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - reT0).count();
        }
        if (t < B.tL - 600.0 || t > B.tVisEnd)
            continue;
        const double tau = t - B.tL;
        // The upper stage's own re-entry.
        if (vh.upperDeorbit && t >= B.tStageRE && t < B.tStageRE + 200.0)
        {
            glm::dvec3 P, Pm;
            if (LaunchImpl::rocketPos(L, B, 1, B.tStageRE - B.tL - 1.0, P) &&
                LaunchImpl::rocketPos(L, B, 1, B.tStageRE - B.tL - 2.0, Pm))
            {
                const glm::dvec3 U0 = glm::normalize(P);
                if (glm::length(kR * U0 - eye) < 3.0e6 || transientWanted(4, key))
                {
                    // Along its entry ellipse's ground track (horizontal part of its velocity).
                    glm::dvec3 A = (P - Pm) - glm::dot(P - Pm, U0) * U0;
                    A = glm::length(A) > 1.0 ? glm::normalize(A) : B.A0;
                    const double m0 = -4.0 - 2.5 * std::log10(vh.upperMassKg / 100.0);
                    fbDrawReentry(U0, A, m0, (int)std::clamp(3.0 + 40.0 * std::sqrt(vh.upperMassKg / 2500.0), 3.0, 80.0),
                                  B.seed ^ 0x51A6E5ull, t - B.tStageRE, out, n, toSky, pixFull, 4, key);
                    ++L.nReentries;
                }
            }
        }
        // A returning upper stage (Starship's ship) re-enters whole: an ~8-min plasma glow along its orbit, descending
        // from 100 to ~45 km and slowing from 7.5 to ~1.2 km/s, brightest near 70 km. (It is not followed to landing.)
        if (!vh.upperDeorbit && t >= B.tStageGone && t < B.tStageGone + 480.0)
        {
            auto shipAt = [&](double te) { return LaunchImpl::shipEntry(B, te, nullptr, nullptr); };
            const double te = t - B.tStageGone;
            double v = 0.0, h = 0.0;
            const glm::dvec3 P = LaunchImpl::shipEntry(B, te, &v, &h), Pp = shipAt(te - 6.0);
            const double I = std::min(std::pow(v / 7500.0, 3.0) * std::exp(-(h - 70000.0) / 8000.0), 3.0) *
                             glm::smoothstep(0.0, 30.0, te) * (1.0 - glm::smoothstep(420.0, 480.0, te));
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 1.0);
            // Listed for the selection (and its mesh) whether glowing or not; pickable once drawn.
            size_t shipIdx = SIZE_MAX;
            if (range < 4.0e6 || ((key << 3) ^ 0x2ull) == selTransientKey || key == launchFocusKey ||
                ((key << 3) ^ 0x2ull) == followTrKey) // (the selected / followed / framed one at any range)
            {
                SelTransient st;
                // The upper stage's own key: a selected or followed ship stays selected into its entry.
                st.key = (key << 3) ^ 0x2ull;
                st.stage = 1;
                st.kind = 2;
                st.a = key;
                st.tau = te;
                const glm::dvec3 P2 = shipAt(te - 12.0);
                st.pos = P;
                st.vel = (P - Pp) / 6.0;
                st.acc = (P - 2.0 * Pp + P2) / 36.0;
                // Belly first: the nose pitched ~60 degrees above the velocity, the heat shield into the flow.
                const glm::dvec3 vh1 = glm::normalize(P - Pp), up = glm::normalize(P);
                glm::dvec3 upp = up - glm::dot(up, vh1) * vh1;
                upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
                st.axis = glm::normalize(0.5 * vh1 + 0.866 * upp);
                auto ti = rocketTypes.find("rocket_" + vh.id + "_upper");
                st.type = ti != rocketTypes.end() ? ti->second : -1;
                st.dirEnu = glm::vec3(toEnu(rel) / range);
                st.flare = 0.0f;
                shipIdx = selTransients.size();
                addSelTransient(st);
            }
            // (2026-10-21) Where the plasma is: the shock layer on the WINDWARD side, the heat-shield belly, along the
            // body's middle, not the track point (the model's origin = the aft end, at the engines: the glow and its
            // glare sat on the engine bells of a followed ship). The belly faces down and forward, perpendicular to the
            // body axis in the plane of the axis and the velocity.
            glm::dvec3 Pg = P, Fb = P, ax(0.0), nb(0.0), vdS = glm::normalize(P - Pp);
            double bodyHalf = 25.0, bodyR = 4.5, meshPx = 0.0;
            {
                const glm::dvec3 vh1 = glm::normalize(P - Pp), up = glm::normalize(P);
                glm::dvec3 upp = up - glm::dot(up, vh1) * vh1;
                upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
                ax = glm::normalize(0.5 * vh1 + 0.866 * upp);
                nb = vh1 - glm::dot(vh1, ax) * ax;
                nb = glm::length(nb) > 1e-6 ? glm::normalize(nb) : -upp;
                auto ti = rocketTypes.find("rocket_" + vh.id + "_upper");
                const SatMeshRenderer::TypeMesh *tm = ti != rocketTypes.end() ? meshRenderer.typeMesh(ti->second) : nullptr;
                double zc = 25.0;
                if (tm && tm->boundsRadius > 1.0f)
                {
                    zc = (double)tm->boundsCenter.z;
                    bodyHalf = std::max(2.0, (double)tm->boundsRadius - 2.0);
                }
                bodyR = std::clamp(0.17 * bodyHalf, 1.5, 6.0);
                Fb = P + ax * zc + nb * bodyR; // the belly's middle, on the hull
                Pg = Fb + nb * 0.6;            // the shock layer, standing just off it
                if (shipIdx < selTransients.size())
                    meshPx = rocketMeshPx(selTransients[shipIdx], pixA, eye);
            }
            const glm::dvec3 relG = Pg - eye;
            const double rangeG = std::max(glm::length(relG), 1.0);
            // The plasma's light on the cloud tops below (faint: it is 45-100 km up).
            if (I > 1e-3)
                engineLightAdd(Pg, flameIntensitySunsM2(-4.0 - 2.5 * std::log10(I)),
                               glm::mix(glm::vec3(1.0f, 0.62f, 0.38f), glm::vec3(1.0f, 0.55f, 0.75f),
                                        (float)glm::clamp(v / 7500.0, 0.0, 1.0)),
                               400.0, glm::normalize(P), 0.0, 0.0, false, 0.0);
            if (I > 1e-3 && !earthBlocks(P))
            {
                const double m = -4.0 - 2.5 * std::log10(I) + 5.0 * std::log10(range / 1.0e5) +
                                 atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
                ambUpwellSource(2, toEnu(rel), range, m);
                const double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.95 * dayness);
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.62f, 0.38f), glm::vec3(1.0f, 0.55f, 0.75f),
                                               (float)glm::clamp(v / 7500.0, 0.0, 1.0));
                // The point gives way, as the ship's mesh resolves, to a sheath of glow along its belly (a point of
                // this brightness on a resolved hull was a white disc with glare spikes): the capsule's rule.
                const double keep = 1.0 - glm::smoothstep(2.0, 8.0, meshPx);
                if (flare * keep > 2e-4 && kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(relG) / rangeG), (float)(flare * keep)), glm::vec4(col, (float)rangeG)};
                if (flare > 2e-4 && shipIdx < selTransients.size())
                    selTransients[shipIdx].flare = (float)flare;
                // (2026-10-21, validation) The sheath, the wake close behind the body and the plasma's light on the hull,
                // through the capsules' emitter (plasmaGauss: resolved, a held and capped surface brightness): the sheath
                // was in the point's units (a wide white ellipse), and the hull read as a dark silhouette in front of it.
                const double dayKS = std::max(1.0 - 0.95 * dayness, 1e-3);
                const double Jpt = flare / dayKS * range * range / K * toSky * dayKS;
                const double Sres = 4.0 * I;
                if (keep < 1.0)
                {
                    const glm::dvec3 side = glm::normalize(glm::cross(ax, nb));
                    const glm::dvec3 ax3[3] = {ax, side, nb};
                    const double s3[3] = {0.4 * bodyHalf, 0.75 * bodyR, 0.45};
                    plasmaGauss(Pg, ax3, s3, 2, 0.6 * Jpt * (1.0 - keep), Sres, col, 1.0);
                    glm::dvec3 u1 = glm::cross(vdS, side);
                    u1 = glm::length(u1) > 1e-6 ? glm::normalize(u1) : nb;
                    const glm::dvec3 u2 = glm::cross(vdS, u1);
                    const double sl = 0.6 * bodyHalf, sw = 1.2 * bodyR;
                    const glm::dvec3 axW[3] = {vdS, u1, u2};
                    const double sW[3] = {sl, sw, sw};
                    plasmaGauss(Fb - vdS * (0.5 * bodyHalf + sl), axW, sW, 1, 0.2 * Jpt * (1.0 - keep), Sres,
                                glm::mix(col, glm::vec3(1.0f, 0.45f, 0.3f), 0.5f), 0.12);
                    // Three lights under the belly: the tiles, the flaps and the body's flanks glow orange / pink.
                    for (int k = -1; k <= 1; ++k)
                        plasmaHullLight(Fb + nb * 2.0 + ax * (0.55 * bodyHalf * k), Sres, 2.0, 2.5, col, 0.5 * (1.0 - keep));
                }
                // Its glowing wake: the last 6 s of path (300 m wide): resolved, a dim trail at its side-on brightness.
                if (flare > 5e-3)
                {
                    const glm::dvec3 axis = P - Pp;
                    const double Lw = glm::length(axis);
                    const glm::dvec3 a = axis / std::max(Lw, 1.0);
                    glm::dvec3 b2 = glm::cross(a, glm::normalize(P));
                    b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                    const glm::dvec3 c2 = glm::cross(a, b2);
                    const glm::dvec3 axF[3] = {a, b2, c2};
                    const double sF[3] = {0.22 * Lw, 300.0, 300.0};
                    plasmaGauss(Pg - 0.6 * axis, axF, sF, 1, 0.7 * Jpt, Sres, col, 0.15);
                }
                ++L.nReentries;
            }
        }
        // The payload fairing's halves (2026-10-20): separated at ~110 km, they fall away tumbling. Listed (selectable,
        // drawn as meshes up close) and, sunlit, a faint point each.
        if (B.tFair > 0.0 && t >= B.tFair && t < B.tFair + 1300.0)
        {
            auto fi = rocketTypes.find("rocket_" + vh.id + "_fairing");
            if (fi != rocketTypes.end())
            {
                LaunchImpl::ensureFairing(L, B, vh.fairingCenter);
                for (int h = 0; h < 2; ++h)
                {
                    glm::dvec3 P, V;
                    glm::dmat3 R;
                    if (!LaunchImpl::fairingAt(B, h, t, P, V, R))
                        continue;
                    const glm::dvec3 rel = P - eye;
                    const double range = std::max(glm::length(rel), 1.0);
                    const uint64_t fkey = (key << 3) ^ (uint64_t)(5 + h);
                    if (range > 4.0e6 && fkey != selTransientKey && fkey != followTrKey)
                        continue;
                    ++L.nFairings;
                    SelTransient st;
                    st.key = fkey;
                    st.kind = 7;
                    st.stage = (uint8_t)h;
                    st.a = key;
                    st.tau = t - B.tFair;
                    glm::dvec3 P1, V1;
                    glm::dmat3 R1;
                    st.pos = P;
                    st.vel = V;
                    st.acc = LaunchImpl::fairingAt(B, h, t - 1.0, P1, V1, R1) ? V - V1 : glm::dvec3(0.0);
                    st.axis = R[2];
                    st.type = fi->second;
                    st.dirEnu = glm::vec3(toEnu(rel) / range);
                    st.flare = 0.0f;
                    st.hasAtt = true;
                    st.attQ = glm::normalize(glm::quat_cast(R));
                    st.attOmega = B.fair[h].w;
                    st.attT0 = t;
                    const size_t idx = selTransients.size();
                    addSelTransient(st);
                    if (earthBlocks(P) || !sunlit(P))
                        continue;
                    // Sunlit: a white half-shell of some 60 m^2, about as bright as a coasting stage; it gives way to
                    // its mesh as that resolves.
                    double m = 4.3 + 5.0 * std::log10(range / 1.0e6) +
                               atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
                    ambUpwellSource(1, toEnu(rel), range, m);
                    const double px = rocketMeshPx(selTransients[idx], pixA, eye);
                    const double keep = 1.0 - glm::smoothstep(2.0, 8.0, px);
                    if (keep <= 0.0)
                        continue;
                    m -= 2.5 * std::log10(keep);
                    const double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.97 * dayness);
                    if (flare < 2e-4)
                        continue;
                    selTransients[idx].flare = (float)flare;
                    if (kFireCount < kFireballMax)
                        fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare),
                                            glm::vec4(0.95f, 0.95f, 0.98f, (float)range)};
                }
            }
        }
        // Rockets: the flame (a point and, close up, an emissive Gaussian) or, coasting, the sunlit body.
        for (int stage = 0; stage < 2; ++stage)
        {
            glm::dvec3 P;
            uint8_t burn = 0;
            if (!LaunchImpl::rocketPos(L, B, stage, tau, P, &burn))
                continue;
            if (stage == 1 && tau < vh.t1)
                continue; // still one vehicle: drawn as the booster
            if (stage == 0 && tau > vh.t1 + 5.0 && B.rec == 0 && burn == launch::BURN_NONE &&
                glm::length(P) - kR < 40000.0)
                continue;
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 1.0);
            // Listed for the selection whether drawn or not (a booster coasting in the dark, behind the Earth: a
            // tracked rocket stays selected); pickable only once drawn (flare > 0).
            size_t trIdx = SIZE_MAX;
            // (and the launch the last jump framed, launchFocusKey: a traced launch's upper stage is ~5000 km downrange
            // by its deployment, and `launch select kind=upper` found nothing; 2026-10-21)
            if (glm::length(P - eye) < 4.0e6 || ((key << 3) ^ (uint64_t)(stage + 1)) == selTransientKey ||
                ((key << 3) ^ (uint64_t)(stage + 1)) == followTrKey || key == launchFocusKey)
            {
                SelTransient st;
                st.key = (key << 3) ^ (uint64_t)(stage + 1);
                // The body axis (nose): the Earth-fixed velocity on the track (in ECI the pad's ~400 m/s of rotation
                // would tilt a rising rocket), straight up on the pad; a booster after staging and an upper stage
                // in its deorbit burn fly engines first. In orbit, the inertial velocity.
                {
                    // (stageNose: smoothed; the booster's flip, the upper stage's turn to retrograde for its deorbit
                    // burn and a ship's pitch-up to its entry attitude are slews, never snaps.)
                    glm::dvec3 ax = LaunchImpl::stageNose(L, B, stage, tau);
                    if (glm::length(ax) < 0.5)
                        ax = glm::normalize(P);
                    // On a ship's deck it stands on the deck as the ship rolls and pitches (2026-10-21).
                    if (stage == 0 && B.landKind == kLandShip && tau >= LaunchImpl::boosterTr(L, B).tEnd)
                        ax = ecefToEci(LaunchImpl::shipFrame(B, t).up, t);
                    st.axis = ax;
                    st.type = stage == 1 ? LaunchImpl::upperStageType(*this, L, B, t)
                                         : [&] {
                                               auto ti = rocketTypes.find("rocket_" + vh.id + (tau < vh.t1 ? "_stack" : "_booster"));
                                               return ti != rocketTypes.end() ? ti->second : -1;
                                           }();
                }
                st.kind = 1;
                st.stage = (uint8_t)stage;
                st.burn = burn;
                st.a = key;
                st.tau = tau;
                glm::dvec3 P1, P2;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 0.5, P1))
                    P1 = P;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 1.0, P2))
                    P2 = P1;
                st.pos = P;
                st.vel = (P - P1) / 0.5;
                st.acc = (P - 2.0 * P1 + P2) / 0.25;
                st.dirEnu = glm::vec3(toEnu(rel) / range);
                st.flare = 0.0f;
                trIdx = selTransients.size();
                addSelTransient(st);
                // Its payload stack, visible once the fairing is gone (2026-10-20): drawn by launchMeshJobs.
                const launch::Stack &stk = L.cat.programs[B.prog].stack;
                if (stage == 1 && B.tFair > 0.0 && t >= B.tFair && t < B.tStageGone &&
                    (stk.layout == launch::Stack::Flat || stk.layout == launch::Stack::Column))
                {
                    LaunchState::StackDraw sd;
                    sd.key = key;
                    sd.t = t;
                    sd.P = st.pos;
                    sd.V = st.vel;
                    sd.A = st.acc;
                    sd.S = LaunchImpl::upperFrame(L, B, tau);
                    sd.type = st.type;
                    L.stackDraws.push_back(sd);
                }
            }
            // The burning engines as a light on the clouds, fog, haze, plumes and ground (2026-10-20; engineLightAdd):
            // listed before the horizon test (a rocket below the eye's horizon still lights the clouds above it).
            if (burn != launch::BURN_NONE && engineLightGain > 0.0f)
            {
                double fmL = stage == 0 ? vh.flameMag1 : vh.flameMag2;
                if (burn == launch::BURN_LANDING)
                    fmL += 1.5;
                else if (burn == launch::BURN_BOOSTBACK || burn == launch::BURN_ENTRY)
                    fmL += 0.7;
                glm::dvec3 nose = glm::normalize(P);
                if (trIdx < selTransients.size() && glm::length(selTransients[trIdx].axis) > 0.5)
                    nose = glm::normalize(selTransients[trIdx].axis);
                glm::dvec3 egL(0.0, 2.0, 40.0);
                {
                    const int ty = trIdx < selTransients.size() ? selTransients[trIdx].type : -1;
                    if (ty >= 0 && ty < (int)satTypes.size())
                    {
                        auto it = L.engineGeom.find(ty);
                        if (it == L.engineGeom.end())
                            it = L.engineGeom.emplace(ty, LaunchImpl::engineGeomOf(satTypes[(size_t)ty])).first;
                        egL = it->second;
                    }
                }
                const double DL = 2.0 * egL.y;
                const double hiL = glm::smoothstep(20000.0, 90000.0, glm::length(P) - kR);
                const double Lf = DL * 6.0 * (1.0 + 2.5 * hiL); // ~ the flame's core + body (launchFillVisuals below)
                const glm::dvec3 Pl = P + egL.x * nose - 0.4 * Lf * nose;
                const glm::vec3 fc = glm::mix(glm::vec3(1.0f, 0.82f, 0.62f), glm::vec3(1.0f, 0.6f, 0.3f), (float)vh.soot);
                // The light the clouds see: the flame's colour toward the deep orange of launch photographs (the lit exhaust
                // and the cloud's own multiple scattering redden it), sootier flames deeper.
                const glm::vec3 lc = glm::mix(fc, glm::mix(glm::vec3(1.0f, 0.45f, 0.15f), glm::vec3(1.0f, 0.38f, 0.08f),
                                                           (float)vh.soot), 0.85f);
                // On the meshes (sat_mesh.frag) the light comes from just behind the nozzles: the flame's brightest
                // part, above the deck it is about to land on (0.4 Lf down it stood under the deck at touchdown).
                const glm::dvec3 PlM = P + egL.x * nose - 0.12 * Lf * nose;
                engineLightAdd(Pl, flameIntensitySunsM2(fmL), lc,
                               std::max(25.0, 0.5 * Lf), nose, std::min(18.0 * DL, 130.0) + 0.4 * Lf, 1.5 * egL.y + 3.0,
                               false, 1.0, &PlM);
            }
            if (earthBlocks(P))
                continue;
            ++L.nRockets;
            const double ext = atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
            const bool burning = burn != launch::BURN_NONE;
            double m;
            glm::vec3 col;
            if (burning)
            {
                double fm = stage == 0 ? vh.flameMag1 : vh.flameMag2;
                if (burn == launch::BURN_LANDING)
                    fm += 1.5;
                else if (burn == launch::BURN_BOOSTBACK || burn == launch::BURN_ENTRY)
                    fm += 0.7;
                m = fm + 5.0 * std::log10(range / 1.0e5) + ext - 2.5 * std::log10(std::max((double)launchRocketGain, 1e-3));
                col = glm::mix(glm::vec3(1.0f, 0.82f, 0.62f), glm::vec3(1.0f, 0.6f, 0.3f), (float)vh.soot);
                ambUpwellSource(0, toEnu(rel), range, m); // the music's upwell (SatelliteSimAmbience.cpp)
            }
            else
            {
                if (!sunlit(P) || tau < 0.0)
                    continue; // on the pad, or dark: nothing to see from afar
                // Landed (in the tower, on its zone, on a ship's deck): scenery, its mesh only (a sunlit point on the
                // sea or the ground read as a star).
                if (stage == 0 && tau >= LaunchImpl::boosterTr(L, B).tEnd)
                    continue;
                m = (stage == 0 ? 3.0 : 3.8) + 5.0 * std::log10(range / 1.0e6) + ext;
                col = glm::vec3(0.95f, 0.95f, 0.98f);
                ambUpwellSource(1, toEnu(rel), range, m); // before the mesh fade: the body, not its point
                // The sunlit body gives way to its mesh as that resolves (a burning stage keeps its flame's point).
                if (trIdx < selTransients.size())
                {
                    // Gone once the mesh is up: floored at 1e-4 (+10 mag) a point of magnitude 3 at 1000 km seen
                    // from 100 m (-17) still drew a blinding white disc on the hull (the user's QA shot 6).
                    const double px = rocketMeshPx(selTransients[trIdx], pixA, eye);
                    const double keep = 1.0 - glm::smoothstep(2.0, 8.0, px);
                    if (keep <= 0.0)
                        continue;
                    m -= 2.5 * std::log10(keep);
                }
            }
            double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - (burning ? 0.8 : 0.97) * dayness);
            if (flare < 2e-4)
                continue;
            if (trIdx < selTransients.size())
                selTransients[trIdx].flare = (float)flare; // drawn: pickable
            if (!burning)
            {
                // The sunlit body's point at the middle of the stage (it sat on the track point, the aft end: half a
                // stage off its mesh while the two cross-fade).
                glm::dvec3 relB = rel;
                if (trIdx < selTransients.size() && glm::length(selTransients[trIdx].axis) > 0.5)
                {
                    const int ty = selTransients[trIdx].type;
                    if (ty >= 0 && ty < (int)satTypes.size())
                    {
                        auto it = L.engineGeom.find(ty);
                        if (it == L.engineGeom.end())
                            it = L.engineGeom.emplace(ty, LaunchImpl::engineGeomOf(satTypes[(size_t)ty])).first;
                        relB += (it->second.x + 0.5 * it->second.z) * glm::normalize(selTransients[trIdx].axis);
                    }
                }
                const double rB = std::max(glm::length(relB), 1.0);
                if (kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(relB) / rB), (float)flare), glm::vec4(col, (float)rB)};
                continue;
            }
            // The flame, from the engines out (2026-10-19, the user's QA: "a plume emerging from the engine", not a
            // white cloud round the vessel). Its geometry is the model's: the nozzle exit plane and the radius the
            // exits span (engineGeomOf), the nose axis (out of the engines: a landing burn's flame points down).
            glm::dvec3 vdir;
            {
                glm::dvec3 Pb;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 0.5, Pb))
                    Pb = P;
                vdir = P - Pb;
                vdir = glm::length(vdir) > 1e-3 ? glm::normalize(vdir) : glm::normalize(P);
                if (trIdx < selTransients.size() && glm::length(selTransients[trIdx].axis) > 0.5)
                    vdir = selTransients[trIdx].axis;
            }
            glm::dvec3 eg(0.0, 2.0, 40.0);
            {
                const int ty = trIdx < selTransients.size() ? selTransients[trIdx].type : -1;
                if (ty >= 0 && ty < (int)satTypes.size())
                {
                    auto it = L.engineGeom.find(ty);
                    if (it == L.engineGeom.end())
                        it = L.engineGeom.emplace(ty, LaunchImpl::engineGeomOf(satTypes[(size_t)ty])).first;
                    eg = it->second;
                }
            }
            const glm::dvec3 Pexit = P + eg.x * vdir;
            const double D = 2.0 * eg.y;
            const double hAlt = glm::length(P) - kR;
            // The flame lengthens and widens as the air thins (the exhaust under-expanded at altitude).
            const double hiK = glm::smoothstep(20000.0, 90000.0, hAlt);
            // A chain of three Gaussians: the bright core at the nozzle, the flame, its fading tail. Each starts far
            // enough behind the exit that its forward tail stays inside the bell (the old flame was one Gaussian
            // 950 m long centred half its length behind the stage: it reached past the nose).
            const double lenK[3] = {1.2, 3.5, 9.0}, widK[3] = {0.75, 1.1, 1.8}, shareK[3] = {0.5, 0.32, 0.18};
            double offK[3], sAk[3], sPk[3];
            for (int k = 0; k < 3; ++k)
            {
                const double len = D * lenK[k] * (1.0 + 2.5 * hiK);
                sAk[k] = 0.42 * len;
                sPk[k] = eg.y * widK[k] * (1.0 + 2.0 * hiK);
                offK[k] = k == 0 ? 2.2 * sAk[0] : offK[k - 1] + 1.2 * (sAk[k - 1] + sAk[k]);
            }
            const double Lcore = offK[1] + sAk[1];
            const double Lpx = Lcore / std::max(range * pixA, 1e-9);
            // The point (bloom, glare) sits just behind the nozzle exits, on the engine cluster (2026-10-20, issue
            // 20261020_181510: it sat on the core Gaussian's centre, ~33 m behind a Starship's exits in vacuum, and
            // seen from behind and aside the glare stood ~300 px off the engines). Once the flame resolves, a point's
            // 1/r^2 no longer holds: its brightness stops growing where the flame is ~4 px long, and most of the light
            // goes to the flame's Gaussians (a glare remains at every scale).
            {
                const glm::dvec3 relC = (Pexit - 0.08 * D * vdir) - eye;
                const double rC = std::max(glm::length(relC), 1.0);
                const double rEff = std::max(rC, Lcore / (4.0 * pixA));
                const double pointK = glm::mix(1.0, 0.3, glm::smoothstep(4.0, 30.0, Lpx));
                double fP = flare * (range / rEff) * (range / rEff) * pointK;
                // ...and its glare stays modest once the flame is resolved (2026-10-20, issue 20261019_222938 shot 2:
                // from 200 m under a Super Heavy the point was ~2e4, a glare and bloom over the whole screen; "a
                // smaller glare with all 33 engines lit up is the ideal look"). Geometric blend toward a flare of 14
                // (just past the glare threshold) as the flame grows from 4 to 30 px; the lit nozzles (the mesh,
                // sat_mesh.frag) and the flame's Gaussians carry the light.
                {
                    const double capK = glm::smoothstep(4.0, 30.0, Lpx);
                    if (fP > 14.0 && capK > 0.0)
                        fP = std::exp(glm::mix(std::log(fP), std::log(14.0), capK));
                }
                if (fP > 2e-4 && kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(relC) / rC), (float)fP), glm::vec4(col, (float)rC)};
            }
            if (n < kMaxDustClouds && flare > 2e-3)
            {
                const double sMin = 0.6 * range * 2.0 * pixFull;
                const glm::dvec3 a = toEnu(vdir);
                glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
                b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                const glm::dvec3 c2 = glm::cross(a, b2);
                // Its light: unresolved, the point's on-screen energy (toSky: a Gaussian of magnitude m carries what a
                // star of magnitude m shows, ~10 mag over a surface's physical brightness); resolved, its surface
                // brightness in the sky's own units (pi x radiance per unit solar irradiance: a sunlit white face is 1)
                // x kFlameCore, the core's excess over the point magnitudes' average (tuned for points). Without the
                // hand-over a flame a few hundred metres long seen from a kilometre was a blown-out white sheet over
                // the vessel even by day (the user's QA shots 5-6).
                // ...x 0.18 at night (2026-10-20): the night exposure is ~5.5x the day's, and a resolved flame's
                // Gaussians, hundreds of times past white, saturated out to 3-4 sigma: a round white blob several
                // times the engine cluster's size (issue 20261019_222938 shot 2). As if the eye adapted to the flame.
                const double kFlameCore = 30.0 * (1.0 - 0.82 * (1.0 - dayness));
                const double dayK = 1.0 - 0.8 * dayness;
                const double J = 0.6 * flare / std::max(dayK, 1e-3) * range * range / K;
                const double corePx = 2.0 * sPk[0] / std::max(range * pixA, 1e-9);
                const double resK = glm::smoothstep(1.5, 8.0, corePx);
                const double unitK = toSky * dayK * std::pow(glm::pi<double>() * kFlameCore / std::max(toSky * dayK, 1e-30), resK);
                for (int k = 0; k < 3 && n < kMaxDustClouds; ++k)
                {
                    const double sA = std::max(sAk[k], sMin), sP = std::max(sPk[k], sMin);
                    const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
                    auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    const glm::dvec3 ce = toEnu(Pexit - offK[k] * vdir - eye);
                    // Faded out as the eye comes inside it (its Mahalanobis radius from 3 to 1.5 sigma): from under a
                    // rising stage, in its exhaust, the tail's Gaussian lit every direction at a third of its core's
                    // brightness - a white screen (issue 20261019_222938 shot 2).
                    const double mA = glm::dot(ce, a) / sA, mP2 = (glm::dot(ce, ce) - glm::dot(ce, a) * glm::dot(ce, a)) / (sP * sP);
                    const double inK = glm::smoothstep(1.5, 3.0, std::sqrt(mA * mA + std::max(mP2, 0.0)));
                    if (inK <= 0.0)
                        continue;
                    // Resolved and seen along its axis (from under a rising stage), a Gaussian's whole length adds up in
                    // each pixel: the flame was a white disc over the engines. Its surface brightness is held at its
                    // side-on value (x sP sqrt(q), q the inverse variance along the line of sight), and from the
                    // exhaust side, within ~40 deg of the axis, it thins further, so the lit nozzles show through
                    // (sat_mesh.frag; "a smaller glare with all 33 engines lit up").
                    double viewK = 1.0;
                    {
                        const double rc = std::max(glm::length(ce), 1e-3);
                        const double cz = glm::dot(-ce / rc, a); // + = the eye on the nose side
                        const double qv = cz * cz / (sA * sA) + (1.0 - cz * cz) / (sP * sP);
                        const double endK = glm::smoothstep(0.75, 0.97, -cz);
                        viewK = glm::mix(1.0, std::min(1.0, sP * std::sqrt(qv)) * glm::mix(1.0, 0.15, endK), resK);
                    }
                    GpuDustCloud &d = out[n++];
                    d.c = glm::vec4(glm::vec3(ce), (float)(J * shareK[k] * unitK * inK * viewK));
                    d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                                     (float)(1.0 / (kTwoPi1p5 * sA * sP * sP)));
                    d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
                    // The core white-hot, the tail the exhaust's colour.
                    d.col = glm::vec4(glm::mix(glm::vec3(1.0f, 0.95f, 0.88f), col, 0.5f * (float)k), 1.0f);
                    d.cone = coneOf(ce, 5.5 * std::max(sA, sP));
                }
                // The exhaust the flame lights: a night effect (by day the lit exhaust is lost in the sky; up close it
                // drew a white ellipse hundreds of metres across over the rocket), scaled with the engines, behind them.
                if (n < kMaxDustClouds)
                {
                    // 12 exit diameters low down (30 drew a 270-m ellipse round a Super Heavy whose lit column filled
                    // the screen from a few km), growing with the expanding exhaust aloft.
                    const double Rg = D * (12.0 + 318.0 * hiK);
                    const double gA = std::max(1.6 * Rg, sMin), gP = std::max(Rg, sMin);
                    const double jA = 1.0 / (gA * gA), jP = 1.0 / (gP * gP);
                    auto Mg = [&](int p, int q) { return jA * a[p] * a[q] + jP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    const glm::dvec3 ce = toEnu(Pexit - 2.0 * gA * vdir - eye);
                    // Gone as the eye comes within a few of its lengths: from inside (under a rising stage, in its
                    // exhaust) it was a white haze over the whole screen (issue 20261019_222938 shot 2).
                    // (0.12 of the flame, was 0.25: from a few km at night it saturated into a column taller than the rocket.)
                    const double nearK = glm::smoothstep(2.0 * gA, 6.0 * gA, glm::length(ce));
                    GpuDustCloud &g = out[n++];
                    g.c = glm::vec4(glm::vec3(ce), (float)(0.12 * J * unitK * (1.0 - 0.95 * dayness) * nearK));
                    g.ia = glm::vec4((float)Mg(0, 0), (float)Mg(1, 1), (float)Mg(2, 2),
                                     (float)(1.0 / (kTwoPi1p5 * gA * gP * gP)));
                    g.ib = glm::vec4((float)Mg(0, 1), (float)Mg(0, 2), (float)Mg(1, 2), 0.0f);
                    g.col = glm::vec4(glm::mix(col, glm::vec3(1.0f, 0.5f, 0.2f), 0.5f), 1.0f);
                    g.cone = coneOf(ce, 5.5 * gA);
                }
            }
        }
        // Plumes: built (once) only for launches that can be seen from here, and not past 1 h/s of warp (a plume lasts
        // about an hour: building one costs a few ms, and at a day a second a dozen launches pass each second).
        // (2026-10-20) Built on a worker thread (LaunchState::parcelJobs), from up to 2 min before lift-off, so it is
        // ready when the rocket lifts off; a plume asked for later appears a few frames after it is first wanted.
        // "Plume growth" changed since these parcels were built (2026-10-21): build them again.
        if (B.parcelsBuilt && (B.plumeGrow != launchPlumeGrowth || B.plumeDisp != launchPlumeDispersal))
        {
            B.parcelsBuilt = false;
            B.parcels.clear();
        }
        // Potato draws no plume (no cloud composite): no parcels either (2026-10-29).
        if (launchPlumeGain <= 0.0f || potatoSkyOn() || tau < -120.0 || (tau < 0.0 && B.parcelsBuilt))
            continue;
        struct PlumeTimer
        {
            double &acc;
            std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
            ~PlumeTimer() { acc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
        } plumeTimer{plumeMs};
        if (!B.parcelsBuilt)
        {
            FPROF_SCOPE("launch.plumeUnbuilt");
            const bool fast = !timePaused && kTimeScales[timeScaleIdx] > 3600.0f;
            const glm::dvec3 padNow = ecefToEci(B.G0, t) * kR;
            const double eyeAlt = glm::length(eye) - kR;
            double dNear = glm::length(padNow - eye);
            glm::dvec3 Pst;
            if (LaunchImpl::rocketPos(L, B, 1, tau, Pst))
                dNear = std::min(dNear, glm::length(Pst - eye)); // (a stage venting half an orbit downrange)
            if (fast || dNear > 4.5e6 + 2.0 * eyeAlt)
                continue;
            const auto bt0 = std::chrono::steady_clock::now();
            const uint64_t jobKey = L.visList[li].first;
            auto job = L.parcelJobs.find(jobKey);
            if (job == L.parcelJobs.end())
            {
                if (L.parcelJobs.size() < 4)
                {
                    const LaunchState *Lc = &L;
                    B.plumeGrow = launchPlumeGrowth; // (the copy below carries it to the worker)
                    B.plumeDisp = launchPlumeDispersal;
                    L.parcelJobs.emplace(jobKey, std::async(std::launch::async, [Lc, Bc = B]() mutable {
                                             LaunchImpl::buildParcels(*Lc, Bc);
                                             return std::move(Bc.parcels);
                                         }));
                }
                buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bt0).count();
                continue;
            }
            if (job->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
                continue;
            B.parcels = job->second.get();
            B.parcelsBuilt = true;
            L.parcelJobs.erase(job);
            buildMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - bt0).count();
        }
        if (tau < 0.0)
            continue;
        const double era = earthRotationAngle(t);
        const double cEra = std::cos(era), sEra = std::sin(era); // (rotZ, once a batch rather than twice a Gaussian)
        auto rotE = [&](const glm::dvec3 &v) { return glm::dvec3(v.x * cEra - v.y * sEra, v.x * sEra + v.y * cEra, v.z); };
        int curGround = 0; // (the parcel being drawn: for the readout)
        // One Gaussian of exhaust: centre C and trail direction axE (Earth-fixed), the half-length it stands for along
        // the trail (hl: its 1-sigma extent beyond its own radius), radius sig, cross-section Sigma (m^2).
        auto addPlume = [&](const glm::dvec3 &CE, const glm::dvec3 &axE, double hl, double sig, double Sigma,
                            double sweep, double albedo, double age, uint64_t rimSeed, bool trail,
                            const glm::vec3 &tint) {
            // The exhaust tuning sliders (2026-10-21; all 1 = the look before them): the ground clouds' density,
            // every Gaussian's radius (the same cross-section: a wider plume is thinner).
            if (curGround != 0)
                Sigma *= (double)launchGroundCloudGain;
            sig *= (double)launchPlumeWidth;
            if (Sigma < 1.0)
                return;
            const glm::dvec3 C = rotE(CE);
            const glm::dvec3 rel = C - eye;
            const double range = std::max(glm::length(rel), 1.0);
            const double ext5 = 5.5 * std::sqrt(sig * sig + hl * hl);
            if (range > 6.0e6 + ext5 || earthHides(C, ext5))
                return; // over the horizon
            // Elongated along the trail (a Gaussian stands for a stretch of the path).
            const glm::dvec3 ax = rotE(axE);
            const double sMin = 0.6 * range * 2.0 * pixFull;
            const double sP = std::max(sig, sMin);
            const double sA = std::max(std::sqrt(sig * sig + hl * hl), sMin);
            const double tauPeak = Sigma / (2.0 * glm::pi<double>() * sP * sA);
            const double pri = Sigma / (range * range) * (1.0 + std::min(tauPeak, 4.0));
            // (2026-10-21b, CPU) Far below last frame's cut it cannot be drawn (it would fade to nothing there): its
            // colour and rim are not worked out. Self-correcting: a frame whose cut fell past the margin turns it off.
            // (not before this launch has its reserved share of candidates: those are drawn whatever the cut)
            // (2026-10-26, issue 20261026_210053) and not one its launch's reserved share could hold: those are drawn
            // whatever the cut (the share is the launch's strongest by priority, not the first emitted: skipping by
            // emission count dropped some of them for a frame and not the next).
            if (pri < 0.25 * L.plumeCutPrev && curGrp >= 0 && pri < 0.25 * L.visList[(size_t)curGrp].second->plumeResPri)
            {
                ++L.plumeSkipped;
                return;
            }
            Cand c;
            c.pri = pri;
            c.grp = curGrp;
            if (curGrp >= 0 && (size_t)curGrp < L.plumeGrpDbg.size())
            {
                L.plumeGrpDbg[(size_t)curGrp].cand++;
                L.plumeGrpDbg[(size_t)curGrp].sigC += Sigma;
                L.plumeGrpDbg[(size_t)curGrp].rangeKm = range / 1000.0;
            }
            if (curGrp >= 0)
                ++grpCount[(size_t)curGrp];
            GpuDustCloud &d = c.d;
            const glm::dvec3 a = toEnu(ax);
            glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
            b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
            const glm::dvec3 c2 = glm::cross(a, b2);
            const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
            auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
            d.c = glm::vec4(glm::vec3(toEnu(rel)), (float)Sigma);
            d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                             (float)(1.0 / (kTwoPi1p5 * sA * sP * sP)));
            // The Sun's colour at the parcel (its path through the air; the Earth's shadow is the shader's) x albedo.
            const double h = glm::length(C) - kR;
            glm::dvec3 tr(1.0);
            const double kc[3] = {0.11, 0.2, 0.42};
            const double ext1 = atmExtinctionMag(C, sunD, 0.0, 1.0); // (linear in k: one column for the three)
            for (int ch = 0; ch < 3; ++ch)
                tr[ch] = std::pow(10.0, -0.4 * std::min(kc[ch] * ext1, 30.0));
            // Why twilight plumes are off-white to blue (the jellyfish, like noctilucent cloud):
            // (1) the Sun's grazing path crosses the ozone layer, whose Chappuis band takes green and orange out
            //     (cloud_v2_sunlut.comp's thin shell at 25 km: vertical optical depth 0.019 / 0.027 / 0.0016 at
            //     680 / 550 / 440 nm, x the secant where the ray crosses it). Counted ONCE, the secant capped at 12:
            //     the 15-35 km layer is thick, so a grazing ray's path through it is ~10-20 vertical columns, not 30
            //     twice; with both, the reddened low sunlight lost its green under the ice's blue and the plumes
            //     between the orange trail and the white bell came out magenta (the user's "odd purple trail").
            {
                const double rO = kR + 25000.0, r = glm::length(C), cz = glm::dot(C, sunD) / r;
                const double sin2 = (r / rO) * (r / rO) * (1.0 - cz * cz);
                const double sec = 1.0 / std::sqrt(std::max(1.0 - sin2, 1e-3));
                const double nX = r <= rO ? 1.0 : ((cz < 0.0 && sin2 < 1.0) ? 1.0 : 0.0);
                const double odO[3] = {0.019, 0.027, 0.0016};
                for (int ch = 0; ch < 3; ++ch)
                    tr[ch] *= std::exp(-odO[ch] * std::min(sec, 12.0) * nX);
            }
            // (2) high up the exhaust's ice is sub-micron: it scatters blue more than red (Angstrom exponent ~1.5,
            //     680 / 550 / 440 nm), normalised to keep the luminance; low down the droplets and soot are grey.
            //     The blue only where the sunlight still has its blue: ice lit by the reddened Sun is lit red.
            {
                const double ice = glm::smoothstep(30000.0, 100000.0, h) * glm::clamp(tr[2] / std::max(tr[0], 1e-6), 0.0, 1.0);
                const glm::dvec3 sp(0.727 / 0.9707, 1.0 / 0.9707, 1.398 / 0.9707);
                tr *= glm::mix(glm::dvec3(1.0), sp, ice);
            }
            const double sinE = glm::dot(glm::normalize(C), sunD);
            // The sky's light on it, relative (1 = a clear daytime sky), fading with the air above it.
            const double amb = glm::smoothstep(-0.1, 0.25, sinE) * std::exp(-std::max(h, 0.0) / 15000.0);
            // w = 4 x a per-parcel seed (cloud_march.comp's billows) + the sky light on it (unread there).
            const double seedK = (double)(launch::hash01(rimSeed, 7, 3) * 255.0);
            // At night the sky's exposure is not physical (only ~5.5x the day's: see the Kessler dust), so a sunlit
            // plume against the dark sky reads far dimmer than it is; its sunlight is raised as the eye's sky darkens.
            const glm::dvec3 sunC = tr * albedo * nightBoost * glm::dvec3(tint);
            // The blue sky's light on it, low down (2026-10-20: a steam cloud's shaded side is bluish grey, not black;
            // the engines' and pads' lights are the shader's engine lights, SatelliteSim::engineLightAdd). The CPU
            // then shadows the Sun's part itself and tells the shader the sky light's share of the colour (ib.w's
            // fraction: 2 + share; the shader lights that share isotropically, with no Earth shadow and half the
            // self-shadow). Above 25 km nothing: the bells keep the shader's per-pixel Earth shadow (their lit edge is
            // the terminator).
            double flameShare = -1.0;
            glm::dvec3 colOut = sunC;
            glm::dvec3 iso(0.0);
            if (h < 25000.0)
                iso += plumeUnitK * amb * 0.18 * glm::dvec3(0.62, 0.78, 1.0) * albedo * glm::dvec3(tint);
            const double lI = iso.x + iso.y + iso.z;
            if (lI > 1e-6)
            {
                const double sp = glm::dot(C, sunD), dS = glm::length(C - sp * sunD);
                const double litC = sp > 0.0 ? 1.0 : glm::smoothstep(kR * 0.9995, kR * 1.003, dS);
                const glm::dvec3 sc = sunC * litC;
                const double lS = sc.x + sc.y + sc.z;
                if (lI > 0.02 * lS)
                {
                    colOut = sc + iso;
                    flameShare = std::min(lI / (lI + lS), 0.999);
                }
            }
            d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2),
                             (float)(4.0 * std::floor(seedK) + (flameShare >= 0.0 ? 2.0 + flameShare : amb)));
            colOut *= (double)launchPlumeBright; // "Plume brightness" (the iso share in ib.w is a ratio: unchanged)
            d.col = glm::vec4(glm::vec3(colOut), (float)(2.0 + albedo));
            d.cone = coneOf(toEnu(rel), 5.5 * std::max(sA, sP));
            plumes.push_back(c);
            // High up, once the expanding exhaust has swept up about its own mass of air it piles up at its rim: a
            // shell, limb-brightened (the jellyfish's bell). Along a trail the rim is a tube, whose limb is two lines
            // beside the trail (2 Gaussians as long as the segment, so neighbours join); only a lone parcel, or a
            // trail seen end-on, shows a ring (7 Gaussians round the line of sight). Rings on every parcel drew the
            // trail as a string of circles, and the young vacuum trail as puffs.
            const double shellK = (h > 60000.0 && sig > 2000.0) ? 0.45 * glm::smoothstep(0.3, 2.0, sweep) : 0.0;
            if (shellK > 0.02)
            {
                const double coreShare = 1.0 - shellK;
                plumes.back().d.c.w = (float)(Sigma * coreShare);
                plumes.back().pri *= coreShare;
                const glm::dvec3 vE = toEnu(rel) / range; // the line of sight
                glm::dvec3 aP = a - glm::dot(a, vE) * vE;
                const double aPl = glm::length(aP);
                const bool tube = trail ? aPl > 0.3 : (aPl > 1e-3 && (sA * aPl) / sP > 1.6);
                glm::dvec3 r1, r2;
                if (tube)
                {
                    r1 = aP / aPl;
                    r2 = glm::cross(vE, r1);
                }
                else
                {
                    r1 = glm::cross(vE, glm::dvec3(0, 0, 1));
                    r1 = glm::length(r1) > 1e-6 ? glm::normalize(r1) : glm::dvec3(1, 0, 0);
                    r2 = glm::cross(vE, r1);
                }
                const int K = tube ? 2 : 7;
                const double rr = 0.9 * sig, sr = std::max(0.42 * sig, sMin);
                for (int k = 0; k < K; ++k)
                {
                    glm::dvec3 nrm, tg;
                    double sT;
                    if (tube)
                    {
                        nrm = k == 0 ? r2 : -r2;
                        tg = r1;
                        sT = std::max(std::sqrt(sig * sig + hl * hl * aPl * aPl), sMin);
                    }
                    else
                    {
                        const double ang = glm::two_pi<double>() * ((double)k + h01(rimSeed, (uint64_t)k, 31)) / K;
                        nrm = std::cos(ang) * r1 + std::sin(ang) * r2;
                        tg = -std::sin(ang) * r1 + std::cos(ang) * r2;
                        sT = std::max(0.9 * sig, sMin);
                    }
                    const glm::dvec3 off = rr * nrm;
                    const double sN = sr;
                    const double iT = 1.0 / (sT * sT), iN = 1.0 / (sN * sN), iV = 1.0 / (sig * sig);
                    auto Mr = [&](int p, int q) { return iT * tg[p] * tg[q] + iN * nrm[p] * nrm[q] + iV * vE[p] * vE[q]; };
                    Cand cr;
                    cr.pri = pri * shellK / K;
                    cr.grp = c.grp;
                    GpuDustCloud &dr = cr.d;
                    const glm::dvec3 ce = toEnu(rel) + off;
                    dr.c = glm::vec4(glm::vec3(ce), (float)(Sigma * shellK / K));
                    dr.ia = glm::vec4((float)Mr(0, 0), (float)Mr(1, 1), (float)Mr(2, 2),
                                      (float)(1.0 / (kTwoPi1p5 * sT * sN * sig)));
                    dr.ib = glm::vec4((float)Mr(0, 1), (float)Mr(0, 2), (float)Mr(1, 2), d.ib.w + (float)(4 * (k + 1)));
                    dr.col = d.col;
                    dr.cone = coneOf(ce, 5.5 * std::max(sT, sig));
                    plumes.push_back(cr);
                }
            }
            if (L.plumeDbg.size() < 4000)
                L.plumeDbg.push_back({range / 1000.0, h / 1000.0, sig / 1000.0, Sigma, tauPeak, age, tr * albedo, amb,
                                      curGround, colOut, flameShare});
        };
        // Each parcel's state now (age >= 0: emitted; ok: still drawn).
        struct PState
        {
            glm::dvec3 cE;
            double sig = 0.0, cond = 0.0, sweep = 0.0;
            bool ok = false;
        };
        static std::vector<PState> ps;
        ps.assign(B.parcels.size(), PState{});
        {
            FPROF_SCOPE("launch.plumeAt");
            for (size_t pi = 0; pi < B.parcels.size(); ++pi)
            {
                const double age = tau - B.parcels[pi].t0;
                ps[pi].ok = age >= 0.0 && B.parcels[pi].at(age, ps[pi].cE, ps[pi].sig, ps[pi].cond, &ps[pi].sweep);
            }
        }
        FPROF_SCOPE("launch.plumeEmit");
        const double gain = (double)launchPlumeGain;
        {
            LaunchState::PlumeGrpDbg &gd = L.plumeGrpDbg[li];
            gd.key = L.visList[li].first;
            gd.tPlus = tau;
            gd.parcels = (int)B.parcels.size();
            for (const PState &q : ps)
                gd.alive += q.ok ? 1 : 0;
        }
        // A burn's parcels are the knots of a polyline: each draws the stretch of trail since the knot before it
        // (centre the midpoint, axis and length the chord between the two as they are NOW, so the trail bends and
        // shears with the winds that carry its parcels), and the youngest knot connects to the burning stage's nozzle
        // (the head grows with the burn instead of a parcel appearing every interval: the trail used to arrive in
        // jumps, each new parcel a needle reaching ahead of the rocket - the user's "intermittent, squished" trail).
        // Old parcels are wide and overlap their neighbours, so a burn's knots are thinned: every 2nd stands for two,
        // every 4th for four ... up to 64, by WIDTH (2026-10-26, issue 20261026_193912: a vacuum bell's parcels are
        // ~150 km wide 90 s after their 1-s emission, 20 overlapping Gaussians where one does). Each level needs the
        // parcels 1.5x as wide as the stretch the survivor stands for, so no gaps.
        // (2026-10-26, issue 20261026_210053) CONTINUOUS: the level is lam = log2(width / 1.5 spacing), and a knot
        // that the next level drops slides onto the midpoint of the two knots that stay (its radius, condensed share
        // and sweep onto their mean) over the last 60% of lam's step, so when it is dropped the chord through it is
        // already the chord that replaces it (same cross-section: a chord sums its parcels). Stepped, half a trail's
        // Gaussians moved at once - a visible jump, in time warp a pop. The ground clouds' separate puffs (outside a
        // burn) were never dropped, only drawn x their step (Sigma x dec: an accidental 8-16x from 20 s of age, which
        // the ground clouds' look was tuned with); that factor is now continuous, 2^lam (wOf), and still capped at 16.
        static std::vector<PState> pb;
        static std::vector<uint8_t> decOf;
        static std::vector<float> wOf;
        pb = ps;
        decOf.assign(B.parcels.size(), 1);
        wOf.assign(B.parcels.size(), 1.0f);
        for (size_t pi = 0; pi < B.parcels.size(); ++pi)
        {
            const launch::Parcel &pc = B.parcels[pi];
            if (!ps[pi].ok || pc.thin)
                continue;
            const bool puff = pc.stream < 0;
            const double age = tau - pc.t0;
            const double wr = ps[pi].sig / std::max(pc.len, 1.0);
            double lam = std::log2(std::max(wr, 1e-6) / 1.5);
            // (from 20 s of age, a level per 4 s at most; up to every 64th: the trails that keep widening reach it)
            const double lamMax = puff ? 4.0 : 6.0;
            lam = std::min({lam, (age - 20.0) / 4.0, lamMax});
            if (lam <= 0.0)
                continue;
            const int Lv = std::min((int)std::floor(lam), (int)lamMax);
            const size_t dec = (size_t)1 << Lv;
            decOf[pi] = (uint8_t)dec;
            if (puff)
            {
                wOf[pi] = (float)std::exp2(lam);
                continue;
            }
            if (Lv >= 6 || pi % (2 * dec) != dec || pi < dec || pi + dec >= B.parcels.size())
                continue;
            const double g = glm::smoothstep(0.4, 1.0, lam - (double)Lv);
            const size_t ia = pi - dec, ib = pi + dec;
            if (g <= 0.0 || !ps[ia].ok || !ps[ib].ok || B.parcels[ia].stream != pc.stream ||
                B.parcels[ib].stream != pc.stream)
                continue;
            PState &q = pb[pi];
            q.cE = glm::mix(ps[pi].cE, 0.5 * (ps[ia].cE + ps[ib].cE), g);
            q.sig = glm::mix(ps[pi].sig, 0.5 * (ps[ia].sig + ps[ib].sig), g);
            q.cond = glm::mix(ps[pi].cond, 0.5 * (ps[ia].cond + ps[ib].cond), g);
            q.sweep = glm::mix(ps[pi].sweep, 0.5 * (ps[ia].sweep + ps[ib].sweep), g);
        }
        int prev = -1;
        for (size_t pi = 0; pi < B.parcels.size(); ++pi)
        {
            const launch::Parcel &pc = B.parcels[pi];
            const PState &s = pb[pi];
            if (!s.ok)
                continue;
            const double age = tau - pc.t0;
            if (prev >= 0 && B.parcels[(size_t)prev].stream != pc.stream)
                prev = -1;
            const int dec = decOf[pi]; // (the thinning level, above)
            const bool last = pi + 1 >= B.parcels.size() || B.parcels[pi + 1].stream != pc.stream || pc.stream < 0 ||
                              !ps[pi + 1].ok;
            L.plumeGrpDbg[li].maxDec = std::max(L.plumeGrpDbg[li].maxDec, dec);
            if (pi % (size_t)dec != 0 && !last)
                continue;
            const uint64_t rimSeed = B.seed ^ (uint64_t)(pc.t0 * 10.0);
            curGround = pc.ground;
            if (pc.stream < 0 || prev < 0)
            {
                // A vent's jet stretch, the pad's ground cloud, or the first knot of a burn: as emitted.
                const double Sigma = pc.kappa * pc.m * s.cond * gain * (double)wOf[pi];
                addPlume(s.cE, pc.axis, 0.5 * pc.len, s.sig, Sigma, s.sweep, pc.albedo, age, rimSeed, pc.stream >= 0,
                         pc.tint);
            }
            else
            {
                const PState &q = pb[(size_t)prev];
                glm::dvec3 seg = s.cE - q.cE;
                const double sl = glm::length(seg);
                const glm::dvec3 axE = sl > 1e-3 ? seg / sl : pc.axis;
                double Sigma = 0.0;
                for (int k = prev + 1; k <= (int)pi; ++k)
                    Sigma += B.parcels[(size_t)k].kappa * B.parcels[(size_t)k].m;
                // The chord's condensed share: the mean of its two ends (the younger end alone made a new segment,
                // born with nothing condensed, dimmer than the head it replaced: the trail pulsed once a knot).
                Sigma *= 0.5 * (s.cond + q.cond) * gain;
                // Half the chord as the along-trail sigma joins neighbours smoothly; a young segment is tighter, so
                // the head does not reach ahead of the stage.
                const double cA = glm::mix(0.35, 0.5, glm::smoothstep(0.0, 3.0 * std::max(pc.dt, 0.5), age));
                addPlume(0.5 * (s.cE + q.cE), axE, cA * sl, 0.5 * (s.sig + q.sig), Sigma, 0.5 * (s.sweep + q.sweep),
                         pc.albedo, age, rimSeed, true, pc.tint);
            }
            prev = (int)pi;
            // The head: from the youngest knot to the nozzle, while the burn goes on.
            if (pc.stream >= 0 && pi + 1 < B.parcels.size() && B.parcels[pi + 1].stream == pc.stream &&
                !ps[pi + 1].ok)
            {
                const launch::Parcel &nx = B.parcels[pi + 1];
                const double f = (tau - pc.t0) / std::max(nx.t0 - pc.t0, 1e-3);
                bool okN = false;
                const glm::dvec3 nz = LaunchImpl::rocketEcef(L, B, nx.stage, tau, okN);
                if (okN && f > 0.0 && f < 1.0 + 1e-6)
                {
                    glm::dvec3 seg = nz - s.cE;
                    const double sl = glm::length(seg);
                    const glm::dvec3 axE = sl > 1e-3 ? seg / sl : nx.axis;
                    // The exhaust's own spread over the head's age (its knot's radius, shrinking to the nozzle).
                    // Like a segment's: the mean of its ends' condensed shares (the nozzle's is 0) and radii (8 m), so
                    // when the next knot is born the segment that replaces the head is the same Gaussian.
                    const double condH = 0.5 * s.cond;
                    addPlume(0.5 * (nz + s.cE), axE, 0.35 * sl, 0.5 * (s.sig + 8.0),
                             nx.kappa * nx.m * f * condH * gain, 0.0, nx.albedo, 0.0, rimSeed ^ 0x5Aull, true, nx.tint);
                }
            }
        }
    }
    L.prof[7] += (plumeMs - L.prof[7]) / 30.0;
    L.prof[9] += (reMs - L.prof[9]) / 30.0;
    L.prof[10] += (buildMs - L.prof[10]) / 30.0;
    const auto sortT0 = std::chrono::steady_clock::now();
    // The plumes: the brightest per pixel of sky first, as many as fit.
    if (!plumes.empty())
    {
        // At most 384 plume Gaussians: a wide one covers much of the screen and costs every pixel it covers
        // (cloud_march.comp's loop), ~3 ms at 1000.
        const uint32_t nMax = std::min<uint32_t>(kMaxDustClouds, n + 384u);
        // Only the drawn ones and the first past the cut need their order (2026-10-20, CPU: a full sort of thousands).
        const size_t nOrd = std::min(plumes.size(), (size_t)(nMax > n ? nMax - n : 0) + 1);
        // A fair share first (2026-10-26, issue 20261026_193912: "once more plumes occur, old ones pretty obviously
        // immediately disappear"): with the budget full, a new launch's young trail outranked a whole older bell and it
        // left in a frame or two. Each launch's strongest Gaussians, up to half the budget split evenly among the
        // launches drawing plumes (at most 48 each), are kept whatever their rank; the rest go by priority as before.
        {
            int groups = 0;
            for (int gc : grpCount)
                groups += gc > 0 ? 1 : 0;
            const size_t room0 = nMax > n ? (size_t)(nMax - n) : 0;
            const int R = groups > 0 ? std::min<int>(48, (int)(room0 / 2) / groups) : 0;
            // (2026-10-26, issue 20261026_210053) The share is EASED (0.25 rank a frame) and its last 8 ranks fade
            // (c.res: the weight a reserved Gaussian is drawn with at least): when another launch started drawing, R
            // stepped down and the ranks it no longer reserved fell under the cut at once - pieces of every older
            // plume popped out together. Never more than the room allows (room0 / groups); a share that has to halve
            // at once (a jump in time or place) is taken at once.
            {
                const double Rmax = groups > 0 ? (double)room0 / (double)groups : 48.0;
                double &Re = L.plumeResEased;
                if ((double)R < 0.5 * Re)
                    Re = (double)R;
                Re = Re > (double)R ? std::max((double)R, Re - 0.25) : std::min((double)R, Re + 0.25);
                Re = std::min(Re, Rmax);
            }
            const double Re = L.plumeResEased;
            std::sort(plumes.begin(), plumes.end(), [](const Cand &a, const Cand &b) {
                return a.grp != b.grp ? a.grp < b.grp : a.pri > b.pri;
            });
            int rank = 0, prevG = -2;
            for (Cand &c : plumes)
            {
                rank = c.grp == prevG ? rank + 1 : 0;
                prevG = c.grp;
                c.res = c.grp >= 0 ? glm::clamp((Re - (double)rank) / 8.0, 0.0, 1.0) : 0.0;
                c.key = c.res > 0.0 ? c.pri * 1e12 + 1e-30 : c.pri;
                // (the weakest of the share, for next frame's skip test; 0 = the launch has fewer than the share)
                if (c.grp >= 0)
                    L.visList[(size_t)c.grp].second->plumeResPri = 0.0;
            }
            rank = 0;
            prevG = -2;
            for (const Cand &c : plumes)
            {
                rank = c.grp == prevG ? rank + 1 : 0;
                prevG = c.grp;
                if (c.grp >= 0 && rank == (int)std::ceil(Re))
                    L.visList[(size_t)c.grp].second->plumeResPri = c.pri;
            }
        }
        std::partial_sort(plumes.begin(), plumes.begin() + nOrd, plumes.end(),
                          [](const Cand &a, const Cand &b) { return a.key > b.key; });
        // Near the cut a Gaussian fades by its priority (to 0 at the cut, full at 3x it): as plumes age and the view
        // moves their ranks shift, and a hard cut switched pieces of a trail on and off (blocky in time warp).
        const size_t room = nMax > n ? (size_t)(nMax - n) : 0;
        const double cut = plumes.size() > room && room > 0 ? plumes[room].key : 0.0;
        // (the skip in addPlume is exact while this frame's cut stayed above its margin)
        const bool unsafe = L.plumeSkipped > 0 && (cut <= 0.0 || cut < 0.25 * L.plumeCutPrev);
        L.plumeCutPrev = unsafe ? 0.0 : cut;
        // The ones that cannot show are dropped (dustCullInsignificant: on a CPU copy, never reading the mapped list).
        static std::vector<GpuDustCloud> sel;
        static std::vector<uint8_t> drop;
        sel.clear();
        for (size_t k = 0; k < std::min(room, plumes.size()); ++k)
            sel.push_back(plumes[k].d);
        dustCullInsignificant(sel.data(), (uint32_t)sel.size(), drop);
        size_t ci = 0;
        for (const Cand &c : plumes)
        {
            if (n >= nMax || ci >= sel.size())
                break;
            if (drop[ci++])
                continue;
            out[n] = c.d;
            if (cut > 0.0)
                out[n].c.w *= (float)(c.key > 1e6 * cut ? std::max(c.res, glm::clamp((c.pri - cut) / (2.0 * cut), 0.0, 1.0))
                                                         : glm::clamp((c.key - cut) / (2.0 * cut), 0.0, 1.0));
            if (c.grp >= 0 && (size_t)c.grp < L.plumeGrpDbg.size())
            {
                L.plumeGrpDbg[(size_t)c.grp].drawn++;
                L.plumeGrpDbg[(size_t)c.grp].sigD += out[n].c.w;
            }
            ++n;
            ++L.nPlumes;
        }
    }
    else
        L.plumeCutPrev = 0.0;
    L.plumeSkippedLast = L.plumeSkipped;
    L.prof[11] += (std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sortT0).count() - L.prof[11]) / 30.0;
    uint32_t *fh = static_cast<uint32_t *>(kFireMapped);
    fh[0] = kFireCount;
}

// ── Readouts, harness, UI ───────────────────────────────────────────────────────────────────────
nlohmann::json SatelliteSim::launchStateJson() const
{
    nlohmann::json j;
    j["enabled"] = launchesEnabled;
    // The engine / floodlight lights drawn this frame (engineLightsFlush), from the dust buffer's tail.
    {
        nlohmann::json el = nlohmann::json::array();
        if (kDustMapped)
        {
            const GpuDustCloud *tail =
                reinterpret_cast<const GpuDustCloud *>(static_cast<const char *>(kDustMapped) + 16) + kMaxDustClouds;
            for (uint32_t i = 0; i < engineLightCount; ++i)
                el.push_back({{"dist_km", glm::length(glm::vec3(tail[i].c)) / 1000.0f},
                              {"up_km", tail[i].c.z / 1000.0f},
                              {"intensity_suns_m2", tail[i].c.w},
                              {"r0_m", tail[i].ia.w},
                              {"reach_km", tail[i].col.z / 1000.0f},
                              {"ground", tail[i].col.y < 0.5f}});
        }
        j["engine_light_gain"] = engineLightGain;
        j["engine_light_adapt"] = engineAdaptEased; // the eye's adaptation to the lights at night (1 = none)
        j["engine_lights"] = el;
    }
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    j["catalog"] = L.loaded ? "ok" : L.err;
    j["planes"] = L.planes.size();
    j["batches_now"] = L.nBatches;
    j["batches_cached"] = L.batches.size();
    j["walk_complete"] = L.walkOk;
    // (2026-10-21b) The background build after a time jump: items, built, moved in; the last one's size and time.
    j["build_job"] = L.job ? nlohmann::json{{"items", L.job->items.size()},
                                             {"built", L.job->done.load()},
                                             {"applied", L.job->applied}}
                           : nlohmann::json(nullptr);
    j["build_jobs_run"] = L.jobsRun;
    j["last_build_job"] = {{"batches", L.lastJobN}, {"ms", L.lastJobMs}};
    j["rockets_drawn"] = L.nRockets;
    j["train_satellites"] = L.nTrain;
    j["retiring_satellites"] = L.nRetire;
    j["plume_gaussians"] = L.nPlumes;
    // The whole Gaussian list cloud_march.comp walks this frame (2026-10-21, its cost): by kind, and how much of the
    // sky each one's view cone covers (cullDustForTile skips a Gaussian on every tile its cone misses).
    if (kDustMapped)
    {
        const uint32_t cnt = std::min<uint32_t>(*static_cast<const uint32_t *>(kDustMapped), kMaxDustClouds);
        const GpuDustCloud *dl = reinterpret_cast<const GpuDustCloud *>(static_cast<const char *>(kDustMapped) + 16);
        int nKind[3] = {0, 0, 0}, nEvery = 0, nWide = 0, nMid = 0;
        double tauEvery = 0.0, radEvery = 0.0, tauEveryMax = 0.0;
        const double expo = (double)skyExposure();
        for (uint32_t i = 0; i < cnt; ++i)
        {
            const float w = dl[i].col.w;
            ++nKind[w < 0.5f ? 0 : (w < 1.5f ? 1 : 2)];
            const glm::vec4 &c = dl[i].cone;
            if (c.w <= -1.0f || glm::dot(glm::vec3(c), glm::vec3(c)) < 0.5f)
            {
                ++nEvery;
                // its optical depth through the centre and the light that could carry (display units, x2 for the phase)
                const GpuDustCloud &g = dl[i];
                const glm::dvec3 v = glm::normalize(glm::dvec3(g.c));
                const glm::dvec3 Mv(g.ia.x * v.x + g.ib.x * v.y + g.ib.y * v.z, g.ib.x * v.x + g.ia.y * v.y + g.ib.z * v.z,
                                    g.ib.y * v.x + g.ib.z * v.y + g.ia.z * v.z);
                const double q = std::max(glm::dot(v, Mv), 1e-30);
                const double tc = (double)g.c.w * g.ia.w * std::sqrt(2.0 * glm::pi<double>() / q);
                tauEvery += tc;
                tauEveryMax = std::max(tauEveryMax, tc);
                radEvery += tc * std::max({g.col.x, g.col.y, g.col.z}) * 2.0 * expo;
            }
            else if (c.w < std::cos(glm::radians(30.0f)))
                ++nWide;
            else if (c.w < std::cos(glm::radians(5.0f)))
                ++nMid;
        }
        j["dust_list"] = {{"count", cnt},          {"dust", nKind[0]},       {"emissive", nKind[1]},
                          {"plumes", nKind[2]},    {"cone_everywhere", nEvery}, {"cone_over_30deg", nWide},
                          {"cone_5_30deg", nMid},  {"everywhere_tau_sum", tauEvery},
                          {"everywhere_tau_max", tauEveryMax}, {"everywhere_display_sum", radEvery},
                          {"exposure", expo},      {"culled", plumesCulled},
                          {"cull_stop", {plumeCullStop.x, plumeCullStop.y, plumeCullStop.z, plumeCullStop.w}}};
    }
    j["reentries_now"] = L.nReentries;
    j["records_written"] = L.nWrites;
    j["fairing_halves_drawn"] = L.nFairings; // (2026-10-20)
    // The crew spacecraft flying or docked now (2026-10-20): phase, range to the station and its port, docked.
    {
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        nlohmann::json cv = nlohmann::json::array();
        for (const auto &kv : L.crewList)
        {
            const LBatch &B = *kv.second;
            const LCrew &C = B.crew;
            glm::dvec3 P;
            int ph = 0;
            uint8_t burn = 0;
            if (!LaunchImpl::crewPos(C, t, P, &ph, &burn))
                continue;
            glm::dvec3 Ps;
            glm::dmat3 R;
            LaunchImpl::crewStation(C, t, Ps, R);
            const glm::dvec3 rel = glm::transpose(R) * (P - Ps);
            const uint64_t ckey = (kv.first << 3) ^ 3ull;
            const launch::Program &pg = L.cat.programs[B.prog];
            cv.push_back({{"batch", kv.first},
                          {"key", ckey},
                          {"program", pg.name},
                          {"spacecraft", pg.spacecraft},
                          {"model", pg.crewModel},
                          {"station", constellations[L.planes[B.plane].constIdx].name},
                          {"station_sat", C.sat},
                          {"port", C.portName},
                          {"port_index", C.port},
                          {"ports", C.nPorts},
                          {"station_type", C.sat < satOrbits.size() && satOrbits[C.sat].typeIdx < satTypes.size()
                                               ? satTypes[satOrbits[C.sat].typeIdx].name
                                               : std::string("?")},
                          {"phase", kCrewPhaseNames[ph]},
                          {"phase_index", ph},
                          {"docked", (ph == CP_DOCKED || ph == CP_BERTHED)},
                          {"burning", burn != launch::BURN_NONE},
                          {"alt_km", (glm::length(P) - kR) / 1000.0},
                          {"range_to_station_m", glm::length(P - Ps)},
                          {"range_to_port_m", glm::length(rel - C.portPos)},
                          {"rel_station_frame_m", {rel.x, rel.y, rel.z}},
                          {"selected", selTransientKey == ckey && selIsTransient()},
                          {"followed", followIsTransient() && followTrKey == ckey},
                          {"t_sep_in_s", C.tSep - t},
                          {"t_arrive_in_s", C.tArr - t},
                          {"t_dock_in_s", C.tDock - t},
                          {"t_undock_in_s", C.tUndock - t},
                          {"t_deorbit_in_s", C.tDeorb - t},
                          {"t_entry_in_s", C.tEI - t},
                          {"t_splash_in_s", C.tSplash - t},
                          {"phasing_alt_km", (C.rP - kR) / 1000.0},
                          {"lands", C.lands}});
        }
        j["crew_vehicles"] = cv;
        j["crew_drawn"] = L.nCrew;
        // (2026-10-21) The station arms now (station frame, m): who they work with, holding or not, the joint points.
        nlohmann::json aj = nlohmann::json::array();
        for (const auto &as : L.armSat)
        {
            const launch::StationArm &A = L.cat.arms[(size_t)as.second];
            ArmPose ap;
            LaunchImpl::stationArmPose(A, t, LaunchImpl::berthVisits(L, as.first), ap);
            std::string with;
            if (ap.with)
                for (const auto &kv : L.crewList)
                    if (&kv.second->crew == ap.with)
                        with = L.cat.programs[kv.second->prog].spacecraft;
            auto v3 = [](const glm::dvec3 &v) { return nlohmann::json::array({v.x, v.y, v.z}); };
            aj.push_back({{"station", constellations[satOrbits[as.first].constIdx].name}, {"model", A.model},
                          {"with", with}, {"attached", ap.attached}, {"reach_short_m", ap.reachShort},
                          {"shoulder", v3(ap.S)}, {"elbow", v3(ap.E)}, {"wrist", v3(ap.W)}, {"tip", v3(ap.G)}});
        }
        j["station_arms"] = aj;
        j["arm_parts_drawn"] = L.nArmParts;
        j["mesh_local_lights"] = meshLightsPrev_.size();
        j["mesh_lights_scene"] = meshLightsScene_;
        j["mesh_lights_viewer"] = meshLightsViewer_;
        nlohmann::json vl = nlohmann::json::array();
        for (int i = 0; i < meshLightsViewer_ && i < 8; ++i)
            vl.push_back({viewerLightDbg_[i].x, viewerLightDbg_[i].y, viewerLightDbg_[i].z, viewerLightDbg_[i].w,
                          (viewerLightMaskDbg_ >> i) & 1u});
        j["mesh_lights_viewer_list"] = vl; // (2026-10-21) each: offset from the subject (m, ECEF axes), I, in its mask
        j["viewer_arm_parts"] = viewerArmPartsDrawn_;
    }
    j["stack_meshes"] = L.nStacked;          // satellites drawn on their upper stage (payload stacks)
    j["stack_stages"] = L.stackDraws.size();
    {
        nlohmann::json pa = nlohmann::json::array();
        for (const auto &q : L.plasmaDbg)
            pa.push_back({{"range_m", q.rangeM}, {"res_k", q.resK}, {"s_asked", q.sAsked}, {"s_disp_capped", q.sDispCapped},
                          {"weight", q.weight}, {"view_hold", q.hold}, {"sigma_min_m", q.sigMin}, {"sigma_max_m", q.sigMax}});
        j["entry_plasma"] = pa;
        j["sky_exposure"] = skyExposure();
    }
    j["free_chunks"] = L.freeChunks.size();
    j["slots_shown"] = std::count_if(L.slots.begin(), L.slots.end(), [](const SlotInfo &x) { return x.shown; });
    j["life_scale"] = launchLifeScale;
    j["cpu_split_ms"] = {{"window", L.prof[0]}, {"live_list", L.prof[1]}, {"freeing", L.prof[2]}, {"writes", L.prof[3]},
                         {"active_scan", L.prof[4]}, {"pad_lights", L.prof[5]}, {"visuals", L.prof[6]}, {"plumes", L.prof[7]},
                         {"writes_member_records", L.prof[8]}, {"reentries", L.prof[9]}, {"plume_builds", L.prof[10]}, {"plume_candidates", L.plumeDbg.size()}, {"plume_skipped", L.plumeSkippedLast}, {"plume_sort_emit", L.prof[11]}, {"due_evals", L.nDueEval},
                         {"window_finds", L.nFinds}};
    {
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const glm::dvec3 eyeE = glm::normalize(glm::dvec3(obsDir)) * (kR + std::max((double)obsTerrainH + 2.0, (double)obsHeightOffset));
        const std::vector<LaunchSound> ls = launchSounds(now, eyeE, glm::radians(camera.azDeg));
        nlohmann::json sa = nlohmann::json::array();
        for (const LaunchSound &x : ls)
            sa.push_back({{"level", x.level}, {"km", x.km}, {"pan", x.pan}, {"stage", x.stage}, {"burn", x.burn},
                          {"t_plus_s", x.tau}, {"alt_km", x.altKm}});
        j["sounds"] = sa;
        j["thunder_level_now"] = thunderLevelNow_;
    }
    {
        // The upper stages flying now: their timeline, in seconds from now (deployment, the turn, the deorbit burn,
        // the entry interface), and what the stage is doing.
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        nlohmann::json us = nlohmann::json::array();
        for (uint64_t key : L.live)
        {
            auto it = L.batches.find(key);
            if (it == L.batches.end() || !it->second.valid)
                continue;
            const LBatch &B = it->second;
            if (now < B.tL || now > B.tStageGone + 480.0)
                continue;
            const launch::Vehicle &vh = L.cat.vehicles[B.veh];
            glm::dvec3 P;
            uint8_t burn = 0;
            const bool fl = LaunchImpl::rocketPos(L, B, 1, now - B.tL, P, &burn);
            const glm::dvec3 nose = LaunchImpl::stageNose(L, B, 1, now - B.tL);
            glm::dvec3 P1;
            double retroDeg = -1.0;
            if (fl && LaunchImpl::rocketPos(L, B, 1, now - B.tL - 1.0, P1) && glm::length(nose) > 0.5)
                retroDeg = glm::degrees(std::acos(glm::clamp(glm::dot(nose, -glm::normalize(P - P1)), -1.0, 1.0)));
            us.push_back({{"vehicle", vh.id}, {"t_plus_s", now - B.tL}, {"deploy_in_s", B.tD - now},
                          {"release_end_in_s", (B.mem.empty() ? B.tD : B.mem.back().tRel) - now},
                          {"deorbit_burn_in_s", B.tDeorbBurn - now}, {"burn_s", B.burnDur},
                          {"entry_interface_in_s", B.tStageGone - now}, {"returns", !vh.upperDeorbit},
                          {"flying", fl}, {"burning", burn != launch::BURN_NONE},
                          {"alt_km", fl ? (glm::length(P) - kR) / 1000.0 : 0.0}, {"nose_from_retrograde_deg", retroDeg},
                          {"fairing_sep_in_s", B.tFair > 0.0 ? B.tFair - now : 0.0},
                          {"model", [&] {
                               const int ty = LaunchImpl::upperStageType(*this, L, B, now);
                               return ty >= 0 && ty < (int)satTypes.size() ? satTypes[ty].modelId : std::string();
                           }()},
                          {"payload_layout", L.cat.programs[B.prog].stack.layout},
                          {"deploy_interval_s", B.deployInterval}});
        }
        j["upper_stages"] = us;
        // The boosters (2026-10-20): each launch's recovery and where the booster is in it.
        nlohmann::json bs = nlohmann::json::array();
        for (uint64_t key : L.live)
        {
            auto it = L.batches.find(key);
            if (it == L.batches.end() || !it->second.valid)
                continue;
            const LBatch &B = it->second;
            const Track &tr = LaunchImpl::boosterTr(L, B);
            const double tTouch = B.tL + tr.tEnd;
            if (now < B.tL - 3600.0 || now > tTouch + LaunchImpl::boosterHoldS(L, B))
                continue;
            glm::dvec3 P;
            uint8_t burn = 0;
            const bool fl = LaunchImpl::rocketPos(L, B, 0, now - B.tL, P, &burn);
            glm::dvec3 E(0.0);
            double tT = 0.0;
            const int lk = LaunchImpl::boosterLandingAt(L, B, now, E, tT);
            const glm::dvec3 Pe = fl ? eciToEcef(P, now) : glm::dvec3(0.0);
            static const char *kKinds[] = {"none", "tower", "landing_zone", "ship", "ground"};
            nlohmann::json o = {{"vehicle", L.cat.vehicles[B.veh].id}, {"site", L.cat.sites[B.site].id},
                                {"t_plus_s", now - B.tL}, {"recovery", B.rec == 0 ? "expendable" : B.rec == 1 ? "return" : "downrange"},
                                {"land_kind", kKinds[std::clamp(lk, 0, 4)]}, {"payload_kg", B.payloadKg},
                                {"touchdown_in_s", tTouch - now}, {"downrange_km", B.rec == 2 ? B.xLand / 1000.0 : 0.0},
                                {"flying", fl}, {"burn", (int)burn}, {"alt_km", fl ? (glm::length(Pe) - kR) / 1000.0 : 0.0},
                                {"dist_to_landing_m", fl && lk != kLandNone ? glm::length(Pe - E) : -1.0}};
            if (B.landKind == kLandShip)
                o["ship"] = L.cat.vehicles[B.veh].ships[(size_t)B.ship].name;
            bs.push_back(o);
        }
        j["boosters"] = bs;
        j["ships"] = launchShipsJson();
        j["ship_meshes"] = L.nShips;
        j["ship_robot_meshes"] = L.nShipRobots;
    }
    j["plume_gain"] = launchPlumeGain;
    // The plume parcels drawn: a sample (the largest optical depths first).
    std::vector<LaunchState::PlumeDbg> pd = L.plumeDbg;
    std::sort(pd.begin(), pd.end(), [](const auto &a, const auto &b) { return a.tauPeak > b.tauPeak; });
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < pd.size() && i < 12; ++i)
        arr.push_back({{"range_km", pd[i].rangeKm}, {"alt_km", pd[i].altKm}, {"sigma_km", pd[i].sigKm},
                       {"cross_section_m2", pd[i].Sigma}, {"tau_peak", pd[i].tauPeak}, {"age_s", pd[i].age},
                       {"sun_col", {pd[i].col.x, pd[i].col.y, pd[i].col.z}}, {"sky", pd[i].amb}});
    j["plume_sample"] = arr;
    std::sort(pd.begin(), pd.end(), [](const auto &a, const auto &b) { return a.altKm > b.altKm; });
    nlohmann::json hi = nlohmann::json::array();
    for (size_t i = 0; i < pd.size() && i < 8; ++i)
        hi.push_back({{"range_km", pd[i].rangeKm}, {"alt_km", pd[i].altKm}, {"sigma_km", pd[i].sigKm},
                      {"cross_section_m2", pd[i].Sigma}, {"tau_peak", pd[i].tauPeak}, {"age_s", pd[i].age},
                      {"sun_col", {pd[i].col.x, pd[i].col.y, pd[i].col.z}}});
    j["plume_high"] = hi;
    j["plume_parcels"] = pd.size();
    {
        nlohmann::json g = nlohmann::json::array();
        for (const auto &e : L.plumeGrpDbg)
            if (e.cand > 0 || e.parcels > 0)
                g.push_back({{"key", e.key}, {"t_plus_s", e.tPlus}, {"cand", e.cand}, {"drawn", e.drawn},
                             {"parcels", e.parcels}, {"alive", e.alive}, {"max_dec", e.maxDec},
                             {"sigma_cand", e.sigC}, {"sigma_drawn", e.sigD}, {"range_km", e.rangeKm}});
        j["plume_groups"] = g;
        j["plume_cut"] = L.plumeCutPrev;
    }
    // The ground clouds (2026-10-20): lift-off and landing parcels drawn, and the thickest of them.
    {
        std::sort(pd.begin(), pd.end(), [](const auto &a, const auto &b) { return a.tauPeak > b.tauPeak; });
        int nG[3] = {0, 0, 0};
        nlohmann::json gs = nlohmann::json::array();
        for (const auto &e : pd)
        {
            nG[std::clamp(e.ground, 0, 2)]++;
            if (e.ground > 0 && gs.size() < 8)
                gs.push_back({{"kind", e.ground == 1 ? "lift_off" : "landing"}, {"range_km", e.rangeKm},
                              {"alt_km", e.altKm}, {"sigma_km", e.sigKm}, {"tau_peak", e.tauPeak}, {"age_s", e.age},
                              {"drawn_col", {e.drawn.x, e.drawn.y, e.drawn.z}}, {"iso_share", e.isoShare}});
        }
        j["ground_clouds"] = {{"lift_off", nG[1]}, {"landing", nG[2]}, {"sample", gs}};
    }
    return j;
}

void formatSimClock(double tJ2000, char *buf, size_t n, bool withDate); // SatelliteSim.cpp

static std::string fmtUtc(double tJ2000)
{
    char b[48];
    formatSimClock(tJ2000, b, sizeof(b), true);
    return b;
}

// "2036-11-22 03:39 UTC" (the mission timeline's rows and facts).
std::string fmtUtcReadable(double tJ2000)
{
    const time_t unixSim = (time_t)std::floor(tJ2000) + 946728000;
    const struct tm *u = gmtime(&unixSim);
    char b[48];
    if (!u)
        return "--";
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d UTC", u->tm_year + 1900, u->tm_mon + 1, u->tm_mday, u->tm_hour,
             u->tm_min, u->tm_sec);
    return b;
}

// Every launch with lift-off in [now, now + hours] (builds the batches it needs).
nlohmann::json SatelliteSim::launchListJson(double hours) const
{
    nlohmann::json arr = nlohmann::json::array();
    if (!launchState)
        return arr;
    LaunchState &L = const_cast<LaunchState &>(*launchState);
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return arr;
    SatelliteSim &sim = const_cast<SatelliteSim &>(*this);
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    struct Item
    {
        double tL;
        uint64_t key;
    };
    std::vector<Item> items;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        const double life = progLifeS(pg, (double)launchLifeScale);
        // Lift-off precedes arrival by the raise (+ under a day for the window).
        const double lo = t + (pg.raiseDays[0] - 0.5 - pg.earlyDays()) * kDay,
                     hi = t + hours * 3600.0 + (pg.raiseDays[1] + 2.5) * kDay;
        LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
            const uint64_t key = batchKey((int)pi, k, b);
            auto it = L.batches.find(key);
            if (it == L.batches.end())
            {
                LBatch nb;
                nb.plane = (int)pi;
                nb.k = k;
                nb.b = b;
                LaunchImpl::buildBatch(sim, L, nb, pl, life);
                nb.touched = L.frame;
                it = L.batches.emplace(key, std::move(nb)).first;
            }
            const LBatch &B = it->second;
            if (B.valid && B.tL >= t && B.tL <= t + hours * 3600.0)
                items.push_back({B.tL, key});
        });
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.tL < b.tL; });
    for (const Item &it : items)
    {
        const LBatch &B = L.batches.at(it.key);
        const LPlane &pl = L.planes[B.plane];
        nlohmann::json o;
        o["utc"] = fmtUtc(B.tL);
        o["in_hours"] = (B.tL - t) / 3600.0;
        o["program"] = L.cat.programs[B.prog].name;
        o["constellation"] = constellations[pl.constIdx].name;
        o["vehicle"] = L.cat.vehicles[B.veh].name;
        o["site"] = L.cat.sites[B.site].name;
        o["azimuth_deg"] = B.azDeg;
        o["payload"] = B.mem.size();
        o["incl_deg"] = glm::degrees(pl.inclRad);
        o["arrival_days"] = (B.tA - B.tL) / kDay;
        o["plane"] = B.plane;
        o["batch"] = B.b;
        o["refresh"] = B.k;
        o["first_fill"] = B.first;
        o["pad_shifts"] = B.shifts;
        o["pad_rivals"] = B.rivals;
        // The Sun at the pad at lift-off (twilight launches make the jellyfish).
        const glm::dvec3 sun = sunDirEciAt(B.tL);
        o["sun_elev_deg"] = glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(L.cat.sites[B.site].ecef, B.tL)), -1.0, 1.0)));
        arr.push_back(o);
    }
    return arr;
}

// Why batches did not fly: every batch the cache holds after listing `hours`, counted by program and reason.
nlohmann::json SatelliteSim::launchWhyJson(double hours) const
{
    (void)launchListJson(hours);
    nlohmann::json j = nlohmann::json::object();
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (B.prog < 0)
            continue;
        const std::string pn = L.cat.programs[B.prog].name;
        const std::string w = B.valid ? std::string("ok") : B.why;
        if (!j.contains(pn))
            j[pn] = nlohmann::json::object();
        j[pn][w] = j[pn].value(w, 0) + 1;
    }
    return j;
}

// Puts the observer near the next launch: twilight (the Sun 5-10 deg below the pad's horizon) within a week if asked,
// else the next. Time 30 s before lift-off at 1x, the camera on the climb.
bool SatelliteSim::launchGoToNext(std::string &msg, bool twilight) { return launchGoToNextWhen(msg, twilight ? 1 : 0); }

// when: 0 the next, 1 twilight (the Sun 5-10 deg down at the pad), 2 day (above 15 deg), 3 night (below -20 deg).
bool SatelliteSim::launchGoToNextWhen(std::string &msg, int when, const std::string &vehicle, double padDistM,
                                      double lead, double sideSign)
{
    const bool twilight = when != 0;
    if (launchState)
        LaunchImpl::ensureCatalog(*launchState);
    if (!launchState || !launchState->loaded || !launchesEnabled)
    {
        msg = "launches are off (or data/launches.json did not load)";
        return false;
    }
    LaunchState &L = *launchState;
    const nlohmann::json list = launchListJson(!vehicle.empty() ? 24.0 * 30.0 : twilight ? 24.0 * 7.0 : 48.0);
    const nlohmann::json *pick = nullptr;
    for (const nlohmann::json &o : list)
    {
        if (o["in_hours"].get<double>() < 0.02)
            continue;
        if (!vehicle.empty())
        {
            // A vehicle id or a part of its name, case-blind.
            std::string nm = o.value("vehicle", std::string());
            std::string q = vehicle;
            for (auto &ch : nm)
                ch = (char)std::tolower((unsigned char)ch);
            for (auto &ch : q)
                ch = (char)std::tolower((unsigned char)ch);
            std::string nmc;
            for (char ch : nm)
                if (std::isalnum((unsigned char)ch))
                    nmc += ch;
            std::string qc;
            for (char ch : q)
                if (std::isalnum((unsigned char)ch))
                    qc += ch;
            if (nmc.find(qc) == std::string::npos)
                continue;
        }
        const double se = o["sun_elev_deg"].get<double>();
        if (when == 0 || (when == 1 && se < -5.0 && se > -10.0) || (when == 2 && se > 15.0) || (when == 3 && se < -20.0))
        {
            pick = &o;
            break;
        }
    }
    if (!pick)
    {
        msg = !vehicle.empty() ? "no such launch in the next 30 days" : twilight ? "no such launch in the next week"
                                                                                : "no launch in the next 48 h";
        return false;
    }
    // Find its batch again.
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay + (*pick)["in_hours"].get<double>() * 3600.0;
    const LBatch *B = nullptr;
    for (const auto &kv : L.batches)
        if (kv.second.valid && std::abs(kv.second.tL - t0) < 1.0)
        {
            B = &kv.second;
            launchFocusKey = kv.first;
        }
    if (!B)
    {
        msg = "launch not found";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tStart = B->tL - lead;
    const double days = std::floor(tStart / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tStart - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // The observer: 60 km back from the pad against the ground track and 50 km to its side: the climb crosses the sky
    // ahead and the staging and the plume's spread are in view.
    const glm::dvec3 side = glm::normalize(glm::cross(B->G0, B->Eaz));
    const glm::dvec3 g = glm::normalize(B->G0 * kR - B->Eaz * 60000.0 + side * 50000.0);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = 0.0f; // on the ground (the height is above sea level, floored at the ground): it was left as it
                            // was, so a jump from orbit stayed in orbit
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    // Look at where the rocket is two minutes in.
    const Track &bt = L.booster[B->veh];
    const glm::dvec2 p = bt.at(std::min(120.0, bt.tEnd));
    glm::dvec3 target = LaunchImpl::trackEcef(*B, p.x, p.y);
    if (padDistM > 0.0)
    {
        // On the ground beside the pad (the lift-off cloud, the first km of the trail), looking at the pad 120 m up.
        const glm::dvec3 gp = glm::normalize(B->G0 * kR + side * (sideSign < 0.0 ? -padDistM : padDistM) -
                                             B->Eaz * (0.35 * padDistM));
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(gp.z, -1.0, 1.0))), glm::degrees(std::atan2(gp.y, gp.x)));
        target = B->G0 * (kR + B->padBase + 120.0);
    }
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 rel = target - up * kR;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 70.0f;
    updatePositions(tStart);
    char b[300];
    snprintf(b, sizeof(b), "%s from %s in 30 s (%s, Sun %.1f deg)", (*pick)["vehicle"].get<std::string>().c_str(),
             (*pick)["site"].get<std::string>().c_str(), (*pick)["constellation"].get<std::string>().c_str(),
             (*pick)["sun_elev_deg"].get<double>());
    msg = b;
    return true;
}

// Aims the camera at the nearest flying rocket's stage (0 booster, 1 upper stage; the harness's `launch look`).
bool SatelliteSim::launchLook(int stage, std::string &msg)
{
    if (!launchState || !launchState->loaded)
    {
        msg = "no launch data";
        return false;
    }
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    double best = 1e30, bestTau = 0.0;
    glm::dvec3 bestP(0.0);
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid || t < B.tL - 600.0 || t > B.tStageGone + 60.0)
            continue;
        // (2026-10-20, the deployment era's dozens of pads) the launch `launch next` went to, when it is flying: the
        // nearest stage was often another rocket still on its pad.
        if (launchFocusKey && kv.first != launchFocusKey && L.batches.count(launchFocusKey) &&
            t >= L.batches.at(launchFocusKey).tL)
            continue;
        glm::dvec3 P;
        if (!LaunchImpl::rocketPos(L, B, stage, t - B.tL, P))
            continue;
        const double d = glm::length(P - eye);
        if (d < best)
        {
            best = d;
            bestP = P;
            bestTau = t - B.tL;
        }
    }
    if (best > 1e29)
    {
        msg = "no rocket flying";
        return false;
    }
    const glm::dvec3 rel = (bestP - eye) / best;
    const glm::dvec3 e(glm::dot(rel, glm::dvec3(eci2enuX)), glm::dot(rel, glm::dvec3(eci2enuY)),
                       glm::dot(rel, glm::dvec3(eci2enuZ)));
    aimCameraAzEl((float)glm::degrees(std::atan2(e.x, e.y)), (float)glm::degrees(std::asin(glm::clamp(e.z, -1.0, 1.0))));
    char b[160];
    snprintf(b, sizeof(b), "stage %d at T%+.0f s, %.0f km, az %.1f el %.1f", stage, bestTau, best / 1000.0,
             glm::degrees(std::atan2(e.x, e.y)), glm::degrees(std::asin(glm::clamp(e.z, -1.0, 1.0))));
    msg = b;
    return true;
}

// Self-check of the phasing (harness `launch check`): for the batches built so far, how far a train member is from
// the upper stage at deployment and from its slot at arrival, and a retiree from its slot when it leaves.
nlohmann::json SatelliteSim::launchCheckJson() const
{
    nlohmann::json j;
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    double maxArr = 0.0, sumArr = 0.0, maxDep = 0.0, sumDep = 0.0, maxRet = 0.0, maxEps = 0.0;
    glm::dvec3 depOffMax(0.0), depVelMax(0.0);
    size_t nMem = 0, nB = 0, nResid = 0;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid)
            continue;
        ++nB;
        for (const LMember &mb : B.mem)
        {
            // At its release the member is at the dispenser: the stage's aft end + the bay's offset (tens of m).
            glm::dvec3 Pst;
            const bool stageOk = LaunchImpl::rocketPos(L, B, 1, mb.tRel - B.tL, Pst);
            glm::dvec3 r, v;
            const glm::dvec3 slotA = satOrbitStateAt(rosterElemsOf(satOrbits[mb.sat]), B.tA).posEci;
            if (LaunchImpl::memberState(*this, B, mb, 0, B.tA - 1e-3, r, v))
            {
                const double d = glm::length(r - slotA);
                maxArr = std::max(maxArr, d);
                sumArr += d;
            }
            if (stageOk && LaunchImpl::memberState(*this, B, mb, 0, mb.tRel, r, v))
            {
                const double d = glm::length(r - Pst);
                maxDep = std::max(maxDep, d);
                sumDep += d;
            }
            if (LaunchImpl::memberState(*this, B, mb, 1, B.tA, r, v))
                maxRet = std::max(maxRet, glm::length(r - slotA));
            maxEps = std::max(maxEps, std::abs(mb.eps));
            for (int a = 0; a < 3; ++a)
            {
                depOffMax[a] = std::max(depOffMax[a], std::abs(mb.depOff[a]));
                depVelMax[a] = std::max(depVelMax[a], std::abs(mb.depVel[a]));
            }
            nResid += std::abs(mb.eps) > 1e-9;
            ++nMem;
        }
    }
    // The stages' attitude: the fastest turn of the (smoothed) nose over three flights, sampled every 0.5 s (no snaps:
    // the booster's flip, the turn to retrograde and a ship's pitch-up are slews), and the largest position step of the
    // upper stage across its deorbit burn and entry interface (continuity).
    {
        double maxRate[2] = {0.0, 0.0}, atTau[2] = {0.0, 0.0}, maxJump = 0.0;
        int nf = 0;
        for (const auto &kv : L.batches)
        {
            const LBatch &B = kv.second;
            if (!B.valid || nf >= 3)
                continue;
            ++nf;
            for (int stage = 0; stage < 2; ++stage)
            {
                const double tEnd = stage == 0 ? LaunchImpl::boosterTr(L, B).tEnd : B.tStageGone - B.tL;
                glm::dvec3 prev(0.0);
                for (double tau = stage == 0 ? 1.0 : L.cat.vehicles[B.veh].t1 + 1.0; tau < tEnd - 0.5; tau += 0.5)
                {
                    const glm::dvec3 n = LaunchImpl::stageNose(L, B, stage, tau);
                    if (glm::length(n) > 0.5 && glm::length(prev) > 0.5)
                    {
                        const double r = glm::degrees(std::acos(glm::clamp(glm::dot(n, prev), -1.0, 1.0))) / 0.5;
                        if (r > maxRate[stage])
                        {
                            maxRate[stage] = r;
                            atTau[stage] = tau;
                        }
                    }
                    prev = n;
                }
            }
            // Position continuity: 1-s steps vs the speed through the burn and to the entry interface (and the
            // returning ship's entry track just after it).
            const bool ret = !L.cat.vehicles[B.veh].upperDeorbit;
            glm::dvec3 Pp(0.0);
            bool have = false;
            for (double t = B.tDeorbBurn - 5.0; t < B.tStageGone + (ret ? 10.0 : 0.0); t += 1.0)
            {
                glm::dvec3 P;
                if (t < B.tStageGone)
                {
                    if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, P))
                        continue;
                }
                else
                    P = LaunchImpl::shipEntry(B, t - B.tStageGone, nullptr, nullptr);
                if (have)
                    maxJump = std::max(maxJump, glm::length(P - Pp));
                Pp = P;
                have = true;
                if (t > B.tDeorbBurn + 60.0 && t < B.tStageGone - 20.0)
                {
                    t = B.tStageGone - 20.0; // (the coast between: skip)
                    have = false;
                }
            }
        }
        j["booster_nose_max_deg_per_s"] = maxRate[0];
        j["booster_nose_max_at_t_plus_s"] = atTau[0];
        j["upper_nose_max_deg_per_s"] = maxRate[1];
        j["upper_nose_max_at_t_plus_s"] = atTau[1];
        j["upper_max_step_m_per_s"] = maxJump; // ~7.7-7.9 km/s: no jump
    }
    // The ship's landing (2026-10-19): the hand-over from the entry glow to the descent (the position and velocity
    // jump there, Earth-fixed), the descent's fastest 1-s step (no jumps on the way down), the catch (its distance
    // from the pad's centre, its height above the pad's ground and its speed), and the boosters' catches likewise.
    {
        double maxEntryP = 0.0, maxEntryV = 0.0, maxHandP = 0.0, maxHandV = 0.0, maxDescStep = 0.0, maxCatchOff = 0.0, maxCatchV = 0.0;
        double catchAbove[2] = {1e9, -1e9}, boostOff = 0.0, boostAbove[2] = {1e9, -1e9};
        int nShip = 0, nCaught = 0, nSplash = 0, nBoost = 0;
        int rcCount[5] = {0, 0, 0, 0, 0}, rcRtls = 0;
        std::map<std::string, std::array<double, 2>> rcDown; // per vehicle: min / max downrange (km)
        double rcOff[2] = {0.0, 0.0}, rcAbove[2][2] = {{1e9, -1e9}, {1e9, -1e9}}, rcSpeed[2] = {0.0, 0.0}, rcAcc = 0.0, rcRide = 0.0;
        nlohmann::json rcShipEx = nlohmann::json::array();
        nlohmann::json ex = nlohmann::json::array();
        // The booster's lean (its nose from the local vertical) on the way down, by landing kind (2026-10-20, issue
        // 20261020_213739 "Rockets land tilted"): at these heights above its touchdown point, max and mean.
        static const double kLeanAltKm[7] = {20.0, 8.0, 3.0, 1.0, 0.3, 0.05, 0.0};
        double leanMax[5][7] = {}, leanSum[5][7] = {};
        int leanN[5][7] = {};
        for (const auto &kv : L.batches)
        {
            const LBatch &B = kv.second;
            if (!B.valid)
                continue;
            const launch::Vehicle &vh = L.cat.vehicles[B.veh];
            // The boosters' recoveries (2026-10-20): counts by kind, downrange distances per vehicle, the touchdown on a
            // ship's deck / a landing zone (offset, height above the deck, speed over the last second), the booster
            // riding home with its ship (its largest offset from the deck point), the descent's continuity.
            {
                const Track &btr = LaunchImpl::boosterTr(L, B);
                rcCount[std::clamp(B.landKind, 0, 4)]++;
                if (btr.landed && B.landKind >= 1 && B.landKind <= 4 && btr.p.size() > 2)
                {
                    const double hT = btr.p.back().y;
                    size_t iTop = 0;
                    for (size_t i = 1; i < btr.p.size(); ++i)
                        if (btr.p[i].y > btr.p[iTop].y)
                            iTop = i;
                    for (int a = 0; a < 7; ++a)
                    {
                        double tau = btr.tEnd - 0.2; // 0 km: at touchdown
                        if (a < 6)
                        {
                            const double H = hT + 1000.0 * kLeanAltKm[a];
                            tau = -1.0;
                            for (size_t i = iTop; i + 1 < btr.p.size(); ++i)
                                if (btr.p[i].y >= H && btr.p[i + 1].y < H)
                                {
                                    tau = btr.dt * ((double)i + (btr.p[i].y - H) / std::max(btr.p[i].y - btr.p[i + 1].y, 1e-9));
                                    break;
                                }
                        }
                        glm::dvec3 Pn;
                        if (tau < 0.0 || !LaunchImpl::rocketPos(L, B, 0, tau, Pn))
                            continue;
                        const glm::dvec3 nose = LaunchImpl::stageNose(L, B, 0, tau);
                        if (glm::length(nose) < 0.5)
                            continue;
                        const double lean = glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(nose), glm::normalize(Pn)), -1.0, 1.0)));
                        leanMax[B.landKind][a] = std::max(leanMax[B.landKind][a], lean);
                        leanSum[B.landKind][a] += lean;
                        ++leanN[B.landKind][a];
                    }
                }
                if (B.rec == 1 && vh.recovery == 2)
                    ++rcRtls;
                if (B.landKind == kLandShip || B.landKind == kLandGround || B.landKind == kLandLz)
                {
                    auto &dr = rcDown[vh.id];
                    const double km = B.landKind == kLandLz ? 0.0 : B.xLand / 1000.0;
                    dr[0] = std::min(dr[0], km);
                    dr[1] = std::max(dr[1], km);
                    glm::dvec3 E, P0, P1;
                    double tT = 0.0;
                    LaunchImpl::boosterLandingAt(L, B, B.tL + btr.tEnd, E, tT);
                    if (LaunchImpl::rocketPos(L, B, 0, btr.tEnd, P0) && LaunchImpl::rocketPos(L, B, 0, btr.tEnd - 1.0, P1))
                    {
                        const glm::dvec3 E0 = eciToEcef(P0, tT), E1 = eciToEcef(P1, tT - 1.0);
                        const glm::dvec3 up = glm::normalize(E);
                        const glm::dvec3 d = E0 - E;
                        const int ki = B.landKind == kLandShip ? 0 : 1;
                        rcOff[ki] = std::max(rcOff[ki], glm::length(d - glm::dot(d, up) * up));
                        // (its legs' feet: the aft end stands vh.legM above them)
                        rcAbove[ki][0] = std::min(rcAbove[ki][0], glm::dot(d, up) - vh.legM);
                        rcAbove[ki][1] = std::max(rcAbove[ki][1], glm::dot(d, up) - vh.legM);
                        rcSpeed[ki] = std::max(rcSpeed[ki], glm::length(E0 - E1));
                    }
                    // Its last 60 s of descent: the largest change of velocity between 1-s steps (Earth-fixed).
                    glm::dvec3 pv(0.0), pp(0.0);
                    bool hv = false, hp = false;
                    for (double tau = btr.tEnd - 60.0; tau <= btr.tEnd + 20.0; tau += 1.0)
                    {
                        glm::dvec3 P;
                        if (!LaunchImpl::rocketPos(L, B, 0, tau, P))
                            continue;
                        const glm::dvec3 Ee = eciToEcef(P, B.tL + tau);
                        if (hp)
                        {
                            const glm::dvec3 v = Ee - pp;
                            if (hv)
                                rcAcc = std::max(rcAcc, glm::length(v - pv));
                            pv = v;
                            hv = true;
                        }
                        pp = Ee;
                        hp = true;
                    }
                    if (B.landKind == kLandShip)
                        for (double t = tT + 60.0; t < B.tShipGone; t += 1800.0)
                        {
                            glm::dvec3 P, Ed;
                            double tt;
                            if (!LaunchImpl::rocketPos(L, B, 0, t - B.tL, P))
                                continue;
                            LaunchImpl::boosterLandingAt(L, B, t, Ed, tt);
                            rcRide = std::max(rcRide, glm::length(eciToEcef(P, t) - Ed - vh.legM * glm::normalize(Ed)));
                        }
                    if (B.landKind == kLandShip && rcShipEx.size() < 4)
                    {
                        const glm::dvec3 g = B.shipG;
                        rcShipEx.push_back({{"vehicle", vh.id}, {"site", L.cat.sites[B.site].id},
                                            {"ship", vh.ships[(size_t)B.ship].name}, {"downrange_km", B.xLand / 1000.0},
                                            {"lat", glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0)))},
                                            {"lon", glm::degrees(std::atan2(g.y, g.x))}, {"payload_kg", B.payloadKg},
                                            {"touchdown_t_plus_s", btr.tEnd}});
                    }
                }
            }
            if (B.landKind == kLandTower && LaunchImpl::boosterTr(L, B).landed)
            {
                glm::dvec3 P;
                const double tc = LaunchImpl::boosterTr(L, B).tEnd;
                if (LaunchImpl::rocketPos(L, B, 0, tc + 5.0, P))
                {
                    const glm::dvec3 E = eciToEcef(P, B.tL + tc + 5.0);
                    const LPad &pd = L.pads[L.sitePad[B.site]];
                    const glm::dvec3 d = E - (kR + pd.elevM) * pd.g;
                    const double above = glm::dot(d, pd.g);
                    boostOff = std::max(boostOff, glm::length(d - above * pd.g));
                    boostAbove[0] = std::min(boostAbove[0], above);
                    boostAbove[1] = std::max(boostAbove[1], above);
                    ++nBoost;
                }
            }
            if (!B.shipLands)
                continue;
            ++nShip;
            (B.landPad >= 0 ? nCaught : nSplash)++;
            {
                // The entry's start vs the stage on its deorbit ellipse a moment before (and its velocity).
                glm::dvec3 Pa, Pb;
                if (LaunchImpl::rocketPos(L, B, 1, B.tStageGone - 1e-3 - B.tL, Pa) &&
                    LaunchImpl::rocketPos(L, B, 1, B.tStageGone - 1.0 - B.tL, Pb))
                {
                    const glm::dvec3 E0 = LaunchImpl::shipEntry(B, 0.0, nullptr, nullptr);
                    const glm::dvec3 E1 = LaunchImpl::shipEntry(B, 1.0, nullptr, nullptr);
                    maxEntryP = std::max(maxEntryP, glm::length(E0 - Pa));
                    maxEntryV = std::max(maxEntryV, glm::length((E1 - E0) - (Pa - Pb)));
                }
            }
            const double tH = B.tShipDesc;
            const glm::dvec3 a1 = eciToEcef(LaunchImpl::shipEntry(B, kShipEntryS, nullptr, nullptr), tH);
            const glm::dvec3 a0 = eciToEcef(LaunchImpl::shipEntry(B, kShipEntryS - 1.0, nullptr, nullptr), tH - 1.0);
            glm::dvec3 b0, b1;
            LaunchImpl::rocketPos(L, B, 1, tH - B.tL, b0);
            LaunchImpl::rocketPos(L, B, 1, tH + 1.0 - B.tL, b1);
            b0 = eciToEcef(b0, tH);
            b1 = eciToEcef(b1, tH + 1.0);
            maxHandP = std::max(maxHandP, glm::length(b0 - a1));
            maxHandV = std::max(maxHandV, glm::length((b1 - b0) - (a1 - a0)));
            glm::dvec3 prevV(0.0), prev(0.0);
            bool have = false;
            for (double t = tH; t < B.tShipCatch + 2.0; t += 1.0)
            {
                glm::dvec3 P;
                if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, P))
                    continue;
                const glm::dvec3 E = eciToEcef(P, t);
                if (have)
                {
                    const glm::dvec3 v = E - prev;
                    if (glm::length(prevV) > 0.0)
                        maxDescStep = std::max(maxDescStep, glm::length(v - prevV)); // m/s per s: the acceleration
                    prevV = v;
                }
                prev = E;
                have = true;
            }
            if (B.landPad >= 0)
            {
                const LPad &pd = L.pads[B.landPad];
                glm::dvec3 P, P1;
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - B.tL, P);
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - 1.0 - B.tL, P1);
                const glm::dvec3 E = eciToEcef(P, B.tShipCatch), E1 = eciToEcef(P1, B.tShipCatch - 1.0);
                const glm::dvec3 d = E - (kR + pd.elevM) * pd.g;
                const double above = glm::dot(d, pd.g);
                maxCatchOff = std::max(maxCatchOff, glm::length(d - above * pd.g));
                catchAbove[0] = std::min(catchAbove[0], above);
                catchAbove[1] = std::max(catchAbove[1], above);
                maxCatchV = std::max(maxCatchV, glm::length(E - E1));
            }
            if (ex.size() < 4)
            {
                nlohmann::json o;
                o["pad"] = B.landPad >= 0 ? L.cat.sites[L.pads[B.landPad].site].id : std::string("splashdown");
                o["deorbit_after_deploy_h"] = (B.tDeorbBurn - B.tD) / 3600.0;
                o["descent_from_km"] = glm::length(B.dX0) / 1000.0;
                o["descent_s"] = B.T1;
                o["landing_burn_s"] = B.T2;
                o["start_alt_km"] = B.dH0 / 1000.0;
                o["start_speed_m_s"] = glm::length(B.dV0);
                ex.push_back(o);
            }
        }
        j["ship_landings"] = nShip;
        j["ship_caught"] = nCaught;
        j["ship_splashdowns"] = nSplash;
        j["ship_entry_start_pos_m_max"] = maxEntryP;
        j["ship_entry_start_vel_m_s_max"] = maxEntryV;
        j["ship_handover_pos_m_max"] = maxHandP;
        j["ship_handover_vel_m_s_max"] = maxHandV;
        j["ship_descent_max_accel_m_s2"] = maxDescStep;
        j["ship_catch_off_centre_m_max"] = maxCatchOff;
        j["ship_catch_above_ground_m"] = {catchAbove[0], catchAbove[1]};
        j["ship_catch_speed_m_s_max"] = maxCatchV;
        j["ship_examples"] = ex;
        j["booster_catches"] = nBoost;
        j["booster_recovery_counts"] = {{"expendable", rcCount[0]}, {"tower", rcCount[1]}, {"landing_zone", rcCount[2]},
                                        {"ship", rcCount[3]}, {"ground", rcCount[4]}, {"rtls_by_payload", rcRtls}};
        {
            nlohmann::json dd = nlohmann::json::object();
            for (const auto &kv : rcDown)
                dd[kv.first] = {kv.second[0], kv.second[1]};
            j["booster_landing_downrange_km"] = dd;
        }
        j["droneship_touchdown_off_centre_m_max"] = rcOff[0];
        j["droneship_touchdown_above_deck_m"] = {rcAbove[0][0], rcAbove[0][1]};
        j["droneship_touchdown_speed_m_s_max"] = rcSpeed[0];
        j["lz_ground_touchdown_off_m_max"] = rcOff[1];
        j["lz_ground_touchdown_above_m"] = {rcAbove[1][0], rcAbove[1][1]};
        j["lz_ground_touchdown_speed_m_s_max"] = rcSpeed[1];
        j["booster_descent_max_dv_m_s_per_s"] = rcAcc;
        j["booster_ride_home_off_deck_m_max"] = rcRide;
        j["droneship_examples"] = rcShipEx;
        {
            static const char *kKindName[5] = {"expendable", "tower", "landing_zone", "ship", "ground"};
            nlohmann::json lj = nlohmann::json::object();
            for (int k = 1; k < 5; ++k)
            {
                nlohmann::json mx = nlohmann::json::array(), mn = nlohmann::json::array();
                for (int a = 0; a < 7; ++a)
                {
                    mx.push_back(leanN[k][a] ? leanMax[k][a] : -1.0);
                    mn.push_back(leanN[k][a] ? leanSum[k][a] / leanN[k][a] : -1.0);
                }
                lj[kKindName[k]] = {{"max", mx}, {"mean", mn}, {"n", leanN[k][0]}};
            }
            lj["alt_km"] = std::vector<double>(kLeanAltKm, kLeanAltKm + 7);
            j["booster_lean_deg_by_alt"] = lj;
        }
        j["booster_catch_off_centre_m_max"] = boostOff;
        j["booster_catch_above_ground_m"] = {boostAbove[0], boostAbove[1]};
    }
    j["batches"] = nB;
    j["members"] = nMem;
    j["arrival_err_m_max"] = maxArr;
    j["arrival_err_m_mean"] = nMem ? sumArr / nMem : 0.0;
    j["deploy_err_km_max"] = maxDep / 1000.0;
    j["deploy_err_km_mean"] = nMem ? sumDep / nMem / 1000.0 : 0.0;
    j["retiree_start_err_m_max"] = maxRet;
    j["phasing_residual_rad_max"] = maxEps;
    // the dispenser less a member's own path at its release (radial, along, cross): m and m/s
    j["release_offset_m_max"] = {depOffMax.x, depOffMax.y, depOffMax.z};
    j["release_rel_vel_mps_max"] = {depVelMax.x, depVelMax.y, depVelMax.z};
    j["members_with_residual"] = nResid;
    // Crew spacecraft (2026-10-20): for each crew batch in the cache, the spacecraft at its separation less its place on
    // the stage, at contact less the station's port, the largest second difference of its position at 1 s (a jump shows
    // as thousands of m/s^2; a burn or the entry's deceleration as tens) and its largest turn rate, over its whole flight
    // (1-s steps around every phase change, 20-s steps between).
    {
        nlohmann::json cj = nlohmann::json::array();
        double sepMax = 0.0, dockMax = 0.0, accMax = 0.0, rateMax = 0.0;
        std::string accAt, rateAt;
        int nFast = 0, nCargo = 0;
        double burnAccMax = 0.0;
        std::string burnAccAt;
        double fastLo = 1e30, fastHi = 0.0;
        // (2026-10-21) Berthing: every cached berthing visit per station (the arm's pose depends on all of them).
        std::map<uint32_t, std::vector<const LCrew *>> berthBy;
        for (const auto &kv : L.batches)
            if (kv.second.valid && kv.second.crew.on && kv.second.crew.berth)
                berthBy[kv.second.crew.sat].push_back(&kv.second.crew);
        int nBerth = 0;
        double bCapMax = 0.0, bGripMax = 0.0, bRelMax = 0.0, bArmStepMax = 0.0, bArmTurnMax = 0.0, bReachMax = 0.0,
               bAttachMax = 0.0;
        std::string bArmStepAt;
        for (const auto &kv : L.batches)
        {
            const LBatch &B = kv.second;
            const LCrew &C = B.crew;
            if (!B.valid || !C.on)
                continue;
            glm::dvec3 Pst, Pc;
            double sepOff = -1.0;
            if (LaunchImpl::rocketPos(L, B, 1, C.tSep - B.tL, Pst) && LaunchImpl::crewPos(C, C.tSep, Pc))
                sepOff = glm::length(Pst + LaunchImpl::upperFrame(L, B, C.tSep - B.tL) *
                                               glm::dvec3(0.0, 0.0, L.cat.vehicles[B.veh].spacecraftDockM) -
                                     Pc);
            glm::dvec3 Ps;
            glm::dmat3 R;
            LaunchImpl::crewStation(C, C.tDock, Ps, R);
            LaunchImpl::crewPos(C, C.tDock, Pc);
            // (an approach the station's loss called off never makes contact: -1)
            const double dockOff = (C.abort == 1 || C.abort == 2) ? -1.0 : glm::length(Ps + R * C.portPos - Pc);
            std::vector<double> ts;
            const double evs[] = {C.tSep, C.tB1, C.tB1e, C.tB2, C.tArr, C.tAxis, C.hold[0], C.hold[1], C.hold[2], C.tDock,
                                  C.tUndock, C.tDepEnd, C.tDeorb, C.tDeorb + C.deorbDur, C.tCm, C.tEI, C.tChute,
                                  C.tMains, C.tSplash};
            for (double e : evs)
                for (double d = -70.0; d <= 70.0; d += (std::abs(d) < 6.5 ? 1.0 : 4.0)) // (the burns span +-60 s)
                    ts.push_back(e + d);
            for (const LCrewSeg &g : C.segs)
                for (double d = -3.0; d <= 3.0; d += 1.0)
                {
                    ts.push_back(g.t0 + d);
                    ts.push_back(g.t1 + d);
                }
            for (double t = C.tSep; t < C.tGone; t += (t > C.tDock + 3600.0 && t < C.tUndock - 3600.0) ? 3600.0 : 20.0)
                ts.push_back(t);
            double bAcc = 0.0, bRate = 0.0, bBurn = 0.0;
            std::string bAccAt, bRateAt;
            for (double t : ts)
            {
                glm::dvec3 a, b0, c0;
                int ph = 0;
                if (!LaunchImpl::crewPos(C, t - 1.0, a) || !LaunchImpl::crewPos(C, t, b0, &ph) ||
                    !LaunchImpl::crewPos(C, t + 1.0, c0))
                    continue;
                const double acc = glm::length(a - 2.0 * b0 + c0);
                if (acc > bAcc)
                {
                    bAcc = acc;
                    bAccAt = kCrewPhaseNames[ph];
                }
                // (2026-10-21) in space (burns, the approach) apart from the entry and the parachutes
                const bool inAir = ph == CP_ENTRY || ph == CP_DROGUES || ph == CP_MAINS || ph == CP_DOWN || ph == CP_GLIDE ||
                                   ph == CP_FINAL_GLIDE || ph == CP_ROLLOUT;
                if (!inAir)
                    bBurn = std::max(bBurn, acc);
                if (!inAir && acc > burnAccMax)
                {
                    burnAccMax = acc;
                    burnAccAt = kCrewPhaseNames[ph];
                }
                const glm::dmat3 Ra = LaunchImpl::crewAttitude(C, t), Rb = LaunchImpl::crewAttitude(C, t + 1.0);
                const glm::dmat3 d = glm::transpose(Ra) * Rb;
                const double ang = glm::degrees(std::acos(std::clamp((d[0][0] + d[1][1] + d[2][2] - 1.0) * 0.5, -1.0, 1.0)));
                if (ang > bRate)
                {
                    bRate = ang;
                    bRateAt = kCrewPhaseNames[ph];
                }
            }
            cj.push_back({{"batch", kv.first}, {"station", constellations[L.planes[B.plane].constIdx].name},
                          {"port", C.portName}, {"sep_offset_m", sepOff}, {"dock_contact_m", dockOff},
                          {"accel_max_mps2", bAcc}, {"accel_max_in", bAccAt}, {"turn_max_deg_s", bRate},
                          {"turn_max_in", bRateAt}, {"days_to_dock", (C.tDock - B.tL) / kDay},
                          {"docked_days", (C.tUndock - C.tDock) / kDay}, {"lands", C.lands},
                          {"phasing_alt_km", (C.rP - kR) / 1000.0},
                          {"spacecraft", L.cat.programs[B.prog].spacecraft}, {"cargo", C.cargo}, {"burns_up", C.burns},
                          {"fast_track", C.fast}, {"hours_to_dock", (C.tDock - B.tL) / 3600.0}, {"abort", C.abort},
                          {"burn_accel_max_mps2", bBurn}, {"key", std::to_string(kv.first)}});
            // Berthing (2026-10-21): the vehicle at the capture point (m off it, mid-hold), the arm's tip on its grapple
            // fixture at capture and at release, and the arm over both of its windows (2-s steps): the largest step of
            // any joint point (shoulder, elbow, wrist, tip) per second, the booms' largest turn rate, the wrist out of
            // reach, and the tip's distance from the fixture while it holds the vehicle.
            const launch::StationArm *arm = L.armOf(C.sat);
            if (C.berth && arm && (C.abort == 0 || C.abort == 4))
            {
                ++nBerth;
                const std::vector<const LCrew *> &vis = berthBy[C.sat];
                nlohmann::json bj;
                glm::dvec3 Pv, Psb;
                glm::dmat3 Rb;
                const glm::dvec3 ax = glm::normalize(C.portAx);
                const double tc = C.tCapArr + 100.0;
                LaunchImpl::crewPos(C, tc, Pv);
                LaunchImpl::crewStation(C, tc, Psb, Rb);
                const double capErr = glm::length(Psb + Rb * (C.portPos + C.capM * ax) - Pv);
                auto gripErr = [&](double t) {
                    ArmPose ap;
                    LaunchImpl::stationArmPose(*arm, t, vis, ap);
                    glm::dvec3 G, n;
                    if (!LaunchImpl::berthFixture(C, t, G, n))
                        return -1.0;
                    return glm::length(ap.G - G);
                };
                const double gGrip = gripErr(C.tGrapple + 1.0);
                const double gRel = C.abort == 4 ? 0.0 : gripErr(C.tRelease - 1.0);
                double stepMax = 0.0, turnMax = 0.0, reachMax = 0.0, attachMax = 0.0;
                std::string stepAt;
                auto sweep = [&](double ta, double tb) {
                    ArmPose prev;
                    bool have = false;
                    for (double t = ta; t <= tb; t += 2.0)
                    {
                        ArmPose ap;
                        LaunchImpl::stationArmPose(*arm, t, vis, ap);
                        reachMax = std::max(reachMax, ap.reachShort);
                        if (ap.attached && ap.with == &C)
                        {
                            glm::dvec3 G, n;
                            if (LaunchImpl::berthFixture(C, t, G, n))
                                attachMax = std::max(attachMax, glm::length(ap.G - G));
                        }
                        if (have)
                        {
                            const double st = std::max({glm::length(ap.E - prev.E), glm::length(ap.W - prev.W),
                                                        glm::length(ap.G - prev.G)}) / 2.0;
                            if (st > stepMax)
                            {
                                stepMax = st;
                                char b[64];
                                snprintf(b, sizeof(b), "%+.0f s from the arm's start", t - ta);
                                stepAt = b;
                            }
                            auto ang = [](glm::dvec3 a, glm::dvec3 b) {
                                return glm::degrees(std::acos(glm::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)));
                            };
                            turnMax = std::max({turnMax, ang(ap.E - ap.S, prev.E - prev.S) / 2.0,
                                                ang(ap.W - ap.E, prev.W - prev.E) / 2.0, ang(ap.G - ap.W, prev.G - prev.W) / 2.0});
                        }
                        prev = ap;
                        have = true;
                    }
                };
                sweep(C.armA0 - 60.0, std::min(C.armA3, C.tDeath) + 60.0);
                if (C.abort == 0)
                    sweep(C.armD0 - 60.0, C.armD3 + 60.0);
                bj = {{"capture_point_err_m", capErr}, {"grip_tip_err_m", gGrip}, {"release_tip_err_m", gRel},
                      {"arm_step_max_m_s", stepMax}, {"arm_step_max_at", stepAt}, {"arm_turn_max_deg_s", turnMax},
                      {"arm_reach_short_m", reachMax}, {"arm_attached_err_m", attachMax}, {"port", C.portName},
                      {"capture_m", C.capM}, {"berthed_days", (C.tUndock - C.tDock) / kDay}};
                cj.back()["berth"] = bj;
                bCapMax = std::max(bCapMax, capErr);
                bGripMax = std::max(bGripMax, gGrip);
                bRelMax = std::max(bRelMax, gRel);
                if (stepMax > bArmStepMax)
                {
                    bArmStepMax = stepMax;
                    bArmStepAt = L.cat.programs[B.prog].spacecraft + " " + stepAt;
                }
                bArmTurnMax = std::max(bArmTurnMax, turnMax);
                bReachMax = std::max(bReachMax, reachMax);
                bAttachMax = std::max(bAttachMax, attachMax);
            }
            if (C.fast)
            {
                ++nFast;
                fastLo = std::min(fastLo, (C.tDock - B.tL) / 3600.0);
                fastHi = std::max(fastHi, (C.tDock - B.tL) / 3600.0);
            }
            nCargo += C.cargo ? 1 : 0;
            sepMax = std::max(sepMax, sepOff);
            dockMax = std::max(dockMax, dockOff);
            if (bAcc > accMax)
            {
                accMax = bAcc;
                accAt = bAccAt;
            }
            if (bRate > rateMax)
            {
                rateMax = bRate;
                rateAt = bRateAt;
            }
        }
        j["crew"] = cj;
        j["crew_sep_offset_m_max"] = sepMax;
        j["crew_dock_contact_m_max"] = dockMax;
        j["crew_accel_max_mps2"] = accMax;
        j["crew_accel_max_in"] = accAt;
        j["crew_turn_max_deg_s"] = rateMax;
        j["crew_turn_max_in"] = rateAt;
        j["crew_burn_accel_max_mps2"] = burnAccMax;
        j["crew_burn_accel_max_in"] = burnAccAt;
        j["crew_fast_track_count"] = nFast;
        j["crew_fast_track_hours"] = {nFast ? fastLo : 0.0, fastHi};
        j["crew_cargo_count"] = nCargo;
        j["berth_count"] = nBerth;
        j["berth_capture_point_err_m_max"] = bCapMax;
        j["berth_grip_tip_err_m_max"] = bGripMax;
        j["berth_release_tip_err_m_max"] = bRelMax;
        j["berth_arm_step_max_m_s"] = bArmStepMax;
        j["berth_arm_step_max_at"] = bArmStepAt;
        j["berth_arm_turn_max_deg_s"] = bArmTurnMax;
        j["berth_arm_reach_short_m"] = bReachMax;
        j["berth_arm_attached_err_m_max"] = bAttachMax;
    }
    return j;
}

// The GPU's view of a launch record (sat_orbit.comp's Kepler block, in float as it runs there; orbitPhase's product
// is exact, so only the final angle is rounded).
static glm::dvec3 gpuKeplerEmu(const GpuKepOrbit &g, double deltaT)
{
    const float M = (float)launchsim::wrap2pi((double)g.M + (double)g.n * deltaT);
    const float dtf = (float)deltaT;
    const float argp = g.argp + g.argpDot * dtf, raan = g.raan + g.raanDot * dtf;
    const float e = g.e, a = g.a, ci = g.cosI, si = g.sinI;
    float E = M + e * std::sin(M);
    for (int k = 0; k < 5; ++k)
        E -= (E - e * std::sin(E) - M) / (1.0f - e * std::cos(E));
    const float cE = std::cos(E), sE = std::sin(E), sq = std::sqrt(std::max(0.0f, 1.0f - e * e));
    const float cO = std::cos(raan), sO = std::sin(raan), cw = std::cos(argp), sw = std::sin(argp);
    const glm::vec3 P(cw * cO - sw * sO * ci, cw * sO + sw * cO * ci, sw * si);
    const glm::vec3 Q(-sw * cO - cw * sO * ci, -sw * sO + cw * cO * ci, cw * si);
    return glm::dvec3((a * (cE - e)) * P + (a * sq * sE) * Q);
}

void SatelliteSim::launchLegacyRecords(bool on)
{
    LaunchImpl::gLegacyRecords = on;
    if (launchState)
    {
        for (auto &kv : launchState->batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300; // rewrite everything now
        std::fill(launchState->liveSkip.begin(), launchState->liveSkip.end(), LaunchState::LiveSkip{});
    }
    if (launchState)
        for (SlotInfo &si : launchState->slots)
            si.tw = -1e300;
}

// Harness `launch jitter` (2026-10-19): how far a train member's drawn position jumps when its record is rewritten
// (the CPU copy, which places the mesh, and the GPU's, which places the point), over the release, the first minutes
// (rewritten every 2 s) and later (every 20 s); the new records against the pre-2026-10-19 ones on the same batches.
nlohmann::json SatelliteSim::launchJitterJson(int maxBatches)
{
    nlohmann::json j;
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    struct Phase
    {
        const char *name;
        double from, to, every;
    };
    static const Phase kPh[] = {{"release_0_60s", 1.0, 60.0, 2.0},
                                {"early_1_10min", 60.0, 600.0, 2.0},
                                {"later_10min_6h", 600.0, 21600.0, 20.0},
                                {"day_1_2", 86400.0, 172800.0, 20.0}};
    const bool keepLegacy = LaunchImpl::gLegacyRecords;
    for (int legacy = 1; legacy >= 0; --legacy)
    {
        LaunchImpl::gLegacyRecords = legacy != 0;
        nlohmann::json out;
        for (const Phase &ph : kPh)
        {
            double maxC = 0.0, sumC = 0.0, maxG = 0.0, sumG = 0.0, maxQ = 0.0;
            nlohmann::json worst;
            size_t n = 0, nb = 0;
            for (const auto &kv : L.batches)
            {
                const LBatch &B = kv.second;
                if (!B.valid || B.mem.empty() || (int)nb >= maxBatches)
                    continue;
                ++nb;
                for (size_t mi = 0; mi < B.mem.size(); mi += std::max<size_t>(1, B.mem.size() / 8))
                {
                    const LMember &mb = B.mem[mi];
                    if (std::abs(mb.eps) > 1e-9)
                        continue; // (a phasing residual: rewritten every 2 s, see LaunchState::update)
                    for (int k = 0; k < 6; ++k)
                    {
                        const double tw = mb.tRel + ph.from + (ph.to - ph.from) * (k + 0.37) / 6.0;
                        const double t1 = tw + ph.every;
                        const double tEpoch = std::floor((tw - 3.5 * 86400.0) / 86400.0) * 86400.0; // mid-rebake
                        GpuKepOrbit g0{}, g1{};
                        SatOrbitElems c0{}, c1{};
                        if (!LaunchImpl::memberRecord(*this, B, mb, 0, tw, tEpoch, g0, c0) ||
                            !LaunchImpl::memberRecord(*this, B, mb, 0, t1, tEpoch, g1, c1))
                            continue;
                        const double dC = glm::length(satOrbitStateAt(c0, t1).posEci - satOrbitStateAt(c1, t1).posEci);
                        const double dG = glm::length(gpuKeplerEmu(g0, t1 - tEpoch) - gpuKeplerEmu(g1, t1 - tEpoch));
                        // (the GPU's float floor: the same record a frame later, against its double position)
                        const double q = glm::length(gpuKeplerEmu(g1, t1 + 0.1 - tEpoch) -
                                                     satOrbitStateAt(c1, t1 + 0.1).posEci);
                        if (dC > maxC)
                            worst = {{"age_s", tw - mb.tRel}, {"raise_in_s", mb.sRaise - tw}, {"arrive_in_s", B.tA - tw},
                                     {"sep", {mb.sepDir.x, mb.sepDir.y, mb.sepDir.z}}, {"ecc", c0.ecc},
                                     {"alt_km", (c0.rSatM - kR) / 1000.0}, {"vehicle", L.cat.vehicles[B.veh].id}};
                        if (dC >= maxC)
                        {
                            // the path's acceleration less gravity (radial, along, cross): what no Kepler record follows
                            glm::dvec3 ra, rb, rc, vv;
                            const double hh = 2.0;
                            if (LaunchImpl::memberState(*this, B, mb, 0, tw - hh, ra, vv) &&
                                LaunchImpl::memberState(*this, B, mb, 0, tw, rb, vv) &&
                                LaunchImpl::memberState(*this, B, mb, 0, tw + hh, rc, vv))
                            {
                                const glm::dvec3 acc = (ra - 2.0 * rb + rc) / (hh * hh) +
                                                       launch::kGM * rb / std::pow(glm::length(rb), 3.0);
                                const glm::dvec3 U = glm::normalize(rb), Cn = glm::normalize(glm::cross(rb, rc - ra)),
                                                 A = glm::cross(Cn, U);
                                worst["accel_resid_rac"] = {glm::dot(acc, U), glm::dot(acc, A), glm::dot(acc, Cn)};
                            }
                        }
                        maxC = std::max(maxC, dC);
                        sumC += dC;
                        maxG = std::max(maxG, dG);
                        sumG += dG;
                        maxQ = std::max(maxQ, q);
                        ++n;
                    }
                }
            }
            out[ph.name] = {{"rewrite_s", ph.every},
                            {"samples", n},
                            {"cpu_jump_m_max", maxC},
                            {"cpu_jump_m_mean", n ? sumC / n : 0.0},
                            {"gpu_jump_m_max", maxG},
                            {"gpu_jump_m_mean", n ? sumG / n : 0.0},
                            {"gpu_vs_cpu_m_max", maxQ},
                            {"worst_cpu", worst}};
        }
        j[legacy ? "before" : "after"] = out;
    }
    LaunchImpl::gLegacyRecords = keepLegacy;
    // (2026-10-26, issue 20261026_205544) The live slots NOW: each record's position (what the GPU's point and, until
    // then, the mesh drew) against the member's exact path, by the time since the record was written — the jump the
    // next rewrite makes — and what the CPU places a mesh at (launchOrbitElems).
    {
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        double maxRec = 0.0, sumRec = 0.0, maxAge = 0.0, maxMesh = 0.0, meshSlotRec = 0.0;
        size_t n = 0, nMeshSlots = 0;
        nlohmann::json byAge = nlohmann::json::object();
        double binMax[6] = {0, 0, 0, 0, 0, 0};
        size_t binN[6] = {0, 0, 0, 0, 0, 0};
        static const char *kBin[6] = {"lt2s", "2_10s", "10_30s", "30_120s", "120_600s", "gt600s"};
        for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
        {
            const SlotInfo &si = L.slots[s];
            if (!si.shown || !si.live || si.owner == 0)
                continue;
            auto it = L.batches.find(si.owner / 2);
            if (it == L.batches.end() || si.m >= it->second.mem.size())
                continue;
            glm::dvec3 r, v;
            if (!LaunchImpl::memberState(*this, it->second, it->second.mem[si.m], (int)(si.owner % 2), t, r, v))
                continue;
            const double dRec = glm::length(satOrbitStateAt(L.elems[s], t).posEci - r);
            const double dMesh = glm::length(satOrbitStateAt(launchOrbitElems(s), t).posEci - r);
            const double age = t - si.tw;
            if (std::find(meshLaunchSlots.begin(), meshLaunchSlots.end(), s) != meshLaunchSlots.end())
            {
                meshSlotRec = std::max(meshSlotRec, dRec);
                ++nMeshSlots;
            }
            const int b =age < 2.0 ? 0 : age < 10.0 ? 1 : age < 30.0 ? 2 : age < 120.0 ? 3 : age < 600.0 ? 4 : 5;
            binMax[b] = std::max(binMax[b], dRec);
            ++binN[b];
            maxRec = std::max(maxRec, dRec);
            maxMesh = std::max(maxMesh, dMesh);
            maxAge = std::max(maxAge, age);
            sumRec += dRec;
            ++n;
        }
        for (int b = 0; b < 6; ++b)
            byAge[kBin[b]] = {{"slots", binN[b]}, {"record_err_m_max", binMax[b]}};
        j["live"] = {{"slots", n},
                     {"record_err_m_max", maxRec},
                     {"record_err_m_mean", n ? sumRec / n : 0.0},
                     {"mesh_err_m_max", maxMesh},
                     {"meshed_slots", nMeshSlots},
                     {"meshed_record_err_m_max", meshSlotRec},
                     {"record_age_s_max", maxAge},
                     {"by_age", byAge},
                     {"writes_total", L.nWrites}};
    }
    return j;
}

// Selects the nearest drawn train member (kind 0) or retiree (1) above the horizon (harness `launch select`).
bool SatelliteSim::launchSelect(int kind, std::string &msg, int member)
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 up = glm::normalize(eye);
    double best = 1e30;
    int bestSlot = -1;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
    {
        const SlotInfo &si = L.slots[s];
        if (!si.shown || (int)(si.owner % 2) != kind || (member >= 0 && (int)si.m != member))
            continue;
        const glm::dvec3 P = satOrbitStateAt(L.elems[s], (double)simDayJ2000 * 86400.0 + simSecInDay).posEci;
        const glm::dvec3 rel = P - eye;
        const double d = glm::length(rel);
        if (glm::dot(rel / d, up) < 0.05 || d >= best)
            continue;
        best = d;
        bestSlot = (int)s;
    }
    if (bestSlot < 0)
    {
        msg = "none above the horizon";
        return false;
    }
    selectSatellite((int)(activeSatCount + (uint32_t)bestSlot));
    char b[240];
    snprintf(b, sizeof(b), "%s, %.0f km away, %.0f km up", launchLabel((uint32_t)bestSlot).c_str(), best / 1000.0,
             (glm::length(satOrbitStateAt(L.elems[bestSlot], (double)simDayJ2000 * 86400.0 + simSecInDay).posEci) - kR) / 1000.0);
    msg = b;
    return true;
}

// The next retiree re-entry over a dark sky (within 48 h; harness `launch reentry`): the observer beside its track,
// 25 s before its breakup, at 1x.
bool SatelliteSim::launchGoToNextReentry(std::string &msg, bool watch)
{
    if (!launchState || !launchState->loaded)
    {
        msg = "no launch data";
        return false;
    }
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    double bestT = 1e300;
    glm::dvec3 bestR(0.0), bestV(0.0);
    uint32_t bestSat = 0;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid)
            continue;
        for (const LMember &mb : B.mem)
        {
            if (mb.tEnd <= t + 60.0 || mb.tEnd > t + 48.0 * 3600.0 || mb.tEnd >= bestT)
                continue;
            glm::dvec3 r, v;
            if (!LaunchImpl::memberState(*this, B, mb, 1, mb.tEnd - 1.0, r, v))
                continue;
            // Dark there?
            const glm::dvec3 sun = sunDirEciAt(mb.tEnd);
            if (glm::dot(glm::normalize(r), sun) > -0.14)
                continue;
            // A Watch: under a clear sky (the observer stands beside the track: either side). An overcast hid it.
            if (watch)
            {
                const glm::dvec3 Uc = glm::normalize(r);
                const glm::dvec3 Ac = glm::normalize(v - glm::dot(v, Uc) * Uc);
                const glm::dvec3 Cc = glm::normalize(glm::cross(Ac, Uc));
                const glm::dvec3 Bc = glm::normalize(Uc * kR + Ac * 600000.0);
                const float c0 = cloudCoverAtEcef(eciToEcef(glm::normalize(Bc * kR + Cc * 120000.0 - Ac * 80000.0), mb.tEnd));
                const float c1 = cloudCoverAtEcef(eciToEcef(glm::normalize(Bc * kR - Cc * 120000.0 - Ac * 80000.0), mb.tEnd));
                if (std::min(c0, c1) > 0.35f)
                    continue;
            }
            bestT = mb.tEnd;
            bestR = r;
            bestV = v;
            bestSat = mb.sat;
        }
    }
    if (bestT > 1e299)
    {
        msg = "no retiree re-enters over a dark sky in the next 48 h (batches are built as time reaches them)";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tb = bestT + 81.0; // the breakup (fbBreakupT)
    const double t0 = watch ? bestT + 45.0 : tb - 25.0; // (a Watch: mid-glow, the breakup ~35 s on)
    const double days = std::floor(t0 / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t0 - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const glm::dvec3 U = glm::normalize(bestR);
    glm::dvec3 A = bestV - glm::dot(bestV, U) * U;
    A = glm::normalize(A);
    const glm::dvec3 C = glm::normalize(glm::cross(A, U));
    // The breakup is ~600 km downrange of the glow's start: watch from beside that point (a Watch: the clearer side).
    const glm::dvec3 Bp = glm::normalize(U * kR + A * 600000.0);
    double side = 1.0;
    if (watch && cloudCoverAtEcef(eciToEcef(glm::normalize(Bp * kR - C * 120000.0 - A * 80000.0), bestT)) <
                     cloudCoverAtEcef(eciToEcef(glm::normalize(Bp * kR + C * 120000.0 - A * 80000.0), bestT)))
        side = -1.0;
    const glm::dvec3 G = glm::normalize(Bp * kR + C * (side * 120000.0) - A * 80000.0);
    const glm::dvec3 g = eciToEcef(G, t0);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = 0.0f; // on the ground
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 rel = eciToEcef(Bp * (kR + 78000.0), tb) - up * kR;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = watch ? 35.0f : 80.0f; // (a Watch tracks it: the fireball and its train fill more of the frame)
    updatePositions(t0);
    char b[240];
    if (watch)
    {
        selectedSatIndex = -1;
        selectedPlanetIndex = -1;
        selTransientKey = 0;
        selWantKind = 3; // the retiree's fireball (fbDrawReentry's tag), selected and tracked once listed
        selWantA = bestSat;
        selWantUntil = tb + 120.0;
        selWantViewer = false;
        selWantTrack = true;
        snprintf(b, sizeof(b), "%s (retired) burning up on re-entry", satLabel((int)bestSat).c_str());
    }
    else
        snprintf(b, sizeof(b), "%s (retired) re-enters: breakup in 25 s", satLabel((int)bestSat).c_str());
    msg = b;
    return true;
}

// ── Settings > Launches: LAUNCHES (the simulation; the graphics are buildLaunchGraphicsSection) ───────
// (Settings > Constellations until 2026-10-19, the Missions window's Controls tab until 2026-10-21: issue
// 20261021_175638 asked for a real place in the settings window, the graphics grouped.)
void SatelliteSim::buildLaunchSettingsSection(const UIInput &inp, UIRenderer &ui)
{
    uiSection("LdSec", 0, "LAUNCHES");
    bool on = launchesEnabled;
    if (uiToggleRow(inp, ui, "LaunchOn", 0, "Launches and retirements", on,
                    "Every constellation is kept flying: each orbital plane is refreshed once per design life by a "
                    "launch from a site under it. The train climbs to its slots, the satellites it replaces deorbit "
                    "and re-enter. Sites, rockets and programs: launches.json next to the exe."))
        launchesEnabled = on;
    bool eraOn = launchDeployEra;
    if (uiToggleRow(inp, ui, "LaunchEra", 0, "Deployment era (2019-2036)", eraOn,
                    "The constellations are built up by launches: before 2036 a constellation has only the satellites "
                    "launched so far (Starlink from 2019, Starmind 2028-2036), stations appear on their launch dates, "
                    "the debris field grows and the 2030s launch sites come online. Off: the 2036 roster at every date."))
        launchDeployEra = eraOn;
    if (!launchesEnabled)
        return;
    static const char *kLife[] = {"x0.5", "x1", "x2", "x4"};
    static const float kLifeV[] = {0.5f, 1.0f, 2.0f, 4.0f};
    int lSel = 1;
    for (int i = 0; i < 4; ++i)
        if (std::abs(launchLifeScale - kLifeV[i]) < 1e-3f)
            lSel = i;
    const int l = uiChoiceRow(inp, ui, "LaunchLife", 0, "Satellite design life", kLife, 4, lSel,
                              "x1 = each program's life (5-7 years). Shorter: more launches and re-entries (Starmind at "
                              "x1 is already ~50 Starship launches a day in 2036). Reshuffles the schedule.");
    if (l >= 0 && l != lSel)
        launchLifeScale = kLifeV[l];
    if (kesslerEnabled)
    {
        static const char *kAfter[] = {"Stand down", "Resume"};
        const int as = uiChoiceRow(inp, ui, "LaunchAfterStorm", 0, "After the Sun's Fury", kAfter, 2, launchAfterStorm,
                                   "Stand down: no launches from the storm on (the shells are filling with debris; no "
                                   "replacements, no retirements). Resume: launches return three weeks after it (the new "
                                   "satellites do not collide with the debris: they are not part of the cascade).");
        if (as >= 0 && as != launchAfterStorm)
            launchAfterStorm = as;
    }
    std::string why;
    // (2026-10-24) Each lands on the event already happening, the subject tracked (launchWatchKey).
    if (uiButton(inp, ui, "LaunchNext", 0, "Watch the next launch", "The next rocket lifting off, from beside the pad."))
    {
        if (launchGoToNextWhen(why, 0))
            launchWatchKey(launchFocusKey, kWatchLiftoffS, why);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (uiButton(inp, ui, "LaunchNext", 1, "Watch a twilight launch",
                 "A rocket's exhaust high up, lit by the Sun over the dark ground after dusk."))
    {
        if (launchGoToNextWhen(why, 1))
            launchWatchKey(launchFocusKey, kWatchTwilightS, why);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (uiButton(inp, ui, "LaunchCatch", 0, "Watch a Starship catch", "A Starship ship in its landing burn, caught by its tower."))
    {
        launchWatchCatch(why);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (uiButton(inp, ui, "LaunchReentry", 0, "Watch a retired satellite re-enter",
                 "A retired satellite burning up over a dark sky."))
    {
        launchGoToNextReentry(why, true);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (!launchState)
        return;
    LaunchState &L = *launchState;
    // The next launch, refreshed every few seconds (it walks the schedule).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - L.uiListAt > 3.0 || std::abs(((double)simDayJ2000 * 86400.0 + simSecInDay) - L.uiListSim) > 600.0)
    {
        L.uiListAt = now;
        L.uiListSim = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const nlohmann::json list = launchListJson(24.0);
        L.uiNext.clear();
        L.uiCount24 = (int)list.size();
        if (!list.empty())
        {
            const nlohmann::json &o = list[0];
            char b[200];
            snprintf(b, sizeof(b), "%s, %s, in %.1f h", o["vehicle"].get<std::string>().c_str(),
                     o["site"].get<std::string>().c_str(), o["in_hours"].get<double>());
            L.uiNext = b;
        }
    }
    const float lw = (float)fs(170);
    uiKV("LaunchKV", 0, "Next launch", L.uiNext.empty() ? "none in 24 h" : L.uiNext.c_str(), lw);
    uiKV("LaunchKV", 1, "Launches in the next 24 h", uiKitStr("%d", L.uiCount24), lw);
    uiKV("LaunchKV", 2, "Trains climbing", uiKitStr("%u satellites", L.nTrain), lw);
    uiKV("LaunchKV", 3, "Retired, deorbiting", uiKitStr("%u satellites", L.nRetire), lw);
}

// ── Settings > Launches: GRAPHICS (rockets, exhaust, launch sites) ─────────────────────────────────────
// The exhaust sliders (2026-10-21, issue 20261021_175638: "sliders for exhaust plumes, I wanna test ranges"): every one
// at 1 is the look before them. Slots 313-320 of the shared slider arrays (kCloudSliderSlots).
void SatelliteSim::buildLaunchGraphicsSection(const UIInput &inp, UIRenderer &ui)
{
    uiSection("LdSec", 2, "GRAPHICS: ROCKETS AND EXHAUST");
    bool padOn = launchPadGraphics;
    if (uiToggleRow(inp, ui, "LaunchPads", 0, "Launch site graphics", padOn,
                    "The pads on the ground: graded flat, concrete, flame trenches, tank farms, the towers (Starship's "
                    "catch tower), landing zones, their floodlights and beacons at night; no city lights within ~3 km."))
        launchPadGraphics = padOn;
    uiKV("LdGfxKV", 0, "",
         "Exhaust: 1 everywhere is the tuned look. Opacity, width and brightness act at once; growth rebuilds the "
         "plumes in view (a moment).",
         4.0f, &Pal::textDim);
    CloudSlider rows[] = {
        {"Plume opacity (x)", &launchPlumeGain, 0.0f, 5.0f, 0.05f, "%.2f", 313},
        {"Plume brightness (x)", &launchPlumeBright, 0.0f, 5.0f, 0.05f, "%.2f", 314},
        {"Plume width (x)", &launchPlumeWidth, 0.25f, 4.0f, 0.05f, "%.2f", 315},
        {"Plume billows (x)", &launchPlumeBillows, 0.0f, 3.0f, 0.05f, "%.2f", 316},
        {"Plume growth (x)", &launchPlumeGrowth, 0.0f, 5.0f, 0.1f, "%.1f", 317},
        {"Ground clouds (x)", &launchGroundCloudGain, 0.0f, 5.0f, 0.05f, "%.2f", 318},
        {"Flame brightness (x)", &launchRocketGain, 0.0f, 10.0f, 0.1f, "%.1f", 319},
        {"Rocket light on clouds (x)", &engineLightGain, 0.0f, 10.0f, 0.1f, "%.1f", 320},
        // 2026-10-28 (the settings round-up): had no UI. Old exhaust's widening with age (launches.plume_dispersal).
        {"Plume dispersal (x)", &launchPlumeDispersal, 0.0f, 5.0f, 0.05f, "%.2f", 425},
    };
    buildCloudSliderRows(inp, ui, rows, (int)(sizeof(rows) / sizeof(rows[0])), false);
    if (uiButton(inp, ui, "LdGfxReset", 0, "Reset exhaust to defaults", "Every slider above back to 1"))
    {
        launchPlumeGain = launchPlumeBright = launchPlumeWidth = launchPlumeBillows = 1.0f;
        launchPlumeGrowth = launchGroundCloudGain = launchRocketGain = engineLightGain = launchPlumeDispersal = 1.0f;
    }
}

// The rockets' roar at the listener (eye: ECI-like Earth-centred position the thunder uses, Earth-fixed axes): for
// each burning rocket within 150 km, the sound arriving now left it at the retarded time tau (tau + r(tau) / c = now);
// loudness as the flow's square root over the distance, thinning with the air at the rocket. Pure function of time.
std::vector<SatelliteSim::LaunchSound> SatelliteSim::launchSounds(double now, const glm::dvec3 &eyeEcef, float camAz) const
{
    std::vector<LaunchSound> out;
    if (!launchState || !launchesEnabled)
        return out;
    const LaunchState &L = *launchState;
    if (!L.loaded)
        return out;
    constexpr double kC = 343.0;
    const glm::dvec3 upE = glm::normalize(eyeEcef);
    const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), upE));
    const glm::dvec3 nE = glm::cross(upE, eE);
    for (uint64_t key : L.live)
    {
        auto it = L.batches.find(key);
        if (it == L.batches.end())
            continue;
        const LBatch &B = it->second;
        if (now < B.tL || now > B.tL + 1200.0)
            continue;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        if (glm::length(B.G0 * kR - eyeEcef) > 2.0e6)
            continue; // nothing of it is ever within hearing
        const double T = now - B.tL;
        for (int stage = 0; stage < 2; ++stage)
        {
            // The retarded time (Earth-fixed positions: the air carries the sound): the latest emission tau whose
            // sound reaches the listener now, g(tau) = (T - tau) - |P(tau) - eye| / c = 0. g(T) < 0 (no time to travel);
            // scan back over the last 440 s (150 km of sound) for the latest sign change, then bisect. (Until 2026-10-19 a fixed-point iteration
            // that did not converge for a fast, distant stage: it alternated between the stage and the pad, and an upper
            // stage 1200 km downrange was heard burning 300 m away, at full level, minutes after it left.)
            auto posE = [&](double tau, glm::dvec3 &E, uint8_t *b) {
                glm::dvec3 Pe;
                if (!LaunchImpl::rocketPos(L, B, stage, tau, Pe, b))
                    return false;
                E = eciToEcef(Pe, B.tL + tau);
                return true;
            };
            auto g = [&](double tau, bool &ok) {
                glm::dvec3 E;
                ok = posE(tau, E, nullptr);
                return ok ? (T - tau) - glm::length(E - eyeEcef) / kC : -1e9;
            };
            double hi = T, lo = -1.0;
            bool okHi = false;
            double gHi = g(hi, okHi);
            for (double tau = T - 2.0; tau >= std::max(0.0, T - 440.0) - 1e-9; tau -= 2.0)
            {
                bool okT = false;
                const double gT = g(tau, okT);
                // A crossing either way: g falls with tau while the stage recedes or climbs slower than sound, and
                // rises while it closes faster than sound (a booster's supersonic descent to its landing pad).
                if (okT && okHi && (gT >= 0.0) != (gHi >= 0.0))
                {
                    lo = tau;
                    break;
                }
                hi = tau;
                gHi = gT;
                okHi = okT;
            }
            if (lo < 0.0)
                continue; // nothing it emitted is arriving now (not arrived yet, gone, or out of hearing)
            {
                bool okL = false;
                const bool posLo = g(lo, okL) >= 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    bool okM = false;
                    const double gM = g(mid, okM);
                    if (okM && (gM >= 0.0) == posLo)
                        lo = mid;
                    else
                        hi = mid;
                }
            }
            double tau = lo;
            glm::dvec3 P;
            uint8_t burn = 0;
            if (!posE(tau, P, &burn) || burn == launch::BURN_NONE)
                continue;
            if (stage == 1 && tau < vh.t1)
                continue; // one vehicle before staging (the booster carries the roar)
            const double r = std::max(glm::length(P - eyeEcef), 200.0);
            if (r > 150000.0)
                continue;
            const double h = glm::length(P) - kR;
            const double flow = stage == 0 ? vh.flow1 * (burn == launch::BURN_ASCENT ? 1.0 : 0.3) : vh.flow2;
            const double lvl = 9.0 * std::sqrt(flow / 2500.0) * (1000.0 / r) * std::exp(-std::max(h, 0.0) / 20000.0) *
                               (1.0 - glm::smoothstep(80000.0, 150000.0, r));
            if (lvl < 1e-3)
                continue;
            const glm::dvec3 dir = (P - eyeEcef) / r;
            const double hE = glm::dot(dir, eE), hN = glm::dot(dir, nE);
            LaunchSound s;
            s.id = 0xA0000000u | (uint32_t)((key * 2 + (uint64_t)stage) & 0x0FFFFFFFu);
            s.level = (float)std::min(lvl, 8.0);
            s.km = (float)(r / 1000.0);
            s.pan = (float)(std::sin(std::atan2(hE, hN) - (double)camAz) * std::sqrt(hE * hE + hN * hN));
            s.stage = stage;
            s.burn = burn;
            s.tau = (float)tau;
            s.altKm = (float)(h / 1000.0);
            out.push_back(s);
        }
    }
    return out;
}

// The next stage venting while sunlit over a dark ground (within 30 days; harness `launch vent`): the observer 250 km
// to the side of the stage's track, 20 s before the vent, looking at it.
bool SatelliteSim::launchGoToVent(std::string &msg) { return launchGoToStageEvent(msg, 0); }
bool SatelliteSim::launchGoToShipEntry(std::string &msg) { return launchGoToStageEvent(msg, 1); }

// ev 0: a stage venting (sunlit, over a dark ground); 1: a returning stage's re-entry glow (over a dark ground).
bool SatelliteSim::launchGoToCrew(const std::string &station, const std::string &event, double leadS, bool follow,
                                  std::string &msg, const std::string &craft)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded || !launchesEnabled)
    {
        msg = "launches are off";
        return false;
    }
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    auto low = [](std::string x) {
        for (char &ch : x)
            ch = (char)std::tolower((unsigned char)ch);
        return x;
    };
    const std::string ev = low(event.empty() ? "dock" : event), sf = low(station), cf = low(craft);
    auto evTime = [&](const LBatch &B, bool &ok) {
        const LCrew &C = B.crew;
        ok = true;
        if (ev == "launch") return B.tL;
        if (ev == "sep") return C.tSep;
        if (ev == "phasing") return C.skipRaise1 ? C.tB2 : C.tB1;
        if (ev == "arrive") return C.tArr;
        if (ev == "axis") return C.tAxis;
        if (ev == "hold400") return C.hold[0];
        if (ev == "hold200") return C.hold[1];
        if (ev == "hold20") return C.hold[2];
        if (ev == "dock") return C.tDock;
        if (ev == "undock") return C.tUndock;
        if (ev == "depart") return C.tDepBurn;
        if (ev == "capture" || ev == "grapple") return C.berth ? C.tGrapple : 1e300;
        if (ev == "capture_point") return C.berth ? C.tCapArr : 1e300;
        if (ev == "arm") return C.berth ? C.armA0 : 1e300;
        if (ev == "berth") return C.berth ? C.tDock : 1e300;
        if (ev == "unberth") return C.berth ? C.tUndock : 1e300;
        if (ev == "release") return C.berth ? C.tRelease : 1e300;
        if (ev == "arm_depart") return C.berth ? C.armD0 : 1e300;
        if (ev == "deorbit") return C.tDeorb;
        if (ev == "entry" || ev == "reentry") return C.tEI;
        if (ev == "abort") return C.abort ? std::min(C.tDeath, C.tGone) : 1e300;
        if (ev == "chutes") return C.tChute;
        if (ev == "splash") return C.tSplash;
        // (2026-10-21c) a runway landing: the cargo module's jettison, the glide, touchdown, wheels stop
        if (ev == "jettison") return C.runway ? C.tCm : 1e300;
        if (ev == "glide") return C.runway ? C.tGlide : 1e300;
        if (ev == "final") return C.runway ? C.tTD - 90.0 : 1e300;
        if (ev == "touchdown") return C.runway ? C.tTD : 1e300;
        if (ev == "stop") return C.runway ? C.tStop : 1e300;
        ok = false;
        return 0.0;
    };
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    uint64_t bestKey = 0;
    double bestT = 1e300;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        if (!pg.crew || pg.crewModel.empty() || pl.sats.empty())
            continue;
        if (!sf.empty() && low(constellations[pl.constIdx].name).find(sf) == std::string::npos)
            continue;
        // craft= a spacecraft name part ("progress", "tianzhou", "cargo dragon"); "cargo" / "crew": either kind
        if (!cf.empty() && !(cf == "cargo" ? pg.cargo : cf == "crew" ? !pg.cargo
                                                                      : low(pg.spacecraft).find(cf) != std::string::npos))
            continue;
        const double life = progLifeS(pg, (double)launchLifeScale);
        std::vector<uint64_t> keys;
        LaunchImpl::forBatches(L, pl, life, t0 - life - (30.0 + pg.earlyDays()) * kDay, t0 + 2.2 * life, [&](int64_t k, int b) {
            keys.push_back(batchKey((int)pi, k, b));
        });
        for (uint64_t key : keys)
        {
            LBatch *B = missionBatch(key);
            if (!B || !B->valid || !B->crew.on)
                continue;
            bool ok = false;
            const double te = evTime(*B, ok);
            if (!ok)
            {
                msg = "unknown event '" + event + "' (launch sep phasing arrive axis hold400 hold200 hold20 dock undock capture_point capture berth unberth release arm arm_depart "
                                                  "depart deorbit entry chutes splash abort jettison glide final touchdown stop)";
                return false;
            }
            if (te < 1e299 && te - leadS >= t0 - 1.0 && te < bestT)
            {
                bestT = te;
                bestKey = key;
            }
        }
    }
    if (!bestKey)
    {
        msg = "no visiting vehicle with that event ahead" + (station.empty() ? std::string() : " at '" + station + "'");
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tS = bestT - leadS;
    const double days = std::floor(tS / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tS - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    launchFocusKey = bestKey;
    updatePositions(tS);
    const LBatch &B = *missionBatch(bestKey);
    SelTransient c;
    bool have = false;
    if (tS >= B.crew.tSep)
        have = LaunchImpl::crewFill(*this, L, B, bestKey, tS, c);
    else
        have = launchRocketTransient(bestKey, tS, c);
    if (have)
    {
        selTransientsT = tS;
        selTransientTake(c);
        selectedPlanetIndex = -1;
        selectedSatIndex = (int)transientSatIdx();
        formatSelectedSatInfo();
        if (follow && c.type >= 0)
            startFollow((int)transientSatIdx());
    }
    char b[320];
    snprintf(b, sizeof(b), "%s to %s: %s in %.0f s (%s)", L.cat.programs[B.prog].spacecraft.c_str(),
             constellations[L.planes[B.plane].constIdx].name.c_str(), ev.c_str(), leadS, fmtUtcReadable(bestT).c_str());
    msg = b;
    return true;
}

bool SatelliteSim::launchGoToStageEvent(std::string &msg, int ev, const std::string &filter, int minMembers)
{
    if (!launchState)
        return false;
    LaunchImpl::ensureCatalog(*launchState);
    LaunchState &L = *launchState;
    const nlohmann::json list = launchListJson(24.0 * 30.0);
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    for (const nlohmann::json &o : list)
    {
        const double tl = t0 + o["in_hours"].get<double>() * 3600.0;
        const LBatch *B = nullptr;
        uint64_t bkey = 0;
        for (const auto &kv : L.batches)
            if (kv.second.valid && std::abs(kv.second.tL - tl) < 1.0)
            {
                B = &kv.second;
                bkey = kv.first;
            }
        if (!B || (ev == 0 && !B->vents) || (ev == 1 && L.cat.vehicles[B->veh].upperDeorbit) || (ev == 4 && B->tFair < 0.0) ||
            (int)B->mem.size() < minMembers)
            continue;
        if (!filter.empty())
        {
            // (harness `launch deploy filter=Starmind`: a part of the constellation, program or vehicle name)
            auto low = [](std::string x) {
                for (char &ch : x)
                    ch = (char)std::tolower((unsigned char)ch);
                return x;
            };
            const std::string f = low(filter);
            const std::string hay = low(o.value("constellation", std::string()) + "|" + o.value("program", std::string()) +
                                        "|" + o.value("vehicle", std::string()));
            if (hay.find(f) == std::string::npos)
                continue;
        }
        // (ev 2: 50 s before the stage starts turning for its deorbit burn; ev 3: 10 s before it deploys; ev 4: 10 s
        // before its fairing separates.)
        const double tv = ev == 0   ? LaunchImpl::ventStart(L, *B) + 30.0
                          : ev == 1 ? B->tStageGone + 230.0
                          : ev == 2 ? B->tDeorbBurn - 100.0
                          : ev == 3 ? B->tD + 40.0
                                    : B->tFair + 40.0;
        glm::dvec3 P;
        if (ev != 1)
        {
            if (!LaunchImpl::rocketPos(L, *B, 1, tv - B->tL, P))
                continue;
        }
        else
        {
            P = LaunchImpl::shipEntry(*B, 230.0, nullptr, nullptr); // near the glow's brightest
        }
        const glm::dvec3 sun = sunDirEciAt(tv);
        const glm::dvec3 U = glm::normalize(P);
        const double sp = glm::dot(P, sun);
        const bool lit = sp > 0.0 || glm::length(P - sp * sun) > kR + 20000.0;
        if (ev == 1 && launchShipDay_)
        {
            if (glm::dot(U, sun) < 0.3)
                continue; // (harness `launch ship day=1`, 2026-10-21: the entry over a sunlit ground)
        }
        else if (ev >= 2 ? !lit : ((ev == 0 && !lit) || glm::dot(U, sun) > -0.12))
            continue; // a vent must be sunlit, both over a dark ground; a deorbit or a deployment sunlit
        if (followActive)
            stopFollow();
        if (trackActive)
            stopTrack();
        const double tS = tv - 50.0;
        const double days = std::floor(tS / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = tS - days * 86400.0;
        trailClearPending = true;
        timePaused = false;
        timeScaleIdx = 0;
        timeDir = 1.0f;
        const glm::dvec3 Cn = glm::normalize(B->N);
        const glm::dvec3 G = glm::normalize(U * kR + Cn * 250000.0);
        const glm::dvec3 g = eciToEcef(G, tS);
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const glm::dvec3 rel = eciToEcef(P, tv) - up * kR;
        glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
        east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
        const glm::dvec3 north = glm::cross(up, east);
        const glm::dvec3 rn = glm::normalize(rel);
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                      (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
        camera.fovYDeg = 60.0f;
        updatePositions(tS);
        launchFocusKey = bkey;
        if (ev >= 2)
            launchWantSelKey = (bkey << 3) ^ 2ull; // its upper stage, selected once listed (selTransientUpdate)
        char b[240];
        static const char *kWhat[] = {"vents in 20 s", "re-entry glow peaks in 20 s", "turns for its deorbit burn in 0 s",
                                      "deploys in 10 s", "drops its fairing in 10 s"};
        snprintf(b, sizeof(b), "%s upper stage from %s %s, %.0f km away", o["vehicle"].get<std::string>().c_str(),
                 o["site"].get<std::string>().c_str(), kWhat[std::clamp(ev, 0, 4)], glm::length(rel) / 1000.0);
        msg = b;
        return true;
    }
    msg = ev == 0   ? "no venting stage sunlit over a dark ground in the next 30 days"
          : ev == 1 ? "no returning stage re-entering over a dark ground in the next 30 days"
                    : "no sunlit upper stage at that event in the next 30 days";
    return false;
}

// ── Transient selection: rockets, the ship coming home, re-entry fireballs ───────────────────────────
// Their heads are listed each frame (addSelTransient) by the code that draws them; the selected one's state goes into
// the reserved last launch slot (transientSatIdx()) as SatOrbitElems::fixedState, so the panel, Track and the
// harness see a satellite there. Nothing else ever owns that slot (launchInitBlock keeps its chunk out of the pool).
// A re-entry the selection or Go to holds or waits for (kind 3 a retiree: a = its satellite; 4 an upper stage: a = the
// batch): listed even where it is not drawn (behind the Earth, out of range, too faint), so the selection carries on.
bool SatelliteSim::transientWanted(uint8_t kind, uint64_t a) const
{
    auto rides = [&](const SelTransient &c) {
        return c.a == a && (c.kind == kind || (kind == 4 && c.kind == 1 && c.stage == 1) || (kind == 9 && c.kind == 8));
    };
    if (selIsTransient() && rides(selTransientCur))
        return true;
    if (selWantKind == kind && selWantA == a)
        return true;
    if (followIsTransient() && (followTrKey ? rides(followTrCur) : (followWantKind == kind && followWantA == a)))
        return true;
    // A retiree's own record selected or followed (its fireball comes next).
    if (kind == 3 && a < activeSatCount && launchState)
        for (int idx : {selectedSatIndex, followActive ? followSatIndex : -1})
            if (idx >= (int)activeSatCount && (uint32_t)idx < poolIdx0() && (uint32_t)idx < transientSatIdx() - 1)
            {
                uint32_t m = 0;
                const uint64_t owner = launchSlotOwner((uint32_t)idx - activeSatCount, m);
                if (owner % 2 == 1)
                {
                    auto it = launchState->batches.find(owner / 2);
                    if (it != launchState->batches.end() && m < it->second.mem.size() && it->second.mem[m].sat == a)
                        return true;
                }
            }
    return false;
}

void SatelliteSim::addSelTransient(const SelTransient &s)
{
    if (selTransients.size() < 16384)
        selTransients.push_back(s);
}

void SatelliteSim::selTransientUpdate()
{
    FPROF_SCOPE("selTransientUpdate");
    selTransientsT = (double)simDayJ2000 * 86400.0 + simSecInDay; // the time the list was made at
    if (launchWantSelKey)
    {
        // (2026-10-20) the stage a `launch deploy` / `fairing` / `deorbit` jump framed: selected once it is listed;
        // (2026-10-24) a Watch also locks the camera on it.
        for (const SelTransient &s : selTransients)
            if (s.key == launchWantSelKey)
            {
                selTransientTake(s);
                selectedSatIndex = (int)transientSatIdx();
                selectedPlanetIndex = -1;
                launchWantSelKey = 0;
                formatSelectedSatInfo();
                if (launchWantTrack)
                    startTrack();
                launchWantTrack = false;
                break;
            }
        if (launchWantSelKey && launchWantTrack && selTransientsT > launchWantSelUntil)
        {
            launchWantSelKey = 0;
            launchWantTrack = false;
        }
    }
    followTransientUpdate();
    const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // A retiree's record ended while selected: its re-entry fireball, once the list has it (launchSlotEndedSelection).
    if (selWantKind)
    {
        if (selectedSatIndex >= 0 || tNow > selWantUntil || tNow < selWantUntil - 3600.0)
            selWantKind = 0;
        else
            for (const SelTransient &s : selTransients)
                if (s.kind == selWantKind && s.a == selWantA)
                {
                    selTransientTake(s);
                    selectedSatIndex = (int)transientSatIdx();
                    selectedPlanetIndex = -1;
                    if (selWantViewer && viewerSatIndex >= (int)activeSatCount && viewerSatIndex < (int)poolIdx0())
                    {
                        viewerSatIndex = selectedSatIndex;
                        viewerLastSelected = selectedSatIndex;
                        viewerSyncModel();
                    }
                    selWantKind = 0;
                    formatSelectedSatInfo();
                    if (selWantTrack)
                        startTrack();
                    selWantTrack = false;
                    break;
                }
        if (!selWantKind)
            selWantTrack = false;
    }
    // Go to rides a transient that is also the selection: the selection IS the followed object, through every
    // handover follow makes (staging, the ship's entry, the fireball), even while the list does not show it.
    if (followIsTransient() && followTrKey != 0 && selIsTransient() &&
        (selTransientKey == followTrKey || (selFollowKeyPrev != 0 && selTransientKey == selFollowKeyPrev)))
    {
        SelTransient c = followTrCur;
        for (const SelTransient &s : selTransients)
            if (s.key == c.key)
            {
                c.axis = s.axis;
                c.flare = s.flare;
                c.dirEnu = s.dirEnu;
                break;
            }
        selTransientTake(c);
        selFollowKeyPrev = followTrKey;
        formatSelectedSatInfo();
        return;
    }
    if (!followIsTransient())
        selFollowKeyPrev = 0;
    else if (followTrKey != 0)
        selFollowKeyPrev = followTrKey;
    // A selected train member or retiree: its panel line carries its height (launchStatus), refreshed every frame.
    if (selectedSatIndex >= (int)activeSatCount && selectedSatIndex < (int)poolIdx0() && !selIsTransient())
        formatSelectedSatInfo();
    if (!selIsTransient() || !launchState)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const SelTransient *f = nullptr;
    for (const SelTransient &s : selTransients)
        if (s.key == selTransientKey)
        {
            f = &s;
            break;
        }
    // The rocket goes on as another object: a stack after staging is its upper stage (the booster falls away), an
    // upper stage coming home its ship entry, a stage that deorbits its re-entry fireball. The selection follows it.
    SelTransient nx;
    // (2026-10-20) A crewed upper stage passing its spacecraft's separation: the selection goes with the spacecraft.
    bool crewSep = false;
    if (f && selTransientCur.kind == 1 && f->kind == 1)
    {
        auto bi = launchState->batches.find(selTransientCur.a);
        crewSep = bi != launchState->batches.end() && bi->second.crew.on &&
                  selTransientCur.tau < bi->second.crew.tSep - bi->second.tL && f->tau >= bi->second.crew.tSep - bi->second.tL;
    }
    if ((!f || crewSep || transientIsStack(selTransientCur) != transientIsStack(*f)) && transientSuccessor(selTransientCur, nx))
    {
        bool listed = false;
        for (const SelTransient &s : selTransients)
            if (s.key == nx.key)
            {
                f = &s;
                listed = true;
                break;
            }
        if (crewSep && !listed)
        {
            // The spacecraft is not in the list (out of its reach): the model places it.
            selTransientTake(nx);
            formatSelectedSatInfo();
            return;
        }
    }
    // An expendable upper stage's listing gives way to its re-entry fireball (kind 4, the same batch) as it starts to
    // glow, before the model's tStageGone says "fireball" (transientContinue): take it as soon as it is listed.
    if (!f && ((selTransientCur.kind == 1 && selTransientCur.stage == 1) || selTransientCur.kind == 8))
        for (const SelTransient &s : selTransients)
            if (s.kind == (selTransientCur.kind == 8 ? 9 : 4) && s.a == selTransientCur.a)
            {
                f = &s;
                break;
            }
    if (!f)
    {
        // Out of view this frame (behind the Earth, below the brightness cut) or over: gone for good after 2 s (or a
        // few frames of a fast warp). A stage that has just ended in its re-entry fireball waits longer for the list to
        // show it (as Go to does: followWantUntil), or the selection and the Info window were dropped at the handover.
        SelTransient cw = selTransientCur;
        uint8_t wantFb = 0;
        if (transientContinue(cw, t, wantFb))
        {
            // (2026-10-20) A rocket or the ship the list does not hold this frame (not drawn, out of the list's reach,
            // its visuals skipped): the launch model places it, so the selection stays, by its batch key.
            selTransientTake(cw);
            formatSelectedSatInfo();
            return;
        }
        if (std::abs(t - selTransientSeenT) > transientGraceS() + (wantFb ? 5.0 : 0.0))
        {
            stopTrack();
            selectedSatIndex = -1;
            selTransientKey = 0;
            formatSelectedSatInfo();
        }
        return;
    }
    selTransientTake(*f);
    formatSelectedSatInfo(); // the label carries altitude and speed
}

// ── Following a transient (2026-10-19) ─────────────────────────────────────────────────────────────
// The followed rocket stage / ship / fireball rides the follow slot (followSlotIdx), whatever is selected. Rockets and
// the ship are evaluated from the launch model at the frame's own time (transientExactState: no stale extrapolation
// at any warp); fireballs keep the list's quadratic. At a mission's seams the follow hands over to the object that
// continues it and the camera eases across the jump; when nothing does, it ends in place (stopFollowHere).
double SatelliteSim::transientGraceS() const
{
    // 2 s of sim time, or ~3 frames of a fast warp (a frame at 1 h/s is a minute).
    return std::max(2.0, 0.1 * (double)kTimeScales[timeScaleIdx]);
}

void SatelliteSim::transientWriteSlot(uint32_t slot, const SelTransient &s, double t0)
{
    if (!launchState || slot >= launchState->elems.size())
        return;
    SatOrbitElems &e = launchState->elems[slot];
    e = SatOrbitElems{};
    e.fixedState = true;
    e.fixPos = s.pos;
    e.fixVel = s.vel;
    e.fixAcc = s.acc;
    e.fixAxis = s.axis;
    LaunchImpl::transientAtt(e, s);
    LaunchImpl::transientLegs(*launchState, e, s);
    e.engineGlow = ((s.kind == 1 || s.kind == 8) && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
    e.fixT0 = t0;
    const uint32_t idx = activeSatCount + slot;
    if (idx < satOrbits.size())
    {
        SatOrbit &o = satOrbits[idx];
        o.typeIdx = s.type >= 0 ? (uint32_t)s.type : UINT32_MAX;
        o.altM = (float)(glm::length(s.pos) - kR);
        const glm::dvec3 h = glm::cross(s.pos, s.vel);
        o.incl = glm::length(h) > 1.0 ? (float)std::acos(glm::clamp(h.z / glm::length(h), -1.0, 1.0)) : 0.0f;
        o.raan = 0.0f;
        o.meanMot = 0.0f;
    }
}

bool SatelliteSim::transientIsStack(const SelTransient &s) const
{
    if (s.kind != 1 || s.stage != 0 || !launchState)
        return false;
    auto it = launchState->batches.find(s.a);
    return it != launchState->batches.end() && it->second.veh >= 0 &&
           s.tau < launchState->cat.vehicles[it->second.veh].t1;
}

bool SatelliteSim::transientExactState(SelTransient &s, double t) const
{
    if (!launchState || (s.kind != 1 && s.kind != 2 && s.kind != 8))
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(s.a);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    if (s.kind == 8)
    {
        // A crew spacecraft (2026-10-20): its own flight model; the list's on-screen fields are kept.
        SelTransient c = s;
        if (!LaunchImpl::crewFill(*this, L, B, s.a, t, c))
            return false;
        c.dirEnu = s.dirEnu;
        c.flare = s.flare;
        s = c;
        return true;
    }
    if (s.kind == 1)
    {
        const double tau = t - B.tL;
        glm::dvec3 P, P1, P2;
        uint8_t burn = 0;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau, P, &burn))
            return false;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau - 0.5, P1))
            P1 = P;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau - 1.0, P2))
            P2 = P1;
        s.pos = P;
        s.vel = (P - P1) / 0.5;
        s.acc = (P - 2.0 * P1 + P2) / 0.25;
        s.tau = tau;
        s.burn = burn;
        if (s.stage == 1)
            s.type = LaunchImpl::upperStageType(*this, L, B, t); // its fairing goes (2026-10-20)
        return true;
    }
    const double te = t - B.tStageGone;
    if (te < 0.0 || te > 480.0)
        return false;
    const glm::dvec3 P = LaunchImpl::shipEntry(B, te, nullptr, nullptr);
    // Differences over +-1 s inside the entry (2026-10-21): the 6-s backward ones read before the entry began for its
    // first 12 s, a velocity ~4 deg off its track that Go to's frame turned toward and back.
    const double ta = std::clamp(te - 1.0, 0.0, 478.0), tb = ta + 2.0;
    const glm::dvec3 Pa = LaunchImpl::shipEntry(B, ta, nullptr, nullptr), Pb = LaunchImpl::shipEntry(B, tb, nullptr, nullptr);
    const glm::dvec3 Pm = LaunchImpl::shipEntry(B, ta + 1.0, nullptr, nullptr);
    s.pos = P;
    s.vel = (Pb - Pa) / 2.0;
    s.acc = Pb - 2.0 * Pm + Pa;
    s.tau = te;
    return true;
}

// Where an object (a stage, the ship) is at time t by the launch model: the handover's "from" must be the predecessor
// at the SAME time as its successor. followTrCur alternates between the list's time and the frame's (a frame apart: at
// 10x, ~0.17 s, ~300 m at staging), and the jump eased out by updateFollow carried that motion as a 1.3-km offset of
// the camera from the rocket it follows (2026-10-19: the rocket left the frame at staging).
glm::dvec3 SatelliteSim::transientPosAt(const SelTransient &s, double t) const
{
    SelTransient p = s;
    if (transientExactState(p, t))
        return p.pos;
    // (2026-10-21) Past its own model's end (a returning stage at its entry interface, the ship's entry at its descent:
    // rocketPos / shipEntry refuse those times): carried on from its last state, at that state's own time. Returning
    // s.pos itself (a frame old) made the jump eased out by updateFollow one frame of motion: 1.3 km at 10x at the
    // entry interface (7.5 km/s), 190 m at the descent, the camera and its view thrown off the ship each time.
    double ts = 0.0;
    bool known = false;
    if (launchState && (s.kind == 1 || s.kind == 2))
    {
        auto it = launchState->batches.find(s.a);
        if (it != launchState->batches.end())
        {
            ts = (s.kind == 1 ? it->second.tL : it->second.tStageGone) + s.tau;
            known = true;
        }
    }
    if (!known || std::abs(t - ts) > 30.0)
        return s.pos;
    const double dt = t - ts;
    return s.pos + s.vel * dt + 0.5 * s.acc * dt * dt;
}

bool SatelliteSim::transientContinue(SelTransient &cur, double t, uint8_t &wantKind) const
{
    wantKind = 0;
    if (!launchState || (cur.kind != 1 && cur.kind != 2 && cur.kind != 8))
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(cur.a);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    auto typeOf = [&](const char *part) {
        auto ti = rocketTypes.find("rocket_" + vh.id + part);
        return ti != rocketTypes.end() ? ti->second : -1;
    };
    // A crew spacecraft (2026-10-20): before its separation it is its upper stage (time running backwards); the upper
    // stage that carried it hands over to it AT the separation (one already past it, picked on its own, stays the stage).
    if (cur.kind == 8 && t < B.crew.tSep)
    {
        cur.kind = 1;
        cur.stage = 1;
        cur.key = (cur.a << 3) ^ 2ull;
        cur.hasAtt = false;
        cur.tau = t - B.tL;
        cur.type = typeOf("_upper");
        if (cur.tau < vh.t1)
            return transientContinue(cur, t, wantKind);
    }
    if (cur.kind == 1 && B.crew.on && cur.tau < B.crew.tSep - B.tL && t >= B.crew.tSep && t < B.crew.tGone &&
        (cur.stage == 1 || cur.tau < vh.t1))
    {
        cur.kind = 8;
        cur.stage = 0;
        cur.key = (cur.a << 3) ^ 3ull;
    }
    // (2026-10-21) A cargo ship that burns up: its re-entry fireball (kind 9, the same batch) from the entry interface.
    if (cur.kind == 8 && B.crew.burns && t >= B.crew.tEI)
    {
        wantKind = 9;
        return false;
    }
    if (cur.kind == 8)
        return transientExactState(cur, t);
    // Time running backwards (or a jump back, 2026-10-20): the ship's entry before it began is its upper stage, an
    // upper stage before staging is the stack again (a selection taken from a list made at a later time came out as
    // "upper stage" at T+74 s, placed on the stack's track).
    if (cur.kind == 2 && t < B.tStageGone)
        cur.kind = 1;
    if (cur.kind == 1 && cur.stage == 1 && t - B.tL < vh.t1)
    {
        cur.stage = 0;
        cur.key = (cur.a << 3) ^ 1ull;
        cur.type = typeOf("_stack");
        cur.tau = t - B.tL;
        return transientExactState(cur, t);
    }
    // The stack separates: its upper stage carries on (the booster is another object).
    if (cur.kind == 1 && cur.stage == 0 && cur.tau < vh.t1 && t - B.tL >= vh.t1)
    {
        cur.stage = 1;
        cur.key = (cur.a << 3) ^ 2ull;
        cur.type = typeOf("_upper");
    }
    // The ship's entry hands back to the rocket path for its descent, landing burn and catch (2026-10-19).
    if (cur.kind == 2 && B.shipLands && t >= B.tShipDesc)
    {
        cur.kind = 1;
        cur.stage = 1;
        cur.type = typeOf("_upper");
    }
    if (cur.kind == 1 && cur.stage == 1 && t >= B.tStageGone && (!B.shipLands || t < B.tShipDesc))
    {
        if (vh.upperDeorbit)
        {
            wantKind = 4; // its re-entry fireball (from the list)
            return false;
        }
        cur.kind = 2; // the listing keeps the upper stage's key for its ship entry
        cur.type = typeOf("_upper");
    }
    return transientExactState(cur, t);
}

bool SatelliteSim::transientSuccessor(const SelTransient &cur, SelTransient &out) const
{
    const double t = selTransientsT;
    SelTransient c = cur;
    uint8_t want = 0;
    if (transientContinue(c, t, want) && (c.key != cur.key || c.kind != cur.kind))
    {
        out = c;
        return true;
    }
    if (want)
        for (const SelTransient &s : selTransients)
            if (s.kind == want && s.a == cur.a)
            {
                out = s;
                return true;
            }
    return false;
}

void SatelliteSim::followTakeTransient(const SelTransient &s, const glm::dvec3 &fromEci, double t)
{
    // The jump, Earth-fixed, eased out by updateFollow (metres at staging; tens of km where a stage's park orbit meets
    // its entry track).
    if (glm::length(fromEci) > 1.0)
        followHandD += eciToEcef(fromEci - s.pos, t);
    followTrCur = s;
    followTrKey = s.key;
    followTrSeenT = t;
    followWantKind = 0;
    transientWriteSlot(kLaunchCap - 2, s, t);
    followSatIndex = (int)followSlotIdx();
    followLaunchOwner = 0;
    snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(s).c_str());
}

void SatelliteSim::followTransientUpdate()
{
    FPROF_SCOPE("followTransientUpdate");
    if (!followIsTransient() || !launchState)
        return;
    const double t = selTransientsT;
    if (followTrKey == 0)
    {
        // A record that ended: its fireball, once the list has it.
        for (const SelTransient &s : selTransients)
            if (followWantKind && s.kind == followWantKind && s.a == followWantA)
            {
                followTakeTransient(s, followTrCur.pos, t);
                return;
            }
        if (t > followWantUntil || t < followWantUntil - 3600.0)
            stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    // Along the mission: staging, the ship's entry (computed from the model, not looked up).
    {
        SelTransient c = followTrCur;
        uint8_t want = 0;
        if (transientContinue(c, t, want))
        {
            for (const SelTransient &s : selTransients)
                if (s.key == c.key)
                {
                    c.axis = s.axis; // the list's attitude
                    c.flare = s.flare;
                    c.dirEnu = s.dirEnu;
                    break;
                }
            if (c.key != followTrKey || c.kind != followTrCur.kind)
                followTakeTransient(c, transientPosAt(followTrCur, t), t);
            else
            {
                followTrCur = c;
                followTrSeenT = t;
                transientWriteSlot(kLaunchCap - 2, c, t);
                snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(c).c_str());
            }
            return;
        }
        if (want)
        {
            followTrKey = 0;
            followWantKind = want;
            followWantA = followTrCur.a;
            followWantUntil = t + transientGraceS() + 5.0;
            return;
        }
    }
    // Fireballs (and a rocket the model no longer places): the list.
    for (const SelTransient &s : selTransients)
        if (s.key == followTrKey)
        {
            followTrCur = s;
            followTrSeenT = t;
            transientWriteSlot(kLaunchCap - 2, s, t);
            snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(s).c_str()); // its height moves
            return;
        }
    if (std::abs(t - followTrSeenT) > transientGraceS())
        stopFollowHere("Go to ended: the object is gone. You stay here.");
}

// Each frame before updateFollow: the followed object may have ended since the last frame.
void SatelliteSim::followCheckTarget()
{
    FPROF_SCOPE("followCheckTarget");
    if (!followActive || followSatIndex < 0)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const uint32_t idx = (uint32_t)followSatIndex;
    if (followIsTransient())
    {
        // Rockets at this frame's exact time (the list is a frame old: at a fast warp, many seconds), along the
        // mission: a stack that staged since the last frame is its upper stage now.
        if (followTrKey != 0)
        {
            SelTransient c = followTrCur;
            uint8_t want = 0;
            if (transientContinue(c, t, want))
            {
                if (c.key != followTrKey || c.kind != followTrCur.kind)
                    followTakeTransient(c, transientPosAt(followTrCur, t), t);
                else
                {
                    followTrCur = c;
                    transientWriteSlot(kLaunchCap - 2, c, t);
                }
            }
        }
        return;
    }
    if (idx >= satOrbits.size())
    {
        stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    if (rocketMeshSlotToTransient((int)idx) != (int)idx)
    {
        // An older path left the follow on a rocket's mesh slot (rebound every frame): its transient.
        if (selIsTransient())
            followTakeTransient(selTransientCur, glm::dvec3(0.0), t);
        else
            stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    // A record that ended in a fireball: ride a coasting copy of it until the list has the fireball (kind, a).
    auto awaitFireball = [&](uint8_t kind, uint64_t a) {
        const SatOrbitState st = satOrbitStateAt(orbitElemsOf(satOrbits[idx]), t);
        SelTransient g;
        g.pos = st.posEci;
        g.vel = st.velEci;
        const double r = std::max(glm::length(st.posEci), 1.0);
        g.acc = -satphot::kGM * st.posEci / (r * r * r);
        g.type = satOrbits[idx].typeIdx < satTypes.size() ? (int)satOrbits[idx].typeIdx : -1;
        followTrCur = g;
        followTrKey = 0;
        followWantKind = kind;
        followWantA = a;
        followWantUntil = t + transientGraceS() + 5.0;
        transientWriteSlot(kLaunchCap - 2, g, t);
        followSatIndex = (int)followSlotIdx();
        followLaunchOwner = 0;
    };
    const int32_t nowSec = (int32_t)std::clamp(std::floor(t - kesslerEpoch()), -2147483000.0, 2147483000.0);
    if (idx < activeSatCount)
    {
        // A roster satellite the cascade ended: destroyed (nothing left to ride) or re-entered (its fireball).
        if (kesslerEnabled && 2ull * idx + 1 < kRosterWords.size() && nowSec >= kRosterWords[2ull * idx + 1])
        {
            if (idx < kRosterDecay.size() && kRosterWords[2ull * idx + 1] == kRosterDecay[idx])
                awaitFireball(5, idx);
            else
                stopFollowHere("Go to ended: the satellite was destroyed. You stay here.");
        }
        return;
    }
    if (idx < poolIdx0())
    {
        uint32_t m = 0;
        const uint64_t owner = launchSlotOwner(idx - activeSatCount, m);
        if (followLaunchOwner != 0 && (owner != followLaunchOwner || m != followLaunchM))
        {
            const LBatch *B = nullptr;
            if (launchState)
            {
                auto it = launchState->batches.find(followLaunchOwner / 2);
                if (it != launchState->batches.end() && followLaunchM < it->second.mem.size())
                    B = &it->second;
            }
            const uint32_t sat = B ? B->mem[followLaunchM].sat : UINT32_MAX;
            if (followLaunchOwner % 2 == 0 && sat < activeSatCount && t >= B->tA - 60.0)
            {
                // A train member arrived: it IS its roster slot now (the camera eases across what is left).
                const glm::dvec3 a = satOrbitStateAt(launchOrbitElems(idx - activeSatCount), t).posEci;
                const glm::dvec3 b = satOrbitStateAt(orbitElemsOf(satOrbits[sat]), t).posEci;
                if (glm::length(a) > 1.0)
                    followHandD += eciToEcef(a - b, t);
                followSatIndex = (int)sat;
                followLaunchOwner = 0;
                snprintf(followLabel, sizeof(followLabel), "Following %s", satLabel((int)sat).c_str());
            }
            else if (followLaunchOwner % 2 == 1 && sat < activeSatCount)
                awaitFireball(3, sat);
            else
                stopFollowHere("Go to ended: the object is gone. You stay here.");
        }
        return;
    }
    const uint32_t j = idx - poolIdx0();
    if (j >= kPool.size() || j >= kPoolGen.size() || kPoolGen[j] != followPoolGen || nowSec < kPool[j].bornSec)
    {
        stopFollowHere("Go to ended: that debris piece does not exist at this time. You stay here.");
        return;
    }
    if (nowSec >= kPool[j].deathSec)
    {
        if (kPool[j].deathSec == kPool[j].decaySec)
            awaitFireball(5, idx);
        else
            stopFollowHere("Go to ended: the piece was destroyed. You stay here.");
    }
}

int SatelliteSim::rocketMeshSlotToTransient(int satIndex)
{
    if (satIndex >= 0 && (uint32_t)satIndex == followSlotIdx())
    {
        // The followed rocket's own mesh (follow's slot): as a selection, the same transient in the selection's slot.
        if (!followIsTransient() || followTrKey == 0)
            return -1;
        selTransientTake(followTrCur);
        selectedSatIndex = (int)transientSatIdx();
        selectedPlanetIndex = -1;
        return (int)transientSatIdx();
    }
    const uint32_t base = activeSatCount + kLaunchCap - 2 - kRocketMeshSlots;
    // A pad's mesh (its tower, mount, tanks) is scenery, a payload stack's (2026-10-20) part of its stage: not
    // selectable.
    if (satIndex >= (int)(base - kPadMeshSlots - kStackMeshSlots - kShipMeshSlots - kArmMeshSlots) && satIndex < (int)base)
        return -1;
    if (satIndex < (int)base || satIndex >= (int)(base + kRocketMeshSlots))
        return satIndex;
    const uint64_t key = rocketMeshKey[satIndex - (int)base];
    for (const SelTransient &s : selTransients)
        if (key != 0 && s.key == key)
        {
            selTransientTake(s);
            selectedSatIndex = (int)transientSatIdx();
            selectedPlanetIndex = -1;
            return (int)transientSatIdx();
        }
    return -1;
}

void SatelliteSim::selTransientTake(const SelTransient &s)
{
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    selTransientKey = s.key;
    selTransientCur = s;
    selTransientSeenT = t;
    if (!launchState)
        return;
    // The list's state is at the time the list was made (the visuals fill), possibly a frame ago.
    const double t0 = selTransientsT > 0.0 ? selTransientsT : t;
    SatOrbitElems &e = launchState->elems[kLaunchCap - 1];
    e = SatOrbitElems{};
    e.fixedState = true;
    e.fixPos = s.pos;
    e.fixVel = s.vel;
    e.fixAcc = s.acc;
    e.fixAxis = s.axis;
    LaunchImpl::transientAtt(e, s);
    LaunchImpl::transientLegs(*launchState, e, s);
    e.engineGlow = ((s.kind == 1 || s.kind == 8) && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
    e.fixT0 = t0;
    // A rocket has a model (Info, Go to, its mesh); a fireball none (Track only). The orbit rows read the state.
    const uint32_t idx = transientSatIdx();
    if (idx < satOrbits.size())
    {
        SatOrbit &o = satOrbits[idx];
        o.typeIdx = s.type >= 0 ? (uint32_t)s.type : UINT32_MAX;
        o.altM = (float)(glm::length(s.pos) - kR);
        const glm::dvec3 h = glm::cross(s.pos, s.vel);
        o.incl = glm::length(h) > 1.0 ? (float)std::acos(glm::clamp(h.z / glm::length(h), -1.0, 1.0)) : 0.0f;
        o.raan = 0.0f;
        o.meanMot = 0.0f;
    }
}

double SatelliteSim::rocketMeshPx(const SelTransient &s, double pixAngle, const glm::dvec3 &obsEci) const
{
    const SatMeshRenderer::TypeMesh *tm = s.type >= 0 ? meshRenderer.typeMesh(s.type) : nullptr;
    if (!tm || pixAngle <= 0.0)
        return 0.0;
    return 2.0 * std::max(0.1, (double)tm->boundsRadius) / std::max(glm::length(s.pos - obsEci), 1.0) / pixAngle;
}

// The rockets near enough to draw as meshes (recordMeshScene): the largest on screen past ~1.4 px, up to
// kRocketMeshSlots, each in a slot of the reserved chunk it keeps while it stays meshed; the selected rocket rides in
// the selection's own slot. Their elements are the frame's state (fixedState), so evalInstance poses them like any
// satellite. Uses the transients the last frame listed (the visuals fill after this pass): one frame of extrapolation.
std::vector<int> SatelliteSim::launchMeshJobs(double pixAngle, const glm::dvec3 &obsEci)
{
    FPROF_SCOPE("launchMeshJobs");
    std::vector<int> jobs;
    if (!launchState)
        return jobs;
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The pads' static meshes (2026-10-19): those over ~1.4 px, nearest first, each kept in its reserved slot while
    // it stays. Ground-fixed: the state is the pad's at t with the Earth's rotation, the "velocity" axis its heading
    // (the model's +X), so the tower stands where the ground pattern draws its base.
    if (launchPadGraphics)
    {
        LaunchImpl::ensureCatalog(L);
        const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
        struct PC
        {
            double px;
            int p;
            int type;
            int kind;
        };
        std::vector<PC> pcs;
        // A slot is keyed by pad AND kind, so a pad rebuilt as another kind (39A's tower) takes a fresh slot.
        auto padKey = [](const PC &c) { return (uint64_t)c.p + 1 + ((uint64_t)(c.kind + 1) << 32); };
        for (size_t pi = 0; pi < L.pads.size(); ++pi)
        {
            const LPad &pd = L.pads[pi];
            // The deployment era: what stands there at this date (none before it is built; 39A a Falcon pad until
            // its Starship tower, SLC-37 a Delta IV pad until it is cleared).
            const int kindT = padKindAt(pd, t, launchDeployEra);
            if (kindT < 0)
                continue;
            auto ti = rocketTypes.find(kindT == 2 ? "pad_starship" : kindT == 1 ? "pad_falcon" : "pad_generic");
            if (ti == rocketTypes.end())
                continue;
            const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh(ti->second);
            if (!tm)
                continue;
            const glm::dvec3 P = ecefToEci((kR + pd.elevM) * pd.g, t);
            const double px = 2.0 * std::max(1.0, (double)tm->boundsRadius) / std::max(glm::length(P - obsEci), 1.0) /
                              std::max(pixAngle, 1e-9);
            if (px > 1.4)
                pcs.push_back({px, (int)pi, ti->second, kindT});
        }
        std::sort(pcs.begin(), pcs.end(), [](const PC &a, const PC &b) { return a.px > b.px || (a.px == b.px && a.p < b.p); });
        if (pcs.size() > (size_t)kPadMeshSlots)
            pcs.resize(kPadMeshSlots);
        bool usedP[kPadMeshSlots] = {};
        int slotP[kPadMeshSlots];
        for (size_t w = 0; w < pcs.size(); ++w)
        {
            slotP[w] = -1;
            for (int k = 0; k < kPadMeshSlots; ++k)
                if (!usedP[k] && L.padMeshKey[k] == padKey(pcs[w]))
                {
                    usedP[k] = true;
                    slotP[w] = k;
                    break;
                }
        }
        for (size_t w = 0; w < pcs.size(); ++w)
            if (slotP[w] < 0)
                for (int k = 0; k < kPadMeshSlots; ++k)
                    if (!usedP[k])
                    {
                        usedP[k] = true;
                        slotP[w] = k;
                        break;
                    }
        for (int k = 0; k < kPadMeshSlots; ++k)
            if (!usedP[k])
                L.padMeshKey[k] = 0;
        const uint32_t pbase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots;
        for (size_t w = 0; w < pcs.size(); ++w)
        {
            const LPad &pd = L.pads[pcs[w].p];
            const int k = slotP[w];
            L.padMeshKey[k] = padKey(pcs[w]);
            const uint32_t slot = pbase + (uint32_t)k, idx = activeSatCount + slot;
            if (idx >= satOrbits.size() || slot >= L.elems.size())
                continue;
            SatOrbitElems &e = L.elems[slot];
            e = SatOrbitElems{};
            e.fixedState = true;
            e.fixPos = ecefToEci((kR + pd.elevM) * pd.g, t);
            e.fixVel = glm::cross(omega, e.fixPos);
            e.fixAcc = glm::cross(omega, e.fixVel);
            e.fixAxis = ecefToEci(pd.u, t);
            e.fixT0 = t;
            satOrbits[idx].typeIdx = (uint32_t)pcs[w].type;
            jobs.push_back((int)idx);
        }
    }
    if (!launchesEnabled)
        return jobs;
    struct C
    {
        double px;
        size_t i;
    };
    std::vector<C> cs;
    for (size_t i = 0; i < selTransients.size(); ++i)
    {
        const SelTransient &s = selTransients[i];
        if (s.type < 0)
            continue;
        const double px = rocketMeshPx(s, pixAngle, obsEci);
        if (px > 1.4)
            cs.push_back({px, i});
    }
    std::sort(cs.begin(), cs.end(), [](const C &a, const C &b) { return a.px > b.px; });
    const uint32_t base = activeSatCount + kLaunchCap - 2 - kRocketMeshSlots; // slots base .. base + 5
    bool used[kRocketMeshSlots] = {};
    std::vector<size_t> want;
    for (const C &c : cs)
    {
        const SelTransient &s = selTransients[c.i];
        if (followIsTransient() && s.key == followTrKey)
            continue; // the followed one is drawn by follow mode (its own slot)
        if (selIsTransient() && s.key == selTransientKey)
        {
            jobs.push_back((int)transientSatIdx()); // the selection's slot (already written by selTransientTake)
            continue;
        }
        if (want.size() < (size_t)kRocketMeshSlots)
            want.push_back(c.i);
    }
    // Keep each rocket in the slot it had; give the rest the free ones.
    int slotOf[kRocketMeshSlots];
    for (int k = 0; k < kRocketMeshSlots; ++k)
        slotOf[k] = -1;
    for (size_t w = 0; w < want.size(); ++w)
        for (int k = 0; k < kRocketMeshSlots; ++k)
            if (!used[k] && rocketMeshKey[k] == selTransients[want[w]].key)
            {
                used[k] = true;
                slotOf[w] = k;
                break;
            }
    for (size_t w = 0; w < want.size(); ++w)
        if (slotOf[w] < 0)
            for (int k = 0; k < kRocketMeshSlots; ++k)
                if (!used[k])
                {
                    used[k] = true;
                    slotOf[w] = k;
                    break;
                }
    for (int k = 0; k < kRocketMeshSlots; ++k)
        if (!used[k])
            rocketMeshKey[k] = 0;
    for (size_t w = 0; w < want.size(); ++w)
    {
        const SelTransient &s = selTransients[want[w]];
        const int k = slotOf[w];
        rocketMeshKey[k] = s.key;
        const uint32_t slot = kLaunchCap - 2 - kRocketMeshSlots + (uint32_t)k;
        const uint32_t idx = base + (uint32_t)k;
        if (idx >= satOrbits.size() || slot >= L.elems.size())
            continue;
        SatOrbitElems &e = L.elems[slot];
        e = SatOrbitElems{};
        e.fixedState = true;
        e.fixPos = s.pos;
        e.fixVel = s.vel;
        e.fixAcc = s.acc;
        e.fixAxis = s.axis;
        LaunchImpl::transientAtt(e, s);
        LaunchImpl::transientLegs(L, e, s);
        e.engineGlow = ((s.kind == 1 || s.kind == 8) && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
        // The transients are the last frame's: their time is the last fill's.
        e.fixT0 = selTransientsT;
        (void)t;
        satOrbits[idx].typeIdx = (uint32_t)s.type;
        jobs.push_back((int)idx);
    }
    launchStackJobs(pixAngle, obsEci, jobs);
    launchShipJobs(pixAngle, obsEci, jobs);
    launchArmJobs(pixAngle, obsEci, jobs);
    return jobs;
}

// The payload stacks (2026-10-20, issue 20261019_225243): the satellites still on an upper stage whose fairing has gone
// and its dispenser, as meshes in the reserved stack slots — the nearest stacks first, each satellite stowed (folded,
// its arrays packed) in its slot's attitude, posed rigidly with the stage (fixedState from the stage's listed state).
// A satellite leaves the stack at its release and is from then on its launch record (memberState: from the same
// point, in the same attitude). Not selectable.
void SatelliteSim::launchStackJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs)
{
    FPROF_SCOPE("launchStackJobs");
    LaunchState &L = *launchState;
    struct Want
    {
        uint64_t key; // batch key ^ (what + 1) << 48: what = member index, or 0xFFFF the dispenser
        int type;
        glm::dvec3 P, V, A;
        glm::dmat3 R;
        double t;
        bool stowed;
    };
    std::vector<Want> want;
    std::vector<std::pair<double, size_t>> order;
    for (size_t i = 0; i < L.stackDraws.size(); ++i)
    {
        const LaunchState::StackDraw &sd = L.stackDraws[i];
        const SatMeshRenderer::TypeMesh *tm = sd.type >= 0 ? meshRenderer.typeMesh(sd.type) : nullptr;
        const double r = tm ? std::max(1.0, (double)tm->boundsRadius) : 10.0;
        const double px = 2.0 * r / std::max(glm::length(sd.P - obsEci), 1.0) / std::max(pixAngle, 1e-9);
        if (px > 1.4)
            order.push_back({px, i});
    }
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &o : order)
    {
        const LaunchState::StackDraw &sd = L.stackDraws[o.second];
        auto bit = L.batches.find(sd.key);
        if (bit == L.batches.end() || !bit->second.valid)
            continue;
        const LBatch &B = bit->second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        const launch::Stack &stk = L.cat.programs[B.prog].stack;
        if (!stk.dispenser.empty())
        {
            auto di = rocketTypes.find("rocket_dispenser_" + stk.dispenser);
            if (di != rocketTypes.end())
                want.push_back({sd.key ^ (0xFFFFull << 48), di->second, sd.P + sd.S * glm::dvec3(0.0, 0.0, vh.payloadBaseM),
                                sd.V, sd.A, sd.S, sd.t, false});
        }
        for (size_t mi = 0; mi < B.mem.size() && want.size() < (size_t)kStackMeshSlots; ++mi)
        {
            const LMember &mb = B.mem[mi];
            if (!mb.stacked || sd.t >= mb.tRel || !LaunchImpl::memberAllowed(*this, B, mb))
                continue;
            const uint32_t ty = satOrbits[mb.sat].typeIdx;
            if (ty >= satTypes.size())
                continue;
            want.push_back({sd.key ^ ((uint64_t)(mi + 1) << 48), (int)ty, sd.P + sd.S * mb.slotO, sd.V, sd.A,
                            sd.S * mb.slotR, sd.t, true});
        }
        if (want.size() >= (size_t)kStackMeshSlots)
            break;
    }
    if (want.size() > (size_t)kStackMeshSlots)
        want.resize(kStackMeshSlots);
    // Keep each object in the slot it had (its mesh fade), give the rest the free ones.
    bool used[kStackMeshSlots] = {};
    std::vector<int> slotOf(want.size(), -1);
    for (size_t w = 0; w < want.size(); ++w)
        for (int k = 0; k < kStackMeshSlots; ++k)
            if (!used[k] && L.stackMeshKey[k] == want[w].key)
            {
                used[k] = true;
                slotOf[w] = k;
                break;
            }
    for (size_t w = 0; w < want.size(); ++w)
        if (slotOf[w] < 0)
            for (int k = 0; k < kStackMeshSlots; ++k)
                if (!used[k])
                {
                    used[k] = true;
                    slotOf[w] = k;
                    break;
                }
    for (int k = 0; k < kStackMeshSlots; ++k)
        if (!used[k])
            L.stackMeshKey[k] = 0;
    const uint32_t sbase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots - kStackMeshSlots;
    L.nStacked = 0;
    for (size_t w = 0; w < want.size(); ++w)
    {
        const Want &W = want[w];
        const int k = slotOf[w];
        L.stackMeshKey[k] = W.key;
        const uint32_t slot = sbase + (uint32_t)k, idx = activeSatCount + slot;
        if (idx >= satOrbits.size() || slot >= L.elems.size())
            continue;
        SatOrbitElems &e = L.elems[slot];
        e = SatOrbitElems{};
        e.fixedState = true;
        e.fixPos = W.P;
        e.fixVel = W.V;
        e.fixAcc = W.A;
        e.fixAxis = W.R[2];
        e.fixT0 = W.t;
        e.attFix = true;
        e.attQ = glm::normalize(glm::quat_cast(W.R));
        e.attT0 = W.t;
        if (W.stowed)
        {
            e.restJoints = true;
            e.unfoldT0 = W.t + 1.0e9; // stowed: progress 0
            e.unfoldDur = 1.0;
        }
        satOrbits[idx].typeIdx = (uint32_t)W.type;
        jobs.push_back((int)idx);
        L.nStacked += W.stowed ? 1u : 0u;
    }
}

std::string SatelliteSim::transientLabel() const { return transientLabelOf(selTransientCur); }

// "42 s", "12 m 03 s", "4 h 12 m", "116 d 17 h" (2026-10-21: a docked Progress read "10086696 s").
static std::string fmtDurationDHM(double s)
{
    s = std::max(0.0, s);
    char b[48];
    const long long n = (long long)std::floor(s);
    if (n < 120)
        snprintf(b, sizeof(b), "%lld s", n);
    else if (n < 3600)
        snprintf(b, sizeof(b), "%lld m %02lld s", n / 60, n % 60);
    else if (n < 86400)
        snprintf(b, sizeof(b), "%lld h %02lld m", n / 3600, (n % 3600) / 60);
    else
        snprintf(b, sizeof(b), "%lld d %02lld h", n / 86400, (n % 86400) / 3600);
    return b;
}

void SatelliteSim::transientTimeRow(const SelTransient &s, std::string &label, std::string &value) const
{
    if (s.kind == 1)
    {
        label = s.tau < 0.0 ? "Lift-off in" : "Since lift-off";
        value = fmtDurationDHM(std::abs(s.tau));
        return;
    }
    if (s.kind == 8 && launchState)
    {
        const LaunchState &L = *launchState;
        auto it = L.batches.find(s.a);
        if (it != L.batches.end())
        {
            const LCrew &C = it->second.crew;
            const double t = C.tSep + s.tau;
            int ph = 0;
            glm::dvec3 P;
            LaunchImpl::crewPos(C, t, P, &ph);
            if (ph == CP_DOCKED || ph == CP_BERTHED)
            {
                label = ph == CP_BERTHED ? "Berthed since" : "Docked since";
                char b[96];
                snprintf(b, sizeof(b), "%s (%.0f days)", fmtUtcReadable(C.tDock).c_str(), std::floor((t - C.tDock) / kDay));
                value = b;
            }
            else if (ph >= CP_UNBERTH && ph < CP_ENTRY && t >= C.tUndock)
            {
                label = ph <= CP_RELEASE ? "Since unberthing" : "Since undocking";
                value = fmtDurationDHM(t - C.tUndock);
            }
            else if (ph >= CP_ENTRY && ph < CP_ABORT && t >= C.tEI)
            {
                label = "Since entry interface";
                value = fmtDurationDHM(t - C.tEI);
            }
            else
            {
                label = "Since separation";
                value = fmtDurationDHM(s.tau);
            }
            return;
        }
    }
    label = s.kind == 7 || s.kind == 8 ? "Since separation" : "Glowing for";
    value = fmtDurationDHM(std::abs(s.tau));
}

std::string SatelliteSim::transientLabelOf(const SelTransient &s) const
{
    if (!launchState)
        return "?";
    const LaunchState &L = *launchState;
    const double hKm = (glm::length(s.pos) - kR) / 1000.0;
    // Speed over the ground for a rocket near the pad reads better Earth-fixed; inertial elsewhere (orbit).
    // (2026-10-20, QA: it was inertial everywhere, so a stack on the pad, a ship in the tower and a booster on its
    // deck read 0.4 km/s, the Earth's rotation. Earth-fixed below 80 km, inertial above 150 km, blended between.)
    double vKms = glm::length(s.vel) / 1000.0;
    {
        const glm::dvec3 vRot = s.vel - glm::dvec3(-satphot::kOmegaEarth * s.pos.y, satphot::kOmegaEarth * s.pos.x, 0.0);
        vKms = glm::mix(glm::length(vRot), glm::length(s.vel), glm::smoothstep(80.0, 150.0, hKm)) / 1000.0;
    }
    char b[200];
    const LBatch *B = nullptr;
    if (s.kind <= 2 || s.kind == 4 || s.kind == 7 || s.kind == 8 || s.kind == 9)
    {
        auto it = L.batches.find(s.a);
        if (it != L.batches.end())
            B = &it->second;
    }
    if (s.kind == 8 && B)
    {
        const LCrew &C = B->crew;
        const double t = C.tSep + s.tau;
        int ph = 0;
        glm::dvec3 P;
        LaunchImpl::crewPos(C, t, P, &ph);
        const launch::Program &pg = L.cat.programs[B->prog];
        const std::string &stn = constellations[L.planes[B->plane].constIdx].name;
        glm::dvec3 Ps;
        glm::dmat3 R;
        LaunchImpl::crewStation(C, t, Ps, R);
        const double dPort = glm::length(P - (Ps + R * C.portPos));
        if (ph == CP_DOCKED || ph == CP_BERTHED)
            snprintf(b, sizeof(b), "%s docked at %s (%s), undocks in %.1f days", pg.spacecraft.c_str(), stn.c_str(),
                     C.portName.c_str(), (C.tUndock - t) / kDay);
        else if (ph >= CP_FAR && ph <= CP_DEPART)
            snprintf(b, sizeof(b), "%s at %s: %s, %.0f m from the port", pg.spacecraft.c_str(), stn.c_str(),
                     kCrewPhaseNames[ph], dPort);
        else if (ph >= CP_ENTRY)
            snprintf(b, sizeof(b), "%s coming home: %s, %.1f km up, %.2f km/s", pg.spacecraft.c_str(), kCrewPhaseNames[ph],
                     hKm, vKms);
        else
            snprintf(b, sizeof(b), "%s to %s: %s, %.0f km up, %.0f km from the station", pg.spacecraft.c_str(), stn.c_str(),
                     kCrewPhaseNames[ph], hKm, dPort / 1000.0);
        return b;
    }
    const char *veh = B && B->veh >= 0 ? L.cat.vehicles[B->veh].name.c_str() : "Rocket";
    switch (s.kind)
    {
    case 1:
    {
        static const char *kBurn[] = {"coasting", "ascent burn", "boostback burn", "entry burn", "landing burn",
                                      "upper-stage burn"};
        const char *what = s.stage == 0 ? " booster" : " upper stage";
        if (B && s.stage == 0 && s.tau < L.cat.vehicles[B->veh].t1)
            what = ""; // still one vehicle
        const char *doing = kBurn[std::min<int>(s.burn, 5)];
        if (s.tau < 0.0)
            doing = "on the pad";
        if (B && s.stage == 1 && s.tau > B->t2)
        {
            const double t = B->tL + s.tau;
            // (2026-10-20, QA: after its entry a returning ship read "falling toward entry" through its descent and
            // "deorbit burn" through its landing burn and in the tower)
            if (B->shipLands && t >= B->tShipDesc)
                doing = t < B->tShipFlip   ? "belly-flop descent"
                        : t < B->tShipCatch ? "flip and landing burn"
                        : B->landPad >= 0   ? "caught by the tower"
                                            : "splashed down";
            else if (s.burn != launch::BURN_NONE)
                doing = "deorbit burn";
            else if (t < B->tD)
                doing = "coasting to deployment";
            else if (!B->mem.empty() && t < B->mem.back().tRel + 30.0)
                doing = "deploying";
            else if (t > B->tDeorbBurn - 150.0 && t < B->tDeorbBurn)
                doing = "turning for its deorbit burn";
            else if (t > B->tDeorbBurn)
                doing = "falling toward entry";
        }
        // Landed (2026-10-20): where it stands.
        if (B && s.stage == 0 && s.burn == launch::BURN_NONE && s.tau >= LaunchImpl::boosterTr(L, *B).tEnd)
        {
            const double t = B->tL + s.tau;
            if (B->landKind == kLandShip)
                snprintf(b, sizeof(b), "%s booster on %s's deck, %s, %.0f km from home", veh,
                         L.cat.vehicles[B->veh].ships[(size_t)B->ship].name.c_str(),
                         t < B->tShipDepart ? "being secured" : "heading home",
                         std::acos(glm::clamp(glm::dot(LaunchImpl::shipEcef(*B, t) / kR, B->G0), -1.0, 1.0)) * kR / 1000.0);
            else
                snprintf(b, sizeof(b), "%s booster, landed %s, T+%.0f s", veh,
                         B->landKind == kLandTower ? "in the tower" : B->landKind == kLandLz ? "on its landing zone"
                                                                                               : "downrange",
                         s.tau);
            break;
        }
        snprintf(b, sizeof(b), "%s%s (%s), T%s%.0f s, %.0f km up, %.2f km/s", veh, what, doing, s.tau < 0.0 ? "-" : "+",
                 std::abs(s.tau), hKm, vKms);
        break;
    }
    case 2:
        snprintf(b, sizeof(b), "%s ship re-entering, %.0f km up, %.2f km/s", veh, hKm, vKms);
        break;
    case 3:
        snprintf(b, sizeof(b), "Re-entry: %s (retired), %.0f km up", satLabel((int)s.a).c_str(), hKm);
        break;
    case 4:
        snprintf(b, sizeof(b), "Re-entry: %s upper stage, %.0f km up", veh, hKm);
        break;
    case 5:
        snprintf(b, sizeof(b), "Re-entry: %s, %.0f km up", satLabel((int)s.a).c_str(), hKm);
        break;
    case 9:
        snprintf(b, sizeof(b), "Re-entry: %s (burning up), %.0f km up",
                 B ? L.cat.programs[B->prog].spacecraft.c_str() : "cargo ship", hKm);
        break;
    case 7:
        snprintf(b, sizeof(b), "%s payload fairing half, %.0f s after separation, %.0f km up, %.2f km/s", veh, s.tau, hKm,
                 vKms);
        break;
    default:
        snprintf(b, sizeof(b), "Re-entry: debris fragment, %.0f km up", hKm);
        break;
    }
    return b;
}

bool SatelliteSim::launchFocusListed() const
{
    // A list made before the jump (another time) does not count: its stages carry the old T-.
    if (std::fabs(selTransientsT - ((double)simDayJ2000 * 86400.0 + simSecInDay)) > 1.0)
        return false;
    for (const SelTransient &s : selTransients)
        if (s.kind == 1 && s.a == launchFocusKey && launchFocusKey != 0)
            return true;
    return false;
}

bool SatelliteSim::selectTransient(int kind, std::string &msg, int stage, bool nearest)
{
    const SelTransient *best = nullptr;
    const glm::dvec3 eye(obsECI);
    // Rockets (2026-10-20): the launch the last jump framed (`launch next`, `launch deploy`, a Missions row's Go) comes
    // first, drawn or not (on the pad before lift-off its stack is not drawn). Without a stage, the stage that flies on:
    // the stack / booster until staging, then the upper stage while it burns or is drawn, else the brighter.
    if (kind == 1 && launchFocusKey != 0)
    {
        for (const SelTransient &s : selTransients)
        {
            if (s.kind != 1 || s.a != launchFocusKey || !(stage < 0 || s.stage == stage) || !(s.type >= 0 || s.flare > 0.0f))
                continue;
            if (!best)
            {
                best = &s;
                continue;
            }
            if (stage < 0)
            {
                // Prefer the upper stage once it exists (it is listed only after staging), unless the booster is the
                // one drawn and the upper stage is not.
                const bool sUp = s.stage == 1, bUp = best->stage == 1;
                if (sUp != bUp)
                {
                    const SelTransient &up = sUp ? s : *best, &lo = sUp ? *best : s;
                    best = (up.flare > 0.0f || up.burn != 0 || lo.flare <= 0.0f) ? &up : &lo;
                }
            }
        }
    }
    // Otherwise: never a booster standing on its pad or a ship after its flight (with nearest=1 another launch's landed
    // booster beside the eye won over the one about to fly). Landed = over its stage time, < 2 km up, < 20 m/s over
    // the ground, engines off.
    auto landed = [&](const SelTransient &s) {
        if (s.kind != 1 || s.tau < 60.0 || s.burn != 0 || glm::length(s.pos) - kR > 2000.0)
            return false;
        const glm::dvec3 vg = s.vel - glm::cross(glm::dvec3(0.0, 0.0, satphot::kOmegaEarth), s.pos);
        return glm::length(vg) < 20.0;
    };
    for (const SelTransient &s : selTransients)
    {
        if (best && kind == 1 && launchFocusKey != 0 && best->a == launchFocusKey)
            break;
        if (kind == 1 && landed(s))
            continue;
        // nearest (2026-10-20, harness `launch select ... nearest=1`): any listed one with a model, drawn or not.
        const bool ok = nearest ? (s.type >= 0 || s.flare > 0.0f) : s.flare > 0.0f;
        if (!ok || !(kind == 0 || s.kind == kind) || !(stage < 0 || s.stage == stage))
            continue;
        if (!best || (nearest ? glm::length(s.pos - eye) < glm::length(best->pos - eye) : s.flare > best->flare))
            best = &s;
    }
    if (!best)
    {
        msg = "nothing of that kind drawn this frame";
        return false;
    }
    selTransientTake(*best);
    selectedPlanetIndex = -1;
    selectedSatIndex = (int)transientSatIdx();
    formatSelectedSatInfo();
    updateSelectedSkyDir();
    msg = transientLabel();
    return true;
}

// ── Missions window queries (2026-10-19, SatelliteSimMissions.cpp draws them) ───────────────────────────────────
namespace
{
// The inverse of batchKey.
void batchKeyDecode(uint64_t key, int &plane, int64_t &k, int &b)
{
    plane = (int)(uint32_t)(key >> 40);
    k = (int64_t)((key >> 20) & 0xFFFFFull) - (int64_t)(1 << 19);
    b = (int)(key & 0xFFFFFull);
}

double sunElevAtPad(const launch::Site &st, double t)
{
    const glm::dvec3 sun = sunDirEciAt(t);
    return glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(st.ecef, t)), -1.0, 1.0)));
}
} // namespace

void SatelliteSim::launchUpcoming(double t0, double t1, int filter, size_t maxN, std::vector<MissionLaunch> &out)
{
    FPROF_SCOPE("launchUpcoming");
    launchUpcomingIncomplete = false;
    if (!launchState || !launchesEnabled)
    {
        out.clear();
        return;
    }
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    LaunchImpl::crewSync(*this, L);
    struct Item
    {
        double tL;
        uint64_t key;
    };
    std::vector<Item> items;
    // (2026-10-21) The batches not built yet are built first, in parallel (LaunchImpl::buildMissing): one by one on the
    // main thread a Missions refresh after a jump took 65-230 ms (ship-landing scans, the rivals' day buckets). The
    // same batches either way (a pure function of the batch).
    {
        FPROF_SCOPE("launchUpcoming.build");
        std::vector<LaunchImpl::ToBuild> toBuild;
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
        {
            const LPlane &pl = L.planes[pi];
            const launch::Program &pg = L.cat.programs[pl.prog];
            if (filter == 1 && (!pg.crew || pg.cargo))
                continue;
            const double life = progLifeS(pg, (double)launchLifeScale);
            const double lo = t0 + (pg.raiseDays[0] - 0.5 - pg.earlyDays()) * kDay, hi = t1 + (pg.raiseDays[1] + 2.5) * kDay;
            LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
                const uint64_t key = batchKey((int)pi, k, b);
                if (L.batches.find(key) == L.batches.end())
                    toBuild.push_back({key, (int)pi, k, b, life});
            });
        }
        // With a budget (launchUpcomingBudgetMs, the Missions window: 8 ms), in pieces until it is spent; then `out` is
        // left as it was and launchUpcomingIncomplete set (the caller asks again next frame, the batches built so far
        // kept).
        if (launchUpcomingBudgetMs > 0.0)
        {
            // The window's own refresh: its builds on their worker (LaunchState::UpJob), the rows when they are in.
            if (L.upJob && L.upJob->finished.load(std::memory_order_acquire))
            {
                if (L.upJob->th.joinable())
                    L.upJob->th.join();
                if (L.upJob->gen == L.buildGen)
                    for (size_t q = 0; q < L.upJob->items.size(); ++q)
                    {
                        auto ins = L.batches.emplace(L.upJob->items[q].key, std::move(L.upJob->out[q]));
                        ins.first->second.touched = std::max(ins.first->second.touched, L.frame);
                    }
                L.upJob.reset();
                L.upKeys.clear();
            }
            bool pending = false;
            std::vector<LaunchImpl::ToBuild> rest;
            for (const auto &tb : toBuild)
            {
                if (L.jobKeys.count(tb.key) || L.upKeys.count(tb.key))
                    pending = true;
                else if (L.batches.find(tb.key) == L.batches.end())
                    rest.push_back(tb);
            }
            if (!rest.empty() && !L.upJob)
            {
                auto J = std::make_unique<LaunchState::UpJob>();
                J->items = std::move(rest);
                J->gen = L.buildGen;
                for (const auto &it : J->items)
                    L.upKeys.insert(it.key);
                LaunchState::UpJob *jp = J.get();
                LaunchState *Lp = &L;
                J->th = std::thread([this, Lp, jp] {
                    LaunchImpl::gBuildBg = true;   // (leaves two cores to the frame, as the walk's job)
                    LaunchImpl::buildMissing(*this, *Lp, jp->items, jp->out);
                    jp->finished.store(true, std::memory_order_release);
                });
                L.upJob = std::move(J);
                pending = true;
            }
            else if (!rest.empty())
                pending = true;
            if (pending)
            {
                launchUpcomingIncomplete = true;
                return;
            }
            toBuild.clear();
        }
        const auto b0 = std::chrono::steady_clock::now();
        size_t done = 0;
        std::vector<LBatch> built;
        double each = std::max(L.buildMsEach, 0.05);   // ms a build, as last measured
        while (done < toBuild.size())
        {
            // Pieces sized to half the budget by the measured cost of a build (perf4-cpu 2026-10-24: pieces of 16 after a
            // jump, the day's rival buckets cold at ~5-7 ms a build, were 14-30 ms a frame for several frames).
            const size_t piece = launchUpcomingBudgetMs > 0.0
                                     ? (size_t)std::clamp(0.5 * launchUpcomingBudgetMs / each, 1.0, done == 0 ? 2.0 : 16.0)
                                     : toBuild.size();
            const size_t e = std::min(toBuild.size(), done + piece);
            const std::vector<LaunchImpl::ToBuild> part(toBuild.begin() + (ptrdiff_t)done, toBuild.begin() + (ptrdiff_t)e);
            const auto p0 = std::chrono::steady_clock::now();
            LaunchImpl::buildMissing(*this, L, part, built);
            for (size_t q = 0; q < part.size(); ++q)
                L.batches.emplace(part[q].key, std::move(built[q]));
            each = std::max(0.05, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count() /
                                      (double)part.size());
            done = e;
            if (launchUpcomingBudgetMs > 0.0 && done < toBuild.size() &&
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - b0).count() > launchUpcomingBudgetMs)
            {
                launchUpcomingIncomplete = true;
                return;
            }
        }
    }
    out.clear();
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        if (filter == 1 && (!pg.crew || pg.cargo))
            continue;
        const double life = progLifeS(pg, (double)launchLifeScale);
        // As launchListJson: lift-off precedes the nominal arrival by the raise (+ the window).
        const double lo = t0 + (pg.raiseDays[0] - 0.5 - pg.earlyDays()) * kDay, hi = t1 + (pg.raiseDays[1] + 2.5) * kDay;
        LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
            const uint64_t key = batchKey((int)pi, k, b);
            auto it = L.batches.find(key);
            if (it == L.batches.end())
            {
                LBatch nb;
                nb.plane = (int)pi;
                nb.k = k;
                nb.b = b;
                LaunchImpl::buildBatch(*this, L, nb, pl, life);
                it = L.batches.emplace(key, std::move(nb)).first;
            }
            LBatch &B = it->second;
            B.touched = std::max(B.touched, L.frame); // kept while the window lists it
            if (!B.valid || B.tL < t0 || B.tL > t1)
                return;
            if (filter == 2)
            {
                const double se = sunElevAtPad(L.cat.sites[B.site], B.tL);
                if (!(se < -5.0 && se > -10.0)) // (the jellyfish window: m.twilight below)
                    return;
            }
            items.push_back({B.tL, key});
        });
    }
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.tL < b.tL || (a.tL == b.tL && a.key < b.key); });
    for (size_t i = 0; i < items.size() && out.size() < maxN; ++i)
    {
        const LBatch &B = L.batches.at(items[i].key);
        const LPlane &pl = L.planes[B.plane];
        const launch::Program &pg = L.cat.programs[B.prog];
        MissionLaunch m;
        m.key = items[i].key;
        m.tL = B.tL;
        m.tA = B.tA;
        m.sunElevDeg = sunElevAtPad(L.cat.sites[B.site], B.tL);
        m.twilight = m.sunElevDeg < -5.0 && m.sunElevDeg > -10.0; // (2026-10-24: -16; below ~-10 the exhaust bells stay in the
                                                                  // Earth's shadow up to ~150 km and a Watch showed a dark sky)
        m.crew = pg.crew && !pg.cargo;
        m.cargo = pg.cargo;
        m.people = pg.people;
        m.payload = (int)B.mem.size();
        m.vehicle = L.cat.vehicles[B.veh].name;
        m.program = pg.name;
        m.site = L.cat.sites[B.site].name;
        m.target = constellations[pl.constIdx].name;
        m.spacecraft = pg.spacecraft;
        m.firstFill = B.first;
        out.push_back(std::move(m));
    }
}

bool SatelliteSim::launchGoToKey(uint64_t key, bool moveObserver, double leadS, std::string &msg, double distM)
{
    if (!launchState || !launchesEnabled)
    {
        msg = "launches are off";
        return false;
    }
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
    {
        msg = "no launch data";
        return false;
    }
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    launchFocusKey = key;
    auto it = L.batches.find(key);
    if (it == L.batches.end())
    {
        // Dropped from the cache since it was listed: build it again (a pure function of its key).
        int plane = 0, b = 0;
        int64_t k = 0;
        batchKeyDecode(key, plane, k, b);
        if (plane < 0 || plane >= (int)L.planes.size())
        {
            msg = "launch not found";
            return false;
        }
        LBatch nb;
        nb.plane = plane;
        nb.k = k;
        nb.b = b;
        const LPlane &pl = L.planes[plane];
        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
        nb.touched = L.frame;
        it = L.batches.emplace(key, std::move(nb)).first;
    }
    const LBatch &B = it->second;
    if (!B.valid)
    {
        msg = "that launch does not fly (" + B.why + ")";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tStart = B.tL - leadS;
    const double days = std::floor(tStart / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tStart - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const launch::Site &st = L.cat.sites[B.site];
    if (moveObserver)
    {
        // On the ground distM (12 km) from the pad, beside the ground track and a little behind it (the climb crosses the
        // sky sideways, and the staging stays in view). The first candidate on land (the DEM reads ~0 over the sea).
        const glm::dvec3 side = glm::normalize(glm::cross(B.G0, B.Eaz));
        const double dd = distM;
        const glm::dvec3 cand[6] = {B.G0 * kR + side * dd - B.Eaz * (0.25 * dd), B.G0 * kR - side * dd - B.Eaz * (0.25 * dd),
                                    B.G0 * kR - B.Eaz * dd, B.G0 * kR + side * (0.5 * dd) - B.Eaz * (0.125 * dd),
                                    B.G0 * kR - side * (0.5 * dd) - B.Eaz * (0.125 * dd), B.G0 * kR - B.Eaz * (0.5 * dd)};
        glm::dvec3 g = glm::normalize(cand[0]);
        for (const glm::dvec3 &c : cand)
        {
            const glm::dvec3 u = glm::normalize(c);
            const float la = (float)glm::degrees(std::asin(glm::clamp(u.z, -1.0, 1.0)));
            const float lo = (float)glm::degrees(std::atan2(u.y, u.x));
            double ck = 0.0;
            const double cd = coastPatchAt(L.coasts, u, ck); // (2026-10-20) a coast patch's land counts (DEM 0 there)
            if (cpuTerrainHeightM(la, lo) > 10.0f || (ck > 0.99 && cd < -300.0))
            {
                g = u;
                break;
            }
        }
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
        obsHeightOffset = 0.0f; // on the ground (the height is above sea level, floored at the ground)
        // The camera on the pad, the pad low in the frame (the climb rises through the middle).
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const glm::dvec3 rel = B.G0 * kR - up * kR;
        glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
        east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
        const glm::dvec3 north = glm::cross(up, east);
        const glm::dvec3 rn = glm::normalize(rel);
        camera.fovYDeg = 60.0f;
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                      (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))) + 12.0f);
    }
    updatePositions(tStart);
    skyTaaHistValid = false; // a cut
    cv2HistoryValid = false;
    char b[320];
    if (L.cat.programs[B.prog].crew)
        snprintf(b, sizeof(b), "%s to %s from %s: lift-off in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 constellations[L.planes[B.plane].constIdx].name.c_str(), st.name.c_str(), leadS);
    else
        snprintf(b, sizeof(b), "%s (%s) from %s: lift-off in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 L.cat.programs[B.prog].name.c_str(), st.name.c_str(), leadS);
    msg = b;
    return true;
}

// WATCH a launch (2026-10-24, the user: "When a use wants to watch a lanch, they should just get one button that takes
// them to a rocket launching" — "1 minute before" Go to's put playtesters at an empty pad). afterS after lift-off at 1x,
// on the ground 5 km from the pad (40 km for a late moment: the twilight plume high up, its bell hundreds of km), the rocket's stage selected and
// tracked once the transient list has it (selTransientUpdate: launchWantSelKey / launchWantTrack).
// The cloud map's cover at an Earth-fixed direction (the CPU copy, drifted as drawn; 0 clear .. 1 overcast).
float SatelliteSim::cloudCoverAtEcef(const glm::dvec3 &e) const
{
    if (earthCloudsCpu.empty())
        return 0.0f;
    const double lat = std::asin(glm::clamp(e.z, -1.0, 1.0)), lon = std::atan2(e.y, e.x);
    double u = (lon + glm::pi<double>()) / glm::two_pi<double>() + cloudDriftPhase() / glm::two_pi<double>();
    u -= std::floor(u);
    const double v = (0.5 * glm::pi<double>() - lat) / glm::pi<double>();
    const int x = std::clamp((int)(u * earthCloudsCpuW), 0, earthCloudsCpuW - 1);
    const int y = std::clamp((int)(v * earthCloudsCpuH), 0, earthCloudsCpuH - 1);
    const float b = earthCloudsCpu[(size_t)y * earthCloudsCpuW + x] / 255.0f;
    const float span = std::max(cv2CoverFull - cv2CoverClear, 1e-3f);
    return std::clamp((b * cv2Coverage - cv2CoverClear) / span, 0.0f, 1.0f);
}

bool SatelliteSim::launchWatchKey(uint64_t key, double afterS, std::string &msg)
{
    // (2026-10-26, issue 20261026_193645) T-5 s at 1x (kWatchLeadS; it was T-60 s, the user: too long a wait), on the ground beside the pad (12 km for a twilight launch: its
    // exhaust rises over most of the sky), the camera on the stack. NOT tracked: the player watches it climb (a tracked
    // Watch landed on a stage they did not expect); the stack is selected so its label and Info are one click away.
    // `afterS` (kWatchLiftoffS / kWatchTwilightS) only says which kind of Watch it is.
    const bool twilight = afterS > 60.0;
    const double leadS = kWatchLeadS;
    if (!launchGoToKey(key, true, leadS, msg, twilight ? 12000.0 : 5000.0))
        return false;
    LaunchState &L = *launchState;
    auto it = L.batches.find(key);
    if (it == L.batches.end())
        return true;
    const LBatch &B = it->second;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    selTransientKey = 0;
    launchWantSelKey = (key << 3) ^ 1ull; // the stack on its pad
    launchWantTrack = false;
    launchWantSelUntil = B.tL + 120.0;
    camera.fovYDeg = twilight ? 60.0f : 40.0f;
    launchWatchLitT = -1.0;
    launchWatchScanN = 0;
    const launch::Site &st = L.cat.sites[B.site];
    const launch::Program &pr = L.cat.programs[B.prog];
    char b[320];
    const std::string what = pr.crew ? (std::string("to ") + constellations[L.planes[B.plane].constIdx].name) : pr.name;
    snprintf(b, sizeof(b), "%s (%s) on the pad at %s: lift-off in %.0f s%s", vh.name.c_str(), what.c_str(), st.name.c_str(),
             leadS, twilight ? " (twilight: its exhaust will catch the Sun high up)" : "");
    msg = b;
    return true;
}

// The next Starship ship caught by its tower, in its landing burn (25 s before the catch), 2.5 km from the pad, the
// ship tracked.
bool SatelliteSim::launchWatchCatch(std::string &msg)
{
    if (!launchGoToLanding(0, 25.0, 2500.0, msg))
        return false;
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    selTransientKey = 0;
    launchWantSelKey = (launchFocusKey << 3) ^ 2ull; // the ship: the upper stage's key
    launchWantTrack = true;
    launchWantSelUntil = (double)simDayJ2000 * 86400.0 + simSecInDay + 60.0;
    msg = "A Starship ship coming home to its tower: the flip and landing burn, then the catch";
    return true;
}

void SatelliteSim::launchOngoing(std::vector<MissionOngoing> &out)
{
    FPROF_SCOPE("launchOngoing");
    out.clear();
    if (!launchState || !launchesEnabled)
        return;
    const LaunchState &L = *launchState;
    if (!L.loaded)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    struct Agg
    {
        int batches = 0, sats = 0;
        double next = 1e300, progSum = 0.0;
    };
    std::map<int, Agg> trains, retire;
    auto drawnRocket = [&](uint64_t key) {
        for (const SelTransient &s : selTransients)
            if ((s.kind == 1 || s.kind == 8) && s.a == key && s.flare > 0.0f)
                return true;
        return false;
    };
    auto phaseOf = [&](const LBatch &B, double tau, double &tNext) -> std::string {
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        if (tau < vh.t1)
        {
            tNext = B.tL + vh.t1;
            return "first stage burning; staging";
        }
        if (tau < B.t2)
        {
            tNext = B.tL + B.t2;
            return B.rec == 1   ? "upper stage burning, booster returning; orbit"
                   : B.landKind == kLandShip ? "upper stage burning, booster to the droneship; orbit"
                   : B.rec == 2 ? "upper stage burning, booster landing downrange; orbit"
                                      : "upper stage burning, booster falling; orbit";
        }
        if (B.tL + tau < B.tD)
        {
            tNext = B.tD;
            return L.cat.programs[B.prog].crew ? "in the parking orbit; separation" : "in the parking orbit; deployment";
        }
        tNext = vh.upperDeorbit ? B.tStageRE : B.tStageGone;
        return vh.upperDeorbit ? "deployed; upper stage re-entry" : "deployed; ship coming home";
    };
    for (uint64_t key : L.live)
    {
        auto bit = L.batches.find(key);
        if (bit == L.batches.end() || !bit->second.valid)
            continue;
        const LBatch &B = bit->second;
        const launch::Program &pg = L.cat.programs[B.prog];
        const std::string &target = constellations[L.planes[B.plane].constIdx].name;
        const std::string &veh = L.cat.vehicles[B.veh].name;
        if (pg.crew)
        {
            // (2026-10-20) a crew spacecraft is ongoing until it is home (docked for months in between)
            if (t < B.tL || t >= (B.crew.on ? B.crew.tGone : B.tA))
                continue;
            MissionOngoing m;
            m.cat = 0;
            m.key = key;
            m.title = pg.spacecraft + " to " + target +
                      (pg.cargo ? std::string(" (cargo)") : " (" + std::to_string(pg.people) + " crew)");
            double tLost = 0.0;
            const int fate = rosterFateAt(L.planes[B.plane].sats[0], t, &tLost);
            const bool flying = B.crew.on && t >= B.crew.tSep;
            if (fate != 0 && !flying)
            {
                // Launched before its station was lost (issue 20261019_231126): nothing to dock with.
                m.status = std::string("The station is ") + rosterFateName(fate) + " (" + fmtUtc(tLost) +
                           "); rendezvous called off";
                m.tNext = 0.0;
            }
            else if (t < B.tL + B.t2)
                m.status = veh + ": " + phaseOf(B, t - B.tL, m.tNext);
            else if (B.crew.on && t >= B.crew.tSep)
            {
                // The spacecraft's own phase and what comes next.
                const LCrew &C = B.crew;
                int ph = 0;
                glm::dvec3 P;
                LaunchImpl::crewPos(C, t, P, &ph);
                const double evs[] = {C.tB1, C.tB1e, C.tB2, C.tArr, C.tAxis, C.hold[0], C.hold[1], C.hold[2], C.tDock,
                                      C.tUndock, C.tDeorb, C.tEI, C.tChute, C.tSplash, C.tGone};
                static const char *const kNext[] = {"phasing burn", "phasing burn", "phasing burn", "approach",
                                                    "docking axis", "hold at 400 m", "hold at 200 m", "hold at 20 m",
                                                    "docking", "undocking", "deorbit burn", "entry interface",
                                                    "parachutes", C.lands ? "landing" : "splashdown", "recovery"};
                m.tNext = C.tGone;
                std::string nx = C.burns ? "burns up on entry" : "recovery";
                // (an aborted flight's skipped events sit at its abort time; a ship that burns up has no parachutes)
                for (size_t i = 0; i < sizeof(evs) / sizeof(evs[0]); ++i)
                    if (evs[i] > t && evs[i] < 1e299)
                    {
                        m.tNext = evs[i];
                        nx = (C.burns && i == 11) ? "burns up on entry" : kNext[i];
                        break;
                    }
                std::string phs = kCrewPhaseNames[ph];
                phs[0] = (char)std::toupper((unsigned char)phs[0]);
                m.status = phs + ((ph == CP_DOCKED || ph == CP_BERTHED) ? " (" + C.portName + ")" : std::string()) + "; " + nx;
                if (fate != 0)
                    m.status = std::string("The station is ") + rosterFateName(fate) + " (" + fmtUtc(tLost) + "). " + m.status;
            }
            else
            {
                m.status = "Rendezvous with the station; docking";
                m.tNext = B.tA;
            }
            m.progress = B.crew.on && t >= B.crew.tDock
                             ? (t < B.crew.tUndock ? (t - B.crew.tDock) / std::max(1.0, B.crew.tUndock - B.crew.tDock)
                                                   : (t - B.crew.tUndock) / std::max(1.0, B.crew.tGone - B.crew.tUndock))
                             : (t - B.tL) / std::max(1.0, B.tA - B.tL);
            m.drawn = drawnRocket(key);
            out.push_back(std::move(m));
            continue;
        }
        if (t >= B.tL && t < B.tStageGone)
        {
            MissionOngoing m;
            m.cat = 2;
            m.key = key;
            m.title = veh + " - " + pg.name + " (" + std::to_string(B.mem.size()) + " satellites to " + target + ")";
            m.status = phaseOf(B, t - B.tL, m.tNext);
            m.status[0] = (char)std::toupper((unsigned char)m.status[0]);
            m.progress = (t - B.tL) / std::max(1.0, B.tStageGone - B.tL);
            m.drawn = drawnRocket(key);
            out.push_back(std::move(m));
        }
        if (t >= B.tD && t < B.tA)
        {
            Agg &a = trains[B.prog];
            ++a.batches;
            a.sats += (int)B.mem.size();
            a.next = std::min(a.next, B.tA);
            a.progSum += (t - B.tD) / std::max(1.0, B.tA - B.tD);
        }
        if (t >= B.tA && !B.ends.empty() && B.ends.back() > t)
        {
            Agg &a = retire[B.prog];
            auto it = std::upper_bound(B.ends.begin(), B.ends.end(), t);
            a.sats += (int)(B.ends.end() - it);
            a.next = std::min(a.next, *it);
            ++a.batches;
        }
    }
    std::sort(out.begin(), out.end(), [](const MissionOngoing &a, const MissionOngoing &b) {
        return a.cat < b.cat || (a.cat == b.cat && a.tNext < b.tNext);
    });
    for (const auto &kv : trains)
    {
        MissionOngoing m;
        m.cat = 3;
        m.prog = kv.first;
        m.count = kv.second.sats;
        m.title = L.cat.programs[kv.first].name + ": " + std::to_string(kv.second.batches) +
                  (kv.second.batches == 1 ? " train raising" : " trains raising");
        m.status = std::to_string(kv.second.sats) + " satellites climbing to their slots; next arrival";
        m.tNext = kv.second.next;
        m.progress = kv.second.progSum / std::max(1, kv.second.batches);
        out.push_back(std::move(m));
    }
    for (const auto &kv : retire)
    {
        MissionOngoing m;
        m.cat = 4;
        m.prog = kv.first;
        m.count = kv.second.sats;
        m.title = L.cat.programs[kv.first].name + ": " + std::to_string(kv.second.sats) + " retired, deorbiting";
        m.status = "Lowering toward re-entry; next re-entry";
        m.tNext = kv.second.next;
        out.push_back(std::move(m));
    }
}

std::vector<SatelliteSim::CrewStation> SatelliteSim::launchCrewStations()
{
    std::vector<CrewStation> out;
    if (!launchState)
        return out;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return out;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    for (const LPlane &pl : L.planes)
        if (L.cat.programs[pl.prog].crew && !L.cat.programs[pl.prog].cargo && !pl.extra && !pl.sats.empty())
            out.push_back({(int)pl.constIdx, (int)pl.sats[0], pl.prog});
    return out;
}

bool SatelliteSim::launchRocketTransient(uint64_t key, double t, SelTransient &out) const
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(key);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const double tau = t - B.tL;
    // The payload's vehicle: the stack before staging, the upper stage after (its ship entry, its landing).
    SelTransient c;
    c.kind = 1;
    c.a = key;
    c.stage = tau < vh.t1 ? 0 : 1;
    c.tau = std::min(tau, vh.t1 - 1e-3); // transientContinue moves a stack past staging itself
    c.key = (key << 3) ^ (uint64_t)(c.stage + 1);
    auto ti = rocketTypes.find("rocket_" + vh.id + (c.stage == 1 ? "_upper" : "_stack"));
    c.type = ti != rocketTypes.end() ? ti->second : -1;
    uint8_t want = 0;
    if (!transientContinue(c, t, want))
        return false;
    if (c.kind == 1)
    {
        glm::dvec3 ax = LaunchImpl::stageNose(L, B, c.stage, c.tau);
        c.axis = glm::length(ax) > 0.5 ? ax : glm::normalize(c.pos);
    }
    const glm::dvec3 rel = c.pos - glm::dvec3(obsECI);
    const double d = glm::length(rel);
    if (d > 1.0)
        c.dirEnu = glm::vec3(glm::dot(rel, glm::dvec3(eci2enuX)) / d, glm::dot(rel, glm::dvec3(eci2enuY)) / d,
                             glm::dot(rel, glm::dvec3(eci2enuZ)) / d);
    c.flare = 0.0f;
    out = c;
    return true;
}

bool SatelliteSim::launchSelectRocketOf(uint64_t key, std::string &msg)
{
    // (2026-10-20) A crew flight past its separation: its spacecraft (flying, docked or coming home).
    if (launchState)
    {
        auto bi = launchState->batches.find(key);
        const double tNow = selTransientsT > 0.0 ? selTransientsT : (double)simDayJ2000 * 86400.0 + simSecInDay;
        SelTransient cs;
        if (bi != launchState->batches.end() && bi->second.crew.on && tNow >= bi->second.crew.tSep &&
            LaunchImpl::crewFill(*this, *launchState, bi->second, key, tNow, cs))
        {
            for (const SelTransient &s : selTransients)
                if (s.key == cs.key)
                    cs = s;
            selTransientTake(cs);
            selectedPlanetIndex = -1;
            selectedSatIndex = (int)transientSatIdx();
            formatSelectedSatInfo();
            updateSelectedSkyDir();
            msg = transientLabel();
            return true;
        }
    }
    const SelTransient *best = nullptr;
    for (const SelTransient &s : selTransients)
        if ((s.kind == 1 || s.kind == 2) && s.a == key && (!best || s.stage > best->stage))
            best = &s; // the upper stage (the payload's) once it flies alone
    // (2026-10-20, issue 20261019_224758) Drawn or not: the list holds rockets within 4000 km only (and the selected one
    // at any range); beyond, the launch model places it. It was "not drawn from here" whenever the head was not drawn.
    SelTransient made;
    if (!best)
    {
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        if (!launchRocketTransient(key, selTransientsT > 0.0 ? selTransientsT : tNow, made))
        {
            msg = "that rocket is not flying now";
            return false;
        }
        best = &made;
    }
    selTransientTake(*best);
    selectedPlanetIndex = -1;
    selectedSatIndex = (int)transientSatIdx();
    formatSelectedSatInfo();
    updateSelectedSkyDir();
    msg = transientLabel();
    return true;
}

bool SatelliteSim::launchSelectNearestOf(int prog, int kind, std::string &msg)
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 up = glm::normalize(eye);
    double best = 1e30, bestAny = 1e30;
    int bestSlot = -1, bestSlotAny = -1;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
    {
        const SlotInfo &si = L.slots[s];
        if (!si.shown || (int)(si.owner % 2) != kind)
            continue;
        auto it = L.batches.find(si.owner / 2);
        if (it == L.batches.end() || it->second.prog != prog)
            continue;
        const glm::dvec3 rel = satOrbitStateAt(L.elems[s], t).posEci - eye;
        const double d = glm::length(rel);
        if (d < bestAny)
        {
            bestAny = d;
            bestSlotAny = (int)s;
        }
        if (glm::dot(rel / d, up) < 0.05 || d >= best)
            continue;
        best = d;
        bestSlot = (int)s;
    }
    const bool above = bestSlot >= 0;
    if (!above)
    {
        bestSlot = bestSlotAny;
        best = bestAny;
    }
    if (bestSlot < 0)
    {
        msg = "none up right now";
        return false;
    }
    selectSatellite((int)(activeSatCount + (uint32_t)bestSlot));
    char b[240];
    snprintf(b, sizeof(b), "%s, %.0f km away%s", launchLabel((uint32_t)bestSlot).c_str(), best / 1000.0,
             above ? "" : " (below your horizon)");
    msg = b;
    return true;
}

// ── Launch pads on the ground (2026-10-19) ──────────────────────────────────────────────────────────────────────
// The user: "Launchpads and landing pads should probably have specialized graphics / play well with the city light
// graphics; we may need to establish exclusion zones near launch sites for rendering city grids." Three parts:
//  - the flat parts are a ground pattern in sat_sky.frag (padGround: graded ground, concrete apron, flame trench /
//    deflector, tank farms, roads, a landing zone; exactly box-filtered, so they average out with the footprint and
//    a pad from orbit is a pale speck, never a blob), with the terrain graded flat under them (terrain_detail.glsl
//    tdPadFlat) and no city grid, glitter, farms or city sprites within exclR (sat_sky.frag, city_sprites.comp);
//  - the tower (Starship: the catch tower with its chopsticks over the mount), the mount, masts and tanks as a static
//    mesh (data/satellite_models/pad_*.json, tools/make_pad_models.py), in reserved launch slots (launchMeshJobs);
//  - the floodlights, the beacons and the landing zone's lights at night as point sprites (launchFillPadLights).
// Everything in the pad's (u, v) frame: u its heading (the middle of its launch azimuths), v = up x u.

static glm::dvec3 padPointEcef(const LPad &p, double u, double v, double h)
{
    return (kR + p.elevM + h) * p.g + u * p.u + v * p.v;
}

void SatelliteSim::launchFillPadParams(GpuCloudParams &cp)
{
    FPROF_SCOPE("launchFillPadParams");
    cp.padInfo = glm::vec4(0.0f);
    cp.coastInfo = glm::vec4(0.0f);
    if (!launchPadGraphics || !launchState)
        return;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (L.pads.empty())
        return;
    // The observer's ENU basis exactly as the shaders build it, from its SEA-LEVEL point (as fillSolarSites).
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 ex = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
    const glm::dvec3 ey = glm::cross(up, ex);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, up)); };
    const double R = kR;
    struct C
    {
        int p;
        double d2;
    };
    std::vector<C> cs;
    const double tNowPad = (double)simDayJ2000 * 86400.0 + simSecInDay;
    for (size_t p = 0; p < L.pads.size(); ++p)
    {
        if (padKindAt(L.pads[p], tNowPad, launchDeployEra) < 0) // (the deployment era: not built yet / cleared)
            continue;
        const double d2 = glm::dot(L.pads[p].g - up, L.pads[p].g - up) * R * R;
        if (d2 < 2500e3 * 2500e3)
            cs.push_back({(int)p, d2});
    }
    std::sort(cs.begin(), cs.end(), [](const C &a, const C &b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.p < b.p); });
    const int n = std::min((int)cs.size(), kMaxPadsGpu);
    int nFlat = 0;
    for (int k = 0; k < n; ++k)
    {
        // The pad as it stands at this date (its kind sets the pattern, the exclusion and the graded radius).
        const LPad pd = padAtTime(L.pads[cs[k].p], tNowPad, launchDeployEra);
        const glm::dvec3 c = toEnu(pd.g) * R - glm::dvec3(0.0, 0.0, R);
        cp.padQ[k] = glm::vec4(glm::vec3(c), (float)pd.exclR);
        cp.padU[k] = glm::vec4(glm::vec3(toEnu(pd.u)), (float)pd.kind);
        cp.padAux[k] = glm::vec4((float)pd.elevM, (float)pd.seed, (float)pd.flatR,
                                 pd.lz && pd.kind == 1 ? (float)(1.0 + pd.lzAng / glm::two_pi<double>()) : 0.0f);
        if (cs[k].d2 < 300e3 * 300e3)
            nFlat = k + 1;
    }
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    cp.padInfo = glm::vec4((float)n, 1.0f, (float)(1.0 - dayness), (float)nFlat);
    // The coast patches within 400 km (nearest first): their grids go to the GPU whole (tdCoastPatch).
    std::vector<C> cc;
    for (size_t c = 0; c < L.coasts.size(); ++c)
    {
        const double d2 = glm::dot(L.coasts[c].g - up, L.coasts[c].g - up) * R * R;
        if (d2 < 400e3 * 400e3)
            cc.push_back({(int)c, d2});
    }
    std::sort(cc.begin(), cc.end(), [](const C &a, const C &b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.p < b.p); });
    const int nc = std::min((int)cc.size(), kMaxCoastPatchesGpu);
    for (int k = 0; k < nc; ++k)
    {
        const LCoast &co = L.coasts[cc[k].p];
        cp.coastC[k] = glm::vec4(glm::vec3(toEnu(co.g) * R - glm::dvec3(0.0, 0.0, R)), (float)co.R);
        cp.coastE[k] = glm::vec4(glm::vec3(toEnu(co.e)), 0.0f);
        cp.coastN[k] = glm::vec4(glm::vec3(toEnu(co.n)), 0.0f);
        cp.coastCol[k] = glm::vec4(co.col, co.floorM);
        std::memcpy(reinterpret_cast<uint8_t *>(cp.coastG) + (size_t)k * kCoastGridN * kCoastGridN, co.grid.data(),
                    (size_t)kCoastGridN * kCoastGridN);
    }
    cp.coastInfo = glm::vec4((float)nc, 0.0f, 0.0f, 0.0f);
}

// Each pad's lights (u, v, height m, kind: 0 flood, 1 red beacon (blinking), 2 landing-zone flood).
struct PadLight
{
    float u, v, h;
    int kind;
};
static void padLightsOf(const LPad &p, std::vector<PadLight> &out)
{
    out.clear();
    if (p.kind == 2)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back({(i & 1) ? 105.0f : -105.0f, (i & 2) ? 105.0f : -105.0f, 45.0f, 0});
        for (int lv = 0; lv < 3; ++lv)
            for (int sd = 0; sd < 2; ++sd)
                out.push_back({sd ? 7.0f : -7.0f, (float)kPadTowerV + 6.5f, 40.0f + 40.0f * lv, 0});
        out.push_back({0.0f, (float)kPadTowerV, 153.0f, 1});
        out.push_back({0.0f, (float)kPadTowerV, 75.0f, 1});
        out.push_back({-230.0f, 120.0f, 28.0f, 0});
        out.push_back({-200.0f, 175.0f, 28.0f, 0});
    }
    else
    {
        const float tv = p.kind == 1 ? -25.0f : -22.0f, th = p.kind == 1 ? 80.0f : 70.0f;
        const float mast[3][2] = {{60.0f, 60.0f}, {60.0f, -60.0f}, {-60.0f, 55.0f}};
        for (const auto &m : mast)
        {
            out.push_back({m[0], m[1], 110.0f, 1});
            out.push_back({m[0], m[1], 35.0f, 0});
        }
        out.push_back({5.0f, tv + 6.0f, 0.4f * th, 0});
        out.push_back({-5.0f, tv + 6.0f, 0.75f * th, 0});
        out.push_back({0.0f, tv, th + 2.0f, 1});
        if (p.lz)
            for (int i = 0; i < 4; ++i)
            {
                const float a = 0.785398f + 1.570796f * i;
                const float cu = (float)(kLzDistM * std::cos(p.lzAng)), cv = (float)(kLzDistM * std::sin(p.lzAng));
                out.push_back({cu + 55.0f * std::cos(a), cv + 55.0f * std::sin(a), 15.0f, 2});
            }
    }
}

void SatelliteSim::launchFillPadLights()
{
    FPROF_SCOPE("launchFillPadLights");
    if (!launchPadGraphics || !launchState || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double night = 1.0 - dayness;
    if (night < 0.02 || L.pads.empty())
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 eyeE = eciToEcef(eye, t);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    std::vector<PadLight> lights;
    for (size_t pi = 0; pi < L.pads.size(); ++pi)
    {
        const LPad pd = padAtTime(L.pads[pi], t, launchDeployEra); // (as it stands at this date)
        if (pd.kind < 0)
            continue; // the deployment era: not built yet / cleared
        // The floodlights' light on low cloud and fog over the pad (2026-10-20): one light per pad, mostly the floods'
        // spill and their light off the pad's concrete (~5e4 cd upward, a Starship pad's more), not on the ground
        // (sat_sky.frag's padGround draws the lit apron itself).
        if (glm::length(pd.g * kR - eyeE) < 300e3)
            engineLightAdd(ecefToEci(padPointEcef(pd, 0.0, pd.kind == 2 ? kPadTowerV : 0.0, 60.0), t),
                           (pd.kind == 2 ? 0.8 : 0.5) * night, glm::vec3(1.0f, 0.93f, 0.80f), 80.0,
                           glm::dvec3(0.0), 0.0, 0.0, true, 0.0);
        // On the meshes (the tower, the mount, a vehicle on the pad: sat_mesh.frag) each flood is its own light,
        // ~2e4 cd (0.16 suns x m^2) toward the pad (2026-10-21).
        if (glm::length(pd.g * kR - eyeE) < 6e3)
        {
            padLightsOf(pd, lights);
            const glm::dvec3 aimPad = ecefToEci(padPointEcef(pd, 0.0, 0.0, 25.0), t);
            for (const PadLight &pl : lights)
                if (pl.kind != 1)
                {
                    // (a landing zone's floods face its own centre, 2.3 km from the pad's)
                    const glm::dvec3 aim = pl.kind == 2 ? ecefToEci(padPointEcef(pd, kLzDistM * std::cos(pd.lzAng),
                                                                                 kLzDistM * std::sin(pd.lzAng), 0.0), t)
                                                        : aimPad;
                    meshLightAdd(ecefToEci(padPointEcef(pd, pl.u, pl.v, pl.h), t), 0.16 * night,
                                 glm::vec3(1.0f, 0.93f, 0.80f), 4.0, &aim, 40.0);
                }
        }
        if (glm::length(pd.g * kR - eyeE) > 160e3)
            continue;
        padLightsOf(pd, lights);
        for (size_t li = 0; li < lights.size(); ++li)
        {
            const PadLight &pl = lights[li];
            const glm::dvec3 P = ecefToEci(padPointEcef(pd, pl.u, pl.v, pl.h), t);
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 5.0);
            if (range > 150e3)
                continue;
            // Behind the Earth's curve (a pad below the horizon from the ground): none.
            {
                const glm::dvec3 u = rel / range;
                const double tc = -glm::dot(eye, u);
                if (tc > 0.0 && tc < range && glm::length(eye + tc * u) < kR + pd.elevM - 50.0)
                    continue;
            }
            // Magnitude at 1 km: a mast's flood cluster seen from the side ~-4, a medium-intensity red aviation
            // beacon ~-2 (flashing ~40 a minute), a landing zone's flood ~-3. (Physical floods are brighter still; the
            // flare is capped below the glare's huge spikes.)
            // Floods -3 (was -4: from a pad's edge they were a field of large stars, issue 20261019_222938 shot 1).
            double m1 = pl.kind == 0 ? -3.0 : pl.kind == 1 ? -2.0 : -3.0;
            double blink = 1.0;
            if (pl.kind == 1)
            {
                const double ph = std::fmod(t / 1.5 + h01(pd.seed, li, 3), 1.0);
                blink = ph < 0.3 ? 1.0 : 0.0;
                if (blink <= 0.0)
                    continue;
            }
            const double m = m1 + 5.0 * std::log10(range / 1000.0) +
                             atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
            // Capped at 14 (was 25): just past the glare threshold (effectFlare ~11.5), so a nearby flood glares faintly
            // and a mast's cluster reads as one soft glow (glare clusters, include/glare.glsl), not a sea urchin.
            const double flare = std::min(0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * night * blink, 14.0);
            if (flare < 2e-4 || kFireCount >= kFireballMax)
                continue;
            const glm::vec3 col = pl.kind == 1 ? glm::vec3(1.0f, 0.12f, 0.06f)
                                : pl.kind == 2 ? glm::vec3(1.0f, 0.93f, 0.8f)
                                               : glm::vec3(0.96f, 0.97f, 1.0f);
            fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare), glm::vec4(col, (float)range)};
        }
    }
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

// -- Engine and floodlight light on clouds, haze, plumes and the ground (2026-10-20) --------------------------------
// A flame's magnitude seen from 100 km above the air -> its luminous intensity in suns x m^2 (the irradiance it gives
// at 1 m, per unit of the Sun's): mag 0 = 2.54e-6 lux, the Sun 1.27e5 lux. A Super Heavy (-6 at 100 km) is ~6e6 cd,
// ~50 suns m^2: ~0.7 lux (~3 full Moons) on a cloud base 3 km up.
double SatelliteSim::flameIntensitySunsM2(double mag100km)
{
    return 2.54e-6 * std::pow(10.0, -0.4 * mag100km) * 1.0e10 / 1.27e5;
}

void SatelliteSim::meshLightAdd(const glm::dvec3 &posEci, double intensitySunsM2, glm::vec3 col, double rs,
                                const glm::dvec3 *aimEci, double beamHalfDeg, bool physical)
{
    if (engineLightGain <= 0.0f || intensitySunsM2 <= 0.0)
        return;
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double lift = (physical ? 1.0 : glm::mix(1.0, (double)moonGain / 2.0e-6, 1.0 - dayness)) * (double)engineLightGain;
    MeshLocalLight m;
    m.physical = physical;
    m.ecef = eciToEcef(posEci, (double)simDayJ2000 * 86400.0 + simSecInDay);
    m.I = intensitySunsM2 * lift;
    m.col = col / std::max(glm::dot(col, glm::vec3(0.2126f, 0.7152f, 0.0722f)), 1e-3f);
    m.rs = (float)std::max(rs, 0.5);
    if (aimEci && beamHalfDeg < 180.0 && glm::length(*aimEci - posEci) > 1e-3)
    {
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        m.aimEcef = glm::normalize(eciToEcef(*aimEci, tNow) - m.ecef);
        m.cosCut = (float)std::cos(glm::radians(beamHalfDeg));
    }
    meshLights_.push_back(m);
}

void SatelliteSim::fillMeshLocalLights(GpuMeshFrame &frame, const glm::dvec3 &originEcef) const
{
    struct Ml
    {
        double w;
        glm::dvec3 p;
        const MeshLocalLight *m;
    };
    std::vector<Ml> ml;
    ml.reserve(meshLightsPrev_.size());
    const double adapt = (double)meshLightAdapt_;
    for (const MeshLocalLight &m : meshLightsPrev_)
    {
        const glm::dvec3 p = m.ecef - originEcef;
        const double I = m.I * (m.physical ? 1.0 : adapt);
        // (where the light no longer matters: ~1e-4 sun of irradiance at its distance from the origin + 20 m)
        const double d2 = glm::dot(p, p);
        if (I < 1e-4 * (d2 + 400.0) * 1e-2)
            continue;
        ml.push_back({I / (d2 + 2500.0), p, &m});
    }
    const size_t nL = std::min<size_t>(ml.size(), (size_t)GpuMeshFrame::kLocalLights);
    std::partial_sort(ml.begin(), ml.begin() + nL, ml.end(), [](const Ml &a, const Ml &b) { return a.w > b.w; });
    frame.lightInfo = glm::vec4((float)nL, 0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < nL; ++i)
    {
        frame.lightPos[i] = glm::vec4(glm::vec3(ml[i].p), (float)(ml[i].m->I * (ml[i].m->physical ? 1.0 : adapt)));
        frame.lightCol[i] = glm::vec4(ml[i].m->col, ml[i].m->rs);
        frame.lightDir[i] = glm::vec4(glm::vec3(ml[i].m->aimEcef), ml[i].m->cosCut);
    }
}

uint32_t SatelliteSim::meshLightMask(const GpuMeshFrame &frame, const glm::dvec3 &centre, double radius, double exposure)
{
    // A light counts for an instance if its largest irradiance anywhere on the bounding sphere, in its beam, would
    // show as more than ~1/1000 of white (the scene's radiance x exposure; the tonemap's toe is linear there).
    const double thr = 1.0e-3 / std::max(exposure, 1e-6);
    const int n = std::min((int)(frame.lightInfo.x + 0.5f), GpuMeshFrame::kLocalLights);
    uint32_t mask = 0;
    for (int i = 0; i < n; ++i)
    {
        const glm::dvec3 d = glm::dvec3(frame.lightPos[i]) - centre;
        const double dist = glm::length(d);
        const double gap = std::max(dist - radius, 0.0), rs = (double)frame.lightCol[i].w;
        const double E = (double)frame.lightPos[i].w / (gap * gap + rs * rs);
        if (E < thr)
            continue;
        const glm::vec4 ad = frame.lightDir[i];
        if (ad.w > -1.5f && dist > radius)
        {
            // An aimed flood: the sphere seen from the light is a cone of half-angle asin(r / d); outside its beam
            // (+ the 10-deg soft edge) it lights nothing there.
            const double cAx = glm::dot(-d / dist, glm::dvec3(ad));
            const double ang = std::acos(glm::clamp(cAx, -1.0, 1.0)) - std::asin(glm::clamp(radius / dist, 0.0, 1.0));
            if (ang > std::acos(glm::clamp((double)ad.w, -1.0, 1.0)) + 0.01)
                continue;
        }
        mask |= 1u << i;
    }
    return mask;
}

void SatelliteSim::engineLightAdd(const glm::dvec3 &posEci, double intensitySunsM2, glm::vec3 col, double r0,
                                  const glm::dvec3 &noseEci, double hullLen, double hullR, bool noGround,
                                  double meshK, const glm::dvec3 *meshPosEci)
{
    if (engineLightGain <= 0.0f || intensitySunsM2 <= 0.0)
        return;
    if (meshK > 0.0)
        meshLightAdd(meshPosEci ? *meshPosEci : posEci, intensitySunsM2 * meshK, col, 0.4 * r0);
    // Physical by day (nothing to see); at night lifted as the moonlight is (moonGain = the full Moon's light on the
    // terrain in the sky's units, physically ~2e-6 of the Sun's): the night exposure is not physical.
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double lift = glm::mix(1.0, (double)moonGain / 2.0e-6, 1.0 - dayness) * (double)engineLightGain;
    const double Il = intensitySunsM2 * lift;
    // Its reach: where a white cloud it lights falls to ~2e-3 of white on screen.
    const double reach = std::min(std::sqrt((double)skyExposure() * 0.9 * Il / 2.0e-3), 300.0e3);
    if (reach < 50.0)
        return;
    const glm::dvec3 eye(obsECI);
    const glm::dvec3 rel = posEci - eye;
    const double dist = glm::length(rel);
    if (dist > 3.0e6 + reach)
        return;
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const float lum = std::max(glm::dot(col, glm::vec3(0.2126f, 0.7152f, 0.0722f)), 1e-3f);
    GpuDustCloud d{};
    d.c = glm::vec4(glm::vec3(toEnu(rel)), (float)Il);
    d.ia = glm::vec4(col / lum, (float)r0);
    d.ib = glm::vec4(glm::vec3(glm::length(noseEci) > 0.5 ? toEnu(glm::normalize(noseEci)) : glm::dvec3(0.0, 0.0, 1.0)),
                     (float)hullLen);
    d.col = glm::vec4((float)hullR, noGround ? 1.0f : 0.0f, (float)reach, 0.0f);
    // (cone.w: the ranking while listing; zeroed on upload) the lights whose lit region is nearest the eye first.
    d.cone = glm::vec4(0.0f, 0.0f, 0.0f, (float)(reach / std::max(dist - reach, reach)));
    engineLights_.push_back(d);
}

void SatelliteSim::engineLightsFlush()
{
    FPROF_SCOPE("engineLightsFlush");
    engineLightCount = 0;
    struct MeshSwap
    {
        SatelliteSim *s;
        ~MeshSwap()
        {
            // (after the adaptation below: the meshes take the same eye adaptation as the clouds)
            s->meshLightsPrev_.swap(s->meshLights_);
            s->meshLights_.clear();
            s->meshLightAdapt_ = s->engineAdaptEased;
        }
    } meshSwap{this};
    if (!kDustMapped)
    {
        engineLights_.clear();
        return;
    }
    // The eye adapts to the lights (2026-10-20, QA: following a Starship 310 m off at night the whole view was a
    // saturated orange-white haze). At night every light is lifted by the moonlight's own lift (~2600x), which is
    // right while the eye is adapted to moonlight; 300 m from a Super Heavy it stands in ~260 full Moons of the
    // flame's light, so its lit exhaust, haze and cloud read hundreds of times past white. The lift is scaled by
    // (1 + R / 2)^-0.75, R = the illuminance at the eye from all the lights in full Moons (the same moonGain
    // reference), eased (adapting to light in ~0.3 s, back to the dark in ~2 s). Only the night's lift adapts: by
    // day the lights are physical. From 2.5 km of a lift-off (~6 full Moons) x0.5; from 10 km and orbit ~1.
    {
        const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
        double R = 0.0;
        for (const GpuDustCloud &d : engineLights_)
            R += (double)d.c.w / (glm::dot(glm::vec3(d.c), glm::vec3(d.c)) + (double)d.ia.w * (double)d.ia.w + 1.0);
        R /= std::max((double)moonGain, 1e-9);
        const double aT = glm::mix(1.0, std::pow(1.0 + R / 2.0, -0.75), 1.0 - dayness);
        const double dt = glm::clamp((double)skyTaaFrameDt, 0.0, 0.5);
        const double rate = aT < engineAdaptEased ? 3.0 : 0.5;
        engineAdaptEased = (float)glm::mix((double)engineAdaptEased, aT, 1.0 - std::exp(-dt * rate));
        // A CUT snaps it (2026-10-20, QA: the first ~0.5 s after jumping beside a burning stage showed the scene
        // unadapted, a white-out): a time jump, the eye moving more than a fifth of the way to the nearest light (or
        // 300 m) in one frame (a teleport, a bookmark, harness / Go-to jumps), and every frame of a Go-to flight
        // (the eye closes on the stage far faster than the eye adapts: the arrival is then already adapted).
        {
            const glm::dvec3 eye(obsECI);
            const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
            double dNear = 1e30;
            for (const GpuDustCloud &d : engineLights_)
                dNear = std::min(dNear, (double)glm::length(glm::vec3(d.c)));
            const bool cut = std::abs(tNow - engineAdaptPrevT) > 2.0 ||
                             glm::length(eye - engineAdaptPrevEye) > std::max(300.0, 0.2 * dNear) || followFlight != 0;
            if (cut)
                engineAdaptEased = (float)aT;
            engineAdaptPrevEye = eye;
            engineAdaptPrevT = tNow;
        }
        if (!(engineAdaptEased > 0.0f) || engineAdaptEased > 1.0f)
            engineAdaptEased = (float)aT;
        const float a = engineAdaptEased;
        for (GpuDustCloud &d : engineLights_)
        {
            d.c.w *= a;
            d.col.z *= std::sqrt(a); // the reach goes as sqrt(I)
        }
    }
    if (engineLights_.size() > kMaxEngineLights)
        std::partial_sort(engineLights_.begin(), engineLights_.begin() + kMaxEngineLights, engineLights_.end(),
                          [](const GpuDustCloud &a, const GpuDustCloud &b) { return a.cone.w > b.cone.w; });
    const uint32_t n = (uint32_t)std::min<size_t>(engineLights_.size(), kMaxEngineLights);
    GpuDustCloud *tail = reinterpret_cast<GpuDustCloud *>(static_cast<char *>(kDustMapped) + 16) + kMaxDustClouds;
    for (uint32_t i = 0; i < n; ++i)
    {
        tail[i] = engineLights_[i];
        tail[i].cone = glm::vec4(0.0f);
    }
    static_cast<uint32_t *>(kDustMapped)[1] = n | smokeHdrBits;   // bits 8-27: the wildfire smoke's range (smoke.glsl)
    engineLightCount = n;
    engineLights_.clear();
}

// The plume Gaussians that cannot show, dropped from the list cloud_march.comp walks (2026-10-21). Every texel tests
// every Gaussian whose view cone covers it, and old, wide or Earth-shadowed exhaust (a bell hundreds of km across seen
// from a few hundred km, a plume in the night) covers the whole sky while adding nothing: in the launch benchmark's
// droneship view 164 of 389 Gaussians covered every texel, ~2 ms of cloud_march, their summed optical depth through
// their centres 8e-4 and their light 1e-6 of white. A worst-case bound per Gaussian (dustCloudsCS: its largest column
// x the billows' and lighting's largest factors, its light per unit optical depth from the Sun / sky colour at the
// phase's peak and from every engine light at its nearest approach, x the frame's exposure), then the faintest are
// dropped while the SUMMED bounds stay under kRadEps of displayed light and kTauEps of dimming (as if every dropped
// Gaussian overlapped one pixel: the image can move by less than that anywhere). Plumes only (col.w >= 2): the
// flames', fireball trails' and dust's units differ. Knockout bit 33554432 keeps them all (the A/B). In the droneship
// view it drops ~60 (the dimming budget binds first: from inside a long Gaussian the eye can look down its length);
// cloud_march.comp's per-texel faint skip (dustCloudsCS) takes most of the rest where they are faint.
void SatelliteSim::dustCullInsignificant(const GpuDustCloud *out, uint32_t n, std::vector<uint8_t> &drop)
{
    plumesCulled = 0;
    drop.assign(n, 0);
    if ((debugDisableMask & 33554432u) != 0u || n == 0)
        return;
    constexpr double kRadEps = 2.0e-4; // display-linear (sRGB: ~0.7 of a level at black, less above)
    constexpr double kTauEps = 1.0e-2; // relative dimming of what is behind (1%: 0.01 mag of a star, ~2 levels at white)
    const double expo = (double)skyExposure();
    // The engine lights as listed so far this frame (all of them are added before the plumes are emitted), before the
    // eye's adaptation (which only dims them) and the cut to kMaxEngineLights: a superset, so the bound holds.
    const GpuDustCloud *lights = engineLights_.data();
    const uint32_t nL = (uint32_t)engineLights_.size();
    struct B
    {
        uint32_t i;
        double rad, dim, key;
    };
    static std::vector<B> bs; // (main thread only; capacity reused)
    bs.clear();
    for (uint32_t i = 0; i < n; ++i)
    {
        const GpuDustCloud &g = out[i];
        if (!(g.col.w >= 2.0f))
            continue;
        // The smallest eigenvalue of the inverse covariance (the longest axis) by the closed form for a symmetric 3x3.
        const double a11 = g.ia.x, a22 = g.ia.y, a33 = g.ia.z, a12 = g.ib.x, a13 = g.ib.y, a23 = g.ib.z;
        const double p1 = a12 * a12 + a13 * a13 + a23 * a23;
        const double qm = (a11 + a22 + a33) / 3.0;
        const double p2 = (a11 - qm) * (a11 - qm) + (a22 - qm) * (a22 - qm) + (a33 - qm) * (a33 - qm) + 2.0 * p1;
        const double p = std::sqrt(std::max(p2, 0.0) / 6.0);
        double lmin = qm;
        if (p > 1e-300)
        {
            const double b11 = (a11 - qm) / p, b22 = (a22 - qm) / p, b33 = (a33 - qm) / p;
            const double b12 = a12 / p, b13 = a13 / p, b23 = a23 / p;
            const double detB = b11 * (b22 * b33 - b23 * b23) - b12 * (b12 * b33 - b23 * b13) + b13 * (b12 * b23 - b22 * b13);
            const double phi = std::acos(glm::clamp(0.5 * detB, -1.0, 1.0)) / 3.0;
            lmin = qm + 2.0 * p * std::cos(phi + 2.0 * glm::pi<double>() / 3.0);
        }
        if (!(lmin > 0.0))
            continue; // (degenerate: keep it)
        // Its largest column along any ray FROM THE EYE, x the billows' largest gain (2.5). In coordinates where the
        // Gaussian is round (y = M^1/2 x) a ray is a line through the eye's y0; its column is
        // K sqrt(2 pi) |M^-1/2 w| exp(-h^2 / 2) for a whitened direction w (unit) at distance h from the centre. With w
        // at an angle th from y0: h^2 = |y0|^2 sin^2 th and |M^-1/2 w| <= s0 cos th + sMax sin th, s0 = the line of
        // sight's 1 / sqrt(q) (w along y0 is the ray through the centre). Maximised over th: an eye far outside a long
        // Gaussian cannot look down its length.
        const double sMax = 1.0 / std::sqrt(lmin);
        double tauMax;
        {
            const glm::dvec3 x0 = -glm::dvec3(g.c);
            auto Mul = [&](const glm::dvec3 &v) {
                return glm::dvec3(a11 * v.x + a12 * v.y + a13 * v.z, a12 * v.x + a22 * v.y + a23 * v.z,
                                  a13 * v.x + a23 * v.y + a33 * v.z);
            };
            const double Y2 = std::max(glm::dot(x0, Mul(x0)), 0.0);
            const double lx = glm::length(x0);
            const glm::dvec3 v = lx > 1e-9 ? x0 / lx : glm::dvec3(0.0, 0.0, 1.0);
            const double s0 = 1.0 / std::sqrt(std::max(glm::dot(v, Mul(v)), lmin));
            // <= s0 + sMax max_u u exp(-Y^2 u^2 / 2) (u = sin th) = s0 + sMax / (Y sqrt(e)) for Y >= 1, else s0 + sMax.
            const double Y = std::sqrt(Y2);
            const double best = s0 + sMax * (Y > 1.0 ? 1.0 / (Y * std::sqrt(glm::e<double>())) : 1.0);
            tauMax = (double)g.c.w * (double)g.ia.w * std::sqrt(2.0 * glm::pi<double>()) * best * 2.5 * std::max(1.0, (double)launchPlumeBillows);
        }
        // Light per unit optical depth: the Sun / sky colour at the phase's peak + multiple scattering (<= 1.9),
        // x the self-shadow's and the lumps' largest gain (1.35); each engine light at its nearest approach to the
        // Gaussian's 8-sigma extent (x the lumps' 1.3 and the isotropic + multiple-scattering factor <= 0.16).
        const double cMax = std::max({(double)g.col.x, (double)g.col.y, (double)g.col.z});
        double perTau = cMax * 1.9 * 1.35;
        const double albedo = std::max((double)g.col.w - 2.0, 0.0);
        for (uint32_t k = 0; k < nL; ++k)
        {
            const GpuDustCloud &el = lights[k];
            const double dC = glm::length(glm::dvec3(el.c) - glm::dvec3(g.c));
            const double dmin = std::max(dC - 8.0 * sMax, 0.0);
            if (dmin > (double)el.col.z)
                continue; // beyond its reach (the shader's test)
            const double cE = std::max({(double)el.ia.x, (double)el.ia.y, (double)el.ia.z});
            perTau += cE * albedo * 1.3 * 0.16 * (double)el.c.w / (dmin * dmin + (double)el.ia.w * (double)el.ia.w);
        }
        const double rad = tauMax * perTau * expo;
        const double dim = tauMax * std::max(1.0 - 0.85 * albedo, 0.0);
        bs.push_back({i, rad, dim, std::max(rad / kRadEps, dim / kTauEps)});
    }
    if (bs.empty())
        return;
    std::sort(bs.begin(), bs.end(), [](const B &a, const B &b) { return a.key < b.key; });
    double sR = 0.0, sD = 0.0;
    plumeCullStop = glm::vec4(0.0f);
    for (const B &b : bs)
    {
        if (sR + b.rad > kRadEps || sD + b.dim > kTauEps)
        {
            plumeCullStop = glm::vec4((float)sR, (float)sD, (float)b.rad, (float)b.dim);
            break;
        }
        sR += b.rad;
        sD += b.dim;
        drop[b.i] = 1;
        ++plumesCulled;
    }
}

nlohmann::json SatelliteSim::launchPadsJson() const
{
    nlohmann::json a = nlohmann::json::array();
    if (!launchState)
        return a;
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eyeE = eciToEcef(glm::dvec3(obsECI), t);
    for (const LPad &p : L.pads)
    {
        nlohmann::json o;
        o["site"] = L.cat.sites[p.site].id;
        o["name"] = p.name;
        o["kind"] = p.kind == 2 ? "starship" : p.kind == 1 ? "falcon" : "generic";
        {
            // As drawn at this date (the deployment era), and the pad's history.
            const int kt = padKindAt(p, t, launchDeployEra);
            o["kind_now"] = kt == 2 ? "starship" : kt == 1 ? "falcon" : kt == 0 ? "generic" : "none";
            nlohmann::json hj = nlohmann::json::array();
            for (const auto &h : p.kindHist)
                hj.push_back({h.first < -1e299 ? -1e9 : 2000.0 + h.first / 31557600.0, h.second});
            o["history"] = hj;
        }
        o["ground_m"] = p.elevM;
        o["lat"] = glm::degrees(std::asin(glm::clamp(p.g.z, -1.0, 1.0)));
        o["lon"] = glm::degrees(std::atan2(p.g.y, p.g.x));
        o["dist_km"] = glm::length(p.g * kR - eyeE) / 1000.0;
        o["landing_zone"] = p.lz;
        a.push_back(o);
    }
    return a;
}

// The observer at a pad (harness `launch pad`): distM from its centre toward azDeg (clockwise from the pad's heading
// u), heightM above its ground, looking at the tower's middle.
bool SatelliteSim::launchGoToPad(const std::string &site, double distM, double azDeg, double heightM, std::string &msg)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    int pi = -1;
    for (size_t s = 0; s < L.cat.sites.size(); ++s)
        if (L.cat.sites[s].id == site && s < L.sitePad.size())
            pi = L.sitePad[s];
    if (pi < 0)
    {
        msg = "no site '" + site + "' (see `launch pads`)";
        return false;
    }
    const LPad pd = padAtTime(L.pads[pi], (double)simDayJ2000 * 86400.0 + simSecInDay, launchDeployEra);
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double a = glm::radians(azDeg);
    const glm::dvec3 off = distM * (std::cos(a) * pd.u - std::sin(a) * pd.v);
    const glm::dvec3 g = glm::normalize(pd.g * kR + off);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)(pd.elevM + heightM);
    const glm::dvec3 tgt = padPointEcef(pd, 0.0, pd.kind == 2 ? kPadTowerV * 0.5 : -10.0, pd.kind == 2 ? 60.0 : 35.0);
    const glm::dvec3 up = g;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rel = tgt - (kR + pd.elevM + heightM) * g;
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay);
    char b[200];
    snprintf(b, sizeof(b), "at %s (%s pad, ground %.0f m), %.0f m out", pd.name.c_str(),
             pd.kind == 2 ? "Starship" : pd.kind == 1 ? "Falcon" : pd.kind == 0 ? "launch" : "not built yet: empty", pd.elevM, distM);
    msg = b;
    return true;
}

// The next catch (harness `launch landing`): which 0 a ship, 1 a booster. The clock leadS before the catch, 1x, the
// observer distM from the pad (to the side of the approach), 30 m up, looking at the catch point.
bool SatelliteSim::launchGoToLanding(int which, double leadS, double distM, std::string &msg, bool wantDay)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<MissionLaunch> ml;
    launchUpcoming(t0 - 4.0 * kDay, t0 + 30.0 * kDay, 0, 4000, ml);
    const LBatch *best = nullptr;
    double bestT = 1e300;
    for (const MissionLaunch &m : ml)
    {
        auto it = L.batches.find(m.key);
        if (it == L.batches.end() || !it->second.valid)
            continue;
        const LBatch &B = it->second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        double tc = -1.0;
        if (which == 0 && B.shipLands && B.landPad >= 0)
            tc = B.tShipCatch;
        else if (which == 1 && vh.recovery == 1 && L.booster[B.veh].landed)
            tc = B.tL + L.booster[B.veh].tEnd;
        else if (which == 2 && B.landKind == kLandShip)
            tc = B.tL + LaunchImpl::boosterTr(L, B).tEnd; // a booster landing at sea (a droneship's deck)
        if (wantDay && tc > 0.0 && which == 2)
        {
            if (sunElevAtPad(L.cat.sites[B.site], tc) < 10.0)
                continue;
        }
        else if (wantDay && tc > 0.0)
        {
            const int pi = which == 0 ? B.landPad : L.sitePad[B.site];
            if (pi < 0 || sunElevAtPad(L.cat.sites[L.pads[pi].site], tc) < 10.0)
                continue;
        }
        if (tc > t0 + leadS + 1.0 && tc < bestT)
        {
            bestT = tc;
            best = &B;
            launchFocusKey = m.key;
        }
    }
    if (!best)
    {
        msg = which == 0 ? "no ship catch in the next 30 days" : which == 1 ? "no booster catch in the next 30 days"
                                                                            : "no booster landing at sea in the next 30 days";
        return false;
    }
    const LBatch &B = *best;
    if (which == 2)
    {
        // At sea (2026-10-20, the landing spray): the observer 30 m over the water distM to the side of the
        // booster's touchdown point, looking at it.
        if (followActive)
            stopFollow();
        if (trackActive)
            stopTrack();
        const double tS = bestT - leadS;
        const double days = std::floor(tS / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = tS - days * 86400.0;
        trailClearPending = true;
        timePaused = false;
        timeScaleIdx = 0;
        timeDir = 1.0f;
        glm::dvec3 LP;
        double tTouch;
        LaunchImpl::boosterLandingAt(L, B, bestT, LP, tTouch); // the ship's deck at touchdown
        const glm::dvec3 gl = glm::normalize(LP);
        const glm::dvec3 side = glm::normalize(glm::cross(gl, B.Eaz));
        const glm::dvec3 g = glm::normalize(gl * kR + distM * side);
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
        obsHeightOffset = 30.0f;
        const glm::dvec3 tgt = gl * (kR + 60.0);
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), g));
        const glm::dvec3 north = glm::cross(g, east);
        const glm::dvec3 rn = glm::normalize(tgt - (kR + 30.0) * g);
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                      (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, g), -1.0, 1.0))));
        camera.fovYDeg = 40.0f;
        skyTaaHistValid = false;
        cv2HistoryValid = false;
        updatePositions(tS);
        char b[300];
        snprintf(b, sizeof(b), "%s booster landing at sea %.0f km downrange: touchdown in %.0f s",
                 L.cat.vehicles[B.veh].name.c_str(), LaunchImpl::boosterTr(L, B).p.back().x / 1000.0, leadS);
        msg = b;
        return true;
    }
    const int pi = which == 0 ? B.landPad : L.sitePad[B.site];
    const LPad &pd = L.pads[pi];
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tS = bestT - leadS;
    const double days = std::floor(tS / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tS - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // To the side of the approach (the ship's descent comes in along the landing frame's -dXF), else of the pad's v.
    glm::dvec3 side = pd.v;
    if (which == 0)
    {
        const glm::dvec3 ap = -(B.dXF.x * B.LE + B.dXF.y * B.LN);
        if (glm::length(ap) > 1.0)
            side = glm::normalize(glm::cross(pd.g, glm::normalize(ap)));
    }
    const glm::dvec3 g = glm::normalize(pd.g * kR + distM * side);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)(pd.elevM + 30.0);
    const double catchUp = which == 0 ? kShipCatchAboveGround + 40.0 : L.cat.vehicles[B.veh].mountM + 60.0;
    const glm::dvec3 tgt = padPointEcef(pd, 0.0, 0.0, catchUp + (which == 0 ? 250.0 : 0.0));
    const glm::dvec3 up = g;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(tgt - (kR + pd.elevM + 30.0) * g);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 40.0f;
    updatePositions(tS);
    char b[300];
    if (which == 0)
        snprintf(b, sizeof(b),
                 "%s ship to the tower at %s: catch in %.0f s (descent from %.0f km, %.0f s; flip and landing burn %.0f s)",
                 L.cat.vehicles[B.veh].name.c_str(), pd.name.c_str(), leadS, B.dH0 / 1000.0, B.T1, B.T2);
    else
        snprintf(b, sizeof(b), "%s booster to the tower at %s: catch in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 pd.name.c_str(), leadS);
    msg = b;
    return true;
}

// ── Booster landing ships (2026-10-20, droneships) ─────────────────────────────────────────────────
// A launch whose booster cannot fly home lands it on a ship downrange (planRecovery: landKind kLandShip). The ship is
// on station kShipOnStationS before lift-off, the booster comes down on its deck (the track ends deckM above the sea at
// the ship), and kShipSecureS later it steams home toward the launch site at kShipSpeed with the booster on deck
// (rocketPos), drawn until kShipHomeS after the landing. Drawn as a mesh in the reserved ship slots (launchShipJobs),
// with its lights at night (launchFillShipLights). All a pure function of the batch and sim time.

namespace
{
// A point on the ship (its body frame: u toward the bow, v to port, h above the waterline), Earth-fixed.
glm::dvec3 shipPointEcef(const LBatch &B, double t, double u, double v, double h)
{
    return LaunchImpl::shipDeckEcef(B, t, u, v, h); // riding the swell (2026-10-21)
}
// The ships whose time it is (t in [tShipOn, tShipGone]) among the live batches.
// LS is SatelliteSim::LaunchState, which is private: a namespace-scope function may not name it (GCC/Clang
// enforce that; MSVC does not), so it is deduced from the member functions that call this.
template <typename LS, typename F> void forEachShip(LS &L, double t, F f)
{
    for (LBatch *pb : L.livePtr)
    {
        const LBatch &B = *pb;
        if (!B.valid || B.landKind != kLandShip || B.ship < 0 || t < B.tShipOn || t > B.tShipGone)
            continue;
        f(B);
    }
}
struct ShipLight
{
    float u, v, h;
    int kind; // 0 flood (warm white), 1 red beacon (flashing), 2 masthead / deck white, 3 green, 4 red (steady)
};
void shipLightsOf(const std::string &model, double deck, std::vector<ShipLight> &out)
{
    out.clear();
    const float D = (float)deck;
    if (model == "ship_jacklyn")
    {
        // Floods on the superstructure's front and the cranes over the landing deck, the masthead light, a red
        // beacon on the mast, the side lights on the bridge wings, the stern light.
        out.push_back({53.0f, 10.0f, D + 12.5f, 0});
        out.push_back({53.0f, -10.0f, D + 12.5f, 0});
        out.push_back({45.0f, 11.0f, D + 5.0f, 0});
        out.push_back({45.0f, -11.0f, D + 5.0f, 0});
        out.push_back({66.0f, 0.0f, D + 30.0f, 1});
        out.push_back({66.0f, 0.0f, D + 26.0f, 2});
        out.push_back({67.0f, 14.0f, D + 15.0f, 4});
        out.push_back({67.0f, -14.0f, D + 15.0f, 3});
        out.push_back({-41.0f, 0.0f, D + 2.0f, 2});
    }
    else
    {
        // An ASDS: floods on the housings' masts facing the deck, a red beacon atop each mast, the navigation lights
        // at the deck's corners (a barge under tow: red to port, green to starboard), the housings' deck lights.
        for (int sd = -1; sd <= 1; sd += 2)
        {
            out.push_back({-42.0f, 21.0f * sd, D + 15.5f, 0});
            out.push_back({-42.0f, 21.0f * sd, D + 16.2f, 1});
            out.push_back({-36.0f, 12.0f * sd, D + 7.2f, 0});
            out.push_back({44.5f, 25.0f * sd, D + 1.2f, sd > 0 ? 4 : 3});
        }
        out.push_back({-45.0f, 0.0f, D + 3.0f, 2});
    }
}
} // namespace

// The station arms (2026-10-21, Canadarm2): the two booms and the wrist as meshes in the arm's reserved chunk, posed by
// LaunchImpl::stationArmPose in the station's frame (the station's own pose: the same law its mesh is posed by).
// Drawn whenever the station is near enough to resolve them; parked when no berthing vehicle needs the arm.
int SatelliteSim::viewerArmParts(int sat, double t, const glm::dmat3 &rootR, const glm::dvec3 &rootT, int *types,
                                 glm::dmat3 *Rb, glm::dvec3 *org)
{
    if (!launchState || sat < 0)
        return 0;
    LaunchState &L = *launchState;
    for (const auto &as : L.armSat)
    {
        if ((int)as.first != sat)
            continue;
        const launch::StationArm &A = L.cat.arms[(size_t)as.second];
        auto tb = rocketTypes.find("rocket_arm_" + A.model + "_boom");
        auto tw = rocketTypes.find("rocket_arm_" + A.model + "_wrist");
        if (tb == rocketTypes.end() || tw == rocketTypes.end())
            return 0;
        ArmPose ap;
        LaunchImpl::stationArmPose(A, t, LaunchImpl::berthVisits(L, (uint32_t)sat), ap);
        const struct
        {
            int type;
            glm::dvec3 o, z;
        } parts[3] = {{tb->second, ap.S, ap.E - ap.S}, {tb->second, ap.E, ap.W - ap.E}, {tw->second, ap.W, ap.G - ap.W}};
        for (int k = 0; k < 3; ++k)
        {
            types[k] = parts[k].type;
            Rb[k] = rootR * LaunchImpl::crewFrame(glm::length(parts[k].z) > 1e-6 ? parts[k].z : A.baseN, ap.nrm);
            org[k] = rootT + rootR * parts[k].o;
        }
        return 3;
    }
    return 0;
}

void SatelliteSim::launchArmJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs)
{
    FPROF_SCOPE("launchArmJobs");
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const uint32_t abase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots - kStackMeshSlots - kShipMeshSlots - kArmMeshSlots;
    L.nArmParts = 0;
    int k = 0;
    // A station with a visiting vehicle near it (at its port, approaching, leaving) is drawn by the CPU too: the GPU
    // lists no satellite in the Earth's shadow, so at night the vehicle was drawn at a station that was not.
    for (const auto &cv : L.crewList)
    {
        const uint32_t sat = cv.second->crew.sat;
        if (sat >= activeSatCount || (followActive && followSatIndex == (int)sat) ||
            std::find(jobs.begin(), jobs.end(), (int)sat) != jobs.end() ||
            satOrbits[sat].constIdx >= constellations.size() || !constellations[satOrbits[sat].constIdx].enabled)
            continue;
        if (kesslerEnabled && 2ull * sat + 1 < kRosterWords.size())
        {
            const int32_t d = std::min(kRosterWords[2ull * sat], kRosterWords[2ull * sat + 1]);
            if (d != kessler::kNever && t >= kesslerEpoch() + (double)d)
                continue;
        }
        const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh((int)satOrbits[sat].typeIdx);
        if (!tm)
            continue;
        glm::dvec3 Ps;
        glm::dmat3 R;
        LaunchImpl::stationFrameAt(rosterElemsOf(satOrbits[sat]), t, Ps, R);
        if (2.0 * std::max(1.0, (double)tm->boundsRadius) / std::max(glm::length(Ps - obsEci), 1.0) /
                std::max(pixAngle, 1e-9) > 3.0)
            jobs.push_back((int)sat);
    }
    for (const auto &as : L.armSat)
    {
        const uint32_t sat = as.first;
        const launch::StationArm &A = L.cat.arms[(size_t)as.second];
        if (sat >= activeSatCount || satOrbits[sat].constIdx >= constellations.size() || !constellations[satOrbits[sat].constIdx].enabled)
            continue;
        if (kesslerEnabled && 2ull * sat + 1 < kRosterWords.size())
        {
            const int32_t d = std::min(kRosterWords[2ull * sat], kRosterWords[2ull * sat + 1]);
            if (d != kessler::kNever && t >= kesslerEpoch() + (double)d)
                continue;
        }
        auto tb = rocketTypes.find("rocket_arm_" + A.model + "_boom");
        auto tw = rocketTypes.find("rocket_arm_" + A.model + "_wrist");
        if (tb == rocketTypes.end() || tw == rocketTypes.end())
            continue;
        glm::dvec3 Ps;
        glm::dmat3 R;
        const SatOrbitElems st = rosterElemsOf(satOrbits[sat]);
        LaunchImpl::stationFrameAt(st, t, Ps, R);
        const double dist = std::max(glm::length(Ps - obsEci), 1.0);
        if (2.0 * A.boomM / dist / std::max(pixAngle, 1e-9) < 1.4)
            continue;
        // The station itself with its arm: the GPU lists no satellite in the Earth's shadow, so the station's mesh
        // would vanish there and leave the arm (and the vehicles at its ports) floating (2026-10-21). Drawn by the CPU
        // as the followed satellite is (recordMeshScene skips it among the GPU's candidates).
        if (!(followActive && followSatIndex == (int)sat) &&
            std::find(jobs.begin(), jobs.end(), (int)sat) == jobs.end())
            jobs.push_back((int)sat);
        ArmPose ap;
        LaunchImpl::stationArmPose(A, t, LaunchImpl::berthVisits(L, sat), ap);
        L.armPoseNow = ap;
        L.armPoseSat = sat;
        const SatOrbitState so = satOrbitStateAt(st, t);
        const glm::dvec3 acc = -launch::kGM * so.posEci / std::pow(glm::length(so.posEci), 3.0);
        struct Part
        {
            int type;
            glm::dvec3 o, z;
        } parts[3] = {{tb->second, ap.S, ap.E - ap.S}, {tb->second, ap.E, ap.W - ap.E}, {tw->second, ap.W, ap.G - ap.W}};
        for (const Part &pt : parts)
        {
            if (k >= kArmMeshSlots)
                break;
            const uint32_t slot = abase + (uint32_t)k, idx = activeSatCount + slot;
            ++k;
            if (idx >= satOrbits.size() || slot >= L.elems.size())
                continue;
            const glm::dmat3 Rb = R * LaunchImpl::crewFrame(glm::length(pt.z) > 1e-6 ? pt.z : A.baseN, ap.nrm);
            SatOrbitElems &e = L.elems[slot];
            e = SatOrbitElems{};
            e.fixedState = true;
            e.fixPos = Ps + R * pt.o;
            e.fixVel = so.velEci;
            e.fixAcc = acc;
            e.fixAxis = Rb[2];
            e.fixT0 = t;
            e.attFix = true;
            e.attQ = glm::normalize(glm::quat_cast(Rb));
            e.attT0 = t;
            satOrbits[idx].typeIdx = (uint32_t)pt.type;
            jobs.push_back((int)idx);
            ++L.nArmParts;
        }
    }
}

// The sea state at a point of the sea (2026-10-21, the ships' swell): the CPU's mirror of the drawn waves' rule
// (sat_sky.frag oceanSeaState: the westerly belts, a slow regional wind, the weather's storms) with the storms from
// the cloud map's coverage there (the CPU has no weather cube: no cloud type or rain). 1 = the default sea.
double SatelliteSim::shipSeaState(const glm::dvec3 &dE) const
{
    const double g = (double)oceanSeaStateGain;
    if (g <= 0.0)
        return 1.0;
    double cov = 0.3;
    if (!earthCloudsCpu.empty())
    {
        const double twoPi = glm::two_pi<double>();
        const double lat = std::asin(glm::clamp(dE.z, -1.0, 1.0)), lon = std::atan2(dE.y, dE.x);
        double u = (lon + glm::pi<double>()) / twoPi + cloudDriftPhase() / twoPi;
        u -= std::floor(u);
        const double v = (0.5 * glm::pi<double>() - lat) / glm::pi<double>();
        const int x = ((int)(u * earthCloudsCpuW) % earthCloudsCpuW + earthCloudsCpuW) % earthCloudsCpuW;
        const int y = std::clamp((int)(v * earthCloudsCpuH), 0, earthCloudsCpuH - 1);
        cov = earthCloudsCpu[(size_t)y * earthCloudsCpuW + x] / 255.0;
    }
    const double storm = glm::smoothstep(0.45, 0.85, cov) * 0.6;
    const double latA = std::abs(dE.z);
    const double belt = glm::smoothstep(0.5, 0.71, latA) * (1.0 - glm::smoothstep(0.87, 0.94, latA));
    const double wind = 0.5 + 0.28 * std::sin(3.1 * dE.x + 5.0 * dE.y + 1.3) + 0.22 * std::sin(4.7 * dE.z - 2.3 * dE.x + 0.4);
    const double s = (0.3 + 0.35 * wind + 0.35 * belt + 0.9 * storm) / 0.65;
    return std::max(0.25, 1.0 + (s - 1.0) * g);
}

namespace
{
// The securing robot (SpaceX's Octagrabber; Jacklyn's own) on a landing ship (2026-10-21; model ship_robot): parked
// on the deck (the droneship: in front of its garage aft), it drives out 75 s after a booster lands, stops with its
// front at the booster's skirt and swings its clamp arms under the engine section (the model's unfolding, 15 s).
// Deck coordinates (u toward the bow, v to port), its heading (rad from the bow, toward port) and when it clamps.
struct ShipRobot
{
    double u, v, head, tClamp;
};
ShipRobot shipRobotAt(const LBatch &B, double t)
{
    const bool jack = B.deckM > 5.0;
    const glm::dvec2 park = jack ? glm::dvec2(24.0, 0.0) : glm::dvec2(-31.5, 0.0);
    const glm::dvec2 dir = -glm::normalize(park); // toward the landing target
    const glm::dvec2 stop = -dir * 4.6;           // its front (arms) at the booster's skirt
    const double dist = glm::length(stop - park);
    const double tGo = B.tLand + 75.0, dur = dist / 0.6 + 6.0;
    const double s = glm::smoothstep(tGo, tGo + dur, t);
    const glm::dvec2 p = glm::mix(park, stop, s);
    return {p.x, p.y, std::atan2(dir.y, dir.x), tGo + dur + 5.0};
}
} // namespace

// The ships as meshes: those over ~1.4 px, nearest first, each kept in its reserved slot while it stays; and their
// securing robots (2026-10-21) in the same slots, once near enough to resolve.
void SatelliteSim::launchShipJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs)
{
    FPROF_SCOPE("launchShipJobs");
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double dt = glm::clamp((double)skyTaaFrameDt, 0.0, 1.0);
    struct W
    {
        double px;
        uint64_t key;
        int type;
        const LBatch *B;
        bool robot;
    };
    std::vector<W> want;
    auto robotTi = rocketTypes.find("ship_robot");
    forEachShip(L, t, [&](const LBatch &B) {
        // Its sea state (the swell, shipSwell): eased over ~20 s (the cloud map's coverage under a moving ship).
        {
            const double sk = shipSeaState(glm::normalize(LaunchImpl::shipEcef(B, t)));
            B.seaK = B.seaK < 0.0f ? (float)sk : (float)glm::mix((double)B.seaK, sk, 1.0 - std::exp(-dt / 20.0));
        }
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        auto ti = rocketTypes.find(vh.ships[(size_t)B.ship].model);
        if (ti == rocketTypes.end())
            return;
        const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh(ti->second);
        if (!tm)
            return;
        const glm::dvec3 P = ecefToEci(LaunchImpl::shipEcef(B, t), t);
        const double dist = std::max(glm::length(P - obsEci), 1.0);
        const double px = 2.0 * std::max(1.0, (double)tm->boundsRadius) / dist / std::max(pixAngle, 1e-9);
        if (px > 1.4)
            want.push_back({px, batchKey(B.plane, B.k, B.b), ti->second, &B, false});
        if (robotTi != rocketTypes.end())
            if (const SatMeshRenderer::TypeMesh *rm = meshRenderer.typeMesh(robotTi->second))
            {
                const double rpx = 2.0 * std::max(1.0, (double)rm->boundsRadius) / dist / std::max(pixAngle, 1e-9);
                if (rpx > 1.4)
                    want.push_back({rpx, batchKey(B.plane, B.k, B.b) ^ 0x8000000000000000ull, robotTi->second, &B, true});
            }
    });
    std::sort(want.begin(), want.end(), [](const W &a, const W &b) { return a.px > b.px || (a.px == b.px && a.key < b.key); });
    if (want.size() > (size_t)kShipMeshSlots)
        want.resize(kShipMeshSlots);
    bool used[kShipMeshSlots] = {};
    std::vector<int> slotOf(want.size(), -1);
    for (size_t w = 0; w < want.size(); ++w)
        for (int k = 0; k < kShipMeshSlots; ++k)
            if (!used[k] && L.shipMeshKey[k] == want[w].key)
            {
                used[k] = true;
                slotOf[w] = k;
                break;
            }
    for (size_t w = 0; w < want.size(); ++w)
        if (slotOf[w] < 0)
            for (int k = 0; k < kShipMeshSlots; ++k)
                if (!used[k])
                {
                    used[k] = true;
                    slotOf[w] = k;
                    break;
                }
    for (int k = 0; k < kShipMeshSlots; ++k)
        if (!used[k])
            L.shipMeshKey[k] = 0;
    const uint32_t sbase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots - kStackMeshSlots - kShipMeshSlots;
    L.nShips = 0;
    L.nShipRobots = 0;
    for (size_t w = 0; w < want.size(); ++w)
    {
        const W &ws = want[w];
        const int k = slotOf[w];
        L.shipMeshKey[k] = ws.key;
        const uint32_t slot = sbase + (uint32_t)k, idx = activeSatCount + slot;
        if (idx >= satOrbits.size() || slot >= L.elems.size())
            continue;
        // Its frame riding the swell (shipFrame): the mesh takes the full attitude (attFix); the robot its heading
        // on the deck, about the deck's normal.
        const LaunchImpl::ShipFrame F = LaunchImpl::shipFrame(*ws.B, t);
        glm::dvec3 E = F.O, X = F.bow, Y = F.port;
        SatOrbitElems &e = L.elems[slot];
        e = SatOrbitElems{};
        if (ws.robot)
        {
            const ShipRobot R = shipRobotAt(*ws.B, t);
            E = F.O + ws.B->deckM * F.up + R.u * F.bow + R.v * F.port;
            X = std::cos(R.head) * F.bow + std::sin(R.head) * F.port;
            Y = glm::cross(F.up, X);
            e.unfoldT0 = R.tClamp; // its clamp arms swing out (the model's stowable parts)
            e.unfoldDur = 15.0;
        }
        const glm::dvec3 E1 = LaunchImpl::shipEcef(*ws.B, t - 1.0) + (E - LaunchImpl::shipEcef(*ws.B, t));
        e.fixedState = true;
        e.fixPos = ecefToEci(E, t);
        e.fixVel = e.fixPos - ecefToEci(E1, t - 1.0);
        const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
        e.fixAcc = glm::cross(omega, e.fixVel);
        e.fixAxis = ecefToEci(X, t);
        e.fixT0 = t;
        e.attFix = true;
        e.attQ = glm::quat_cast(glm::dmat3(ecefToEci(X, t), ecefToEci(Y, t), ecefToEci(glm::cross(X, Y), t)));
        e.attT0 = t;
        e.attSlewDur = 0.0;
        satOrbits[idx].typeIdx = (uint32_t)ws.type;
        jobs.push_back((int)idx);
        (ws.robot ? L.nShipRobots : L.nShips)++;
    }
}

// Their lights at night (point sprites in kFireBuf, like the pads'): within 150 km.
void SatelliteSim::launchFillShipLights()
{
    FPROF_SCOPE("launchFillShipLights");
    if (!launchState || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double night = 1.0 - dayness;
    if (night < 0.02)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 eyeE = eciToEcef(eye, t);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    std::vector<ShipLight> lights;
    forEachShip(L, t, [&](const LBatch &B) {
        // The deck floods' light on the sea around the ship, on low cloud and haze (engineLightAdd, like a pad's: one
        // light ~15 m over the deck; the landing burn's own light is the rocket's, launchFillVisuals).
        if (glm::length(LaunchImpl::shipEcef(B, t) - eyeE) < 300e3)
            engineLightAdd(ecefToEci(shipPointEcef(B, t, 0.0, 0.0, B.deckM + 15.0), t), 0.3 * night,
                           glm::vec3(1.0f, 0.9f, 0.76f), 40.0, glm::dvec3(0.0), 0.0, 0.0, false, 0.0);
        // On the meshes (the deck, a booster on it: sat_mesh.frag) each deck flood is its own light (2026-10-21).
        if (glm::length(LaunchImpl::shipEcef(B, t) - eyeE) < 6e3)
        {
            shipLightsOf(L.cat.vehicles[B.veh].ships[(size_t)B.ship].model, B.deckM, lights);
            const glm::dvec3 aimDeck = ecefToEci(shipPointEcef(B, t, 0.0, 0.0, B.deckM + 10.0), t);
            for (const ShipLight &sl : lights)
                if (sl.kind == 0)
                    meshLightAdd(ecefToEci(shipPointEcef(B, t, sl.u, sl.v, sl.h), t), 0.1 * night,
                                 glm::vec3(1.0f, 0.9f, 0.76f), 4.0, &aimDeck, 45.0);
        }
        if (glm::length(LaunchImpl::shipEcef(B, t) - eyeE) > 160e3)
            return;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        shipLightsOf(vh.ships[(size_t)B.ship].model, B.deckM, lights);
        for (size_t li = 0; li < lights.size(); ++li)
        {
            const ShipLight &sl = lights[li];
            const glm::dvec3 P = ecefToEci(shipPointEcef(B, t, sl.u, sl.v, sl.h), t);
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 5.0);
            if (range > 150e3)
                continue;
            {
                const glm::dvec3 u = rel / range;
                const double tc = -glm::dot(eye, u);
                if (tc > 0.0 && tc < range && glm::length(eye + tc * u) < kR - 20.0)
                    continue; // below the horizon
            }
            // At 1 km: a deck flood ~-3 (as the pads'), a beacon ~-2 flashing, navigation lights ~0 (2 nmi lamps).
            const double m1 = sl.kind == 0 ? -2.0 : sl.kind == 1 ? -1.5 : 0.0;
            double blink = 1.0;
            if (sl.kind == 1)
            {
                const double ph = std::fmod(t / 1.3 + h01(B.seed, li, 41), 1.0);
                if (ph >= 0.25)
                    continue;
            }
            const double m = m1 + 5.0 * std::log10(range / 1000.0) +
                             atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
            const double flare = std::min(0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * night * blink, 14.0);
            if (flare < 2e-4 || kFireCount >= kFireballMax)
                continue;
            const glm::vec3 col = sl.kind == 0   ? glm::vec3(1.0f, 0.9f, 0.74f)
                                  : sl.kind == 1 ? glm::vec3(1.0f, 0.12f, 0.06f)
                                  : sl.kind == 3 ? glm::vec3(0.2f, 1.0f, 0.45f)
                                  : sl.kind == 4 ? glm::vec3(1.0f, 0.15f, 0.1f)
                                                 : glm::vec3(0.96f, 0.97f, 1.0f);
            fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare), glm::vec4(col, (float)range)};
        }
    });
}

nlohmann::json SatelliteSim::launchShipsJson() const
{
    nlohmann::json a = nlohmann::json::array();
    if (!launchState)
        return a;
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eyeE = eciToEcef(glm::dvec3(obsECI), t);
    forEachShip(L, t, [&](const LBatch &B) {
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        glm::dvec3 bow;
        const glm::dvec3 S = LaunchImpl::shipEcef(B, t, &bow);
        const glm::dvec3 g = S / kR;
        nlohmann::json o;
        o["ship"] = vh.ships[(size_t)B.ship].name;
        o["model"] = vh.ships[(size_t)B.ship].model;
        o["vehicle"] = vh.id;
        o["site"] = L.cat.sites[B.site].id;
        o["downrange_km"] = B.xLand / 1000.0;
        o["lat"] = glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0)));
        o["lon"] = glm::degrees(std::atan2(g.y, g.x));
        o["dist_km"] = glm::length(S - eyeE) / 1000.0;
        o["landing_in_s"] = B.tLand - t;
        o["state"] = t < B.tL ? "on station" : t < B.tLand ? "booster inbound" : t < B.tShipDepart ? "securing the booster"
                                                                                                    : "heading home";
        o["home_km"] = std::acos(glm::clamp(glm::dot(g, B.G0), -1.0, 1.0)) * kR / 1000.0;
        o["payload_kg"] = B.payloadKg;
        // Its swell (2026-10-21) and its securing robot.
        {
            double hv, p, r;
            LaunchImpl::shipSwell(B, t, hv, p, r);
            o["sea_state"] = B.seaK;
            o["heave_m"] = hv;
            o["pitch_deg"] = glm::degrees(p);
            o["roll_deg"] = glm::degrees(r);
            const ShipRobot R = shipRobotAt(B, t);
            o["robot_deck_uv_m"] = {R.u, R.v};
            o["robot_clamp_in_s"] = R.tClamp - t;
        }
        a.push_back(o);
    });
    return a;
}

// The next booster landing on a ship (harness `launch droneship`).
bool SatelliteSim::launchGoToShipLanding(double leadS, double distM, double azDeg, double heightM, int dayNight,
                                         const std::string &vehicle, std::string &msg)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<MissionLaunch> ml;
    // (a vehicle filter looks further: New Glenn flies a few times a month)
    const double horizonD = vehicle.empty() ? 30.0 : 120.0;
    launchUpcoming(t0 - 1.0 * kDay, t0 + horizonD * kDay, 0, 20000, ml);
    const LBatch *best = nullptr;
    double bestT = 1e300;
    for (const MissionLaunch &m : ml)
    {
        auto it = L.batches.find(m.key);
        if (it == L.batches.end() || !it->second.valid)
            continue;
        const LBatch &B = it->second;
        if (B.landKind != kLandShip || B.tLand <= t0 + leadS + 1.0 || B.tLand >= bestT)
            continue;
        if (!vehicle.empty() && L.cat.vehicles[B.veh].id.find(vehicle) == std::string::npos)
            continue;
        if (dayNight != 0)
        {
            const glm::dvec3 sun = sunDirEciAt(B.tLand);
            const double el = glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(B.shipG, B.tLand)), -1.0, 1.0)));
            if ((dayNight > 0 && el < 10.0) || (dayNight < 0 && el > -8.0))
                continue;
        }
        bestT = B.tLand;
        best = &B;
        launchFocusKey = m.key;
    }
    if (!best)
    {
        msg = "no booster landing on a ship in the next " + std::to_string((int)horizonD) + " days" +
              (vehicle.empty() ? std::string() : " (" + vehicle + ")");
        return false;
    }
    const LBatch &B = *best;
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tS = bestT - leadS;
    const double days = std::floor(tS / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tS - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    glm::dvec3 bow;
    const glm::dvec3 S = LaunchImpl::shipEcef(B, bestT, &bow);
    const glm::dvec3 up = S / kR;
    const glm::dvec3 port = glm::cross(up, bow);
    const double a = glm::radians(azDeg);
    const glm::dvec3 g = glm::normalize(S + distM * (std::cos(a) * bow - std::sin(a) * port));
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)heightM;
    // Look at a point above the deck (the booster's landing burn comes down onto it).
    const glm::dvec3 tgt = (kR + B.deckM + std::min(120.0, 0.08 * distM)) * up;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), g));
    const glm::dvec3 north = glm::cross(g, east);
    const glm::dvec3 rn = glm::normalize(tgt - (kR + heightM) * g);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, g), -1.0, 1.0))));
    camera.fovYDeg = 40.0f;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    updatePositions(tS);
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    char b[320];
    snprintf(b, sizeof(b), "%s booster onto %s, %.0f km downrange of %s: touchdown in %.0f s (payload %.0f kg)",
             vh.name.c_str(), vh.ships[(size_t)B.ship].name.c_str(), B.xLand / 1000.0, L.cat.sites[B.site].id.c_str(),
             leadS, B.payloadKg);
    msg = b;
    return true;
}

// The floodlit deck at night (and a booster standing on it): emissive Gaussians in the dust list (cloud_march.comp
// col.w = 1). The mesh renderer has no local lights, so without them a droneship at night was its lights over a
// black hull. Their light, like the lights' sprites, is in the point sources' units: the energy of a point of
// magnitude m1 at 1 km (a deck under ~30 lux of floods is ~-9 physically; the night exposure is not physical, and the
// floods' own sprites are drawn at -3: kept in step with them).
void SatelliteSim::launchFillShipGlow(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    FPROF_SCOPE("launchFillShipGlow");
    if (!launchState)
        return;
    LaunchState &L = *launchState;
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double night = 1.0 - dayness;
    if (night < 0.02)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    // One Gaussian: centre P (ECI), axis a (unit, ECI), sigma along it sA and across sP, the light of a point of
    // magnitude m1 at 1 km, colour col.
    auto add = [&](const glm::dvec3 &P, const glm::dvec3 &aEci, double sA0, double sP0, double m1, const glm::vec3 &col) {
        if (n >= kMaxDustClouds)
            return;
        const glm::dvec3 rel = P - eye;
        const double range = std::max(glm::length(rel), 1.0);
        const double sMin = 0.6 * range * 2.0 * pixFull;
        const double sA = std::max(sA0, sMin), sP = std::max(sP0, sMin);
        const glm::dvec3 a = toEnu(aEci);
        glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
        b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
        const glm::dvec3 c2 = glm::cross(a, b2);
        const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
        auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
        const double J = 0.008 * std::pow(10.0, 0.4 * (6.0 - m1)) * 1.0e6 / K * night;
        const glm::dvec3 ce = toEnu(rel);
        GpuDustCloud &d = out[n++];
        d.c = glm::vec4(glm::vec3(ce), (float)(J * toSky));
        d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                         (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
        d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
        d.col = glm::vec4(col, 1.0f);
        const double R = 5.5 * std::max(sA, sP), dd = glm::length(ce);
        d.cone = R >= 0.95 * dd ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                : glm::vec4(glm::vec3(ce / dd), (float)std::sqrt(1.0 - (R / dd) * (R / dd)));
    };
    const glm::dvec3 eyeE = eciToEcef(eye, t);
    forEachShip(L, t, [&](const LBatch &B) {
        glm::dvec3 bow;
        const glm::dvec3 S = LaunchImpl::shipEcef(B, t, &bow);
        if (glm::length(S - eyeE) > 60e3)
            return;
        const glm::dvec3 up = S / kR;
        const bool jack = L.cat.vehicles[B.veh].ships[(size_t)B.ship].model == "ship_jacklyn";
        // Only while the ship is small on screen (2026-10-21): once its mesh resolves, the deck and the booster on it
        // are lit by the floods in sat_mesh.frag (local lights); these glows over them read as a milky slab.
        const double px = 100.0 / std::max(glm::length(S - eyeE), 1.0) / std::max(pixFull, 1e-9);
        const double keep = 1.0 - glm::smoothstep(6.0, 30.0, px);
        if (keep <= 0.0)
            return;
        const double dK = -2.5 * std::log10(keep);
        // The lit deck (the landing area, a few m thick so it reads from a grazing view).
        const glm::dvec3 Pd = ecefToEci((kR + B.deckM + 1.5) * up + (jack ? 0.0 : -6.0) * bow, t);
        add(Pd, ecefToEci(up, t), 2.0, jack ? 16.0 : 22.0, -4.0 + dK, glm::vec3(1.0f, 0.88f, 0.7f));
        // The booster on it (white, floodlit): a column ~40 m tall.
        if (t >= B.tLand)
        {
            const glm::dvec3 Pb = ecefToEci((kR + B.deckM + 21.0) * up, t);
            add(Pb, ecefToEci(up, t), 12.0, 2.2, -3.5 + dK, glm::vec3(1.0f, 0.92f, 0.8f));
        }
    });
}

// ── Mission milestones (2026-10-20, the user: "skip to the next mission milestone", "mission activity in the info
// screen") ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// A launch's timeline from its batch's phase times: lift-off, max-Q and MECO / staging from the booster's track, the
// fairing, SECO, the deployment, the upper stage's deorbit burn and entry, the ship's descent and catch; the booster's
// own recovery burns are READ OFF its track (every change of its burn kind after staging, then its landing), so a new
// recovery kind (a droneship) shows up without code here. Trains: release, raising, arrival; retirees: deorbit,
// re-entry; a roster satellite in operations: the batch that put it there, and the one that will replace it.
// A subject's skips land `lead` before each milestone (10 s, less where milestones crowd) at 1x, and keep the
// selection and Go to on the subject's object at the new time (missionJump / missionAttachAfterJump).
namespace
{
std::string msFmtRel(double dt)
{
    char b[48];
    const char *pre = dt < 0.0 ? "T-" : "T+";
    const long long s = (long long)std::floor(std::abs(dt));
    if (s >= 86400)
        snprintf(b, sizeof(b), "%s%lldd %02lldh", pre, s / 86400, (s % 86400) / 3600);
    else if (s >= 3600)
        snprintf(b, sizeof(b), "%s%lldh %02lldm", pre, s / 3600, (s % 3600) / 60);
    else if (s >= 60)
        snprintf(b, sizeof(b), "%s%lldm %02llds", pre, s / 60, s % 60);
    else
        snprintf(b, sizeof(b), "%s%llds", pre, s);
    return b;
}
const char *msBurnName(uint8_t burn)
{
    switch (burn)
    {
    case launch::BURN_BOOSTBACK:
        return "Booster boostback burn";
    case launch::BURN_ENTRY:
        return "Booster entry burn";
    case launch::BURN_LANDING:
        return "Booster landing burn";
    case launch::BURN_ASCENT:
        return "Booster burn";
    case launch::BURN_UPPER:
        return "Booster burn";
    default:
        return "Booster burn"; // a kind added later (a droneship's): still a milestone
    }
}
} // namespace

LBatch *SatelliteSim::missionBatch(uint64_t key)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return nullptr;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    auto it = L.batches.find(key);
    if (it == L.batches.end())
    {
        int plane = 0, b = 0;
        int64_t k = 0;
        batchKeyDecode(key, plane, k, b);
        if (plane < 0 || plane >= (int)L.planes.size())
            return nullptr;
        LBatch nb;
        nb.plane = plane;
        nb.k = k;
        nb.b = b;
        const LPlane &pl = L.planes[plane];
        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
        it = L.batches.emplace(key, std::move(nb)).first;
    }
    it->second.touched = std::max(it->second.touched, L.frame);
    return &it->second;
}

// The batch's whole timeline (every object's milestones, `who` says whose), sorted.
void SatelliteSim::missionBatchMilestones(const LBatch &B, std::vector<MissionMilestone> &ms) const
{
    const SatelliteSim &sim = *this;
    const LaunchState &L = *launchState;
    using MM = SatelliteSim::MissionMilestone;
    const uint8_t P = SatelliteSim::kMsPayload, Bo = SatelliteSim::kMsBooster, Tr = SatelliteSim::kMsTrain,
                  Re = SatelliteSim::kMsRetiree;
    ms.clear();
    if (!B.valid || B.veh < 0)
        return;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Program &pg = L.cat.programs[B.prog];
    auto add = [&](double t, uint8_t who, const std::string &label) { ms.push_back(MM{t, label, who, 10.0}); };
    add(B.tL, P | Bo | Tr, "Lift-off from " + L.cat.sites[B.site].name);
    // Max-Q: the largest dynamic pressure on the climb (the booster's track, ground-relative speed). THIS launch's
    // track (2026-10-20, mission-ops suite): a Falcon 9 flying a light payload back to its landing zone has its own
    // (boosterTr); the vehicle's droneship track listed a trim burn and a droneship landing it never flew.
    const Track &tr = LaunchImpl::boosterTr(L, B);
    {
        double qMax = 0.0, tq = -1.0;
        for (size_t i = 0; i < tr.p.size() && (double)i * tr.dt < vh.t1; ++i)
        {
            const double q = 0.5 * launch::airDensity(tr.p[i].y) * glm::dot(tr.v[i], tr.v[i]);
            if (q > qMax)
            {
                qMax = q;
                tq = (double)i * tr.dt;
            }
        }
        if (tq > 5.0)
        {
            char b[64];
            snprintf(b, sizeof(b), "Max-Q (%.0f kPa)", qMax / 1000.0);
            add(B.tL + tq, P | Bo | Tr, b);
        }
    }
    add(B.tL + vh.t1, P | Bo | Tr, "MECO and stage separation");
    // The booster after staging: each change of its burn kind, then its landing (or its end).
    {
        uint8_t prev = launch::BURN_NONE;
        for (size_t i = 0; i < tr.burn.size(); ++i)
        {
            const double tau = (double)i * tr.dt;
            const uint8_t b = tr.burn[i];
            if (tau > vh.t1 + 0.5 && b != prev && b != launch::BURN_NONE)
                add(B.tL + tau, Bo, msBurnName(b));
            prev = b;
        }
        if (tr.tEnd > vh.t1 + 5.0)
        {
            // By where THIS booster comes to rest (B.landKind): a droneship vehicle may fly back to its landing zone
            // or come down on the ground (an inland site's arc).
            const char *end = !tr.landed || B.landKind == kLandNone
                                  ? (vh.recovery == 0 ? "Booster falls into the sea" : "Booster's track ends")
                              : B.landKind == kLandTower ? "Booster caught by the tower"
                              : B.landKind == kLandLz    ? "Booster lands at the landing zone"
                              : B.landKind == kLandShip  ? "Booster lands on the droneship"
                                                         : "Booster touches down on the ground";
            add(B.tL + tr.tEnd, Bo, end);
        }
    }
    if (B.tFair >= 0.0)
        add(B.tFair, P | Tr, "Fairing separation");
    add(B.tL + B.t2, P | Tr, "SECO: in the parking orbit");
    const size_t n = B.mem.size();
    // (2026-10-20) A crew flight: after the separation the payload role is the spacecraft's; the upper stage's own
    // deorbit is kMsStage's.
    const uint8_t St = pg.crew && B.crew.on ? SatelliteSim::kMsStage : P;
    if (pg.crew)
        add(B.tD, P | St, pg.spacecraft.empty() ? "Spacecraft separation" : pg.spacecraft + " separates");
    else if (n > 0)
    {
        add(B.tD, P | Tr, "Deployment begins (" + std::to_string(n) + " satellites)");
        if (n > 1)
            add(B.tD + B.deployInterval * (double)(n - 1), P | Tr, "Last satellite released");
    }
    if (B.tDeorbBurn > B.tD)
        add(B.tDeorbBurn, St, "Upper stage deorbit burn");
    add(B.tStageGone, St, vh.upperDeorbit ? "Upper stage re-enters (burns up)" : "Ship entry interface (100 km)");
    if (pg.crew && B.crew.on)
    {
        // The spacecraft's own flight (lift-off to separation are the shared ones above).
        for (MM &m : ms)
            if (m.who & P)
                m.who |= St;
        const LCrew &C = B.crew;
        const std::string &stn = sim.constellations[L.planes[B.plane].constIdx].name;
        char b[160];
        if (!C.skipRaise1 && !(C.abort == 1 && C.tDeath < C.tB1))
        {
            snprintf(b, sizeof(b), "Phasing burn 1: raising to %.0f km", (C.rP - kR) / 1000.0);
            add(C.tB1, P, b);
            add(C.tB1e, P, "Phasing burn 2: circularising, the station catching up");
        }
        // (2026-10-21) The station's loss (C.abort): the rendezvous called off, the approach turned away, an emergency
        // undocking, or (cargo docked) gone with the station.
        const bool reachesPhasing2 = !(C.abort == 1 && C.tDeath < C.tB2);
        if (C.abort == 1)
            add(C.tDeath, P, stn + " is lost: rendezvous called off");
        if (reachesPhasing2)
        {
            snprintf(b, sizeof(b), "Phasing burn %d: raising to the station's altitude", C.skipRaise1 ? 1 : 3);
            add(C.tB2, P, b);
        }
        if (C.abort != 1)
        {
            add(C.tArr - 30.0, P, "Arrives 5 km behind " + stn + ": the approach begins");
            const double tEnd = C.abort == 2 ? C.tDeath : 1e300;
            auto addA = [&](double t, const std::string &l) {
                if (t < tEnd)
                    add(t, P, l);
            };
            addA(C.segs.empty() ? C.tArr : C.segs[0].t1, "Enters the approach ellipsoid (1 km)");
            if (C.berth)
            {
                // (2026-10-21) Berthing: up the R-bar to the capture point, the arm's capture and berthing.
                addA(C.tAxis, "On the R-bar under " + stn + ", 1 km out");
                addA(C.hold[0], "Hold at 250 m");
                addA(C.hold[1], "Hold at 30 m: go for capture");
                addA(C.armA0, "The arm leaves its park for the capture point");
                addA(C.tCapArr, "At the capture point (" + std::to_string((int)std::lround(C.capM)) + " m): free drift");
                addA(C.tGrapple, "Captured by the station's arm");
                addA(C.tBerth0, "On the arm: moved to the berthing port (" + C.portName + ")");
            }
            else
            {
            addA(C.tAxis, "On the docking axis (" + C.portName + "), 1 km out");
            addA(C.hold[0], "Hold at 400 m");
            addA(C.hold[1], "Hold at 200 m: go for the final approach");
            addA(C.hold[2], "Hold at 20 m");
            }
            if (C.abort == 2)
                add(C.tDeath, P, stn + " is lost: the approach is called off, turning away");
            else
            {
                if (C.berth)
                {
                    add(C.tDock, P, "Berthed to " + stn + " (" + C.portName + "): the bolts drive");
                    if (C.armA2 > C.tDock && C.abort != 4)
                        add(C.armA2, P, "The arm lets go and returns to its park");
                }
                else
                add(C.tDock, P, std::string(C.fast ? "Fast-track docking" : "Soft capture") + ": docked to " + stn + " (" +
                                    C.portName + ")");
                if (C.abort == 4)
                    add(C.tDeath, P, stn + " breaks up: " + pg.spacecraft + " is destroyed with it");
                else if (C.abort == 3)
                    add(C.tUndock, P, stn + " is lost: emergency undocking");
                else if (C.berth)
                {
                    add(C.armD0, P, "The arm takes hold of " + pg.spacecraft);
                    add(C.tUndock, P, "Unberthing: the arm lifts it off the port");
                    add(C.tRelease, P, "Released by the station's arm");
                }
                else
                    add(C.tUndock, P, "Undocking from " + stn);
            }
        }
        if (C.abort != 4)
        {
            if (C.abort == 0 || C.abort == 3)
                add(C.tDepBurn, P, "Departure burn");
            add(C.tDeorb, P, "Deorbit burn");
            if (C.burns)
            {
                add(C.tEI, P, "Entry interface (100 km): breaks up and burns");
            }
            else if (C.runway)
            {
                add(C.tCm, P, "Cargo module jettisoned (it burns up)");
                add(C.tEI, P, "Entry interface (100 km)");
                add(C.tGlide, P, "Entry ends: gliding to the runway");
                add(C.tTD - 90.0, P, "Final approach");
                add(C.tTD, P, "Touchdown");
                add(C.tStop, P, "Wheels stop");
            }
            else
            {
                add(C.tCm, P, pg.crewModel.find("dragon") != std::string::npos ? "Trunk jettison" : "Modules separate for the entry");
                add(C.tEI, P, "Entry interface (100 km)");
                add(C.tChute, P, "Drogue parachutes");
                add(C.tMains, P, "Main parachutes");
                add(C.tSplash, P, C.lands ? "Landing" : "Splashdown");
            }
        }
    }
    if (!vh.upperDeorbit && B.shipLands)
    {
        add(B.tShipDesc, P, "Entry ends: belly-flop descent");
        add(B.tShipFlip, P, "Flip and landing burn");
        add(B.tShipCatch, P, B.landPad >= 0 ? "Ship caught by the tower" : "Ship splashes down");
    }
    if (pg.crew)
    {
        if (!B.crew.on)
            add(B.tA, P, "Docking with " + sim.constellations[L.planes[B.plane].constIdx].name);
    }
    else if (n > 0)
    {
        double r0 = 1e300;
        for (const LMember &m : B.mem)
            r0 = std::min(r0, m.sRaise);
        if (r0 < B.tA)
            add(r0, Tr, "Train starts raising");
        add(B.tA, Tr | Re, B.first ? "Arrival in the slots (first deployment: they were empty)"
                                   : "Arrival in the slots; the old satellites retire");
        if (!B.ends.empty())
        {
            add(B.ends.front(), Re, "First retiree re-enters");
            if (B.ends.size() > 1)
                add(B.ends.back(), Re, "Last retiree re-enters");
        }
    }
    std::stable_sort(ms.begin(), ms.end(), [](const MM &a, const MM &b) { return a.t < b.t; });
}

// Fills s.ms from the batch for its role (and member), the current milestone and the leads.
void SatelliteSim::missionFinish(MissionSubject &s, double now)
{
    LaunchState &L = *launchState;
    LBatch *B = missionBatch(s.key);
    s.ms.clear();
    if ((!B || !B->valid) && !s.roster)
    {
        s.ok = false;
        return;
    }
    std::vector<MissionMilestone> all;
    if (B && B->valid)
        missionBatchMilestones(*B, all);
    const bool member = B && B->valid && s.member >= 0 && (size_t)s.member < B->mem.size();
    for (const MissionMilestone &m : all)
    {
        if (!(m.who & s.role))
            continue;
        if (member && s.role == kMsTrain && (m.label == "Train starts raising" || m.label == "Last satellite released"))
            continue; // the member's own below
        if (member && s.role == kMsRetiree && m.label.find("retiree re-enters") != std::string::npos)
            continue;
        s.ms.push_back(m);
    }
    if (member)
    {
        const LMember &mb = B->mem[(size_t)s.member];
        if (s.role == kMsTrain)
        {
            if (mb.tRel > B->tD + 0.5)
                s.ms.push_back({mb.tRel, "Released from the dispenser", kMsTrain, 10.0});
            if (mb.sRaise < B->tA)
                s.ms.push_back({mb.sRaise, "Starts raising to its slot", kMsTrain, 10.0});
        }
        else if (s.role == kMsRetiree)
        {
            for (MissionMilestone &m : s.ms)
                if (m.t == B->tA)
                    m.label = "Replacement arrives: retires, starts deorbiting";
            s.ms.push_back({mb.tEnd, "Re-enters", kMsRetiree, 10.0});
        }
    }
    // A roster satellite: how it got there (the batch before).
    if (s.roster && s.keyPrev)
        if (LBatch *Bp = missionBatch(s.keyPrev); Bp && Bp->valid)
        {
            s.ms.push_back({Bp->tL, "Launched (" + L.cat.vehicles[Bp->veh].name + ")", kMsTrain, 10.0});
            if (s.member >= 0 && (size_t)s.member < Bp->mem.size())
                s.ms.push_back({Bp->mem[(size_t)s.member].tRel, "Released from the dispenser", kMsTrain, 10.0});
            s.ms.push_back({Bp->tA, "Arrived in its slot", kMsTrain, 10.0});
        }
    std::stable_sort(s.ms.begin(), s.ms.end(), [](const MissionMilestone &a, const MissionMilestone &b) { return a.t < b.t; });
    // Leads: 10 s, less where two crowd (never past the previous one).
    for (size_t i = 0; i < s.ms.size(); ++i)
    {
        const double gap = i ? s.ms[i].t - s.ms[i - 1].t : 1e9;
        s.ms[i].lead = std::clamp(0.45 * gap, 0.5, 10.0);
    }
    s.cur = -1;
    for (size_t i = 0; i < s.ms.size(); ++i)
        if (s.ms[i].t <= now)
            s.cur = (int)i;
    s.t0 = s.ms.empty() ? 0.0 : s.ms.front().t;
    s.t1 = s.ms.empty() ? 0.0 : s.ms.back().t;
    s.ok = !s.ms.empty();
}

bool SatelliteSim::missionSubjectOfBatch(uint64_t key, MissionSubject &out, uint8_t role)
{
    out = MissionSubject{};
    if (!launchState)
        launchInitBlock();
    LBatch *B = missionBatch(key);
    if (!B || !B->valid)
        return false;
    LaunchState &L = *launchState;
    const launch::Program &pg = L.cat.programs[B->prog];
    const std::string &target = constellations[L.planes[B->plane].constIdx].name;
    out.key = key;
    out.role = role;
    out.title = pg.crew ? pg.spacecraft + " to " + target + " on " + L.cat.vehicles[B->veh].name
                        : L.cat.vehicles[B->veh].name + " - " + pg.name + ", " + std::to_string(B->mem.size()) +
                              " satellites to " + target + (B->first ? " (first deployment)" : "");
    out.what = role == kMsBooster ? "the booster" : role == kMsTrain ? "the train" : role == kMsRetiree ? "the retirees"
               : role == kMsStage ? "the upper stage"
               : pg.crew && B->crew.on ? "the " + pg.spacecraft
                                       : "the rocket";
    missionFinish(out, (double)simDayJ2000 * 86400.0 + simSecInDay);
    return out.ok;
}

// The roster satellite at index `sat` (its plane's batches: the one that put it there, the one that replaces it).
bool SatelliteSim::missionRosterBatches(int sat, double t, uint64_t &keyPrev, uint64_t &keyNext, int &member)
{
    if (!launchState)
        return false;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return false;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    if (L.satPlane.size() != activeSatCount)
    {
        L.satPlane.assign(activeSatCount, -1);
        L.satPos.assign(activeSatCount, 0);
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
            for (size_t j = 0; j < L.planes[pi].sats.size(); ++j)
                if (L.planes[pi].sats[j] < activeSatCount && !L.planes[pi].extra) // (a station's cargo planes: its crew plane stands for it)
                {
                    L.satPlane[L.planes[pi].sats[j]] = (int32_t)pi;
                    L.satPos[L.planes[pi].sats[j]] = (uint32_t)j;
                }
    }
    if (sat < 0 || (uint32_t)sat >= activeSatCount || L.satPlane[(size_t)sat] < 0)
        return false;
    const int pi = L.satPlane[(size_t)sat];
    const LPlane &pl = L.planes[(size_t)pi];
    const launch::Program &pg = L.cat.programs[pl.prog];
    if (pg.crew)
        return false;
    const int payload = LaunchImpl::payloadOf(*this, L, pl);
    const int pos = (int)L.satPos[(size_t)sat];
    const int b = pos / payload;
    member = pos % payload;
    const double life = progLifeS(pg, (double)launchLifeScale);
    const int64_t k = (int64_t)std::floor((t - b * pl.gap) / life - pl.phase);
    keyPrev = batchKey(pi, k, b);
    keyNext = batchKey(pi, k + 1, b);
    // The deployment era: the first fill stands for every lattice refresh it precedes (they do not fly).
    if (!pl.fillT.empty() && (size_t)b < pl.fillT.size())
    {
        int64_t kn = k + 1;
        while (!LaunchImpl::batchExists(L, pl, kn, b, life) && kn < k + 64)
            ++kn;
        keyNext = batchKey(pi, kn, b);
        if (!LaunchImpl::batchExists(L, pl, k, b, life))
        {
            keyPrev = batchKey(pi, kFillK, b);
            if (t < pl.fillT[(size_t)b]) // not there yet: its first fill is next
            {
                keyNext = keyPrev;
                keyPrev = 0;
            }
        }
    }
    return true;
}

// Extra rows for the MISSION section: what comes next, a train member's climb, a satellite's design life.
void SatelliteSim::missionFacts(MissionSubject &s, int idx, double t)
{
    auto inStr = [](double dt) {
        std::string r = msFmtRel(dt).substr(2);
        return dt >= 0.0 ? "in " + r : r + " ago";
    };
    for (const MissionMilestone &m : s.ms)
        if (m.t > t)
        {
            s.facts.push_back({"Coming up", m.label + ", " + inStr(m.t - t)});
            break;
        }
    if (!launchState)
        return;
    const LaunchState &L = *launchState;
    LBatch *B = missionBatch(s.key);
    if ((uint32_t)idx >= activeSatCount && (uint32_t)idx < poolIdx0() && (uint32_t)idx != transientSatIdx() &&
        (uint32_t)idx != followSlotIdx())
    {
        const std::string st = launchStatus((uint32_t)idx - activeSatCount);
        if (!st.empty())
            s.facts.push_back({"Now", st.substr(st.find(" - ") == std::string::npos ? 0 : st.find(" - ") + 3)});
    }
    if (B && B->valid && s.member >= 0 && (size_t)s.member < B->mem.size() && !s.roster)
    {
        const LMember &mb = B->mem[(size_t)s.member];
        char b[120];
        if (s.role == kMsTrain && t >= mb.sRaise && t < B->tA)
        {
            snprintf(b, sizeof(b), "%.0f%% of the climb to %.0f km", 100.0 * (t - mb.sRaise) / std::max(1.0, B->tA - mb.sRaise),
                     (mb.rSlot - kR) / 1000.0);
            s.facts.push_back({"Raising", b});
        }
        if (s.role == kMsTrain && t < B->tA)
            s.facts.push_back({"Arrives", inStr(B->tA - t) + " (" + fmtUtcReadable(B->tA) + ")"});
        if (s.role == kMsRetiree && t < mb.tEnd)
            s.facts.push_back({"Re-enters", inStr(mb.tEnd - t) + " (" + fmtUtcReadable(mb.tEnd) + ")"});
    }
    if (s.roster)
    {
        if (LBatch *Bp = missionBatch(s.keyPrev); Bp && Bp->valid)
        {
            s.facts.push_back({"In its slot", "since " + fmtUtcReadable(Bp->tA) + " (" + inStr(Bp->tA - t) + ")"});
            const launch::Program &pg = L.cat.programs[Bp->prog];
            const double life = progLifeS(pg, (double)launchLifeScale);
            char b[96];
            snprintf(b, sizeof(b), "%.1f years, %.0f%% used", life / kYear, 100.0 * std::clamp((t - Bp->tA) / life, 0.0, 9.99));
            s.facts.push_back({"Design life", b});
        }
        if (B && B->valid)
            s.facts.push_back({"Replaced", inStr(B->tA - t) + " (" + fmtUtcReadable(B->tA) + ")"});
    }
}

bool SatelliteSim::missionSubjectOfIndex(int idx, MissionSubject &out)
{
    const bool ok = missionSubjectOfIndexRaw(idx, out);
    if (ok)
        missionFacts(out, idx, (double)simDayJ2000 * 86400.0 + simSecInDay);
    return ok;
}

bool SatelliteSim::missionSubjectOfIndexRaw(int idx, MissionSubject &out)
{
    out = MissionSubject{};
    if (!launchState || !launchesEnabled || idx < 0 || idx >= (int)satOrbits.size())
        return false;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const SelTransient *tr = nullptr;
    if ((uint32_t)idx == transientSatIdx() && selIsTransient())
        tr = &selTransientCur;
    else if ((uint32_t)idx == followSlotIdx() && followIsTransient() && followTrKey)
        tr = &followTrCur;
    if (tr)
    {
        if (tr->kind == 1 || tr->kind == 2 || tr->kind == 4 || tr->kind == 8 || tr->kind == 9)
        {
            LBatch *B = missionBatch(tr->a);
            if (!B || !B->valid)
                return false;
            const double t1 = launchState->cat.vehicles[B->veh].t1;
            const bool booster = tr->kind == 1 && tr->stage == 0 && t - B->tL >= t1;
            // (2026-10-20) A crewed upper stage after its spacecraft has left, or its re-entry: the stage's own role.
            const bool stage = B->crew.on && ((tr->kind == 1 && tr->stage == 1 && t >= B->crew.tSep) || tr->kind == 4);
            if (!missionSubjectOfBatch(tr->a, out, booster ? kMsBooster : stage ? kMsStage : kMsPayload))
                return false;
            out.what = booster ? "the booster" : tr->kind == 2 ? "the ship" : tr->kind == 4 ? "the upper stage's re-entry"
                     : tr->kind == 8 ? "the " + launchState->cat.programs[B->prog].spacecraft
                     : tr->kind == 9 ? "the " + launchState->cat.programs[B->prog].spacecraft + "'s re-entry"
                                     : (tr->stage == 0 ? "the rocket" : "the upper stage");
            return true;
        }
        if (tr->kind == 3 && tr->a < activeSatCount)
        {
            // A retiree's re-entry: the batch that retired it (its replacement's, arrived before now).
            uint64_t kp = 0, kn = 0;
            int m = -1;
            if (!missionRosterBatches((int)tr->a, t, kp, kn, m) || !missionSubjectOfBatch(kp, out, kMsRetiree))
                return false;
            out.member = m;
            out.sat = (int)tr->a;
            out.what = "a retired satellite re-entering";
            missionFinish(out, t);
            return out.ok;
        }
        return false;
    }
    if ((uint32_t)idx >= activeSatCount && (uint32_t)idx < poolIdx0())
    {
        uint32_t m = 0;
        const uint64_t owner = launchSlotOwner((uint32_t)idx - activeSatCount, m);
        if (!owner)
            return false;
        const uint8_t role = owner % 2 == 0 ? kMsTrain : kMsRetiree;
        if (!missionSubjectOfBatch(owner / 2, out, role))
            return false;
        out.member = (int)m;
        if (LBatch *B = missionBatch(owner / 2); B && m < B->mem.size())
            out.sat = (int)B->mem[m].sat;
        out.what = role == kMsTrain ? "a satellite in the train" : "a retired satellite";
        missionFinish(out, t);
        return out.ok;
    }
    if ((uint32_t)idx < activeSatCount)
    {
        if (rosterFateAt((uint32_t)idx, t) != 0)
            return false; // dead or gone: the Kessler cascade owns it now
        uint64_t kp = 0, kn = 0;
        int m = -1;
        if (!missionRosterBatches(idx, t, kp, kn, m))
            return false;
        if (!missionSubjectOfBatch(kn, out, kMsRetiree))
        {
            // No replacement flies (launches stood down, ...): the past only.
            out = MissionSubject{};
            out.key = kn;
            out.role = kMsRetiree;
        }
        out.roster = true;
        out.keyPrev = kp;
        out.member = m;
        out.sat = idx;
        out.what = "a satellite in operations";
        const LaunchState &L = *launchState;
        if (LBatch *Bp = missionBatch(kp); Bp && Bp->valid)
        {
            const launch::Program &pg = L.cat.programs[Bp->prog];
            out.title = pg.name + ": launched on " + L.cat.vehicles[Bp->veh].name + " from " + L.cat.sites[Bp->site].name;
        }
        else
            out.title = "No launch on record for this slot";
        missionFinish(out, t);
        if (LBatch *Bn = missionBatch(kn); !Bn || !Bn->valid)
            out.facts.push_back({"Replacement", Bn ? "none scheduled (" + Bn->why + ")" : "none scheduled"});
        out.ok = !out.ms.empty();
        return out.ok;
    }
    return false;
}

bool SatelliteSim::missionSubjectNow(MissionSubject &out)
{
    if (followActive && followSatIndex >= 0 && missionSubjectOfIndex(followSatIndex, out))
    {
        out.followed = true;
        return true;
    }
    return selectedSatIndex >= 0 && missionSubjectOfIndex(selectedSatIndex, out);
}

// Puts the selection / Go to (whichever was on the subject) on the subject's object at time t.
void SatelliteSim::missionAttachAt(const MissionSubject &s, double t, bool sel, bool fol)
{
    msAttachOwner = 0;
    if (!sel && !fol)
        return;
    LBatch *B = missionBatch(s.key);
    if (!B || !B->valid)
        return;
    auto takeTransient = [&](const SelTransient &c) {
        if (fol && followActive)
            followTakeTransient(c, glm::dvec3(0.0), t); // the camera keeps its offset in the object's frame
        if (sel)
        {
            selTransientTake(c);
            selectedPlanetIndex = -1;
            selectedSatIndex = (int)transientSatIdx();
            formatSelectedSatInfo();
        }
    };
    auto takeIndex = [&](int idx) {
        if (fol && followActive)
        {
            followSatIndex = idx;
            followLaunchOwner = 0;
            followTrKey = 0;
            followWantKind = 0;
            snprintf(followLabel, sizeof(followLabel), "Following %s", satLabel(idx).c_str());
        }
        if (sel)
            selectSatellite(idx);
    };
    auto rocket = [&]() {
        SelTransient c;
        if (launchRocketTransient(s.key, t, c))
            takeTransient(c);
    };
    if (s.role == kMsPayload)
    {
        rocket(); // (a crew flight past its separation: the spacecraft, by transientContinue)
        return;
    }
    if (s.role == kMsStage)
    {
        // A crewed upper stage after its spacecraft has left (2026-10-20).
        SelTransient c;
        c.kind = 1;
        c.a = s.key;
        c.stage = 1;
        c.key = (s.key << 3) ^ 2ull;
        c.tau = t - B->tL;
        uint8_t want = 0;
        if (transientContinue(c, t, want) && c.kind == 1)
        {
            glm::dvec3 ax = LaunchImpl::stageNose(*launchState, *B, 1, c.tau);
            c.axis = glm::length(ax) > 0.5 ? ax : glm::normalize(c.pos);
            takeTransient(c);
        }
        return;
    }
    if (s.role == kMsBooster)
    {
        const launch::Vehicle &vh = launchState->cat.vehicles[B->veh];
        SelTransient c;
        c.kind = 1;
        c.a = s.key;
        c.stage = 0;
        c.key = (s.key << 3) ^ 1ull;
        auto ti = rocketTypes.find("rocket_" + vh.id + (t - B->tL < vh.t1 ? "_stack" : "_booster"));
        c.type = ti != rocketTypes.end() ? ti->second : -1;
        if (transientExactState(c, t))
        {
            glm::dvec3 ax = LaunchImpl::stageNose(*launchState, *B, 0, c.tau);
            c.axis = glm::length(ax) > 0.5 ? ax : glm::normalize(c.pos);
            takeTransient(c);
        }
        return;
    }
    // A member (or a roster satellite: the retiree of s.key, the train member of s.keyPrev).
    if (s.member < 0)
        return;
    const LBatch *Bt = B; // the batch whose train it is at time t
    uint64_t trainKey = s.key;
    if (s.role == kMsRetiree)
    {
        if (t >= B->tA)
        {
            if ((size_t)s.member < B->mem.size() && t < B->mem[(size_t)s.member].tEnd)
            {
                msAttachOwner = s.key * 2 + 1;
                msAttachM = (uint16_t)s.member;
            }
            else
                return; // its re-entry: the fireball, from the list
        }
        else if (s.roster && s.keyPrev)
        {
            LBatch *Bp = missionBatch(s.keyPrev);
            if (Bp && Bp->valid && t < Bp->tA)
            {
                Bt = Bp;
                trainKey = s.keyPrev;
            }
            else
            {
                takeIndex(s.sat); // in operations
                return;
            }
        }
        else
        {
            takeIndex(s.sat);
            return;
        }
    }
    if (!msAttachOwner)
    {
        if (t >= Bt->tA)
        {
            takeIndex(s.sat >= 0 ? s.sat : (int)Bt->mem[(size_t)std::min<int>(s.member, (int)Bt->mem.size() - 1)].sat);
            return;
        }
        if (t < Bt->tD)
        {
            SelTransient c;
            if (launchRocketTransient(trainKey, t, c))
                takeTransient(c); // still on its rocket
            return;
        }
        msAttachOwner = trainKey * 2;
        msAttachM = (uint16_t)s.member;
    }
    // The record's slot is allocated by launchUpdate at the new time: attached right after it (this frame).
    msAttachSel = sel;
    msAttachFollow = fol && followActive;
    msAttachTries = 0;
    msAttachFallback = s.sat;
    if (msAttachFollow)
        followLaunchOwner = 0; // the old slot changes hands: not "the object is gone"
}

void SatelliteSim::missionAttachAfterJump()
{
    FPROF_SCOPE("missionAttachAfterJump");
    if (!msAttachOwner || !launchState)
        return;
    const LaunchState &L = *launchState;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
        if (L.slots[s].live && L.slots[s].owner == msAttachOwner && L.slots[s].m == msAttachM)
        {
            const int idx = (int)(activeSatCount + s);
            if (msAttachFollow && followActive)
            {
                followSatIndex = idx;
                followLaunchOwner = msAttachOwner;
                followLaunchM = msAttachM;
                followTrKey = 0;
                followWantKind = 0;
                snprintf(followLabel, sizeof(followLabel), "Following %s", launchLabel(s).c_str());
            }
            if (msAttachSel)
            {
                selectedSatIndex = idx;
                selectedPlanetIndex = -1;
                formatSelectedSatInfo();
            }
            msAttachOwner = 0;
            return;
        }
    // Not up yet (the staging is shared): a few frames, then the satellite's roster slot.
    if (++msAttachTries > 30)
    {
        msAttachOwner = 0;
        if (msAttachFallback >= 0)
        {
            if (msAttachFollow && followActive)
            {
                followSatIndex = msAttachFallback;
                followLaunchOwner = 0;
            }
            if (msAttachSel)
                selectSatellite(msAttachFallback);
        }
    }
}

bool SatelliteSim::missionJump(const MissionSubject &s, int dir, int index, std::string &msg)
{
    if (!s.ok || s.ms.empty())
    {
        msg = "No mission milestones here";
        return false;
    }
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    int pick = -1;
    if (index >= 0)
        pick = index < (int)s.ms.size() ? index : -1;
    else if (dir > 0)
    {
        for (size_t i = 0; i < s.ms.size() && pick < 0; ++i)
            if (s.ms[i].t - s.ms[i].lead > now + 0.5)
                pick = (int)i;
    }
    else
    {
        for (size_t i = s.ms.size(); i-- > 0 && pick < 0;)
            if (s.ms[i].t - s.ms[i].lead < now - 1.5)
                pick = (int)i;
    }
    if (pick < 0)
    {
        msg = dir > 0 ? "No later milestone" : "No earlier milestone";
        return false;
    }
    const MissionMilestone &m = s.ms[(size_t)pick];
    const double tNew = m.t - m.lead;
    // Who rides the subject: the selection (it or its slot) and Go to.
    const bool sel = selectedSatIndex >= 0 && [&]() {
        MissionSubject q;
        return missionSubjectOfIndex(selectedSatIndex, q) && q.key == s.key && q.role == s.role && q.member == s.member;
    }();
    const bool fol = followActive && followSatIndex >= 0 && [&]() {
        MissionSubject q;
        return missionSubjectOfIndex(followSatIndex, q) && q.key == s.key && q.role == s.role && q.member == s.member;
    }();
    const double days = std::floor(tNew / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tNew - days * 86400.0;
    timeScaleIdx = 0; // at 1x: the milestone happens in front of you
    timeDir = 1.0f;
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    missionAttachAt(s, tNew, sel, fol);
    updatePositions(tNew);
    char b[200];
    snprintf(b, sizeof(b), "%s in %.0f s (%s)", m.label.c_str(), m.lead, msFmtRel(m.t - s.ms.front().t).c_str());
    msg = b;
    infoMissionWall_ = followMissionWall_ = -1e300; // the readouts refresh now
    return true;
}

bool SatelliteSim::missionJumpNow(int dir, std::string &msg)
{
    MissionSubject s;
    if (!missionSubjectNow(s))
    {
        msg = followActive || selectedSatIndex >= 0 ? "This object has no mission timeline"
                                                    : "Select a rocket, a train or a satellite first";
        return false;
    }
    return missionJump(s, dir, -1, msg);
}

nlohmann::json SatelliteSim::missionSubjectJson(const MissionSubject &s) const
{
    nlohmann::json j;
    j["ok"] = s.ok;
    if (!s.ok && s.ms.empty())
        return j;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["title"] = s.title;
    j["what"] = s.what;
    j["role"] = s.role;
    j["member"] = s.member;
    j["sat"] = s.sat;
    j["roster"] = s.roster;
    j["followed"] = s.followed;
    j["current"] = s.cur;
    nlohmann::json ms = nlohmann::json::array();
    for (size_t i = 0; i < s.ms.size(); ++i)
        ms.push_back({{"i", i}, {"label", s.ms[i].label}, {"utc", fmtUtcReadable(s.ms[i].t)}, {"in_s", s.ms[i].t - now},
                      {"lead_s", s.ms[i].lead}, {"who", s.ms[i].who}});
    j["milestones"] = ms;
    nlohmann::json f = nlohmann::json::array();
    for (const auto &kv : s.facts)
        f.push_back({kv.first, kv.second});
    j["facts"] = f;
    return j;
}

// ── The deployment era (2026-10-20) ──────────────────────────────────────────────────────────────────
// The constellations are BUILT UP between the late 2010s and 2036: every plane a program's "deployment" block covers is
// first filled by launches along the block's S-curve (LPlane::fillT, buildPlanes), and a slot is not there before its
// first fill arrives; stations, the 2020 benchmark shells and the debris field follow "roster_history" (count over
// time, members in a hashed order). rosterLife holds each roster satellite's born / gone second (since the Kessler
// epoch): sat_orbit.comp hides it outside them, the Kessler world leaves it out of collisions and the storm.
namespace
{
constexpr int32_t kLifeAlways = INT32_MIN + 1;
int32_t lifeSec(double t, double epoch)
{
    if (t <= -1e299)
        return kLifeAlways;
    if (t >= 1e299)
        return kessler::kNever;
    return (int32_t)std::clamp(std::ceil(t - epoch), -2147483000.0, 2147483000.0);
}
// A piecewise-linear count (held at its ends): the first time it rises above `level`, then the first after that it
// falls back to it (+-inf: never).
void countCrossings(const std::vector<std::pair<double, double>> &c, double scale, double level, double &born, double &gone)
{
    born = 1e300;
    gone = 1e300;
    if (c.empty())
    {
        born = -1e300;
        return;
    }
    bool present = c.front().second * scale > level;
    if (present)
        born = -1e300;
    for (size_t i = 0; i + 1 < c.size(); ++i)
    {
        const double t0 = c[i].first, t1 = c[i + 1].first, c0 = c[i].second * scale, c1 = c[i + 1].second * scale;
        if (!present && c0 <= level && c1 > level)
        {
            born = t0 + (level - c0) / (c1 - c0) * (t1 - t0);
            present = true;
        }
        else if (present && c0 > level && c1 <= level)
        {
            gone = t0 + (c0 - level) / (c0 - c1) * (t1 - t0);
            return;
        }
    }
}
} // namespace

void SatelliteSim::rosterLifeEnsure()
{
    FPROF_SCOPE("rosterLifeEnsure");
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    const uint64_t key = 1ull | (launchDeployEra ? 2ull : 0ull) | (L.loaded ? 4ull : 0ull) |
                         (launchEraBenchmarkShells ? 8ull : 0ull) | ((uint64_t)activeSatCount << 8);
    if (key == rosterLifeKey && rosterLife.size() == 2ull * activeSatCount)
        return;
    const auto t0 = std::chrono::steady_clock::now();
    rosterLifeKey = key;
    rosterLifeUploaded = false;
    rosterLife.assign(2ull * activeSatCount, 0);
    for (uint32_t i = 0; i < activeSatCount; ++i)
    {
        rosterLife[2ull * i] = kLifeAlways;
        rosterLife[2ull * i + 1] = kessler::kNever;
    }
    const double E = kesslerEpoch();
    if (launchDeployEra && L.loaded && activeSatCount > 0)
    {
        if (L.planesBuilt && L.deployOn != launchDeployEra)
            launchInitBlock(); // (the batches were built for the other era)
        if (!L.planesBuilt)
            LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
        size_t nFill = 0;
        for (const LPlane &pl : L.planes)
        {
            if (pl.fillT.empty())
                continue;
            const int payload = LaunchImpl::payloadOf(*this, L, pl);
            for (size_t j = 0; j < pl.sats.size(); ++j)
                if (pl.sats[j] < activeSatCount)
                {
                    rosterLife[2ull * pl.sats[j]] = lifeSec(pl.fillT[std::min(j / (size_t)payload, pl.fillT.size() - 1)], E);
                    ++nFill;
                }
        }
        // Groups without launches (stations, benchmark shells, the debris field).
        size_t nHist = 0;
        for (size_t c = 0; c < constellations.size(); ++c)
        {
            const ConstellationConfig &cc = constellations[c];
            const launch::RosterHistory *rh = nullptr;
            for (const launch::RosterHistory &h : L.cat.history)
            {
                for (const std::string &m : h.match)
                    if (cc.name.find(m) != std::string::npos)
                        rh = &h;
                if (rh)
                    break;
            }
            if (!rh)
                continue;
            const uint32_t i0 = cc.orbitStart, i1 = std::min(cc.orbitStart + cc.orbitCount, activeSatCount);
            if (i1 <= i0)
                continue;
            // Members in a hashed order (a growing debris field fills in everywhere at once, not plane by plane).
            std::vector<std::pair<double, uint32_t>> ord;
            ord.reserve(i1 - i0);
            for (uint32_t i = i0; i < i1; ++i)
                ord.push_back({h01(0x5EED0000ull + c, i, 3), i});
            std::sort(ord.begin(), ord.end());
            const double scale = rh->fraction ? (double)(i1 - i0) : 1.0;
            // A benchmark shell stands for satellites Starlink Gen1's first fills already fly (2026-10-21): left out
            // of the era's sky unless the player opts in (it would count them twice in 2019-2024).
            const bool hideBench = rh->benchmark && !launchEraBenchmarkShells;
            for (size_t r = 0; r < ord.size(); ++r)
            {
                double born, gone;
                countCrossings(rh->count, scale, (double)r + 0.5, born, gone);
                if (hideBench)
                    born = gone = 1e300;
                rosterLife[2ull * ord[r].second] = lifeSec(born, E);
                rosterLife[2ull * ord[r].second + 1] = lifeSec(gone, E);
                ++nHist;
            }
        }
        char m[200];
        snprintf(m, sizeof(m), "launches: deployment era: %zu satellites by first fills, %zu by history, %.0f ms", nFill, nHist,
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        Log::line(m);
    }
    // The Kessler world was built with another table: re-simulate (its cache key includes it).
    if (kWorker && kLifeHashKey != rosterLifeKey)
        kRestartPending = true;
}

// The share of roster satellites [i0, i1) there at t (a shell group, a disk's ring): sorted born / gone seconds per
// range, built on first use and dropped when rosterLife is recomputed.
double SatelliteSim::rosterRangePresentFrac(uint32_t i0, uint32_t i1, double t) const
{
    i1 = std::min(i1, activeSatCount);
    if (i1 <= i0 || rosterLife.size() < 2ull * i1)
        return 1.0;
    if (rangeLifeKey_ != rosterLifeKey)
    {
        rangeLifeCache_.clear();
        rangeLifeKey_ = rosterLifeKey;
    }
    auto &e = rangeLifeCache_[((uint64_t)i0 << 32) | i1];
    if (e.first.empty())
    {
        for (uint32_t i = i0; i < i1; ++i)
        {
            e.first.push_back(rosterLife[2ull * i]);
            if (rosterLife[2ull * i + 1] != kessler::kNever)
                e.second.push_back(rosterLife[2ull * i + 1]);
        }
        std::sort(e.first.begin(), e.first.end());
        std::sort(e.second.begin(), e.second.end());
    }
    const int32_t sec = (int32_t)std::clamp(std::floor(t - kesslerEpoch()), -2147483000.0, 2147483000.0);
    const size_t born = (size_t)(std::upper_bound(e.first.begin(), e.first.end(), sec) - e.first.begin());
    const size_t gone = (size_t)(std::upper_bound(e.second.begin(), e.second.end(), sec) - e.second.begin());
    return born > gone ? (double)(born - gone) / (double)e.first.size() : 0.0;
}

void SatelliteSim::rosterLifeUpload(VulkanContext &ctx)
{
    FPROF_SCOPE("rosterLifeUpload");
    if (kesslerBuf == VK_NULL_HANDLE || activeSatCount == 0)
        return;
    const VkDeviceSize bytes = 8ull * activeSatCount;
    std::vector<int32_t> words;
    const std::vector<int32_t> *src = &rosterLife;
    if (rosterLife.size() < 2ull * activeSatCount)
    {
        words.assign(2ull * activeSatCount, kessler::kNever);
        for (uint32_t i = 0; i < activeSatCount; ++i)
            words[2ull * i] = kLifeAlways;
        src = &words;
    }
    VkBuffer st;
    VkDeviceMemory sm;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, st, sm);
    void *m = nullptr;
    vkMapMemory(ctx.device, sm, 0, bytes, 0, &m);
    memcpy(m, src->data(), (size_t)bytes);
    vkUnmapMemory(ctx.device, sm);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkBufferCopy region{0, 8ull * activeSatCount, bytes}; // after the 2 dead / end words per satellite
    vkCmdCopyBuffer(cmd, st, kesslerBuf, 1, &region);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, st, nullptr);
    vkFreeMemory(ctx.device, sm, nullptr);
    rosterLifeUploaded = src == &rosterLife;
}

std::vector<SatelliteSim::DeployRow> SatelliteSim::deployRows(double t) const
{
    std::vector<DeployRow> out;
    if (!launchState)
        return out;
    const LaunchState &L = *launchState;
    if (!L.deployOn)
        return out;
    for (const LaunchState::Cohort &co : L.cohorts)
    {
        const launch::Program &pg = L.cat.programs[(size_t)co.prog];
        const double life = progLifeS(pg, (double)launchLifeScale);
        DeployRow r;
        r.name = co.name;
        r.target = co.N;
        r.deployed = co.N * co.curve.frac(t);
        r.startT = co.curve.S;
        r.completeT = co.curve.C;
        // Launches a day: the first fills (the curve's slope) + the refreshes of planes past their first life.
        const double h = 0.5 * kDay;
        const double fillPerDay = co.nBatches * (co.curve.frac(t + h) - co.curve.frac(t - h)) / (2.0 * h) * kDay;
        const double refreshPerDay = co.nBatches * co.curve.frac(t - L.cat.minFirstLifeFrac * life) / (life / kDay);
        r.perDay = fillPerDay + refreshPerDay;
        out.push_back(r);
    }
    return out;
}

nlohmann::json SatelliteSim::deployStateJson() const
{
    nlohmann::json j;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["enabled"] = launchDeployEra;
    j["utc"] = fmtUtc(t);
    nlohmann::json rows = nlohmann::json::array();
    for (const DeployRow &r : deployRows(t))
        rows.push_back({{"cohort", r.name}, {"deployed", std::lround(r.deployed)}, {"target", std::lround(r.target)},
                        {"launches_per_day", r.perDay}, {"start", fmtUtc(r.startT)}, {"complete", fmtUtc(r.completeT)}});
    j["cohorts"] = rows;
    // The roster now: how many satellites are there per constellation (rosterLife).
    nlohmann::json cs = nlohmann::json::object();
    const int32_t sec = (int32_t)std::clamp(std::floor(t - kesslerEpoch()), -2147483000.0, 2147483000.0);
    uint64_t total = 0, there = 0;
    for (const ConstellationConfig &cc : constellations)
    {
        uint64_t n = 0;
        const uint32_t i1 = std::min(cc.orbitStart + cc.orbitCount, activeSatCount);
        for (uint32_t i = cc.orbitStart; i < i1; ++i)
            n += 2ull * i + 1 >= rosterLife.size() || (sec >= rosterLife[2ull * i] && sec < rosterLife[2ull * i + 1]);
        cs[cc.name] = {{"there", n}, {"of", i1 > cc.orbitStart ? i1 - cc.orbitStart : 0}};
        total += i1 > cc.orbitStart ? i1 - cc.orbitStart : 0;
        there += n;
    }
    j["constellations"] = cs;
    j["roster_there"] = there;
    j["roster_total"] = total;
    if (launchState)
    {
        const LaunchState &L = *launchState;
        int online = 0, onlineStarship = 0;
        for (const LPad &p : L.pads)
        {
            const int k = padKindAt(p, t, launchDeployEra);
            online += k >= 0;
            onlineStarship += k == 2;
        }
        j["pads_online"] = online;
        j["starship_pads_online"] = onlineStarship;
        j["pads_total"] = L.pads.size();
    }
    return j;
}

// Launch counts from the schedule alone (no batch is built): per program, per calendar year, by arrival less the mean
// raise (+ ~1 day for the window) — the first fills and the refreshes that exist; and the Starship launches per day
// (programs flying Starship only, early vehicles excepted), their busiest day. Approximate: a batch that turns out
// invalid (no window, a pad taken) still counts.
nlohmann::json SatelliteSim::deployStatsJson(double fromT, double toT, double stepDays) const
{
    nlohmann::json j;
    if (!launchState)
        return j;
    LaunchState &L = const_cast<LaunchState &>(*launchState);
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return j;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    int starshipVeh = -1;
    for (size_t v = 0; v < L.cat.vehicles.size(); ++v)
        if (L.cat.vehicles[v].id == "starship")
            starshipVeh = (int)v;
    const int nDays = std::max(1, (int)std::ceil((toT - fromT) / kDay));
    std::vector<float> ssDay((size_t)nDays, 0.0f), allDay((size_t)nDays, 0.0f);
    std::map<std::string, std::map<int, int>> byProgYear;
    std::map<std::string, int> fillByProg, refreshByProg;
    for (const LPlane &pl : L.planes)
    {
        const launch::Program &pg = L.cat.programs[(size_t)pl.prog];
        const double life = progLifeS(pg, (double)launchLifeScale);
        const double raise = 0.5 * (pg.raiseDays[0] + pg.raiseDays[1]) * kDay + 1.0 * kDay;
        const bool ssOnly = pg.vehicles.size() == 1 && pg.vehicles[0] == starshipVeh;
        const launch::Deployment *dp =
            pl.cohort >= 0 ? &pg.deploy[(size_t)L.cohorts[(size_t)pl.cohort].dep] : nullptr;
        LaunchImpl::forBatches(L, pl, life, fromT + raise, toT + raise, [&](int64_t k, int b) {
            const double tl = LaunchImpl::arrivalOf(pl, k, b, life) - (pg.crew ? 0.0 : raise);
            if (tl < fromT || tl >= toT)
                return;
            if (pg.crew && rosterFateAt(pl.sats[0], tl) >= 4)
                return;
            const int d = (int)((tl - fromT) / kDay);
            if (d < 0 || d >= nDays)
                return;
            allDay[(size_t)d] += 1.0f;
            const bool early = dp && !dp->earlyVehicles.empty() && tl < dp->earlyUntilT;
            if (ssOnly && !early)
                ssDay[(size_t)d] += 1.0f;
            // The calendar year (J2000 s -> year, 365.2425-day years from 2000-01-01).
            const int year = 2000 + (int)std::floor((tl + 43200.0) / (365.2425 * kDay));
            byProgYear[pg.name][year] += 1;
            (k == kFillK ? fillByProg : refreshByProg)[pg.name] += 1;
        });
    }
    nlohmann::json progs = nlohmann::json::object();
    for (const auto &kv : byProgYear)
    {
        nlohmann::json y = nlohmann::json::object();
        int tot = 0;
        for (const auto &yv : kv.second)
        {
            y[std::to_string(yv.first)] = yv.second;
            tot += yv.second;
        }
        progs[kv.first] = {{"by_year", y}, {"total", tot}, {"first_fills", fillByProg[kv.first]},
                           {"refreshes", refreshByProg[kv.first]}};
    }
    j["programs"] = progs;
    // Rates: per-step means (stepDays) and the busiest single day.
    auto peak = [&](const std::vector<float> &v, double &peakDay) {
        float mx = 0.0f;
        for (size_t d = 0; d < v.size(); ++d)
            if (v[d] > mx)
            {
                mx = v[d];
                peakDay = fromT + (double)d * kDay;
            }
        return mx;
    };
    double pAll = 0.0, pSs = 0.0;
    j["peak_launches_per_day"] = peak(allDay, pAll);
    j["peak_day"] = fmtUtc(pAll);
    j["peak_starship_per_day"] = peak(ssDay, pSs);
    j["peak_starship_day"] = fmtUtc(pSs);
    nlohmann::json series = nlohmann::json::array();
    const int step = std::max(1, (int)std::lround(stepDays));
    for (int d0 = 0; d0 < nDays; d0 += step)
    {
        double a = 0.0, s = 0.0;
        int n = 0;
        for (int d = d0; d < std::min(nDays, d0 + step); ++d, ++n)
        {
            a += allDay[(size_t)d];
            s += ssDay[(size_t)d];
        }
        series.push_back({{"from", fmtUtc(fromT + d0 * kDay)}, {"launches_per_day", a / std::max(n, 1)},
                          {"starship_per_day", s / std::max(n, 1)}});
    }
    j["series"] = series;
    return j;
}

// Same-pad pressure in a window (harness `launch padstats`): the listed launches (built: deconflicted), per site, the
// pairs on one pad within 90 min, the busiest pad-day.
nlohmann::json SatelliteSim::launchPadStatsJson(double hours) const
{
    const nlohmann::json list = launchListJson(hours);
    nlohmann::json j;
    std::map<std::string, std::vector<double>> bySite;
    for (const nlohmann::json &o : list)
        bySite[o["site"].get<std::string>()].push_back(o["in_hours"].get<double>());
    int pairs90 = 0, maxPadDay = 0;
    nlohmann::json sites = nlohmann::json::object();
    for (auto &kv : bySite)
    {
        std::sort(kv.second.begin(), kv.second.end());
        int p = 0;
        for (size_t a = 0; a < kv.second.size(); ++a)
            for (size_t b = a + 1; b < kv.second.size() && kv.second[b] - kv.second[a] < 1.5; ++b)
                ++p;
        pairs90 += p;
        // The busiest 24 h on this pad.
        int best = 0;
        for (size_t a = 0, b = 0; a < kv.second.size(); ++a)
        {
            while (b < kv.second.size() && kv.second[b] - kv.second[a] < 24.0)
                ++b;
            best = std::max(best, (int)(b - a));
        }
        maxPadDay = std::max(maxPadDay, best);
        sites[kv.first] = {{"launches", kv.second.size()}, {"pairs_within_90min", p}, {"busiest_24h", best}};
    }
    j["hours"] = hours;
    j["launches"] = list.size();
    j["launches_per_day"] = (double)list.size() / std::max(hours / 24.0, 1e-9);
    j["same_pad_pairs_within_90min"] = pairs90;
    j["same_pad_pairs_per_week"] = pairs90 * 168.0 / std::max(hours, 1.0);
    j["busiest_pad_24h"] = maxPadDay;
    j["sites"] = sites;
    return j;
}

// ── Mission-operations trace (2026-10-20, harness `launch trace` / `launch formation` / `launch goto`) ─────────────
// tools/harness/mission_ops.py drives these: for every (vehicle, program) pair in data/launches.json it finds a real
// launch after a date and traces it through its phases, numerically, from the launch model itself (no frames): the
// pad, the climb, max-Q, staging, the booster's recovery, the fairing, insertion into the parking orbit, the coast,
// the deployment (each member's release, separation, unfolding), the train's raising and phasing, the arrival in the
// slots, the retirees' deorbit, the upper stage's deorbit and entry (or a returning ship's entry and catch), and a
// crew flight's spacecraft (phasing, approach holds, docking, the stay, the return: LCrew). The thresholds live in the
// Python (one table, traceable); this side measures.
namespace
{
const char *landKindName(int k)
{
    switch (k)
    {
    case kLandTower: return "tower";
    case kLandLz: return "landing_zone";
    case kLandShip: return "ship";
    case kLandGround: return "ground";
    default: return "expendable";
    }
}
const char *stackLayoutName(int l)
{
    switch (l)
    {
    case launch::Stack::Pez: return "pez";
    case launch::Stack::Flat: return "flat";
    case launch::Stack::Column: return "column";
    default: return "dispenser_point";
    }
}
// Proper acceleration (non-gravitational, m/s^2) of a path P(t) at t from a central second difference, ECI.
template <class F> double properAccel(F &&pos, double t, double h, bool &ok)
{
    glm::dvec3 a, b, c;
    ok = pos(t - h, a) && pos(t, b) && pos(t + h, c);
    if (!ok)
        return 0.0;
    const glm::dvec3 acc = (a - 2.0 * b + c) / (h * h);
    const double r = glm::length(b);
    return glm::length(acc + launch::kGM * b / (r * r * r));
}
} // namespace

// The batch with this key (a pure function of it), built if the cache does not hold it. nullptr: no such plane / no
// launch data. Returned as void* (LBatch is local to this file).
const void *SatelliteSim::launchEnsureBatch(uint64_t key)
{
    if (!launchState)
        return nullptr;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return nullptr;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    auto it = L.batches.find(key);
    if (it == L.batches.end())
    {
        int plane = 0, b = 0;
        int64_t k = 0;
        batchKeyDecode(key, plane, k, b);
        if (plane < 0 || plane >= (int)L.planes.size())
            return nullptr;
        LBatch nb;
        nb.plane = plane;
        nb.k = k;
        nb.b = b;
        const LPlane &pl = L.planes[plane];
        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
        nb.touched = L.frame;
        it = L.batches.emplace(key, std::move(nb)).first;
    }
    return &it->second;
}

nlohmann::json SatelliteSim::launchTraceBatchJson(uint64_t key)
{
    nlohmann::json j;
    if (!launchState)
        return j;
    LaunchState &L = *launchState;
    auto it = launchEnsureBatch(key);
    if (it == nullptr)
        return j;
    const LBatch &B = *static_cast<const LBatch *>(it);
    j["key"] = std::to_string(key);
    j["valid"] = B.valid;
    if (!B.valid)
    {
        j["why"] = B.why;
        return j;
    }
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Program &pg = L.cat.programs[B.prog];
    const launch::Site &st = L.cat.sites[B.site];
    const LPlane &pl = L.planes[B.plane];
    const bool earlyVeh = std::find(pg.vehicles.begin(), pg.vehicles.end(), B.veh) == pg.vehicles.end();
    const int layout = earlyVeh ? 0 : pg.stack.layout;
    const Track &btr = LaunchImpl::boosterTr(L, B);
    j["vehicle"] = vh.id;
    j["vehicle_name"] = vh.name;
    j["program"] = pg.name;
    j["constellation"] = constellations[pl.constIdx].name;
    j["site"] = st.id;
    j["site_name"] = st.name;
    j["crew"] = pg.crew;
    j["cargo"] = pg.cargo;
    j["spacecraft"] = pg.spacecraft;
    j["first_fill"] = B.first;
    j["early_vehicle"] = earlyVeh;
    j["layout"] = pg.crew ? "crew" : stackLayoutName(layout);
    j["members"] = B.mem.size();
    j["t_launch"] = B.tL;
    j["utc_launch"] = fmtUtcReadable(B.tL);
    j["azimuth_deg"] = B.azDeg;
    j["sun_elev_pad_deg"] = sunElevAtPad(st, B.tL);
    j["recovery"] = landKindName(B.landKind);
    j["park_km"] = pg.parkKm;
    j["cluster_size"] = std::max(1, constellations[pl.constIdx].clusterSize);
    nlohmann::json ph = nlohmann::json::object();
    auto phase = [&](const char *name, double t) -> nlohmann::json & {
        nlohmann::json &p = ph[name];
        p["t"] = t;
        p["t_plus_s"] = t - B.tL;
        p["utc"] = fmtUtcReadable(t);
        return p;
    };
    auto alt = [](const glm::dvec3 &P) { return glm::length(P) - kR; };
    // Velocity (ECI, central difference h).
    auto velOf = [&](int stage, double t, double h, glm::dvec3 &V) {
        glm::dvec3 a, b;
        if (!LaunchImpl::rocketPos(L, B, stage, t + h - B.tL, a) || !LaunchImpl::rocketPos(L, B, stage, t - h - B.tL, b))
            return false;
        V = (a - b) / (2.0 * h);
        return true;
    };
    // Largest proper acceleration and nose turn rate of a stage over [t0, t1).
    auto scanStage = [&](int stage, double t0, double t1, double dt, double &maxAcc, double &atAcc, double &maxNose,
                         double &atNose) {
        maxAcc = maxNose = 0.0;
        atAcc = atNose = t0;
        glm::dvec3 prev(0.0);
        auto pos = [&](double t, glm::dvec3 &P) { return LaunchImpl::rocketPos(L, B, stage, t - B.tL, P); };
        for (double t = t0; t < t1; t += dt)
        {
            bool ok = false;
            const double a = properAccel(pos, t, 0.5, ok);
            if (ok && a > maxAcc)
            {
                maxAcc = a;
                atAcc = t;
            }
            const glm::dvec3 n = LaunchImpl::stageNose(L, B, stage, t - B.tL);
            if (glm::length(n) > 0.5 && glm::length(prev) > 0.5)
            {
                const double r = glm::degrees(std::acos(glm::clamp(glm::dot(n, prev), -1.0, 1.0))) / dt;
                if (r > maxNose)
                {
                    maxNose = r;
                    atNose = t;
                }
            }
            prev = n;
        }
    };
    // ── Pad and lift-off ──
    {
        nlohmann::json &p = phase("pad_liftoff", B.tL);
        glm::dvec3 P0;
        LaunchImpl::rocketPos(L, B, 0, 0.0, P0);
        const glm::dvec3 E0 = eciToEcef(P0, B.tL);
        const glm::dvec3 g = st.ecef;
        const glm::dvec3 d = E0 - kR * g;
        p["pad_offset_m"] = glm::length(d - glm::dot(d, g) * g);
        p["height_above_sea_m"] = glm::dot(d, g);
        p["pad_base_m"] = B.padBase;
        p["mount_m"] = vh.mountM;
        p["pad_shifts"] = B.shifts;
        glm::dvec3 P10;
        LaunchImpl::rocketPos(L, B, 0, 10.0, P10);
        p["rise_10s_m"] = alt(P10) - alt(P0);
    }
    // ── The climb to MECO (the stack) ──
    {
        nlohmann::json &p = phase("ascent", B.tL + 1.0);
        double ma, ta, mn, tn;
        scanStage(0, B.tL + 1.0, B.tL + vh.t1 - 1.0, 0.5, ma, ta, mn, tn);
        p["max_proper_accel_m_s2"] = ma;
        p["max_accel_at_t_plus_s"] = ta - B.tL;
        p["nose_max_deg_s"] = mn;
        p["nose_max_at_t_plus_s"] = tn - B.tL;
        double hPrev = -1e9, drop = 0.0;
        for (double tau = 0.0; tau < vh.t1; tau += 1.0)
        {
            glm::dvec3 P;
            if (!LaunchImpl::rocketPos(L, B, 0, tau, P))
                continue;
            drop = std::max(drop, hPrev - alt(P));
            hPrev = alt(P);
        }
        p["max_alt_drop_m"] = drop;
    }
    // ── Max-Q (the climb's largest dynamic pressure, on THIS launch's booster track) ──
    {
        double qMax = 0.0, tq = -1.0, hq = 0.0;
        for (size_t i = 0; i < btr.p.size() && (double)i * btr.dt < vh.t1; ++i)
        {
            const double q = 0.5 * launch::airDensity(btr.p[i].y) * glm::dot(btr.v[i], btr.v[i]);
            if (q > qMax)
            {
                qMax = q;
                tq = (double)i * btr.dt;
                hq = btr.p[i].y;
            }
        }
        nlohmann::json &p = phase("max_q", B.tL + std::max(tq, 0.0));
        p["q_kpa"] = qMax / 1000.0;
        p["alt_km"] = hq / 1000.0;
    }
    // ── MECO and staging ──
    {
        nlohmann::json &p = phase("meco_staging", B.tL + vh.t1);
        glm::dvec3 Pb, Pu;
        LaunchImpl::rocketPos(L, B, 0, vh.t1, Pb);
        LaunchImpl::rocketPos(L, B, 1, vh.t1, Pu);
        p["alt_km"] = alt(Pb) / 1000.0;
        p["alt_spec_km"] = vh.h1 / 1000.0;
        const glm::dvec2 bp = btr.at(vh.t1);
        p["downrange_km"] = bp.x / 1000.0;
        p["downrange_spec_km"] = vh.x1 / 1000.0;
        p["stage_gap_m"] = glm::length(Pb - Pu);
        glm::dvec3 a0, a1, b0, b1;
        LaunchImpl::rocketPos(L, B, 0, vh.t1 - 1.0, a0);
        LaunchImpl::rocketPos(L, B, 0, vh.t1, a1);
        LaunchImpl::rocketPos(L, B, 1, vh.t1, b0);
        LaunchImpl::rocketPos(L, B, 1, vh.t1 + 1.0, b1);
        p["speed_m_s"] = glm::length(a1 - a0);
        p["velocity_jump_m_s"] = glm::length((b1 - b0) - (a1 - a0));
        {
            // (diagnostics) ECI velocities by 0.2-s differences: the stack before, the booster and the upper stage after
            glm::dvec3 q0, q1;
            auto vAt = [&](int stg, double tau) {
                LaunchImpl::rocketPos(L, B, stg, tau - 0.1, q0);
                LaunchImpl::rocketPos(L, B, stg, tau + 0.1, q1);
                return (q1 - q0) / 0.2;
            };
            const glm::dvec3 vS = vAt(0, vh.t1 - 0.3), vB = vAt(0, vh.t1 + 0.3), vU = vAt(1, vh.t1 + 0.3);
            p["v_stack_before_m_s"] = glm::length(vS);
            p["v_booster_after_m_s"] = glm::length(vB);
            p["v_upper_after_m_s"] = glm::length(vU);
            p["dv_booster_m_s"] = glm::length(vB - vS);
            p["dv_upper_m_s"] = glm::length(vU - vS);
        }
    }
    // ── The booster after staging: its recovery ──
    {
        nlohmann::json &p = phase("booster_recovery", B.tL + btr.tEnd);
        p["kind"] = landKindName(B.landKind);
        p["rec"] = B.rec;
        p["landed"] = btr.landed;
        p["track_end_t_plus_s"] = btr.tEnd;
        p["payload_kg"] = B.payloadKg;
        double ma, ta, mn, tn;
        scanStage(0, B.tL + vh.t1 + 1.0, B.tL + btr.tEnd - 0.5, 0.5, ma, ta, mn, tn);
        p["max_proper_accel_m_s2"] = ma;
        p["max_accel_at_t_plus_s"] = ta - B.tL;
        p["nose_max_deg_s"] = mn;
        p["nose_max_at_t_plus_s"] = tn - B.tL;
        nlohmann::json burns = nlohmann::json::array();
        uint8_t prev = launch::BURN_NONE;
        for (size_t i = 0; i < btr.burn.size(); ++i)
        {
            const double tau = (double)i * btr.dt;
            if (tau > vh.t1 + 0.5 && btr.burn[i] != prev && btr.burn[i] != launch::BURN_NONE)
                burns.push_back({{"t_plus_s", tau}, {"burn", msBurnName(btr.burn[i])}});
            prev = btr.burn[i];
        }
        p["burns"] = burns;
        glm::dvec3 Pend;
        if (LaunchImpl::rocketPos(L, B, 0, btr.tEnd, Pend))
            p["end_alt_m"] = alt(Pend);
        const glm::dvec2 pe = btr.p.empty() ? glm::dvec2(0.0) : btr.p.back();
        p["end_downrange_km"] = pe.x / 1000.0;
        {
            const double a = pe.x / kR;
            const glm::dvec3 gg = std::cos(a) * B.G0 + std::sin(a) * B.Eaz;
            const double la = glm::degrees(std::asin(glm::clamp(gg.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(gg.y, gg.x));
            p["end_lat"] = la;
            p["end_lon"] = lo;
            p["end_shore_dist_m"] = shoreDistCpu(la, lo); // > 0 at sea
            p["end_ground_m"] = demElevCpu(la, lo);
        }
        if (B.landKind != kLandNone && btr.landed)
        {
            glm::dvec3 E, P0, P1;
            double tT = 0.0;
            LaunchImpl::boosterLandingAt(L, B, B.tL + btr.tEnd, E, tT);
            if (LaunchImpl::rocketPos(L, B, 0, btr.tEnd, P0) && LaunchImpl::rocketPos(L, B, 0, btr.tEnd - 1.0, P1))
            {
                const glm::dvec3 E0 = eciToEcef(P0, tT), E1 = eciToEcef(P1, tT - 1.0);
                const glm::dvec3 up = glm::normalize(E), d = E0 - E;
                p["touchdown_off_m"] = glm::length(d - glm::dot(d, up) * up);
                p["touchdown_above_m"] = glm::dot(d, up) - vh.legM; // (the legs' feet; the aft end is legM above)
                p["leg_m"] = vh.legM;
                p["touchdown_speed_m_s"] = glm::length(E0 - E1);
                double tilt = 0.0;
                for (double tau = btr.tEnd - 10.0; tau <= btr.tEnd; tau += 1.0)
                {
                    const glm::dvec3 n = LaunchImpl::stageNose(L, B, 0, tau);
                    glm::dvec3 Pp;
                    if (glm::length(n) > 0.5 && LaunchImpl::rocketPos(L, B, 0, tau, Pp))
                        tilt = std::max(tilt, glm::degrees(std::acos(glm::clamp(glm::dot(n, glm::normalize(Pp)), -1.0, 1.0))));
                }
                p["landing_tilt_deg_max_last10s"] = tilt;
            }
            if (B.landKind == kLandShip)
            {
                p["ship"] = vh.ships[(size_t)B.ship].name;
                double ride = 0.0;
                for (double t = tT + 60.0; t < B.tShipGone; t += 1800.0)
                {
                    glm::dvec3 P, Ed;
                    double tt;
                    if (!LaunchImpl::rocketPos(L, B, 0, t - B.tL, P))
                        continue;
                    LaunchImpl::boosterLandingAt(L, B, t, Ed, tt);
                    ride = std::max(ride, glm::length(eciToEcef(P, t) - Ed - vh.legM * glm::normalize(Ed)));
                }
                p["ride_home_off_deck_m"] = ride;
            }
        }
    }
    // ── The upper stage to orbit ──
    {
        nlohmann::json &p = phase("upper_ascent", B.tL + vh.t1);
        double ma, ta, mn, tn;
        scanStage(1, B.tL + vh.t1 + 1.0, B.tL + B.t2 - 1.0, 0.5, ma, ta, mn, tn);
        p["max_proper_accel_m_s2"] = ma;
        p["max_accel_at_t_plus_s"] = ta - B.tL;
        p["nose_max_deg_s"] = mn;
        p["nose_max_at_t_plus_s"] = tn - B.tL;
    }
    // ── Fairing separation ──
    if (vh.fairingSepKm > 0.0)
    {
        nlohmann::json &p = phase("fairing_sep", B.tFair >= 0.0 ? B.tFair : B.tL);
        p["present"] = B.tFair >= 0.0;
        p["spec_km"] = vh.fairingSepKm;
        if (B.tFair >= 0.0)
        {
            glm::dvec3 P;
            LaunchImpl::rocketPos(L, B, 1, B.tFair - B.tL, P);
            p["alt_km"] = alt(P) / 1000.0;
            p["before_seco"] = B.tFair < B.tL + B.t2;
            LBatch Bc = B;
            Bc.fairBuilt = false;
            LaunchImpl::ensureFairing(L, Bc, vh.fairingCenter);
            double gap10 = -1.0, endAlt = -1.0, dur = 0.0;
            for (int h = 0; h < 2; ++h)
            {
                glm::dvec3 fp, fv, Ps;
                glm::dmat3 R;
                if (LaunchImpl::fairingAt(Bc, h, B.tFair + 10.0, fp, fv, R) && LaunchImpl::rocketPos(L, B, 1, B.tFair + 10.0 - B.tL, Ps))
                    gap10 = std::max(gap10, glm::length(fp - Ps));
                dur = std::max(dur, Bc.fair[h].tEnd - B.tFair);
                if (!Bc.fair[h].c.empty())
                    endAlt = std::max(endAlt, alt(Bc.fair[h].c.back()));
            }
            p["half_gap_after_10s_m"] = gap10;
            p["halves_fall_s"] = dur;
            p["halves_end_alt_km"] = endAlt / 1000.0;
        }
    }
    else
    {
        nlohmann::json &p = phase("fairing_sep", B.tL);
        p["present"] = false;
        p["not_applicable"] = true;
    }
    // ── SECO: the parking orbit ──
    {
        const double t = B.tL + B.t2 + 2.0;
        nlohmann::json &p = phase("insertion", B.tL + B.t2);
        glm::dvec3 P, V;
        LaunchImpl::rocketPos(L, B, 1, t - B.tL, P);
        velOf(1, t, 1.0, V);
        const double r = glm::length(P);
        p["alt_km"] = alt(P) / 1000.0;
        p["park_km"] = pg.parkKm;
        p["speed_m_s"] = glm::length(V);
        p["circular_speed_m_s"] = std::sqrt(launch::kGM / r);
        p["flight_path_angle_deg"] = glm::degrees(std::asin(glm::clamp(glm::dot(V, P) / (glm::length(V) * r), -1.0, 1.0)));
        const glm::dvec3 n = glm::normalize(glm::cross(P, V));
        p["inclination_deg"] = glm::degrees(std::acos(glm::clamp(n.z, -1.0, 1.0)));
        p["plane_inclination_deg"] = glm::degrees((double)pl.inclRad);
        const glm::dvec3 Np = LaunchImpl::planeNormal(*this, pl, B.b, std::max(1, (int)B.mem.size()), t);
        p["plane_angle_deg"] = glm::degrees(std::acos(glm::clamp(glm::dot(n, Np), -1.0, 1.0)));
        double dv = 0.0;
        for (double tau = B.t2 - 5.0; tau < B.t2 + 5.0; tau += 1.0)
        {
            glm::dvec3 a, b, c;
            if (LaunchImpl::rocketPos(L, B, 1, tau - 1.0, a) && LaunchImpl::rocketPos(L, B, 1, tau, b) &&
                LaunchImpl::rocketPos(L, B, 1, tau + 1.0, c))
            {
                const glm::dvec3 acc = a - 2.0 * b + c;
                const double rr = glm::length(b);
                dv = std::max(dv, glm::length(acc + launch::kGM * b / (rr * rr * rr)));
            }
        }
        p["seco_max_proper_accel_m_s2"] = dv;
        {
            glm::dvec3 q0, q1, q2, q3;
            LaunchImpl::rocketPos(L, B, 1, B.t2 - 0.4, q0);
            LaunchImpl::rocketPos(L, B, 1, B.t2 - 0.2, q1);
            LaunchImpl::rocketPos(L, B, 1, B.t2 + 0.2, q2);
            LaunchImpl::rocketPos(L, B, 1, B.t2 + 0.4, q3);
            const glm::dvec3 vA = (q1 - q0) / 0.2, vB = (q3 - q2) / 0.2;
            p["v_before_seco_m_s"] = glm::length(vA);
            p["v_after_seco_m_s"] = glm::length(vB);
            p["dv_seco_m_s"] = glm::length(vB - vA);
            p["r_jump_seco_m"] = glm::length(q2) - glm::length(q1);
        }
    }
    // ── Coast to deployment ──
    {
        nlohmann::json &p = phase("coast", B.tL + B.t2);
        double hMin = 1e30, hMax = -1e30;
        for (double t = B.tL + B.t2 + 1.0; t < B.tD; t += 10.0)
        {
            glm::dvec3 P;
            if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, P))
                continue;
            hMin = std::min(hMin, alt(P));
            hMax = std::max(hMax, alt(P));
        }
        p["alt_min_km"] = hMin < 1e29 ? hMin / 1000.0 : 0.0;
        p["alt_max_km"] = hMax > -1e29 ? hMax / 1000.0 : 0.0;
        p["duration_s"] = B.tD - (B.tL + B.t2);
    }
    // ── Deployment, raising, arrival, retirees ──
    if (!B.mem.empty())
    {
        const bool big = vh.upperMassKg > 50000.0;
        const double noseOff = big ? 31.0 : 9.0, sideOff = big ? 5.3 : 1.6;
        const double sp = !earlyVeh && pg.stack.sepMps >= 0.0 ? pg.stack.sepMps : vh.sepSpeedMps;
        nlohmann::json &p = phase("deployment", B.tD);
        p["interval_s"] = B.deployInterval;
        p["sep_speed_spec_m_s"] = sp;
        double relErr = 0.0, sepMin = 1e30, sepMax = 0.0, ivMin = 1e30, ivMax = 0.0, awayMin = 1e30;
        double unfoldBefore = 1e30, unfoldDur = 0.0;
        int stacked = 0, nUnfold = 0;
        for (size_t i = 0; i < B.mem.size(); ++i)
        {
            const LMember &mb = B.mem[i];
            stacked += mb.stacked;
            if (i > 0)
            {
                ivMin = std::min(ivMin, mb.tRel - B.mem[i - 1].tRel);
                ivMax = std::max(ivMax, mb.tRel - B.mem[i - 1].tRel);
            }
            glm::dvec3 P1, P0, r, v;
            if (!LaunchImpl::rocketPos(L, B, 1, mb.tRel - B.tL, P1) || !LaunchImpl::rocketPos(L, B, 1, mb.tRel - B.tL - 1.0, P0) ||
                !LaunchImpl::memberState(*this, B, mb, 0, mb.tRel, r, v))
                continue;
            const glm::dvec3 nose = glm::normalize(P1 - P0), up = glm::normalize(P1);
            const glm::dvec3 disp = mb.stacked ? P1 + LaunchImpl::upperFrame(L, B, mb.tRel - B.tL) * mb.slotO
                                               : P1 + noseOff * nose + sideOff * up;
            relErr = std::max(relErr, glm::length(r - disp));
            glm::dvec3 ra, rb, va, vb, Sa, Sb;
            if (LaunchImpl::memberState(*this, B, mb, 0, mb.tRel + 3.0, ra, va) &&
                LaunchImpl::memberState(*this, B, mb, 0, mb.tRel + 1.0, rb, vb) &&
                LaunchImpl::rocketPos(L, B, 1, mb.tRel + 3.0 - B.tL, Sa) && LaunchImpl::rocketPos(L, B, 1, mb.tRel + 1.0 - B.tL, Sb))
            {
                const double rel = glm::length(((ra - rb) - (Sa - Sb)) / 2.0);
                sepMin = std::min(sepMin, rel);
                sepMax = std::max(sepMax, rel);
                glm::dvec3 r60, v60, S60;
                if (LaunchImpl::memberState(*this, B, mb, 0, mb.tRel + 60.0, r60, v60) &&
                    LaunchImpl::rocketPos(L, B, 1, mb.tRel + 60.0 - B.tL, S60))
                    awayMin = std::min(awayMin, glm::length(r60 - S60) - glm::length(rb - Sb));
            }
            double u0, ud;
            LaunchImpl::memberUnfold(*this, mb, 0, u0, ud);
            if (ud > 0.0)
            {
                ++nUnfold;
                unfoldBefore = std::min(unfoldBefore, u0 - mb.tRel);
                unfoldDur = std::max(unfoldDur, ud);
            }
        }
        p["stacked_members"] = stacked;
        p["release_err_m_max"] = relErr;
        p["sep_speed_m_s"] = {sepMin < 1e29 ? sepMin : 0.0, sepMax};
        p["interval_actual_s"] = {ivMin < 1e29 ? ivMin : 0.0, ivMax};
        p["recede_60s_m_min"] = awayMin < 1e29 ? awayMin : 0.0;
        p["unfolding_members"] = nUnfold;
        p["unfold_start_after_release_s_min"] = nUnfold ? unfoldBefore : 0.0;
        p["unfold_dur_s"] = unfoldDur;
        // Members never pass through each other or the stage in their free flight (kFreeFlightS; min pairwise distance, 30 s).
        {
            // (A stack's satellites start as close as they were stacked: each pair is judged against its distance when
            // the second of the two leaves, gap_ratio = closest later / that.)
            const size_t nm = std::min<size_t>(B.mem.size(), 48);
            double dMin = 1e30, dStage = 1e30, ratio = 1e30, relMin = 1e30, d60 = 1e30, d60At = 0.0, d10 = 1e30;
            double dStageLate = 1e30; // from 3/4 of an orbit on: a member must not come back to the stage
            const double tOrbit = glm::two_pi<double>() / std::max(B.nPark, 1e-6);
            std::array<int, 2> d60Pair{-1, -1};
            std::vector<double> dRel(nm * nm, -1.0), dLow(nm * nm, 1e30);
            for (size_t a = 0; a < nm; ++a)
                for (size_t b = a + 1; b < nm; ++b)
                {
                    glm::dvec3 ra, rb, v;
                    if (LaunchImpl::memberState(*this, B, B.mem[a], 0, B.mem[b].tRel, ra, v) &&
                        LaunchImpl::memberState(*this, B, B.mem[b], 0, B.mem[b].tRel, rb, v))
                    {
                        dRel[a * nm + b] = glm::length(ra - rb);
                        relMin = std::min(relMin, dRel[a * nm + b]);
                    }
                }
            for (double t = B.mem[0].tRel + 5.0; t < B.mem[nm - 1].tRel + kFreeFlightS; t += (t < B.mem[nm - 1].tRel + 120.0 ? 2.0 : 30.0))
            {
                std::vector<glm::dvec3> rs(nm);
                std::vector<char> up(nm, 0);
                glm::dvec3 S;
                const bool sOk = LaunchImpl::rocketPos(L, B, 1, t - B.tL, S);
                for (size_t i = 0; i < nm; ++i)
                {
                    glm::dvec3 v;
                    if (B.mem[i].tRel + 1.0 <= t && LaunchImpl::memberState(*this, B, B.mem[i], 0, t, rs[i], v))
                    {
                        up[i] = 1;
                        if (sOk && B.mem[i].tRel + 30.0 <= t)
                            dStage = std::min(dStage, glm::length(rs[i] - S));
                        if (sOk && B.mem[i].tRel + 0.75 * tOrbit <= t)
                            dStageLate = std::min(dStageLate, glm::length(rs[i] - S));
                    }
                }
                for (size_t a = 0; a < nm; ++a)
                    for (size_t b = a + 1; b < nm; ++b)
                        if (up[a] && up[b])
                        {
                            const double d = glm::length(rs[a] - rs[b]);
                            dMin = std::min(dMin, d);
                            dLow[a * nm + b] = std::min(dLow[a * nm + b], d);
                            if (t > B.mem[b].tRel + 60.0 && t < B.mem[b].tRel + 600.0)
                                d10 = std::min(d10, d);
                            // (free flight: from a minute after the later one leaves until the earlier one's own
                            // propulsion takes over, kFreeFlightS after its release)
                            if (t > B.mem[b].tRel + 60.0 && t < B.mem[a].tRel + kFreeFlightS && d < d60)
                            {
                                d60 = d;
                                d60At = t - B.mem[b].tRel;
                                d60Pair = {(int)a, (int)b};
                            }
                        }
            }
            for (size_t q = 0; q < dRel.size(); ++q)
                if (dRel[q] > 0.0 && dLow[q] < 1e29)
                    ratio = std::min(ratio, dLow[q] / std::max(dRel[q], 0.05));
            p["min_member_gap_2h_m"] = dMin < 1e29 ? dMin : -1.0;
            p["min_member_gap_at_release_m"] = relMin < 1e29 ? relMin : -1.0;
            p["member_gap_ratio_min"] = ratio < 1e29 ? ratio : -1.0;
            p["min_member_gap_after_60s_m"] = d60 < 1e29 ? d60 : -1.0;
            p["min_member_gap_1_10min_m"] = d10 < 1e29 ? d10 : -1.0;
            p["min_member_gap_after_60s_at_s"] = d60At;
            p["min_member_gap_after_60s_pair"] = d60Pair;
            p["min_stage_gap_2h_m"] = dStage < 1e29 ? dStage : -1.0;
            p["min_stage_gap_after_orbit_m"] = dStageLate < 1e29 ? dStageLate : -1.0;
        }
        if (B.mem.size() > 1)
            phase("deployment_end", B.mem.back().tRel);
        // Raising: up to 12 members, from release to arrival.
        {
            double r0 = 1e300;
            for (const LMember &m : B.mem)
                r0 = std::min(r0, m.sRaise);
            nlohmann::json &q = phase("raising", r0);
            const size_t nm = B.mem.size(), step = std::max<size_t>(1, nm / 12);
            double acc = 0.0, accAt = 0.0, accLate = 0.0, hMin = 1e30, hMax = -1e30, stepMax = 0.0, descend = 0.0, spreadMax = 0.0;
            for (size_t i = 0; i < nm; i += step)
            {
                const LMember &mb = B.mem[i];
                auto pos = [&](double t, glm::dvec3 &r) {
                    glm::dvec3 v;
                    return LaunchImpl::memberState(*this, B, mb, 0, t, r, v);
                };
                double hPrev = -1.0;
                for (double t = mb.tRel + 2.0; t < B.tA - 2.0;)
                {
                    bool ok = false;
                    const double a = properAccel(pos, t, 1.0, ok);
                    if (ok)
                    {
                        if (a > acc)
                        {
                            acc = a;
                            accAt = t - mb.tRel;
                        }
                        if (t > mb.tRel + 2.5 * kDay)
                            accLate = std::max(accLate, a);
                        glm::dvec3 r, r1;
                        pos(t, r);
                        pos(t + 1.0, r1);
                        stepMax = std::max(stepMax, glm::length(r1 - r) - std::sqrt(launch::kGM / glm::length(r)));
                        hMin = std::min(hMin, alt(r));
                        hMax = std::max(hMax, alt(r));
                        if (t > mb.sRaise + 1.0 && t > mb.tRel + 2.5 * kDay && hPrev > 0.0)
                            descend = std::max(descend, hPrev - alt(r));
                        hPrev = alt(r);
                    }
                    t += (t < mb.tRel + 3.0 * 3600.0) ? 30.0 : 600.0;
                }
            }
            nlohmann::json spread = nlohmann::json::array();
            for (double f : {0.0, 0.05, 0.25, 0.5, 0.75, 0.95, 0.999})
            {
                const double t = B.tD + 600.0 + f * (B.tA - B.tD - 600.0);
                std::vector<glm::dvec3> rs;
                for (size_t i = 0; i < nm; ++i)
                {
                    glm::dvec3 r, v;
                    if (LaunchImpl::memberState(*this, B, B.mem[i], 0, t, r, v))
                        rs.push_back(r);
                }
                double s = 0.0;
                for (size_t a = 0; a < rs.size(); ++a)
                    for (size_t b = a + 1; b < rs.size(); ++b)
                        s = std::max(s, glm::length(rs[a] - rs[b]));
                spread.push_back({{"frac", f}, {"spread_km", s / 1000.0}});
                spreadMax = std::max(spreadMax, s);
            }
            q["max_proper_accel_m_s2"] = acc;
            q["max_accel_after_release_s"] = accAt;
            q["max_proper_accel_after_2_5d_m_s2"] = accLate;
            q["alt_min_km"] = hMin < 1e29 ? hMin / 1000.0 : 0.0;
            q["alt_max_km"] = hMax > -1e29 ? hMax / 1000.0 : 0.0;
            q["max_1s_step_over_circular_m"] = stepMax;
            q["descend_after_2_5d_m_max"] = descend;
            q["spread"] = spread;
            q["spread_max_km"] = spreadMax / 1000.0;
            double eps = 0.0;
            for (const LMember &m : B.mem)
                eps = std::max(eps, std::abs(m.eps));
            q["phasing_residual_rad_max"] = eps;
            q["raise_days"] = (B.tA - B.tD) / kDay;
            q["slot_alt_km"] = (B.mem[0].rSlot - kR) / 1000.0;
        }
        // Arrival.
        {
            nlohmann::json &q = phase("arrival", B.tA);
            double dMax = 0.0, vMax = 0.0;
            for (const LMember &mb : B.mem)
            {
                const SatOrbitElems e = rosterElemsOf(satOrbits[mb.sat]);
                const glm::dvec3 s0 = satOrbitStateAt(e, B.tA - 1e-3).posEci, s1 = satOrbitStateAt(e, B.tA - 1.0).posEci;
                glm::dvec3 r, v, r1, v1;
                if (LaunchImpl::memberState(*this, B, mb, 0, B.tA - 1e-3, r, v) &&
                    LaunchImpl::memberState(*this, B, mb, 0, B.tA - 1.0, r1, v1))
                {
                    dMax = std::max(dMax, glm::length(r - s0));
                    const glm::dvec3 dv = (r - r1) - (s0 - s1);
                    if (glm::length(dv) > vMax)
                    {
                        vMax = glm::length(dv);
                        const glm::dvec3 U = glm::normalize(s0), Cn = glm::normalize(glm::cross(s0, s0 - s1)), A = glm::cross(Cn, U);
                        q["vel_err_rac_m_s"] = {glm::dot(dv, U), glm::dot(dv, A), glm::dot(dv, Cn)};
                    }
                }
            }
            q["pos_err_m_max"] = dMax;
            q["vel_err_m_s_max"] = vMax;
        }
        // Retirees.
        if (!B.first)
        {
            nlohmann::json &q = phase("retiree_deorbit", B.tA);
            double dMax = 0.0, vMax = 0.0, endAlt = 0.0, rise = 0.0;
            for (const LMember &mb : B.mem)
            {
                const SatOrbitElems e = rosterElemsOf(satOrbits[mb.sat]);
                const glm::dvec3 s0 = satOrbitStateAt(e, B.tA).posEci, s1 = satOrbitStateAt(e, B.tA + 1.0).posEci;
                glm::dvec3 r, v, r1, v1;
                if (LaunchImpl::memberState(*this, B, mb, 1, B.tA, r, v) && LaunchImpl::memberState(*this, B, mb, 1, B.tA + 1.0, r1, v1))
                {
                    dMax = std::max(dMax, glm::length(r - s0));
                    vMax = std::max(vMax, glm::length((r1 - r) - (s1 - s0)));
                }
                if (LaunchImpl::memberState(*this, B, mb, 1, mb.tEnd - 1.0, r, v))
                    endAlt = std::max(endAlt, alt(r));
                double hp = 1e30;
                for (double t = B.tA; t < mb.tEnd; t += (mb.tEnd - B.tA) / 50.0)
                    if (LaunchImpl::memberState(*this, B, mb, 1, t, r, v))
                    {
                        rise = std::max(rise, alt(r) - hp);
                        hp = alt(r);
                    }
            }
            q["start_pos_err_m_max"] = dMax;
            q["start_vel_err_m_s_max"] = vMax;
            q["end_alt_km_max"] = endAlt / 1000.0;
            q["max_rise_m"] = rise;
            q["deorbit_days"] = B.ends.empty() ? 0.0 : (B.ends.back() - B.tA) / kDay;
            if (!B.ends.empty())
                phase("retiree_reentry", B.ends.front())["count"] = B.ends.size();
        }
    }
    // ── Crew: docking (the spacecraft's own flight since 2026-10-20: LCrew) ──
    if (pg.crew)
    {
        nlohmann::json &q = phase("docking", B.tA);
        q["modelled"] = B.crew.on; // (2026-10-20: the spacecraft's own flight, LCrew)
        q["hours_after_launch"] = (B.tA - B.tL) / 3600.0;
        if (B.crew.on)
        {
            const LCrew &C = B.crew;
            auto portDist = [&](double t) {
                glm::dvec3 Pc, Ps;
                glm::dmat3 R;
                if (!LaunchImpl::crewPos(C, t, Pc))
                    return -1.0;
                LaunchImpl::crewStation(C, t, Ps, R);
                return glm::length(Ps + R * C.portPos - Pc);
            };
            auto phaseAt = [&](double t) {
                glm::dvec3 Pc;
                int ph = 0;
                return LaunchImpl::crewPos(C, t, Pc, &ph) ? ph : -1;
            };
            glm::dvec3 Pst, Pc;
            if (LaunchImpl::rocketPos(L, B, 1, C.tSep - B.tL, Pst) && LaunchImpl::crewPos(C, C.tSep, Pc))
                q["sep_offset_m"] = glm::length(Pst + LaunchImpl::upperFrame(L, B, C.tSep - B.tL) *
                                                         glm::dvec3(0.0, 0.0, L.cat.vehicles[B.veh].spacecraftDockM) -
                                                 Pc);
            q["port"] = C.portName;
            q["dock_contact_m"] = portDist(C.tDock);
            if (C.berth)
            {
                // (2026-10-21) Berthing: the R-bar holds, the capture point and the arm's work.
                q["berth"] = true;
                q["hold250_range_m"] = portDist(C.hold[0] + 0.5 * LaunchImpl::kBerthLegs[1]);
                q["hold30_range_m"] = portDist(C.hold[1] + 0.5 * LaunchImpl::kBerthLegs[3]);
                q["capture_range_m"] = portDist(C.tCapArr + 0.5 * LaunchImpl::kBerthLegs[5]);
                q["capture_m"] = C.capM;
                q["capture_h_after_launch"] = (C.tGrapple - B.tL) / 3600.0;
                if (const launch::StationArm *arm = L.armOf(C.sat))
                {
                    std::vector<const LCrew *> vis;
                    for (const auto &kv2 : L.batches)
                        if (kv2.second.valid && kv2.second.crew.on && kv2.second.crew.berth && kv2.second.crew.sat == C.sat)
                            vis.push_back(&kv2.second.crew);
                    auto tipErr = [&](double tt) {
                        ArmPose ap;
                        LaunchImpl::stationArmPose(*arm, tt, vis, ap);
                        glm::dvec3 G, n;
                        return LaunchImpl::berthFixture(C, tt, G, n) ? glm::length(ap.G - G) : -1.0;
                    };
                    q["grip_tip_err_m"] = tipErr(C.tGrapple + 1.0);
                    q["berthed_tip_err_m"] = tipErr(C.tDock + 60.0);
                    if (C.abort == 0)
                        q["release_tip_err_m"] = tipErr(C.tRelease - 1.0);
                }
            }
            else
            {
            q["hold400_range_m"] = portDist(C.hold[0] + 0.5 * LaunchImpl::kCrewApproachLegs[C.fast ? 1 : 0][3]);
            q["hold200_range_m"] = portDist(C.hold[1] + 0.5 * LaunchImpl::kCrewApproachLegs[C.fast ? 1 : 0][5]);
            q["hold20_range_m"] = portDist(C.hold[2] + 0.5 * LaunchImpl::kCrewApproachLegs[C.fast ? 1 : 0][7]);
            }
            q["arrival_range_to_station_km"] = portDist(C.tArr) / 1000.0;
            { const int pa = phaseAt(C.tDock + 60.0), pb = phaseAt(0.5 * (C.tDock + C.tUndock));
              q["docked_after_60s"] = pa == CP_DOCKED || pa == CP_BERTHED;
              q["docked_mid_stay"] = pb == CP_DOCKED || pb == CP_BERTHED; }
            q["undocked_after"] = phaseAt(C.tUndock + 60.0) == (C.berth ? CP_UNBERTH : CP_BACKOUT);
            q["docked_days"] = (C.tUndock - C.tDock) / kDay;
            q["phasing_alt_km"] = (C.rP - kR) / 1000.0;
            q["lands"] = C.lands;
            q["splash_h_after_undock"] = (C.tSplash - C.tUndock) / 3600.0;
            // (2026-10-21) visiting vehicles: cargo ships, fast tracks, destructive re-entries, the station's loss
            q["spacecraft"] = pg.spacecraft;
            q["cargo"] = C.cargo;
            q["fast_track"] = C.fast;
            q["burns_up"] = C.burns;
            q["abort"] = C.abort;
            if (C.burns)
                q["reentry_h_after_undock"] = (C.tEI - C.tUndock) / 3600.0;
            if (C.runway)
            {
                // (2026-10-21c) the runway landing: the glide's start, the touchdown on the centreline and the rollout
                nlohmann::json rw;
                const glm::dvec3 Eg = LaunchImpl::crewRunwayEcef(C, C.tGlide, nullptr);
                const glm::dvec3 Ee = eciToEcef(LaunchImpl::crewEntryPos(C, C.tGlide), C.tGlide);
                rw["glide_start_jump_m"] = glm::length(Eg - Ee);
                rw["glide_km"] = glm::length(C.pTD - C.p0) / 1000.0;
                rw["glide_s"] = C.Tg;
                const glm::dvec3 Ta = LaunchImpl::crewRunwayEcef(C, C.tTD - 0.5, nullptr),
                                 Tb = LaunchImpl::crewRunwayEcef(C, C.tTD + 0.5, nullptr);
                rw["touchdown_speed_mps"] = glm::length(Tb - Ta);
                rw["touchdown_sink_mps"] = glm::length(Ta) - glm::length(Tb);
                const glm::dvec3 Td = LaunchImpl::crewRunwayEcef(C, C.tTD, nullptr);
                const glm::dvec3 u = glm::normalize(Td);
                const glm::dvec2 pp(kR * glm::dot(u, C.rwE) / glm::dot(u, C.rwC), kR * glm::dot(u, C.rwN) / glm::dot(u, C.rwC));
                rw["touchdown_off_centreline_m"] = std::abs(pp.x * C.rwDir.y - pp.y * C.rwDir.x);
                rw["touchdown_height_m"] = glm::length(Td) - kR - C.rwElev;
                rw["rollout_m"] = C.lRoll;
                rw["landing_heading_deg"] = std::fmod(glm::degrees(std::atan2(C.rwDir.x, C.rwDir.y)) + 360.0, 360.0);
                rw["deorbit_h_after_undock"] = (C.tDeorb - C.tUndock) / 3600.0;
                q["runway"] = rw;
            }
            if (C.fast)
                q["fast_hours"] = (C.tDock - B.tL) / 3600.0;
            // The port it docks at is free for its stay: every other visit to the station (crews and cargo, each plane's
            // batches arriving within a refresh period + 31 days of its docking) docked at the same port at the same time.
            {
                int conflicts = 0;
                const double tLo = C.tDock, tHi = C.tUndock;
                for (size_t pj = 0; pj < L.planes.size(); ++pj)
                {
                    const LPlane &pq = L.planes[pj];
                    const launch::Program &gq = L.cat.programs[(size_t)pq.prog];
                    if (!gq.crew || pq.sats.empty() || pq.sats[0] != C.sat)
                        continue;
                    const double lifeQ = progLifeS(gq, (double)launchLifeScale);
                    std::vector<uint64_t> keys;
                    LaunchImpl::forBatches(L, pq, lifeQ, tLo - lifeQ - 31.0 * kDay, tHi + 31.0 * kDay,
                                           [&](int64_t k, int b) { keys.push_back(batchKey((int)pj, k, b)); });
                    for (uint64_t kq : keys)
                    {
                        if (kq == key)
                            continue;
                        const LBatch *Q = missionBatch(kq);
                        if (!Q || !Q->valid || !Q->crew.on || Q->crew.port != C.port || Q->crew.abort == 1 ||
                            Q->crew.abort == 2)
                            continue;
                        if (Q->crew.tDock < tHi && Q->crew.tUndock > tLo)
                            ++conflicts;
                    }
                }
                q["port_conflicts"] = conflicts;
            }
            // Continuity: the largest 1-s second difference of its position and its fastest turn, around every seam.
            double acc = 0.0, turn = 0.0, burnAcc = 0.0;
            const double evs[] = {C.tSep, C.tB1, C.tB1e, C.tB2, C.tArr, C.tAxis, C.hold[0], C.hold[1], C.hold[2],
                                  C.tDock, C.tUndock, C.tDepEnd, C.tDeorb, C.tDeorb + C.deorbDur, C.tEI, C.tChute,
                                  C.tMains, C.tSplash};
            for (double e : evs)
                for (double d = -70.0; d <= 70.0; d += (std::abs(d) < 4.5 ? 1.0 : 2.0)) // (the burns span +-60 s)
                {
                    glm::dvec3 a, b0, c0;
                    if (!LaunchImpl::crewPos(C, e + d - 1.0, a) || !LaunchImpl::crewPos(C, e + d, b0) ||
                        !LaunchImpl::crewPos(C, e + d + 1.0, c0))
                        continue;
                    acc = std::max(acc, glm::length(a - 2.0 * b0 + c0));
                    int phx = 0;
                    glm::dvec3 tmp;
                    LaunchImpl::crewPos(C, e + d, tmp, &phx);
                    if (!(phx == CP_ENTRY || phx == CP_DROGUES || phx == CP_MAINS || phx == CP_DOWN || phx == CP_GLIDE ||
                          phx == CP_FINAL_GLIDE || phx == CP_ROLLOUT))
                        burnAcc = std::max(burnAcc, glm::length(a - 2.0 * b0 + c0));
                    const glm::dmat3 Dm = glm::transpose(LaunchImpl::crewAttitude(C, e + d)) * LaunchImpl::crewAttitude(C, e + d + 1.0);
                    turn = std::max(turn, glm::degrees(std::acos(std::clamp((Dm[0][0] + Dm[1][1] + Dm[2][2] - 1.0) * 0.5, -1.0, 1.0))));
                }
            q["accel_max_mps2"] = acc;
            q["burn_accel_max_mps2"] = burnAcc;
            q["turn_max_deg_s"] = turn;
        }
        if (!pl.sats.empty())
        {
            const SatOrbitState s = satOrbitStateAt(rosterElemsOf(satOrbits[pl.sats[0]]), B.tD);
            glm::dvec3 P, V;
            if (LaunchImpl::rocketPos(L, B, 1, B.tD - B.tL, P) && velOf(1, B.tD, 1.0, V))
            {
                const glm::dvec3 n = glm::normalize(glm::cross(P, V)), ns = glm::normalize(glm::cross(s.posEci, s.velEci));
                q["plane_angle_to_station_deg"] = glm::degrees(std::acos(glm::clamp(glm::dot(n, ns), -1.0, 1.0)));
                q["station_alt_km"] = alt(s.posEci) / 1000.0;
                q["capsule_alt_at_separation_km"] = alt(P) / 1000.0;
            }
        }
    }
    // ── The upper stage after deployment: deorbit, entry (a returning ship: its descent and catch) ──
    {
        nlohmann::json &q = phase("upper_deorbit", B.tDeorbBurn);
        q["after_deploy_h"] = (B.tDeorbBurn - B.tD) / 3600.0;
        q["burn_s"] = B.burnDur;
        glm::dvec3 V;
        const glm::dvec3 n = LaunchImpl::stageNose(L, B, 1, B.tDeorbBurn + 2.0 - B.tL);
        if (glm::length(n) > 0.5 && velOf(1, B.tDeorbBurn + 2.0, 1.0, V))
            q["nose_from_retrograde_deg"] = glm::degrees(std::acos(glm::clamp(glm::dot(n, -glm::normalize(V)), -1.0, 1.0)));
        double ma, ta, mn, tn;
        scanStage(1, B.tDeorbBurn - 300.0, B.tStageGone - 1.0, 0.5, ma, ta, mn, tn);
        q["nose_max_deg_s"] = mn;
        q["nose_max_at_t_plus_s"] = tn - B.tL;
        q["burn_max_proper_accel_m_s2"] = ma;
        nlohmann::json &e = phase("upper_entry", B.tStageGone);
        glm::dvec3 P;
        if (LaunchImpl::rocketPos(L, B, 1, B.tStageGone - 1.0 - B.tL, P))
            e["alt_km"] = alt(P) / 1000.0;
        e["returning"] = !vh.upperDeorbit;
        e["coast_to_entry_min"] = (B.tStageGone - B.tDeorbBurn) / 60.0;
        double jump = 0.0;
        glm::dvec3 Pp(0.0);
        bool have = false;
        for (double t = B.tStageGone - 20.0; t < B.tStageGone + (vh.upperDeorbit ? 0.0 : 20.0); t += 1.0)
        {
            glm::dvec3 Q;
            if (t < B.tStageGone)
            {
                if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, Q))
                    continue;
            }
            else
                Q = LaunchImpl::shipEntry(B, t - B.tStageGone, nullptr, nullptr);
            if (have)
                jump = std::max(jump, glm::length(Q - Pp));
            Pp = Q;
            have = true;
        }
        e["max_1s_step_m"] = jump;
        if (!vh.upperDeorbit && B.shipLands)
        {
            nlohmann::json &c = phase("ship_catch", B.tShipCatch);
            c["pad"] = B.landPad >= 0 ? L.cat.sites[L.pads[B.landPad].site].id : std::string("splashdown");
            c["descent_s"] = B.tShipCatch - B.tShipDesc;
            const glm::dvec3 a1 = eciToEcef(LaunchImpl::shipEntry(B, kShipEntryS, nullptr, nullptr), B.tShipDesc);
            glm::dvec3 b0;
            LaunchImpl::rocketPos(L, B, 1, B.tShipDesc - B.tL, b0);
            c["handover_m"] = glm::length(eciToEcef(b0, B.tShipDesc) - a1);
            if (B.landPad >= 0)
            {
                const LPad &pd = L.pads[B.landPad];
                glm::dvec3 Q, Q1;
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - B.tL, Q);
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - 1.0 - B.tL, Q1);
                const glm::dvec3 E = eciToEcef(Q, B.tShipCatch), E1 = eciToEcef(Q1, B.tShipCatch - 1.0);
                const glm::dvec3 d = E - (kR + pd.elevM) * pd.g;
                c["off_centre_m"] = glm::length(d - glm::dot(d, pd.g) * pd.g);
                c["above_ground_m"] = glm::dot(d, pd.g);
                c["speed_m_s"] = glm::length(E - E1);
            }
        }
    }
    j["phases"] = ph;
    std::vector<MissionMilestone> ms;
    missionBatchMilestones(B, ms);
    nlohmann::json ml = nlohmann::json::array();
    for (const MissionMilestone &m : ms)
        ml.push_back({{"t_plus_s", m.t - B.tL}, {"label", m.label}, {"who", (int)m.who}});
    j["milestones"] = ml;
    return j;
}

// The next valid launch of each (vehicle, program) pair after fromT, within `days` (searching 7, then 60, then
// `days`): the candidates first (vehicle and time, before deconfliction: cheap), then those of the pair built in time
// order until one flies.
nlohmann::json SatelliteSim::launchTraceJson(double fromT, double days, const std::string &vehFilter,
                                             const std::string &progFilter, bool trace)
{
    nlohmann::json out;
    if (!launchState || !launchesEnabled)
    {
        out["error"] = "launches are off";
        return out;
    }
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
    {
        out["error"] = "no launch data";
        return out;
    }
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale, launchDeployEra);
    LaunchImpl::crewSync(*this, L);
    auto lowerS = [](std::string s) {
        for (char &c : s)
            c = (char)std::tolower((unsigned char)c);
        return s;
    };
    const std::string vf = lowerS(vehFilter), pf = lowerS(progFilter);
    out["from_utc"] = fmtUtcReadable(fromT);
    out["from_t"] = fromT;
    out["deployment_era"] = launchDeployEra;
    nlohmann::json combos = nlohmann::json::array();
    for (size_t v = 0; v < L.cat.vehicles.size(); ++v)
    {
        const launch::Vehicle &vh = L.cat.vehicles[v];
        if (!vf.empty() && lowerS(vh.id) != vf && lowerS(vh.name).find(vf) == std::string::npos)
            continue;
        bool any = false;
        for (size_t p = 0; p < L.cat.programs.size(); ++p)
        {
            const launch::Program &pg = L.cat.programs[p];
            const bool flies = std::find(pg.vehicles.begin(), pg.vehicles.end(), (int)v) != pg.vehicles.end();
            bool early = false;
            for (const launch::Deployment &d : pg.deploy)
                if (std::find(d.earlyVehicles.begin(), d.earlyVehicles.end(), (int)v) != d.earlyVehicles.end())
                    early = true;
            if (!flies && !early)
                continue;
            any = true;
            if (!pf.empty() && lowerS(pg.name).find(pf) == std::string::npos)
                continue;
            nlohmann::json c;
            c["vehicle"] = vh.id;
            c["program"] = pg.name;
            c["early_only"] = !flies;
            c["layout"] = pg.crew ? "crew" : (flies ? stackLayoutName(pg.stack.layout) : "dispenser_point");
            uint64_t found = 0;
            double foundT = 0.0;
            int built = 0, cands = 0;
            std::string lastWhy;
            std::map<std::string, int> whys;
            for (double span : {7.0, 60.0, days})
            {
                if (found || span > days + 1e-9)
                    continue;
                struct Item
                {
                    double tL;
                    uint64_t key;
                };
                std::vector<Item> items;
                const double life = progLifeS(pg, (double)launchLifeScale);
                for (size_t pi = 0; pi < L.planes.size(); ++pi)
                {
                    const LPlane &pl = L.planes[pi];
                    if (pl.prog != (int)p)
                        continue;
                    const double lo = fromT + (pg.raiseDays[0] - 1.0 - pg.earlyDays()) * kDay,
                                 hi = fromT + span * kDay + (pg.raiseDays[1] + 3.0) * kDay;
                    LaunchImpl::forBatches(L, pl, life, lo, hi, [&](int64_t k, int b) {
                        const SatelliteSim::LaunchState::Cand cd = LaunchImpl::candidate(*this, L, (int)pi, k, b);
                        ++cands;
                        if (cd.veh == (int)v && cd.tL >= fromT - 2.0 * kDay && cd.tL <= fromT + span * kDay)
                            items.push_back({cd.tL, batchKey((int)pi, k, b)});
                    });
                }
                std::sort(items.begin(), items.end(),
                          [](const Item &a, const Item &b) { return a.tL < b.tL || (a.tL == b.tL && a.key < b.key); });
                for (const Item &itm : items)
                {
                    auto bi = L.batches.find(itm.key);
                    if (bi == L.batches.end())
                    {
                        int plane = 0, b = 0;
                        int64_t k = 0;
                        batchKeyDecode(itm.key, plane, k, b);
                        LBatch nb;
                        nb.plane = plane;
                        nb.k = k;
                        nb.b = b;
                        const LPlane &pl = L.planes[plane];
                        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
                        ++built;
                        bi = L.batches.emplace(itm.key, std::move(nb)).first;
                    }
                    LBatch &B = bi->second;
                    B.touched = std::max(B.touched, L.frame);
                    if (!B.valid)
                    {
                        lastWhy = B.why;
                        whys[B.why]++;
                        continue;
                    }
                    if (B.veh != (int)v || B.tL < fromT || B.tL > fromT + span * kDay)
                        continue;
                    found = itm.key;
                    foundT = B.tL;
                    break;
                }
            }
            c["candidates_scanned"] = cands;
            c["batches_built"] = built;
            c["invalid_reasons"] = whys;
            if (!found)
            {
                c["found"] = false;
                c["why"] = lastWhy.empty() ? "no launch of this pair in the window" : lastWhy;
            }
            else
            {
                c["found"] = true;
                c["key"] = std::to_string(found);
                c["utc_launch"] = fmtUtcReadable(foundT);
                c["in_days"] = (foundT - fromT) / kDay;
                if (trace)
                    c["trace"] = launchTraceBatchJson(found);
            }
            combos.push_back(c);
        }
        if (!any && pf.empty())
            combos.push_back({{"vehicle", vh.id}, {"program", ""}, {"found", false}, {"why", "no program flies this vehicle"}});
    }
    out["combos"] = combos;
    return out;
}

// The arrangement of a formation launch (Starmind: a ship's 8 = one ring cluster): each member's offset from the
// train's centroid vs its slot's offset from the cluster's centroid (both in the centroid's radial / along / cross
// axes), at moments from the release to the arrival. A member converging into its ring: its error falls to zero.
nlohmann::json SatelliteSim::launchFormationJson(uint64_t key, int samples, bool before)
{
    nlohmann::json j;
    if (!launchState)
        return j;
    LaunchState &L = *launchState;
    const LBatch *found = static_cast<const LBatch *>(launchEnsureBatch(key));
    if (!found || !found->valid || found->mem.empty())
    {
        j["error"] = "no such launch with a train";
        return j;
    }
    auto it = L.batches.find(key);
    // before=1: the same launch rebuilt with each member on its own raise schedule (pre-2026-10-20), for comparison.
    LBatch old;
    if (before)
    {
        old.plane = it->second.plane;
        old.k = it->second.k;
        old.b = it->second.b;
        LaunchImpl::gPerMemberRaise = true;
        const LPlane &pl0 = L.planes[old.plane];
        LaunchImpl::buildBatch(*this, L, old, pl0, progLifeS(L.cat.programs[pl0.prog], (double)launchLifeScale));
        LaunchImpl::gPerMemberRaise = false;
        j["before"] = true;
    }
    const LBatch &B = before ? old : it->second;
    const LPlane &pl = L.planes[B.plane];
    const int C = std::max(1, constellations[pl.constIdx].clusterSize);
    j["key"] = std::to_string(key);
    j["constellation"] = constellations[pl.constIdx].name;
    j["cluster_size"] = C;
    j["members"] = B.mem.size();
    j["t_release"] = B.mem.front().tRel;
    j["t_arrival"] = B.tA;
    j["raise_days"] = (B.tA - B.tD) / kDay;
    {
        nlohmann::json sr = nlohmann::json::array();
        for (const LMember &m : B.mem)
            sr.push_back({{"t_rel_after_deploy_s", m.tRel - B.tD}, {"raise_start_days", (m.sRaise - B.tD) / kDay},
                          {"ramp_days", m.tRamp / kDay}, {"eps_rad", m.eps}});
        j["schedule"] = sr;
    }
    std::vector<double> ts;
    const double t0 = B.mem.back().tRel + 1.0;
    for (double d : {0.0, 60.0, 600.0, 3600.0, 3.0 * 3600.0, 6.0 * 3600.0, 12.0 * 3600.0, kDay, 1.5 * kDay, 2.0 * kDay, 2.5 * kDay})
        if (t0 + d < B.tA)
            ts.push_back(t0 + d);
    const int extra = std::max(2, samples);
    for (int k = 1; k <= extra; ++k)
        ts.push_back(t0 + 2.5 * kDay + (B.tA - 1.0 - t0 - 2.5 * kDay) * (double)k / (double)extra);
    for (double d : {3600.0, 600.0, 60.0, 1.0})
        ts.push_back(B.tA - d);
    std::sort(ts.begin(), ts.end());
    nlohmann::json rows = nlohmann::json::array();
    double prevErr = -1.0, worstGrow = 0.0, errAfter2d = 0.0, spreadAfter2d = 0.0;
    for (double t : ts)
    {
        if (t < t0 || t >= B.tA)
            continue;
        double errMax = 0.0, spreadMax = 0.0, ringR = 0.0, shapeErr = 0.0;
        nlohmann::json first;
        for (size_t c0 = 0; c0 + (size_t)C <= B.mem.size(); c0 += (size_t)C)
        {
            std::vector<glm::dvec3> rm, rs;
            for (size_t i = c0; i < c0 + (size_t)C; ++i)
            {
                glm::dvec3 r, v;
                if (!LaunchImpl::memberState(*this, B, B.mem[i], 0, t, r, v))
                    break;
                rm.push_back(r);
                rs.push_back(satOrbitStateAt(rosterElemsOf(satOrbits[B.mem[i].sat]), t).posEci);
            }
            if (rm.size() != (size_t)C)
                continue;
            glm::dvec3 cm(0.0), cs(0.0);
            for (size_t i = 0; i < rm.size(); ++i)
            {
                cm += rm[i] / (double)C;
                cs += rs[i] / (double)C;
            }
            const SatOrbitState s0 = satOrbitStateAt(rosterElemsOf(satOrbits[B.mem[c0].sat]), t);
            const glm::dvec3 Nn = glm::normalize(glm::cross(s0.posEci, s0.velEci));
            const glm::dvec3 Um = glm::normalize(cm), Us = glm::normalize(cs);
            const glm::dvec3 Am = glm::normalize(glm::cross(Nn, Um)), As = glm::normalize(glm::cross(Nn, Us));
            const glm::dvec3 Cm = glm::cross(Um, Am), Cs = glm::cross(Us, As);
            // The ring's SHAPE (rotation-free: the group's ring turns with its own argument of latitude, which runs
            // ahead of or behind the slots' while it climbs): every pairwise distance vs the slots'.
            for (size_t a = 0; a < rm.size(); ++a)
                for (size_t b = a + 1; b < rm.size(); ++b)
                    shapeErr = std::max(shapeErr, std::abs(glm::length(rm[a] - rm[b]) - glm::length(rs[a] - rs[b])));
            nlohmann::json mem = nlohmann::json::array();
            for (size_t i = 0; i < rm.size(); ++i)
            {
                const glm::dvec3 dm = rm[i] - cm, ds = rs[i] - cs;
                const glm::dvec3 om(glm::dot(dm, Um), glm::dot(dm, Am), glm::dot(dm, Cm));
                const glm::dvec3 os(glm::dot(ds, Us), glm::dot(ds, As), glm::dot(ds, Cs));
                const double e = glm::length(om - os);
                errMax = std::max(errMax, e);
                spreadMax = std::max(spreadMax, glm::length(om));
                ringR = std::max(ringR, glm::length(os));
                if (c0 == 0)
                    mem.push_back({{"m", i}, {"offset_m", {om.x, om.y, om.z}}, {"slot_offset_m", {os.x, os.y, os.z}}, {"err_m", e}});
            }
            if (c0 == 0)
            {
                first["members"] = mem;
                first["centroid_alt_km"] = (glm::length(cm) - kR) / 1000.0;
                first["slot_centroid_alt_km"] = (glm::length(cs) - kR) / 1000.0;
                first["centroid_to_slot_km"] = glm::length(cm - cs) / 1000.0;
            }
        }
        if (t > t0 + 2.0 * kDay)
        {
            if (prevErr >= 0.0)
                worstGrow = std::max(worstGrow, shapeErr - prevErr);
            prevErr = shapeErr;
            errAfter2d = std::max(errAfter2d, shapeErr);
            spreadAfter2d = std::max(spreadAfter2d, spreadMax);
        }
        rows.push_back({{"t_after_release_h", (t - B.mem.front().tRel) / 3600.0},
                        {"t_before_arrival_h", (B.tA - t) / 3600.0},
                        {"ring_shape_err_m_max", shapeErr},
                        {"ring_err_m_max", errMax},
                        {"spread_from_centroid_m_max", spreadMax},
                        {"ring_radius_m", ringR},
                        {"cluster0", first}});
    }
    j["samples"] = rows;
    j["shape_err_growth_after_2d_m_max"] = worstGrow;
    j["ring_shape_err_after_2d_m_max"] = errAfter2d;
    j["spread_after_2d_m_max"] = spreadAfter2d;
    return j;
}

// A rocket model's engines for the Info window's 3D view (its flame; 2026-10-21): (exit plane z, exit radius, length).
glm::dvec3 SatelliteSim::rocketEngineGeom(int typeIdx) const
{
    if (typeIdx < 0 || typeIdx >= (int)satTypes.size())
        return glm::dvec3(0.0, 2.0, 40.0);
    return LaunchImpl::engineGeomOf(satTypes[(size_t)typeIdx]);
}

// (2026-10-28) The launches' share of the harness's `kessler mem` report.
void SatelliteSim::launchMemReport(std::vector<std::pair<std::string, double>> &out) const
{
    if (!launchState)
        return;
    const LaunchState &L = *launchState;
    auto vb = [](const auto &x) { return (double)x.capacity() * sizeof(x[0]); };
    double bat = (double)L.batches.bucket_count() * 8.0, parcels = 0.0;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        bat += sizeof(LBatch) + 32.0 + vb(B.mem) + vb(B.chunks[0]) + vb(B.chunks[1]) + vb(B.ends);
        parcels += vb(B.parcels);
    }
    out.push_back({"launch.batches", bat});
    out.push_back({"launch.parcels", parcels});
    double pl = vb(L.planes);
    for (const auto &p : L.planes)
        pl += vb(p.sats) + vb(p.fillT);
    out.push_back({"launch.planes", pl});
    double riv = (double)L.rivalIdx.size() * 64.0 + (double)L.cands.size() * 96.0;
    for (const auto &kv : L.rivalIdx)
        if (kv.second)
            riv += vb(*kv.second);
    out.push_back({"launch.cands_rivals", riv});
    out.push_back({"launch.slots_elems", vb(L.slots) + vb(L.elems)});
}
