#include "SatMesh.h"

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace
{
constexpr double kPiD = 3.14159265358979323846;

// Appends a component's geometry, given in component-local coordinates, transformed into the rest
// body frame.
struct MeshSink
{
    SatRenderMesh &out;
    glm::dmat3 R;
    glm::dvec3 offset;
    int group, component;

    uint32_t vert(glm::dvec3 p, glm::dvec3 n, glm::dvec2 uv, int material)
    {
        SatMeshVertex v;
        v.pos = glm::vec3(R * p + offset);
        v.normal = glm::vec3(glm::normalize(R * n));
        v.uv = glm::vec2(uv);
        v.packed = packSatMeshVertexIds(group, material, component);
        out.vertices.push_back(v);
        return (uint32_t)(out.vertices.size() - 1);
    }
    // Triangle a-b-c, flipped if needed so it winds counter-clockwise seen from `outward` (local).
    void tri(uint32_t a, uint32_t b, uint32_t c, glm::dvec3 outwardLocal)
    {
        const glm::vec3 pa = out.vertices[a].pos, pb = out.vertices[b].pos, pc = out.vertices[c].pos;
        const glm::dvec3 n = glm::cross(glm::dvec3(pb - pa), glm::dvec3(pc - pa));
        if (glm::dot(n, R * outwardLocal) < 0.0)
            std::swap(b, c);
        out.indices.insert(out.indices.end(), {a, b, c});
    }
    // Flat quad a,b,c,d (local, in order around it) with normal n; uv = local coordinates projected
    // on the quad's own axes.
    void quad(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d, glm::dvec3 n, int material)
    {
        // UV origin at the first corner, so periodic patterns (truss bays, solar-cell modules) start at
        // an edge of the face rather than straddling it.
        const glm::dvec3 ex = glm::normalize(b - a), ey = glm::normalize(glm::cross(n, ex));
        auto uvOf = [&](glm::dvec3 p) { return glm::dvec2(glm::dot(p - a, ex), glm::dot(p - a, ey)); };
        uint32_t ia = vert(a, n, uvOf(a), material), ib = vert(b, n, uvOf(b), material);
        uint32_t ic = vert(c, n, uvOf(c), material), id = vert(d, n, uvOf(d), material);
        tri(ia, ib, ic, n);
        tri(ia, ic, id, n);
    }
};

// Cylinder (r0 == r1) or cone frustum along local Z, centred, smooth side normals.
void meshRevolved(MeshSink &s, double r0, double r1, double h, bool caps, int mat, int seg)
{
    const double z0 = -0.5 * h, z1 = 0.5 * h;
    const double slope = (r0 - r1) / std::max(h, 1e-9); // normal z-component factor
    const double rAvg = 0.5 * (r0 + r1);
    std::vector<uint32_t> ring0, ring1;
    for (int i = 0; i <= seg; ++i)
    {
        const double phi = 2.0 * kPiD * i / seg, c = std::cos(phi), sn = std::sin(phi);
        const glm::dvec3 n = glm::normalize(glm::dvec3(c, sn, slope));
        ring0.push_back(s.vert({r0 * c, r0 * sn, z0}, n, {phi * rAvg, z0}, mat));
        ring1.push_back(s.vert({r1 * c, r1 * sn, z1}, n, {phi * rAvg, z1}, mat));
    }
    for (int i = 0; i < seg; ++i)
    {
        const double phi = 2.0 * kPiD * (i + 0.5) / seg;
        const glm::dvec3 out(std::cos(phi), std::sin(phi), slope);
        if (r0 > 1e-9)
            s.tri(ring0[i], ring0[i + 1], ring1[i + 1], out);
        if (r1 > 1e-9)
            s.tri(ring0[i], ring1[i + 1], ring1[i], out);
    }
    if (!caps)
        return;
    for (int end = 0; end < 2; ++end)
    {
        const double r = end ? r1 : r0, z = end ? z1 : z0;
        if (r <= 1e-9)
            continue;
        const glm::dvec3 n(0.0, 0.0, end ? 1.0 : -1.0);
        const uint32_t centre = s.vert({0.0, 0.0, z}, n, {0.0, 0.0}, mat);
        std::vector<uint32_t> rim;
        for (int i = 0; i <= seg; ++i)
        {
            const double phi = 2.0 * kPiD * i / seg;
            const glm::dvec3 p(r * std::cos(phi), r * std::sin(phi), z);
            rim.push_back(s.vert(p, n, {p.x, p.y}, mat));
        }
        for (int i = 0; i < seg; ++i)
            s.tri(centre, rim[i], rim[i + 1], n);
    }
}

void meshSphere(MeshSink &s, double r, int mat, int seg)
{
    const int lat = std::max(4, seg / 2);
    std::vector<uint32_t> grid;
    for (int j = 0; j <= lat; ++j)
    {
        const double th = kPiD * j / lat; // from +Z
        for (int i = 0; i <= seg; ++i)
        {
            const double phi = 2.0 * kPiD * i / seg;
            const glm::dvec3 n(std::sin(th) * std::cos(phi), std::sin(th) * std::sin(phi), std::cos(th));
            grid.push_back(s.vert(r * n, n, {phi * r, th * r}, mat));
        }
    }
    auto at = [&](int j, int i) { return grid[(size_t)j * (seg + 1) + i]; };
    for (int j = 0; j < lat; ++j)
        for (int i = 0; i < seg; ++i)
        {
            const double th = kPiD * (j + 0.5) / lat, phi = 2.0 * kPiD * (i + 0.5) / seg;
            const glm::dvec3 out(std::sin(th) * std::cos(phi), std::sin(th) * std::sin(phi), std::cos(th));
            if (j > 0)
                s.tri(at(j, i), at(j + 1, i), at(j, i + 1), out);
            if (j < lat - 1)
                s.tri(at(j, i + 1), at(j + 1, i), at(j + 1, i + 1), out);
        }
}
// Body of revolution (PrimitiveKind::Profile): smooth normals around and along the meridian, except at a
// crease (> 35 deg between neighbouring segments: a dome's rim, a bell's lip); each azimuth interval is
// its own strip so the arc material changes at its edge. UV: u = azimuth x the ring's radius, v = arc
// length along the meridian (metres, continuous across rings). Optionally the inside too (an open bell).
void meshProfile(MeshSink &s, const SatComponent &c, int segBase)
{
    const auto &P = c.profile;
    double rMax = 0.0;
    for (const glm::vec2 &q : P)
        rMax = std::max(rMax, (double)q.y);
    // ~0.3 m facets on a large body, at most twice the model's base count, at least 12.
    int seg = (int)std::ceil(2.0 * kPiD * rMax / 0.3);
    seg = std::clamp((seg + 3) / 4 * 4, 12, 2 * segBase);
    const std::vector<double> az = satProfileAzimuths(c, seg);
    const size_t n = P.size();
    // Meridian normals per segment (r, z), outward for a profile running bottom to top along the outside.
    std::vector<glm::dvec2> segN(n - 1, glm::dvec2(1.0, 0.0));
    std::vector<double> arc(n, 0.0);
    for (size_t i = 0; i + 1 < n; ++i)
    {
        const double dz = P[i + 1].x - P[i].x, dr = P[i + 1].y - P[i].y;
        const double l = std::sqrt(dz * dz + dr * dr);
        if (l > 1e-12)
            segN[i] = glm::dvec2(dz / l, -dr / l);
        arc[i + 1] = arc[i] + l;
    }
    auto endN = [&](size_t i, int end) { // normal of segment i at its start (0) or end (1)
        const size_t j = end ? i + 1 : i;                // the ring
        const int other = end ? (int)i + 1 : (int)i - 1; // the neighbouring segment at that ring
        if (other < 0 || other >= (int)n - 1 || (j < n && P[j].y < 1e-6f))
            return segN[i]; // an end, or a pointed tip: the segment's own
        if (glm::dot(segN[i], segN[other]) < 0.819) // > 35 deg: a crease
            return segN[i];
        return glm::normalize(segN[i] + segN[other]);
    };
    for (int side = 0; side < (c.innerMaterial >= 0 ? 2 : 1); ++side)
    {
        const double sgn = side ? -1.0 : 1.0;
        for (size_t i = 0; i + 1 < n; ++i)
        {
            const double z0 = P[i].x, r0 = P[i].y, z1 = P[i + 1].x, r1 = P[i + 1].y;
            if (r0 < 1e-9 && r1 < 1e-9)
                continue;
            const glm::dvec2 n0 = endN(i, 0) * sgn, n1 = endN(i, 1) * sgn;
            for (size_t k = 0; k + 1 < az.size(); ++k)
            {
                const double a0 = az[k], a1 = az[k + 1], am = 0.5 * (a0 + a1);
                const int mat = side ? c.innerMaterial : (satProfileInArc(c, am) ? c.arcMaterial : c.material);
                const double ca0 = std::cos(a0), sa0 = std::sin(a0), ca1 = std::cos(a1), sa1 = std::sin(a1);
                // A pointed tip: the vertex normal at r = 0 is the facet's own around the axis.
                const double cn0 = r0 < 1e-6 ? std::cos(am) : ca0, sn0 = r0 < 1e-6 ? std::sin(am) : sa0;
                const double cn1 = r1 < 1e-6 ? std::cos(am) : ca1, sn1 = r1 < 1e-6 ? std::sin(am) : sa1;
                const double cm0 = r0 < 1e-6 ? std::cos(am) : ca1, sm0 = r0 < 1e-6 ? std::sin(am) : sa1;
                const double cm1 = r1 < 1e-6 ? std::cos(am) : ca1, sm1 = r1 < 1e-6 ? std::sin(am) : sa1;
                const uint32_t v00 = s.vert({r0 * ca0, r0 * sa0, z0}, {n0.x * cn0, n0.x * sn0, n0.y}, {a0 * r0, arc[i]}, mat);
                const uint32_t v10 = s.vert({r0 * ca1, r0 * sa1, z0}, {n0.x * cm0, n0.x * sm0, n0.y}, {a1 * r0, arc[i]}, mat);
                const uint32_t v11 = s.vert({r1 * ca1, r1 * sa1, z1}, {n1.x * cm1, n1.x * sm1, n1.y}, {a1 * r1, arc[i + 1]}, mat);
                const uint32_t v01 = s.vert({r1 * ca0, r1 * sa0, z1}, {n1.x * cn1, n1.x * sn1, n1.y}, {a0 * r1, arc[i + 1]}, mat);
                const glm::dvec2 nm = segN[i] * sgn;
                const glm::dvec3 out(nm.x * std::cos(am), nm.x * std::sin(am), nm.y);
                if (r0 > 1e-9)
                    s.tri(v00, v10, v11, out);
                if (r1 > 1e-9)
                    s.tri(v00, v11, v01, out);
            }
        }
    }
    if (!c.profileCaps)
        return;
    const double dir = P.back().x > P.front().x ? 1.0 : -1.0;
    for (int end = 0; end < 2; ++end)
    {
        const glm::vec2 q = end ? P.back() : P.front();
        const bool upper = (end == 1) == (dir > 0.0); // this end is the higher-z one
        if (q.y <= 1e-6f || !(c.profileCaps & (upper ? 2 : 1)))
            continue;
        const glm::dvec3 nrm(0.0, 0.0, end ? dir : -dir);
        const uint32_t centre = s.vert({0.0, 0.0, q.x}, nrm, {0.0, 0.0}, c.material);
        for (size_t k = 0; k + 1 < az.size(); ++k)
        {
            const glm::dvec3 pa(q.y * std::cos(az[k]), q.y * std::sin(az[k]), q.x);
            const glm::dvec3 pb(q.y * std::cos(az[k + 1]), q.y * std::sin(az[k + 1]), q.x);
            const uint32_t ia = s.vert(pa, nrm, {pa.x, pa.y}, c.material), ib = s.vert(pb, nrm, {pb.x, pb.y}, c.material);
            s.tri(centre, ia, ib, nrm);
        }
    }
}

// A fin (PrimitiveKind::Fin): flat-shaded triangles (bevel bands taper, so are not planar), UV = the local
// (x, z) in metres on the sides, (y, z) on the end faces.
void meshFin(MeshSink &s, const SatFinShape &f, int mat, int backMat)
{
    glm::dvec3 p[12];
    satFinCorners(f, p);
    auto flat = [&](glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 outward, bool endFace, int mat) {
        glm::dvec3 nn = glm::cross(b - a, c - a);
        if (glm::length(nn) < 1e-12)
            return;
        nn = glm::normalize(nn);
        if (glm::dot(nn, outward) < 0.0)
            nn = -nn;
        auto uv = [&](glm::dvec3 q) { return endFace ? glm::dvec2(q.y, q.z) : glm::dvec2(q.x, q.z); };
        const uint32_t ia = s.vert(a, nn, uv(a), mat), ib = s.vert(b, nn, uv(b), mat), ic = s.vert(c, nn, uv(c), mat);
        s.tri(ia, ib, ic, nn);
    };
    for (int k = 0; k < 6; ++k)
    {
        const int k1 = (k + 1) % 6;
        const glm::dvec3 e = (p[k1] - p[k]) + (p[6 + k1] - p[6 + k]);
        const glm::dvec3 out(0.0, -e.z, e.y);
        const int m = out.y < 0.0 ? backMat : mat; // the -Y side may be another material (a flap's tiled face)
        flat(p[k], p[k1], p[6 + k1], out, false, m);
        flat(p[k], p[6 + k1], p[6 + k], out, false, m);
    }
    for (int end = 0; end < 2; ++end)
        for (int k = 1; k + 1 < 6; ++k)
            flat(p[6 * end], p[6 * end + k], p[6 * end + k + 1], glm::dvec3(end ? 1.0 : -1.0, 0, 0), true, mat);
}
} // namespace

