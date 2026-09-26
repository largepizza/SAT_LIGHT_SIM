#pragma once
// ── Ambience ──────────────────────────────────────────────────────────────────────────────────────
// The ambient-sound layer table: which sounds play where, and how loud. App-independent — the sim
// computes a CONTEXT (named "drivers": altitude, Sun elevation, ocean fraction, city brightness,
// beam light, satellite-shell proximity, ...) each frame and hands it over; this class turns it
// into per-layer gains and drives AudioSystem voices.
//
// Data: assets/sound/ambience/ambience.json (moddable, like constellations.json). A layer is
//   {"id", one of "synth" (AmbientSynth kind) | "loop" (file) | "events" (files),
//    "group", "gain", "fade_in_s", "fade_out_s", "when": {driver: ramp, ...},
//    "any": [{driver: ramp, ...}, ...], "params": {synth param: value | {"root": multiple}},
//    "mod": {param: {"driver", "in": [a, b], "out": [c, d]}},
//    events only: "interval_s": [min, max], "pan": spread, "gain_jitter"}
// A ramp is [a, b] (smooth 0 -> 1 from a to b; a > b gives a falling ramp) or [a, b, c, d]
// (rises a -> b, falls c -> d). A layer's target gain = gain x group gain x every "when" ramp x
// (max over the "any" entries, each the product of its ramps). Everything is a crossfade by
// construction — there is no hard switch anywhere in the table.
//
// Fades are LINEAR slews: a layer goes from silent to its full gain in fade_in_s and back in
// fade_out_s, whatever the distance. The first cut eased exponentially (1 - e^(-dt/tau)): it never
// finished, a layer 4 s behind was still at -20 dB nine seconds later, and a jump from the ground
// to orbit dragged a cloud of crickets and birds up with it. Both times scale with global
// multipliers (Settings -> Sound, advanced).
//
// Tonal root: a synth param given as {"root": k} is k x the root frequency (setRootHz), so every
// pitched voice — the LEO drone, the datacenter hum and whine, their low tones — shares one key and
// stays consonant where their zones overlap.
//
// Drivers are registered by name: the sim registers its fixed set before load(); load() adds two
// per "shell_groups" entry (<group>_count, <group>_near_m) and refuses a layer that names a driver
// nobody registered, so a typo is a load error rather than a silently muted layer.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

class AudioSystem;

class Ambience
{
public:
    struct ShellGroup
    {
        std::string name;
        std::vector<std::string> patterns; // matched against a satellite type's model id or name ('*' glob)
        double hearingM = 500000.0;        // <group>_count counts satellites within this range
        int countDriver = -1, nearDriver = -1;
    };

    int registerDriver(const std::string &name); // idempotent; returns the index
    int driverIndex(const std::string &name) const;
    void set(int driver, float v)
    {
        if (driver >= 0 && driver < (int)values_.size())
            values_[driver] = v;
    }
    float get(int driver) const { return (driver >= 0 && driver < (int)values_.size()) ? values_[driver] : 0.0f; }

    bool load(const std::string &path, std::string &err);
    bool loaded() const { return loaded_; }
    const std::vector<ShellGroup> &shellGroups() const { return groups_; }

    // Per frame: evaluate targets, ease gains, (lazily) create voices, schedule events.
    void update(float dt, AudioSystem *audio);
    // Event scheduling only — renderWav's per-chunk hook, so one-shots keep firing during a render.
    void tickEvents(float dt, AudioSystem *audio);
    // Drops every voice (e.g. before AudioSystem goes away); they are re-created on the next update.
    void releaseVoices(AudioSystem *audio);

    // Mix controls (Settings -> Sound). Group gains multiply every layer of that "group".
    void setFadeScales(float in, float out)
    {
        fadeInScale_ = in;
        fadeOutScale_ = out;
    }
    void setRootHz(float hz) { rootHz_ = hz; }
    float rootHz() const { return rootHz_; }
    void setGroupGain(const std::string &group, float g);
    float groupGain(const std::string &group) const;
    static const std::vector<std::string> &groupNames(); // the groups the Sound tab offers

    // Harness/debug: pin a layer's target gain regardless of the context (gain < 0 = release).
    bool force(const std::string &id, float gain);
    void releaseForced();

    int layerCount() const { return (int)layers_.size(); }
    int layerIndex(const std::string &id) const;
    int layerVoice(int layer) const { return layers_[layer].voice; }
    nlohmann::json stateJson() const;

private:
    struct Ramp
    {
        int driver = -1;
        float p[4] = {};
        int n = 2;
    };
    struct Mod
    {
        std::string param;
        int driver = -1;
        float in0 = 0, in1 = 1, out0 = 0, out1 = 1;
        int paramIdx = -1; // resolved when the synth exists
    };
    enum class Kind
    {
        Synth,
        Loop,
        Events
    };
    struct Layer
    {
        std::string id;
        Kind kind = Kind::Synth;
        std::string synthKind;
        std::vector<std::pair<std::string, float>> params;
        std::vector<std::pair<std::string, float>> rootParams; // value = multiple of the root
        std::vector<int> rootParamIdx;                         // resolved when the synth exists
        std::string group;
        std::vector<Mod> mods;
        std::vector<std::string> files;
        float gain = 1.0f, fadeInS = 1.5f, fadeOutS = 0.8f;
        std::vector<Ramp> when;
        std::vector<std::vector<Ramp>> any;
        float intervalMin = 6.0f, intervalMax = 18.0f, pan = 0.8f, gainJitter = 0.3f;

        // runtime
        int voice = -1;
        bool failed = false;
        std::string failure;
        float target = 0.0f, current = 0.0f, rateScale = 1.0f;
        float forced = -1.0f;
        float nextEvent = -1.0f;
        uint32_t rng = 1;
        int events = 0;
    };

    static float rampValue(const Ramp &r, float x);
    float evalTarget(const Layer &l) const;
    bool parseRamps(const nlohmann::json &obj, std::vector<Ramp> &out, std::string &err) const;
    void ensureVoice(Layer &l, AudioSystem *audio);
    float rnd(Layer &l);

    std::vector<std::string> names_;
    std::vector<float> values_;
    std::vector<ShellGroup> groups_;
    std::vector<Layer> layers_;
    std::string baseDir_;
    bool loaded_ = false;
    float fadeInScale_ = 1.0f, fadeOutScale_ = 1.0f, rootHz_ = 55.0f;
    std::vector<std::pair<std::string, float>> groupGains_;
};
