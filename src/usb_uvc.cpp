// usb_uvc.cpp - see usb_uvc.hpp
#include "usb_uvc.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>

namespace vftstream {
namespace {

uint8_t kCmdMagic[2] = {0x51, 0x52};

uint8_t sendBuf[384];
uint8_t recvBuf[384];

int xioctl(int fd, unsigned long req, void* arg) {
    for (int i = 0; i < 30; ++i) {
        int r = ioctl(fd, req, arg);
        if (r != -1 || errno != EINTR) return r;
    }
    return -1;
}

void steadySleep(int ms) {
    struct timespec ts{ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

double nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

std::string trimCopy(const std::string& in) {
    size_t a = in.find_first_not_of(" \t\r\n");
    size_t b = in.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : in.substr(a, b - a + 1);
}

// True when the tracker sits behind a USB hub (its sysfs port path has a
// parent hub, e.g. "1-1.3" vs "1-1" for the port itself). Used to skip
// USBDEVFS_RESET: behind a hub that ioctl resets the HUB's downstream port
// and this tracker drops off the bus entirely, while a direct xhci port
// reset is the stale-state revive.
bool trackerBehindHub() {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return false;
    bool behind = false;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        std::string name = e->d_name;
        if (name.find(':') != std::string::npos) continue;  // interface
        if (name.rfind("usb", 0) == 0) continue;            // root hub
        if (name.find('-') == std::string::npos) continue;  // not a device
        if (name.find('.') == std::string::npos) continue;  // on the port itself
        std::ifstream vf("/sys/bus/usb/devices/" + name + "/idVendor");
        std::ifstream pf("/sys/bus/usb/devices/" + name + "/idProduct");
        std::string vid, pid;
        std::getline(vf, vid);
        std::getline(pf, pid);
        if (trimCopy(vid) == "0bb4" && trimCopy(pid) == "0321") { behind = true; break; }
    }
    closedir(dir);
    return behind;
}

} // namespace

static void urbSignalHandler(int) {}

std::string UsbUvcCamera::findDeviceNode() {
    DIR* dir = opendir("/sys/bus/usb/devices");
    if (!dir) return "";
    std::string node;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        std::string name = e->d_name;
        if (name.find(':') != std::string::npos) continue;      // interface
        if (name.rfind("usb", 0) == 0) continue;               // root hub
        if (name.find('-') == std::string::npos) continue;     // not a device

        std::ifstream vf("/sys/bus/usb/devices/" + name + "/idVendor");
        std::ifstream pf("/sys/bus/usb/devices/" + name + "/idProduct");
        std::string vid, pid;
        std::getline(vf, vid);
        std::getline(pf, pid);
        vid = trimCopy(vid);
        pid = trimCopy(pid);
        if (vid != "0bb4" || pid != "0321") continue;

        std::ifstream bf("/sys/bus/usb/devices/" + name + "/busnum");
        std::ifstream df("/sys/bus/usb/devices/" + name + "/devnum");
        std::string bus, dev;
        std::getline(bf, bus);
        std::getline(df, dev);
        bus = trimCopy(bus);
        dev = trimCopy(dev);
        if (bus.empty() || dev.empty()) continue;

        char nodeBuf[64];
        snprintf(nodeBuf, sizeof(nodeBuf), "/dev/bus/usb/%03d/%03d",
                 std::stoi(bus), std::stoi(dev));
        node = nodeBuf;
        break;
    }
    closedir(dir);
    return node;
}

bool UsbUvcCamera::resetDevice() {
    if (trackerBehindHub()) {
        fprintf(stderr, "[usb_uvc] USBDEVFS_RESET skipped: tracker is behind a "
                        "USB hub (a hub port reset drops it off the bus; replug "
                        "the tracker instead)\n");
        return false;
    }
    std::string node = findDeviceNode();
    if (node.empty()) return false;
    int fd = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    int r = ioctl(fd, USBDEVFS_RESET, nullptr);
    int err = errno;
    ::close(fd);
    if (r == 0) {
        fprintf(stderr, "[usb_uvc] USBDEVFS_RESET done on %s\n", node.c_str());
        return true;
    }
    fprintf(stderr, "[usb_uvc] USBDEVFS_RESET %s failed: %s\n", node.c_str(), strerror(err));
    return false;
}

UsbUvcCamera::~UsbUvcCamera() { close(); }

int UsbUvcCamera::xCtl(const uint8_t* in, uint8_t* out, uint16_t len, bool isGet,
                       uint8_t rq, uint8_t sel) const {
    return xCtlRaw(isGet ? 0xA1 : 0x21, rq, (uint16_t)(sel << 8), 0x0400, len,
                   isGet ? (void*)out : (void*)in);
}

int UsbUvcCamera::xCtlRaw(uint8_t bmRequestType, uint8_t rq, uint16_t wValue,
                          uint16_t wIndex, uint16_t len, void* data) const {
    usbdevfs_ctrltransfer c{};
    c.bRequestType = bmRequestType;
    c.bRequest = rq;
    c.wValue = wValue;
    c.wIndex = wIndex; // XU: unit 4 (high) | interface 0 (low); VS: iface 1
    c.wLength = len;
    c.timeout = 1000;
    c.data = data;
    return xioctl(fd_, USBDEVFS_CONTROL, &c);
}

int UsbUvcCamera::xuSend(const uint8_t* cmd, size_t len, bool waitAck) {
    // Persona ground truth captured from SRanipal on Windows:
    //  - USB3/384-scratch: 384-byte SET_CUR payloads with 0x53/0x54 at
    //    [254]/[255].
    //  - USB2.1/64-scratch: 64-byte payloads, no trailer.
    // Every command: GET_LEN, SET_CUR(native size), GET_LEN, GET_CUR until
    // the 0x56|command-echo ack. The unit ACKs any length, but on the 64
    // persona only native-size payloads actually program the sensor.
    uint8_t lb[2] = {0, 0};
    xCtl(NULL, lb, 2, true, 0x85, 2);   // GET_LEN (SRanipal rhythm)

    memset(sendBuf, 0, sizeof(sendBuf));
    memcpy(sendBuf, cmd, len);
    if (xuBufSize_ >= 256) {
        sendBuf[254] = 0x53;
        sendBuf[255] = 0x54;
    }
    if (xCtl(sendBuf, recvBuf, xuBufSize_, false, 0x01) < 0) return -1;
    if (!waitAck) return 0;

    xCtl(NULL, lb, 2, true, 0x85, 2);   // GET_LEN between stage hops
    for (int i = 0; i < 200; ++i) {
        memset(recvBuf, 0, sizeof(recvBuf));
        if (xCtl(NULL, recvBuf, xuBufSize_, true, 0x81) < 0) return -1;
        if (recvBuf[0] == 0x56)
            return memcmp(recvBuf + 1, sendBuf, std::min<size_t>(16, len)) == 0 ? 0 : -2;
        if (recvBuf[0] != 0x55) return -2;
        steadySleep(1);
    }
    return -3;
}

void UsbUvcCamera::xuStreamToggle(bool enable, bool waitAck) {
    uint8_t cmd[4] = {0x50, 0x14, 0x00, (uint8_t)(enable ? 0x01 : 0x00)};
    xuSend(cmd, sizeof(cmd), waitAck);
}

uint16_t UsbUvcCamera::xuGetLen(uint8_t selector) {
    uint8_t lb[2] = {0, 0};
    if (xCtl(nullptr, lb, 2, true, 0x85, selector) < 0) return 0;
    return (uint16_t)((lb[1] << 8) | lb[0]);
}

bool UsbUvcCamera::negotiateStreaming() {
    // UVC PROBE/COMMIT on the video-streaming interface (wIndex=0x0001)
    // followed by SET_INTERFACE(1, alt 0). On normal hosts uvcvideo does
    // this in-kernel; on the USB 2.1 persona it is what actually arms the
    // streaming interface - without it the sensors stay in the OV580
    // factory test-bar mode.
    //
    // Captured rhythm (34-byte UVC 1.5 probe/commit payloads):
    //   GET_CUR(probe) -> SET_CUR(probe, 60fps request) -> GET_CUR(probe)
    //   -> GET_MAX(probe) -> GET_MIN(probe) -> SET_CUR(commit, the
    //   device-adjusted GET_CUR response) -> SET_INTERFACE(1, 0).
    static const uint8_t kProbeSet[34] = {
        0x01, 0x00, 0x01, 0x01, 0x0a, 0x8b, 0x02, 0x00,  // bmHint, fmt 1, frame 1, 166666ns
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x60, 0x09, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x80, 0xd1, 0xf0, 0x08, 0x00,
        0x00, 0x00,
    };
    uint8_t probe[34];
    memcpy(probe, kProbeSet, sizeof(probe));
    // dwFrameInterval (bytes 4..7, 100 ns units) honors --fps. The captured
    // ground truth is 166666 (60 fps): the default sends byte-identical
    // traffic, lower values request a slower sensor cadence.
    const uint32_t interval = 10000000u / static_cast<uint32_t>(fps_);
    probe[4] = static_cast<uint8_t>(interval);
    probe[5] = static_cast<uint8_t>(interval >> 8);
    probe[6] = static_cast<uint8_t>(interval >> 16);
    probe[7] = static_cast<uint8_t>(interval >> 24);
    uint8_t buf[34];

    memset(buf, 0, sizeof(buf));
    xCtlRaw(0xA1, 0x81, 0x0100, 0x0001, sizeof(buf), buf);  // GET_CUR probe

    if (xCtlRaw(0x21, 0x01, 0x0100, 0x0001, sizeof(buf), probe) < 0) {
        fprintf(stderr, "[usb_uvc] VS SET_CUR probe failed: %s\n", strerror(errno));
        return false;
    }

    uint8_t commit[34];
    memset(commit, 0, sizeof(commit));
    if (xCtlRaw(0xA1, 0x81, 0x0100, 0x0001, sizeof(commit), commit) < 0 ||
        commit[2] == 0) {
        fprintf(stderr, "[usb_uvc] VS GET_CUR probe failed: %s\n", strerror(errno));
        return false;
    }
    xCtlRaw(0xA1, 0x83, 0x0100, 0x0001, sizeof(buf), buf);  // GET_MAX
    xCtlRaw(0xA1, 0x82, 0x0100, 0x0001, sizeof(buf), buf);  // GET_MIN

    if (xCtlRaw(0x21, 0x01, 0x0200, 0x0001, sizeof(commit), commit) < 0) {
        fprintf(stderr, "[usb_uvc] VS SET_CUR commit failed: %s\n", strerror(errno));
        return false;
    }

    if (xCtlRaw(0x00, 0x0B, 0x0000, 0x0001, 0, nullptr) < 0) {
        fprintf(stderr, "[usb_uvc] SET_INTERFACE(1,0) failed: %s\n", strerror(errno));
        return false;
    }
    return true;
}

void UsbUvcCamera::applyEnableRegs(bool enable) {
    struct Reg { uint8_t addr, val; };
    static const Reg base[] = {
        {0x00, 0x40}, {0x08, 0x01}, {0x70, 0x00},
        {0x0e, 0x00}, {0x05, 0xb2}, {0x06, 0xb2}, {0x07, 0xb2}, {0x0f, 0x03},
    };
    uint8_t cmd[17] = {0};
    cmd[0] = 0x50; cmd[1] = 0xab; cmd[2] = 0x60; cmd[3] = 0x01; cmd[4] = 0x01;
    cmd[9] = 0x90; cmd[10] = 0x01; cmd[11] = 0x00; cmd[12] = 0x01;
    for (const Reg& r : base) {
        cmd[8] = r.addr;
        cmd[16] = r.val;
        if (xuSend(cmd, sizeof(cmd), 1) != 0) {
            fprintf(stderr, "[usb_uvc] reg 0x%02x write failed (%s)\n",
                    r.addr, strerror(errno));
            return;
        }
    }
    uint8_t ir = enable ? irLevel_ : 0x00;
    for (uint8_t a : {0x02, 0x03, 0x04}) {
        cmd[8] = a;
        cmd[16] = ir;
        if (xuSend(cmd, sizeof(cmd), 1) != 0) {
            fprintf(stderr, "[usb_uvc] IR reg 0x%02x=%02x write failed (%s)\n",
                    a, ir, strerror(errno));
            return;
        }
    }
}

void UsbUvcCamera::applyDisableRegs() {
    struct Reg { uint8_t addr, val; };
    static const Reg regs[] = {
        {0x00, 0x40}, {0x08, 0x01}, {0x70, 0x00},
        {0x02, 0x00}, {0x03, 0x00}, {0x04, 0x00},
        {0x0e, 0x00}, {0x05, 0xb2}, {0x06, 0xb2}, {0x07, 0xb2}, {0x0f, 0x03},
    };
    uint8_t cmd[17] = {0};
    cmd[0] = 0x50; cmd[1] = 0xab; cmd[2] = 0x60; cmd[3] = 0x01; cmd[4] = 0x01;
    cmd[9] = 0x90; cmd[10] = 0x01; cmd[11] = 0x00; cmd[12] = 0x01;
    for (const Reg& r : regs) {
        cmd[8] = r.addr;
        cmd[16] = r.val;
        if (xuSend(cmd, sizeof(cmd), 1) != 0) {
            fprintf(stderr, "[usb_uvc] disable reg 0x%02x write failed (%s)\n",
                    r.addr, strerror(errno));
            return;
        }
    }
}

// Cutting the bulk stream without this dance wedges the tracker's MCU.
// Same order as Babble's SetState(false): magic, stream off, pause, magic,
// IR-off register writes.
void UsbUvcCamera::cleanShutdown() {
    if (fd_ < 0 || xuBufSize_ == 0) return;

    steadySleep(30);
    if (xuSend(kCmdMagic, sizeof(kCmdMagic), 1) != 0) {
        fprintf(stderr, "[usb_uvc] shutdown: magic failed (%s)\n", strerror(errno));
        return;
    }
    xuStreamToggle(false, false);
    steadySleep(100);
    if (xuSend(kCmdMagic, sizeof(kCmdMagic), 1) != 0) return;
    applyDisableRegs();
    steadySleep(200);
    fprintf(stderr, "[usb_uvc] clean shutdown done\n");
}

bool UsbUvcCamera::open(const std::string& usbNode, bool hardReset) {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    close();
    stallSawData_ = false;

    // A real (no-op) handler, not SIG_IGN: only a caught signal
    // interrupts kernel waits and is delivered; SIG_IGN silently drops
    // the urb-completion ping which is what protects the process.
    struct sigaction sa{};
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = urbSignalHandler;
    sigaction(SIGUSR1, &sa, nullptr);

    // The tracker's XU state machine keeps stale state across sessions:
    // it ACKs the whole handshake but the sensors stay in the factory
    // test-bar mode. A port reset before the handshake revives it, but
    // ONLY after a dead pipe - resetting a device that is actively
    // streaming wedges it. And ONLY on a direct port: behind a USB hub a
    // port reset drops this tracker off the bus outright, so it is skipped
    // there (replug the tracker to recover stale state behind a hub).
    if (hardReset) {
        if (trackerBehindHub()) {
            fprintf(stderr, "[usb_uvc] tracker is behind a USB hub: skipping the "
                            "open-time USBDEVFS_RESET (a hub port reset drops it; "
                            "replug the tracker to recover stale state)\n");
        } else {
            resetDevice();
            steadySleep(2000);
        }
    }
    std::string fresh = findDeviceNode();
    const std::string& node = !fresh.empty() ? fresh : usbNode;

    fd_ = ::open(node.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0) {
        fprintf(stderr, "[usb_uvc] open(%s): %s\n", node.c_str(), strerror(errno));
        return false;
    }

    unsigned int ifaces[2] = {0, 1};
    for (int i = 0; i < 2; ++i) {
        if (xioctl(fd_, USBDEVFS_CLAIMINTERFACE, &ifaces[i]) < 0) {
            fprintf(stderr, "[usb_uvc] claim iface%d: %s\n", i, strerror(errno));
            close();
            return false;
        }
        claimed_[i] = true;
    }

    // After a clean shutdown the tracker needs a moment before the next
    // activation lands; racing the settle window makes the later gain
    // registers time out. This wait must happen BEFORE the handshake: the
    // OV580's host-read watchdog stops the stream when the host stops
    // reaping URBs, and a gap between the probe and readFrame trips it.
    if (lastCleanShutdownMs_ > 0) {
        double sinceMs = nowMs() - lastCleanShutdownMs_;
        double needMs = 8000.0; // generous settle after stream stop
        if (sinceMs < needMs) {
            int wait = (int)(needMs - sinceMs);
            fprintf(stderr, "[usb_uvc] settling %d ms after previous shutdown\n", wait);
            steadySleep(wait);
        }
    }
    lastCleanShutdownMs_ = 0;

    xuBufSize_ = xuGetLen(2);
    if (xuBufSize_ != 384 && xuBufSize_ != 64) {
        fprintf(stderr, "[usb_uvc] GET_LEN(sel2)=%u unexpected; aborting\n", xuBufSize_);
        close();
        return false;
    }
    fprintf(stderr, "[usb_uvc] scratch=%u bytes\n", xuBufSize_);

    if (xuSend(kCmdMagic, sizeof(kCmdMagic), 1) != 0) {
        fprintf(stderr, "[usb_uvc] magic ack failed\n");
        close();
        return false;
    }
    xuStreamToggle(false, false);
    steadySleep(250);
    // SRanipal and DragonLord both gate the register block AND the
    // stream-on with a fresh magic. On the USB 2.1 (64-byte scratch)
    // revision a two-magic cadence ACKs every command but never programs
    // the sensor; the third magic is what makes the registers take effect.
    if (xuSend(kCmdMagic, sizeof(kCmdMagic), 1) != 0) {
        fprintf(stderr, "[usb_uvc] magic ack (2) failed\n");
        close();
        return false;
    }
    applyEnableRegs(true);
    if (xuSend(kCmdMagic, sizeof(kCmdMagic), 1) != 0) {
        fprintf(stderr, "[usb_uvc] magic ack (3) failed\n");
        close();
        return false;
    }
    if (!holdXu_) {
        xuStreamToggle(true, false);
        steadySleep(500);   // SRanipal waits ~560ms before the VS negotiation
        if (!negotiateStreaming()) {
            fprintf(stderr, "[usb_uvc] streaming negotiation failed; continuing\n");
        }
        steadySleep(250);
    } else {
        fprintf(stderr, "[usb_uvc] hold mode: skipping stream-on (XU live)\n");
    }

    for (int i = 0; i < kBulkUrbs; ++i) {
        bulkBuf_[i] = static_cast<uint8_t*>(malloc(kBulkUrbBytes));
        auto* u = new usbdevfs_urb{};
        u->type = USBDEVFS_URB_TYPE_BULK;
        u->endpoint = 0x81;
        u->buffer = bulkBuf_[i];
        u->buffer_length = kBulkUrbBytes;
        // usbfs poll() only reports POLLIN on signaled completions; without
        // signr set, each reap pays the full poll timeout.
        u->signr = SIGUSR1;
        bulkUrbs_[i] = u;
        if (xioctl(fd_, USBDEVFS_SUBMITURB, u) < 0) {
            fprintf(stderr, "[usb_uvc] submit bulk %d: %s\n", i, strerror(errno));
            close();
            return false;
        }
    }

    pstate_ = ParseState::Expect;
    carry_.clear();
    carry_.shrink_to_fit();
    learnBuf_.clear();
    learnBuf_.shrink_to_fit();
    learnScan_ = 0;
    finalGap_ = 0;
    chunkRemain_ = 0;
    eofPending_ = false;
    payload_.clear();
    payload_.reserve(kMaxFrame);
    frameQ_.clear();
    framePool_.clear();
    frameCount_ = 0;

    if (holdXu_) {
        fprintf(stderr, "[usb_uvc] hold mode: XU probe point reached (stream OFF)\n");
        return true;
    }
    // The pump thread takes over URB reaping immediately: the OV580 stops
    // the stream if the host stops reading for more than a moment.
    lastDataMs_ = nowMs();
    pumpRun_ = true;
    pumpThread_ = std::thread([this] { pumpLoop(); });
    return true;
}

void UsbUvcCamera::pumpLoop() {
    // Blocking REAPURB woken by the URBs' SIGUSR1 (caught handler). Runs in
    // its own thread so reaping never pauses for encode/publish work.
    //
    // SIGUSR1 is blocked in every other thread (main blocks it before any
    // thread is created), so the completion pings are delivered here
    // exclusively - they interrupt this thread's REAPURB (wanted) and
    // never interrupt a send()/recv() in an HTTP thread.
    sigset_t usr1;
    sigemptyset(&usr1);
    sigaddset(&usr1, SIGUSR1);
    pthread_sigmask(SIG_UNBLOCK, &usr1, nullptr);

    while (pumpRun_ && fd_ >= 0) {
        usbdevfs_urb* reaped = nullptr;
        if (xioctl(fd_, USBDEVFS_REAPURB, &reaped) < 0) {
            if (errno == EINTR) continue;   // woken by a completion signal
            if (errno == ENODEV) { fprintf(stderr, "[usb_uvc] device gone (pump)\n"); break; }
            fprintf(stderr, "[usb_uvc] pump reap: %s\n", strerror(errno));
            break;
        }
        if (!reaped) continue;

        bool isBulk = false;
        for (int i = 0; i < kBulkUrbs; ++i) {
            if (reaped == bulkUrbs_[i]) { isBulk = true; break; }
        }
        if (!isBulk) continue; // unexpected completion; ignore it

        const uint8_t* c = static_cast<const uint8_t*>(reaped->buffer);
        const int r = reaped->actual_length;

        if (reaped->status == 0 && r > 0) {
            feedWire(c, (size_t)r);
            lastDataMs_ = nowMs();
        }

        reaped->status = 0;
        reaped->actual_length = 0;
        reaped->signr = SIGUSR1;
        if (xioctl(fd_, USBDEVFS_SUBMITURB, reaped) < 0) {
            fprintf(stderr, "[usb_uvc] pump resubmit: %s\n", strerror(errno));
            break;
        }
    }
    pthread_sigmask(SIG_BLOCK, &usr1, nullptr);
    pumpRun_ = false;
}

void UsbUvcCamera::close() {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (fd_ >= 0) {
        // Let the tracker power down its stream cleanly BEFORE the pump is
        // stopped and the interfaces are released; skipping this wedges the
        // tracker. Safe to run while the pump thread is blocked in REAPURB:
        // these are independent ioctls on the same fd.
        cleanShutdown();
        lastCleanShutdownMs_ = nowMs();

        // Stop the pump: closing the fd unblocks its REAPURB (EBADF).
        // fd_ is flipped under frameQMu_ + notified so a readFrame() blocked
        // on the condvar wakes immediately and observes the close.
        pumpRun_ = false;
        int pumpFd = fd_;
        {
            std::lock_guard<std::mutex> fq(frameQMu_);
            fd_ = -1;
            frameQCv_.notify_all();
        }
        ::close(pumpFd);
        if (pumpThread_.joinable()) pumpThread_.join();

        // After fd close the kernel has torn down any queued URBs; the
        // user-side structs are no longer written to, so they can be freed
        // outright.
        for (int i = 0; i < kBulkUrbs; ++i) {
            free(bulkBuf_[i]);
            bulkBuf_[i] = nullptr;
            auto* u = static_cast<usbdevfs_urb*>(bulkUrbs_[i]);
            delete u;
            bulkUrbs_[i] = nullptr;
        }
        claimed_[0] = claimed_[1] = false;  // released implicitly by close
    }
    // Parser state: the pump is gone (joined above), but a readFrame() in
    // flight on another thread may still be inside frameQMu_ - take it.
    {
        std::lock_guard<std::mutex> fq(frameQMu_);
        pstate_ = ParseState::Expect;
        carry_.clear();
        carry_.shrink_to_fit();
        learnBuf_.clear();
        learnBuf_.shrink_to_fit();
        learnScan_ = 0;
        finalGap_ = 0;
        chunkRemain_ = 0;
        eofPending_ = false;
        payload_.clear();
        payload_.shrink_to_fit();
        frameQ_.clear();
        framePool_.clear();
    }
    memset(&sendBuf, 0, sizeof(sendBuf));
    memset(&recvBuf, 0, sizeof(recvBuf));
}

// ---------------------------------------------------------------------------
// UVC bulk payload parser.
// The bulk pipe is a raw byte stream; each "chunk" = a 12-byte UVC payload
// header (bLength=12, bInfo EOH|PTS +/-FID/EOF) + payload. Headers appear
// every stride bytes (32768 on the HS rev) regardless of URB boundaries,
// so URB-start heuristics cannot find them - this walks the continuous
// stream instead. Frames end at the EOF header's chunk.
// ---------------------------------------------------------------------------

namespace {
inline bool isUvcHeader12(const uint8_t* p) {
    return p[0] == 12 && (p[1] & 0xFC) == 0x0C;  // EOH|PTS (+FID/EOF)
}
// The bulk stream is chunked into 32768-byte units (12-byte header + 32756
// payload); the frame's final chunk is shorter. Random image bytes
// false-positive as headers far too often to learn the stride dynamically,
// so it is pinned.
constexpr size_t kChunkStride = 32768;
// finalGap_ learning window, as offsets from the EOF header: candidates at
// [kLearnMin, kLearnLimit), each validated by another header exactly one
// stride further out; kLearnNeed is the most data a decision can require.
constexpr size_t kLearnMin = 1024;
constexpr size_t kLearnLimit = 131072;
constexpr size_t kLearnNeed = kLearnLimit + kChunkStride + 12;
} // namespace

void UsbUvcCamera::feedWire(const uint8_t* d, size_t n) {
    if (pstate_ == ParseState::Learn) {
        learnBuf_.insert(learnBuf_.end(), d, d + n);
    } else {
        parseInput(d, n);
    }
    // parseInput can (re-)arm Learn with an already-decidable window (its
    // input was a former learn buffer): decide now instead of waiting for
    // the next URB. learnTry() returning true means it acted (found a gap
    // or gave up); loop while it keeps landing on fresh Learn entries -
    // every iteration consumes at least kLearnMin bytes of stream, so it
    // terminates.
    while (pstate_ == ParseState::Learn && learnTry()) {
    }
}

void UsbUvcCamera::parseInput(const uint8_t* d, size_t n) {
    const uint8_t* p = d;
    size_t left = n;
    for (;;) {
        if (pstate_ == ParseState::InChunk) {
            // Copy payload eagerly as it arrives (URB -> payload_, the only
            // full-rate copy). On any validation failure the accumulated
            // frame is discarded either way.
            const size_t take = std::min(left, chunkRemain_);
            if (take > 0) payload_.insert(payload_.end(), p, p + take);
            p += take;
            left -= take;
            chunkRemain_ -= take;
            if (chunkRemain_ > 0) return; // need more payload bytes
            pstate_ = eofPending_ ? ParseState::ValidateNext : ParseState::Expect;
            continue;
        }

        // Expect and ValidateNext both need the next 12 stream bytes. The
        // steady state takes the zero-copy path (the header sits fully
        // inside the current URB buffer); only headers split across URB
        // boundaries go through the carry assembly. Invariant: carry_ holds
        // the unconsumed stream prefix, the input continues right after it.
        const uint8_t* h;   // the 12 contiguous header bytes
        uint8_t hbuf[12];   // assembly area for the split case
        size_t fromInput;   // header bytes taken from this input
        if (carry_.empty()) {
            if (left < 12) {
                carry_.assign(p, p + left); // split header prefix
                return;
            }
            h = p;
            fromInput = 12;
        } else {
            if (carry_.size() + left < 12) {
                carry_.insert(carry_.end(), p, p + left);
                return;
            }
            fromInput = 12 - carry_.size();
            memcpy(hbuf, carry_.data(), carry_.size());
            memcpy(hbuf + carry_.size(), p, fromInput);
            h = hbuf;
        }

        if (pstate_ == ParseState::ValidateNext) {
            // Final chunk's payload complete: the next frame's first header
            // must validate before the frame is emitted.
            if (!isUvcHeader12(h)) {
                // structure drifted: drop the frame, relearn, resync from
                // the byte after this failed header
                payload_.clear();
                finalGap_ = 0;
                carry_.assign(h + 1, h + 12);
                p += fromInput;
                left -= fromInput;
                pstate_ = ParseState::Expect;
                continue;
            }
            // Emit (or drop + relearn on a wrong size). The peeked header
            // is NOT consumed: Expect re-parses it as the next frame's
            // first chunk header.
            commitFrame();
            pstate_ = ParseState::Expect;
            continue;
        }

        // ParseState::Expect: validate the candidate header.
        if (!isUvcHeader12(h)) {
            // Desync: discard the partial frame, hunt for the next
            // candidate header.
            payload_.clear(); // mid-frame desync: discard partial frame
            if (carry_.empty()) {
                // Fast path: tight scan inside this buffer (the common
                // attach-mid-stream desync shape).
                size_t i = 1;
                for (; i + 12 <= left; ++i)
                    if (isUvcHeader12(p + i)) break;
                if (i + 12 > left) {
                    // No candidate: keep the last 11 bytes - a split header
                    // can begin anywhere in them.
                    carry_.assign(p + left - 11, p + left);
                    return;
                }
                p += i;
                left -= i;
                continue; // re-validate the candidate as a header
            }
            // Header split across URB boundaries: advance one stream byte
            // and re-examine (the same scan, unrolled).
            carry_.assign(h + 1, h + 12);
            p += fromInput;
            left -= fromInput;
            continue;
        }

        const bool eof = (h[1] & 0x02) != 0;
        if (!eof || finalGap_ != 0) {
            carry_.clear();
            p += fromInput;
            left -= fromInput;
            pstate_ = ParseState::InChunk;
            eofPending_ = eof;
            chunkRemain_ = (eof ? finalGap_ : kChunkStride) - 12;
            continue;
        }

        // Final chunk with an unknown gap: enter Learn. Buffer this header
        // plus the whole input tail (bounded at ~kLearnNeed by learnTry);
        // the decision runs from feedWire()/learnTry().
        learnBuf_.assign(h, h + 12);
        learnBuf_.insert(learnBuf_.end(), p + fromInput, p + left);
        carry_.clear();
        learnScan_ = kLearnMin;
        pstate_ = ParseState::Learn;
        return;
    }
}

bool UsbUvcCamera::learnTry() {
    // Learn the final-chunk gap with strong validation: the true next-frame
    // header is followed by another valid header one stride later.
    // Candidates must be at least kLearnMin out (the final payload is
    // ~25 KB in both captured personas) so image-byte false positives
    // can't produce degenerate gaps. learnScan_ persists across calls, so
    // every byte is examined exactly once.
    for (; learnScan_ < kLearnLimit &&
           learnScan_ + kChunkStride + 12 <= learnBuf_.size();
         ++learnScan_) {
        if (!isUvcHeader12(&learnBuf_[learnScan_])) continue;
        if (isUvcHeader12(&learnBuf_[learnScan_ + kChunkStride])) {
            finalGap_ = learnScan_;
            // Re-parse the buffered window through the normal states: the
            // EOF header at [0] now has a known gap; parsing continues
            // into the next frame's data in the buffer tail.
            std::vector<uint8_t> b;
            b.swap(learnBuf_);
            pstate_ = ParseState::Expect;
            parseInput(b.data(), b.size());
            return true; // may have (re-)armed Learn; see feedWire's loop
        }
    }
    if (learnScan_ >= kLearnLimit) {
        // Whole window scanned, no validated pair: the structure is broken.
        // Drop the frame and resync from the buffered tail.
        payload_.clear();
        const size_t sz = learnBuf_.size();
        const size_t keep = sz > 11 ? 11 : sz;
        carry_.assign(learnBuf_.data() + sz - keep, learnBuf_.data() + sz);
        std::vector<uint8_t>().swap(learnBuf_); // free the look-ahead now
        learnScan_ = 0;
        pstate_ = ParseState::Expect;
        return true;
    }
    return false; // waiting for more look-ahead
}

void UsbUvcCamera::commitFrame() {
    if (payload_.size() == kMaxFrame || payload_.size() == kClassicFrame) {
        std::lock_guard<std::mutex> lk(frameQMu_);
        while (frameQ_.size() >= 4) { // keep newest
            recycleFrameBuffer(std::move(frameQ_.front()));
            frameQ_.pop_front();
        }
        frameQ_.push_back(std::move(payload_));
        frameQCv_.notify_all(); // wake readFrame
        // Re-arm the accumulator from the pool: the recycled buffer keeps
        // its kMaxFrame capacity, so the steady state never touches the
        // allocator.
        if (!framePool_.empty()) {
            payload_ = std::move(framePool_.back());
            framePool_.pop_back();
            payload_.clear();
        } else {
            payload_.clear();
            payload_.reserve(kMaxFrame);
        }
    } else {
        // partial frame (stream started mid-frame) or anomaly
        static int drops = 0;
        if (drops < 8)
            fprintf(stderr, "[usb_uvc] parser: dropped %zu-byte frame\n",
                    payload_.size());
        payload_.clear();
        finalGap_ = 0; // structure violated: relearn
    }
}

// Caller holds frameQMu_. Keeps one full-size payload buffer per pool
// slot out of the allocator's hands; anything non-poolable (wrong capacity,
// pool full) is simply destroyed here.
void UsbUvcCamera::recycleFrameBuffer(std::vector<uint8_t>&& v) {
    if (framePool_.size() < kFramePool && v.capacity() >= kMaxFrame)
        framePool_.push_back(std::move(v));
}

bool UsbUvcCamera::readFrame(std::vector<uint8_t>& yuyvOut, int timeoutMs) {
    if (fd_ < 0) return false;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);

