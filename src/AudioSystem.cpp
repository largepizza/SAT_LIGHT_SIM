// AudioSystem.cpp — miniaudio implementation.
// MINIAUDIO_IMPLEMENTATION must appear in exactly one translation unit.
// miniaudio pulls in minimp3 and minivorbis automatically; no extra libs needed.
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "AudioSystem.h"
#include "AmbientSynth.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

AudioSystem::~AudioSystem() { cleanup(); }

// ── init ─────────────────────────────────────────────────────────────────────
void AudioSystem::init(bool offline)
{
    engine_ = new ma_engine;
    ma_engine_config cfg = ma_engine_config_init();
    // Every volume change is ramped over 50 ms: the ambience eases layer gains once per frame,
    // and unsmoothed per-frame steps are audible as zipper noise on a steady wind.
    cfg.defaultVolumeSmoothTimeInPCMFrames = 2400;
    if (offline) {
        // Harness: no device at all. The graph is only pulled by renderWav().
        cfg.noDevice   = MA_TRUE;
        cfg.channels   = 2;
        cfg.sampleRate = 48000;
    }
    if (ma_engine_init(&cfg, engine_) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] ma_engine_init failed — audio disabled.\n");
        delete engine_;
        engine_ = nullptr;
        return;
    }
    ma_engine_set_volume(engine_, masterVol_);

    // Music sub-mix: all streaming background tracks attach here.
    musicGroup_ = new ma_sound_group;
    if (ma_sound_group_init(engine_, 0, nullptr, musicGroup_) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] Failed to create music group.\n");
        delete musicGroup_;
        musicGroup_ = nullptr;
    } else {
        ma_sound_group_set_volume(musicGroup_, musicVol_);
    }

    // SFX sub-mix: all fire-and-forget UI sounds attach here.
    sfxGroup_ = new ma_sound_group;
    if (ma_sound_group_init(engine_, 0, nullptr, sfxGroup_) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] Failed to create SFX group.\n");
        delete sfxGroup_;
        sfxGroup_ = nullptr;
    } else {
        ma_sound_group_set_volume(sfxGroup_, sfxVol_);
    }

    // Ambience sub-mix: looping beds, procedural voices and ambience one-shots.
    ambienceGroup_ = new ma_sound_group;
    if (ma_sound_group_init(engine_, 0, nullptr, ambienceGroup_) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] Failed to create ambience group.\n");
        delete ambienceGroup_;
        ambienceGroup_ = nullptr;
    } else {
        ma_sound_group_set_volume(ambienceGroup_, ambienceVol_);
    }

    offline_ = offline;
    initialized_ = true;
}

// ── cleanup ───────────────────────────────────────────────────────────────────
void AudioSystem::cleanup()
{
    if (!initialized_) return;

    stopMusic();
    clearAmbience();

    if (ambienceGroup_) {
        ma_sound_group_uninit(ambienceGroup_);
        delete ambienceGroup_;
        ambienceGroup_ = nullptr;
    }
    if (sfxGroup_) {
        ma_sound_group_uninit(sfxGroup_);
        delete sfxGroup_;
        sfxGroup_ = nullptr;
    }
    if (musicGroup_) {
        ma_sound_group_uninit(musicGroup_);
        delete musicGroup_;
        musicGroup_ = nullptr;
    }
    if (engine_) {
        ma_engine_uninit(engine_);
        delete engine_;
        engine_ = nullptr;
    }

    initialized_ = false;
}

