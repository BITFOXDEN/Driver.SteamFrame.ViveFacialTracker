// bench_pipeline.cpp - production pipeline vs the reference implementation.
// Measures the effect of the fused crop, cached maps, integer blur and
// folded LUT.
#include "../src/pipeline.hpp"
#include "reference_pipeline.hpp"

#include <chrono>
#include <cstdio>
#include <vector>

using vftstream::FramePipeline;
using vftstream_ref::ReferencePipeline;

namespace {

struct Rng {
    uint64_t s = 42;
    uint8_t byte() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint8_t)(s >> 32);
    }
};

double bench(bool ref, int w, int h, int iters) {
    std::vector<uint8_t> yuyv((size_t)w * h * 2);
    Rng rng;
    for (auto& b : yuyv) b = rng.byte();

    FramePipeline pipe;
    ReferencePipeline reference;
    std::vector<uint8_t> out;

    // warm-up (allocations, maps)
    if (ref) reference.process(yuyv, w, h, out, true, true);
    else     pipe.process(yuyv, w, h, out);

    auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        if (ref) reference.process(yuyv, w, h, out, true, true);
        else     pipe.process(yuyv, w, h, out);
    }
    auto t1 = std::chrono::steady_clock::now();
    volatile uint8_t sink = out[0]; (void)sink;
    return std::chrono::duration<double, std::micro>(t1 - t0).count() / iters;
}

} // namespace

int main() {
    std::printf("== pipeline benchmark (us/frame, lower is better) ==\n");
    const int iters = 2000;
    double ref400 = bench(true, 400, 400, iters);
    double new400 = bench(false, 400, 400, iters);
    double ref640 = bench(true, 640, 480, iters / 2);
    double new640 = bench(false, 640, 480, iters / 2);
    std::printf("  400x400 -> 400x400: ref %7.1f us  cur %7.1f us  (%.2fx)\n",
                ref400, new400, ref400 / new400);
    std::printf("  640x480 -> 400x400: ref %7.1f us  cur %7.1f us  (%.2fx)\n",
                ref640, new640, ref640 / new640);
    return 0;
}
