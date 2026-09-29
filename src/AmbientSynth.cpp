// AmbientSynth.cpp — procedural ambience voices. See AmbientSynth.h for the threading contract.
//
// Building blocks (all per-sample, all allocation-free):
//   Svf    — Andrew Simper's trapezoidal state-variable filter. Stable under per-block modulation,
//            which is the whole point here (gusts sweep a band-pass continuously). `k * bp` is the
//            unity-peak band-pass.
//   Pink   — Paul Kellet's economy pink filter on white noise.
//   Drift  — a smooth random walk in [0, 1]: cosine interpolation between random targets whose
//            spacing is random around 1/rate. Gusts, pitch drift, chorus clustering.
// Levels were set by rendering each synth alone through the harness (tools/harness/scripts/
// ambience_solos.satcmd + imgtools.py audio): at level 1, layer gain 1 and the default ambience
// volume, every kind measures roughly -24 LUFS (ungated) — the table's gains then set the mix.
#include "AmbientSynth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530717959f;

inline float rnd(uint32_t &s)
{
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return (float)(s >> 8) * (1.0f / 16777216.0f);
}

struct Svf
{
    float ic1 = 0, ic2 = 0, a1 = 0, a2 = 0, a3 = 0, k = 1;
    float lp = 0, bp = 0, hp = 0;
    void set(float fc, float q, float sr)
    {
        fc = std::clamp(fc, 10.0f, 0.45f * sr);
        const float g = tanf(kPi * fc / sr);
        k = 1.0f / std::max(q, 0.05f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
    void tick(float v0)
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        lp = v2;
        bp = v1;
        hp = v0 - k * v1 - v2;
    }
    float band() const { return k * bp; } // unity gain at the centre frequency
};

struct Pink
{
    float b0 = 0, b1 = 0, b2 = 0;
    float tick(float w)
    {
        b0 = 0.99765f * b0 + w * 0.0990460f;
        b1 = 0.96300f * b1 + w * 0.2965164f;
        b2 = 0.57000f * b2 + w * 1.0526913f;
        return (b0 + b1 + b2 + w * 0.1848f) * 0.3f;
    }
};

struct Brown
{
    float z = 0;
    float tick(float w)
    {
        z = z * 0.996f + w * 0.06f;
        return z * 1.6f;
    }
};

struct Drift
{
    float a = 0.5f, b = 0.5f, t = 1.0f, dur = 1.0f;
    float tick(float dtS, float rate, uint32_t &s)
    {
        t += dtS;
        if (t >= dur)
        {
            a = b;
            b = rnd(s);
            t = 0.0f;
            dur = (0.5f + rnd(s)) / std::max(rate, 1e-3f);
        }
        const float x = t / dur;
        return a + (b - a) * (0.5f - 0.5f * cosf(kPi * x));
    }
};

// Constant-power pan, p in [-1, 1].
inline void panGains(float p, float &l, float &r)
{
    const float a = (std::clamp(p, -1.0f, 1.0f) + 1.0f) * (kPi * 0.25f);
    l = cosf(a);
    r = sinf(a);
}

// Colour: 0 white, 0.5 pink, 1 brown.
inline float colour(float c, float w, float pk, float br)
{
    if (c < 0.5f)
        return w + (pk - w) * (c * 2.0f);
    return pk + (br - pk) * (c * 2.0f - 1.0f);
}

// Stereo flanger: a short delay swept by a slow LFO (left and right in quadrature) with feedback,
// mixed back over the dry signal — the comb-filter "jet" sweep.
struct Flanger
{
    static constexpr int kLen = 2048; // 42 ms at 48 kHz, a power of two
    float bufL[kLen] = {}, bufR[kLen] = {};
    int w = 0;
    float ph = 0.0f;
    float read(const float *b, float d) const
    {
        float p = (float)w - d;
        while (p < 0.0f)
            p += (float)kLen;
        const int i0 = (int)p;
        const float f = p - (float)i0;
        return b[i0 & (kLen - 1)] * (1.0f - f) + b[(i0 + 1) & (kLen - 1)] * f;
    }
    void process(float &l, float &r, float sr, float rateHz, float maxMs, float fb, float mix)
    {
        ph += kTwoPi * rateHz / sr;
        if (ph > kTwoPi)
            ph -= kTwoPi;
        const float span = std::clamp(maxMs, 0.1f, 15.0f) * 0.001f * sr;
        const float base = 0.0003f * sr;
        const float yl = read(bufL, base + span * (0.5f + 0.5f * sinf(ph)));
        const float yr = read(bufR, base + span * (0.5f + 0.5f * cosf(ph)));
        fb = std::clamp(fb, -0.9f, 0.9f);
        bufL[w] = l + yl * fb;
        bufR[w] = r + yr * fb;
        w = (w + 1) & (kLen - 1);
        l = (l + mix * yl) / (1.0f + 0.5f * mix);
        r = (r + mix * yr) / (1.0f + 0.5f * mix);
    }
};

// Stereo phaser: six first-order all-passes whose break frequency an LFO sweeps between lo and hi
// (log-spaced), with feedback. The coefficient is set once per block — cheap, and inaudible.
struct Phaser
{
    static constexpr int kStages = 6;
    float zl[kStages] = {}, zr[kStages] = {};
    float yl = 0.0f, yr = 0.0f, al = 0.0f, ar = 0.0f, ph = 0.0f;
    static float coef(float f, float sr)
    {
        const float t = tanf(kPi * std::clamp(f, 20.0f, 0.45f * sr) / sr);
        return (t - 1.0f) / (t + 1.0f);
    }
    void setBlock(float dtS, float sr, float rateHz, float depth, float lo, float hi)
    {
        ph += kTwoPi * rateHz * dtS;
        if (ph > kTwoPi)
            ph -= kTwoPi;
        const float d = std::clamp(depth, 0.0f, 1.0f);
        auto sweep = [&](float s) { return lo * powf(hi / lo, 0.5f + 0.5f * d * s); };
        al = coef(sweep(sinf(ph)), sr);
        ar = coef(sweep(cosf(ph)), sr);
    }
    static float chain(float x, float a, float *z)
    {
        for (int k = 0; k < kStages; ++k)
        {
            const float y = a * x + z[k];
            z[k] = x - a * y;
            x = y;
        }
        return x;
    }
    void process(float &l, float &r, float fb, float mix)
    {
        fb = std::clamp(fb, -0.85f, 0.85f);
        yl = chain(l + yl * fb, al, zl);
        yr = chain(r + yr * fb, ar, zr);
        l = (l + mix * yl) / (1.0f + mix);
        r = (r + mix * yr) / (1.0f + mix);
    }
};

// Easing coefficient for a per-block one-pole toward a target with time constant tau.
inline float blockEase(float frames, float sr, float tau) { return 1.0f - expf(-frames / (sr * tau)); }

// Band-limited harmonic wavetable: `harm` partials whose amplitudes fall as h^-bright (even ones
// scaled by 1-odd), normalised to unit peak, linearly interpolated. bright = 1 and harm at Nyquist
// IS a saw; the drone uses it for its organ-like series and the saw stack for a warm saw stack.
// Rebuild is the expensive part (harm x kTab sines), so the shape is cached and only rebuilt when a
// shape parameter actually moves — same rule both callers used separately before this was shared.
struct Wavetable
{
    static constexpr int kTab = 2048;
    float tab[kTab] = {};
    int harm = -1;
    float bright = -1.0f, odd = -1.0f;

    void rebuild(int h, float br, float od)
    {
        float peak = 1e-6f;
        for (int i = 0; i < kTab; ++i)
        {
            const float x = kTwoPi * (float)i / (float)kTab;
            float v = 0.0f;
            for (int k = 1; k <= h; ++k)
            {
                const float a = powf((float)k, -br) * ((k % 2 == 0) ? (1.0f - od) : 1.0f);
                v += a * sinf((float)k * x);
            }
            tab[i] = v;
            peak = std::max(peak, std::fabs(v));
        }
        for (float &v : tab)
            v /= peak;
        harm = h;
        bright = br;
        odd = od;
    }

    void ensure(int h, float br, float od)
    {
        if (h != harm || std::fabs(br - bright) > 0.01f || std::fabs(od - odd) > 0.01f)
            rebuild(h, br, od);
    }

