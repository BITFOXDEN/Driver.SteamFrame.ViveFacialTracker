// reference_pipeline.cpp - see reference_pipeline.hpp
#include "reference_pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vftstream_ref {

ReferencePipeline::ReferencePipeline() {
    lut_.resize(256);
    for (int i = 0; i < 256; ++i) {
        double v = std::pow(i / 2048.0, 1.0 / 2.5) * 255.0;
        lut_[i] = static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
    }
}

void ReferencePipeline::process(const std::vector<uint8_t>& yuyv,
                                int width, int height,
                                std::vector<uint8_t>& grayOut,
                                bool blur, bool lut) {
    const bool classic = (width == 400);
    const int cropW = classic ? 200 : width / 2;
    const int rowBytes = width * 2;
    const int cropH = height;

    crop_.resize(static_cast<size_t>(cropW) * cropH);
    for (int y = 0; y < cropH; ++y) {
        const uint8_t* src = yuyv.data() + static_cast<size_t>(y) * rowBytes;
        uint8_t* dst = crop_.data() + static_cast<size_t>(y) * cropW;
        for (int x = 0; x < cropW; ++x)
            dst[x] = src[2 * x]; // Y of pair (x, x+1)
    }

    small_.resize(400ull * 400ull);
    resizeBilinear(crop_.data(), cropW, cropH, cropW,
                   small_.data(), 400, 400, 400);

    if (blur)
        gaussianBlur5x5(small_, 400, 400, blurTmp_);

    if (lut) {
        for (size_t i = 0; i < small_.size(); ++i)
            small_[i] = lut_[small_[i]];
    }

    grayOut.swap(small_);
}

// Bilinear upscale, fixed-point 16.16.
void ReferencePipeline::resizeBilinear(const uint8_t* src, int sw, int sh, int sstride,
                                       uint8_t* dst, int dw, int dh, int dstride) {
    const int32_t fx = (static_cast<int64_t>(sw) << 16) / dw;
    const int32_t fy = (static_cast<int64_t>(sh) << 16) / dh;

    std::vector<int32_t> xmap(dw * 2), ymap(dh * 2);
    for (int dx = 0; dx < dw; ++dx) {
        int32_t sx = std::clamp((dx * fx) >> 16, 0, sw - 1);
        int32_t frac = std::clamp((dx * fx) & 0xFFFF, 0, 65535);
        xmap[2 * dx] = sx;
        xmap[2 * dx + 1] = frac;
    }
    for (int dy = 0; dy < dh; ++dy) {
        int32_t sy = (dy * fy) >> 16;
        sy = std::clamp(sy, 0, sh - 1);
        int32_t frac = ((dy * fy) & 0xFFFF);
        frac = std::clamp(frac, 0, 65535);
        ymap[2 * dy] = sy;
        ymap[2 * dy + 1] = frac;
    }

    for (int dy = 0; dy < dh; ++dy) {
        int32_t sy = ymap[2 * dy];
        int32_t fyFrac = ymap[2 * dy + 1];
        const uint8_t* row0 = src + static_cast<size_t>(sy) * sstride;
        const uint8_t* row1 = src + static_cast<size_t>(std::min(sy + 1, sh - 1)) * sstride;
        uint8_t* drow = dst + static_cast<size_t>(dy) * dstride;

        for (int dx = 0; dx < dw; ++dx) {
            int32_t sx = xmap[2 * dx];
            int32_t fxFrac = xmap[2 * dx + 1];
            int32_t sx1 = std::min(sx + 1, sw - 1);

            int32_t p00 = row0[sx];
            int32_t p10 = row0[sx1];
            int32_t p01 = row1[sx];
            int32_t p11 = row1[sx1];

            int32_t top = p00 + (((p10 - p00) * fxFrac) >> 16);
            int32_t bot = p01 + (((p11 - p01) * fxFrac) >> 16);
            drow[dx] = static_cast<uint8_t>(top + (((bot - top) * fyFrac) >> 16));
        }
    }
}

// Gaussian 5x5, sigma ~= 1.0 (matches OpenCV GaussianBlur((5,5),0) closely
// enough for tracking). Tmp is a single float image.
void ReferencePipeline::gaussianBlur5x5(std::vector<uint8_t>& buf, int w, int h,
                                        std::vector<float>& tmp) {
    // 1D kernel for sigma 1.1 (OpenCV's rule for ksize 5 with sigmaX=0).
    static const float k[5] = {0.0708f, 0.2444f, 0.3697f, 0.2444f, 0.0708f};
    tmp.assign(static_cast<size_t>(w) * h, 0.0f);

    // Horizontal
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = buf.data() + static_cast<size_t>(y) * w;
        float* out = tmp.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -2; i <= 2; ++i) {
                int xx = std::clamp(x + i, 0, w - 1);
                acc += k[i + 2] * row[xx];
            }
            out[x] = acc;
        }
    }

    // Vertical
    for (int y = 0; y < h; ++y) {
        float* out = tmp.data() + static_cast<size_t>(y) * w;
        uint8_t* dst = buf.data() + static_cast<size_t>(y) * w;
        for (int x = 0; x < w; ++x) {
            float acc = 0.0f;
            for (int i = -2; i <= 2; ++i) {
                int yy = std::clamp(y + i, 0, h - 1);
                acc += k[i + 2] * tmp[static_cast<size_t>(yy) * w + x];
            }
            dst[x] = static_cast<uint8_t>(acc + 0.5f);
        }
    }
}

} // namespace vftstream_ref