    // The pump thread keeps the wire moving and notifies frameQCv_ on every
    // completed frame; this consumes frames as they land and enforces a
    // freshness deadline. stallSawData_ distinguishes "wire still
    // delivering, parser starved" (soft reopen) from a dead pipe (hard
    // reopen - resetting a live device wedges it).
    //
    // Frames are consumed latest-wins, like the FrameTopic: anything queued
    // while this thread was busy (encode/publish, or the cores being hogged
    // by the streamed game's decoder) is dropped instead of digested
    // serially - stale frames are worthless to a face tracker.
    std::unique_lock<std::mutex> lk(frameQMu_);
    for (;;) {
        if (!frameQ_.empty()) {
            while (frameQ_.size() > 1) {
                recycleFrameBuffer(std::move(frameQ_.front()));
                frameQ_.pop_front();
            }
            // Swap (not move-assign): the consumer's old buffer goes back
            // to the pool and the fresh buffer survives in yuyvOut - no
            // alloc/free on either side in the steady state.
            yuyvOut.swap(frameQ_.front());
            recycleFrameBuffer(std::move(frameQ_.front()));
            frameQ_.pop_front();
            frameCount_++;

            const bool check = (frameCount_ % 60 == 1);
            lk.unlock();  // don't hold the pump's lock over stderr I/O
            if (check) {
                int d = 0; double m = 0;
                if (looksLikeTestPattern(yuyvOut, &d, &m))
                    fprintf(stderr, "[usb_uvc] test-pattern mode (distinct=%d mean=%.1f)\n", d, m);
                else
                    fprintf(stderr, "[usb_uvc] frame %d ok (%zu B, distinct=%d mean=%.1f)\n",
                            frameCount_, yuyvOut.size(), d, m);
            }
            return true;
        }
        if (fd_ < 0) return false; // closed underneath us
        if (frameQCv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            if (!frameQ_.empty()) continue; // raced with the timeout
            const bool recent = (nowMs() - lastDataMs_.load()) < 500.0;
            stallSawData_ = recent;
            if (recent)
                fprintf(stderr, "[usb_uvc] readFrame: no frame in %d ms but wire busy - "
                                "parser starved, SOFT reopen\n", timeoutMs);
            else
                fprintf(stderr, "[usb_uvc] readFrame: no frame within %d ms "
                                "(pipe dead); HARD reopen\n", timeoutMs);
            return false;
        }
    }
}

