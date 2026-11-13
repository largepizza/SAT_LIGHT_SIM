// Video recorder (see VideoRecorder.h).
#include "VideoRecorder.h"

#include "VulkanContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <vector>

namespace fs = std::filesystem;

static double wallNow()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

VideoRecorder::~VideoRecorder()
{
    if (thread_.joinable())
    {
        {
            std::lock_guard<std::mutex> lk(mx_);
            endOfStream_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }
}

bool VideoRecorder::start(VulkanContext &ctx, uint32_t srcW, uint32_t srcH, const VideoRecordOptions &o, std::string &err)
{
    err.clear();
    if (state_ != State::Idle)
    {
        err = "a recording is already running";
        return false;
    }
    if (!ctx.screenshotSupported)
    {
        err = "video needs swapchain TRANSFER_SRC (not supported by this GPU/driver)";
        return false;
    }
    const double t0 = wallNow();
    opt_ = o;
    opt_.fps = std::clamp(opt_.fps, 1.0, 240.0);
    opt_.scale = std::clamp(opt_.scale, 0.1f, 1.0f);
    const uint32_t w = std::max(2u, (uint32_t)std::floor(srcW * opt_.scale)) & ~1u;
    const uint32_t h = std::max(2u, (uint32_t)std::floor(srcH * opt_.scale)) & ~1u;
    if (!cap_.create(ctx, w, h, std::max(3, opt_.slots), err))
        return false;
    outW_ = w;
    outH_ = h;
    std::error_code ec;
    fs::create_directories(fs::path(opt_.path).parent_path(), ec);

    clock_ = 0.0;
    due_ = carry_ = 0;
    requested_ = false;
    framesIn_ = captures_ = dropped_ = repeated_ = framesRendered_ = missed_ = 0;
    framesOut_ = 0;
    convertMs_ = encodeMs_ = maxEncodeMs_ = 0.0;
    workerFrames_ = 0;
    queue_.clear();
    endOfStream_ = false;
    workerDone_ = false;
    workerErr_.clear();
    encoderName_.clear();
    err_.clear();
    last_ = nlohmann::json();

    // The encoder is opened on the worker (Media Foundation wants COM on the thread that uses it); start() waits for
    // that one open (~0.1 s), never afterwards.
    auto opened = std::make_shared<std::promise<std::string>>();
    std::future<std::string> openRes = opened->get_future();
    VideoEncoderConfig cfg;
    cfg.path = opt_.path;
    cfg.width = (int)w;
    cfg.height = (int)h;
    cfg.fps = opt_.fps;
    cfg.bitrateKbps = opt_.bitrateKbps;
    cfg.backend = opt_.encoder;
    audioOn_ = opt_.audioRate > 0 && (bool)opt_.audioPull;
    cfg.audioRate = audioOn_ ? opt_.audioRate : 0;
    audioQ_.clear();
    audioPumped_ = audioIn_ = skipOwed_ = 0;
    thread_ = std::thread([this, cfg, opened]()
    {
        std::string e;
        std::unique_ptr<VideoEncoder> enc = openVideoEncoder(cfg, e);
        if (enc)
            encoderName_ = enc->name();
        opened->set_value(enc ? std::string() : e);
        if (!enc)
        {
            workerDone_ = true;
            return;
        }
        std::vector<uint8_t> y((size_t)cfg.width * cfg.height), uv((size_t)cfg.width * cfg.height / 2);
        int64_t index = 0, audioOut = 0;
        bool failed = false;
        std::deque<std::vector<float>> sound;
        for (;;)
        {
            Item it{};
            bool haveItem = false;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [&] { return !queue_.empty() || !audioQ_.empty() || endOfStream_; });
                sound.swap(audioQ_);
                if (!queue_.empty())
                {
                    it = queue_.front();
                    queue_.pop_front();
                    haveItem = true;
                }
                else if (sound.empty())
                    break; // end of stream, all written
            }
            for (const std::vector<float> &ch : sound)
            {
                if (!failed && !enc->writeAudio(ch.data(), (uint32_t)(ch.size() / 2)))
                    failed = true;
                audioOut += (int64_t)(ch.size() / 2);
            }
            sound.clear();
            if (failed)
            {
                std::lock_guard<std::mutex> lk(mx_);
                if (workerErr_.empty())
                    workerErr_ = enc->error();
            }
            if (!haveItem)
                continue;
            index += it.skip;
            const double t0 = wallNow();
            const FrameCapture::Slot &s = cap_.slot(it.slot);
            if (!failed)
                convertToNV12(s.map, (size_t)cap_.width() * 4, cap_.bgra(), cfg.width, cfg.height, y.data(), uv.data());
            cap_.release(it.slot);
            const double t1 = wallNow();
            for (int r = 0; r < it.repeat && !failed; ++r)
                if (!enc->write(y.data(), uv.data(), index++))
                    failed = true;
            const double t2 = wallNow();
            std::lock_guard<std::mutex> lk(mx_);
            if (failed && workerErr_.empty())
                workerErr_ = enc->error();
            if (!failed)
                framesOut_ = index;
            convertMs_ += (t1 - t0) * 1000.0;
            encodeMs_ += (t2 - t1) * 1000.0;
            maxEncodeMs_ = std::max(maxEncodeMs_, (t2 - t1) * 1000.0);
            ++workerFrames_;
        }
        // The sound track ends with the video: pad a short one with silence (a long one is cut by the players).
        if (!failed && enc->hasAudio())
        {
            const int64_t want = (int64_t)std::llround((double)index / cfg.fps * cfg.audioRate);
            std::vector<float> zeros(4096 * 2, 0.0f);
            while (audioOut < want && !failed)
            {
                const uint32_t n = (uint32_t)std::min<int64_t>(4096, want - audioOut);
                failed = !enc->writeAudio(zeros.data(), n);
                audioOut += n;
            }
        }
        if (!enc->close())
        {
            std::lock_guard<std::mutex> lk(mx_);
            if (workerErr_.empty())
                workerErr_ = enc->error();
        }
        workerDone_ = true;
    });
    const std::string openErr = openRes.get();
    if (!openErr.empty())
    {
        thread_.join();
        cap_.destroy(ctx);
        err = openErr;
        return false;
    }
    state_ = State::Recording;
    startWall_ = wallNow();
    stallS_ = opt_.wallClock ? startWall_ - t0 : 0.0;
    return true;
}

