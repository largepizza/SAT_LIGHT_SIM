// Video encoder backends (see VideoEncoder.h).
#include "VideoEncoder.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <codecapi.h>
#include <strmif.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

// ─── NV12 conversion ──────────────────────────────────────────────────────────────────────────────
// BT.709 limited range on the sRGB-encoded bytes (what the swapchain holds), 8.8 fixed point:
//   Y  = 16  + ( 47 R + 157 G +  16 B) / 256
//   Cb = 128 + (-26 R -  87 G + 112 B) / 256
//   Cr = 128 + (112 R - 102 G -  10 B) / 256
// Chroma from the 2x2 block's mean.
void convertToNV12(const uint8_t *src, size_t srcStride, bool bgra, int w, int h, uint8_t *y, uint8_t *uv)
{
    const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
    for (int row = 0; row < h; row += 2)
    {
        const uint8_t *s0 = src + (size_t)row * srcStride;
        const uint8_t *s1 = s0 + srcStride;
        uint8_t *y0 = y + (size_t)row * w;
        uint8_t *y1 = y0 + w;
        uint8_t *c = uv + (size_t)(row / 2) * w;
        for (int x = 0; x < w; x += 2)
        {
            const uint8_t *p00 = s0 + x * 4, *p01 = p00 + 4, *p10 = s1 + x * 4, *p11 = p10 + 4;
            const int r00 = p00[ri], g00 = p00[1], b00 = p00[bi];
            const int r01 = p01[ri], g01 = p01[1], b01 = p01[bi];
            const int r10 = p10[ri], g10 = p10[1], b10 = p10[bi];
            const int r11 = p11[ri], g11 = p11[1], b11 = p11[bi];
            y0[x] = (uint8_t)(16 + ((47 * r00 + 157 * g00 + 16 * b00 + 128) >> 8));
            y0[x + 1] = (uint8_t)(16 + ((47 * r01 + 157 * g01 + 16 * b01 + 128) >> 8));
            y1[x] = (uint8_t)(16 + ((47 * r10 + 157 * g10 + 16 * b10 + 128) >> 8));
            y1[x + 1] = (uint8_t)(16 + ((47 * r11 + 157 * g11 + 16 * b11 + 128) >> 8));
            const int r = r00 + r01 + r10 + r11, g = g00 + g01 + g10 + g11, b = b00 + b01 + b10 + b11; // x4
            c[x] = (uint8_t)(128 + ((-26 * r - 87 * g + 112 * b + 512) >> 10));
            c[x + 1] = (uint8_t)(128 + ((112 * r - 102 * g - 10 * b + 512) >> 10));
        }
    }
}

static int autoBitrateKbps(const VideoEncoderConfig &c)
{
    if (c.bitrateKbps > 0)
        return c.bitrateKbps;
    const double bps = 0.12 * (double)c.width * (double)c.height * c.fps;
    return (int)std::clamp(bps / 1000.0, 2000.0, 80000.0);
}

// The frame rate as a ratio (29.97 -> 30000/1001-ish, 60 -> 60/1).
static void fpsRatio(double fps, uint32_t &num, uint32_t &den)
{
    uint64_t n = (uint64_t)std::llround(fps * 1000.0), d = 1000;
    const uint64_t g = std::gcd(n, d);
    num = (uint32_t)(n / g);
    den = (uint32_t)(d / g);
}

// ─── ffmpeg on PATH ───────────────────────────────────────────────────────────────────────────────
static std::string ffmpegPathFound()
{
    static int state = 0; // 0 unknown, 1 found, 2 not found
    static std::string found;
    if (state)
        return found;
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr) > 0)
    {
        char u8[MAX_PATH * 3];
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof(u8), nullptr, nullptr);
        found = u8;
    }