    float lookup(float ph) const
    {
        const float p = ph * (float)kTab;
        const int i0 = (int)p;
        const float f = p - (float)i0;
        return tab[i0 & (kTab - 1)] * (1.0f - f) + tab[(i0 + 1) & (kTab - 1)] * f;
    }
};

// ── wind / air ───────────────────────────────────────────────────────────────────────────────────
// Wind is band-limited noise whose loudness AND pitch rise together in a gust. Four parts:
//   band    coloured noise through two cascaded band-passes (12 dB/oct skirts, the "whoosh"); a
//           gust raises its level strongly and slides its centre up
//   rumble  a little low-passed brown noise (the pressure buffeting a microphone would add)
//   hiss    high-passed white noise growing with the square of the gust (grit, grass, snow)
//   whistle a narrow band that only speaks near gust peaks (edges of rock and ice)
// The output is high-passed at 45 Hz: sub-bass under the music costs headroom and reads as mud.
// Gusts: two random walks (a slow swell and faster puffs) plus a flutter a few times per second.
// One generator, parameterised: desert, alpine, glacier, jet-stream roar, the thin stratosphere.
const AmbientSynth::ParamDef kWindParams[] = {
    {"level", 1.0f},     {"color", 0.4f},     {"center_hz", 500.0f}, {"q", 0.9f},
    {"body", 0.15f},     {"body_hz", 140.0f}, {"gust", 0.7f},        {"gust_rate", 0.35f},
    {"whistle", 0.0f},   {"whistle_hz", 900.0f}, {"width", 0.7f},    {"hiss", 0.25f},
    {"hiss_hz", 3500.0f}, {"flutter", 0.25f},
};
enum
{
    kWLevel,
    kWColor,
    kWCenter,
    kWQ,
    kWBody,
    kWBodyHz,
    kWGust,
    kWGustRate,
    kWWhistle,
    kWWhistleHz,
    kWWidth,
    kWHiss,
    kWHissHz,
    kWFlutter
};

class WindSynth final : public AmbientSynth
{
public:
    WindSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("wind", kWindParams, (int)(sizeof(kWindParams) / sizeof(kWindParams[0])), sr, seed)
    {
    }

protected:
    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float rate = std::max(p_[kWGustRate], 0.01f);
        const float g = std::clamp(0.55f * slow_.tick(bt, rate * 0.4f, rng_) + 0.45f * fast_.tick(bt, rate * 1.6f, rng_),
                                   0.0f, 1.0f);
        const float depth = std::clamp(p_[kWGust], 0.0f, 1.0f);
        const float flutter = 1.0f + std::clamp(p_[kWFlutter], 0.0f, 1.0f) * depth * 0.6f *
                                         (flut_.tick(bt, 4.0f, rng_) - 0.5f);
        // Gusts are a swing in DECIBELS (+-15 dB at full depth, around g = 0.45 so peaks overshoot):
        // loudness is perceived logarithmically, and a linear swing read as a steady hiss.
        const float gustTarget = exp2f(depth * 5.0f * (g - 0.45f)) * flutter;
        // ... and the band slides +-0.7 octave with them: a gust is higher as well as louder.
        const float fc = p_[kWCenter] * exp2f(1.4f * (g - 0.45f));
        bandL_.set(fc, p_[kWQ], sr_);
        bandL2_.set(fc * 1.1f, p_[kWQ], sr_);
        bandR_.set(fc * 1.06f, p_[kWQ], sr_);
        bandR2_.set(fc * 1.16f, p_[kWQ], sr_);
        bodyL_.set(p_[kWBodyHz], 0.7f, sr_);
        bodyR_.set(p_[kWBodyHz] * 0.93f, 0.7f, sr_);
        hissL_.set(p_[kWHissHz] * (0.8f + 0.5f * g), 0.7f, sr_);
        hissR_.set(p_[kWHissHz] * (0.84f + 0.5f * g), 0.7f, sr_);
        hpL_.set(45.0f, 0.7f, sr_);
        hpR_.set(45.0f, 0.7f, sr_);
        const float wf = p_[kWWhistleHz] * (0.8f + 0.4f * g);
        whL_.set(wf, 28.0f, sr_);
        whR_.set(wf * 1.013f, 28.0f, sr_);
        const float whTarget = p_[kWWhistle] * smooth(0.45f, 0.9f, g) * 2.0f;
        const float hissTarget = p_[kWHiss] * (0.15f + 1.4f * g * g);
        const float w = std::clamp(p_[kWWidth], 0.0f, 1.0f);
        const float norm = 1.0f / sqrtf((1.0f - w) * (1.0f - w) + w * w);
        const float body = std::clamp(p_[kWBody], 0.0f, 1.0f);
        const float c = std::clamp(p_[kWColor], 0.0f, 1.0f);
        const float lvl = p_[kWLevel] * 0.33f;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float gust = gustPrev_ + (gustTarget - gustPrev_) * x;
            const float wh = whPrev_ + (whTarget - whPrev_) * x;
            const float hs = hissPrev_ + (hissTarget - hissPrev_) * x;
            const float wc = bip();
            const float nl = (wc * (1.0f - w) + bip() * w) * norm;
            const float nr = (wc * (1.0f - w) + bip() * w) * norm;
            const float bl = brownL_.tick(nl), br = brownR_.tick(nr);
            const float cl = colour(c, nl, pinkL_.tick(nl), bl);
            const float cr = colour(c, nr, pinkR_.tick(nr), br);
            bandL_.tick(cl);
            bandL2_.tick(bandL_.band());
            bandR_.tick(cr);
            bandR2_.tick(bandR_.band());
            bodyL_.tick(bl);
            bodyR_.tick(br);
            hissL_.tick(nl);
            hissR_.tick(nr);
            whL_.tick(nl);
            whR_.tick(nr);
            hpL_.tick((bandL2_.band() * 1.6f + bodyL_.lp * body) * gust + hissL_.hp * hs * 0.5f + whL_.band() * wh);
            hpR_.tick((bandR2_.band() * 1.6f + bodyR_.lp * body) * gust + hissR_.hp * hs * 0.5f + whR_.band() * wh);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        gustPrev_ = gustTarget;
        whPrev_ = whTarget;
        hissPrev_ = hissTarget;
    }

private:
    static float smooth(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
    Drift slow_, fast_, flut_;
    Svf bandL_, bandL2_, bandR_, bandR2_, bodyL_, bodyR_, hissL_, hissR_, whL_, whR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    Brown brownL_, brownR_;
    float gustPrev_ = 1.0f, whPrev_ = 0.0f, hissPrev_ = 0.0f;
};

// ── hum ──────────────────────────────────────────────────────────────────────────────────────────
// Pink noise through a few resonances at f0, 2f0, ... (moderate Q, so it reads as a hum of air
// and machinery rather than a note) plus broadband "air handler" noise, with slow pitch drift and
// a slow swell. The LEO cabin bed and the warm aurora hum are both this.
const AmbientSynth::ParamDef kHumParams[] = {
    {"level", 1.0f}, {"f0", 70.0f},       {"harmonics", 3.0f}, {"q", 10.0f},        {"drift", 25.0f},
    {"drift_rate", 0.05f}, {"air", 0.4f}, {"air_hz", 900.0f},  {"wobble", 0.2f},    {"wobble_rate", 0.15f},
    {"width", 0.6f},
};
enum
{
    kHLevel,
    kHF0,
    kHHarm,
    kHQ,
    kHDrift,
    kHDriftRate,
    kHAir,
    kHAirHz,
    kHWobble,
    kHWobbleRate,
    kHWidth
};

class HumSynth final : public AmbientSynth
{
public:
    HumSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("hum", kHumParams, (int)(sizeof(kHumParams) / sizeof(kHumParams[0])), sr, seed)
    {
    }

protected:
    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float d = pitch_.tick(bt, p_[kHDriftRate], rng_);
        const float factor = powf(2.0f, p_[kHDrift] * (2.0f * d - 1.0f) / 1200.0f);
        const int harm = std::clamp((int)lroundf(p_[kHHarm]), 1, 4);
        for (int h = 0; h < harm; ++h)
        {
            const float f = p_[kHF0] * (float)(h + 1) * factor;
            resL_[h].set(f, p_[kHQ], sr_);
            resR_[h].set(f * 1.004f, p_[kHQ], sr_);
        }
        airL_.set(p_[kHAirHz], 0.6f, sr_);
        airR_.set(p_[kHAirHz] * 1.1f, 0.6f, sr_);
        hpL_.set(32.0f, 0.7f, sr_);
        hpR_.set(32.0f, 0.7f, sr_);
        const float wob = std::clamp(p_[kHWobble], 0.0f, 1.0f);
        const float swellTarget = (1.0f - wob) + wob * 2.0f * swell_.tick(bt, p_[kHWobbleRate], rng_);
        const float w = std::clamp(p_[kHWidth], 0.0f, 1.0f);
        const float air = p_[kHAir];
        // Resonance gain compensation: a narrower band passes less noise power.
        const float resGain = 1.8f * sqrtf(std::max(p_[kHQ], 0.5f));
        const float lvl = p_[kHLevel] * 0.45f;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float swell = swellPrev_ + (swellTarget - swellPrev_) * x;
            const float wc = bip();
            const float nl = wc * (1.0f - w) + bip() * w;
            const float nr = wc * (1.0f - w) + bip() * w;
            const float pl = pinkL_.tick(nl), pr = pinkR_.tick(nr);
            float l = 0.0f, r = 0.0f;
            for (int h = 0; h < harm; ++h)
            {
                resL_[h].tick(pl);
                resR_[h].tick(pr);
                const float wgt = 1.0f / (float)(h + 1);
                l += resL_[h].band() * wgt;
                r += resR_[h].band() * wgt;
            }
            airL_.tick(pl);
            airR_.tick(pr);
            hpL_.tick((l * resGain + airL_.lp * air) * swell);
            hpR_.tick((r * resGain + airR_.lp * air) * swell);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        swellPrev_ = swellTarget;
    }

private:
    Drift pitch_, swell_;
    Svf resL_[4], resR_[4], airL_, airR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    float swellPrev_ = 1.0f;
};

// ── beeps ────────────────────────────────────────────────────────────────────────────────────────
// FSK data bursts: a burst is a run of short tone symbols drawn from a few frequencies around the
// carrier, with raised-cosine edges, a random pan and distance, and a small Doppler glide across
// the burst (a passing satellite). Bursts arrive as a Poisson process at `rate` per second, which
// the ambience table drives from how many satellites of a group are within hearing range.
const AmbientSynth::ParamDef kBeepParams[] = {
    {"level", 1.0f},   {"rate", 1.0f},    {"carrier_hz", 2400.0f}, {"spread", 0.25f},
    {"tone_ms", 22.0f}, {"tones_min", 3.0f}, {"tones_max", 10.0f},  {"gap_ms", 6.0f},
    {"doppler", 0.5f}, {"width", 0.8f},   {"bright_hz", 6000.0f},  {"symbols", 3.0f},
};
enum
{
    kBLevel,
    kBRate,
    kBCarrier,
    kBSpread,
    kBToneMs,
    kBTonesMin,
    kBTonesMax,
    kBGapMs,
    kBDoppler,
    kBWidth,
    kBBright,
    kBSymbols
};

class BeepSynth final : public AmbientSynth
{
public:
    BeepSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("beeps", kBeepParams, (int)(sizeof(kBeepParams) / sizeof(kBeepParams[0])), sr, seed)
    {
    }

protected:
    struct Voice
    {
        bool on = false;
        float gl = 0, gr = 0, amp = 0;
        float sym[4] = {};
        int nsym = 2;
        int tonesLeft = 0;
        int toneLen = 0, pos = 0, gapLeft = 0;
        int burstLen = 1, burstPos = 0;
        float f = 0, phase = 0, glide = 0;
    };

    void startTone(Voice &v)
    {
        v.f = v.sym[(int)(uni() * (float)v.nsym) % v.nsym];
        v.toneLen = std::max(8, (int)(p_[kBToneMs] * (0.6f + 0.8f * uni()) * 0.001f * sr_));
        v.pos = 0;
    }

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        // Poisson arrivals, at most a few per block.
        int births = (int)floorf(std::max(p_[kBRate], 0.0f) * bt + uni());
        for (Voice &v : voices_)
        {
            if (births <= 0)
                break;
            if (v.on)
                continue;
            --births;
            v.on = true;
            v.amp = 0.25f + 0.75f * uni() * uni();
            panGains(bip() * std::clamp(p_[kBWidth], 0.0f, 1.0f), v.gl, v.gr);
            v.nsym = std::clamp((int)lroundf(p_[kBSymbols]), 1, 4);
            const float base = p_[kBCarrier] * (1.0f + p_[kBSpread] * bip());
            for (int s = 0; s < v.nsym; ++s)
                v.sym[s] = base * (1.0f + 0.13f * (float)s);
            const int tmin = std::max(1, (int)lroundf(p_[kBTonesMin]));
            const int tmax = std::max(tmin, (int)lroundf(p_[kBTonesMax]));
            v.tonesLeft = tmin + (int)(uni() * (float)(tmax - tmin + 1));
            v.burstLen = std::max(1, (int)((float)v.tonesLeft * (p_[kBToneMs] + p_[kBGapMs]) * 0.001f * sr_));
            v.burstPos = 0;
            v.glide = p_[kBDoppler] * 0.04f * bip();
            v.gapLeft = 0;
            startTone(v);
        }
        lpL_.set(p_[kBBright], 0.7f, sr_);
        lpR_.set(p_[kBBright], 0.7f, sr_);
        const int gap = (int)(p_[kBGapMs] * 0.001f * sr_);
        const int ramp = std::max(2, (int)(0.0025f * sr_));
        const float lvl = p_[kBLevel] * 0.5f;

        for (uint32_t i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            for (Voice &v : voices_)
            {
                if (!v.on)
                    continue;
                ++v.burstPos;
                if (v.gapLeft > 0)
                {
                    if (--v.gapLeft == 0)
                        startTone(v);
                    continue;
                }
                const float prog = std::min(1.0f, (float)v.burstPos / (float)v.burstLen);
                const float f = v.f * (1.0f + v.glide * (prog - 0.5f));
                v.phase += kTwoPi * f / sr_;
                if (v.phase > kTwoPi)
                    v.phase -= kTwoPi;
                float env = 1.0f;
                if (v.pos < ramp)
                    env = 0.5f - 0.5f * cosf(kPi * (float)v.pos / (float)ramp);
                else if (v.toneLen - v.pos < ramp)
                    env = 0.5f - 0.5f * cosf(kPi * (float)(v.toneLen - v.pos) / (float)ramp);
                // A slightly squared-off sine: the "digital" timbre of a modem blip.
                const float s = tanhf(1.8f * sinf(v.phase)) * env * v.amp;
                l += s * v.gl;
                r += s * v.gr;
                if (++v.pos >= v.toneLen)
                {
                    if (--v.tonesLeft <= 0)
                        v.on = false;
                    else if (gap > 0)
                        v.gapLeft = gap;
                    else
                        startTone(v);
                }
            }
            lpL_.tick(l);
            lpR_.tick(r);
            out[2 * i] = lpL_.lp * lvl;
            out[2 * i + 1] = lpR_.lp * lvl;
        }
    }

private:
    Voice voices_[8];
    Svf lpL_, lpR_;
};

// ── disk ─────────────────────────────────────────────────────────────────────────────────────────
// A room of servers: fan noise, a faint spindle whine, and seek activity — bursts of head clicks
// (an impulse ringing a small high resonance plus a low thunk) at irregular few-ms intervals.
const AmbientSynth::ParamDef kDiskParams[] = {
    {"level", 1.0f}, {"activity", 1.5f}, {"whine", 0.25f}, {"whine_hz", 120.0f}, {"fan", 0.5f},
    {"fan_hz", 1400.0f}, {"click_hz", 2800.0f}, {"seek_ms", 14.0f}, {"clicks", 1.0f}, {"width", 0.7f},
};
enum
{
    kDLevel,
    kDActivity,
    kDWhine,
    kDWhineHz,
    kDFan,
    kDFanHz,
    kDClickHz,
    kDSeekMs,
    kDClicks,
    kDWidth
};

