#pragma once
// ── Video encoder (H.264 in MP4) ──────────────────────────────────────────────────────────────────
// App-independent. Takes NV12 frames (BT.709, limited range) and writes an .mp4. Two backends:
//   - "mf":     Windows Media Foundation's sink writer (the H.264 encoder that ships with Windows, a hardware
//               one when the driver provides it). Nothing to bundle or license: the default on Windows.
//   - "ffmpeg": an `ffmpeg` found on PATH, fed raw NV12 on its stdin (libx264). Any platform; nothing is shipped.
// "auto" = mf on Windows, else ffmpeg. Everything here runs on the caller's thread (VideoRecorder's worker):
// open, write and close may block.
#include <cstdint>
#include <memory>
#include <string>

struct VideoEncoderConfig
{
    std::string path;        // output .mp4 (UTF-8)
    int width = 0, height = 0; // both even
    double fps = 60.0;
    int bitrateKbps = 0;     // 0 = from the size and rate (~0.12 bit per pixel per frame)
    std::string backend = "auto"; // auto | mf | ffmpeg
};

class VideoEncoder
{
public:
    virtual ~VideoEncoder() = default;
    // y: width x height bytes; uv: width x height / 2 bytes (interleaved U,V). index: the frame's number from 0
    // (its time is index / fps). Returns false on an encoder error (see error()).
    virtual bool write(const uint8_t *y, const uint8_t *uv, int64_t index) = 0;
    virtual bool close() = 0;
    const std::string &error() const { return err_; }
    const std::string &name() const { return name_; }

protected:
    std::string err_, name_;
};

// Opens an encoder; on failure returns null and sets err (the reasons of every backend tried).
std::unique_ptr<VideoEncoder> openVideoEncoder(const VideoEncoderConfig &cfg, std::string &err);

// True when an `ffmpeg` executable answers on PATH (cached after the first call).
bool ffmpegOnPath();

// BGRA (or RGBA) 8-bit -> NV12, BT.709 limited range. src rows are srcStride bytes apart; w, h even.
void convertToNV12(const uint8_t *src, size_t srcStride, bool bgra, int w, int h, uint8_t *y, uint8_t *uv);