bool UsbUvcCamera::looksLikeTestPattern(const std::vector<uint8_t>& yuyv,
                                        int* distinctOut, double* meanOut) {
    if (yuyv.size() < 8192) return false;
    // Sample 4096 Y values spread over the whole frame.
    const size_t totalY = yuyv.size() / 2;
    int hist[256] = {0};
    double sum = 0;
    for (int i = 0; i < 4096; ++i) {
        const uint8_t v = yuyv[2 * ((size_t)i * totalY / 4096)];
        hist[v]++;
        sum += v;
    }
    int distinct = 0;
    for (int i = 0; i < 256; ++i) if (hist[i]) distinct++;
    if (distinctOut) *distinctOut = distinct;
    if (meanOut) *meanOut = sum / 4096;
    return distinct < 16;
}

// ---------------------------------------------------------------------------
// diagnostic control endpoints
// ---------------------------------------------------------------------------

static std::string hexString(const uint8_t* d, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(hx[d[i] >> 4]);
        out.push_back(hx[d[i] & 0xf]);
    }
    return out;
}

static int hexToBytes(const std::string& hex, uint8_t* out, size_t max) {
    int idx = 0;
    for (size_t i = 0; i + 1 < hex.size() && (size_t)idx < max; i += 2) {
        auto one = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return -1;
        };
        int hi = one(hex[i]);
        int lo = one(hex[i + 1]);
        if (hi < 0 || lo < 0) return idx;
        out[idx++] = (uint8_t)((hi << 4) | lo);
    }
    return idx;
}