void VideoRecorder::tick(double dt)
{
    if (state_ == State::Recording && due_ > 0 && !opt_.lossless)
    {
        // Last frame owed video frames but recorded no copy (no copy hook ran: a photo frame, a skipped present):
        // the next capture repeats for them.
        carry_ += due_;
        dropped_ += due_;
        ++missed_;
    }
    due_ = 0;
    if (state_ != State::Recording)
        return;
    ++framesRendered_;
    if (opt_.lossless)
    {
        due_ = requested_ ? 1 : 0;
        requested_ = false;
        return;
    }
    // This frame is drawn at clock_ = the time since the first recorded frame: its own dt is added before it is
    // placed; the first frame's dt (from before recording began) never is. start() runs in buildUI, before this
    // frame's tick, so its own stall (~0.1 s: the readback ring, the encoder open) lands in the SECOND frame's dt, and
    // on a wall clock is taken out of it. (Before 2026-11-12 each dt was added after placing its frame: frame 0 held
    // for the stall and the whole video ran ~0.1 s behind the wall clock, a constant lead of the sound track.)
    if (framesRendered_ > 1)
    {
        clock_ += std::max(0.0, dt - stallS_);
        stallS_ = 0.0;
    }
    // Video frame k shows [k / fps, (k + 1) / fps): this frame owes every frame up to its own.
    const int64_t target = (int64_t)std::floor(clock_ * opt_.fps + 1e-6) + 1;
    due_ = (int)std::max<int64_t>(0, target - (framesIn_ + carry_));
}

bool VideoRecorder::recordCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image, VkExtent2D ext, VkFormat fmt)
{
    if (!wantsCapture())
        return false;
    int repeat = due_ + carry_;
    // A long stall (the encoder minutes behind) is not replayed frame by frame: at most 2 s of repeats.
    const int maxRepeat = (int)std::ceil(opt_.fps * 2.0);
    if (repeat > maxRepeat)
    {
        dropped_ += repeat - maxRepeat;
        framesIn_ += repeat - maxRepeat; // the timeline moves on without them
        if (audioOn_)
            skipOwed_ += repeat - maxRepeat; // ...and with sound the encoder leaves the gap, so the two stay in step
        repeat = maxRepeat;
    }
    const int slot = cap_.record(cmd, ctx, image, ext, fmt, repeat);
    if (slot < 0)
    {
        if (!opt_.lossless)
        {
            carry_ += due_;
            dropped_ += due_;
        }
        due_ = 0;
        return false;
    }
    repeated_ += repeat - 1;
    framesIn_ += repeat;
    carry_ = 0;
    due_ = 0;
    ++captures_;
    return true;
}

