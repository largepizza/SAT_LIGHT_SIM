// Ambience.cpp — the ambient-sound layer table. See Ambience.h.
#include "Ambience.h"

#include "AmbientSynth.h"
#include "AudioSystem.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using json = nlohmann::json;

namespace
{
uint32_t fnv1a(const std::string &s)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 16777619u;
    }
    return h ? h : 1u;
}

float smooth01(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

int Ambience::registerDriver(const std::string &name)
{
    const int i = driverIndex(name);
    if (i >= 0)
        return i;
    names_.push_back(name);
    values_.push_back(0.0f);
    return (int)names_.size() - 1;
}

int Ambience::driverIndex(const std::string &name) const
{
    for (size_t i = 0; i < names_.size(); ++i)
        if (names_[i] == name)
            return (int)i;
    return -1;
}

int Ambience::layerIndex(const std::string &id) const
{
    for (size_t i = 0; i < layers_.size(); ++i)
        if (layers_[i].id == id)
            return (int)i;
    return -1;
}

float Ambience::rampValue(const Ramp &r, float x)
{
    auto s = [x](float a, float b) { return a == b ? (x >= a ? 1.0f : 0.0f) : smooth01((x - a) / (b - a)); };
    if (r.n == 4)
        return s(r.p[0], r.p[1]) * (1.0f - s(r.p[2], r.p[3]));
    return s(r.p[0], r.p[1]);
}

bool Ambience::parseRamps(const json &obj, std::vector<Ramp> &out, std::string &err) const
{
    if (!obj.is_object())
    {
        err = "a condition must be an object of driver: ramp";
        return false;
    }
    for (auto it = obj.begin(); it != obj.end(); ++it)
    {
        Ramp r;
        r.driver = driverIndex(it.key());
        if (r.driver < 0)
        {
            err = "unknown driver '" + it.key() + "'";
            return false;
        }
        const json &v = it.value();
        if (!v.is_array() || (v.size() != 2 && v.size() != 4))
        {
            err = "ramp for '" + it.key() + "' must be [a, b] or [a, b, c, d]";
            return false;
        }
        r.n = (int)v.size();
        for (int k = 0; k < r.n; ++k)
            r.p[k] = v[k].get<float>();
        out.push_back(r);
    }
    return true;
}

bool Ambience::load(const std::string &path, std::string &err)
{
    std::ifstream f(path);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    json j;
    try
    {
        f >> j;
    }
    catch (const std::exception &e)
    {
        err = path + ": " + e.what();
        return false;
    }
    const size_t slash = path.find_last_of("/\\");
    baseDir_ = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);

    try
    {
        groups_.clear();
        if (j.contains("shell_groups"))
        {
            for (auto it = j["shell_groups"].begin(); it != j["shell_groups"].end(); ++it)
            {
                ShellGroup g;
                g.name = it.key();
                g.patterns = it.value().value("match", std::vector<std::string>{});
                g.hearingM = it.value().value("hearing_m", 500000.0);
                g.countDriver = registerDriver(g.name + "_count");
                g.nearDriver = registerDriver(g.name + "_near_m");
                groups_.push_back(g);
            }
        }

        layers_.clear();
        for (const json &lj : j.at("layers"))
        {
            Layer l;
            l.id = lj.at("id").get<std::string>();
            if (layerIndex(l.id) >= 0)
                throw std::runtime_error("duplicate layer id '" + l.id + "'");
            if (lj.contains("synth"))
            {
                l.kind = Kind::Synth;
                l.synthKind = lj["synth"].get<std::string>();
                auto probe = AmbientSynth::create(l.synthKind, 48000, 1);
                if (!probe)
                    throw std::runtime_error(l.id + ": unknown synth '" + l.synthKind + "'");
                if (lj.contains("params"))
                    for (auto it = lj["params"].begin(); it != lj["params"].end(); ++it)
                    {
                        if (probe->paramIndex(it.key()) < 0)
                            throw std::runtime_error(l.id + ": synth '" + l.synthKind + "' has no parameter '" +
                                                     it.key() + "'");
                        if (it.value().is_object())
                            l.rootParams.push_back({it.key(), it.value().at("root").get<float>()});
                        else
                            l.params.push_back({it.key(), it.value().get<float>()});
                    }
            }
            else if (lj.contains("loop"))
            {
                l.kind = Kind::Loop;
                l.files.push_back(lj["loop"].get<std::string>());
            }
            else if (lj.contains("events"))
            {
                l.kind = Kind::Events;
                l.files = lj["events"].get<std::vector<std::string>>();
                if (l.files.empty())
                    throw std::runtime_error(l.id + ": \"events\" lists no files");
                if (lj.contains("interval_s"))
                {
                    l.intervalMin = lj["interval_s"].at(0).get<float>();
                    l.intervalMax = lj["interval_s"].at(1).get<float>();
                }
                l.pan = lj.value("pan", l.pan);
                l.gainJitter = lj.value("gain_jitter", l.gainJitter);
            }
            else
                throw std::runtime_error(l.id + ": needs one of \"synth\", \"loop\", \"events\"");

            l.gain = lj.value("gain", 1.0f);
            l.group = lj.value("group", std::string());
            l.fadeInS = std::max(0.05f, lj.value("fade_in_s", 1.5f));
            l.fadeOutS = std::max(0.05f, lj.value("fade_out_s", 0.8f));
            std::string e;
            if (lj.contains("when") && !parseRamps(lj["when"], l.when, e))
                throw std::runtime_error(l.id + ": " + e);
            if (lj.contains("any"))
                for (const json &alt : lj["any"])
                {
                    std::vector<Ramp> rs;
                    if (!parseRamps(alt, rs, e))
                        throw std::runtime_error(l.id + ": " + e);
                    l.any.push_back(std::move(rs));
                }
            if (lj.contains("mod"))
                for (auto it = lj["mod"].begin(); it != lj["mod"].end(); ++it)
                {
                    Mod m;
                    m.param = it.key();
                    const json &mj = it.value();
                    m.driver = driverIndex(mj.at("driver").get<std::string>());
                    if (m.driver < 0)
                        throw std::runtime_error(l.id + ": mod '" + m.param + "' names unknown driver '" +
                                                 mj["driver"].get<std::string>() + "'");
                    m.in0 = mj.at("in").at(0).get<float>();
                    m.in1 = mj.at("in").at(1).get<float>();
                    m.out0 = mj.at("out").at(0).get<float>();
                    m.out1 = mj.at("out").at(1).get<float>();
                    if (l.kind == Kind::Synth)
                    {
                        auto probe = AmbientSynth::create(l.synthKind, 48000, 1);
                        if (probe->paramIndex(m.param) < 0)
                            throw std::runtime_error(l.id + ": mod of unknown parameter '" + m.param + "'");
                    }
                    else if (m.param != "rate")
                        throw std::runtime_error(l.id + ": a loop/events layer can only mod \"rate\"");
                    l.mods.push_back(m);
                }
            l.rng = fnv1a(l.id);
            layers_.push_back(std::move(l));
        }
    }
    catch (const std::exception &e)
    {
        err = path + ": " + e.what();
        layers_.clear();
        return false;
    }
    loaded_ = true;
    return true;
}

