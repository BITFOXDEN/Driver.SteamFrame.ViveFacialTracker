// usb_uvc.hpp
// Userspace UVC capture for the VIVE Facial Tracker over Linux usbfs, for
// hosts whose kernel lacks the uvcvideo module (e.g. Steam Frame).
//
// The XU byte protocol matches Baballonia's VFTCapture/LinuxUsbCommunicator
// (unit 4, selector 2). Capture uses asynchronous usbfs URBs
// (SUBMITURB/REAPURB, 8 x 16 KB in flight); a synchronous USBDEVFS_BULK
// read cannot keep up with the payload rate and eventually wedges the
// tracker.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vftstream {

class UsbUvcCamera {
public:
    UsbUvcCamera() = default;
    ~UsbUvcCamera();
    UsbUvcCamera(const UsbUvcCamera&) = delete;
    UsbUvcCamera& operator=(const UsbUvcCamera&) = delete;

    // Finds "/dev/bus/usb/B/D" for VID 0x0bb4 PID 0x0321 via sysfs.
    static std::string findDeviceNode();

    // Port-level reset (USBDEVFS_RESET); revives a wedged device.
    static bool resetDevice();

    // Claim interfaces 0+1, run the XU enable sequence (scratch size
    // auto-detected, 384 or 64), start streaming, and submit bulk URBs.
    // hardReset power-cycles the port first, which is required after a
    // dead-pipe stall but must not be repeated on a healthy stream. Behind
    // a USB hub the reset is skipped: a hub-mediated port reset drops this
    // tracker off the bus.
    bool open(const std::string& usbNode, bool hardReset = true);
    void close();
    bool isOpen() const { return fd_ >= 0; }
    // Why the last readFrame timed out: true = bytes were still arriving
    // (stream framing problem -> soft reopen); false = pipe dead.
    bool stallSawData() const { return stallSawData_; }

    // Blocks until one complete YUYV frame is assembled by the pump
    // thread's parser (notified via condvar; actual EOF size is
    // auto-detected: classic rev = 320000 B = 400x400).
    bool readFrame(std::vector<uint8_t>& yuyvOut, int timeoutMs);

    // Diagnostic control endpoints (also usable while streaming):
    // GET_* on a selector, returns "...hex" or "!<error>".
    std::string debugXuGet(uint8_t selector, uint8_t rq);
    // SET_CUR from "aabbccddee..." hex (padded to the device's scratch
    // size), returns the ack echo as hex.
    std::string debugXuSet(uint8_t selector, const std::string& hex);
    // Stream on/off via XU 0x14.
    std::string debugStreamOnOff(bool enable);
    // Clears a halted endpoint (default ep 0, the control pipe).
    std::string debugClearHalt(int ep);
    // Full port reset (revives babbling/wedged states).
    std::string debugReset();

    size_t lastFrameBytes() const { return payload_.size(); }

    // Heuristic detector for the OV580 built-in test-bar pattern.
    static bool looksLikeTestPattern(const std::vector<uint8_t>& yuyv,
                                     int* distinctOut = nullptr,
                                     double* meanOut = nullptr);

private:
    // 8 queued bulk URBs = 128 KB in flight: enough headroom to keep the
    // host controller fed across the reap -> resubmit bubbles while the
    // pump also runs the parser.
    static constexpr int kBulkUrbs = 8;
    static constexpr size_t kBulkUrbBytes = 16384;
    static constexpr size_t kMaxFrame = 614400; // covers revs tested
    static constexpr size_t kClassicFrame = 320000; // 400x400 YUYV

    int fd_ = -1;
    bool claimed_[2] = {false, false};

    // Serializes the mutating entry points (open/close and the HTTP
    // diagnostic endpoints); recursive because those endpoints can call
    // close() internally.
    std::recursive_mutex lifeMu_;

    // Pimpl'd to keep usbfs structs out of the header.
    void* bulkUrbs_[kBulkUrbs]{};
    uint8_t* bulkBuf_[kBulkUrbs]{};

    uint16_t xuBufSize_ = 0;
    double lastCleanShutdownMs_ = 0;
    int frameCount_ = 0;