class DiskSynth final : public AmbientSynth
{
public:
    DiskSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("disk", kDiskParams, (int)(sizeof(kDiskParams) / sizeof(kDiskParams[0])), sr, seed)
    {
    }

protected:
    struct Burst
    {
        bool on = false;
        int left = 0, next = 0, interval = 0;
        float gl = 0, gr = 0, amp = 0;
    };

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        int births = (int)floorf(std::max(p_[kDActivity], 0.0f) * bt + uni());
        for (Burst &b : bursts_)
        {
            if (births <= 0)
                break;
            if (b.on)
                continue;
            --births;
            b.on = true;
            b.left = 3 + (int)(uni() * 24.0f * std::max(p_[kDClicks], 0.1f));
            b.interval = std::max(16, (int)(p_[kDSeekMs] * (0.5f + uni()) * 0.001f * sr_));
            b.next = 0;
            b.amp = 0.3f + 0.7f * uni();
            panGains(bip() * std::clamp(p_[kDWidth], 0.0f, 1.0f), b.gl, b.gr);
        }
        clickL_.set(p_[kDClickHz], 5.0f, sr_);
        clickR_.set(p_[kDClickHz] * 1.09f, 5.0f, sr_);
        thunkL_.set(170.0f, 3.0f, sr_);
        thunkR_.set(185.0f, 3.0f, sr_);
        fanL_.set(p_[kDFanHz], 0.6f, sr_);
        fanR_.set(p_[kDFanHz] * 0.9f, 0.6f, sr_);
        hpL_.set(60.0f, 0.7f, sr_);
        hpR_.set(60.0f, 0.7f, sr_);
        const float wobTarget = 0.8f + 0.4f * wob_.tick(bt, 0.3f, rng_);
        const float w = std::clamp(p_[kDWidth], 0.0f, 1.0f);
        const float lvl = p_[kDLevel] * 1.2f;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float wob = wobPrev_ + (wobTarget - wobPrev_) * x;
            float exL = 0.0f, exR = 0.0f;
            for (Burst &b : bursts_)
            {
                if (!b.on || --b.next > 0)
                    continue;
                const float a = b.amp * (0.5f + 0.5f * uni()) * 24.0f;
                exL += a * b.gl;
                exR += a * b.gr;
                b.next = (int)((float)b.interval * (0.4f + 1.2f * uni()));
                if (--b.left <= 0)
                    b.on = false;
            }
            clickL_.tick(exL);
            clickR_.tick(exR);
            thunkL_.tick(exL);
            thunkR_.tick(exR);
            whinePh_ += kTwoPi * p_[kDWhineHz] / sr_;
            if (whinePh_ > kTwoPi)
                whinePh_ -= kTwoPi;
            const float whine = (sinf(whinePh_) * 0.6f + sinf(2.0f * whinePh_) * 0.25f + sinf(3.0f * whinePh_) * 0.1f) *
                                p_[kDWhine] * 0.06f * wob;
            const float wc = bip();
            const float nl = wc * (1.0f - w) + bip() * w;
            const float nr = wc * (1.0f - w) + bip() * w;
            fanL_.tick(pinkL_.tick(nl));
            fanR_.tick(pinkR_.tick(nr));
            const float fan = p_[kDFan] * 0.45f;
            hpL_.tick(clickL_.band() * 0.5f + thunkL_.band() * 0.35f + fanL_.lp * fan + whine);
            hpR_.tick(clickR_.band() * 0.5f + thunkR_.band() * 0.35f + fanR_.lp * fan + whine);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        wobPrev_ = wobTarget;
    }

private:
    Burst bursts_[4];
    Svf clickL_, clickR_, thunkL_, thunkR_, fanL_, fanR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    Drift wob_;
    float whinePh_ = 0.0f, wobPrev_ = 1.0f;
};

// ── chorus (magnetospheric VLF) ──────────────────────────────────────────────────────────────────
// What a VLF receiver hears in the magnetosphere (the Van Allen Probes / EMFISIS recordings):
// "chorus" — clusters of short rising chirps — over a bed of hiss, occasional whistlers (lightning
// energy dispersed along a field line: a tone falling as f = (D/t)^2), and sferic crackle. The
// chirps and whistlers (not the hiss) run through a slow stereo flanger.
const AmbientSynth::ParamDef kChorusParams[] = {
    {"level", 1.0f},        {"hiss", 0.35f},       {"chorus_rate", 6.0f}, {"chorus_hz", 1400.0f},
    {"chorus_rise", 1.0f},  {"chorus_ms", 260.0f}, {"cluster", 0.7f},     {"whistler_rate", 0.06f},
    {"crackle", 3.0f},      {"width", 0.8f},       {"flange", 0.6f},      {"flange_rate", 0.12f},
    {"flange_ms", 4.0f},    {"flange_fb", 0.55f},
};
enum
{
    kCLevel,
    kCHiss,
    kCRate,
    kCHz,
    kCRise,
    kCMs,
    kCCluster,
    kCWhistler,
    kCCrackle,
    kCWidth,
    kCFlange,
    kCFlangeRate,
    kCFlangeMs,
    kCFlangeFb
};

class ChorusSynth final : public AmbientSynth
{
public:
    ChorusSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("chorus", kChorusParams, (int)(sizeof(kChorusParams) / sizeof(kChorusParams[0])), sr, seed)
    {
    }

protected:
    struct Chirp
    {
        bool on = false;
        float t = 0, dur = 1, f0 = 1000, rise = 1, phase = 0, amp = 0, gl = 0, gr = 0;
    };
    struct Whistler
    {
        bool on = false;
        float t = 0, dur = 1, t0 = 0, d = 0, phase = 0, amp = 0, gl = 0, gr = 0;
    };

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float cl = std::clamp(p_[kCCluster], 0.0f, 1.0f);
        const float c = cluster_.tick(bt, 0.12f, rng_);
        const float rate = std::max(p_[kCRate], 0.0f) * ((1.0f - cl) + cl * 2.2f * c * c);
        const float w = std::clamp(p_[kCWidth], 0.0f, 1.0f);
        int births = (int)floorf(rate * bt + uni());
        for (Chirp &ch : chirps_)
        {
            if (births <= 0)
                break;
            if (ch.on)
                continue;
            --births;
            ch.on = true;
            ch.t = 0.0f;
            ch.dur = std::max(0.03f, p_[kCMs] * 0.001f * (0.5f + uni()));
            ch.f0 = p_[kCHz] * (0.65f + 0.8f * uni());
            ch.rise = p_[kCRise] * (0.6f + 0.8f * uni());
            ch.amp = 0.2f + 0.8f * uni() * uni();
            panGains(bip() * w, ch.gl, ch.gr);
        }
        if (uni() < std::max(p_[kCWhistler], 0.0f) * bt)
        {
            for (Whistler &wh : whistlers_)
            {
                if (wh.on)
                    continue;
                wh.on = true;
                wh.dur = 0.8f + 1.2f * uni();
                // f = (D / t)^2 from 7 kHz down to 700 Hz over dur.
                const float fHi = 7000.0f, fLo = 700.0f;
                wh.d = wh.dur / (1.0f / sqrtf(fLo) - 1.0f / sqrtf(fHi));
                wh.t0 = wh.d / sqrtf(fHi);
                wh.t = 0.0f;
                wh.amp = 0.5f + 0.5f * uni();
                panGains(bip() * w * 0.6f, wh.gl, wh.gr);
                break;
            }
        }
        int crackles = (int)floorf(std::max(p_[kCCrackle], 0.0f) * bt + uni());
        int crackleAt[4] = {-1, -1, -1, -1};
        for (int k = 0; k < std::min(crackles, 4); ++k)
            crackleAt[k] = (int)(uni() * (float)n);
        hissL_.set(2600.0f, 0.6f, sr_);
        hissR_.set(2900.0f, 0.6f, sr_);
        crL_.set(3200.0f, 1.2f, sr_);
        crR_.set(3500.0f, 1.2f, sr_);
        const float dt = 1.0f / sr_;
        const float hiss = p_[kCHiss];
        const float lvl = p_[kCLevel] * 0.9f;

        for (uint32_t i = 0; i < n; ++i)
        {
            float l = 0.0f, r = 0.0f;
            for (Chirp &ch : chirps_)
            {
                if (!ch.on)
                    continue;
                const float x = ch.t / ch.dur;
                const float f = ch.f0 * exp2f(ch.rise * powf(x, 0.8f));
                ch.phase += kTwoPi * f * dt;
                if (ch.phase > kTwoPi)
                    ch.phase -= kTwoPi;
                const float env = sinf(kPi * x);
                const float s = sinf(ch.phase) * env * env * ch.amp;
                l += s * ch.gl;
                r += s * ch.gr;
                ch.t += dt;
                if (ch.t >= ch.dur)
                    ch.on = false;
            }
            for (Whistler &wh : whistlers_)
            {
                if (!wh.on)
                    continue;
                const float tt = wh.t0 + wh.t;
                const float f = (wh.d / tt) * (wh.d / tt);
                wh.phase += kTwoPi * f * dt;
                if (wh.phase > kTwoPi)
                    wh.phase -= kTwoPi;
                const float x = wh.t / wh.dur;
                const float env = std::min(1.0f, wh.t / 0.04f) * (1.0f - x) * (1.0f - x);
                const float s = sinf(wh.phase) * env * wh.amp * 0.6f;
                l += s * wh.gl;
                r += s * wh.gr;
                wh.t += dt;
                if (wh.t >= wh.dur)
                    wh.on = false;
            }
            if (p_[kCFlange] > 0.0f)
                flanger_.process(l, r, sr_, p_[kCFlangeRate], p_[kCFlangeMs], p_[kCFlangeFb], p_[kCFlange]);
            float ex = 0.0f;
            for (int k = 0; k < 4; ++k)
                if (crackleAt[k] == (int)i)
                    ex += (0.5f + uni()) * 3.0f;
            crL_.tick(ex * (0.6f + 0.4f * uni()));
            crR_.tick(ex * (0.6f + 0.4f * uni()));
            const float wc = bip();
            hissL_.tick(wc * (1.0f - w) + bip() * w);
            hissR_.tick(wc * (1.0f - w) + bip() * w);
            out[2 * i] = (l + hissL_.band() * hiss * 0.4f + crL_.band() * 0.5f) * lvl;
            out[2 * i + 1] = (r + hissR_.band() * hiss * 0.4f + crR_.band() * 0.5f) * lvl;
        }
    }

private:
    Chirp chirps_[16];
    Whistler whistlers_[3];
    Svf hissL_, hissR_, crL_, crR_;
    Drift cluster_;
    Flanger flanger_;
};

// ── drone ────────────────────────────────────────────────────────────────────────────────────────
// A machine or electrical tone, built to sit UNDER music: a harmonic series on f0 (wavetable, with
// a second detuned copy per side for slow beating), through a phaser; an optional thin whine a few
// octaves up with vibrato (a fridge, a transformer, a server hall); a little air; an optional
// compressor cycle (the hum swells on and drops back like a fridge); and occasional soft low
// "status" tones — short motifs on consonant ratios of a base pitch, not a melody. Pitches are
// meant to be given as multiples of the ambience's tonal root ({"root": k} in the table), so every
// drone in the sim shares a key.
const AmbientSynth::ParamDef kDroneParams[] = {
    {"level", 1.0f},        {"f0", 110.0f},        {"harmonics", 6.0f},   {"bright", 1.0f},
    {"odd", 0.0f},          {"detune", 4.0f},      {"phaser", 0.5f},      {"phaser_rate", 0.08f},
    {"phaser_depth", 0.8f}, {"phaser_fb", 0.5f},   {"whine", 0.0f},       {"whine_ratio", 16.0f},
    {"whine_wobble", 6.0f}, {"air", 0.1f},         {"air_hz", 600.0f},    {"cycle_s", 0.0f},
    {"cycle_depth", 0.5f},  {"tones", 0.05f},      {"tone_ratio", 4.0f},  {"tone_ms", 180.0f},
    {"tone_level", 0.5f},   {"swell", 0.15f},      {"width", 0.6f},
};
enum
{
    kDLvl,
    kDF0,
    kDHarm,
    kDBright,
    kDOdd,
    kDDetune,
    kDPhaser,
    kDPhRate,
    kDPhDepth,
    kDPhFb,
    kDrWhine,
    kDrWhineRatio,
    kDrWhineWobble,
    kDAir,
    kDAirHz,
    kDCycleS,
    kDCycleDepth,
    kDTones,
    kDToneRatio,
    kDToneMs,
    kDToneLevel,
    kDSwell,
    kDDWidth
};

