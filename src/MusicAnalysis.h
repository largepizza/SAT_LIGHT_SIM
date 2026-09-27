#pragma once
// ── MusicAnalysis ─────────────────────────────────────────────────────────────────────────────────
// What key is the music in, and where is its pitch right now? The ambience's tonal root (every
// pitched voice is a multiple of it, Ambience.h) follows the soundtrack, so this reads each track
// once and caches the answer:
//
//   tuning        the track's deviation from A440 in cents, from the phase of every spectral peak
//                 against the semitone grid (a circular mean, weighted by amplitude), plus a CURVE
//                 of it over time (8 s windows, 1 s steps). gravity_wave's pitch-bending piano is a
//                 +-30 cent swing with a ~50 s period in that curve, and the sim's root rides it.
//   chroma/bass   energy per pitch class from the peaks (55 Hz - 2.5 kHz), with the local tuning
//                 removed first; the bass one is the same below 160 Hz
//   key           Krumhansl-Kessler major/minor profiles correlated with chroma + bass/2
//   pitch set     the 3-7 strongest pitch classes (always the tonic): what the track actually
//                 plays, which is what a voice has to stay inside to sit with it. `scaleName` is
//                 only the nearest named scale, for reading.
//   chords        per 3 s segment, cosine match against triad / sus / power-chord templates, merged
//                 while unchanged; `homeChord` is the one held longest
//
// Chroma-template harmony is rough by nature — sus4 and sus2 on different roots are the same notes,
// and a sustained pad blurs a progression — but for ambient material (drones, pedals, slow pads) it
// is dependable, and a chord here is only ever used as a SET of pitch classes.
//
// Analysis: mono, 11025 Hz (miniaudio decodes and resamples), 8192-point Hann FFT (1.35 Hz bins, so
// semitones are resolved down to ~40 Hz), hop 2048. ~0.1 s per minute of music on one core, plus
// the MP3 decode.
//
// Library: analyses a playlist on a worker thread, reading and writing a JSON cache per track
// (<cache dir>/<track>.analysis.json, keyed by file size, write time and kAnalysisVersion), so a
// track dropped into the music folder is analysed once on its first launch and read back after.
// Nothing here touches the audio device; the CLI (tools/sound_tool) runs the same code.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace music
{
constexpr int kAnalysisVersion = 1;

struct Chord
{
    float t0 = 0.0f, t1 = 0.0f; // seconds
    int rootPc = -1;            // -1 = no chord (silence)
    std::string quality;        // "", "m", "dim", "aug", "sus2", "sus4", "5"
    uint16_t mask = 0;          // absolute pitch classes, bit 0 = C
    float confidence = 0.0f;    // cosine against the template
};

struct Analysis
{
    std::string file; // file name, no directory
    uint64_t fileSize = 0;
    int64_t fileTime = 0; // last-write time in the filesystem clock's ticks (a cache key, nothing more)
    float durationS = 0.0f;

    float tuningCents = 0.0f;      // against A440, the whole track
    float tuningConfidence = 0.0f; // length of the mean phase vector, 0..1
    float curveStepS = 1.0f;
    std::vector<float> tuningCurve; // cents, one per curveStepS from t = 0

    float chroma[12] = {}, bass[12] = {}; // normalised to a maximum of 1
    int tonicPc = 9;
    bool minor = true;
    float keyConfidence = 0.0f; // Pearson r of the best key
    int bassPc = -1;
    uint16_t pitchSet = 0; // absolute
    std::string scaleName; // nearest named scale on the tonic ("Aeolian", "major pentatonic", ...)
    std::vector<Chord> chords;
    uint16_t homeChord = 0; // absolute mask of the chord held longest

    std::string keyName() const;         // "Ab major"
    float tuningAt(float t) const;       // cents at t (interpolated; the global value if no curve)
    const Chord *chordAt(float t) const; // null in a gap between chords
    // The tonic's frequency in [lowHz, 2 lowHz), at `cents` of tuning.
    double tonicHz(double lowHz, float cents) const;
};

// Pitch-class helpers. A RELATIVE mask has bit k = k semitones above the tonic.
std::string pcName(int pc);                        // "C", "C#", "D", "Eb", ...
uint16_t toRelative(uint16_t absMask, int tonicPc);
std::string intervalList(uint16_t relMask);        // "1 2 b3 4 5 b6 b7"
std::string chordName(const Chord &c);             // "Absus4", "Em", "N"

bool analyzeFile(const std::string &path, Analysis &out, std::string &err,
                 const std::atomic<bool> *cancel = nullptr);
nlohmann::json toJson(const Analysis &a);
bool fromJson(const nlohmann::json &j, Analysis &a);

class Library
{
public:
    ~Library();
    // Analyses every track, cache first. cacheDirs[0] is where new results are written; the rest
    // are read-only fallbacks (the exe's own folder, when a harness run points user data elsewhere).
    // wait = true runs on the calling thread and returns when done (deterministic harness runs).
    void start(const std::vector<std::string> &tracks, const std::vector<std::string> &cacheDirs, bool wait);
    std::shared_ptr<const Analysis> get(const std::string &trackPath) const; // null until analysed
    int pending() const { return pending_.load(); }
    // Log lines produced since the last call (the worker cannot use the app's log directly).
    std::vector<std::string> takeMessages();

private:
    void run(std::vector<std::string> tracks, std::vector<std::string> dirs);
    void say(const std::string &s);

    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<const Analysis>> done_;
    std::vector<std::string> messages_;
    std::atomic<int> pending_{0};
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::thread worker_;
};
} // namespace music