// ── update ────────────────────────────────────────────────────────────────────
// Poll for track completion each frame and advance the playlist.
// miniaudio's end-of-stream callback runs on the audio thread, so polling
// ma_sound_at_end() from the main thread is the safe, simple approach here.
void AudioSystem::update(float dt)
{
    if (!initialized_) return;

    // Reap finished ambience one-shots.
    for (size_t i = 0; i < oneShots_.size();) {
        if (ma_sound_at_end(oneShots_[i].sound)) {
            ma_sound_uninit(oneShots_[i].sound);
            delete oneShots_[i].sound;
            oneShots_[i] = oneShots_.back();
            oneShots_.pop_back();
        } else {
            ++i;
        }
    }

    if (!musicOn_ || musicPaused_ || tracks_.empty()) return;

    // Between tracks: count the gap down, then start the next one.
    if (!music_) {
        gapLeft_ -= dt;
        if (gapLeft_ <= 0.0f) {
            gapLeft_ = 0.0f;
            trackIdx_ = (trackIdx_ + 1) % (int)tracks_.size();
            loadTrack(trackIdx_);
        }
        return;
    }
    if (ma_sound_at_end(music_)) {
        ma_sound_uninit(music_);
        delete music_;
        music_ = nullptr;
        gapLeft_ = musicGapS_;
        if (gapLeft_ <= 0.0f) {
            trackIdx_ = (trackIdx_ + 1) % (int)tracks_.size();
            loadTrack(trackIdx_);
        }
    }
}

void AudioSystem::playTrack(int idx)
{
    if (!initialized_ || tracks_.empty()) return;
    trackIdx_ = ((idx % (int)tracks_.size()) + (int)tracks_.size()) % (int)tracks_.size();
    gapLeft_ = 0.0f;
    musicOn_ = true;
    loadTrack(trackIdx_);
    if (musicPaused_ && music_) ma_sound_stop(music_);
}

void AudioSystem::nextTrack() { playTrack(trackIdx_ + 1); }

void AudioSystem::prevTrack()
{
    if (music_ && trackPosition() > 3.0f)
        playTrack(trackIdx_);
    else
        playTrack(trackIdx_ - 1);
}

void AudioSystem::setMusicPaused(bool paused)
{
    musicPaused_ = paused;
    if (!music_) return;
    if (paused) ma_sound_stop(music_);   // keeps the cursor: resume continues from here
    else        ma_sound_start(music_);
}

std::string AudioSystem::trackName(int idx) const
{
    if (idx < 0 || idx >= (int)tracks_.size()) return std::string();
    std::string s = tracks_[idx];
    const size_t slash = s.find_last_of("/\\");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const size_t dot = s.find_last_of('.');
    if (dot != std::string::npos) s = s.substr(0, dot);
    bool up = true;
    for (char& c : s) {
        if (c == '_' || c == '-') { c = ' '; up = true; }
        else if (up) { c = (char)toupper((unsigned char)c); up = false; }
    }
    // Orbit acronyms stay acronyms: "leo_motif" -> "LEO Motif", not "Leo Motif".
    for (const char* w : {"Leo", "Meo", "Geo", "Heo"}) {
        for (size_t p = s.find(w); p != std::string::npos; p = s.find(w, p + 3)) {
            if (p + 3 == s.size() || s[p + 3] == ' ')
                for (size_t k = 1; k < 3; ++k) s[p + k] = (char)toupper((unsigned char)s[p + k]);
        }
    }
    return s;
}

float AudioSystem::trackPosition() const
{
    float t = 0.0f;
    if (music_) ma_sound_get_cursor_in_seconds(music_, &t);
    return t;
}

float AudioSystem::trackLength() const
{
    float t = 0.0f;
    if (music_) ma_sound_get_length_in_seconds(music_, &t);
    return t;
}

// ── playlist management ───────────────────────────────────────────────────────
void AudioSystem::addTrack(const std::string& path)
{
    tracks_.push_back(path);
}

void AudioSystem::clearTracks()
{
    stopMusic();
    tracks_.clear();
    trackIdx_ = 0;
}

void AudioSystem::startMusic()
{
    if (tracks_.empty() || !initialized_) return;
    trackIdx_ = 0;
    gapLeft_ = 0.0f;
    musicOn_ = true;
    loadTrack(0);
}

void AudioSystem::stopMusic()
{
    musicOn_ = false;
    if (music_) {
        ma_sound_uninit(music_);
        delete music_;
        music_ = nullptr;
    }
}

