// MusicAnalysis.cpp — key, tuning and chords of the soundtrack. See MusicAnalysis.h.
#include "MusicAnalysis.h"

#include "miniaudio.h" // declarations only: the implementation is compiled once (AudioSystem.cpp)

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>

namespace music
{
namespace
{
constexpr int kSr = 11025;
constexpr int kN = 8192;   // FFT size: 1.35 Hz bins
constexpr int kHop = 2048; // 0.19 s
constexpr double kPi = 3.14159265358979323846;
constexpr float kCurveWindowS = 8.0f; // tuning curve: circular mean over this window ...
constexpr float kCurveStepS = 1.0f;   // ... every this many seconds
constexpr float kChordSegS = 3.0f;

// Krumhansl-Kessler key profiles (C major / C minor).
constexpr float kMajorProfile[12] = {6.35f, 2.23f, 3.48f, 2.33f, 4.38f, 4.09f, 2.52f, 5.19f, 2.39f, 3.66f, 2.29f, 2.88f};
constexpr float kMinorProfile[12] = {6.33f, 2.68f, 3.52f, 5.38f, 2.60f, 3.53f, 2.54f, 4.75f, 3.98f, 2.69f, 3.34f, 3.17f};

struct NamedScale
{
    const char *name;
    uint16_t rel;
};
constexpr uint16_t bits(std::initializer_list<int> iv)
{
    uint16_t m = 0;
    for (int k : iv)
        m |= (uint16_t)(1u << k);
    return m;
}
const NamedScale kScales[] = {
    {"Ionian (major)", bits({0, 2, 4, 5, 7, 9, 11})},
    {"Dorian", bits({0, 2, 3, 5, 7, 9, 10})},
    {"Phrygian", bits({0, 1, 3, 5, 7, 8, 10})},
    {"Lydian", bits({0, 2, 4, 6, 7, 9, 11})},
    {"Mixolydian", bits({0, 2, 4, 5, 7, 9, 10})},
    {"Aeolian (minor)", bits({0, 2, 3, 5, 7, 8, 10})},
    {"Locrian", bits({0, 1, 3, 5, 6, 8, 10})},
    {"harmonic minor", bits({0, 2, 3, 5, 7, 8, 11})},
    {"melodic minor", bits({0, 2, 3, 5, 7, 9, 11})},
    {"Phrygian dominant", bits({0, 1, 4, 5, 7, 8, 10})},
    {"Hungarian minor", bits({0, 2, 3, 6, 7, 8, 11})},
    {"double harmonic", bits({0, 1, 4, 5, 7, 8, 11})},
    {"major pentatonic", bits({0, 2, 4, 7, 9})},
    {"minor pentatonic", bits({0, 3, 5, 7, 10})},
    {"suspended pentatonic", bits({0, 2, 5, 7, 10})},
    {"hirajoshi", bits({0, 2, 3, 7, 8})},
    {"in-sen", bits({0, 1, 5, 7, 10})},
    {"whole tone", bits({0, 2, 4, 6, 8, 10})},
};

struct ChordTemplate
{
    const char *quality;
    int iv[3];
    int n;
};
const ChordTemplate kChordTemplates[] = {
    {"", {0, 4, 7}, 3},    {"m", {0, 3, 7}, 3},    {"dim", {0, 3, 6}, 3}, {"aug", {0, 4, 8}, 3},
    {"sus2", {0, 2, 7}, 3}, {"sus4", {0, 5, 7}, 3}, {"5", {0, 7, 0}, 2},
};

struct Peak
{
    float midi; // fractional MIDI note
    float amp;
};

// Iterative radix-2 FFT, in place.
void fft(std::vector<std::complex<float>> &a, const std::vector<std::complex<float>> &tw)
{
    const int n = (int)a.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const int step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < len / 2; ++k)
            {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * tw[(size_t)k * step];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

float centsFromVector(double c, double s) { return (float)(std::atan2(s, c) / (2.0 * kPi) * 100.0); }

int rotl12(int pc) { return ((pc % 12) + 12) % 12; }

float pearson(const float *a, const float *b)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; ++i)
    {
        ma += a[i];
        mb += b[i];
    }
    ma /= 12.0;
    mb /= 12.0;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < 12; ++i)
    {
        sab += (a[i] - ma) * (b[i] - mb);
        saa += (a[i] - ma) * (a[i] - ma);
        sbb += (b[i] - mb) * (b[i] - mb);
    }
    return (saa > 0 && sbb > 0) ? (float)(sab / std::sqrt(saa * sbb)) : 0.0f;
}
} // namespace

