// SoundTool — the ambience's offline workbench: no window, no Vulkan, no audio device.
//
//   SoundTool --analyze <track> [<track> ...] [--out <dir>]
//       Runs MusicAnalysis on each file (the same code the app runs at startup), prints key, tuning,
//       pitch set and the chord timeline, and writes <dir>/<track>.analysis.json (default: cwd).
//
//   SoundTool --render <synth kind> --out <file.wav> [--seconds S] [--rate HZ] [--seed N] [name=spec ...]
//       Renders one AmbientSynth voice to a 16-bit stereo WAV and prints its RMS and peak. A param spec
//       is a constant (voices=4), a linear ramp over the render (voices=0:12) or keyframes
//       (voices=0/0,1/2,4/2,5/0 — time/value pairs, linear between them, held past the ends).
//       Measure loudness with: tools/harness/.venv/Scripts/python tools/harness/imgtools.py audio <wav>
//
// Build: cmake --build build --target SoundTool (EXCLUDE_FROM_ALL).
#ifndef NOMINMAX
#define NOMINMAX // miniaudio's implementation pulls in windows.h
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "AmbientSynth.h"
#include "MusicAnalysis.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
struct Key
{
    float t, v;
};
struct ParamSpec
{
    int idx = -1;
    std::string name;
    std::vector<Key> keys; // one key = constant; t < 0 on both = ramp over the render
    bool ramp = false;
};

float evalSpec(const ParamSpec &p, float t, float total)
{
    if (p.ramp)
        return p.keys[0].v + (p.keys[1].v - p.keys[0].v) * std::min(1.0f, t / std::max(total, 1e-3f));
    if (p.keys.size() == 1 || t <= p.keys.front().t)
        return p.keys.front().v;
    for (size_t i = 1; i < p.keys.size(); ++i)
        if (t <= p.keys[i].t)
        {
            const Key &a = p.keys[i - 1], &b = p.keys[i];
            return a.v + (b.v - a.v) * (t - a.t) / std::max(b.t - a.t, 1e-6f);
        }
    return p.keys.back().v;
}

bool parseSpec(const std::string &arg, ParamSpec &p)
{
    const size_t eq = arg.find('=');
    if (eq == std::string::npos)
        return false;
    p.name = arg.substr(0, eq);
    const std::string v = arg.substr(eq + 1);
    if (v.find('/') != std::string::npos)
    {
        size_t b = 0;
        while (b < v.size())
        {
            const size_t e = v.find(',', b);
            const std::string kv = v.substr(b, e == std::string::npos ? std::string::npos : e - b);
            const size_t sl = kv.find('/');
            if (sl == std::string::npos)
                return false;
            p.keys.push_back({std::stof(kv.substr(0, sl)), std::stof(kv.substr(sl + 1))});
            if (e == std::string::npos)
                break;
            b = e + 1;
        }
    }
    else if (v.find(':') != std::string::npos)
    {
        const size_t c = v.find(':');
        p.keys = {{0.0f, std::stof(v.substr(0, c))}, {0.0f, std::stof(v.substr(c + 1))}};
        p.ramp = true;
    }
    else
        p.keys = {{0.0f, std::stof(v)}};
    return !p.keys.empty();
}

bool writeWav(const std::string &path, const std::vector<float> &lr, uint32_t sr)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    const uint32_t frames = (uint32_t)(lr.size() / 2), dataBytes = frames * 4;
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(sr);
    u32(sr * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    for (float s : lr)
    {
        const int16_t q = (int16_t)std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        f.write((const char *)&q, 2);
    }
    return (bool)f;
}

