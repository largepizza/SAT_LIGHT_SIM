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
    static constexpr int kTab = 2048;
    // Consonant ratios over the tone base (just intonation: 1, 9/8, 5/4, 3/2, 5/3, 2) and a few
    // short motifs over them. Sparse, soft and repeated: status chirps, not a tune.
    static constexpr float kRatios[6] = {1.0f, 1.125f, 1.25f, 1.5f, 1.6667f, 2.0f};
    static constexpr int kMotifs[7][3] = {{0, 0, -1}, {0, 3, -1}, {0, 2, 3}, {3, 0, -1}, {0, 5, -1}, {3, 2, 0}, {0, 3, 5}};

    struct Note
    {
        bool on = false;
        float t = 0.0f, dur = 0.2f, f = 220.0f, ph = 0.0f, amp = 0.0f, gl = 0.7f, gr = 0.7f;
    };

    void rebuildTable(int harm, float bright, float odd)
    {
        float peak = 1e-6f;
        for (int i = 0; i < kTab; ++i)
        {
            const float x = kTwoPi * (float)i / (float)kTab;
            float v = 0.0f;
            for (int h = 1; h <= harm; ++h)
            {
                const float a = powf((float)h, -bright) * ((h % 2 == 0) ? (1.0f - odd) : 1.0f);
                v += a * sinf((float)h * x);
            }
            tab_[i] = v;
            peak = std::max(peak, std::fabs(v));
        }
        for (float &v : tab_)
            v /= peak;
        tabHarm_ = harm;
        tabBright_ = bright;
        tabOdd_ = odd;
    }
    float lookup(float ph) const
    {
        const float p = ph * (float)kTab;
        const int i0 = (int)p;
        const float f = p - (float)i0;
        return tab_[i0 & (kTab - 1)] * (1.0f - f) + tab_[(i0 + 1) & (kTab - 1)] * f;
    }

    void block(float *out, uint32_t n) override
    {
        const float bt = (float)n / sr_;
        const int harm = std::clamp((int)lroundf(p_[kDHarm]), 1, 12);
        const float bright = std::max(p_[kDBright], 0.0f), odd = std::clamp(p_[kDOdd], 0.0f, 1.0f);
        if (harm != tabHarm_ || std::fabs(bright - tabBright_) > 0.01f || std::fabs(odd - tabOdd_) > 0.01f)
            rebuildTable(harm, bright, odd);

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
            const float a = lookup(ph_[0]), b = lookup(ph_[1]), c = lookup(ph_[2]);
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
    float tab_[kTab] = {};
    int tabHarm_ = -1;
    float tabBright_ = -1.0f, tabOdd_ = -1.0f;
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
    return nullptr;
}

std::vector<std::string> AmbientSynth::kinds() { return {"wind", "hum", "drone", "beeps", "disk", "chorus", "surf"}; }

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
            p_[i] += (t - p_[i]) * a;
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