bool VideoRecorder::poll(VulkanContext &ctx)
{
    if (state_ == State::Idle)
        return false;
    int gpu = 0;
    {
        std::vector<Item> got;
        cap_.collect(ctx, [&](int slot, const FrameCapture::Slot &s) { got.push_back({slot, s.tag}); });
        if (!got.empty() && skipOwed_ > 0)
        {
            got.front().skip = skipOwed_;
            skipOwed_ = 0;
        }
        if (state_ == State::Recording)
            pullAudio();
        if (!got.empty())
        {
            {
                std::lock_guard<std::mutex> lk(mx_);
                for (const Item &i : got)
                    queue_.push_back(i);
            }
            cv_.notify_one();
        }
        gpu = cap_.countState(FrameCapture::kGpu); // copies still on the GPU
    }
    if (state_ == State::Stopping && gpu == 0)
    {
        {
            std::lock_guard<std::mutex> lk(mx_);
            endOfStream_ = true;
        }
        cv_.notify_all();
        state_ = State::Finishing;
    }
    if (state_ == State::Finishing && workerDone_.load())
    {
        thread_.join();
        cap_.destroy(ctx);
        last_ = stats();
        state_ = State::Idle;
        return true;
    }
    return false;
}

void VideoRecorder::pullAudio()
{
    if (!audioOn_)
        return;
    if (opt_.audioPump)
    {
        const int64_t owed = (int64_t)std::llround((double)framesIn_ / opt_.fps * opt_.audioRate) - audioPumped_;
        if (owed > 0)
        {
            opt_.audioPump((uint32_t)std::min<int64_t>(owed, (int64_t)opt_.audioRate * 10));
            audioPumped_ += owed;
        }
    }
    std::vector<float> chunk;
    for (;;)
    {
        const size_t old = chunk.size();
        chunk.resize(old + 4096 * 2);
        const uint32_t n = opt_.audioPull(chunk.data() + old, 4096);
        chunk.resize(old + (size_t)n * 2);
        if (n < 4096)
            break;
    }
    if (chunk.empty())
        return;
    audioIn_ += (int64_t)(chunk.size() / 2);
    {
        std::lock_guard<std::mutex> lk(mx_);
        audioQ_.push_back(std::move(chunk));
    }
    cv_.notify_one();
}

void VideoRecorder::stop()
{
    if (state_ != State::Recording)
        return;
    pullAudio(); // the sound up to now (the capture is stopped by its owner after this)
    state_ = State::Stopping;
    stopWall_ = wallNow();
    due_ = 0;
}

void VideoRecorder::shutdown(VulkanContext &ctx)
{
    if (state_ == State::Idle)
        return;
    stop();
    ctx.drainFrames();
    // Everything recorded is complete now: hand it over, end the stream, wait for the file.
    for (int guard = 0; guard < 100000 && state_ != State::Idle; ++guard)
    {
        if (state_ == State::Finishing && !workerDone_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // completedFrame() lags frameIndex; after drainFrames every recorded copy is finished, so force-collect.
        if (state_ == State::Stopping)
        {
            std::vector<Item> got;
            cap_.collectAll([&](int slot, const FrameCapture::Slot &s) { got.push_back({slot, s.tag}); });
            std::lock_guard<std::mutex> lk(mx_);
            for (const Item &it : got)
                queue_.push_back(it);
        }
        poll(ctx);
    }
}

nlohmann::json VideoRecorder::stats() const
{
    nlohmann::json j;
    j["file"] = opt_.path;
    j["encoder"] = encoderName_;
    j["width"] = outW_;
    j["height"] = outH_;
    j["fps"] = opt_.fps;
    j["ui"] = opt_.ui;
    j["lossless"] = opt_.lossless;
    j["audio_rate"] = audioOn_ ? opt_.audioRate : 0;
    j["audio_captured_s"] = audioOn_ && opt_.audioRate > 0 ? (double)audioIn_ / opt_.audioRate : 0.0;
    j["frames"] = framesOut_.load();               // video frames written
    j["duration_s"] = (double)framesOut_.load() / opt_.fps;
    j["captures"] = captures_;                     // distinct frames copied
    j["dropped"] = dropped_;                       // video frames with no copy of their own (filled by repeats)
    j["repeated"] = repeated_;                     // extra copies of a frame written for the frames it stood for
    j["rendered_frames"] = framesRendered_;
    j["missed_copies"] = missed_;                 // frames that owed video but recorded no copy (counted in dropped)
    const double endWall = state_ == State::Recording ? wallNow() : stopWall_;
    j["wall_s"] = endWall - startWall_;
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex &>(mx_));
        j["convert_ms_mean"] = workerFrames_ ? convertMs_ / (double)workerFrames_ : 0.0;
        j["encode_ms_mean"] = workerFrames_ ? encodeMs_ / (double)workerFrames_ : 0.0; // per captured frame (repeats included)
        j["encode_ms_max"] = maxEncodeMs_;
        if (!workerErr_.empty())
            j["error"] = workerErr_;
    }
    std::error_code ec;
    const auto sz = fs::file_size(fs::path(opt_.path), ec);
    if (!ec)
    {
        j["bytes"] = (uint64_t)sz;
        const double dur = (double)framesOut_.load() / opt_.fps;
        if (dur > 0.0)
            j["mb_per_min"] = (double)sz / 1.0e6 / dur * 60.0;
    }
    return j;
}