#else
    const char *p = std::getenv("PATH");
    std::string path = p ? p : "";
    size_t a = 0;
    while (a <= path.size())
    {
        size_t b = path.find(':', a);
        if (b == std::string::npos) b = path.size();
        std::string cand = path.substr(a, b - a) + "/ffmpeg";
        if (b > a && access(cand.c_str(), X_OK) == 0)
        {
            found = cand;
            break;
        }
        a = b + 1;
    }
#endif
    state = found.empty() ? 2 : 1;
    return found;
}

bool ffmpegOnPath() { return !ffmpegPathFound().empty(); }

// Runs a command line to completion; returns its exit code (-1 if it could not start).
static int runCommand(const std::string &cmd)
{
#ifdef _WIN32
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring wcmd(cmd.size() + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, cmd.c_str(), -1, wcmd.data(), (int)wcmd.size());
    if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                        nullptr, nullptr, &si, &pi))
        return -1;
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 600000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
#else
    return std::system(cmd.c_str());
#endif
}

static FILE *openUtf8(const std::string &path, const char *mode)
{
#ifdef _WIN32
    std::wstring w(path.size() + 1, L'\0'), m(strlen(mode) + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), (int)w.size());
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, m.data(), (int)m.size());
    return _wfopen(w.c_str(), m.c_str());
#else
    return fopen(path.c_str(), mode);
#endif
}

static void removeUtf8(const std::string &path)
{
#ifdef _WIN32
    std::wstring w(path.size() + 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), (int)w.size());
    DeleteFileW(w.c_str());
#else
    unlink(path.c_str());
#endif
}

class FfmpegEncoder : public VideoEncoder
{
public:
    bool open(const VideoEncoderConfig &c, const std::string &exe)
    {
        name_ = "ffmpeg";
        w_ = c.width;
        h_ = c.height;
        exe_ = exe;
        path_ = c.path;
        audioRate_ = c.audioRate;
        audioKbps_ = c.audioKbps;
        // With sound, the video goes to a side file first and the sound to a raw float one; close() muxes them.
        vpath_ = audioRate_ > 0 ? c.path + ".video.mp4" : c.path;
        if (audioRate_ > 0)
        {
            apath_ = c.path + ".sound.f32";
            afile_ = openUtf8(apath_, "wb");
            if (!afile_)
            {
                err_ = "ffmpeg: could not create " + apath_;
                return false;
            }
        }
        uint32_t num, den;
        fpsRatio(c.fps, num, den);
        char cmd[4096];
        // Quality-based (crf), the rate the size and frame rate suggest as a ceiling.
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -hide_banner -loglevel error -y -f rawvideo -pix_fmt nv12 -s %dx%d -r %u/%u -i - "
                 "-c:v libx264 -preset veryfast -crf 18 -maxrate %dk -bufsize %dk -pix_fmt yuv420p "
                 "-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv -movflags +faststart \"%s\"",
                 exe.c_str(), w_, h_, num, den, autoBitrateKbps(c) * 2, autoBitrateKbps(c) * 4, vpath_.c_str());
#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE rd = nullptr, wr = nullptr;
        if (!CreatePipe(&rd, &wr, &sa, 1 << 20))
        {
            err_ = "ffmpeg: CreatePipe failed";
            return false;
        }
        SetHandleInformation(wr, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{sizeof(si)};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = rd;
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        std::wstring wcmd(strlen(cmd) + 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd.data(), (int)wcmd.size());
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, wcmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                            nullptr, nullptr, &si, &pi))
        {
            CloseHandle(rd);
            CloseHandle(wr);
            err_ = "ffmpeg: CreateProcess failed";
            return false;
        }
        CloseHandle(rd);
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        pipe_ = wr;
#else
        pipe_ = popen(cmd, "w");
        if (!pipe_)
        {
            err_ = "ffmpeg: popen failed";
            return false;
        }
