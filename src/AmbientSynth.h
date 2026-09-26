#pragma once
// ── AmbientSynth ──────────────────────────────────────────────────────────────────────────────────
// Procedural ambience voices: wind and air, a noise hum, a harmonic drone (phaser, whine,
// compressor cycling, soft low tones), FSK data beeps, disk/server activity, magnetospheric VLF
// "chorus" (flanged) and breaking surf. Each synth is a miniaudio data source (f32 stereo
// at the engine's rate) that AudioSystem plays through an ma_sound on the ambience bus, so its
// level, the bus volume and the master volume apply like they do to a sample.
//
// Threading: the main thread writes parameters with setParam() (atomics); the audio thread reads
// them once per kBlock frames and eases toward them per block, so a parameter change never clicks.
// Everything else is audio-thread state. A synth never allocates or locks while rendering.
//
// Deterministic: all randomness comes from a per-voice xorshift seeded at creation, so an offline
// harness render of the same scene is the same waveform every run.
//
// No tonal "melody" content by design (the ambience must sit under the music): pitched material is
// either filtered noise (a hum is noise through resonances, not a sine), short data blips, or sweeps.
// ─────────────────────────────────────────────────────────────────────────────────────────────────
#include "miniaudio.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class AmbientSynth
{
public:
    static constexpr int kMaxParams = 24;
    static constexpr uint32_t kBlock = 64; // parameter update granularity (frames)

    struct ParamDef
    {
        const char *name;
        float def;
    };

    virtual ~AmbientSynth();

    // "wind", "hum", "drone", "beeps", "disk", "chorus", "surf"; null for an unknown kind.
    static std::unique_ptr<AmbientSynth> create(const std::string &kind, uint32_t sampleRate, uint32_t seed);
    static std::vector<std::string> kinds();

    const char *kind() const { return kind_; }
    int paramCount() const { return paramCount_; }
    const ParamDef &paramDef(int i) const { return defs_[i]; }
    int paramIndex(const std::string &name) const; // -1 if this kind has no such parameter
    void setParam(int i, float v);
    float param(int i) const;

    // Interleaved stereo. Audio thread (or the offline render on the main thread).
    void render(float *out, uint32_t frames);

    ma_data_source *dataSource() { return &ds_.base; }

protected:
    AmbientSynth(const char *kind, const ParamDef *defs, int count, uint32_t sampleRate, uint32_t seed);
    // One block of at most kBlock frames, parameters in p_ (already eased).
    virtual void block(float *out, uint32_t frames) = 0;

    // Smoothed parameter values, read by block().
    float p_[kMaxParams] = {};
    float sr_;
    uint32_t rng_;

    float uni();      // [0, 1)
    float bip();      // [-1, 1)
    float gauss();    // ~N(0, 1), cheap (sum of 4 uniforms)

private:
    struct DataSource
    {
        ma_data_source_base base;
        AmbientSynth *self;
    };
    static ma_result dsRead(ma_data_source *ds, void *out, ma_uint64 frames, ma_uint64 *read);
    static ma_result dsSeek(ma_data_source *ds, ma_uint64 frame);
    static ma_result dsFormat(ma_data_source *ds, ma_format *fmt, ma_uint32 *ch, ma_uint32 *sr,
                              ma_channel *map, size_t mapCap);
    static ma_result dsCursor(ma_data_source *ds, ma_uint64 *cursor);
    static ma_result dsLength(ma_data_source *ds, ma_uint64 *len);
    static const ma_data_source_vtable kVtable;

    const char *kind_;
    const ParamDef *defs_;
    int paramCount_;
    std::atomic<float> target_[kMaxParams];
    bool first_ = true;
    DataSource ds_{};
};
