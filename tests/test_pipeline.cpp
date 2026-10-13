// test_pipeline.cpp - golden test: production pipeline vs the reference
// implementation, for both sensor geometries and every blur/LUT option.
//
// PASS criteria:
//   - blur off (either LUT state): byte-identical to the reference
//     (the resize/fused-crop is the exact same integer math);
//   - blur on, LUT off: <= 2 LSB difference from the float reference
//     (kernel quantization to {18,63,94,63,18}/256), mean abs < 0.5 LSB;
//   - blur on, LUT on: composition must be EXACT (new output ==
//     lut[new blur output] - proves the LUT-folding-into-the-blur-store)
//     and the pre-LUT blur difference must satisfy the <= 2 LSB bound.
//     The composed outputs may legitimately differ a bit more where the
//     <=1-LSB blur difference crosses the LUT's black step (lut[0]=0,
//     lut[1]=12: the gamma stretch amplifies dark rounding differences
//     up to 12) - reported, not gated.
#include "../src/pipeline.hpp"
#include "reference_pipeline.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using vftstream::FramePipeline;
using vftstream_ref::ReferencePipeline;

namespace {

int g_failures = 0;

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ULL + 1442695040888963407ULL) {}
    uint32_t next() {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        return (uint32_t)(s >> 32);
    }
    uint8_t byte() { return (uint8_t)(next() & 0xff); }
};

// Synthetic YUYV frame: Y carries a pattern, U/V are garbage (the pipeline
// must ignore them).
enum Pattern { PAT_GRADIENT, PAT_NOISE, PAT_IMPULSE, PAT_SINUOID };
void makeFrame(std::vector<uint8_t>& yuyv, int w, int h, Pattern pat, Rng& rng) {
    yuyv.assign(static_cast<size_t>(w) * h * 2, 0);
    for (int y = 0; y < h; ++y) {
        uint8_t* row = yuyv.data() + static_cast<size_t>(y) * w * 2;
        for (int x = 0; x < w; ++x) {
            uint8_t* px = row + 2 * x;
            switch (pat) {
            case PAT_GRADIENT:
                px[0] = (uint8_t)((x * 255) / (w - 1));
                break;
            case PAT_NOISE:
                px[0] = rng.byte();
                break;
            case PAT_IMPULSE:
                px[0] = 0;
                if (y == h / 2 && x == w / 4) px[0] = 255;
                if (y == h / 8 && x == 3 * w / 8) px[0] = 200;
                break;
            case PAT_SINUOID:
                px[0] = (uint8_t)(128 + 127 * sin((x * 0.31) + (y * 0.17)));
                break;
            }
            px[1] = rng.byte(); // chroma garbage
        }
    }
}

struct Diff {
    int maxAbs = 0;
    double meanAbs = 0;
    size_t above1 = 0;
};

Diff diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
    Diff d;
    size_t n = a.size() < b.size() ? a.size() : b.size();
    double sum = 0;
    for (size_t i = 0; i < n; ++i) {
        int v = (int)a[i] - (int)b[i];
        int av = v < 0 ? -v : v;
        if (av > d.maxAbs) d.maxAbs = av;
        if (av > 1) ++d.above1;
        sum += av;
    }
    d.meanAbs = n ? sum / n : 0.0;
    return d;
}

