#pragma once
// A lightning flash's channel as geometry (2026-11-21, issue_20261121_163317; SatelliteSimLightning.cpp draws it,
// tools/bolt_tool measures it). The tree is a pure function of the flash's own record: its seed and id, its kind,
// the cloud's base and top and the strike point, all in the flash's Earth-fixed local frame. Nothing about the
// observer enters it, so every viewpoint and every run builds the same vertices.
//
// It is built ONCE at full detail (pieces of a few metres) as paths of midpoint displacement whose vertices carry
// the subdivision LEVEL that made them: the path at level L is the vertices with lvl <= L, a subset of the full
// path. Level of detail is therefore a choice of which vertices to DRAW (bolt::segments, per frame, from the
// distance and the field of view), never a different shape. Every random number is drawn from a counter keyed by
// (path, vertex): no shared stream, so one decision cannot shift another.
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>

namespace bolt
{
struct Params
{
    uint32_t seed = 0, id = 0;
    int kind = 0;                    // 0 in the cloud, 1 cloud-to-ground, 2 red sprite, 3 spider under the base
    glm::vec3 ground{0.0f};          // kind 1: the strike point (m from the origin; x, y tangents, z up)
    float baseZ = -1000.0f;          // the cloud's base and top relative to the origin (m)
    float topZ = 3000.0f;
    float strength = 0.5f;           // the storm's strength (0..1): a spider's reach
    float tendrils = 1.0f;           // "Lightning tendrils": x the branch and tendril counts
};

struct Vert
{
    glm::vec3 p;      // local (m from the origin)
    float arc;        // along the tree from the origin (m)
    float w;          // brightness weight
    uint8_t lvl;      // the subdivision level that made it (0 = a path's ends)
};

enum PathFlag : uint8_t
{
    kInCloud = 1,     // a ground stroke's part inside the cloud
    kStreamer = 2,    // an upward streamer: rooted on the ground beside the strike point
    kTrunk = 4,       // a ground stroke's descent below the cloud (its last vertex is the strike point)
    kLevel = 8,       // a near-level sheet (spider, in-cloud): kept above the terrain by a clearance, not cut
};

struct Path
{
    uint32_t first = 0, count = 0;   // its vertices (count = 2^levels + 1 until the terrain cuts it short)
    uint8_t order = 0;               // 0 main channel .. 3 tendril
    uint8_t levels = 0;
    uint8_t flags = 0;
    int32_t parent = -1;             // the path it leaves (-1: none), at that path's vertex parentVert
    uint32_t parentVert = 0;
    float length = 0.0f;             // its arc length (m)
    float f0 = 0.0f, f1 = 1.0f;      // a sprite's tendril: top 0 .. tip 1 along it
    glm::vec3 centre{0.0f};          // a sphere round it (the draw's level of detail goes by the eye's distance to it)
    float radius = 0.0f;
};

struct Tree
{
    std::vector<Vert> verts;
    std::vector<Path> paths;
    std::vector<uint32_t> arms;      // a spider's main arms: for each, the paths of its lineage (see armFront)
    std::vector<uint32_t> armStart;  // arms[armStart[k] .. armStart[k + 1]) = arm k's paths, origin outward
    float mainArc = 1.0f, maxArc = 1.0f;
    glm::vec3 exitLocal{0.0f};       // kind 1: where the channel leaves the cloud
    uint64_t hash = 0;               // of the vertices as built (before the terrain touches them)
    int sideBranches = 0;            // order-1 branches below the cloud (kind 1) / in all (others)
};

// One drawn piece (== GpuBoltSeg's content, SatelliteSim.h; `order`, `arc` and `f` are packed by the caller).
struct Seg
{
    glm::vec3 p0;
    float w;
    glm::vec3 p1;
    uint32_t order;
    float arc, f;
};

void build(const Params &p, Tree &t);

// The tree's pieces no shorter than about targetLenM (a path shorter than 0.7 x that is left out): the level of
// detail of a draw (2-3 pixels at the flash's distance) or of the thunder's sources. maxSegs caps the result (the
// finest orders are dropped first). Returns the number of pieces written.
size_t segments(const Tree &t, float targetLenM, std::vector<Seg> &out, size_t maxSegs = 1u << 20);
// The same with a level per path (pathLevel: -1 = left out), so one tree can be fine where the eye is near it and
// coarse where it is far (a spider's arms reach 30 km).
int pathLevel(const Path &p, float targetLenM);
size_t segmentsAt(const Tree &t, const int8_t *levels, std::vector<Seg> &out, size_t maxSegs = 1u << 20);

// Arm k's spreading front at `reveal` metres of arc from the origin (a spider's light moves with it).
glm::vec3 armFront(const Tree &t, size_t k, float reveal);

// Fit the tree to the terrain under it. terr[i] = the ground's height at vertex i's place, in the tree's local z
// (m; one per vertex of `t`). A ground stroke's descent is scaled so its last vertex lands ON the ground and is lifted
// where a ridge stands in its way; a branch that reaches the ground ends there; streamers are rooted on the ground;
// near-level sheets ride over the relief with a clearance. Returns the least clearance left (m, >= 0).
float fitTerrain(Tree &t, const std::vector<float> &terr);

// Statistics (tools/bolt_tool, harness `lightning`).
struct Stats
{
    int paths[4] = {0, 0, 0, 0};
    int verts = 0;
    int sideBranches = 0;
    float mainLen = 0.0f;        // the main channel's arc (m)
    float mainChord = 0.0f;      // origin to its end, straight (m)
    float maxStraight = 0.0f;    // the longest run of the main channel that stays within 2% of its length of a line (m)
};
Stats stats(const Tree &t);
uint64_t hashVerts(const Tree &t);
} // namespace bolt