    // ---- UVC bulk payload parser ----
    // The wire is a byte stream of "chunks": each chunk = a 12-byte UVC
    // payload header (bLength=12, bInfo EOH|PTS [+FID] [+EOF]) followed by
    // payload bytes; headers land every 32768 bytes (pinned stride - see
    // parseInput) regardless of URB boundaries. Frames are delimited by the
    // EOF header's chunk; frame payload sizes: 614400 (640x480 YUYV) and
    // 320000 (400x400 YUYV).
    //
    // The parser consumes each reaped URB buffer DIRECTLY - a single bulk
    // endpoint delivers strictly in order, which this parser relies on
    // anyway. Payload bytes copy exactly once (URB -> payload_); the only
    // other buffered bytes are a <12-byte header carry split across URB
    // boundaries and the bounded, transient finalGap_ learning buffer.
    size_t finalGap_ = 0;           // EOF-header -> next frame's header (0 = unknown)
    std::vector<uint8_t> payload_;  // current frame payload accumulator
    std::deque<std::vector<uint8_t>> frameQ_; // completed frames (newest kept)
    // Recycles full-size payload buffers between the pump (allocating side)
    // and the consumer (freeing side) so the steady state never touches the
    // allocator. Guarded by frameQMu_ like frameQ_.
    std::vector<std::vector<uint8_t>> framePool_;
    static constexpr size_t kFramePool = 4;
    void recycleFrameBuffer(std::vector<uint8_t>&& v); // frameQMu_ held
    // Parser state machine:
    //   Expect       - cursor at a candidate 12-byte header
    //   InChunk      - copying chunk payload (chunkRemain_ left; eofPending_
    //                  if it is the frame's final chunk)
    //   ValidateNext - final chunk's payload complete; the next frame's
    //                  header must validate before the frame is emitted
    //   Learn        - final chunk seen but finalGap_ unknown: buffering the
    //                  look-ahead needed to learn it (bounded, transient)
    enum class ParseState { Expect, InChunk, ValidateNext, Learn };
    ParseState pstate_ = ParseState::Expect;
    std::vector<uint8_t> carry_;    // Expect/ValidateNext: <12 bytes of a
                                    // header split across URB boundaries
    std::vector<uint8_t> learnBuf_; // Learn: EOF header + look-ahead
    size_t learnScan_ = 0;          // Learn: next candidate offset to try
    size_t chunkRemain_ = 0;        // payload bytes left in current chunk
    bool eofPending_ = false;       // current chunk is the frame's last
    void feedWire(const uint8_t* d, size_t n);
    void parseInput(const uint8_t* d, size_t n);
    bool learnTry();   // one Learn attempt; true = acted (found or gave up)
    void commitFrame(); // size-check + enqueue + re-arm the accumulator

    // Dedicated URB pump: blocking REAPURB in its own thread so reaping
    // never pauses while the main loop encodes/publishes (the OV580 stops
    // the stream if the host stops reading for more than a moment).
    // SIGUSR1 (the URBs' completion signr) is blocked in every other
    // thread, so it is delivered to - and interrupts - this thread only.
    std::thread pumpThread_;
    std::atomic<bool> pumpRun_{false};
    std::atomic<double> lastDataMs_{0.0};
    std::mutex frameQMu_;           // guards frameQ_
    std::condition_variable frameQCv_; // wakes readFrame (frame done / close)
    void pumpLoop();

    int xCtl(const uint8_t* in, uint8_t* out, uint16_t len, bool isGet, uint8_t rq,
             uint8_t sel = 2) const;
    // Raw control transfer with explicit fields (for VS interface controls
    // and SET_INTERFACE during streaming negotiation).
    int xCtlRaw(uint8_t bmRequestType, uint8_t rq, uint16_t wValue, uint16_t wIndex,
                uint16_t len, void* data) const;
    // UVC PROBE/COMMIT on the streaming interface + SET_INTERFACE(1,0);
    // required by the USB 2.1 persona.
    bool negotiateStreaming();
    int xuSend(const uint8_t* cmd, size_t len, bool waitAck);
    void xuStreamToggle(bool enable, bool waitAck);
    uint16_t xuGetLen(uint8_t selector);
    void applyEnableRegs(bool enable);
    void applyDisableRegs();
    void cleanShutdown(); // XU stream-off + IR dark; must precede release

public:
    void setIrLevel(uint8_t v) { irLevel_ = v; }
    // Requested sensor fps (1..60), folded into the VS negotiation's
    // dwFrameInterval. 60 = the captured ground truth.
    void setFps(int v) { fps_ = v < 1 ? 1 : (v > 60 ? 60 : v); }
    void setHoldXu(bool v) { holdXu_ = v; }
    bool holdXuActive() const { return holdXu_ && fd_ >= 0; }
    // Leaves hold mode: starts streaming and submits URBs.
    bool resumeStreaming();

private:
    uint8_t irLevel_ = 0xff;
    int fps_ = 60;
    bool holdXu_ = false;
    bool stallSawData_ = false;
};

} // namespace vftstream