#endif
        return true;
    }
    bool write(const uint8_t *y, const uint8_t *uv, int64_t) override
    {
        return put(y, (size_t)w_ * h_) && put(uv, (size_t)w_ * h_ / 2);
    }
    bool writeAudio(const float *lr, uint32_t frames) override
    {
        if (!afile_ || frames == 0)
            return true;
        if (fwrite(lr, sizeof(float) * 2, frames, afile_) != frames)
        {
            err_ = "ffmpeg: could not write the sound side file";
            return false;
        }
        return true;
    }
    bool close() override
    {
        bool ok = closeVideo();
        if (afile_)
        {
            fclose(afile_);
            afile_ = nullptr;
            if (ok)
            {
                char cmd[4096];
                snprintf(cmd, sizeof(cmd),
                         "\"%s\" -hide_banner -loglevel error -y -i \"%s\" -f f32le -ar %d -ac 2 -i \"%s\" -map 0:v -map 1:a "
                         "-c:v copy -c:a aac -b:a %dk -movflags +faststart \"%s\"",
                         exe_.c_str(), vpath_.c_str(), audioRate_, apath_.c_str(), audioKbps_, path_.c_str());
                const int code = runCommand(cmd);
                if (code != 0)
                {
                    err_ = "ffmpeg (adding the sound) exited with code " + std::to_string(code) + "; the silent video is " +
                           vpath_;
                    ok = false;
                }
                else
                    removeUtf8(vpath_);
            }
            removeUtf8(apath_);
        }
        return ok;
    }
    bool closeVideo()
    {
        bool ok = true;
#ifdef _WIN32
        if (pipe_)
            CloseHandle(pipe_);
        pipe_ = nullptr;
        if (proc_)
        {
            WaitForSingleObject(proc_, 120000);
            DWORD code = 1;
            GetExitCodeProcess(proc_, &code);
            CloseHandle(proc_);
            proc_ = nullptr;
            if (code != 0)
            {
                err_ = "ffmpeg exited with code " + std::to_string(code);
                ok = false;
            }
        }
#else
        if (pipe_)
        {
            const int code = pclose(pipe_);
            pipe_ = nullptr;
            if (code != 0)
            {
                err_ = "ffmpeg exited with code " + std::to_string(code);
                ok = false;
            }
        }
#endif
        return ok;
    }
    ~FfmpegEncoder() override { close(); }

private:
    bool put(const uint8_t *p, size_t n)
    {
#ifdef _WIN32
        while (n > 0)
        {
            DWORD wrote = 0;
            if (!WriteFile(pipe_, p, (DWORD)std::min<size_t>(n, 1u << 24), &wrote, nullptr) || wrote == 0)
            {
                err_ = "ffmpeg: the pipe closed (did ffmpeg fail? see the app's stderr)";
                return false;
            }
            p += wrote;
            n -= wrote;
        }
        return true;
#else
        if (fwrite(p, 1, n, pipe_) != n)
        {
            err_ = "ffmpeg: the pipe closed";
            return false;
        }
        return true;
#endif
    }
    int w_ = 0, h_ = 0;
    int audioKbps_ = 192;
    std::string exe_, path_, vpath_, apath_;
    FILE *afile_ = nullptr;
#ifdef _WIN32
    HANDLE pipe_ = nullptr, proc_ = nullptr;
#else
    FILE *pipe_ = nullptr;
#endif
};

// ─── Media Foundation ─────────────────────────────────────────────────────────────────────────────
#ifdef _WIN32
template <class T> static void safeRelease(T *&p)
{
    if (p)
        p->Release();
    p = nullptr;
}