class DroneSynth final : public AmbientSynth
{
public:
    DroneSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("drone", kDroneParams, (int)(sizeof(kDroneParams) / sizeof(kDroneParams[0])), sr, seed)
    {
        ph_[1] = 0.37f;
        ph_[2] = 0.71f;
    }

protected:
    // Consonant ratios over the tone base (just intonation: 1, 9/8, 5/4, 3/2, 5/3, 2) and a few
    // short motifs over them. Sparse, soft and repeated: status chirps, not a tune.
    static constexpr float kRatios[6] = {1.0f, 1.125f, 1.25f, 1.5f, 1.6667f, 2.0f};
    static constexpr int kMotifs[7][3] = {{0, 0, -1}, {0, 3, -1}, {0, 2, 3}, {3, 0, -1}, {0, 5, -1}, {3, 2, 0}, {0, 3, 5}};

    struct Note
    {
        bool on = false;
        float t = 0.0f, dur = 0.2f, f = 220.0f, ph = 0.0f, amp = 0.0f, gl = 0.7f, gr = 0.7f;
    };

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const int harm = std::clamp((int)lroundf(p_[kDHarm]), 1, 12);
        const float bright = std::max(p_[kDBright], 0.0f), odd = std::clamp(p_[kDOdd], 0.0f, 1.0f);
        tab_.ensure(harm, bright, odd);

        // Compressor cycle: on for 60% of the period, off for 40%, with 2 s ramps.
        float cycleTarget = 1.0f;
        const float period = p_[kDCycleS];
        if (period > 1.0f)
        {
            cycT_ += bt;
            if (cycT_ >= period)
                cycT_ -= period;
            const float onEnd = 0.6f * period;
            const float e = cycT_ < onEnd ? std::min(1.0f, cycT_ / 2.0f) : std::max(0.0f, 1.0f - (cycT_ - onEnd) / 2.0f);
            const float d = std::clamp(p_[kDCycleDepth], 0.0f, 1.0f);
            cycleTarget = (1.0f - d) + d * e;
        }
        const float sw = std::clamp(p_[kDSwell], 0.0f, 1.0f);
        const float swellTarget = ((1.0f - sw) + sw * 2.0f * swell_.tick(bt, 0.07f, rng_)) * cycleTarget;
        phaser_.setBlock(bt, sr_, p_[kDPhRate], p_[kDPhDepth], 180.0f, 2400.0f);
        airL_.set(p_[kDAirHz], 0.6f, sr_);
        airR_.set(p_[kDAirHz] * 1.1f, 0.6f, sr_);
        hpL_.set(30.0f, 0.7f, sr_);
        hpR_.set(30.0f, 0.7f, sr_);
        // Tone phrases (Poisson), each a short motif.
        if (!phraseLeft_ && uni() < std::max(p_[kDTones], 0.0f) * bt)
        {
            motif_ = (int)(uni() * 7.0f) % 7;
            motifPos_ = 0;
            phraseLeft_ = true;
            nextNote_ = 0.0f;
            phrasePan_ = bip() * 0.5f;
            phraseAmp_ = 0.6f + 0.4f * uni();
        }
        if (phraseLeft_)
        {
            nextNote_ -= bt;
            if (nextNote_ <= 0.0f)
            {
                const int step = motifPos_ < 3 ? kMotifs[motif_][motifPos_] : -1;
                if (step < 0)
                    phraseLeft_ = false;
                else
                {
                    for (Note &nt : notes_)
                        if (!nt.on)
                        {
                            nt.on = true;
                            nt.t = 0.0f;
                            nt.dur = std::max(0.04f, p_[kDToneMs] * 0.001f) * 2.5f;
                            nt.f = p_[kDF0] * p_[kDToneRatio] * kRatios[step];
                            nt.amp = phraseAmp_;
                            panGains(phrasePan_, nt.gl, nt.gr);
                            break;
                        }
                    ++motifPos_;
                    nextNote_ = std::max(0.04f, p_[kDToneMs] * 0.001f) * 1.6f;
                }
            }
        }

        const float cents = p_[kDDetune] / 1200.0f;
        const float f0 = std::max(p_[kDF0], 10.0f);
        const float inc0 = f0 / sr_, incUp = f0 * exp2f(cents) / sr_, incDn = f0 * exp2f(-cents) / sr_;
        const float w = std::clamp(p_[kDDWidth], 0.0f, 1.0f);
        const float whine = p_[kDrWhine] * 0.12f;
        const float wobDepth = p_[kDrWhineWobble] / 1200.0f;
        const float fWhine = f0 * p_[kDrWhineRatio];
        const float air = p_[kDAir];
        const float phMix = std::clamp(p_[kDPhaser], 0.0f, 1.0f);
        const float tauTone = std::max(0.02f, p_[kDToneMs] * 0.001f) * 0.5f;
        const float toneLvl = p_[kDToneLevel] * 0.35f;
        const float lvl = p_[kDLvl] * 0.3f;
        const float dt = 1.0f / sr_;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float swell = swellPrev_ + (swellTarget - swellPrev_) * x;
            ph_[0] += inc0;
            ph_[1] += incUp;
            ph_[2] += incDn;
            for (float &p : ph_)
                if (p >= 1.0f)
                    p -= 1.0f;
            const float a = tab_.lookup(ph_[0]), b = tab_.lookup(ph_[1]), c = tab_.lookup(ph_[2]);
            // The detuned copies are a quarter of the mix: at 40% they pumped the level +-7 dB at
            // the beat rate, a slow "wah" that grew tiresome.
            float l = a * 0.75f + (b * (1.0f - w * 0.5f) + c * w * 0.5f) * 0.25f;
            float r = a * 0.75f + (c * (1.0f - w * 0.5f) + b * w * 0.5f) * 0.25f;
            phaser_.process(l, r, p_[kDPhFb], phMix);
            if (whine > 0.0f)
            {
                vib_ += kTwoPi * 0.31f * dt;
                if (vib_ > kTwoPi)
                    vib_ -= kTwoPi;
                phW_ += kTwoPi * fWhine * exp2f(wobDepth * sinf(vib_)) * dt;
                if (phW_ > kTwoPi)
                    phW_ -= kTwoPi;
                const float s = sinf(phW_) * whine;
                l += s;
                r += s;
            }
            const float wc = bip();
            airL_.tick(pinkL_.tick(wc * (1.0f - w) + bip() * w));
            airR_.tick(pinkR_.tick(wc * (1.0f - w) + bip() * w));
            l = l * swell + airL_.lp * air * 2.0f;
            r = r * swell + airR_.lp * air * 2.0f;
            for (Note &nt : notes_)
            {
                if (!nt.on)
                    continue;
                nt.ph += kTwoPi * nt.f * dt;
                if (nt.ph > kTwoPi)
                    nt.ph -= kTwoPi;
                const float env = std::min(1.0f, nt.t / 0.02f) * expf(-nt.t / tauTone);
                const float s = (sinf(nt.ph) + 0.15f * sinf(2.0f * nt.ph)) * env * nt.amp * toneLvl;
                l += s * nt.gl;
                r += s * nt.gr;
                nt.t += dt;
                if (nt.t >= nt.dur)
                    nt.on = false;
            }
            hpL_.tick(l);
            hpR_.tick(r);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        swellPrev_ = swellTarget;
    }

private:
    Wavetable tab_;
    float ph_[3] = {0.0f, 0.0f, 0.0f};
    float phW_ = 0.0f, vib_ = 0.0f, cycT_ = 0.0f, swellPrev_ = 1.0f;
    Phaser phaser_;
    Svf airL_, airR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    Drift swell_;
    Note notes_[6];
    bool phraseLeft_ = false;
    int motif_ = 0, motifPos_ = 0;
    float nextNote_ = 0.0f, phrasePan_ = 0.0f, phraseAmp_ = 1.0f;
};

// ── saw stack (unused by default) ─────────────────────────────────────────────────────────────────
// The first beam sound (2026-09-27, layer beam_swell), kept as a synth kind for the table: it read as
// an FPV drone, and the beams are now the glare pad (flares on screen) over the site bass below.
// The Reflect Orbital beam light, heard as warmth: about one saw oscillator per beam of light in
// view. A single beam is one oscillator high up and quiet; what makes it read as a twinkle rather
// than a tone is its own slow amplitude shimmer (`shimmer`, each voice its own random rate), which
// averages away into a steady warm wash as voices arrive. More beams add more oscillators — a
// FRACTIONAL count, so the last one crossfades in and the count can never step — each a little
// sharper than the last, so the stack widens and beats as it grows, while the whole stack sinks by
// `fall_oct`: the light getting heavier and lower as the mirrors converge, with `sub` folding in an
// octave below for the body. A shared ~30-cent vibrato, offset per voice so the stack breathes as a
// chord instead of one organ, is the "uneasy warmth" this was asked for.
// Everything pitched is a RATIO of f0_hz (f0 x 2^-(fall_oct) x 2^(cents/1200)), so the voice stays a
// multiple of the tonal root like every other pitched layer ({"root": k} in the table). `fall_oct`
// is a separate parameter rather than a mod on f0_hz because a parameter that is BOTH a table
// `params` entry and a `mod` target is overwritten by the root write every frame — Ambience::update
// applies mods first and the table's root params after.
const AmbientSynth::ParamDef kSawParams[] = {
    {"level", 1.0f},     {"f0_hz", 440.0f},      {"voices", 1.0f},     {"detune_cents", 22.0f},
    {"fall_oct", 0.0f},  {"sub", 0.0f},          {"bright", 0.55f},    {"drive", 0.3f},
    {"shimmer", 0.45f},  {"shimmer_rate", 0.5f}, {"lfo_cents", 30.0f}, {"lfo_rate_hz", 0.19f},
    {"spread", 0.8f},    {"hp_hz", 40.0f},
};
enum
{
    kSwLvl,
    kSwF0,
    kSwVoices,
    kSwDetune,
    kSwFall,
    kSwSub,
    kSwBright,
    kSwDrive,
    kSwShimmer,
    kSwShimRate,
    kSwLfoCents,
    kSwLfoRate,
    kSwSpread,
    kSwHp
};

class SawSynth final : public AmbientSynth
{
public:
    SawSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("saw", kSawParams, (int)(sizeof(kSawParams) / sizeof(kSawParams[0])), sr, seed)
    {
        for (int v = 0; v < kMaxVoices; ++v)
        {
            ph_[v] = uni();                              // the voices start spread over the cycle
            const float off = kTwoPi * (float)v * 0.37f; // ... and so does their vibrato
            lfoCos_[v] = cosf(off);
            lfoSin_[v] = sinf(off);
            shimRate_[v] = 0.6f + 0.8f * uni();          // each voice shimmers at its own rate
        }
    }

protected:
    static constexpr int kMaxVoices = 12;
    // One FIXED band limit, built once: 16 harmonics is everything the low-pass below this leaves
    // audible, and it stays under Nyquist for the whole range this voice can reach (the table's
    // root tops out at 82 Hz, x12 = 984 Hz for voice 0, x2^(22 cents x 11) for the sharpest — 16
    // x 1.13 kHz = 18 kHz, still below 24 kHz). A pitch-tracking table would be more "correct" and
    // sound identical, but it would rebuild on every beam arriving — a 30k-sine loop on the audio
    // thread, per crossing, for harmonics the low-pass has already muted.
    static constexpr int kTableHarm = 16;
    // Trimmed so a full stack measures about -24 LUFS at level 1, gain 1 (see the file header):
    // measured -23.0 LUFS at the Topaz beam site, all beams in view, via ambience_solos.satcmd.
    static constexpr float kOut = 0.19f;

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float f0 = std::max(p_[kSwF0], 5.0f) * exp2f(-std::clamp(p_[kSwFall], -2.0f, 5.0f));
        const float voices = std::clamp(p_[kSwVoices], 0.0f, (float)kMaxVoices);
        const int nFull = std::min((int)voices, kMaxVoices);
        const float frac = voices - (float)nFull;
        const float cents = std::clamp(p_[kSwDetune], 0.0f, 200.0f) / 1200.0f;

        // Voice 0 is exactly f0_hz; each further voice is `detune_cents` sharper. `bright` is the
        // table's harmonic roll-off (1.0 with no band limit is a pure saw; below that the top end is
        // softened — warm) and the low-pass below scales with the pitch, so the stack darkens as it
        // sinks. See kTableHarm for why the band limit doesn't follow fTop.
        const float fTop = f0 * exp2f(cents * (float)std::max(nFull, 1));
        tab_.ensure(kTableHarm, std::max(p_[kSwBright], 0.05f), 0.0f);