// ── helpers ──────────────────────────────────────────────────────────────────────────────────────
std::string pcName(int pc)
{
    static const char *k[12] = {"C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"};
    return pc < 0 ? "-" : k[rotl12(pc)];
}

uint16_t toRelative(uint16_t absMask, int tonicPc)
{
    uint16_t r = 0;
    for (int k = 0; k < 12; ++k)
        if (absMask & (1u << rotl12(tonicPc + k)))
            r |= (uint16_t)(1u << k);
    return r;
}

std::string intervalList(uint16_t relMask)
{
    static const char *k[12] = {"1", "b2", "2", "b3", "3", "4", "#4", "5", "b6", "6", "b7", "7"};
    std::string s;
    for (int i = 0; i < 12; ++i)
        if (relMask & (1u << i))
            s += (s.empty() ? "" : " ") + std::string(k[i]);
    return s;
}

std::string chordName(const Chord &c) { return c.rootPc < 0 ? "N" : pcName(c.rootPc) + c.quality; }

std::string Analysis::keyName() const { return pcName(tonicPc) + (minor ? " minor" : " major"); }

float Analysis::tuningAt(float t) const
{
    if (tuningCurve.empty())
        return tuningCents;
    const float x = std::clamp(t / curveStepS, 0.0f, (float)(tuningCurve.size() - 1));
    const size_t i0 = (size_t)x;
    const size_t i1 = std::min(i0 + 1, tuningCurve.size() - 1);
    const float f = x - (float)i0;
    return tuningCurve[i0] + (tuningCurve[i1] - tuningCurve[i0]) * f;
}

const Chord *Analysis::chordAt(float t) const
{
    for (const Chord &c : chords)
        if (t >= c.t0 && t < c.t1)
            return c.rootPc >= 0 ? &c : nullptr;
    return nullptr;
}

double Analysis::tonicHz(double lowHz, float cents) const
{
    // MIDI 69 = A4 = 440 Hz; pick the octave of the tonic that lands in [lowHz, 2 lowHz).
    double f = 440.0 * std::pow(2.0, ((double)(tonicPc - 9) + cents / 100.0) / 12.0);
    while (f >= 2.0 * lowHz)
        f *= 0.5;
    while (f < lowHz)
        f *= 2.0;
    return f;
}