std::string UsbUvcCamera::debugXuGet(uint8_t selector, uint8_t rq) {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!isOpen()) return "!not-open";
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    uint16_t len;
    switch (rq) {
        case 0x85: len = 2; break;                 // GET_LEN
        case 0x86: len = 1; break;                 // GET_INFO
        default:   len = xuBufSize_; break;        // CUR/MIN/MAX/RES/DEF
    }
    if (xCtl(NULL, buf, len, true, rq, selector) < 0) {
        if (errno == ENODEV) {
            fprintf(stderr, "[usb_uvc] stale device node; reopening\n");
            close();
            return "!stale-reopen";
        }
        return std::string("!") + strerror(errno);
    }
    return hexString(buf, len);
}

std::string UsbUvcCamera::debugXuSet(uint8_t selector, const std::string& hex) {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!isOpen()) return "!not-open";
    uint8_t payload[384];
    int n = hexToBytes(hex, payload, sizeof(payload));
    if (n <= 0) return "!empty-or-bad-hex";
    int rc = xuSend(payload, (size_t)n, 1);
    if (rc != 0)
        return std::string("!") + (rc == -1 ? strerror(errno) : "ack-mismatch");
    return hexString(sendBuf, xuBufSize_);
}

std::string UsbUvcCamera::debugStreamOnOff(bool enable) {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!isOpen()) return "!not-open";
    uint8_t cmd[4] = {0x50, 0x14, 0x00, (uint8_t)(enable ? 0x01u : 0x00u)};
    int rc = xuSend(cmd, sizeof(cmd), 1);
    if (rc != 0)
        return std::string("!") + (rc == -1 ? strerror(errno) : "ack-mismatch");
    return hexString(sendBuf, xuBufSize_);
}