        const float spread = std::clamp(p_[kSwSpread], 0.0f, 1.0f);
        const float shim = std::clamp(p_[kSwShimmer], 0.0f, 1.0f);
        const float shimRate = std::max(p_[kSwShimRate], 0.0f);
        for (int v = 0; v < kMaxVoices; ++v)
        {
            inc_[v] = f0 * exp2f(cents * (float)v) / sr_;
            panGains(spread * ((v % 2) ? 1.0f : -1.0f) * (float)v / (float)(kMaxVoices - 1), gl_[v], gr_[v]);
            shimGain_[v] = 1.0f - shim * (1.0f - shim_[v].tick(bt, shimRate * shimRate_[v], rng_));
        }

        // Soft saturation on the sum: the stack MUST get louder as beams arrive (that is the whole
        // point of the layer), and tanh is what keeps twelve of them from clipping while it does —
        // the peak stops rising long before the RMS does.
        const float drive = std::clamp(p_[kSwDrive], 0.0f, 1.0f);
        const float sat = 1.0f + 4.0f * drive, satInv = 1.0f / sat, post = 1.0f + 1.5f * drive;
        // Cents as a ratio, linear approximation (1200/ln2 = 1731.2 cents per octave).
        const float lfoDepth = std::clamp(p_[kSwLfoCents], 0.0f, 300.0f) / 1731.0f;
        const float lfoInc = kTwoPi * std::max(p_[kSwLfoRate], 0.0f) * bt;
        const float sub = std::clamp(p_[kSwSub], 0.0f, 1.0f);
        const float fc = std::clamp(fTop * (1.0f + 6.0f * std::max(p_[kSwBright], 0.05f)), 300.0f, 0.42f * sr_);
        const float hp = std::clamp(p_[kSwHp], 10.0f, 4000.0f);
        lpL_.set(fc, 0.7f, sr_);
        lpR_.set(fc * 1.04f, 0.7f, sr_); // a hair apart, so the two sides never sound identical
        hpL_.set(hp, 0.7f, sr_);
        hpR_.set(hp, 0.7f, sr_);
        const float subInc = 0.5f * f0 / sr_;
        const int nv = std::min(kMaxVoices, nFull + (frac > 0.0f ? 1 : 0));
        const float level = p_[kSwLvl];

        for (uint32_t i = 0; i < n; ++i)
        {
            lfoPh_ += lfoInc;
            if (lfoPh_ >= kTwoPi)
                lfoPh_ -= kTwoPi;
            const float sa = sinf(lfoPh_), ca = cosf(lfoPh_);
            float l = 0.0f, r = 0.0f;
            for (int v = 0; v < nv; ++v)
            {
                // sin(lfo + off_v) from one sin/cos pair per block: a rotation per voice, not a
                // per-voice sin. Those offsets are what stop the stack moving as one body.
                float &ph = ph_[v];
                ph += inc_[v] * (1.0f + lfoDepth * (sa * lfoCos_[v] + ca * lfoSin_[v]));
                if (ph >= 1.0f)
                    ph -= 1.0f;
                const float s = tab_.lookup(ph) * shimGain_[v] * (v == nFull ? frac : 1.0f);
                l += s * gl_[v];
                r += s * gr_[v];
            }
            if (sub > 0.0f)
            {
                subPh_ += subInc;
                if (subPh_ >= 1.0f)
                    subPh_ -= 1.0f;
                const float s = sinf(kTwoPi * subPh_) * sub * 0.6f;
                l += s;
                r += s;
            }
            lpL_.tick(tanhf(l * sat) * satInv * post);
            lpR_.tick(tanhf(r * sat) * satInv * post);
            hpL_.tick(lpL_.lp);
            hpR_.tick(lpR_.lp);
            out[2 * i] = hpL_.hp * level * kOut;
            out[2 * i + 1] = hpR_.hp * level * kOut;
        }
    }

private:
    Wavetable tab_;
    float ph_[kMaxVoices] = {};
    float inc_[kMaxVoices] = {};
    float gl_[kMaxVoices] = {}, gr_[kMaxVoices] = {};
    float lfoCos_[kMaxVoices] = {}, lfoSin_[kMaxVoices] = {};
    float shimGain_[kMaxVoices] = {};
    float shimRate_[kMaxVoices] = {};
    Drift shim_[kMaxVoices];
    float lfoPh_ = 0.0f, subPh_ = 0.0f;
    Svf lpL_, lpR_, hpL_, hpR_;
};

// Feedback-delay-network reverb: four delay lines (31.7-49.9 ms x size), a Hadamard mix in the loop
// and a one-pole damping per line, each line's gain set from its own length so the whole tail falls
// 60 dB in `t60` seconds. Enough to turn a dry chord into a space; allocation-free (fixed buffers).
struct Fdn
{
    static constexpr int kLen = 8192; // power of two; 49.9 ms x size 2 at 48 kHz fits (4790)
    float buf[4][kLen] = {};
    int len[4] = {64, 64, 64, 64};
    float g[4] = {};
    float lp[4] = {};
    int w = 0;
    float srSet = 0.0f, t60Set = -1.0f, sizeSet = -1.0f;

    void set(float sr, float t60, float size)
    {
        if (sr == srSet && std::fabs(t60 - t60Set) < 0.02f && std::fabs(size - sizeSet) < 0.02f)
            return;
        static constexpr float kMs[4] = {31.7f, 37.3f, 43.1f, 49.9f};
        for (int k = 0; k < 4; ++k)
        {
            len[k] = std::clamp((int)(kMs[k] * size * 0.001f * sr), 16, kLen - 1);
            g[k] = powf(10.0f, -3.0f * (float)len[k] / (std::max(t60, 0.1f) * sr));
        }
        srSet = sr;
        t60Set = t60;
        sizeSet = size;
    }
    // damp: 0 = bright tail, 1 = very dark.
    void process(float inL, float inR, float damp, float &outL, float &outR)
    {
        float d[4];
        const float c = 1.0f - std::clamp(damp, 0.0f, 0.95f);
        for (int k = 0; k < 4; ++k)
        {
            const float y = buf[k][(w - len[k]) & (kLen - 1)];
            lp[k] += (y - lp[k]) * c;
            d[k] = lp[k] * g[k];
        }
        const float h0 = 0.5f * (d[0] + d[1] + d[2] + d[3]);
        const float h1 = 0.5f * (d[0] - d[1] + d[2] - d[3]);
        const float h2 = 0.5f * (d[0] + d[1] - d[2] - d[3]);
        const float h3 = 0.5f * (d[0] - d[1] - d[2] + d[3]);
        buf[0][w] = inL + h0;
        buf[1][w] = inR + h1;
        buf[2][w] = inL * 0.6f - inR * 0.4f + h2;
        buf[3][w] = inR * 0.6f - inL * 0.4f + h3;
        w = (w + 1) & (kLen - 1);
        outL = d[0] + 0.6f * d[2];
        outR = d[1] + 0.6f * d[3];
    }
};

// ── pad (the glare chorus) ───────────────────────────────────────────────────────────────────────
// Bright flares in view heard as a chord: hollow voices (odd harmonics — a clarinet's, a stopped
// pipe's — through a low-pass and a long FDN tail), one after another as more flares glare, each in
// the sim's key. The sim writes three pitch-class masks RELATIVE to f0_hz (bit k = k semitones above
// it): the SCALE (the music's own pitch set), the CHORD the music is on now, and TENSION tones (the
// minor second and tritone by default, ambience.json "tonality") that are only reached by the last
// voices. Voices are placed by a fixed LADDER of roles, so what a given number of flares sounds like
// is always the same shape in whatever key:
//   1 flare  ~ 2 voices: the chord's fifth and octave — open, hollow, no third: a single glint
//   more     the root below, then the third high up, the ninth against the octave (the first
//            cluster), the fifth an octave down, the seventh, the root two octaves down, the sixth
//            against the fifth, a TENSION tone a semitone off the root, a low fifth, the tritone.
// So the stack widens downward and fills in with seconds as flares gather: deeper and more crowded,
// still inside the music's notes until the very end, where the tension tones rub against it.
// Every voice has its own slow pitch WARP (a drift of +-warp_cents, which the table grows with the
// glare): the chord never quite sits still, the way gravity_wave's piano bends under it. A voice
// that joins BLOOMS: louder for bloom_s, with a glassy octave partial on top — the fleeting part —
// then settles into the chord or fades with the flare. `voices` is fractional (the last voice
// crossfades), so the count can never step. A chord change glides every voice to its new note over
// glide_s instead of jumping.
const AmbientSynth::ParamDef kPadParams[] = {
    {"level", 1.0f},
    {"f0_hz", 330.0f},
    {"scale_mask", 1453.0f, true}, // Aeolian until the sim says otherwise (1 2 b3 4 5 b6 b7)
    {"chord_mask", 129.0f, true},  // root + fifth
    {"tension_mask", 66.0f, true}, // b2 + #4
    {"voices", 0.0f},
    {"hollow", 0.8f},
    {"bright", 1.6f},
    {"detune_cents", 5.0f},
    {"warp_cents", 10.0f},
    {"warp_rate", 0.06f},
    {"glide_s", 0.9f},
    {"attack_s", 0.4f},
    {"release_s", 1.1f},
    {"bloom", 0.8f},
    {"bloom_s", 1.4f},
    {"shimmer", 0.25f},
    {"cutoff_hz", 2400.0f},
    {"reverb", 0.4f},
    {"reverb_s", 4.5f},
    {"spread", 0.8f},
    {"hp_hz", 110.0f},
};
enum
{
    kPdLvl,
    kPdF0,
    kPdScale,
    kPdChord,
    kPdTension,
    kPdVoices,
    kPdHollow,
    kPdBright,
    kPdDetune,
    kPdWarp,
    kPdWarpRate,
    kPdGlide,
    kPdAttack,
    kPdRelease,
    kPdBloom,
    kPdBloomS,
    kPdShimmer,
    kPdCutoff,
    kPdReverb,
    kPdReverbS,
    kPdSpread,
    kPdHp
};

class PadSynth final : public AmbientSynth
{
public:
    PadSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("pad", kPadParams, (int)(sizeof(kPadParams) / sizeof(kPadParams[0])), sr, seed)
    {
        sine_.rebuild(1, 1.0f, 0.0f);
        for (int v = 0; v < kMaxVoices; ++v)
        {
            phA_[v] = uni();
            phB_[v] = uni();
            phS_[v] = uni();
            warpRate_[v] = 0.7f + 0.6f * uni();
            shimRate_[v] = 0.5f + 0.9f * uni();
            // Alternate sides, wider for later (lower, busier) voices.
            pan_[v] = ((v % 2) ? 1.0f : -1.0f) * (0.2f + 0.8f * (float)v / (float)(kMaxVoices - 1));
        }
    }

protected:
    static constexpr int kMaxVoices = 12;
    static constexpr int kTableHarm = 10; // f0 x 2^(26/12) x 10 stays under Nyquist for f0 < 470 Hz
    // Trimmed so four voices at level 1, gain 1 measure about -24 LUFS (see the file header).
    static constexpr float kOut = 0.055f;

    enum RoleKind
    {
        kChord,
        kScale,
        kTension
    };
    struct Role
    {
        float pref; // preferred interval, semitones from f0
        RoleKind kind;
    };
    // The ladder (see the class comment). A chord role takes the nearest chord tone within 3
    // semitones, else a scale tone; a scale role the nearest unused scale tone; a tension role the
    // nearest tension tone within 7, else a scale tone. No two voices share a note.
    static constexpr Role kRoles[kMaxVoices] = {
        {7.0f, kChord},  {12.0f, kChord},  {0.0f, kChord},   {15.5f, kChord},  {14.0f, kScale},    {-5.0f, kChord},
        {10.5f, kScale}, {-12.0f, kChord}, {8.5f, kScale},   {13.0f, kTension}, {-17.0f, kChord}, {18.0f, kTension},
    };

    static bool has(uint16_t m, int iv) { return (m >> (((iv % 12) + 12) % 12)) & 1u; }