// ── analysis ─────────────────────────────────────────────────────────────────────────────────────
bool analyzeFile(const std::string &path, Analysis &out, std::string &err, const std::atomic<bool> *cancel)
{
    auto cancelled = [&] { return cancel && cancel->load(); };
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, kSr);
    ma_decoder dec;
    if (ma_decoder_init_file(path.c_str(), &cfg, &dec) != MA_SUCCESS)
    {
        err = "cannot decode " + path;
        return false;
    }
    std::vector<float> x;
    {
        std::vector<float> buf(16384);
        for (;;)
        {
            ma_uint64 got = 0;
            const ma_result r = ma_decoder_read_pcm_frames(&dec, buf.data(), buf.size(), &got);
            x.insert(x.end(), buf.begin(), buf.begin() + (size_t)got);
            if (r != MA_SUCCESS || got == 0 || cancelled())
                break;
        }
    }
    ma_decoder_uninit(&dec);
    if (cancelled())
    {
        err = "cancelled";
        return false;
    }
    if ((int)x.size() < kN * 2)
    {
        err = path + ": too short to analyse";
        return false;
    }

    Analysis a = out; // keeps the caller's file / size / time
    a.durationS = (float)x.size() / (float)kSr;
    const int frames = (int)((x.size() - kN) / kHop) + 1;

    std::vector<std::complex<float>> tw(kN / 2), buf(kN);
    for (int k = 0; k < kN / 2; ++k)
        tw[k] = std::polar(1.0f, (float)(-2.0 * kPi * k / kN));
    std::vector<float> win(kN), mag(kN / 2 + 1);
    for (int i = 0; i < kN; ++i)
        win[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * i / (kN - 1)));

    std::vector<std::vector<Peak>> peaks(frames);
    std::vector<double> tunC(frames, 0.0), tunS(frames, 0.0);
    std::vector<float> rms(frames, 0.0f);
    const float binHz = (float)kSr / (float)kN;
    const int kLo = (int)std::ceil(30.0f / binHz), kHi = (int)(2500.0f / binHz);

    for (int f = 0; f < frames; ++f)
    {
        if ((f & 63) == 0 && cancelled())
        {
            err = "cancelled";
            return false;
        }
        const float *seg = x.data() + (size_t)f * kHop;
        double e = 0.0;
        for (int i = 0; i < kN; ++i)
        {
            const float v = seg[i] * win[i];
            buf[i] = {v, 0.0f};
            e += (double)v * v;
        }
        rms[f] = (float)std::sqrt(e / kN);
        fft(buf, tw);
        for (int k = 0; k <= kN / 2; ++k)
            mag[k] = std::abs(buf[k]);

        std::vector<Peak> &pk = peaks[f];
        float maxMag = 0.0f;
        for (int k = kLo; k <= kHi; ++k)
            if (mag[k] > mag[k - 1] && mag[k] >= mag[k + 1])
                maxMag = std::max(maxMag, mag[k]);
        if (maxMag <= 1e-6f)
            continue;
        const float thr = 0.02f * maxMag;
        for (int k = kLo; k <= kHi; ++k)
        {
            if (!(mag[k] > mag[k - 1] && mag[k] >= mag[k + 1]) || mag[k] < thr)
                continue;
            // Parabolic interpolation on the log magnitude: sub-bin frequency.
            const float la = std::log(mag[k - 1] + 1e-12f), lb = std::log(mag[k] + 1e-12f), lc = std::log(mag[k + 1] + 1e-12f);
            const float den = la - 2.0f * lb + lc;
            const float d = std::fabs(den) > 1e-12f ? 0.5f * (la - lc) / den : 0.0f;
            const float hz = ((float)k + std::clamp(d, -0.5f, 0.5f)) * binHz;
            pk.push_back({69.0f + 12.0f * std::log2(hz / 440.0f), mag[k]});
        }
        // The strongest 96 only: the rest are sidelobes and noise.
        if (pk.size() > 96)
        {
            std::nth_element(pk.begin(), pk.begin() + 96, pk.end(), [](const Peak &p, const Peak &q) { return p.amp > q.amp; });
            pk.resize(96);
        }
        // Tuning: the phase of every peak above ~80 Hz against the semitone grid.
        for (const Peak &p : pk)
        {
            if (p.midi < 39.5f) // 80 Hz: below it a bin is a sizeable fraction of a semitone
                continue;
            const double dev = p.midi - std::round(p.midi);
            tunC[f] += p.amp * std::cos(2.0 * kPi * dev);
            tunS[f] += p.amp * std::sin(2.0 * kPi * dev);
        }
    }

    // Global tuning, and its curve.
    {
        double c = 0, s = 0, w = 0;
        for (int f = 0; f < frames; ++f)
        {
            c += tunC[f];
            s += tunS[f];
            w += std::hypot(tunC[f], tunS[f]);
        }
        a.tuningCents = centsFromVector(c, s);
        a.tuningConfidence = w > 0 ? (float)(std::hypot(c, s) / w) : 0.0f;
        a.curveStepS = kCurveStepS;
        a.tuningCurve.clear();
        const float fps = (float)kSr / (float)kHop;
        for (float t = 0.0f; t <= a.durationS; t += kCurveStepS)
        {
            const int f0 = std::max(0, (int)((t - 0.5f * kCurveWindowS) * fps));
            const int f1 = std::min(frames, (int)((t + 0.5f * kCurveWindowS) * fps) + 1);
            double cc = 0, ss = 0;
            for (int f = f0; f < f1; ++f)
            {
                cc += tunC[f];
                ss += tunS[f];
            }
            a.tuningCurve.push_back(std::hypot(cc, ss) > 1e-9 ? centsFromVector(cc, ss) : a.tuningCents);
        }
    }

    // Chroma, with each frame's local tuning taken out first (gravity_wave drifts by +-30 cents,
    // which is most of the way to the next semitone).
    std::vector<std::array<float, 12>> chromaF(frames), bassF(frames);
    float chroma[12] = {}, bass[12] = {};
    for (int f = 0; f < frames; ++f)
    {
        chromaF[f].fill(0.0f);
        bassF[f].fill(0.0f);
        const float t = ((float)f * kHop + 0.5f * kN) / (float)kSr;
        const float tl = a.tuningAt(t) / 100.0f;
        for (const Peak &p : peaks[f])
        {
            const float m = p.midi - tl;
            const int pc = rotl12((int)std::lround(m));
            if (m < 51.1f) // < 160 Hz
                bassF[f][pc] += p.amp;
            if (m >= 33.0f) // >= 55 Hz
                chromaF[f][pc] += p.amp;
        }
        for (int k = 0; k < 12; ++k)
        {
            chroma[k] += chromaF[f][k];
            bass[k] += bassF[f][k];
        }
    }
    const float cMax = std::max(1e-9f, *std::max_element(chroma, chroma + 12));
    const float bMax = std::max(1e-9f, *std::max_element(bass, bass + 12));
    float profile[12];
    for (int k = 0; k < 12; ++k)
    {
        a.chroma[k] = chroma[k] / cMax;
        a.bass[k] = bass[k] / bMax;
        profile[k] = a.chroma[k] + 0.5f * a.bass[k];
    }
    a.bassPc = (int)(std::max_element(bass, bass + 12) - bass);

    // Key: Krumhansl-Kessler, all 24.
    {
        float best = -2.0f;
        for (int t = 0; t < 12; ++t)
            for (int m = 0; m < 2; ++m)
            {
                float rot[12];
                for (int k = 0; k < 12; ++k)
                    rot[rotl12(k + t)] = (m ? kMinorProfile : kMajorProfile)[k];
                const float r = pearson(profile, rot);
                if (r > best)
                {
                    best = r;
                    a.tonicPc = t;
                    a.minor = m == 1;
                }
            }
        a.keyConfidence = best;
    }

    // Pitch set: the strongest pitch classes (>= 12% of the strongest, 3..7 of them, tonic always).
    {
        const float pMax = std::max(1e-9f, *std::max_element(profile, profile + 12));
        int order[12];
        for (int k = 0; k < 12; ++k)
            order[k] = k;
        std::sort(order, order + 12, [&](int p, int q) { return profile[p] > profile[q]; });
        a.pitchSet = (uint16_t)(1u << a.tonicPc);
        int n = 1;
        for (int i = 0; i < 12 && n < 7; ++i)
        {
            const int pc = order[i];
            if (pc == a.tonicPc)
                continue;
            if (profile[pc] / pMax < 0.12f && n >= 3)
                break;
            a.pitchSet |= (uint16_t)(1u << pc);
            ++n;
        }
        // Nearest named scale: F-measure of the energy it covers against the notes it needs.
        float rel[12], total = 0.0f;
        for (int k = 0; k < 12; ++k)
        {
            rel[k] = profile[rotl12(a.tonicPc + k)] / pMax;
            total += rel[k];
        }
        float bestF = -1.0f;
        for (const NamedScale &s : kScales)
        {
            float inside = 0.0f;
            int need = 0, have = 0;
            for (int k = 0; k < 12; ++k)
                if (s.rel & (1u << k))
                {
                    inside += rel[k];
                    ++need;
                    have += rel[k] >= 0.1f ? 1 : 0;
                }
            const float p = total > 0 ? inside / total : 0.0f, r = (float)have / (float)need;
            const float F = (p + r) > 0 ? 2.0f * p * r / (p + r) : 0.0f;
            if (F > bestF)
            {
                bestF = F;
                a.scaleName = s.name;
            }
        }
    }

    // Chords: 3 s segments, template cosine, merged while unchanged.
    {
        a.chords.clear();
        std::vector<float> sorted(rms);
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        const float quiet = 0.15f * sorted[sorted.size() / 2];
        const int segFrames = std::max(1, (int)std::lround(kChordSegS * kSr / kHop));
        std::map<uint16_t, float> held;
        for (int s0 = 0; s0 < frames; s0 += segFrames)
        {
            const int s1 = std::min(frames, s0 + segFrames);
            float v[12] = {};
            float e = 0.0f;
            for (int f = s0; f < s1; ++f)
            {
                for (int k = 0; k < 12; ++k)
                    v[k] += chromaF[f][k] + 0.7f * bassF[f][k];
                e += rms[f];
            }
            e /= (float)(s1 - s0);
            Chord c;
            c.t0 = (float)s0 * kHop / (float)kSr;
            c.t1 = std::min(a.durationS, (float)s1 * kHop / (float)kSr);
            float norm = 0.0f;
            for (float q : v)
                norm += q * q;
            norm = std::sqrt(norm);
            if (e >= quiet && norm > 1e-9f)
            {
                float best = -1.0f;
                for (int r = 0; r < 12; ++r)
                    for (const ChordTemplate &tpl : kChordTemplates)
                    {
                        float t[12] = {};
                        uint16_t m = 0;
                        for (int i = 0; i < tpl.n; ++i)
                        {
                            t[rotl12(r + tpl.iv[i])] = 1.0f;
                            m |= (uint16_t)(1u << rotl12(r + tpl.iv[i]));
                        }
                        t[r] += 0.5f; // the root is the strongest part of a chord's spectrum
                        float dot = 0.0f, tn = 0.0f;
                        for (int k = 0; k < 12; ++k)
                        {
                            dot += v[k] * t[k];
                            tn += t[k] * t[k];
                        }
                        const float cs = dot / (norm * std::sqrt(tn));
                        if (cs > best)
                        {
                            best = cs;
                            c.rootPc = r;
                            c.quality = tpl.quality;
                            c.mask = m;
                            c.confidence = cs;
                        }
                    }
            }
            if (!a.chords.empty() && a.chords.back().mask == c.mask && a.chords.back().rootPc == c.rootPc)
            {
                a.chords.back().t1 = c.t1;
                a.chords.back().confidence = std::max(a.chords.back().confidence, c.confidence);
            }
            else
                a.chords.push_back(c);
            if (c.rootPc >= 0)
                held[c.mask] += c.t1 - c.t0;
        }
        a.homeChord = 0;
        float longest = -1.0f;
        for (const auto &h : held)
            if (h.second > longest)
            {
                longest = h.second;
                a.homeChord = h.first;
            }
    }

    out = std::move(a);
    return true;
}