const std::vector<std::string> &Ambience::groupNames()
{
    static const std::vector<std::string> k = {"wind", "water", "nature", "city", "space", "machines"};
    return k;
}

void Ambience::setGroupGain(const std::string &group, float g)
{
    for (auto &p : groupGains_)
        if (p.first == group)
        {
            p.second = g;
            return;
        }
    groupGains_.push_back({group, g});
}

float Ambience::groupGain(const std::string &group) const
{
    for (const auto &p : groupGains_)
        if (p.first == group)
            return p.second;
    return 1.0f;
}

float Ambience::evalTarget(const Layer &l) const
{
    float g = l.gain * groupGain(l.group);
    for (const Ramp &r : l.when)
        g *= rampValue(r, values_[r.driver]);
    if (!l.any.empty())
    {
        float best = 0.0f;
        for (const auto &alt : l.any)
        {
            float a = 1.0f;
            for (const Ramp &r : alt)
                a *= rampValue(r, values_[r.driver]);
            best = std::max(best, a);
        }
        g *= best;
    }
    return std::isfinite(g) ? std::max(0.0f, g) : 0.0f;
}

float Ambience::rnd(Layer &l)
{
    l.rng ^= l.rng << 13;
    l.rng ^= l.rng >> 17;
    l.rng ^= l.rng << 5;
    return (float)(l.rng >> 8) * (1.0f / 16777216.0f);
}

void Ambience::ensureVoice(Layer &l, AudioSystem *audio)
{
    if (l.voice >= 0 || l.failed || l.kind == Kind::Events)
        return;
    if (l.kind == Kind::Synth)
    {
        auto s = AmbientSynth::create(l.synthKind, audio->sampleRate(), fnv1a(l.id));
        for (const auto &p : l.params)
            s->setParam(s->paramIndex(p.first), p.second);
        for (Mod &m : l.mods)
            m.paramIdx = s->paramIndex(m.param);
        l.rootParamIdx.clear();
        for (const auto &p : l.rootParams)
        {
            l.rootParamIdx.push_back(s->paramIndex(p.first));
            s->setParam(l.rootParamIdx.back(), p.second * rootHz_);
        }
        l.voice = audio->addAmbienceSynth(std::move(s));
    }
    else
    {
        l.voice = audio->addAmbienceLoop(baseDir_ + l.files[0]);
    }
    if (l.voice < 0)
    {
        l.failed = true;
        l.failure = l.kind == Kind::Loop ? "missing or unreadable " + l.files[0] : "could not create voice";
    }
}