    void placeVoices(uint16_t scale, uint16_t chord, uint16_t tension)
    {
        if (!chord)
            chord = 129; // root + fifth
        scale |= chord | 1u;
        uint64_t used = 0; // bit (iv + 24)
        auto search = [&](uint16_t m, float pref, float maxDist) -> int
        {
            int best = INT32_MIN;
            float bestD = 1e9f;
            for (int iv = -24; iv <= 26; ++iv)
            {
                if (!has(m, iv) || (used >> (iv + 24)) & 1ull)
                    continue;
                const float d = std::fabs((float)iv - pref);
                if (d <= maxDist && d < bestD)
                {
                    bestD = d;
                    best = iv;
                }
            }
            return best;
        };
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const Role &r = kRoles[v];
            int iv = INT32_MIN;
            if (r.kind == kChord)
            {
                iv = search(chord, r.pref, 3.0f);
                if (iv == INT32_MIN)
                    iv = search(scale, r.pref, 3.0f);
            }
            else if (r.kind == kScale)
                iv = search(scale, r.pref, 3.0f);
            else if (tension)
                iv = search(tension, r.pref, 7.0f);
            if (iv == INT32_MIN)
                iv = search(scale, r.pref, 99.0f);
            if (iv == INT32_MIN)
                iv = (int)r.pref;
            used |= 1ull << (iv + 24);
            note_[v] = (float)iv;
            if (!placed_)
                cur_[v] = (float)iv; // the first placement doesn't glide in from nowhere
        }
        placed_ = true;
    }

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const uint16_t scale = (uint16_t)std::clamp((int)lroundf(p_[kPdScale]), 0, 4095);
        const uint16_t chord = (uint16_t)std::clamp((int)lroundf(p_[kPdChord]), 0, 4095);
        const uint16_t tension = (uint16_t)std::clamp((int)lroundf(p_[kPdTension]), 0, 4095);
        if (!placed_ || scale != scale_ || chord != chord_ || tension != tension_)
        {
            scale_ = scale;
            chord_ = chord;
            tension_ = tension;
            placeVoices(scale, chord, tension);
        }
        const float bright = std::max(p_[kPdBright], 0.3f), hollow = std::clamp(p_[kPdHollow], 0.0f, 1.0f);
        tab_.ensure(kTableHarm, bright, hollow);

        const float f0 = std::max(p_[kPdF0], 20.0f);
        const float voices = std::clamp(p_[kPdVoices], 0.0f, (float)kMaxVoices);
        const float aAtk = 1.0f - expf(-bt / std::max(p_[kPdAttack], 0.01f));
        const float aRel = 1.0f - expf(-bt / std::max(p_[kPdRelease], 0.01f));
        const float aGlide = 1.0f - expf(-bt / std::max(p_[kPdGlide], 0.01f));
        const float bloomDecay = expf(-bt / std::max(p_[kPdBloomS], 0.05f));
        const float aBloom = 1.0f - expf(-bt / 0.04f); // the bloom's own 40 ms rise: no click
        const float bloomAmt = std::clamp(p_[kPdBloom], 0.0f, 2.0f);
        const float warp = std::clamp(p_[kPdWarp], 0.0f, 200.0f);
        const float shim = std::clamp(p_[kPdShimmer], 0.0f, 1.0f);
        const float det = std::clamp(p_[kPdDetune], 0.0f, 100.0f) / 2400.0f; // half the spread, octaves
        const float spread = std::clamp(p_[kPdSpread], 0.0f, 1.0f);

        float bloomSum = 0.0f;
        int nv = 0;
        for (int v = 0; v < kMaxVoices; ++v)
        {
            const float gt = std::clamp(voices - (float)v, 0.0f, 1.0f);
            if (gt >= 0.5f && gtPrev_[v] < 0.5f)
                bloomEnv_[v] = 1.0f; // a voice joins: it blooms
            gtPrev_[v] = gt;
            gate_[v] += (gt - gate_[v]) * (gt > gate_[v] ? aAtk : aRel);
            bloomEnv_[v] *= bloomDecay;
            bloomLvl_[v] += (bloomEnv_[v] - bloomLvl_[v]) * aBloom;
            cur_[v] += (note_[v] - cur_[v]) * aGlide;
            const float w = warp * (2.0f * warp_[v].tick(bt, p_[kPdWarpRate] * warpRate_[v], rng_) - 1.0f);
            const float f = f0 * exp2f((cur_[v] + w * 0.01f) / 12.0f);
            incA_[v] = f * exp2f(det) / sr_;
            incB_[v] = f * exp2f(-det) / sr_;
            incS_[v] = 2.0f * f * 1.0015f / sr_;
            const float sg = 1.0f - shim * (1.0f - shim_[v].tick(bt, shimRate_[v], rng_));
            ampTarget_[v] = gate_[v] * (1.0f + bloomAmt * bloomLvl_[v]) * sg;
            sparkTarget_[v] = gate_[v] * bloomAmt * bloomLvl_[v] * 0.35f;
            panGains(spread * std::clamp(pan_[v] - 0.2f, -1.0f, 1.0f), glA_[v], grA_[v]);
            panGains(spread * std::clamp(pan_[v] + 0.2f, -1.0f, 1.0f), glB_[v], grB_[v]);
            bloomSum += gate_[v] * bloomLvl_[v];
            if (ampTarget_[v] > 1e-5f || amp_[v] > 1e-5f)
                nv = v + 1;
        }

        // The low-pass opens a little with the blooms: a new flare is brighter than the chord.
        const float fc = std::clamp(p_[kPdCutoff] * (1.0f + 0.6f * std::min(bloomSum, 2.0f)), 200.0f, 0.42f * sr_);
        lpL_.set(fc, 0.6f, sr_);
        lpR_.set(fc * 1.05f, 0.6f, sr_);
        const float hp = std::clamp(p_[kPdHp], 20.0f, 2000.0f);
        hpL_.set(hp, 0.7f, sr_);
        hpR_.set(hp, 0.7f, sr_);
        const float wet = std::clamp(p_[kPdReverb], 0.0f, 1.0f);
        fdn_.set(sr_, std::max(p_[kPdReverbS], 0.2f), 1.6f);
        const float level = p_[kPdLvl] * kOut;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            float l = 0.0f, r = 0.0f;
            for (int v = 0; v < nv; ++v)
            {
                const float a = amp_[v] + (ampTarget_[v] - amp_[v]) * x;
                const float sp = spark_[v] + (sparkTarget_[v] - spark_[v]) * x;
                phA_[v] += incA_[v];
                if (phA_[v] >= 1.0f)
                    phA_[v] -= 1.0f;
                phB_[v] += incB_[v];
                if (phB_[v] >= 1.0f)
                    phB_[v] -= 1.0f;
                const float sa = tab_.lookup(phA_[v]) * a * 0.5f, sb = tab_.lookup(phB_[v]) * a * 0.5f;
                l += sa * glA_[v] + sb * glB_[v];
                r += sa * grA_[v] + sb * grB_[v];
                if (sp > 1e-5f)
                {
                    phS_[v] += incS_[v];
                    if (phS_[v] >= 1.0f)
                        phS_[v] -= 1.0f;
                    const float s = sine_.lookup(phS_[v]) * sp;
                    l += s * glB_[v];
                    r += s * grA_[v];
                }
            }
            lpL_.tick(l);
            lpR_.tick(r);
            float wl, wr;
            fdn_.process(lpL_.lp, lpR_.lp, 0.35f, wl, wr);
            hpL_.tick(lpL_.lp * (1.0f - 0.5f * wet) + wl * wet * 0.8f);
            hpR_.tick(lpR_.lp * (1.0f - 0.5f * wet) + wr * wet * 0.8f);
            // A soft ceiling for twelve voices blooming at once. Kept gentle: at 1.4x the chord's
            // difference tones (third-order intermodulation) showed up as mud below 200 Hz.
            out[2 * i] = tanhf(hpL_.hp * level * 0.6f) / 0.6f;
            out[2 * i + 1] = tanhf(hpR_.hp * level * 0.6f) / 0.6f;
        }
        for (int v = 0; v < kMaxVoices; ++v)
        {
            amp_[v] = ampTarget_[v];
            spark_[v] = sparkTarget_[v];
        }
    }

private:
    Wavetable tab_, sine_;
    Drift warp_[kMaxVoices], shim_[kMaxVoices];
    float warpRate_[kMaxVoices] = {}, shimRate_[kMaxVoices] = {}, pan_[kMaxVoices] = {};
    float note_[kMaxVoices] = {}, cur_[kMaxVoices] = {}; // interval (semitones): placed, and gliding
    float gate_[kMaxVoices] = {}, gtPrev_[kMaxVoices] = {}, bloomEnv_[kMaxVoices] = {}, bloomLvl_[kMaxVoices] = {};
    float amp_[kMaxVoices] = {}, ampTarget_[kMaxVoices] = {}, spark_[kMaxVoices] = {}, sparkTarget_[kMaxVoices] = {};
    float phA_[kMaxVoices] = {}, phB_[kMaxVoices] = {}, phS_[kMaxVoices] = {};
    float incA_[kMaxVoices] = {}, incB_[kMaxVoices] = {}, incS_[kMaxVoices] = {};
    float glA_[kMaxVoices] = {}, grA_[kMaxVoices] = {}, glB_[kMaxVoices] = {}, grB_[kMaxVoices] = {};
    uint16_t scale_ = 0, chord_ = 0, tension_ = 0;
    bool placed_ = false;
    Svf lpL_, lpR_, hpL_, hpR_;
    Fdn fdn_;
};

// ── bass (the beam-site hum) ─────────────────────────────────────────────────────────────────────
// A pedal on the tonic for where Reflect beams concentrate: a harmonic tone on f0 beating against a
// copy `beat_hz` sharp (the two sides beat against each other, so the throb moves across the head),
// the fifth above it, a sine an octave below, all driven into a soft clip and low-passed — then a
// slow SAG: the pitch drifts down by up to `bend_cents` and back (gravity_wave's queasy bass), with
// a slow amplitude throb on top. The table grows the sag, the drive and the sub with the number of
// beams concentrated near the listener.
const AmbientSynth::ParamDef kBassParams[] = {
    {"level", 1.0f},     {"f0_hz", 82.0f},     {"fifth", 0.3f},       {"sub", 0.35f},
    {"bright", 1.3f},    {"hollow", 0.3f},     {"beat_hz", 0.3f},     {"throb", 0.25f},
    {"throb_rate", 0.11f}, {"bend_cents", 0.0f}, {"bend_rate", 0.035f}, {"drive", 0.35f},
    {"cutoff_hz", 420.0f}, {"width", 0.5f},    {"hp_hz", 28.0f},
};
enum
{
    kBsLvl,
    kBsF0,
    kBsFifth,
    kBsSub,
    kBsBright,
    kBsHollow,
    kBsBeat,
    kBsThrob,
    kBsThrobRate,
    kBsBend,
    kBsBendRate,
    kBsDrive,
    kBsCutoff,
    kBsWidth,
    kBsHp
};

class BassSynth final : public AmbientSynth
{
public:
    BassSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("bass", kBassParams, (int)(sizeof(kBassParams) / sizeof(kBassParams[0])), sr, seed)
    {
        for (float &p : ph_)
            p = uni();
    }

protected:
    // Trimmed so level 1, gain 1 measures about -24 LUFS (see the file header).
    static constexpr float kOut = 0.21f;

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        tab_.ensure(8, std::max(p_[kBsBright], 0.3f), std::clamp(p_[kBsHollow], 0.0f, 1.0f));
        const float sag = p_[kBsBend] * bend_.tick(bt, p_[kBsBendRate], rng_); // down only
        const float f = std::max(p_[kBsF0], 15.0f) * exp2f(-sag / 1200.0f);
        const float beat = std::clamp(p_[kBsBeat], 0.0f, 10.0f);
        inc_[0] = f / sr_;
        inc_[1] = (f + beat) / sr_;
        inc_[2] = 1.5f * f / sr_;
        inc_[3] = (1.5f * f + 0.7f * beat) / sr_;
        inc_[4] = 0.5f * f / sr_;
        throbPh_ += kTwoPi * std::max(p_[kBsThrobRate], 0.0f) * bt;
        if (throbPh_ > kTwoPi)
            throbPh_ -= kTwoPi;
        const float th = std::clamp(p_[kBsThrob], 0.0f, 1.0f);
        const float ampTarget = 1.0f - th * (0.5f + 0.5f * sinf(throbPh_));
        const float fifth = std::clamp(p_[kBsFifth], 0.0f, 1.0f), sub = std::clamp(p_[kBsSub], 0.0f, 1.0f);
        const float w = std::clamp(p_[kBsWidth], 0.0f, 1.0f);
        const float drive = std::clamp(p_[kBsDrive], 0.0f, 1.0f);
        const float sat = 1.0f + 5.0f * drive, satInv = 1.0f / sat;
        const float fc = std::clamp(p_[kBsCutoff], 60.0f, 0.42f * sr_);
        lpL_.set(fc, 0.8f, sr_);
        lpR_.set(fc * 1.04f, 0.8f, sr_);
        hpL_.set(std::clamp(p_[kBsHp], 15.0f, 200.0f), 0.7f, sr_);
        hpR_.set(std::clamp(p_[kBsHp], 15.0f, 200.0f), 0.7f, sr_);
        const float level = p_[kBsLvl] * kOut;

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float amp = ampPrev_ + (ampTarget - ampPrev_) * x;
            for (int k = 0; k < 5; ++k)
            {
                ph_[k] += inc_[k];
                if (ph_[k] >= 1.0f)
                    ph_[k] -= 1.0f;
            }
            const float a = tab_.lookup(ph_[0]), b = tab_.lookup(ph_[1]);
            const float c = tab_.lookup(ph_[2]) * fifth, d = tab_.lookup(ph_[3]) * fifth;
            const float s = sinf(kTwoPi * ph_[4]) * sub;
            // The beating pair split across the sides by `width`: the throb of the beat moves.
            float l = (a * (1.0f - 0.5f * w) + b * 0.5f * w) * 0.6f + (c * (1.0f - 0.5f * w) + d * 0.5f * w) * 0.35f + s;
            float r = (b * (1.0f - 0.5f * w) + a * 0.5f * w) * 0.6f + (d * (1.0f - 0.5f * w) + c * 0.5f * w) * 0.35f + s;
            l = tanhf(l * amp * sat) * satInv * (1.0f + drive);
            r = tanhf(r * amp * sat) * satInv * (1.0f + drive);
            lpL_.tick(l);
            lpR_.tick(r);
            hpL_.tick(lpL_.lp);
            hpR_.tick(lpR_.lp);
            out[2 * i] = hpL_.hp * level;
            out[2 * i + 1] = hpR_.hp * level;
        }
        ampPrev_ = ampTarget;
    }

