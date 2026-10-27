#include "Refraction.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace refr
{
namespace
{
constexpr double kR = 6371000.0;          // R_EARTH (common.glsl)
constexpr double kG = 9.80665, kRd = 287.053;
constexpr double kTop = 100000.0;          // above this the air bends nothing measurable
constexpr double kNfac = 77.6 * 1.02486;   // N = kNfac P / T at 550 nm

double dispAt(double lambdaUm) { return (1.0 + 0.00752 / (lambdaUm * lambdaUm)) / 1.02486; }
} // namespace

const double kDisp[3] = {dispAt(0.610), dispAt(0.545), dispAt(0.465)};

double saemundsson(double eApp)
{
    const double h = std::max(eApp * 57.29577951308232, -1.0);
    return (1.02 / std::tan((h + 10.3 / (h + 5.11)) / 57.29577951308232)) / 60.0 / 57.29577951308232;
}

double Table::eAt(int i) const
{
    const double u = (double)i / (double)(kTableN - 1);
    return eC + c * std::sinh(alpha * (u - u0));
}
double Table::uOf(double eApp) const { return u0 + std::asinh((eApp - eC) / c) / alpha; }

namespace
{
// The background temperature (an ICAO-like standard atmosphere through T0 zRef above the surface) and the pressure.
struct Air
{
    const Profile &p;
    double Tsl;   // the background at sea level
    double lnL;   // ln(1 + zRef / z0)
    explicit Air(const Profile &pr) : p(pr)
    {
        Tsl = p.T0 + 0.0065 * (p.surfaceZ + p.zRef);
        lnL = std::log(1.0 + p.zRef / p.z0);
    }
    void bg(double z, double &T, double &dT) const
    {
        const double T11 = Tsl - 0.0065 * 11000.0;
        if (z < 11000.0) { T = Tsl - 0.0065 * z; dT = -0.0065; }
        else if (z < 20000.0) { T = T11; dT = 0.0; }
        else if (z < 32000.0) { T = T11 + 0.001 * (z - 20000.0); dT = 0.001; }
        else if (z < 47000.0) { T = T11 + 12.0 + 0.0028 * (z - 32000.0); dT = 0.0028; }
        else { T = T11 + 54.0; dT = 0.0; }
    }
    // Pressure (hPa) from the background by the barometric formula, layer by layer.
    double P(double z) const
    {
        const double Tsl_ = Tsl, T11 = Tsl - 0.0065 * 11000.0;
        const double ex = kG / (kRd * 0.0065);
        if (z < 11000.0) return p.P0 * std::pow((Tsl_ - 0.0065 * z) / Tsl_, ex);
        const double P11 = p.P0 * std::pow(T11 / Tsl_, ex);
        if (z < 20000.0) return P11 * std::exp(-kG * (z - 11000.0) / (kRd * T11));
        const double P20 = P11 * std::exp(-kG * 9000.0 / (kRd * T11));
        if (z < 32000.0) return P20 * std::pow((T11 + 0.001 * (z - 20000.0)) / T11, -kG / (kRd * 0.001));
        const double P32 = P20 * std::pow((T11 + 12.0) / T11, -kG / (kRd * 0.001));
        if (z < 47000.0) return P32 * std::pow((T11 + 12.0 + 0.0028 * (z - 32000.0)) / (T11 + 12.0), -kG / (kRd * 0.0028));
        const double P47 = P32 * std::pow((T11 + 54.0) / (T11 + 12.0), -kG / (kRd * 0.0028));
        return P47 * std::exp(-kG * (z - 47000.0) / (kRd * (T11 + 54.0)));
    }
    // N (refractivity, 550 nm) and dN/dz at height z (m above sea level).
    void N(double z, double &Nz, double &dNz) const
    {
        double T, dT;
        bg(z, T, dT);
        const double zs = z - p.surfaceZ;
        if (p.dTs != 0.0 && zs < 3000.0)
        {
            const double zz = std::max(zs, 0.0) + p.z0;
            T += p.dTs * std::log(1.0 + p.zRef / zz) / lnL;
            dT += -p.dTs * (p.zRef / (zz * (zz + p.zRef))) / lnL;
        }
        if (p.invDT != 0.0)
        {
            const double x = (zs - p.invZ) / std::max(p.invDz, 0.1);
            if (x > -6.0 && x < 6.0)
            {
                const double th = std::tanh(2.0 * x);
                T += p.invDT * 0.5 * (1.0 + th);
                dT += p.invDT * (1.0 - th * th) / std::max(p.invDz, 0.1);
            }
            else if (x >= 6.0)
                T += p.invDT;
        }
        if (z > kTop) { Nz = 0.0; dNz = 0.0; return; }
        const double Pz = P(z);
        Nz = kNfac * Pz / T;
        dNz = Nz * (-kG / (kRd * T) - dT / T);
    }
    // The local vertical length scale the profile varies on (the step's resolution).
    double scale(double z) const
    {
        const double zs = z - p.surfaceZ;
        double L = (z < 20000.0) ? 1500.0 : 15000.0;
        if (p.dTs != 0.0 && zs < 3000.0)
            L = std::min(L, std::max(zs, 0.0) + 0.004);
        if (p.invDT != 0.0 && std::abs(zs - p.invZ) < 4.0 * p.invDz)
            L = std::min(L, 0.2 * std::max(p.invDz, 0.1));
        return L;
    }
    double cn2(double z) const
    {
        const double zs = std::max(z - p.surfaceZ, 0.05);
        const double surf = p.cnSurf * std::pow(zs, -4.0 / 3.0) * std::exp(-zs / 1000.0);
        const double hv = 2.7e-16 * std::exp(-z / 1500.0) + 3.59e-53 * std::pow(z * 1e-3, 10.0) * 1e30 * std::exp(-z / 1000.0);
        return surf + p.cnFree * hv;
    }
};

struct RayOut
{
    bool hit = false;
    double eTrue = 0.0;
    double colR = 0, colM = 0, colO3 = 0, colH = 0, I1 = 0, I2 = 0;
};

// Traces one ray back from the observer (h0 above sea level) at apparent elevation e0, with (n - 1) x f.
RayOut trace(const Air &air, double h0, double e0, double f, bool columns)
{
    RayOut o;
    double px = 0.0, py = kR + h0;
    double dx = std::cos(e0), dy = std::sin(e0);
    double s = 0.0;
    // From above the air: straight to its top (or past the Earth).
    if (h0 > kTop)
    {
        const double b = px * dx + py * dy, cc = (px * px + py * py) - (kR + kTop) * (kR + kTop);
        const double disc = b * b - cc;
        if (disc <= 0.0 || -b - std::sqrt(disc) < 0.0)
        {
            o.eTrue = e0;
            return o;
        }
        const double t = -b - std::sqrt(disc) + 1.0;
        px += dx * t; py += dy * t; s += t;
    }
    auto grad = [&](double x, double y, double &gx, double &gy, double &z, double &nn, double &dn) {
        const double r = std::sqrt(x * x + y * y);
        z = r - kR;
        double Nz, dNz;
        air.N(z, Nz, dNz);
        nn = 1.0 + f * Nz * 1e-6;
        dn = f * dNz * 1e-6;
        gx = dn / nn * x / r;
        gy = dn / nn * y / r;
    };
    for (int it = 0; it < 40000; ++it)
    {
        double gx, gy, z, nn, dn;
        grad(px, py, gx, gy, z, nn, dn);
        const double r = z + kR;
        const double sinE = (px * dx + py * dy) / r;
        if (z < air.p.surfaceZ) { o.hit = true; return o; }
        if (z > kTop && sinE > 0.0) break;
        if (s > 4.0e6) { o.hit = true; return o; }   // trapped in a duct
        const double L = air.scale(z);
        const double kap = std::abs(dn) + 1.0 / kR;
        double ds = std::min(0.15 * L / std::max(std::abs(sinE), 1e-7), std::sqrt(0.3 * L / kap));
        ds = std::min(ds, z < 20000.0 ? 2000.0 : 20000.0);
        ds = std::max(ds, 0.005);
        // midpoint step of dd/ds = g - (g.d) d
        const double g0 = gx * dx + gy * dy;
        double mdx = dx + 0.5 * ds * (gx - g0 * dx), mdy = dy + 0.5 * ds * (gy - g0 * dy);
        double ml = std::sqrt(mdx * mdx + mdy * mdy); mdx /= ml; mdy /= ml;
        const double mx = px + 0.5 * ds * dx, my = py + 0.5 * ds * dy;
        double gmx, gmy, zm, nm, dm;
        grad(mx, my, gmx, gmy, zm, nm, dm);
        const double gm = gmx * mdx + gmy * mdy;
        double ndx = dx + ds * (gmx - gm * mdx), ndy = dy + ds * (gmy - gm * mdy);
        const double nl = std::sqrt(ndx * ndx + ndy * ndy);
        px += ds * mdx; py += ds * mdy;
        dx = ndx / nl; dy = ndy / nl;
        if (columns)
        {
            o.colR += std::exp(-zm / 7994.0) * ds;
            o.colM += std::exp(-zm / 1200.0) * ds;
            o.colH += std::exp(-zm / 2500.0) * ds;
            if (zm > 15000.0 && zm < 35000.0) o.colO3 += ds / 20000.0;
            if (zm < 25000.0)
            {
                const double c2 = air.cn2(zm);
                const double sm = s + 0.5 * ds;
                o.I1 += c2 * ds;
                o.I2 += c2 * std::pow(sm, 5.0 / 6.0) * ds;
            }
        }
        s += ds;
    }
    o.eTrue = std::atan2(dy, dx);   // the direction it came from, in the observer's frame (x = the horizontal)
    return o;
}
} // namespace

void build(const Profile &p, double h0, Table &out, int threads)
{
    Table t;
    t.h0 = h0;
    const double r0 = kR + std::max(h0, 0.0);
    const double eLimb = -std::acos(std::min(kR / r0, 1.0));
    const double eLo = eLimb - 0.006, eHi = eLimb + 0.07;
    t.eC = eLimb + 0.0006;
    t.c = 8e-4;
    const double aLo = std::asinh((eLo - t.eC) / t.c), aHi = std::asinh((eHi - t.eC) / t.c);
    t.alpha = aHi - aLo;
    t.u0 = -aLo / t.alpha;
    t.e.resize(kTableN);
    const Air air(p);
    const double D = 0.01;   // the "aperture" the jitter is seen with (m)
    const double k76 = std::pow(2.0 * 3.14159265358979 / 550e-9, 7.0 / 6.0);
    std::atomic<int> next{0};
    auto run = [&] {
        for (int i = next.fetch_add(1); i < kTableN; i = next.fetch_add(1))
        {
            const double e0 = t.eAt(i);
            Entry &en = t.e[i];
            for (int c = 0; c < 3; ++c)
            {
                const double f = 1.0 + (kDisp[c] - 1.0) * p.dispScale;
                const RayOut ro = trace(air, h0, e0, f, c == 1);
                en.dR[c] = ro.hit ? kHit : (float)(e0 - ro.eTrue);
                if (c == 1)
                {
                    en.colR = (float)ro.colR;
                    en.colM = (float)ro.colM;
                    en.colO3 = (float)ro.colO3;
                    en.colH = (float)ro.colH;
                    en.aoa = (float)std::sqrt(2.91 * std::pow(D, -1.0 / 3.0) * ro.I1);
                    const double sr2 = 2.25 * k76 * ro.I2;
                    en.scint = (float)std::sqrt(1.0 - std::exp(-sr2));
                }
            }
        }
    };
    const int nt = std::max(1, threads);
    std::vector<std::thread> th;
    for (int k = 1; k < nt; ++k)
        th.emplace_back(run);
    run();
    for (auto &x : th)
        x.join();
    {
        const RayOut z = trace(air, h0, 1.5707, 1.0, true);
        t.zenAoa2 = 2.91 * std::pow(D, -1.0 / 3.0) * z.I1;
        t.zenSr2 = 2.25 * k76 * z.I2;
    }
    // Continuation above the table: the refraction there x Saemundsson's shape (0 from orbit).
    const Entry &top = t.e[kTableN - 1];
    const double sTop = saemundsson(eHi);
    t.topScale = (top.dR[1] < kHit && sTop > 1e-9) ? top.dR[1] / sTop : 0.0;
    t.valid = true;
    out = std::move(t);
}

bool lookup(const Table &t, double eApp, double &dRg, Entry *outI)
{
    if (!t.valid || t.e.empty())
    {
        dRg = eApp >= 0.0 ? saemundsson(eApp) : 0.0;
        return false;
    }
    const double u = t.uOf(eApp);
    if (u >= 1.0)
    {
        dRg = t.topScale * saemundsson(eApp);
        if (outI)
        {
            Entry m = t.e[kTableN - 1];
            const double se = std::max(std::sin(eApp), 1e-3);
            m.dR[0] = m.dR[1] = m.dR[2] = (float)dRg;
            m.aoa = (float)std::sqrt(t.zenAoa2 / se);
            m.scint = (float)std::sqrt(1.0 - std::exp(-t.zenSr2 * std::pow(se, -11.0 / 6.0)));
            *outI = m;
        }
        return false;
    }
    const double fi = std::max(u, 0.0) * (kTableN - 1);
    const int i = std::min((int)fi, kTableN - 2);
    const double w = std::clamp(fi - i, 0.0, 1.0);
    const Entry &a = t.e[i], &b = t.e[i + 1];
    if (a.dR[1] >= kHit || b.dR[1] >= kHit)
    {
        const Entry &n = (w < 0.5) ? a : b;
        dRg = n.dR[1];
        if (outI) *outI = n;
        return n.dR[1] >= kHit;
    }
    dRg = a.dR[1] + (b.dR[1] - a.dR[1]) * w;
    if (outI)
    {
        Entry m;
        for (int c = 0; c < 3; ++c)
            m.dR[c] = (float)(a.dR[c] + (b.dR[c] - a.dR[c]) * w);
        m.aoa = (float)(a.aoa + (b.aoa - a.aoa) * w);
        m.colR = (float)(a.colR + (b.colR - a.colR) * w);
        m.colM = (float)(a.colM + (b.colM - a.colM) * w);
        m.colO3 = (float)(a.colO3 + (b.colO3 - a.colO3) * w);
        m.colH = (float)(a.colH + (b.colH - a.colH) * w);
        m.scint = (float)(a.scint + (b.scint - a.scint) * w);
        *outI = m;
    }
    return false;
}
} // namespace refr
