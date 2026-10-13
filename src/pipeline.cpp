// pipeline.cpp - see pipeline.hpp
#include "pipeline.hpp"

#include <algorithm>
#include <cmath>

namespace vftstream {

FramePipeline::FramePipeline() {
    lut_.resize(256);
    for (int i = 0; i < 256; ++i) {
        double v = std::pow(i / 2048.0, 1.0 / 2.5) * 255.0;
        lut_[i] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
    }
}

void FramePipeline::process(const std::vector<uint8_t>& yuyv,
                            int width, int height,
                            std::vector<uint8_t>& grayOut) {
    const bool classic = (width == 400);
    const int cropW = classic ? 200 : width / 2;
    const int cropH = height;

    small_.resize(static_cast<size_t>(kOutW) * kOutH);
    buildMaps(cropW, cropH);
    resizeYUYV(yuyv.data(), static_cast<size_t>(width) * 2, small_.data());

    if (blurOn_) {
        blur5x5(small_, blurTmp_, lutOn_ ? lut_.data() : nullptr);
    } else if (lutOn_) {
        const uint8_t* lut = lut_.data();
        uint8_t* d = small_.data();
        const size_t n = small_.size();
        for (size_t i = 0; i < n; ++i)
            d[i] = lut[d[i]];
    }

    grayOut.swap(small_);
}

// Sampling maps, fixed-point 16.16, cached per geometry (which only changes
// on reopen).
void FramePipeline::buildMaps(int sw, int sh) {
    if (sw == mapW_ && sh == mapH_) return;
    mapW_ = sw;
    mapH_ = sh;

    const int64_t fx = (static_cast<int64_t>(sw) << 16) / kOutW;
    const int64_t fy = (static_cast<int64_t>(sh) << 16) / kOutH;

    xSx_.resize(kOutW); xSx1_.resize(kOutW); xFrac_.resize(kOutW);
    for (int dx = 0; dx < kOutW; ++dx) {
        const int64_t sxr = dx * fx;
        int32_t sx = std::clamp(static_cast<int32_t>(sxr >> 16), 0, sw - 1);
        xSx_[dx] = sx;
        xSx1_[dx] = std::min(sx + 1, sw - 1);
        xFrac_[dx] = std::clamp(static_cast<int32_t>(sxr & 0xFFFF), 0, 65535);
    }
    ySy_.resize(kOutH); ySy1_.resize(kOutH); yFrac_.resize(kOutH);
    for (int dy = 0; dy < kOutH; ++dy) {
        const int64_t syr = dy * fy;
        int32_t sy = std::clamp(static_cast<int32_t>(syr >> 16), 0, sh - 1);
        ySy_[dy] = sy;
        ySy1_[dy] = std::min(sy + 1, sh - 1);
        yFrac_[dy] = std::clamp(static_cast<int32_t>(syr & 0xFFFF), 0, 65535);
    }
}

void FramePipeline::resizeYUYV(const uint8_t* src, size_t srcRowBytes,
                               uint8_t* dst) {
    for (int dy = 0; dy < kOutH; ++dy) {
        const uint8_t* r0 = src + static_cast<size_t>(ySy_[dy]) * srcRowBytes;
        const uint8_t* r1 = src + static_cast<size_t>(ySy1_[dy]) * srcRowBytes;
        const int32_t fyF = yFrac_[dy];
        uint8_t* drow = dst + static_cast<size_t>(dy) * kOutW;

        for (int dx = 0; dx < kOutW; ++dx) {
            // Byte offset of the Y sample inside the pixel pair.
            const int32_t sx = 2 * xSx_[dx];
            const int32_t sx1 = 2 * xSx1_[dx];
            const int32_t fxF = xFrac_[dx];

            const int32_t p00 = r0[sx];
            const int32_t p10 = r0[sx1];
            const int32_t p01 = r1[sx];
            const int32_t p11 = r1[sx1];

            const int32_t top = p00 + (((p10 - p00) * fxF) >> 16);
            const int32_t bot = p01 + (((p11 - p01) * fxF) >> 16);
            drow[dx] = static_cast<uint8_t>(top + (((bot - top) * fyF) >> 16));
        }
    }
}

// Gaussian 5x5, sigma ~= 1.1 (matches OpenCV GaussianBlur((5,5),0) closely
// enough for tracking).
//
// 1D kernel scaled by 256: float {0.0708, 0.2444, 0.3697, ...} ->
// {18, 63, 94, 63, 18}; the taps sum to exactly 256, so there is no
// gain/bias vs the float kernel (tap quantization alone, bounded to a
// ~1 LSB difference, checked by the golden test). All arithmetic is exact
// integer: the horizontal dot fits u16 (<= 256*255, an 8.8 fixed-point
// value), the vertical dot fits u32 and its >> 16 store recovers the 8-bit
// result.
void FramePipeline::blur5x5(std::vector<uint8_t>& buf, std::vector<uint16_t>& tmp,
                            const uint8_t* lut) {
    // Identity table so the folded LUT store needs no per-pixel branch.
    static const uint8_t kIdentity[256] = {
        0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,
        16,  17,  18,  19,  20,  21,  22,  23,  24,  25,  26,  27,  28,  29,  30,  31,
        32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  47,
        48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,
        64,  65,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,  77,  78,  79,
        80,  81,  82,  83,  84,  85,  86,  87,  88,  89,  90,  91,  92,  93,  94,  95,
        96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111,
        112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127,
        128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143,
        144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154, 155, 156, 157, 158, 159,
        160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
        176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191,
        192, 193, 194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204, 205, 206, 207,
        208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219, 220, 221, 222, 223,
        224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239,
        240, 241, 242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255,
    };
    const uint8_t* const lut8 = lut ? lut : kIdentity;
    constexpr uint16_t k0 = 18, k1 = 63, k2 = 94, k3 = 63, k4 = 18;

    const int w = kOutW, h = kOutH;
    tmp.resize(static_cast<size_t>(w) * h); // every element written below

    // ---- horizontal pass: u8 -> u16 ----
    for (int y = 0; y < h; ++y) {
        const uint8_t* s = buf.data() + static_cast<size_t>(y) * w;
        uint16_t* t = tmp.data() + static_cast<size_t>(y) * w;
        int x = 0;
        for (; x < 2; ++x) { // left edge: clamped taps
            t[x] = static_cast<uint16_t>(
                k0 * s[std::clamp(x - 2, 0, w - 1)] +
                k1 * s[std::clamp(x - 1, 0, w - 1)] +
                k2 * s[x] +
                k3 * s[std::clamp(x + 1, 0, w - 1)] +
                k4 * s[std::clamp(x + 2, 0, w - 1)]);
        }
        for (; x <= w - 3; ++x) { // interior: branchless, vectorizable
            t[x] = static_cast<uint16_t>(
                k0 * s[x - 2] + k1 * s[x - 1] + k2 * s[x] +
                k3 * s[x + 1] + k4 * s[x + 2]);
        }
        for (; x < w; ++x) { // right edge: clamped taps
            t[x] = static_cast<uint16_t>(
                k0 * s[std::clamp(x - 2, 0, w - 1)] +
                k1 * s[std::clamp(x - 1, 0, w - 1)] +
                k2 * s[x] +
                k3 * s[std::clamp(x + 1, 0, w - 1)] +
                k4 * s[std::clamp(x + 2, 0, w - 1)]);
        }
    }

    // ---- vertical pass: u16 -> u8 (LUT folded into the store) ----
    for (int y = 0; y < h; ++y) {
        uint8_t* d = buf.data() + static_cast<size_t>(y) * w;
        const uint16_t* r0;
        const uint16_t* r1;
        const uint16_t* r2;
        const uint16_t* r3;
        const uint16_t* r4;
        if (y >= 2 && y <= h - 3) { // interior: branchless, vectorizable
            r0 = tmp.data() + static_cast<size_t>(y - 2) * w;
            r1 = tmp.data() + static_cast<size_t>(y - 1) * w;
            r2 = tmp.data() + static_cast<size_t>(y) * w;
            r3 = tmp.data() + static_cast<size_t>(y + 1) * w;
            r4 = tmp.data() + static_cast<size_t>(y + 2) * w;
        } else { // edge rows: clamped taps
            r0 = tmp.data() + static_cast<size_t>(std::clamp(y - 2, 0, h - 1)) * w;
            r1 = tmp.data() + static_cast<size_t>(std::clamp(y - 1, 0, h - 1)) * w;
            r2 = tmp.data() + static_cast<size_t>(y) * w;
            r3 = tmp.data() + static_cast<size_t>(std::clamp(y + 1, 0, h - 1)) * w;
            r4 = tmp.data() + static_cast<size_t>(std::clamp(y + 2, 0, h - 1)) * w;
        }
        for (int x = 0; x < w; ++x) {
            // acc is the blurred value in 16.16 fixed point (each pass's
            // /256 is still inside the integer dot products); >> 16 with
            // round-half-up matches the float reference's +0.5f.
            const uint32_t acc = k0 * r0[x] + k1 * r1[x] + k2 * r2[x] +
                                 k3 * r3[x] + k4 * r4[x];
            d[x] = lut8[(acc + 32768) >> 16];
        }
    }
}

} // namespace vftstream