// ── JSON ─────────────────────────────────────────────────────────────────────────────────────────
nlohmann::json toJson(const Analysis &a)
{
    using nlohmann::json;
    auto round1 = [](float v) { return std::round(v * 10.0f) / 10.0f; };
    auto round3 = [](float v) { return std::round(v * 1000.0f) / 1000.0f; };
    json j;
    j["format"] = "sat-light-sim-music-analysis/1";
    j["version"] = kAnalysisVersion;
    j["file"] = a.file;
    j["file_size"] = a.fileSize;
    j["file_time"] = a.fileTime;
    j["duration_s"] = round1(a.durationS);
    j["key"] = a.keyName();
    j["tonic_pc"] = a.tonicPc;
    j["minor"] = a.minor;
    j["key_confidence"] = round3(a.keyConfidence);
    j["bass_note"] = pcName(a.bassPc);
    j["bass_pc"] = a.bassPc;
    j["tuning_cents"] = round1(a.tuningCents);
    j["tuning_confidence"] = round3(a.tuningConfidence);
    j["pitch_set"] = a.pitchSet;
    j["pitch_set_notes"] = [&]
    {
        std::string s;
        for (int k = 0; k < 12; ++k)
            if (a.pitchSet & (1u << k))
                s += (s.empty() ? "" : " ") + pcName(k);
        return s;
    }();
    j["pitch_set_intervals"] = intervalList(toRelative(a.pitchSet, a.tonicPc));
    j["scale_name"] = a.scaleName;
    json ch = json::array(), bs = json::array();
    for (int k = 0; k < 12; ++k)
    {
        ch.push_back(round3(a.chroma[k]));
        bs.push_back(round3(a.bass[k]));
    }
    j["chroma"] = ch;
    j["bass"] = bs;
    j["home_chord"] = a.homeChord;
    j["curve_step_s"] = a.curveStepS;
    json cv = json::array();
    for (float c : a.tuningCurve)
        cv.push_back(round1(c));
    j["tuning_curve_cents"] = cv;
    json cs = json::array();
    for (const Chord &c : a.chords)
        cs.push_back({{"t0", round1(c.t0)}, {"t1", round1(c.t1)}, {"name", chordName(c)}, {"root_pc", c.rootPc},
                      {"quality", c.quality}, {"mask", c.mask}, {"confidence", round3(c.confidence)}});
    j["chords"] = cs;
    return j;
}