void compareCase(const char* name, bool blur, bool lut,
                 const std::vector<uint8_t>& yuyv, int w, int h,
                 FramePipeline& pipe, ReferencePipeline& ref) {
    std::vector<uint8_t> got, want;
    pipe.setBlur(blur);
    pipe.setLut(lut);
    pipe.process(yuyv, w, h, got);
    ref.process(yuyv, w, h, want, blur, lut);

    if (got.size() != 160000 || want.size() != 160000) {
        std::printf("  [FAIL] %s: bad output size (%zu / %zu)\n",
                    name, got.size(), want.size());
        ++g_failures;
        return;
    }

    if (!blur) {
        // Pure integer paths: must be exact.
        Diff d = diff(got, want);
        if (d.maxAbs != 0) {
            std::printf("  [FAIL] %s: expected identical, maxAbs=%d\n", name, d.maxAbs);
            ++g_failures;
        } else {
            std::printf("  [ ok ] %s: identical to reference\n", name);
        }
        return;
    }

    if (!lut) {
        // Blur accuracy vs the float reference: the real quality bound.
        Diff d = diff(got, want);
        if (d.maxAbs > 2 || d.meanAbs >= 0.5 || d.above1 > 160000 / 1000) {
            std::printf("  [FAIL] %s: maxAbs=%d meanAbs=%.4f above1=%zu\n",
                        name, d.maxAbs, d.meanAbs, d.above1);
            ++g_failures;
        } else {
            std::printf("  [ ok ] %s: maxAbs=%d meanAbs=%.4f above1=%zu (<0.1%%)\n",
                        name, d.maxAbs, d.meanAbs, d.above1);
        }
        return;
    }

    // blur + lut: gate the composition (exact) and the pre-LUT blur
    // accuracy; report the composed diff (LUT black-step amplification).
    std::vector<uint8_t> gotPre, wantPre;
    pipe.setLut(false);
    pipe.process(yuyv, w, h, gotPre);
    ref.process(yuyv, w, h, wantPre, true, false);
    pipe.setLut(true);

    const uint8_t* lutT = pipe.lut().data();
    size_t compBad = 0;
    for (size_t i = 0; i < got.size(); ++i)
        if (got[i] != lutT[gotPre[i]]) ++compBad;

    Diff dPre = diff(gotPre, wantPre);
    Diff dComp = diff(got, want); // informational: lut[0]=0, lut[1]=12
    if (compBad != 0 || dPre.maxAbs > 2 || dPre.meanAbs >= 0.5 ||
        dPre.above1 > 160000 / 1000) {
        std::printf("  [FAIL] %s: compBad=%zu preLUT maxAbs=%d meanAbs=%.4f "
                    "above1=%zu (composed maxAbs=%d, above1=%zu)\n",
                    name, compBad, dPre.maxAbs, dPre.meanAbs, dPre.above1,
                    dComp.maxAbs, dComp.above1);
        ++g_failures;
    } else {
        std::printf("  [ ok ] %s: composition exact; preLUT maxAbs=%d "
                    "meanAbs=%.4f; composed maxAbs=%d above1=%zu (LUT "
                    "black-step only)\n",
                    name, dPre.maxAbs, dPre.meanAbs,
                    dComp.maxAbs, dComp.above1);
    }
}

} // namespace

int main() {
    std::printf("== pipeline golden test ==\n");
    FramePipeline pipe;
    ReferencePipeline ref;

    // LUT tables must be identical (same construction).
    if (std::memcmp(pipe.lut().data(), ref.lut().data(), 256) != 0) {
        std::printf("  [FAIL] LUT tables differ\n");
        ++g_failures;
    } else {
        std::printf("  [ ok ] LUT tables identical\n");
    }

    const struct { int w, h; const char* name; } geos[] = {
        {400, 400, "400x400 classic (320000B)"},
        {640, 480, "640x480 test-bar (614400B)"},
    };
    const struct { Pattern pat; const char* name; } pats[] = {
        {PAT_GRADIENT, "gradient"},
        {PAT_NOISE, "noise"},
        {PAT_IMPULSE, "impulse"},
        {PAT_SINUOID, "sinusoid"},
    };
    const struct { bool blur, lut; const char* name; } opts[] = {
        {false, false, "blur=off lut=off"},
        {false, true, "blur=off lut=on"},
        {true, false, "blur=on lut=off"},
        {true, true, "blur=on lut=on (default)"},
    };

    Rng rng(0xC0FFEE);
    for (const auto& g : geos) {
        for (const auto& p : pats) {
            std::vector<uint8_t> yuyv;
            makeFrame(yuyv, g.w, g.h, p.pat, rng);
            for (const auto& o : opts) {
                char name[96];
                std::snprintf(name, sizeof(name), "%s / %s / %s",
                              g.name, p.name, o.name);
                compareCase(name, o.blur, o.lut, yuyv, g.w, g.h, pipe, ref);
            }
        }
    }

    // Deterministic across repeated calls (map cache reuse).
    {
        std::vector<uint8_t> yuyv, a, b;
        makeFrame(yuyv, 400, 400, PAT_NOISE, rng);
        pipe.setBlur(true); pipe.setLut(true);
        pipe.process(yuyv, 400, 400, a);
        pipe.process(yuyv, 400, 400, b);
        if (std::memcmp(a.data(), b.data(), a.size()) != 0) {
            std::printf("  [FAIL] repeated process() differs\n");
            ++g_failures;
        } else {
            std::printf("  [ ok ] repeated process() deterministic\n");
        }
        // Geometry switch and switch back (map cache rebuild).
        std::vector<uint8_t> yuyv2, c, d2;
        makeFrame(yuyv2, 640, 480, PAT_NOISE, rng);
        pipe.process(yuyv2, 640, 480, c);
        pipe.process(yuyv, 400, 400, d2);
        if (std::memcmp(a.data(), d2.data(), a.size()) != 0) {
            std::printf("  [FAIL] geometry switch-back differs\n");
            ++g_failures;
        } else {
            std::printf("  [ ok ] geometry switch-back deterministic\n");
        }
    }

    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
