// pipeline.hpp
// Grayscale extraction + Babble-compatible processing for VFT frames.
//
// The VFT's "YUYV" is actually a side-by-side stereo image: the left 200
// columns are the left camera, the right 200 the right camera, and both
// live in the Y channel of each 2-byte pixel pair. Processing mirrors
// Baballonia's VFTCommon.cs so users see the same image Babble would:
//   crop to left camera (cols 0..200) -> 400x400 -> 5x5 gaussian ->
//   LUT: out = (i / 2048)^(1/2.5) * 255
//
// Performance notes (this runs 60x/s on the Frame's ARM cores):
//   - the crop is fused into the resize's source sampling (src[2*x]):
//     no intermediate crop buffer, one full pass less of memory traffic;
//   - the resize coordinate maps are cached per source geometry instead
//     of rebuilt (and re-allocated) per frame;
//   - the gaussian is integer fixed-point (kernel scaled by 256, sums to
//     exactly 256) with border-specialized loops, so the interiors have
//     no clamps/branches and auto-vectorize; the intermediate is u16
//     instead of float (half the bytes);
//   - the LUT is folded into the blur's final store (a separate pass only
//     when the blur is disabled).
// setBlur()/setLut() honor the ini/CLI --no-blur/--no-lut options.
#pragma once

#include <cstdint>
#include <vector>

namespace vftstream {

class FramePipeline {
public:
    FramePipeline();

    void setBlur(bool on) { blurOn_ = on; }
    void setLut(bool on) { lutOn_ = on; }

    // In: raw YUYV pixels at the given sensor geometry. Out: 400x400
    // CV_8UC1-shaped buffer, row stride 400. Handles both known layouts:
    //   320000-byte frame => 400x400 stereo, left half (0..200) used;
    //   614400-byte frame => 640x480 fused stereo, left half (0..320) used.
    void process(const std::vector<uint8_t>& yuyv,
                 int sensorWidth, int sensorHeight,
                 std::vector<uint8_t>& grayOut);

    const std::vector<uint8_t>& lut() const { return lut_; }

private:
    static constexpr int kOutW = 400;
    static constexpr int kOutH = 400;

    // (Re)builds the fixed-point sampling maps for sw x sh -> 400x400.
    void buildMaps(int sw, int sh);
    // Bilinear upscale to 400x400, sampling the Y of YUYV pairs directly
    // (src[2*sx + row*srcRowBytes]) - the fused left-camera crop.
    void resizeYUYV(const uint8_t* src, size_t srcRowBytes, uint8_t* dst);
    // Separable 5x5 gaussian, in place on buf via a u16 tmp; `lut` (may be
    // null) is folded into the vertical pass's final store.
    void blur5x5(std::vector<uint8_t>& buf, std::vector<uint16_t>& tmp,
                 const uint8_t* lut);

    std::vector<uint8_t> lut_;
    std::vector<uint8_t> small_;    // 400x400 work/output buffer
    std::vector<uint16_t> blurTmp_; // horizontal-pass intermediate
    bool blurOn_ = true;
    bool lutOn_ = true;

    // Cached resize maps (rebuilt only when the source geometry changes).
    int mapW_ = 0, mapH_ = 0;
    std::vector<int32_t> xSx_, xSx1_, xFrac_;
    std::vector<int32_t> ySy_, ySy1_, yFrac_;
};

} // namespace vftstream
