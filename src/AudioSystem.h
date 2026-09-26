#pragma once
// Include miniaudio declarations (NOT the implementation — MINIAUDIO_IMPLEMENTATION
// is defined only in AudioSystem.cpp). This brings in the type definitions needed
// for the pointer members below without triggering any conflicting forward declarations.
#include "miniaudio.h"
#include "AmbientSynth.h" // Voice owns its synth (unique_ptr needs the complete type in every TU)

#include <functional>
#include <memory>
#include <string>
#include <vector>


// ── AudioSystem ───────────────────────────────────────────────────────────────
// Thin wrapper around miniaudio that provides:
//   • A sequential music playlist (streaming, no gap between tracks).
//   • Fire-and-forget UI sound effects.
//   • Three independent volume tiers: master > music, master > sfx.
//   • An ambience bus (master > ambience) of long-running voices — looping samples and
//     procedural AmbientSynth voices — each with its own gain, plus positioned one-shots.
//     The sim's Ambience table decides the gains; this class only plays them.
//   • Offline mode (harness runs): the engine has no device. Nothing is audible and nothing is
//     mixed until renderWav() pulls the mix synchronously into a WAV — a deterministic recording
//     of exactly what the listener would hear, for spectrograms and level checks.
//
// Lifecycle (managed by App):
//   init()      — start the audio device (WASAPI on Windows)
//   update(dt)  — call once per frame to advance the playlist on track end
//   cleanup()   — stop all audio and release the device
//
// Simulation integration:
//   App calls sim->setAudio(&audio) after both are initialised so each
//   simulation can set up its own playlist and trigger UI sounds from buildUI.
// ─────────────────────────────────────────────────────────────────────────────
class AudioSystem {
public:
    AudioSystem()  = default;
    ~AudioSystem();

    // Non-copyable — owns native audio resources.
    AudioSystem(const AudioSystem&)            = delete;
    AudioSystem& operator=(const AudioSystem&) = delete;

    void init(bool offline = false);
    void cleanup();
    void update(float dt);   // advance playlist when current track ends

    // ── Music playlist ───────────────────────────────────────────────────────
    // Tracks play in order, then loop back to the first track, with a silent GAP between tracks
    // (default 30 s) so the ambience has room to breathe. The player (Settings -> Sound) can pause,
    // skip and go back; pausing also freezes the gap countdown.
    void addTrack(const std::string& path);
    void clearTracks();
    void startMusic();       // begin from track 0
    void stopMusic();
    void playTrack(int idx); // jump to a track now (no gap)
    void nextTrack();
    void prevTrack();        // restarts the current track if more than 3 s in, else the previous one
    void setMusicPaused(bool paused);
    bool musicPaused() const { return musicPaused_; }
    void setMusicGap(float seconds) { musicGapS_ = seconds < 0.0f ? 0.0f : seconds; }
    float musicGap() const { return musicGapS_; }
    // Seconds until the next track starts while between tracks, else 0.
    float gapRemaining() const { return gapLeft_; }
    int  trackIndex() const { return trackIdx_; }
    int  trackCount() const { return (int)tracks_.size(); }
    std::string trackName(int idx) const;   // "gravity_wave.mp3" -> "Gravity Wave"
    float trackPosition() const;            // seconds into the current track
    float trackLength() const;              // seconds, 0 if unknown

    // ── UI sound effects (fire-and-forget, very short) ───────────────────────
    void playSfx(const std::string& path);

    // ── Volume controls (0.0 – 1.0) ──────────────────────────────────────────
    // master  : overall output gain applied to both music and sfx
    // music   : sub-mix gain for streaming background tracks only
    // sfx     : sub-mix gain for UI sound effects only
    void  setMasterVolume(float v);
    void  setMusicVolume(float v);
    void  setSfxVolume(float v);
    float getMasterVolume() const { return masterVol_; }
    float getMusicVolume()  const { return musicVol_;  }
    float getSfxVolume()    const { return sfxVol_;    }
    // A multiplier on the music bus under the user's music volume (the sim fades music out with
    // altitude). 0..1; not persisted.
    void  setMusicFade(float f);
    float musicFade() const { return musicFade_; }

    bool isInitialized() const { return initialized_; }
    bool isOffline() const { return offline_; }
    uint32_t sampleRate() const;

    // ── Ambience bus ─────────────────────────────────────────────────────────
    // A voice plays continuously while its gain is > 0 and is stopped (costs nothing) at 0.
    // Returns a handle >= 0, or -1 if the file could not be loaded.
    int  addAmbienceLoop(const std::string& path);
    int  addAmbienceSynth(std::unique_ptr<AmbientSynth> synth);
    void setVoiceGain(int voice, float gain);
    float voiceGain(int voice) const;
    AmbientSynth* voiceSynth(int voice) const;
    void clearAmbience();
    // One-shot on the ambience bus (a gull call): pan in [-1, 1]. Finished ones are reaped in update().
    void playAmbienceOneShot(const std::string& path, float gain, float pan);
    void  setAmbienceVolume(float v);
    float getAmbienceVolume() const { return ambienceVol_; }

    // ── Offline render (offline mode only) ───────────────────────────────────
    enum Bus : uint32_t { BusMusic = 1, BusSfx = 2, BusAmbience = 4, BusAll = 7 };
    struct RenderStats {
        double rmsDb = -120.0, peakDb = -120.0;   // over the whole recording, both channels
        std::vector<double> rmsDbPerSecond;
        double seconds = 0.0;
    };
    // Renders `seconds` of the mix (after a short discarded pre-roll that lets volume ramps and
    // just-started voices settle) into a 16-bit stereo WAV. `busMask` mutes the other buses and
    // `soloVoice` >= 0 mutes every other ambience voice for the duration. `perChunk(dt)` runs
    // between 1/60 s chunks so event layers keep scheduling while the render runs.
    bool renderWav(const std::string& path, double seconds, uint32_t busMask, int soloVoice,
                   const std::function<void(float)>& perChunk, RenderStats& stats, std::string& err);

private:
    void loadTrack(int idx);    // load + start tracks_[idx]
    void applyMusicVolume();    // push musicVol_ into the music group
    void applyVoiceVolume(int voice);

    struct Voice {
        ma_sound* sound = nullptr;
        std::unique_ptr<AmbientSynth> synth; // null for a sample loop
        float gain = 0.0f;
        bool playing = false;
    };
    struct OneShot {
        ma_sound* sound = nullptr;
    };

    ma_engine*      engine_     = nullptr;
    ma_sound_group* musicGroup_ = nullptr;  // music sub-mix node
    ma_sound_group* sfxGroup_   = nullptr;  // SFX  sub-mix node
    ma_sound_group* ambienceGroup_ = nullptr;
    ma_sound*       music_      = nullptr;  // currently streaming track

    std::vector<Voice>   voices_;
    std::vector<OneShot> oneShots_;
    int  solo_ = -1;            // renderWav's solo voice, -1 = none

    std::vector<std::string> tracks_;
    int  trackIdx_   = 0;
    bool  musicPaused_ = false;
    bool  musicOn_     = false;   // startMusic() called and not stopped
    float musicGapS_   = 30.0f;
    float gapLeft_     = 0.0f;
    bool initialized_= false;
    bool offline_    = false;
    bool musicDecoded_ = false; // offline: music loaded fully decoded (see loadTrack)

    float masterVol_ = 0.8f;
    float musicVol_  = 0.6f;
    float musicFade_ = 1.0f;
    float sfxVol_    = 1.0f;
    float ambienceVol_ = 0.7f;
};