SatRenderMesh buildSatRenderMesh(const SatModel &m, int segments)
{
    segments = std::max(8, segments);
    // Group origins in REST coordinates: a child's hinge point accumulates down the tree (as in
    // tessellateSatModel).
    std::vector<glm::dvec3> origin(m.groups.size(), glm::dvec3(0.0));
    for (size_t gi = 0; gi < m.groups.size(); ++gi)
        if (m.groups[gi].parent >= 0)
            origin[gi] = origin[m.groups[gi].parent] + glm::dvec3(m.groups[gi].hingePos);

    SatRenderMesh mesh;
    for (size_t ci = 0; ci < m.components.size(); ++ci)
    {
        const SatComponent &c = m.components[ci];
        const size_t firstVert = mesh.vertices.size(), firstIdx = mesh.indices.size();
        MeshSink s{mesh, glm::dmat3(glm::mat3_cast(c.rotation)), origin[c.group] + glm::dvec3(c.position),
                   c.group, (int)ci};
        switch (c.prim)
        {
        case PrimitiveKind::Plane:
        {
            const double w = 0.5 * c.size.x, h = 0.5 * c.size.y;
            const glm::dvec3 a(-w, -h, 0), b(w, -h, 0), cc(w, h, 0), d(-w, h, 0);
            s.quad(a, b, cc, d, glm::dvec3(0, 0, 1), c.material);
            if (!c.singleSided)
                s.quad(a, b, cc, d, glm::dvec3(0, 0, -1), c.backMaterial >= 0 ? c.backMaterial : c.material);
            break;
        }
        case PrimitiveKind::Box:
        {
            const glm::dvec3 e = 0.5 * glm::dvec3(c.size);
            for (int axis = 0; axis < 3; ++axis)
                for (int sign = -1; sign <= 1; sign += 2)
                {
                    glm::dvec3 n(0.0);
                    n[axis] = sign;
                    const int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    glm::dvec3 p[4];
                    const double su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
                    for (int k = 0; k < 4; ++k)
                    {
                        p[k][axis] = sign * e[axis];
                        p[k][u] = su[k] * e[u];
                        p[k][v] = sv[k] * e[v];
                    }
                    s.quad(p[0], p[1], p[2], p[3], n, c.material);
                }
            break;
        }
        case PrimitiveKind::Cylinder:
            meshRevolved(s, c.radius, c.radius, c.height, c.caps, c.material, segments);
            break;
        case PrimitiveKind::Cone:
            meshRevolved(s, c.radius, c.radiusTop, c.height, c.caps, c.material, segments);
            break;
        case PrimitiveKind::Sphere:
            meshSphere(s, c.radius, c.material, segments);
            break;
        case PrimitiveKind::Profile:
            meshProfile(s, c, segments);
            break;
        case PrimitiveKind::Fin:
            meshFin(s, c.fin, c.material, c.backMaterial >= 0 ? c.backMaterial : c.material);
            break;
        }
        // An open lattice (SatMaterial::coverage < 1) of a closed primitive: its far side shows
        // through the near side's gaps, so every face is drawn from the inside too (same positions,
        // reversed winding and normal). Back faces are culled, so each copy is drawn only from its
        // own side.
        if (c.prim != PrimitiveKind::Plane && m.materials[c.material].coverage < 1.0f)
        {
            const size_t v1 = mesh.vertices.size(), i1 = mesh.indices.size();
            for (size_t v = firstVert; v < v1; ++v)
            {
                SatMeshVertex iv = mesh.vertices[v];
                iv.normal = -iv.normal;
                mesh.vertices.push_back(iv);
            }
            const uint32_t shift = (uint32_t)(v1 - firstVert);
            for (size_t i = firstIdx; i < i1; i += 3)
            {
                const uint32_t a = mesh.indices[i] + shift, b = mesh.indices[i + 1] + shift,
                               cc = mesh.indices[i + 2] + shift;
                mesh.indices.insert(mesh.indices.end(), {a, cc, b});
            }
        }
    }

    // Bounding sphere: centre of the vertex AABB, radius to the farthest vertex.
    if (!mesh.vertices.empty())
    {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const SatMeshVertex &v : mesh.vertices)
        {
            lo = glm::min(lo, v.pos);
            hi = glm::max(hi, v.pos);
        }
        mesh.boundsCenter = 0.5f * (lo + hi);
        for (const SatMeshVertex &v : mesh.vertices)
            mesh.boundsRadius = std::max(mesh.boundsRadius, glm::length(v.pos - mesh.boundsCenter));
    }
    return mesh;
}