private:
    Wavetable tab_;
    Drift bend_;
    float ph_[5] = {}, inc_[5] = {};
    float throbPh_ = 0.0f, ampPrev_ = 1.0f;
    Svf lpL_, lpR_, hpL_, hpR_;
};

// ── surf ─────────────────────────────────────────────────────────────────────────────────────────
// Breaking waves. Each wave: a swell that builds (low roar rising), the break (a bright broadband
// burst), a roar that opens up and dies away, then the wash — fizz whose band slides down as the
// water drains back. Two wave trains at incommensurate periods keep it from sounding like a
// metronome, over a constant bed of distant surf. Pink noise, not brown, and high-passed at 55 Hz:
// real surf is broadband; a brown-noise roar was a sub-bass wall with no wave in it.
// `distance` 0 = on the beach, 1 = open ocean out of sight of the break (no crash, a soft low wash).
const AmbientSynth::ParamDef kSurfParams[] = {
    {"level", 1.0f}, {"period", 10.0f},  {"jitter", 0.3f},  {"roar_hz", 600.0f}, {"fizz", 0.6f},
    {"fizz_hz", 3200.0f}, {"distance", 0.0f}, {"bed", 0.3f}, {"width", 0.7f}, {"crash", 0.7f},
};
enum
{
    kSLevel,
    kSPeriod,
    kSJitter,
    kSRoarHz,
    kSFizz,
    kSFizzHz,
    kSDistance,
    kSBed,
    kSWidth,
    kSCrash
};

class SurfSynth final : public AmbientSynth
{
public:
    SurfSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("surf", kSurfParams, (int)(sizeof(kSurfParams) / sizeof(kSurfParams[0])), sr, seed)
    {
        trains_[0].t = 0.3f;
        trains_[1].t = 0.75f;
        trains_[1].scale = 1.37f;
        trains_[1].amp = 0.55f;
    }

protected:
    static constexpr float kBreak = 0.42f; // phase of the break within a wave

    struct Train
    {
        float t = 0.0f; // phase within the current wave, 0..1
        float dur = 10.0f;
        float scale = 1.0f, amp = 1.0f, size = 1.0f, pan = 0.0f;
        float sinceBreak = 100.0f; // seconds
    };

    // Swell+roar and wash envelopes of one wave at phase x.
    static void envelopes(float x, float &roar, float &wash)
    {
        if (x < kBreak)
        {
            const float b = x / kBreak;
            roar = 0.3f * b * b;
        }
        else
        {
            const float y = x - kBreak;
            roar = std::min(1.0f, 0.3f + y / 0.02f) * expf(-y / 0.12f);
        }
        const float y = x - (kBreak + 0.04f);
        wash = y > 0.0f ? std::min(1.0f, y / 0.05f) * expf(-y / 0.2f) : 0.0f;
    }

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float dist = std::clamp(p_[kSDistance], 0.0f, 1.0f);
        float roarT = 0.0f, washT = 0.0f, crashT = 0.0f, panR = 0.0f, panW = 0.0f;
        for (Train &tr : trains_)
        {
            const float before = tr.t;
            tr.t += bt / tr.dur;
            tr.sinceBreak += bt;
            if (before < kBreak && tr.t >= kBreak)
                tr.sinceBreak = 0.0f;
            if (tr.t >= 1.0f)
            {
                tr.t -= 1.0f;
                tr.dur = std::max(2.0f, p_[kSPeriod] * tr.scale * (1.0f + p_[kSJitter] * bip()));
                tr.size = 0.45f + 0.55f * uni();
                tr.pan = bip() * 0.5f;
            }
            float r, w;
            envelopes(tr.t, r, w);
            const float a = tr.amp * tr.size;
            roarT += r * a;
            washT += w * a;
            crashT += a * expf(-tr.sinceBreak / 0.35f);
            panR += r * a * tr.pan;
            panW += w * a * tr.pan;
        }
        const float near = 1.0f - 0.85f * dist;
        const float roarPan = roarT > 1e-4f ? panR / roarT * near : 0.0f;
        const float washPan = washT > 1e-4f ? panW / washT : 0.0f;
        roarT *= 0.35f + 0.65f * near;
        washT *= near * p_[kSFizz];
        crashT *= near * near * p_[kSCrash];
        const float open = std::min(roarT, 1.2f);
        const float rhz = p_[kSRoarHz] * (1.0f - 0.55f * dist);
        roarL_.set(rhz * (0.7f + 2.0f * open), 0.7f, sr_);
        roarR_.set(rhz * (0.74f + 2.0f * open), 0.7f, sr_);
        crashL_.set(5000.0f * (1.0f - 0.6f * dist), 0.6f, sr_);
        crashR_.set(5300.0f * (1.0f - 0.6f * dist), 0.6f, sr_);
        const float fz = p_[kSFizzHz] * (0.5f + 0.8f * std::min(washT / std::max(p_[kSFizz], 0.05f), 1.0f));
        fizzL_.set(fz, 0.8f, sr_);
        fizzR_.set(fz * 1.08f, 0.8f, sr_);
        bedL_.set(450.0f * (1.0f - 0.4f * dist), 0.6f, sr_);
        bedR_.set(480.0f * (1.0f - 0.4f * dist), 0.6f, sr_);
        hpL_.set(55.0f, 0.7f, sr_);
        hpR_.set(55.0f, 0.7f, sr_);
        const float w = std::clamp(p_[kSWidth], 0.0f, 1.0f);
        const float bed = p_[kSBed] + 0.3f * dist;
        const float lvl = p_[kSLevel] * 1.1f;
        float gRl, gRr, gWl, gWr;
        panGains(roarPan, gRl, gRr);
        panGains(washPan, gWl, gWr);

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float roar = roarPrev_ + (roarT - roarPrev_) * x;
            const float wash = washPrev_ + (washT - washPrev_) * x;
            const float crash = crashPrev_ + (crashT - crashPrev_) * x;
            const float wc = bip();
            const float nl = wc * (1.0f - w) + bip() * w;
            const float nr = wc * (1.0f - w) + bip() * w;
            const float pl = pinkL_.tick(nl), pr = pinkR_.tick(nr);
            roarL_.tick(pl);
            roarR_.tick(pr);
            crashL_.tick(nl);
            crashR_.tick(nr);
            fizzL_.tick(nl);
            fizzR_.tick(nr);
            bedL_.tick(pl);
            bedR_.tick(pr);
            hpL_.tick(roarL_.lp * roar * gRl * 1.41f + crashL_.lp * crash * 0.35f + fizzL_.band() * wash * gWl * 0.45f +
                      bedL_.lp * bed);
            hpR_.tick(roarR_.lp * roar * gRr * 1.41f + crashR_.lp * crash * 0.35f + fizzR_.band() * wash * gWr * 0.45f +
                      bedR_.lp * bed);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        roarPrev_ = roarT;
        washPrev_ = washT;
        crashPrev_ = crashT;
    }

private:
    Train trains_[2];
    Svf roarL_, roarR_, crashL_, crashR_, fizzL_, fizzR_, bedL_, bedR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    float roarPrev_ = 0.0f, washPrev_ = 0.0f, crashPrev_ = 0.0f;
};

// ── rain ─────────────────────────────────────────────────────────────────────────────────────────
// Rain around the listener (2026-09-28; driven by the cloud field's rain rate at the eye). Three parts:
//   hiss    the sum of countless distant impacts: white noise through a broad band-pass (2-6 kHz),
//           whose centre drops a little and whose level rises with the intensity
//   rumble  heavy rain only: pink noise low-passed (~500 Hz), the roar of a downpour on everything
//   drops   individual near impacts: a Poisson stream of short decaying noise ticks, each through its
//           own resonance (1.2-6 kHz), level (heavy-tailed) and pan; the rate follows the intensity
// `intensity` 0..1 (a drizzle .. a cumulonimbus core), slowly gusting (+-20%) on its own.
const AmbientSynth::ParamDef kRainParams[] = {
    {"level", 1.0f}, {"intensity", 0.5f}, {"drops", 1.0f}, {"hiss_hz", 3500.0f}, {"rumble", 1.0f}, {"width", 0.8f},
};
enum
{
    kRLevel,
    kRIntensity,
    kRDrops,
    kRHissHz,
    kRRumble,
    kRWidth
};

class RainSynth final : public AmbientSynth
{
public:
    RainSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("rain", kRainParams, (int)(sizeof(kRainParams) / sizeof(kRainParams[0])), sr, seed)
    {
    }

protected:
    static constexpr int kDrops = 12;
    struct Drop
    {
        Svf f;
        float env = 0.0f, decay = 0.0f, gl = 0.0f, gr = 0.0f;
    };

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const float gust = 0.8f + 0.4f * gust_.tick(bt, 0.15f, rng_);
        const float I = std::clamp(p_[kRIntensity] * gust, 0.0f, 1.2f);
        const float hz = p_[kRHissHz] * (1.1f - 0.25f * std::min(I, 1.0f));
        hissL_.set(hz, 0.55f, sr_);
        hissR_.set(hz * 1.07f, 0.55f, sr_);
        rumL_.set(420.0f + 200.0f * I, 0.6f, sr_);
        rumR_.set(450.0f + 200.0f * I, 0.6f, sr_);
        hpL_.set(60.0f, 0.7f, sr_);
        hpR_.set(60.0f, 0.7f, sr_);
        const float hiss = 0.25f * sqrtf(I);
        const float rumble = p_[kRRumble] * 0.6f * I * I;
        const float rate = p_[kRDrops] * (6.0f + 260.0f * I) / sr_; // impacts per sample
        const float w = std::clamp(p_[kRWidth], 0.0f, 1.0f);
        const float lvl = p_[kRLevel];

        for (uint32_t i = 0; i < n; ++i)
        {
            const float x = (float)(i + 1) / (float)n;
            const float hissG = hissPrev_ + (hiss - hissPrev_) * x;
            const float rumG = rumPrev_ + (rumble - rumPrev_) * x;
            if (uni() < rate)
            {
                Drop &d = drops_[next_];
                next_ = (next_ + 1) % kDrops;
                d.f.set(1200.0f * powf(5.0f, uni()), 4.0f + 6.0f * uni(), sr_);
                const float a = 0.15f + 0.85f * powf(uni(), 3.0f); // mostly faint, a few close
                d.env = a;
                d.decay = expf(-1.0f / (sr_ * (0.002f + 0.006f * uni())));
                panGains(bip() * 0.9f, d.gl, d.gr);
            }
            const float wc = bip();
            const float nl = wc * (1.0f - w) + bip() * w;
            const float nr = wc * (1.0f - w) + bip() * w;
            hissL_.tick(nl);
            hissR_.tick(nr);
            rumL_.tick(pinkL_.tick(nl));
            rumR_.tick(pinkR_.tick(nr));
            float dl = 0.0f, dr = 0.0f;
            for (Drop &d : drops_)
            {
                if (d.env < 1e-4f)
                    continue;
                d.f.tick(wc * d.env);
                d.env *= d.decay;
                dl += d.f.band() * d.gl;
                dr += d.f.band() * d.gr;
            }
            hpL_.tick(hissL_.band() * hissG + rumL_.lp * rumG + dl * 0.8f);
            hpR_.tick(hissR_.band() * hissG + rumR_.lp * rumG + dr * 0.8f);
            out[2 * i] = hpL_.hp * lvl;
            out[2 * i + 1] = hpR_.hp * lvl;
        }
        hissPrev_ = hiss;
        rumPrev_ = rumble;
    }