// ── loadTrack (private) ───────────────────────────────────────────────────────
// Stream from disk (MA_SOUND_FLAG_STREAM) so large music files don't sit in RAM.
// The file is read relative to the working directory, which is the exe directory
// at runtime (matching the same convention used by shader and icon loading).
void AudioSystem::loadTrack(int idx)
{
    if (!initialized_ || idx < 0 || idx >= (int)tracks_.size()) return;

    if (music_) { // release the old track (not stopMusic(): that also switches the player off)
        ma_sound_uninit(music_);
        delete music_;
        music_ = nullptr;
    }
    music_ = new ma_sound;

    // Stream; don't decode the entire file. Offline (harness) renders pull the graph far faster than
    // real time and outrun the streamer's page loads — every ~1 s of music came out with a 0.25 s
    // hole — so offline the track is decoded up front, once a recording actually asks for music.
    ma_uint32 flags = (offline_ && musicDecoded_) ? MA_SOUND_FLAG_DECODE : MA_SOUND_FLAG_STREAM;
    if (ma_sound_init_from_file(engine_,
                                tracks_[idx].c_str(),
                                flags,
                                musicGroup_,   // attach to music sub-mix
                                nullptr,
                                music_) != MA_SUCCESS)
    {
        fprintf(stderr, "[AudioSystem] Failed to load track: %s\n", tracks_[idx].c_str());
        delete music_;
        music_ = nullptr;
        return;
    }

    ma_sound_start(music_);
}

// ── SFX ───────────────────────────────────────────────────────────────────────
// ma_engine_play_sound creates a self-managed, fire-and-forget node attached to
// sfxGroup_ so the SFX sub-mix volume applies automatically.
void AudioSystem::playSfx(const std::string& path)
{
    if (!initialized_) return;
    ma_engine_play_sound(engine_, path.c_str(), sfxGroup_);
}

// ── Volume setters ────────────────────────────────────────────────────────────
void AudioSystem::setMasterVolume(float v)
{
    masterVol_ = std::clamp(v, 0.0f, 1.0f);
    if (engine_) ma_engine_set_volume(engine_, masterVol_);
}

void AudioSystem::setMusicVolume(float v)
{
    musicVol_ = std::clamp(v, 0.0f, 1.0f);
    applyMusicVolume();
}

void AudioSystem::setSfxVolume(float v)
{
    sfxVol_ = std::clamp(v, 0.0f, 1.0f);
    if (sfxGroup_) ma_sound_group_set_volume(sfxGroup_, sfxVol_);
}

void AudioSystem::setMusicFade(float f)
{
    f = std::clamp(f, 0.0f, 1.0f);
    if (f == musicFade_) return;
    musicFade_ = f;
    applyMusicVolume();
}

void AudioSystem::applyMusicVolume()
{
    if (musicGroup_) ma_sound_group_set_volume(musicGroup_, musicVol_ * musicFade_);
}

uint32_t AudioSystem::sampleRate() const
{
    return engine_ ? ma_engine_get_sample_rate(engine_) : 48000;
}

void AudioSystem::setAmbienceVolume(float v)
{
    ambienceVol_ = std::clamp(v, 0.0f, 1.0f);
    if (ambienceGroup_) ma_sound_group_set_volume(ambienceGroup_, ambienceVol_);
}

// ── Ambience voices ───────────────────────────────────────────────────────────
// Loops STREAM in the live app: a voice is created the first time its layer becomes audible, on
// the main thread, and decoding a 36 s FLAC there would be a visible hitch (and ~9 MB of PCM each).
// Offline renders outrun the streamer (see loadTrack), so there they are decoded up front.
int AudioSystem::addAmbienceLoop(const std::string& path)
{
    if (!initialized_ || !ambienceGroup_) return -1;
    auto* snd = new ma_sound;
    const ma_uint32 flags = (offline_ ? MA_SOUND_FLAG_DECODE : MA_SOUND_FLAG_STREAM) | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(engine_, path.c_str(), flags, ambienceGroup_, nullptr, snd) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] Failed to load ambience loop: %s\n", path.c_str());
        delete snd;
        return -1;
    }
    ma_sound_set_looping(snd, MA_TRUE);
    ma_sound_set_volume(snd, 0.0f);
    Voice v;
    v.sound = snd;
    voices_.push_back(std::move(v));
    return (int)voices_.size() - 1;
}

