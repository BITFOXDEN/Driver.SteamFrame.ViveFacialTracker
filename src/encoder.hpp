// encoder.hpp - grayscale JPEG encoding via libjpeg-turbo
#pragma once

#include <cstdint>
#include <vector>
#include <turbojpeg.h>

namespace vftstream {

class JpegEncoder {
public:
    JpegEncoder() : handle_(tjInitCompress()) {}

    ~JpegEncoder() {
        if (handle_) {
            tjDestroy(handle_);
            handle_ = nullptr;
        }
    }

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // gray: single-channel, w x h, row stride == w.
    // outJpeg is raw storage grown by reserve() only: resizing to the
    // worst-case tjBufSize every frame would value-init (zero-fill) ~120 KB
    // of tail bytes after the topic hands back the previous frame's
    // actual-size buffer. The compressed length comes back in outLen;
    // consumers must pair outJpeg.data() with outLen, not size().
    bool encodeGray(const uint8_t* gray, int w, int h, int quality,
                    std::vector<uint8_t>& outJpeg, size_t& outLen) {
        if (!handle_) { outLen = 0; return false; }
        const unsigned long need = tjBufSize(w, h, TJSAMP_GRAY);
        if (outJpeg.capacity() < need) outJpeg.reserve(need);  // no zero-fill
        uint8_t* outPtr = outJpeg.data();
        unsigned long jpegSize = static_cast<unsigned long>(outJpeg.capacity());
        int rc = tjCompress2(handle_, gray, w, 0, h, TJPF_GRAY,
                             &outPtr, &jpegSize, TJSAMP_GRAY, quality,
                             TJFLAG_FASTDCT | TJFLAG_NOREALLOC);
        if (rc != 0) { outLen = 0; return false; }
        outLen = static_cast<size_t>(jpegSize);
        return true;
    }

    static size_t maxJpegSize(int w, int h) {
        return tjBufSize(w, h, TJSAMP_GRAY) + 64;
    }

private:
    tjhandle handle_ = nullptr;
};

} // namespace vftstream
