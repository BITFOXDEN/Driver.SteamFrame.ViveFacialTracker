// reference_pipeline.hpp - golden reference for tests.
//
// A straightforward float implementation of the same crop/resize/gaussian/
// LUT pipeline, with blur and LUT individually skippable. The production
// pipeline is diffed against it for every option combination. It favors
// clarity over speed.
#pragma once

#include <cstdint>
#include <vector>

namespace vftstream_ref {

class ReferencePipeline {
public:
    ReferencePipeline();

    void process(const std::vector<uint8_t>& yuyv,
                  int sensorWidth, int sensorHeight,
                  std::vector<uint8_t>& grayOut,
                  bool blur, bool lut);

    const std::vector<uint8_t>& lut() const { return lut_; }

private:
    std::vector<uint8_t> lut_;
    std::vector<uint8_t> crop_;
    std::vector<uint8_t> small_;
    std::vector<float>   blurTmp_;

    static void resizeBilinear(const uint8_t* src, int sw, int sh, int sstride,
                               uint8_t* dst, int dw, int dh, int dstride);
    static void gaussianBlur5x5(std::vector<uint8_t>& buf, int w, int h,
                                std::vector<float>& tmp);
};

} // namespace vftstream_ref