int AudioSystem::addAmbienceSynth(std::unique_ptr<AmbientSynth> synth)
{
    if (!initialized_ || !ambienceGroup_ || !synth) return -1;
    auto* snd = new ma_sound;
    if (ma_sound_init_from_data_source(engine_, synth->dataSource(), MA_SOUND_FLAG_NO_SPATIALIZATION,
                                       ambienceGroup_, snd) != MA_SUCCESS) {
        fprintf(stderr, "[AudioSystem] Failed to create ambience synth '%s'\n", synth->kind());
        delete snd;
        return -1;
    }
    ma_sound_set_volume(snd, 0.0f);
    Voice v;
    v.sound = snd;
    v.synth = std::move(synth);
    voices_.push_back(std::move(v));
    return (int)voices_.size() - 1;
}

void AudioSystem::applyVoiceVolume(int i)
{
    Voice& v = voices_[i];
    const float g = (solo_ < 0 || solo_ == i) ? v.gain : 0.0f;
    ma_sound_set_volume(v.sound, g);
    // Start/stop on the voice's own gain (not the solo-muted one), so a solo render does not
    // restart the other voices when it ends. A stopped voice costs nothing.
    const bool want = v.gain > 1e-4f;
    if (want && !v.playing) {
        ma_sound_start(v.sound);
        v.playing = true;
    } else if (!want && v.playing) {
        ma_sound_stop(v.sound);
        v.playing = false;
    }
}

void AudioSystem::setVoiceGain(int voice, float gain)
{
    if (voice < 0 || voice >= (int)voices_.size()) return;
    voices_[voice].gain = (std::max)(0.0f, gain);
    applyVoiceVolume(voice);
}

float AudioSystem::voiceGain(int voice) const
{
    return (voice >= 0 && voice < (int)voices_.size()) ? voices_[voice].gain : 0.0f;
}

AmbientSynth* AudioSystem::voiceSynth(int voice) const
{
    return (voice >= 0 && voice < (int)voices_.size()) ? voices_[voice].synth.get() : nullptr;
}

void AudioSystem::clearAmbience()
{
    for (Voice& v : voices_) {
        if (v.sound) {
            ma_sound_uninit(v.sound); // detaches from the graph before the synth it reads goes away
            delete v.sound;
        }
    }
    voices_.clear();
    for (OneShot& o : oneShots_) {
        ma_sound_uninit(o.sound);
        delete o.sound;
    }
    oneShots_.clear();
}

void AudioSystem::playAmbienceOneShot(const std::string& path, float gain, float pan)
{
    if (!initialized_ || !ambienceGroup_ || gain <= 1e-4f) return;
    if (solo_ >= 0) return;             // a solo render hears one voice, not the gulls as well
    if (oneShots_.size() >= 24) return; // a runaway event rate must not pile up voices
    auto* snd = new ma_sound;
    // DECODE goes through the resource manager's cache: the file is decoded once and shared.
    const ma_uint32 flags = MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_NO_SPATIALIZATION;
    if (ma_sound_init_from_file(engine_, path.c_str(), flags, ambienceGroup_, nullptr, snd) != MA_SUCCESS) {
        delete snd;
        return;
    }
    ma_sound_set_volume(snd, gain);
    ma_sound_set_pan(snd, std::clamp(pan, -1.0f, 1.0f));
    ma_sound_start(snd);
    oneShots_.push_back({snd});
}

// ── Offline render ────────────────────────────────────────────────────────────
namespace {
double toDb(double v) { return v > 1e-9 ? 20.0 * std::log10(v) : -120.0; }

bool writeWav16(const std::string& path, const std::vector<int16_t>& pcm, uint32_t sr, uint16_t ch)
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    const uint32_t dataBytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    f.write("RIFF", 4); u32(36 + dataBytes); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(ch); u32(sr); u32(sr * ch * 2); u16((uint16_t)(ch * 2)); u16(16);
    f.write("data", 4); u32(dataBytes);
    f.write(reinterpret_cast<const char*>(pcm.data()), dataBytes);
    return (bool)f;
}
} // namespace

