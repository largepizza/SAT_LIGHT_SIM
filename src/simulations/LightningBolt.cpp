// A lightning flash's channel as geometry: see LightningBolt.h.
#include "LightningBolt.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace bolt
{
namespace
{
inline uint32_t pcg(uint32_t v)
{
    v = v * 747796405u + 2891336453u;
    const uint32_t w = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (w >> 22u) ^ w;
}
inline uint32_t mix2(uint32_t a, uint32_t b) { return pcg(a ^ (pcg(b) + 0x9E3779B9u + (a << 6) + (a >> 2))); }

// A counter keyed by what it decides (a path's vertex, a branch point): no draw depends on how many came before.
struct Rng
{
    uint32_t k, n = 0;
    explicit Rng(uint32_t key) : k(key) {}
    uint32_t next() { return pcg(k + 0x632BE5ABu * ++n); }
    float u() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
    float s1() { return 2.0f * u() - 1.0f; }
    float g() { return (u() + u() + u() - 1.5f) * 1.41f; }   // a bell, sigma ~0.7
};

// A unit vector across d; vz < 1 flattens it (the near-level sheets wander sideways, hardly up and down).
glm::vec3 across(Rng &r, glm::vec3 d, float vz)
{
    glm::vec3 v(r.s1(), r.s1(), r.s1());
    v -= d * glm::dot(v, d);
    v.z *= vz;
    const float l = glm::length(v);
    if (l > 1e-4f)
        return v / l;
    v = glm::cross(d, glm::vec3(0.0f, 0.0f, 1.0f));
    const float l2 = glm::length(v);
    return l2 > 1e-4f ? v / l2 : glm::vec3(1.0f, 0.0f, 0.0f);
}

// The midpoint's throw as a share of the piece's length (x a path's roughK).
float roughAt(float)
{
    return 0.2f;
}

struct PathOpt
{
    float minSeg = 6.0f;
    int maxLevels = 11;
    float roughK = 1.0f;
    float vz = 1.0f;
    uint8_t order = 0, flags = 0;
    int parent = -1;
    uint32_t parentVert = 0;
    float arc0 = 0.0f, w0 = 1.0f, w1 = 1.0f;
};

// A tortuous path a -> b: midpoint displacement level by level (level l makes the vertices at odd multiples of
// 2^(levels - l)), each midpoint thrown across its piece by its own counter. Returns the path's index.
uint32_t addPath(Tree &t, uint32_t key, glm::vec3 a, glm::vec3 b, const PathOpt &o)
{
    const float L = glm::length(b - a);
    const int D = std::clamp((int)std::ceil(std::log2(std::max(L / std::max(o.minSeg, 0.5f), 1.0f))), 1, o.maxLevels);
    const uint32_t N = 1u << D;
    const size_t first = t.verts.size();
    t.verts.resize(first + N + 1);
    Vert *v = &t.verts[first];
    v[0].p = a;
    v[N].p = b;
    v[0].lvl = v[N].lvl = 0;
    for (int l = 1; l <= D; ++l)
    {
        const uint32_t st = N >> l;
        for (uint32_t i = st; i < N; i += 2 * st)
        {
            Rng r(mix2(key, i));
            const glm::vec3 pa = v[i - st].p, pb = v[i + st].p;
            const float len = glm::length(pb - pa);
            glm::vec3 m = 0.5f * (pa + pb);
            if (len > 1e-3f)
            {
                const glm::vec3 d = (pb - pa) / len;
                const float tt = 0.5f + 0.13f * r.s1();
                const glm::vec3 n = across(r, d, o.vz);
                m = glm::mix(pa, pb, tt) + n * (len * roughAt(len) * o.roughK * r.g());
            }
            v[i].p = m;
            v[i].lvl = (uint8_t)l;
        }
    }
    float arc = o.arc0;
    v[0].arc = arc;
    for (uint32_t i = 1; i <= N; ++i)
    {
        arc += glm::length(v[i].p - v[i - 1].p);
        v[i].arc = arc;
    }
    const float len = std::max(arc - o.arc0, 1e-3f);
    for (uint32_t i = 0; i <= N; ++i)
        v[i].w = glm::mix(o.w0, o.w1, (v[i].arc - o.arc0) / len);
    Path p;
    p.first = (uint32_t)first;
    p.count = N + 1;
    p.order = o.order;
    p.levels = (uint8_t)D;
    p.flags = o.flags;
    p.parent = o.parent;
    p.parentVert = o.parentVert;
    p.length = len;
    p.centre = 0.5f * (a + b);
    float r2 = 0.0f;
    for (uint32_t i = 0; i <= N; ++i)
        r2 = std::max(r2, glm::dot(v[i].p - p.centre, v[i].p - p.centre));
    p.radius = std::sqrt(r2);
    t.paths.push_back(p);
    t.maxArc = std::max(t.maxArc, arc);
    return (uint32_t)t.paths.size() - 1;
}

// The path's direction about vertex i over ~spanM (its trend, not its last wiggle).
glm::vec3 trend(const Tree &t, const Path &p, uint32_t i, float spanM)
{
    const uint32_t N = p.count - 1;
    const uint32_t s = std::max<uint32_t>(1u, (uint32_t)std::lround((double)N * spanM / std::max(p.length, 1.0f)));
    const uint32_t i0 = i > s ? i - s : 0u, i1 = std::min(N, i + s);
    const glm::vec3 d = t.verts[p.first + i1].p - t.verts[p.first + i0].p;
    const float l = glm::length(d);
    return l > 1e-3f ? d / l : glm::vec3(0.0f, 0.0f, -1.0f);
}

// The stride between a path's candidate branch points: about spacingM apart.
uint32_t candStride(const Path &p, float spacingM)
{
    const uint32_t N = p.count - 1;
    const int lc = std::clamp((int)std::ceil(std::log2(std::max(p.length / std::max(spacingM, 1.0f), 1.0f))), 0, (int)p.levels);
    return std::max<uint32_t>(1u, N >> lc);
}

// ── Side branches off a path (the rules of 2026-10-05): per metre of the parent `perM` of them, each `lenScale`
// long at most, an order finer; twigs and tendrils off each in turn. A ground stroke's head down and out, shorter the
// lower they start; a sheet's run sideways, nearly level, the finest drooping a little. ──
void sideBranches(Tree &t, uint32_t pi, const Params &P, uint32_t key, int order, float perM, float wgt, float lenScale,
                  size_t budget)
{
    if (order > 3 || t.verts.size() > budget)
        return;
    const Path par = t.paths[pi];   // (a copy: the vectors grow)
    if (par.count < 5)
        return;
    const uint32_t N = par.count - 1;
    const uint32_t sc = candStride(par, 25.0f);
    const bool ground = P.kind == 1;
    for (uint32_t i = sc; i + sc <= N; i += sc)
    {
        if (t.verts.size() > budget)
            return;
        Rng r(mix2(key, 0xB0000u + i));
        const Vert vp = t.verts[par.first + i];
        const float pl = vp.arc - t.verts[par.first + i - sc].arc;
        if (r.u() > perM * pl)
            continue;
        const glm::vec3 d = trend(t, par, i, 30.0f);
        glm::vec3 dir;
        float len;
        if (ground)
        {
            const float h = std::max(vp.p.z - P.ground.z, 50.0f);
            const glm::vec3 out = across(r, d, 1.0f);
            dir = glm::normalize(d * (0.3f + 0.4f * r.u()) + out * (0.5f + 0.7f * r.u()) + glm::vec3(0.0f, 0.0f, -0.6f * r.u()));
            if (dir.z > -0.15f)
                dir = glm::normalize(dir + glm::vec3(0.0f, 0.0f, -0.4f));
            len = std::min(h * (0.12f + 0.45f * r.u()), lenScale * (0.4f + r.u()));
        }
        else
        {
            glm::vec3 out = across(r, d, 1.0f);
            out.z *= 0.25f;
            dir = glm::normalize(d * 0.35f + glm::normalize(out + glm::vec3(1e-4f)) + glm::vec3(0.0f, 0.0f, order >= 3 ? -0.35f : 0.0f));
            len = lenScale * (0.4f + r.u());
        }
        if (len < 12.0f)
            continue;
        PathOpt o;
        o.minSeg = ground ? (order == 1 ? 6.0f : 8.0f) : (order == 1 ? 16.0f : 20.0f);
        o.maxLevels = order == 1 ? 9 : (order == 2 ? 7 : 5);
        o.roughK = (order >= 3 ? 0.3f : 0.24f) / 0.2f;
        o.vz = ground ? 1.0f : 0.22f;
        o.order = (uint8_t)order;
        o.flags = ground ? (uint8_t)0 : (uint8_t)kLevel;
        o.parent = (int)pi;
        o.parentVert = i;
        o.arc0 = vp.arc;
        o.w0 = wgt;
        o.w1 = wgt * 0.35f;
        const uint32_t ck = mix2(key, 0xC0000u + i);
        const uint32_t ci = addPath(t, ck, vp.p, vp.p + dir * len, o);
        if (order == 1 && (!ground || (par.flags & kTrunk)))
            ++t.sideBranches;
        const float childScale = len * (ground ? 0.45f : 0.5f);
        if (order + 1 <= 3)
            sideBranches(t, ci, P, ck, order + 1, perM * (order + 1 == 3 ? 3.0f : 1.6f), wgt * 0.5f,
                         order + 1 == 3 ? std::min(childScale, 260.0f) : childScale, budget);
    }
}

void buildGround(const Params &P, Tree &t, uint32_t key)
{
    // From the charge region in the cloud, a wander inside it, out of the cloud's flank or base, and down SLANTED
    // to the strike point (cloud_v2_lightning.comp put it 1.5-9 km aside).
    Rng r(mix2(key, 0xC6u));
    const glm::vec3 G = P.ground;
    // The cloud base, never below the strike's ground (high terrain under a low base).
    const float zb = std::min(std::max(P.baseZ, G.z + 400.0f), -100.0f);
    const glm::vec2 h(G.x, G.y);
    const float hl = glm::length(h);
    const glm::vec2 hd = hl > 1.0f ? h / hl : glm::vec2(1.0f, 0.0f);
    const glm::vec2 side(-hd.y, hd.x);
    const glm::vec3 P1(hd * (0.1f * hl * r.u()) + side * (800.0f * r.s1()), zb * (0.35f + 0.3f * r.u()));
    const float xf = 0.05f + 0.3f * r.u();
    const glm::vec3 X(h * xf + side * (600.0f * r.s1()), zb);
    PathOpt o;
    o.order = 0;
    o.minSeg = 10.0f;
    o.maxLevels = 8;
    o.flags = kInCloud;
    const uint32_t a = addPath(t, key ^ 0x11u, glm::vec3(0.0f), P1, o);
    o.arc0 = t.verts.back().arc;
    const uint32_t b = addPath(t, key ^ 0x22u, P1, X, o);
    o.arc0 = t.verts.back().arc;
    o.minSeg = 5.0f;
    o.maxLevels = 11;
    o.flags = kTrunk;
    const uint32_t c = addPath(t, key ^ 0x33u, X, G, o);
    t.mainArc = t.verts.back().arc;
    t.exitLocal = X;
    const size_t budget = 9000;
    const float dens = std::clamp(P.tendrils, 0.0f, 4.0f);
    // Branches: the stepped leader's; then twigs and tendrils.
    // (a share of the vertex budget each, the descent below the cloud the largest: taken in order from the top with
    // one budget, the cloud's part used it up and the visible channel was bare.)
    sideBranches(t, a, P, key ^ 0x44u, 1, dens / 260.0f, 0.32f, 3000.0f, t.verts.size() + budget / 6);
    sideBranches(t, b, P, key ^ 0x45u, 1, dens / 260.0f, 0.32f, 3000.0f, t.verts.size() + budget / 6);
    sideBranches(t, c, P, key ^ 0x46u, 1, dens / 260.0f, 0.32f, 3000.0f, budget);
    // Upward streamers from the ground near the strike (the ones the leader did not meet).
    const int nUp = 2 + (int)(r.u() * 4.0f);
    for (int k = 0; k < nUp; ++k)
    {
        Rng q(mix2(key, 0x8800u + (uint32_t)k));
        const glm::vec3 g0 = G + glm::vec3(q.s1() * 120.0f, q.s1() * 120.0f, 0.0f);
        PathOpt s;
        s.order = 3;
        s.minSeg = 6.0f;
        s.maxLevels = 5;
        s.roughK = 1.25f;
        s.flags = kStreamer;
        s.arc0 = t.mainArc;
        s.w0 = 0.06f;
        s.w1 = 0.02f;
        addPath(t, mix2(key, 0x8900u + (uint32_t)k), g0, g0 + glm::vec3(q.s1() * 30.0f, q.s1() * 30.0f, 40.0f + 200.0f * q.u()), s);
    }
}

// ── In the cloud (kind 0) or just under its base (kind 3, spider lightning): 3-6 arms spreading out nearly level
// for 6-30 km, branching into a web of twigs and drooping tendrils. The paths wander sideways, hardly up and down
// (across()'s vz): thrown freely, a 30 km arm's midpoint went kilometres up or down, through the ground
// (issue_20261121_163317 shot 1). ──
void buildSheet(const Params &P, Tree &t, uint32_t key)
{
    Rng r(mix2(key, 0x5Bu));
    const size_t budget = 9000;
    const float dens = std::clamp(P.tendrils, 0.0f, 4.0f);
    const int nA = 3 + (int)(r.u() * 4.0f);
    const float reach = 6000.0f + 24000.0f * r.u() * (0.5f + 0.5f * P.strength);
    const float a0 = 6.2831853f * r.u();
    for (int k = 0; k < nA; ++k)
    {
        Rng q(mix2(key, 0x3300u + (uint32_t)k));
        const float a = a0 + 6.2831853f * ((float)k + 0.35f * q.s1()) / (float)nA;
        const float ln = reach * (0.45f + 0.55f * q.u());
        const glm::vec3 e(std::cos(a) * ln, std::sin(a) * ln, 350.0f * q.s1() + (P.kind == 3 ? 150.0f * q.u() : 0.0f));
        PathOpt o;
        o.order = 0;
        o.minSeg = 14.0f;
        o.maxLevels = 9;
        o.vz = 0.22f;
        o.flags = kLevel;
        o.w0 = 0.5f;   // fainter than a return stroke
        o.w1 = 0.25f;
        const uint32_t pk = mix2(key, 0x3400u + (uint32_t)k);
        const uint32_t pi = addPath(t, pk, glm::vec3(0.0f), e, o);
        t.armStart.push_back((uint32_t)t.arms.size());
        t.arms.push_back(pi);
        sideBranches(t, pi, P, pk, 1, dens / 1500.0f, 0.4f, ln * 0.3f, budget * (size_t)(k + 1) / (size_t)nA);
    }
    t.armStart.push_back((uint32_t)t.arms.size());
    t.mainArc = t.maxArc;
}

// ── A red sprite, ~75 km up: a head, 20-45 tendrils hanging 12-35 km, forking as they fall and splaying out (a
// jellyfish of roots), and a few streamers rising from the head. ──
void spriteBranches(Tree &t, uint32_t pi, uint32_t key, int order, float expect, float wgt, float lenScale)
{
    if (order > 3 || expect <= 0.0f || t.verts.size() > 9000)
        return;
    const Path par = t.paths[pi];
    const uint32_t N = par.count - 1;
    for (uint32_t i = 1; i < N; ++i)
    {
        Rng r(mix2(key, 0xA0000u + i));
        if (r.u() >= expect / (float)std::max(N - 1u, 1u))
            continue;
        const Vert vp = t.verts[par.first + i];
        const glm::vec3 d = trend(t, par, i, 1500.0f);
        const glm::vec3 dir = glm::normalize(d * 0.8f + across(r, d, 1.0f) * 0.45f);
        const float len = lenScale * (0.3f + 0.5f * r.u());
        if (len < 600.0f)
            continue;
        PathOpt o;
        o.minSeg = len / 12.0f;
        o.maxLevels = 4;
        o.roughK = 0.42f;
        o.order = (uint8_t)order;
        o.parent = (int)pi;
        o.parentVert = i;
        o.arc0 = vp.arc;
        o.w0 = wgt;
        o.w1 = wgt * 0.35f;
        const uint32_t ck = mix2(key, 0xA8000u + i);
        const uint32_t ci = addPath(t, ck, vp.p, vp.p + dir * len, o);
        spriteBranches(t, ci, ck, order + 1, expect * (order + 1 == 3 ? 0.9f : 0.6f), wgt * 0.5f, len * 0.5f);
    }
}

void buildSprite(const Params &P, Tree &t, uint32_t key)
{
    Rng r(mix2(key, 0x59u));
    const float dens = std::clamp(P.tendrils, 0.0f, 4.0f);
    const int nT = (int)((20.0f + 25.0f * r.u()) * std::max(dens, 0.3f));
    for (int k = 0; k < nT; ++k)
    {
        Rng q(mix2(key, 0x9100u + (uint32_t)k));
        const float a = 6.2831853f * q.u(), rr = 6000.0f * std::sqrt(q.u());
        const glm::vec3 s0(std::cos(a) * rr, std::sin(a) * rr, 3000.0f * q.s1());
        const float ln = 12000.0f + 23000.0f * q.u();
        const glm::vec3 e = s0 + glm::vec3(std::cos(a) * rr * 0.6f + 2500.0f * q.s1(), std::sin(a) * rr * 0.6f + 2500.0f * q.s1(), -ln);
        PathOpt o;
        o.minSeg = ln / 16.0f;
        o.maxLevels = 4;
        o.roughK = 0.3f;
        o.order = 0;
        o.w0 = 0.7f;
        o.w1 = 0.25f;
        const uint32_t pk = mix2(key, 0x9200u + (uint32_t)k);
        const uint32_t pi = addPath(t, pk, s0, e, o);
        spriteBranches(t, pi, pk, 1, ln / 5000.0f * dens, 0.4f, ln * 0.45f);
    }
    const int nU = 4 + (int)(r.u() * 6.0f);
    for (int k = 0; k < nU; ++k)
    {
        Rng q(mix2(key, 0x9500u + (uint32_t)k));
        const glm::vec3 s0(3000.0f * q.s1(), 3000.0f * q.s1(), 4000.0f);
        PathOpt o;
        o.minSeg = 1500.0f;
        o.maxLevels = 3;
        o.roughK = 0.35f;
        o.order = 2;
        o.w0 = 0.25f;
        o.w1 = 0.05f;
        addPath(t, mix2(key, 0x9600u + (uint32_t)k), s0, s0 + glm::vec3(2500.0f * q.s1(), 2500.0f * q.s1(), 5000.0f + 9000.0f * q.u()), o);
    }
    t.mainArc = t.maxArc;
}
} // namespace

uint64_t hashVerts(const Tree &t)
{
    uint64_t h = 1469598103934665603ull;
    auto eat = [&](const void *p, size_t n) {
        const unsigned char *c = (const unsigned char *)p;
        for (size_t i = 0; i < n; ++i)
        {
            h ^= c[i];
            h *= 1099511628211ull;
        }
    };
    for (const Path &p : t.paths)
        for (uint32_t i = 0; i < p.count; ++i)
            eat(&t.verts[p.first + i].p, sizeof(glm::vec3));
    return h;
}

void build(const Params &P, Tree &t)
{
    t = Tree();
    const uint32_t key = mix2(mix2(P.seed, P.id), 0xB017u + (uint32_t)P.kind);
    if (P.kind == 1)
        buildGround(P, t, key);
    else if (P.kind == 2)
        buildSprite(P, t, key);
    else
        buildSheet(P, t, key);
    t.maxArc = std::max(t.maxArc, 1.0f);
    t.mainArc = std::max(t.mainArc, 1.0f);
    t.hash = hashVerts(t);
}

int pathLevel(const Path &p, float targetLenM)
{
    if (p.count < 2)
        return -1;
    const float target = std::max(targetLenM, 0.5f);
    if (p.order > 0 && p.length < 0.7f * target)
        return -1;
    return std::clamp((int)std::ceil(std::log2(std::max(p.length / target, 1.0f))), 0, (int)p.levels);
}

size_t segmentsAt(const Tree &t, const int8_t *levels, std::vector<Seg> &out, size_t maxSegs)
{
    out.clear();
    for (uint32_t ord = 0; ord < 4; ++ord)
        for (size_t pi = 0; pi < t.paths.size(); ++pi)
        {
            const Path &p = t.paths[pi];
            const int L = levels[pi];
            if (p.order != ord || p.count < 2 || L < 0)
                continue;
            const uint32_t N = p.count - 1;
            const float a0 = t.verts[p.first].arc;
            const float invLen = 1.0f / std::max(t.verts[p.first + N].arc - a0, 1e-3f);
            uint32_t prev = 0;
            for (uint32_t i = 1; i <= N; ++i)
            {
                const Vert &v = t.verts[p.first + i];
                if (i != N && (int)v.lvl > L)
                    continue;
                if (out.size() >= maxSegs)
                    return out.size();
                const Vert &u = t.verts[p.first + prev];
                Seg s;
                s.p0 = u.p;
                s.p1 = v.p;
                s.w = 0.5f * (u.w + v.w);
                s.order = p.order;
                s.arc = u.arc;
                s.f = glm::mix(p.f0, p.f1, (0.5f * (u.arc + v.arc) - a0) * invLen);
                out.push_back(s);
                prev = i;
            }
        }
    return out.size();
}

size_t segments(const Tree &t, float targetLenM, std::vector<Seg> &out, size_t maxSegs)
{
    std::vector<int8_t> levels(t.paths.size());
    for (size_t pi = 0; pi < t.paths.size(); ++pi)
        levels[pi] = (int8_t)pathLevel(t.paths[pi], targetLenM);
    return segmentsAt(t, levels.data(), out, maxSegs);
}

glm::vec3 armFront(const Tree &t, size_t k, float reveal)
{
    if (k + 1 >= t.armStart.size())
        return glm::vec3(0.0f);
    glm::vec3 front(0.0f);
    bool any = false;
    for (uint32_t j = t.armStart[k]; j < t.armStart[k + 1]; ++j)
    {
        const Path &p = t.paths[t.arms[j]];
        if (p.count < 2)
            break;
        if (!any)
        {
            front = t.verts[p.first].p;
            any = true;
        }
        // (coarse steps: the front is a light's place, not a vertex to draw)
        const uint32_t N = p.count - 1, st = std::max<uint32_t>(1u, N / 32u);
        for (uint32_t i = 0; i <= N; i += st)
        {
            const Vert &v = t.verts[p.first + i];
            if (v.arc > reveal)
                return front;
            front = v.p;
        }
        if (t.verts[p.first + N].arc <= reveal)
            front = t.verts[p.first + N].p;
    }
    return front;
}

float fitTerrain(Tree &t, const std::vector<float> &terr)
{
    if (terr.size() < t.verts.size())
        return 0.0f;
    std::vector<float> lift(t.verts.size(), 0.0f);
    // A ground stroke: everything below the cloud is scaled in height about the exit so that the trunk's last
    // vertex lands on the ground there.
    float zTop = 0.0f, scale = 1.0f;
    bool warp = false;
    for (const Path &p : t.paths)
        if ((p.flags & kTrunk) && p.count >= 2)
        {
            const Vert &v0 = t.verts[p.first], &v1 = t.verts[p.first + p.count - 1];
            const float drop = v0.p.z - v1.p.z;
            if (drop > 50.0f)
            {
                zTop = v0.p.z;
                scale = std::clamp((v0.p.z - terr[p.first + p.count - 1]) / drop, 0.2f, 5.0f);
                warp = true;
            }
        }
    if (warp)
        for (const Path &p : t.paths)
        {
            if (p.flags & (kInCloud | kStreamer))
                continue;
            for (uint32_t i = 0; i < p.count; ++i)
            {
                Vert &v = t.verts[p.first + i];
                if (v.p.z < zTop)
                    v.p.z = zTop + (v.p.z - zTop) * scale;
            }
        }
    std::vector<uint32_t> cutAt(t.paths.size(), 0xFFFFFFFFu);   // a path's last kept vertex, where the ground ended it
    for (size_t pi = 0; pi < t.paths.size(); ++pi)
    {
        Path &p = t.paths[pi];
        if (p.count < 2)
            continue;
        Vert *v = &t.verts[p.first];
        const float *tr = &terr[p.first];
        float *lf = &lift[p.first];
        const uint32_t N = p.count - 1;
        // Its root follows what the terrain did to its parent there (moved, or gone).
        float rootLift = 0.0f;
        if (p.parent >= 0)
        {
            const Path &par = t.paths[(size_t)p.parent];
            if (par.count < 2 || p.parentVert > par.count - 1 || (cutAt[(size_t)p.parent] != 0xFFFFFFFFu && p.parentVert >= par.count - 1))
            {
                p.count = 0;   // its parent ended before it (or on the ground at it)
                continue;
            }
            rootLift = t.verts[par.first + p.parentVert].p.z - v[0].p.z;
        }
        if (p.flags & kStreamer)
        {
            // Rooted on the ground: moved whole, then kept above it.
            const float dz = tr[0] - v[0].p.z;
            for (uint32_t i = 0; i <= N; ++i)
                v[i].p.z = std::max(v[i].p.z + dz, tr[i] + (i == 0 ? 0.0f : 0.5f));
            continue;
        }
        if (p.flags & kTrunk)
        {
            // Over a ridge in its way, with a clearance that closes to nothing at the strike point.
            const float arcEnd = v[N].arc;
            for (uint32_t i = 0; i <= N; ++i)
            {
                const float c = std::min(10.0f, 0.3f * (arcEnd - v[i].arc));
                lf[i] = std::max(0.0f, tr[i] + c - v[i].p.z);
            }
            lf[N] = tr[N] - v[N].p.z;   // exactly on the ground
            for (uint32_t i = 1; i < N; ++i)
                lf[i] = std::max(lf[i], lf[i - 1] - 0.6f * (v[i].arc - v[i - 1].arc));
            for (uint32_t i = N - 1; i > 0; --i)
                lf[i] = std::max(lf[i], std::max(lf[i + 1], 0.0f) - 0.6f * (v[i + 1].arc - v[i].arc));
            for (uint32_t i = 0; i <= N; ++i)
                v[i].p.z += lf[i];
            continue;
        }
        if (p.flags & (kLevel | kInCloud))
        {
            // A sheet rides over the relief with a clearance: the lift each vertex needs, spread along the path as a
            // tent so it climbs a slope instead of stepping.
            const float clear = (p.flags & kInCloud) ? 60.0f : 120.0f;
            for (uint32_t i = 0; i <= N; ++i)
                lf[i] = std::max(0.0f, tr[i] + clear - v[i].p.z);
            if (p.parent >= 0)
                lf[0] = rootLift;   // (>= its own need: the parent's vertex there cleared the same ground)
            for (uint32_t i = 1; i <= N; ++i)
                lf[i] = std::max(lf[i], lf[i - 1] - 0.5f * (v[i].arc - v[i - 1].arc));
            for (uint32_t i = N; i-- > 1;)
                lf[i] = std::max(lf[i], lf[i + 1] - 0.5f * (v[i + 1].arc - v[i].arc));
            for (uint32_t i = 0; i <= N; ++i)
                v[i].p.z += lf[i];
            continue;
        }
        // A ground stroke's branch: carried with its root, and ended where it meets the ground.
        for (uint32_t i = 0; i <= N; ++i)
        {
            const float fade = 1.0f - std::min((v[i].arc - v[0].arc) / std::max(p.length, 1.0f), 1.0f);
            lf[i] = rootLift * fade;
            v[i].p.z += lf[i];
        }
        for (uint32_t i = 1; i <= N; ++i)
            if (v[i].p.z < tr[i] + 3.0f)
            {
                v[i].p.z = tr[i];
                lf[i] = 0.0f;
                p.count = i + 1;
                p.length = std::max(v[i].arc - v[0].arc, 1e-3f);
                cutAt[pi] = i;
                break;
            }
    }
    float minClear = 1e30f;
    for (const Path &p : t.paths)
        for (uint32_t i = 0; i < p.count; ++i)
            minClear = std::min(minClear, t.verts[p.first + i].p.z - terr[p.first + i]);
    return minClear > 1e29f ? 0.0f : minClear;
}

Stats stats(const Tree &t)
{
    Stats s;
    s.sideBranches = t.sideBranches;
    glm::vec3 first(0.0f), last(0.0f);
    bool any = false;
    for (const Path &p : t.paths)
    {
        if (p.count < 2)
            continue;
        ++s.paths[std::min<int>(p.order, 3)];
        s.verts += (int)p.count;
        if (p.order != 0)
            continue;
        if (!any)
            first = t.verts[p.first].p;
        any = true;
        last = t.verts[p.first + p.count - 1].p;
        s.mainLen += t.verts[p.first + p.count - 1].arc - t.verts[p.first].arc;
    }
    s.mainChord = glm::length(last - first);
    // The longest run of an order-0 path that a line fits to within 2% of the run's own length.
    for (const Path &p : t.paths)
    {
        if (p.order != 0 || p.count < 3)
            continue;
        const Vert *v = &t.verts[p.first];
        const uint32_t N = p.count - 1;
        const uint32_t step = std::max<uint32_t>(1u, N / 256u);
        for (uint32_t a = 0; a < N; a += step)
        {
            uint32_t best = a;
            for (uint32_t b = a + step; b <= N; b += step)
            {
                const glm::vec3 ab = v[b].p - v[a].p;
                const float L = glm::length(ab);
                if (L < 1e-3f)
                    continue;
                const glm::vec3 d = ab / L;
                float dev = 0.0f;
                for (uint32_t i = a + 1; i < b; ++i)
                {
                    const glm::vec3 r = v[i].p - v[a].p;
                    dev = std::max(dev, glm::length(r - d * glm::dot(r, d)));
                }
                if (dev > 0.02f * L)
                    break;
                best = b;
            }
            if (best > a)
                s.maxStraight = std::max(s.maxStraight, glm::length(v[best].p - v[a].p));
        }
    }
    return s;
}
} // namespace bolt
