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
    g.beckmann = m.beckmann ? 1u : 0u;
    g.pattern = (uint32_t)satMaterialPattern(m);
    // Phase 4f: rgb = transmission tint normalised to luminance 1 (so `a` is the V-band value the
    // photometry uses), a = transmission.
    const float lum = std::max(0.2126f * m.transmissionColor.r + 0.7152f * m.transmissionColor.g +
                                   0.0722f * m.transmissionColor.b, 1e-4f);
    g.extra = glm::vec4(m.transmissionColor / lum, m.transmission);
    // Truss: the member width (fraction of a bay) whose drawn area fraction is `coverage` — members
    // on the bay grid in both directions, 1 − (1 − x)², plus one diagonal per bay across the open
    // square, √2·x·(1 − x).
    float x = 0.0f;
    if (m.coverage < 1.0f)
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
    g.lattice = glm::vec4(m.coverage, m.trussPitch, x, 0.0f);
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