class MfEncoder : public VideoEncoder
{
public:
    // hw: let the sink writer pick a hardware encoder MFT. Runs on the worker thread (COM per thread).
    bool open(const VideoEncoderConfig &c, bool hw)
    {
        name_ = hw ? "mf-hw" : "mf";
        w_ = c.width;
        h_ = c.height;
        fps_ = c.fps;
        if (!comInit_)
        {
            const HRESULT ch = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            comInit_ = SUCCEEDED(ch);
            if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE)))
                return fail("MFStartup failed");
            mfInit_ = true;
        }
        std::wstring wpath(c.path.size() + 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, c.path.c_str(), -1, wpath.data(), (int)wpath.size());

        IMFAttributes *attr = nullptr;
        MFCreateAttributes(&attr, 4);
        attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
        attr->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
        attr->SetUINT32(MF_LOW_LATENCY, FALSE);
        HRESULT h = MFCreateSinkWriterFromURL(wpath.c_str(), nullptr, attr, &writer_);
        safeRelease(attr);
        if (FAILED(h))
            return fail("MFCreateSinkWriterFromURL", h);

        uint32_t num, den;
        fpsRatio(c.fps, num, den);
        IMFMediaType *out = nullptr, *in = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)autoBitrateKbps(c) * 1000u);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32)w_, (UINT32)h_);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
        out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        h = writer_->AddStream(out, &stream_);
        safeRelease(out);
        if (FAILED(h))
            return fail("AddStream (H.264)", h);

        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)w_, (UINT32)h_);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, num, den);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)w_);
        in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        h = writer_->SetInputMediaType(stream_, in, nullptr);
        safeRelease(in);
        if (FAILED(h))
            return fail("SetInputMediaType (NV12)", h);
        // Quality over a fixed rate where the encoder supports it (the software one does): fine noise and grain, the
        // things a debugging video is for, otherwise turn to mush at a constant rate.
        ICodecAPI *api = nullptr;
        if (SUCCEEDED(writer_->GetServiceForStream(stream_, GUID_NULL, IID_PPV_ARGS(&api))) && api)
        {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_UI4;
            v.ulVal = eAVEncCommonRateControlMode_Quality;
            if (SUCCEEDED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v)))
            {
                v.ulVal = 80;
                api->SetValue(&CODECAPI_AVEncCommonQuality, &v);
            }
            safeRelease(api);
        }
        // The sound: 16-bit PCM in, AAC-LC out (the Windows AAC encoder takes 44.1 / 48 kHz, 96..192 kbit/s stereo).
        if (c.audioRate > 0)
        {
            audioRate_ = c.audioRate;
            IMFMediaType *aout = nullptr, *ain = nullptr;
            MFCreateMediaType(&aout);
            aout->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            aout->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            aout->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            aout->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)audioRate_);
            aout->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            const int kbps = c.audioKbps >= 176 ? 192 : (c.audioKbps >= 144 ? 160 : (c.audioKbps >= 112 ? 128 : 96));
            aout->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)kbps * 125u);
            h = writer_->AddStream(aout, &astream_);
            safeRelease(aout);
            if (FAILED(h))
                return fail("AddStream (AAC)", h);
            MFCreateMediaType(&ain);
            ain->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            ain->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            ain->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            ain->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)audioRate_);
            ain->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            ain->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            ain->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)audioRate_ * 4u);
            h = writer_->SetInputMediaType(astream_, ain, nullptr);
            safeRelease(ain);
            if (FAILED(h))
                return fail("SetInputMediaType (PCM)", h);
        }
        h = writer_->BeginWriting();
        if (FAILED(h))
            return fail("BeginWriting", h);
        return true;
    }
    bool writeAudio(const float *lr, uint32_t frames) override
    {
        if (audioRate_ <= 0 || frames == 0)
            return true;
        const DWORD size = (DWORD)frames * 4;
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buf)))
            return fail("MFCreateMemoryBuffer (sound)");
        BYTE *p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        int16_t *d = reinterpret_cast<int16_t *>(p);
        for (size_t i = 0; i < (size_t)frames * 2; ++i)
            d[i] = (int16_t)std::lround(std::clamp(lr[i], -1.0f, 1.0f) * 32767.0f);
        buf->Unlock();
        buf->SetCurrentLength(size);
        IMFSample *s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        const LONGLONG t0 = (LONGLONG)(audioFrames_ * 10000000ll / audioRate_);
        audioFrames_ += frames;
        const LONGLONG t1 = (LONGLONG)(audioFrames_ * 10000000ll / audioRate_);
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        const HRESULT h = writer_->WriteSample(astream_, s);
        safeRelease(s);
        safeRelease(buf);
        if (FAILED(h))
            return fail("WriteSample (sound)", h);
        return true;
    }
    bool write(const uint8_t *y, const uint8_t *uv, int64_t index) override
    {
        const DWORD ySize = (DWORD)w_ * h_, size = ySize + ySize / 2;
        IMFMediaBuffer *buf = nullptr;
        if (FAILED(MFCreateMemoryBuffer(size, &buf)))
            return fail("MFCreateMemoryBuffer");
        BYTE *p = nullptr;
        buf->Lock(&p, nullptr, nullptr);
        memcpy(p, y, ySize);
        memcpy(p + ySize, uv, ySize / 2);
        buf->Unlock();
        buf->SetCurrentLength(size);
        IMFSample *s = nullptr;
        MFCreateSample(&s);
        s->AddBuffer(buf);
        const LONGLONG t0 = (LONGLONG)std::llround((double)index * 1e7 / fps_);
        const LONGLONG t1 = (LONGLONG)std::llround((double)(index + 1) * 1e7 / fps_);
        s->SetSampleTime(t0);
        s->SetSampleDuration(t1 - t0);
        const HRESULT h = writer_->WriteSample(stream_, s);
        safeRelease(s);
        safeRelease(buf);
        if (FAILED(h))
            return fail("WriteSample", h);
        ++written_;
        return true;
    }
    bool close() override
    {
        bool ok = true;
        if (writer_)
        {
            if (written_ > 0)
            {
                const HRESULT h = writer_->Finalize();
                if (FAILED(h))
                    ok = fail("Finalize", h);
            }
            safeRelease(writer_);
        }
        if (mfInit_)
            MFShutdown();
        mfInit_ = false;
        if (comInit_)
            CoUninitialize();
        comInit_ = false;
        return ok;
    }
    ~MfEncoder() override { close(); }