bool AudioSystem::renderWav(const std::string& path, double seconds, uint32_t busMask, int soloVoice,
                            const std::function<void(float)>& perChunk, RenderStats& stats, std::string& err)
{
    if (!initialized_) { err = "audio is not initialised"; return false; }
    if (!offline_) { err = "renderWav needs offline mode (a harness run without --sound)"; return false; }
    seconds = std::clamp(seconds, 0.1, 120.0);
    const uint32_t sr = sampleRate();
    const uint32_t chunk = sr / 60;

    // Mute what was not asked for; restore afterwards.
    auto groupVol = [](ma_sound_group* g, float v) { if (g) ma_sound_group_set_volume(g, v); };
    groupVol(musicGroup_, (busMask & BusMusic) ? musicVol_ * musicFade_ : 0.0f);
    groupVol(sfxGroup_, (busMask & BusSfx) ? sfxVol_ : 0.0f);
    groupVol(ambienceGroup_, (busMask & BusAmbience) ? ambienceVol_ : 0.0f);
    if ((busMask & BusMusic) && !musicDecoded_ && music_) {
        musicDecoded_ = true;
        loadTrack(trackIdx_);
    }
    solo_ = (soloVoice >= 0 && soloVoice < (int)voices_.size()) ? soloVoice : -1;
    for (int i = 0; i < (int)voices_.size(); ++i) applyVoiceVolume(i);
    if (solo_ >= 0)
        for (OneShot& o : oneShots_) ma_sound_set_volume(o.sound, 0.0f);

    std::vector<float> buf((size_t)chunk * 2);
    auto pull = [&](uint32_t frames) {
        ma_uint64 got = 0;
        ma_engine_read_pcm_frames(engine_, buf.data(), frames, &got);
        for (size_t i = (size_t)got * 2; i < (size_t)frames * 2; ++i) buf[i] = 0.0f;
    };
    // Pre-roll: lets the 50 ms volume ramps above and just-started voices settle.
    for (uint32_t done = 0; done < sr / 4; done += chunk) {
        if (perChunk) perChunk((float)chunk / (float)sr);
        pull(chunk);
    }

    const uint64_t total = (uint64_t)(seconds * sr);
    std::vector<int16_t> pcm;
    pcm.reserve((size_t)total * 2);
    double sumSq = 0.0, peak = 0.0, secSq = 0.0;
    uint64_t secFrames = 0;
    stats.rmsDbPerSecond.clear();
    for (uint64_t done = 0; done < total; done += chunk) {
        const uint32_t n = (uint32_t)(std::min<uint64_t>)(chunk, total - done);
        if (perChunk) perChunk((float)n / (float)sr);
        pull(n);
        for (uint32_t i = 0; i < n * 2; ++i) {
            const double v = buf[i];
            sumSq += v * v;
            secSq += v * v;
            peak = (std::max)(peak, std::fabs(v));
            pcm.push_back((int16_t)std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
        }
        secFrames += n;
        if (secFrames >= sr) {
            stats.rmsDbPerSecond.push_back(toDb(std::sqrt(secSq / (double)(secFrames * 2))));
            secSq = 0.0;
            secFrames = 0;
        }
    }

    solo_ = -1;
    for (int i = 0; i < (int)voices_.size(); ++i) applyVoiceVolume(i);
    groupVol(musicGroup_, musicVol_ * musicFade_);
    groupVol(sfxGroup_, sfxVol_);
    groupVol(ambienceGroup_, ambienceVol_);

    stats.seconds = (double)total / sr;
    stats.rmsDb = toDb(std::sqrt(sumSq / (std::max<double>)(1.0, (double)total * 2)));
    stats.peakDb = toDb(peak);
    if (!writeWav16(path, pcm, sr, 2)) { err = "could not write " + path; return false; }
    return true;
}