int analyze(int argc, char **argv)
{
    std::string outDir = ".";
    std::vector<std::string> files;
    for (int i = 2; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            outDir = argv[++i];
        else
            files.push_back(argv[i]);
    }
    int rc = 0;
    for (const std::string &path : files)
    {
        music::Analysis a;
        a.file = std::filesystem::path(path).filename().string();
        std::string err;
        if (!music::analyzeFile(path, a, err))
        {
            fprintf(stderr, "%s\n", err.c_str());
            rc = 1;
            continue;
        }
        printf("== %s  %.1f s\n", a.file.c_str(), a.durationS);
        printf("  key %s (r %.2f), bass note %s, nearest scale: %s %s\n", a.keyName().c_str(), a.keyConfidence,
               music::pcName(a.bassPc).c_str(), music::pcName(a.tonicPc).c_str(), a.scaleName.c_str());
        printf("  pitch set: %s  (%s)\n", music::intervalList(music::toRelative(a.pitchSet, a.tonicPc)).c_str(),
               [&]
               {
                   std::string s;
                   for (int k = 0; k < 12; ++k)
                       if (a.pitchSet & (1u << k))
                           s += (s.empty() ? "" : " ") + music::pcName(k);
                   return s;
               }()
                   .c_str());
        printf("  tuning %+.1f cents (confidence %.2f); curve every 4 s:", a.tuningCents, a.tuningConfidence);
        for (size_t i = 0; i < a.tuningCurve.size(); i += 4)
            printf(" %+.0f", a.tuningCurve[i]);
        printf("\n  chords:");
        for (const music::Chord &c : a.chords)
            printf(" %s[%.0f-%.0f]", music::chordName(c).c_str(), c.t0, c.t1);
        printf("\n  tonic: %.2f Hz in the 36-72 Hz octave\n", a.tonicHz(36.0, a.tuningCents));
        const std::string out = (std::filesystem::path(outDir) / (std::filesystem::path(path).stem().string() + ".analysis.json")).string();
        std::ofstream(out) << music::toJson(a).dump(1) << '\n';
    }
    return rc;
}

int render(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    const std::string kind = argv[2];
    std::string out = kind + ".wav";
    float seconds = 10.0f;
    uint32_t sr = 48000, seed = 1;
    std::vector<ParamSpec> specs;
    for (int i = 3; i < argc; ++i)
    {
        if (!strcmp(argv[i], "--out") && i + 1 < argc)
            out = argv[++i];
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = std::stof(argv[++i]);
        else if (!strcmp(argv[i], "--rate") && i + 1 < argc)
            sr = (uint32_t)std::stoul(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc)
            seed = (uint32_t)std::stoul(argv[++i]);
        else
        {
            ParamSpec p;
            if (!parseSpec(argv[i], p))
            {
                fprintf(stderr, "bad parameter spec '%s'\n", argv[i]);
                return 2;
            }
            specs.push_back(p);
        }
    }
    auto synth = AmbientSynth::create(kind, sr, seed);
    if (!synth)
    {
        fprintf(stderr, "unknown synth '%s'\n", kind.c_str());
        return 2;
    }
    for (ParamSpec &p : specs)
    {
        p.idx = synth->paramIndex(p.name);
        if (p.idx < 0)
        {
            fprintf(stderr, "synth '%s' has no parameter '%s'; it has:", kind.c_str(), p.name.c_str());
            for (int i = 0; i < synth->paramCount(); ++i)
                fprintf(stderr, " %s", synth->paramDef(i).name);
            fprintf(stderr, "\n");
            return 2;
        }
    }
    // Parameters are updated every 1/60 s, like the app's main thread does.
    const uint32_t total = (uint32_t)(seconds * sr), chunk = sr / 60;
    std::vector<float> lr((size_t)total * 2);
    double sum2 = 0.0, peak = 0.0;
    for (uint32_t done = 0; done < total;)
    {
        const float t = (float)done / (float)sr;
        for (const ParamSpec &p : specs)
            synth->setParam(p.idx, evalSpec(p, t, seconds));
        const uint32_t n = std::min(chunk, total - done);
        synth->render(lr.data() + (size_t)done * 2, n);
        done += n;
    }
    for (float s : lr)
    {
        sum2 += (double)s * s;
        peak = std::max(peak, (double)std::fabs(s));
    }
    if (!writeWav(out, lr, sr))
    {
        fprintf(stderr, "cannot write %s\n", out.c_str());
        return 1;
    }
    printf("%s: %.1f s, RMS %.1f dBFS, peak %.1f dBFS\n", out.c_str(), seconds,
           10.0 * std::log10(std::max(sum2 / lr.size(), 1e-12)), 20.0 * std::log10(std::max(peak, 1e-9)));
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--analyze"))
        return analyze(argc, argv);
    if (argc >= 2 && !strcmp(argv[1], "--render"))
        return render(argc, argv);
    fprintf(stderr, "usage:\n  SoundTool --analyze <track> [...] [--out <dir>]\n"
                    "  SoundTool --render <synth> --out <wav> [--seconds S] [name=v | name=a:b | name=t/v,t/v ...]\n");
    return 2;
}
