// test_topic.cpp - FrameTopic semantics: latest-frame-wins hand-off,
// producer/consumer concurrency, timeout behavior, and buffer recycling
// (the swapped-back producer buffer must keep working across publishes).
#include "../src/frame_topic.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using vftstream::FrameData;
using vftstream::FrameTopic;

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { std::printf("  [ ok ] %s\n", msg); }         \
        else      { std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
    } while (0)

void fillSeq(std::vector<uint8_t>& buf, uint64_t seq, size_t len) {
    buf.resize(len);
    for (size_t i = 0; i < len; ++i)
        buf[i] = (uint8_t)((seq * 131 + i * 7) & 0xff);
}
bool checkSeq(const uint8_t* p, size_t n, uint64_t seq, size_t len) {
    if (n != len) return false;
    for (size_t i = 0; i < len; ++i)
        if (p[i] != (uint8_t)((seq * 131 + i * 7) & 0xff)) return false;
    return true;
}

} // namespace

int main() {
    std::printf("== FrameTopic test ==\n");

    // -- single-threaded basics (fresh topic: seq == publish count) --
    {
        FrameTopic topic;
        std::vector<uint8_t> buf;
        fillSeq(buf, 1, 4096);
        topic.publish(buf, buf.size(), 1.0);
        CHECK(topic.published() == 1, "published() counts");
        // First-ever publish swaps with a fresh (empty) frame; the
        // producer's buffer is handed back, whatever it holds.

        auto f = topic.latest();
        CHECK(f && f->seq == 1 && f->tsMs == 1.0 &&
                  checkSeq(f->jpeg.data(), f->jpegLen, 1, 4096),
              "latest() returns the published frame intact");

        fillSeq(buf, 2, 4096);
        topic.publish(buf, buf.size(), 2.0);
        uint64_t seq = 0;
        auto f2 = topic.waitFor(1, 100, seq);
        CHECK(f2 && f2->seq == 2 && seq == 2 &&
                  checkSeq(f2->jpeg.data(), f2->jpegLen, 2, 4096),
              "waitFor(lastSeq) returns only newer frame");
        CHECK(topic.waitFor(seq, 50, seq) == nullptr,
              "waitFor times out with nothing newer");
        CHECK(seq == 2, "timeout leaves seq untouched");
    }

    // -- slow consumer skips (latest wins; fresh topic) --
    {
        FrameTopic topic;
        std::vector<uint8_t> buf;
        for (uint64_t i = 1; i <= 100; ++i) {
            fillSeq(buf, i, 2048);
            topic.publish(buf, buf.size(), (double)i);
        }
        uint64_t seq = 0;
        auto f = topic.waitFor(0, 100, seq);
        CHECK(f && f->seq == 100 && seq == 100 &&
                  checkSeq(f->jpeg.data(), f->jpegLen, 100, 2048),
              "slow consumer sees the newest frame, not a queue");
    }

    // -- concurrent producer + consumers --
    {
        FrameTopic t2;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> maxSeq{0};
        std::atomic<int> inconsistencies{0};

        std::thread producer([&] {
            std::vector<uint8_t> buf;
            for (uint64_t i = 1; i <= 20000; ++i) {
                fillSeq(buf, i, 1024);
                t2.publish(buf, buf.size(), (double)i);
                if (i % 97 == 0) std::this_thread::yield();
            }
            stop = true;
        });

        std::vector<std::thread> consumers;
        for (int c = 0; c < 4; ++c) {
            consumers.emplace_back([&] {
                uint64_t lastSeq = 0;
                while (!stop) {
                    uint64_t seq = 0;
                    auto f = t2.waitFor(lastSeq, 20, seq);
                    if (f) {
                        if (seq <= lastSeq ||
                            !checkSeq(f->jpeg.data(), f->jpegLen, f->seq, 1024))
                            ++inconsistencies;
                        if (seq > lastSeq) lastSeq = seq;
                        uint64_t m = maxSeq.load();
                        while (seq > m && !maxSeq.compare_exchange_weak(m, seq)) {}
                    }
                }
            });
        }

        producer.join();
        for (auto& th : consumers) th.join();
        CHECK(inconsistencies == 0, "20k frames x 4 consumers: data consistent");
        CHECK(maxSeq == 20000, "consumers observed the last frame");
    }

    // -- steady-state recycling (fresh topic) --
    {
        FrameTopic t3;
        std::vector<uint8_t> buf;
        size_t capAt50 = 0;
        for (uint64_t i = 1; i <= 300; ++i) {
            fillSeq(buf, i, 8192); // resizes whatever buffer came back
            t3.publish(buf, buf.size(), (double)i);
            if (i == 50) capAt50 = buf.capacity();
        }
        CHECK(capAt50 >= 8192,
              "producer buffer retains capacity (recycled, no per-frame alloc)");

        // A held frame must never be written behind its holder's back by
        // recycling/reuse of the same FrameData.
        auto held = t3.latest(); // frame 300 (8192 bytes)
        for (uint64_t i = 301; i <= 350; ++i) {
            fillSeq(buf, i, 4096);
            t3.publish(buf, buf.size(), (double)i);
            if (!checkSeq(held->jpeg.data(), held->jpegLen, 300, 8192)) {
                CHECK(false, "held frame corrupted by later publishes");
                break;
            }
        }
        CHECK(true, "held frame immune to recycling");
    }

    // -- shutdown() wakes every waitFor immediately --
    {
        FrameTopic t4;
        std::vector<uint8_t> buf;
        fillSeq(buf, 1, 512);
        t4.publish(buf, buf.size(), 1.0);

        std::atomic<int> woke{0};
        std::vector<std::thread> waiters;
        for (int w = 0; w < 3; ++w) {
            waiters.emplace_back([&] {
                uint64_t seq = 0;
                // Would block for the full 10 s without shutdown().
                auto f = t4.waitFor(1, 10000, seq);
                if (!f) ++woke; // shutdown(), not a frame
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto t0 = std::chrono::steady_clock::now();
        t4.shutdown();
        for (auto& th : waiters) th.join();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0)
                      .count();
        CHECK(woke == 3, "shutdown() woke all 3 waitFor callers");
        CHECK(ms < 1000, "shutdown() returned them promptly, not via timeout");
        uint64_t seq2 = 0;
        CHECK(t4.waitFor(0, 50, seq2) == nullptr,
              "waitFor after shutdown returns nullptr");
    }

    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