private:
    bool fail(const char *what, HRESULT h = E_FAIL)
    {
        char b[160];
        snprintf(b, sizeof(b), "Media Foundation: %s (0x%08lx)", what, (unsigned long)h);
        err_ = b;
        return false;
    }
    IMFSinkWriter *writer_ = nullptr;
    DWORD stream_ = 0, astream_ = 0;
    int64_t audioFrames_ = 0;
    int w_ = 0, h_ = 0;
    double fps_ = 60.0;
    int64_t written_ = 0;
    bool comInit_ = false, mfInit_ = false;
};
#endif

std::unique_ptr<VideoEncoder> openVideoEncoder(const VideoEncoderConfig &cfg, std::string &err)
{
    err.clear();
    if (cfg.width <= 0 || cfg.height <= 0 || (cfg.width & 1) || (cfg.height & 1))
    {
        err = "the video size must be positive and even";
        return nullptr;
    }
    const std::string be = cfg.backend.empty() ? "auto" : cfg.backend;
#ifdef _WIN32
    if (be == "auto" || be == "mf" || be == "mf-hw")
    {
        for (int hw = 1; hw >= 0; --hw)
        {
            auto e = std::make_unique<MfEncoder>();
            if (e->open(cfg, hw != 0))
                return e;
            err += (err.empty() ? "" : "; ") + e->error();
            e->close();
            DeleteFileA(cfg.path.c_str()); // a half-created file from the failed attempt
        }
        if (be != "auto")
            return nullptr;
    }
#endif
    if (be == "auto" || be == "ffmpeg")
    {
        const std::string exe = ffmpegPathFound();
        if (exe.empty())
        {
            err += (err.empty() ? "" : "; ") + std::string("no ffmpeg on PATH");
            return nullptr;
        }
        auto e = std::make_unique<FfmpegEncoder>();
        if (e->open(cfg, exe))
            return e;
        err += (err.empty() ? "" : "; ") + e->error();
        return nullptr;
    }
    err = "unknown encoder '" + be + "' (auto | mf | ffmpeg)";
    return nullptr;
}