GpuSatMeshMaterial packSatMeshMaterial(const SatMaterial &m)
{
    GpuSatMeshMaterial g{};
    g.color = m.color;
    g.albedo = m.diffuseAlbedo;
    g.f0 = m.specularF0;
    g.roughness = m.roughness;
    g.beckmann = (m.beckmann ? 1u : 0u) | (satMinnaertCode(m.minnaertK) << 1); // bits 1-7: Minnaert (0 = Lambert)
    g.pattern = (uint32_t)satMaterialPattern(m);
    // The haze (2026-10-21, SatMaterial::hazeFraction): its fraction and α as unorm16 in the two words' high halves
    // (sat_mesh.frag decodes them). 0 without haze, so the words are what they always were.
    if (m.hazeFraction > 0.0f)
    {
        g.beckmann |= (uint32_t)std::lround(std::clamp(m.hazeFraction, 0.0f, 1.0f) * 65535.0f) << 16;
        g.pattern |= (uint32_t)std::lround(std::clamp(m.hazeRoughness, 0.0f, 1.0f) * 65535.0f) << 16;
    }
    // Phase 4f: rgb = transmission tint normalised to luminance 1 (so `a` is the V-band value the
    // photometry uses), a = transmission.
    const float lum = std::max(0.2126f * m.transmissionColor.r + 0.7152f * m.transmissionColor.g +
                                   0.0722f * m.transmissionColor.b, 1e-4f);
    g.extra = glm::vec4(m.transmissionColor / lum, m.transmission);
    // Truss: the member width (fraction of a bay) whose drawn area fraction is `coverage` — members
    // on the bay grid in both directions, 1 − (1 − x)², plus one diagonal per bay across the open
    // square, √2·x·(1 − x).
    float x = 0.0f;
    // A rocket's grid fin (2026-10-21): a square grid of plates with no diagonals, 1 - (1 - x)^2 = coverage.
    const bool gridFin = m.name == "gridfin";
    if (m.coverage < 1.0f && gridFin)
        x = 1.0f - std::sqrt(std::max(1.0f - m.coverage, 0.0f));
    else if (m.coverage < 1.0f)
    {
        float lo = 0.0f, hi = 1.0f;
        for (int it = 0; it < 40; ++it)
        {
            const float mid = 0.5f * (lo + hi);
            const float f = 1.0f - (1.0f - mid) * (1.0f - mid) + 1.41421356f * mid * (1.0f - mid);
            (f < m.coverage ? lo : hi) = mid;
        }
        x = 0.5f * (lo + hi);
    }
    // w = 1: an engine's inside (the rocket models' "soot", tools/make_rocket_models.py), which glows while the
    // stage burns (GpuMeshInstance::earthShC.w, sat_mesh.frag; 2026-10-20).
    // w = 2: a grid fin (sat_mesh.frag: the grid without diagonals, drawn solid at its mean once the plates are
    // finer than a pixel).
    g.lattice = glm::vec4(m.coverage, m.trussPitch, x, m.name == "soot" ? 1.0f : gridFin ? 2.0f : 0.0f);
    return g;
}

std::vector<GpuSatMeshOccluder> packSatMeshOccluders(const SatOcclusion &occ)
{
    std::vector<GpuSatMeshOccluder> out;
    out.reserve(occ.occluders.size());
    for (const SatOccluder &o : occ.occluders)
    {
        GpuSatMeshOccluder g{};
        g.center = glm::vec3(o.center);
        g.kind = o.blocks ? (uint32_t)o.kind : 0xFFu; // 0xFF: an open lattice, skipped by sat_mesh.frag
        g.half = glm::vec3(o.half);
        g.group = (uint32_t)o.group;
        g.axisX = glm::vec4(glm::vec3(o.axes[0]), 0.0f);
        g.axisY = glm::vec4(glm::vec3(o.axes[1]), 0.0f);
        g.axisZ = glm::vec4(glm::vec3(o.axes[2]), 0.0f);
        out.push_back(g);
    }
    return out;
}