bool fromJson(const nlohmann::json &j, Analysis &a)
{
    try
    {
        if (j.value("version", -1) != kAnalysisVersion)
            return false;
        a.file = j.at("file").get<std::string>();
        a.fileSize = j.at("file_size").get<uint64_t>();
        a.fileTime = j.at("file_time").get<int64_t>();
        a.durationS = j.at("duration_s").get<float>();
        a.tonicPc = j.at("tonic_pc").get<int>();
        a.minor = j.at("minor").get<bool>();
        a.keyConfidence = j.at("key_confidence").get<float>();
        a.bassPc = j.at("bass_pc").get<int>();
        a.tuningCents = j.at("tuning_cents").get<float>();
        a.tuningConfidence = j.at("tuning_confidence").get<float>();
        a.pitchSet = j.at("pitch_set").get<uint16_t>();
        a.scaleName = j.at("scale_name").get<std::string>();
        for (int k = 0; k < 12; ++k)
        {
            a.chroma[k] = j.at("chroma").at(k).get<float>();
            a.bass[k] = j.at("bass").at(k).get<float>();
        }
        a.homeChord = j.at("home_chord").get<uint16_t>();
        a.curveStepS = j.at("curve_step_s").get<float>();
        a.tuningCurve = j.at("tuning_curve_cents").get<std::vector<float>>();
        a.chords.clear();
        for (const auto &c : j.at("chords"))
        {
            Chord ch;
            ch.t0 = c.at("t0").get<float>();
            ch.t1 = c.at("t1").get<float>();
            ch.rootPc = c.at("root_pc").get<int>();
            ch.quality = c.at("quality").get<std::string>();
            ch.mask = c.at("mask").get<uint16_t>();
            ch.confidence = c.at("confidence").get<float>();
            a.chords.push_back(ch);
        }
        return a.curveStepS > 0.0f;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

// ── Library ──────────────────────────────────────────────────────────────────────────────────────
Library::~Library()
{
    stop_.store(true);
    if (worker_.joinable())
        worker_.join();
}

void Library::say(const std::string &s)
{
    std::lock_guard<std::mutex> lk(mu_);
    messages_.push_back(s);
}

std::vector<std::string> Library::takeMessages()
{
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

std::shared_ptr<const Analysis> Library::get(const std::string &trackPath) const
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = done_.find(trackPath);
    return it == done_.end() ? nullptr : it->second;
}

void Library::start(const std::vector<std::string> &tracks, const std::vector<std::string> &cacheDirs, bool wait)
{
    if (started_)
        return; // once
    started_ = true;
    pending_.store((int)tracks.size());
    if (wait)
        run(tracks, cacheDirs);
    else
        worker_ = std::thread([this, tracks, cacheDirs] { run(tracks, cacheDirs); });
}

void Library::run(std::vector<std::string> tracks, std::vector<std::string> dirs)
{
    namespace fs = std::filesystem;
    for (const std::string &path : tracks)
    {
        if (stop_.load())
            return;
        std::error_code ec;
        const fs::path p(path);
        Analysis a;
        a.file = p.filename().string();
        a.fileSize = (uint64_t)fs::file_size(p, ec);
        // The cache key is the file's CONTENT (FNV-1a 64 over its bytes, ~10 ms a track), not its write time: the
        // build copies the music next to the exe, so the write time changed with every build and every launch after
        // a rebuild re-analysed all the tracks (~10 s, on the startup's critical path for the music; 2026-10-02).
        {
            uint64_t h = 1469598103934665603ull;
            std::ifstream in(p, std::ios::binary);
            std::vector<char> buf(1 << 16);
            while (in)
            {
                in.read(buf.data(), (std::streamsize)buf.size());
                const std::streamsize got = in.gcount();
                for (std::streamsize i = 0; i < got; ++i)
                    h = (h ^ (uint8_t)buf[(size_t)i]) * 1099511628211ull;
            }
            a.fileTime = (int64_t)h;
        }
        const std::string cacheName = p.stem().string() + ".analysis.json";

        bool cached = false;
        for (const std::string &d : dirs)
        {
            std::ifstream f(fs::path(d) / cacheName);
            if (!f)
                continue;
            try
            {
                nlohmann::json j;
                f >> j;
                Analysis c;
                if (fromJson(j, c) && c.fileSize == a.fileSize && c.fileTime == a.fileTime)
                {
                    a = std::move(c);
                    cached = true;
                    break;
                }
            }
            catch (const std::exception &)
            {
            }
        }
        if (!cached)
        {
            std::string err;
            if (!analyzeFile(path, a, err, &stop_))
            {
                if (!stop_.load())
                    say("music analysis: " + err);
                pending_.fetch_sub(1);
                continue;
            }
            if (!dirs.empty())
            {
                fs::create_directories(dirs[0], ec);
                std::ofstream(fs::path(dirs[0]) / cacheName) << toJson(a).dump(1) << '\n';
            }
        }
        char buf[256];
        snprintf(buf, sizeof(buf), "music analysis: %s%s: %s (r %.2f), tuning %+.0f cents, %s, %zu chord segments",
                 a.file.c_str(), cached ? " (cached)" : "", a.keyName().c_str(), a.keyConfidence, a.tuningCents,
                 intervalList(toRelative(a.pitchSet, a.tonicPc)).c_str(), a.chords.size());
        say(buf);
        {
            std::lock_guard<std::mutex> lk(mu_);
            done_[path] = std::make_shared<const Analysis>(std::move(a));
        }
        pending_.fetch_sub(1);
    }
}
} // namespace music