bool UsbUvcCamera::resumeStreaming() {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!holdXu_) return true;
    if (fd_ < 0) return false;
    holdXu_ = false;

    xuStreamToggle(true, false);
    steadySleep(250);

    for (int i = 0; i < kBulkUrbs; ++i) {
        if (bulkUrbs_[i]) continue; // already in flight
        auto* u = new usbdevfs_urb{};
        u->type = USBDEVFS_URB_TYPE_BULK;
        u->endpoint = 0x81;
        u->buffer = bulkBuf_[i];
        u->buffer_length = kBulkUrbBytes;
        u->signr = SIGUSR1;
        bulkUrbs_[i] = u;
        if (xioctl(fd_, USBDEVFS_SUBMITURB, u) < 0) {
            fprintf(stderr, "[usb_uvc] submit bulk %d: %s\n", i, strerror(errno));
            return false;
        }
    }
    fprintf(stderr, "[usb_uvc] resumed streaming from hold mode\n");
    return true;
}

std::string UsbUvcCamera::debugClearHalt(int ep) {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!isOpen()) return "!not-open";
    int e = ep;
    if (xioctl(fd_, USBDEVFS_CLEAR_HALT, &e) < 0)
        return std::string("!") + strerror(errno);
    return "cleared";
}

std::string UsbUvcCamera::debugReset() {
    std::lock_guard<std::recursive_mutex> life(lifeMu_);
    if (!isOpen()) return "!not-open";
    if (xioctl(fd_, USBDEVFS_RESET, nullptr) < 0)
        return std::string("!") + strerror(errno);
    return "reset-ok";
}

} // namespace vftstream