void Ambience::update(float dt, AudioSystem *audio)
{
    if (!loaded_)
        return;
    for (Layer &l : layers_)
    {
        l.target = l.forced >= 0.0f ? l.forced : evalTarget(l);
        // Linear slew, full scale (the layer's own gain) in fade_in_s / fade_out_s.
        const float full = std::max(l.gain, 1e-3f);
        const float step = std::max(dt, 0.0f) * full;
        if (l.current < l.target)
            l.current = std::min(l.target, l.current + step / (l.fadeInS * std::max(fadeInScale_, 0.01f)));
        else
            l.current = std::max(l.target, l.current - step / (l.fadeOutS * std::max(fadeOutScale_, 0.01f)));

        l.rateScale = 1.0f;
        for (const Mod &m : l.mods)
        {
            const float x = values_[m.driver];
            const float t = m.in1 == m.in0 ? (x >= m.in0 ? 1.0f : 0.0f)
                                           : std::clamp((x - m.in0) / (m.in1 - m.in0), 0.0f, 1.0f);
            const float v = m.out0 + (m.out1 - m.out0) * t;
            if (l.kind != Kind::Synth)
                l.rateScale = std::max(1e-3f, v);
            else if (audio && l.voice >= 0)
                if (AmbientSynth *s = audio->voiceSynth(l.voice))
                    s->setParam(m.paramIdx, v);
        }
        if (audio && l.voice >= 0 && !l.rootParams.empty())
            if (AmbientSynth *s = audio->voiceSynth(l.voice))
                for (size_t k = 0; k < l.rootParams.size() && k < l.rootParamIdx.size(); ++k)
                    s->setParam(l.rootParamIdx[k], l.rootParams[k].second * rootHz_);

        if (!audio || !audio->isInitialized())
            continue;
        if (l.current > 0.0f)
            ensureVoice(l, audio);
        if (l.voice >= 0)
            audio->setVoiceGain(l.voice, l.current);
    }
    tickEvents(dt, audio);
}

void Ambience::tickEvents(float dt, AudioSystem *audio)
{
    if (!loaded_ || !audio || !audio->isInitialized())
        return;
    for (Layer &l : layers_)
    {
        if (l.kind != Kind::Events || l.failed)
            continue;
        if (l.current < 0.01f)
        {
            l.nextEvent = -1.0f; // re-armed with a fresh random delay when the layer comes back
            continue;
        }
        if (l.nextEvent < 0.0f)
            l.nextEvent = (l.intervalMin + (l.intervalMax - l.intervalMin) * rnd(l)) * 0.5f / l.rateScale;
        l.nextEvent -= dt;
        if (l.nextEvent > 0.0f)
            continue;
        const std::string &file = l.files[std::min(l.files.size() - 1, (size_t)(rnd(l) * (float)l.files.size()))];
        const float g = l.current * (1.0f - l.gainJitter * rnd(l));
        audio->playAmbienceOneShot(baseDir_ + file, g, l.pan * (rnd(l) * 2.0f - 1.0f));
        ++l.events;
        l.nextEvent = (l.intervalMin + (l.intervalMax - l.intervalMin) * rnd(l)) / l.rateScale;
    }
}

bool Ambience::force(const std::string &id, float gain)
{
    const int i = layerIndex(id);
    if (i < 0)
        return false;
    layers_[i].forced = gain;
    return true;
}

void Ambience::releaseForced()
{
    for (Layer &l : layers_)
        l.forced = -1.0f;
}

void Ambience::releaseVoices(AudioSystem *audio)
{
    if (audio)
        audio->clearAmbience();
    for (Layer &l : layers_)
        l.voice = -1;
}

json Ambience::stateJson() const
{
    json j;
    json d = json::object();
    for (size_t i = 0; i < names_.size(); ++i)
        d[names_[i]] = values_[i];
    j["drivers"] = d;
    json ls = json::array();
    for (const Layer &l : layers_)
    {
        json e = {{"id", l.id},
                  {"kind", l.kind == Kind::Synth ? "synth:" + l.synthKind : l.kind == Kind::Loop ? "loop" : "events"},
                  {"target", l.target},
                  {"gain", l.current}};
        if (l.kind == Kind::Events)
        {
            e["events_fired"] = l.events;
            e["rate_scale"] = l.rateScale;
        }
        if (l.failed)
            e["error"] = l.failure;
        if (l.forced >= 0.0f)
            e["forced"] = l.forced;
        ls.push_back(e);
    }
    j["layers"] = ls;
    // The audible ones, loudest first — what a reader actually wants to know.
    std::vector<std::pair<float, std::string>> on;
    for (const Layer &l : layers_)
        if (l.current > 0.01f && !l.failed)
            on.push_back({l.current, l.id});
    std::sort(on.begin(), on.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    json audible = json::array();
    for (const auto &p : on)
        audible.push_back(p.second);
    j["audible"] = audible;
    return j;
}
