// frame_topic.hpp - "latest frame wins" mailbox shared by capture/encode/HTTP.
//
// Subscribers poll by sequence number, so a slow consumer simply skips
// frames - exactly what live streaming wants (never build a backlog).
//
// publish() is zero-copy in the steady state: it swaps the producer's JPEG
// buffer into the topic and hands a retired buffer back (so the encoder's
// next resize() is a no-op), and recycles the previous frame's FrameData
// only when no client still holds a reference. References are only ever
// taken under the topic mutex, so a buffer obtained via latest()/waitFor()
// can never be recycled or written behind its holder's back.
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace vftstream {

// A single JPEG frame shared between producer and all subscribers.
// jpeg is capacity-backed storage (the encoder never resizes it, see
// JpegEncoder::encodeGray): the valid bytes are jpeg.data()[0 .. jpegLen).
struct FrameData {
    std::vector<uint8_t> jpeg;
    size_t jpegLen = 0;
    uint64_t seq = 0;
    double tsMs = 0.0; // capture timestamp in ms since epoch
};

class FrameTopic {
public:
    // Publishes the first `len` bytes of `jpegInOut`, swapping a retired
    // buffer back into it for the producer's reuse.
    void publish(std::vector<uint8_t>& jpegInOut, size_t len, double tsMs) {
        std::lock_guard<std::mutex> lk(mu_);
        std::shared_ptr<FrameData> f;
        if (retired_.empty()) {
            f = std::make_shared<FrameData>();
        } else {
            f = std::move(retired_.back());
            retired_.pop_back();
        }
        f->jpeg.swap(jpegInOut);
        f->jpegLen = len;
        f->tsMs = tsMs;
        f->seq = ++seq_;
        // Recycle the previous frame only when no client still holds it;
        // otherwise it is freed normally when the last reference drops.
        if (latest_ && latest_.use_count() == 1)
            retired_.push_back(std::move(latest_));
        while (retired_.size() > 4) retired_.pop_back(); // cap the pool
        latest_ = std::move(f);
        cv_.notify_all();
    }

    // Blocks until a frame with seq > lastSeq is available (or timeout).
    std::shared_ptr<const FrameData> waitFor(uint64_t lastSeq, int timeoutMs,
                                             uint64_t& newSeqOut) const {
        std::unique_lock<std::mutex> lk(mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                     [&] { return closed_ || (latest_ && latest_->seq > lastSeq); });
        if (closed_) return nullptr; // topic shut down: caller should exit
        if (!latest_ || latest_->seq <= lastSeq) return nullptr;
        newSeqOut = latest_->seq;
        return latest_;
    }

    std::shared_ptr<const FrameData> latest() const {
        std::lock_guard<std::mutex> lk(mu_);
        return latest_;
    }

    uint64_t published() const {
        std::lock_guard<std::mutex> lk(mu_);
        return seq_;
    }

    // Wakes every waitFor immediately with nullptr. Daemon shutdown calls
    // this before joining threads that can be blocked in waitFor (the
    // /stream senders) - without it they sit out their full stall timeout
    // (12 s in the daemon) while the process tears down around them.
    void shutdown() {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    // Held non-const internally so retired frames can be recycled;
    // latest()/waitFor() hand out shared_ptr<const FrameData> copies.
    std::shared_ptr<FrameData> latest_;
    std::vector<std::shared_ptr<FrameData>> retired_; // pool, use_count==1
    uint64_t seq_ = 0;
    bool closed_ = false; // guarded by mu_; waitFor returns nullptr when set
};

} // namespace vftstream