private:
    Drop drops_[kDrops];
    int next_ = 0;
    Drift gust_;
    Svf hissL_, hissR_, rumL_, rumR_, hpL_, hpR_;
    Pink pinkL_, pinkR_;
    float hissPrev_ = 0.0f, rumPrev_ = 0.0f;
};

// ── thunder ─────────────────────────────────────────────────────────────────────────────────────────
// One roll of thunder per trigger (the sim schedules it at the flash's distance / 343 m/s, from the
// lightning flash list; SatelliteSimAmbience.cpp). A roll is low-passed noise under an envelope of
// overlapping "peals" — each a swell and decay as the sound from another stretch of the channel
// arrives — that thin out over the roll; close strikes open with a crack. Distance does most of the
// work, as it does outdoors: it lowers the cutoff (the air takes the highs), slows the onset (the
// channel is kilometres long, so the sound arrives smeared), lengthens the roll and lowers the level.
// Params (all snap): trigger (a counter: any change starts a roll), distance_km, energy (a ground
// strike ~1, in cloud ~0.5), pan.
const AmbientSynth::ParamDef kThunderParams[] = {
    {"level", 1.0f}, {"trigger", 0.0f, true}, {"distance_km", 5.0f, true}, {"energy", 1.0f, true}, {"pan", 0.0f, true},
};
enum
{
    kTLevel,
    kTTrigger,
    kTDist,
    kTEnergy,
    kTPan,
};

class ThunderSynth final : public AmbientSynth
{
public:
    ThunderSynth(uint32_t sr, uint32_t seed)
        : AmbientSynth("thunder", kThunderParams, (int)(sizeof(kThunderParams) / sizeof(kThunderParams[0])), sr, seed)
    {
    }

protected:
    static constexpr int kRolls = 6;
    struct Roll
    {
        bool on = false;
        float t = 0.0f, dur = 0.0f, rise = 0.0f, gain = 0.0f, gl = 0.0f, gr = 0.0f;
        float crack = 0.0f, peal = 0.0f, pealTarget = 0.0f, pealRate = 0.0f, pealDecay = 0.0f, base = 0.0f;
        Svf lpL, lpR, crHp;
        Brown brL, brR;
    };

    void start()
    {
        Roll &r = rolls_[next_];
        next_ = (next_ + 1) % kRolls;
        const float d = std::clamp(p_[kTDist], 0.1f, 40.0f);
        const float e = std::clamp(p_[kTEnergy], 0.0f, 2.0f);
        r = Roll{};
        r.on = true;
        r.dur = std::min(4.0f + 0.9f * d + 2.0f * uni(), 18.0f);
        r.rise = 0.02f + 0.07f * d;
        r.gain = e * 1.6f / (1.0f + d / 2.5f);
        const float fc = std::clamp(3000.0f / (1.0f + d / 0.7f), 70.0f, 3000.0f);
        r.lpL.set(fc, 0.6f, sr_);
        r.lpR.set(fc * 1.04f, 0.6f, sr_);
        r.crHp.set(900.0f, 0.7f, sr_);
        r.crack = e * std::clamp(1.0f - d / 2.5f, 0.0f, 1.0f);
        r.pealRate = 0.8f + 0.8f * uni();
        r.pealDecay = expf(-1.0f / (sr_ * (0.35f + 0.5f * uni())));
        r.base = 0.15f;
        attack_ = 1.0f - expf(-1.0f / (sr_ * 0.05f));
        panGains(std::clamp(p_[kTPan], -1.0f, 1.0f) * 0.8f, r.gl, r.gr);
    }

    void block(float *out, uint32_t n) override
    {
        if (p_[kTTrigger] != lastTrig_)
        {
            lastTrig_ = p_[kTTrigger];
            start();
        }
        const float lvl = p_[kTLevel];
        const float dt = 1.0f / sr_;
        for (uint32_t i = 0; i < n; ++i)
        {
            float l = 0.0f, rr = 0.0f;
            for (Roll &r : rolls_)
            {
                if (!r.on)
                    continue;
                r.t += dt;
                if (r.t > r.dur)
                {
                    r.on = false;
                    continue;
                }
                // Peals: a Poisson stream thinning out over the roll, each a swell (~50 ms attack: an
                // instant jump clicked) and a slow decay.
                const float rate = r.pealRate * expf(-r.t / (0.4f * r.dur));
                if (uni() < rate * dt)
                    r.pealTarget += 0.25f + 2.0f * uni() * uni() * uni();
                r.pealTarget *= r.pealDecay;
                r.peal += (r.pealTarget - r.peal) * attack_;
                const float onset = std::min(r.t / r.rise, 1.0f);
                const float fade = 1.0f - smooth01((r.t - 0.55f * r.dur) / (0.45f * r.dur));
                const float env = (r.peal + r.base * expf(-r.t / (0.2f * r.dur))) * onset * onset * fade;
                const float wl = bip(), wr = bip();
                r.lpL.tick(r.brL.tick(wl) * 2.0f + wl * 0.3f);
                r.lpR.tick(r.brR.tick(wr) * 2.0f + wr * 0.3f);
                float o = 0.0f;
                if (r.crack > 0.0f && r.t < 0.25f)
                {
                    r.crHp.tick(bip());
                    o = r.crHp.hp * r.crack * expf(-r.t / 0.05f) * 1.5f;
                }
                // Scaled so a strike 1 km away rolls at ~-24 LUFS (the ambience convention), -35 at 15 km.
                l += (r.lpL.lp * env * 0.24f + o * 0.07f) * r.gain * r.gl;
                rr += (r.lpR.lp * env * 0.24f + o * 0.07f) * r.gain * r.gr;
            }
            // A soft limit: the loudest peals of a close strike stacked past full scale.
            out[2 * i] = softLimit(l * lvl);
            out[2 * i + 1] = softLimit(rr * lvl);
        }
    }
    static float smooth01(float x)
    {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3.0f - 2.0f * x);
    }
    static float softLimit(float x) { return x / (1.0f + fabsf(x) * 0.5f); }

private:
    float attack_ = 0.0005f;   // the peals' swell (set from the rate in start())
    Roll rolls_[kRolls];
    int next_ = 0;
    float lastTrig_ = 0.0f;
};
} // namespace

// ── AmbientSynth base ────────────────────────────────────────────────────────────────────────────
const ma_data_source_vtable AmbientSynth::kVtable = {
    &AmbientSynth::dsRead, &AmbientSynth::dsSeek, &AmbientSynth::dsFormat,
    &AmbientSynth::dsCursor, &AmbientSynth::dsLength, nullptr, 0};

AmbientSynth::AmbientSynth(const char *kind, const ParamDef *defs, int count, uint32_t sampleRate, uint32_t seed)
    : sr_((float)sampleRate), rng_(seed ? seed : 0x9E3779B9u), kind_(kind), defs_(defs),
      paramCount_(std::min(count, kMaxParams))
{
    for (int i = 0; i < kMaxParams; ++i)
        target_[i].store(i < paramCount_ ? defs_[i].def : 0.0f, std::memory_order_relaxed);
    ma_data_source_config cfg = ma_data_source_config_init();
    cfg.vtable = &kVtable;
    ma_data_source_init(&cfg, &ds_.base);
    ds_.self = this;
}

AmbientSynth::~AmbientSynth() { ma_data_source_uninit(&ds_.base); }

std::unique_ptr<AmbientSynth> AmbientSynth::create(const std::string &kind, uint32_t sampleRate, uint32_t seed)
{
    if (kind == "wind")
        return std::make_unique<WindSynth>(sampleRate, seed);
    if (kind == "hum")
        return std::make_unique<HumSynth>(sampleRate, seed);
    if (kind == "drone")
        return std::make_unique<DroneSynth>(sampleRate, seed);
    if (kind == "beeps")
        return std::make_unique<BeepSynth>(sampleRate, seed);
    if (kind == "disk")
        return std::make_unique<DiskSynth>(sampleRate, seed);
    if (kind == "chorus")
        return std::make_unique<ChorusSynth>(sampleRate, seed);
    if (kind == "surf")
        return std::make_unique<SurfSynth>(sampleRate, seed);
    if (kind == "saw")
        return std::make_unique<SawSynth>(sampleRate, seed);
    if (kind == "pad")
        return std::make_unique<PadSynth>(sampleRate, seed);
    if (kind == "bass")
        return std::make_unique<BassSynth>(sampleRate, seed);
    if (kind == "rain")
        return std::make_unique<RainSynth>(sampleRate, seed);
    if (kind == "thunder")
        return std::make_unique<ThunderSynth>(sampleRate, seed);
    return nullptr;
}

std::vector<std::string> AmbientSynth::kinds()
{
    return {"wind", "hum", "drone", "beeps", "disk", "chorus", "surf", "saw", "pad", "bass", "rain", "thunder"};
}

int AmbientSynth::paramIndex(const std::string &name) const
{
    for (int i = 0; i < paramCount_; ++i)
        if (name == defs_[i].name)
            return i;
    return -1;
}

void AmbientSynth::setParam(int i, float v)
{
    if (i >= 0 && i < paramCount_ && std::isfinite(v))
        target_[i].store(v, std::memory_order_relaxed);
}

float AmbientSynth::param(int i) const
{
    return (i >= 0 && i < paramCount_) ? target_[i].load(std::memory_order_relaxed) : 0.0f;
}

float AmbientSynth::uni() { return rnd(rng_); }
float AmbientSynth::bip() { return rnd(rng_) * 2.0f - 1.0f; }
float AmbientSynth::gauss() { return (rnd(rng_) + rnd(rng_) + rnd(rng_) + rnd(rng_) - 2.0f) * 1.732f; }

void AmbientSynth::render(float *out, uint32_t frames)
{
    while (frames > 0)
    {
        const uint32_t n = std::min(frames, kBlock);
        const float a = first_ ? 1.0f : blockEase((float)n, sr_, 0.12f);
        for (int i = 0; i < paramCount_; ++i)
        {
            const float t = target_[i].load(std::memory_order_relaxed);
            p_[i] = defs_[i].snap ? t : p_[i] + (t - p_[i]) * a;
        }
        first_ = false;
        block(out, n);
        out += 2 * n;
        frames -= n;
    }
}

ma_result AmbientSynth::dsRead(ma_data_source *ds, void *out, ma_uint64 frames, ma_uint64 *read)
{
    auto *self = reinterpret_cast<DataSource *>(ds)->self;
    float *o = static_cast<float *>(out);
    ma_uint64 left = frames;
    while (left > 0)
    {
        const uint32_t n = (uint32_t)std::min<ma_uint64>(left, 4096);
        self->render(o, n);
        o += 2 * n;
        left -= n;
    }
    if (read)
        *read = frames;
    return MA_SUCCESS;
}

ma_result AmbientSynth::dsSeek(ma_data_source *, ma_uint64) { return MA_SUCCESS; }

ma_result AmbientSynth::dsFormat(ma_data_source *ds, ma_format *fmt, ma_uint32 *ch, ma_uint32 *sr, ma_channel *map,
                                 size_t mapCap)
{
    auto *self = reinterpret_cast<DataSource *>(ds)->self;
    if (fmt)
        *fmt = ma_format_f32;
    if (ch)
        *ch = 2;
    if (sr)
        *sr = (ma_uint32)self->sr_;
    if (map)
        ma_channel_map_init_standard(ma_standard_channel_map_default, map, mapCap, 2);
    return MA_SUCCESS;
}

ma_result AmbientSynth::dsCursor(ma_data_source *, ma_uint64 *cursor)
{
    if (cursor)
        *cursor = 0;
    return MA_SUCCESS;
}

ma_result AmbientSynth::dsLength(ma_data_source *, ma_uint64 *len)
{
    if (len)
        *len = 0;
    return MA_NOT_IMPLEMENTED; // infinite
}
