#pragma once
// ── Video recorder ────────────────────────────────────────────────────────────────────────────────
// Frames -> FrameCapture (asynchronous GPU readback) -> a worker thread (NV12 conversion + VideoEncoder) -> .mp4.
// The frame thread never waits: a frame with no free readback slot is DROPPED and its time given to the next
// captured frame (repeated), so the video keeps the clock it was recorded on. Lossless mode (the harness's
// path play) instead asks canCapture() first and holds the path back until a slot is free.
//
// Frame-thread calls, in a frame's order:
//   tick(dt)            once per frame with the frame's dt (the sim clock: fixed in a harness run, wall time live)
//   wantsCapture()      whether this frame should be copied
//   recordCopy(...)     records the copy (the image in TRANSFER_SRC_OPTIMAL)
//   poll(ctx)           after the frame fence: hands completed copies to the worker; finishes a stop
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "FrameCapture.h"
#include "VideoEncoder.h"

struct VulkanContext;

struct VideoRecordOptions
{
    std::string path;          // .mp4
    double fps = 60.0;
    float scale = 1.0f;        // of the window size (the output is rounded down to even)
    bool ui = false;           // include the UI (else the frame before the UI is drawn)
    bool lossless = false;     // every frame is one video frame; never drop (the caller holds back)
    int bitrateKbps = 0;       // 0 = auto
    std::string encoder = "auto";
    int slots = 6;             // readback ring
};

class VideoRecorder
{
public:
    enum class State { Idle, Recording, Stopping, Finishing };

    ~VideoRecorder();
    bool start(VulkanContext &ctx, uint32_t srcW, uint32_t srcH, const VideoRecordOptions &o, std::string &err);
    void stop();                       // no more captures; the file is closed in the background
    State state() const { return state_; }
    bool recording() const { return state_ == State::Recording; }
    bool busy() const { return state_ != State::Idle; }
    const VideoRecordOptions &options() const { return opt_; }

    void tick(double dt);
    void requestFrame() { requested_ = true; } // lossless mode: capture this frame (path play)
    bool wantsCapture() const { return state_ == State::Recording && due_ > 0; }
    bool canCapture() const { return state_ == State::Recording && cap_.freeSlots() > 0; }
    // Records the copy of `image` (TRANSFER_SRC_OPTIMAL). Returns false when the frame was dropped.
    bool recordCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image, VkExtent2D ext, VkFormat fmt);
    // After the fence. Returns true on the frame the recording finished (then lastResult() holds its stats).
    bool poll(VulkanContext &ctx);
    void shutdown(VulkanContext &ctx); // app exit: waits for the worker

    // The finished recording's numbers (file, frames, drops, encode cost, size), or the running ones.
    nlohmann::json stats() const;
    const nlohmann::json &lastResult() const { return last_; }
    double secondsRecorded() const { return (double)framesOut_ / opt_.fps; }
    const std::string &error() const { return err_; }

private:
    struct Item
    {
        int slot;
        int repeat;
    };
    void worker();

    VideoRecordOptions opt_;
    FrameCapture cap_;
    State state_ = State::Idle;
    double clock_ = 0.0;      // seconds of video time owed
    int due_ = 0;             // video frames this frame stands for
    bool requested_ = false;  // lossless: requestFrame() this frame
    int carry_ = 0;           // frames dropped since the last capture (the next one repeats for them)
    int64_t framesIn_ = 0;    // video frames handed to the worker (repeats included)
    std::atomic<int64_t> framesOut_{0};
    int64_t captures_ = 0, dropped_ = 0, repeated_ = 0, framesRendered_ = 0, missed_ = 0;
    double startWall_ = 0.0, stopWall_ = 0.0;
    uint32_t outW_ = 0, outH_ = 0;
    std::string err_;
    nlohmann::json last_;

    std::thread thread_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool endOfStream_ = false;
    std::atomic<bool> workerDone_{false};
    std::string workerErr_, encoderName_;
    double convertMs_ = 0.0, encodeMs_ = 0.0, maxEncodeMs_ = 0.0; // worker sums (under mx_ when read)
    int64_t workerFrames_ = 0;
};
