// main.cpp - vftstream: stream the VIVE Facial Tracker over HTTP/MJPEG.
//
// Pipeline:
//   [VFT USB cam] --V4L2 YUYV--> [left-crop + 400x400 + blur + LUT]
//     --> [turbojpeg gray JPEG] --> FrameTopic --> HTTP MJPEG / snap / status
//
// The topic is "latest frame wins", so slow/late consumers skip frames
// rather than build backlogs and every client gets freshest data.
#include "config.hpp"
#include "encoder.hpp"
#include "frame_topic.hpp"
#include "http_server.hpp"
#include "pipeline.hpp"
#include "usb_uvc.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <thread>

using namespace std::chrono_literals;

namespace {

std::atomic<bool> g_run{true};

void onSignal(int) {
    // Async-signal-safe: repeated SIGINT/SIGTERM just re-set the flag. The
    // default kill dispositions are restored only after the tracker
    // teardown completes, so a second signal cannot interrupt the
    // XU-disable sequence.
    g_run = false;
}

// Latest-frame mailbox shared between capture/encode and the HTTP layer.
vftstream::FrameTopic g_topic;
std::mutex g_statsMu;
struct Stats {
    uint64_t captured = 0;
    uint64_t encoded = 0;
    uint64_t droppedAtSource = 0;
    uint64_t reconnects = 0;
    double lastEncodeMs = 0;
    double lastCaptureIntervalMs = 0;
    double actualFps = 0;
};
Stats g_stats;

std::string g_statusJson = "{}";
std::mutex g_statusJsonMu;

} // namespace

int mainBody(int argc, char** argv);

int main(int argc, char** argv) {
    // Top-level guard: an uncaught exception must not abort the process
    // (core-dumping mid-operation can leave the tracker USB state wedged).
    try {
        return mainBody(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[fatal] uncaught exception: %s\n", e.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "[fatal] uncaught unknown exception\n");
        return 2;
    }
}

int mainBody(int argc, char** argv) {
    // SIGUSR1 is the usbfs URB-completion ping; UsbUvcCamera installs a
    // real (no-op) handler for it. Block it process-wide before any thread
    // is created: only the URB pump thread unblocks it (pumpLoop), so the
    // completion pings can never interrupt an HTTP send()/recv()/accept().
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);
    signal(SIGPIPE, SIG_IGN);
    {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_BLOCK, &set, nullptr);
    }

    std::string iniPath = "/etc/vftstream.ini";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--ini" && i + 1 < argc) {
            iniPath = argv[++i];
        }
    }
    vftstream::Config cfg;
    if (std::filesystem::exists(iniPath)) {
        cfg = vftstream::Config::fromIni(iniPath);
    } else {
        std::error_code ec;
        std::filesystem::create_directories(
            std::filesystem::path(iniPath).parent_path(), ec);
        cfg.writeTemplate(iniPath);
        std::cout << "wrote default config to " << iniPath << "\n";    }
    cfg.applyArgs(argc, argv);
    cfg.print();

    // ------------------- HTTP server -------------------
    auto parseQuery = [](const std::string& target) {
        std::map<std::string, std::string> q;
        size_t qm = target.find('?');
        if (qm == std::string::npos) return q;
        std::string qs = target.substr(qm + 1);
        size_t p = 0;
        while (p < qs.size()) {
            size_t amp = qs.find('&', p);
            if (amp == std::string::npos) amp = qs.size();
            std::string kv = qs.substr(p, amp - p);
            size_t eq = kv.find('=');
            if (eq != std::string::npos)
                q[kv.substr(0, eq)] = kv.substr(eq + 1);
            p = amp + 1;
        }
        return q;
    };

    // Raw storage for the encoder (never resized; valid bytes = jpegLen).
    std::vector<uint8_t> jpeg;
    size_t jpegLen = 0;
    jpeg.reserve(vftstream::JpegEncoder::maxJpegSize(400, 400) + 16 * 1024);

    vftstream::JpegEncoder enc;
    vftstream::FramePipeline pipe;
    pipe.setBlur(cfg.blur);
    pipe.setLut(cfg.enableLut);
    vftstream::UsbUvcCamera cam;
    cam.setIrLevel(static_cast<uint8_t>(cfg.irLevel));
    cam.setFps(cfg.fps);
    cam.setHoldXu(cfg.holdXu);
    int sensorW = 400, sensorH = 400;
    std::string camLabel = "(nothing)";

    vftstream::HttpServer http(
        cfg.port,
        [&](int fd, const std::string& method, const std::string& target,
            const std::map<std::string, std::string>& headers,
            const std::string& body) {
            (void)headers;
            auto route = target;
            size_t qm = route.find('?');
            if (qm != std::string::npos) route = route.substr(0, qm);

    if (route == "/debug/xu" || route == "/debug/stream" ||
        route == "/debug/clear" || route == "/debug/reset" ||
        route == "/debug/resume") {
        auto q = parseQuery(target);
        std::string out;
        auto toNum = [&](const std::string& s, int def) {
            if (s.empty()) return def;
            try { return std::stoi(s); } catch (...) { return def; }
        };
        if (route == "/debug/stream") {
            int on = toNum(q.count("on") ? q["on"] : "", -1);
            out = (on >= 0) ? cam.debugStreamOnOff(on != 0) :
                              "!use ?on=1 or ?on=0";
        } else if (route == "/debug/clear") {
            out = cam.debugClearHalt(toNum(q.count("ep") ? q["ep"] : "", 0));
        } else if (route == "/debug/reset") {
            out = cam.debugReset();
        } else if (route == "/debug/resume") {
            out = cam.resumeStreaming() ? "resumed" : "!resume-failed";
        } else {
                    int sel = toNum(q.count("sel") ? q["sel"] : "", 2);
                    std::string g = q.count("get") ? q["get"] : "";
                    if (!body.empty() && g.empty()) {
                        out = cam.debugXuSet((uint8_t)std::clamp(sel, 1, 31), body);
                    } else if (!g.empty()) {
                        uint8_t rq = (uint8_t)std::clamp(toNum(g, 0x81) & 0xff, 0, 255);
                        out = cam.debugXuGet((uint8_t)std::clamp(sel, 1, 31), rq);
                    } else {
                        out = "!need get=<rq> or POST body=<hex>";
                    }
                }
                vftstream::writeSimple(fd, "200 OK", "text/plain", out, {});
                return;
            }

            if (method != "GET" && method != "HEAD") {
                vftstream::writeSimple(fd, "405 Method Not Allowed", "text/plain",
                                       "method not allowed", {});
                return;
            }

            if (route == "/status") {
                std::lock_guard<std::mutex> lk(g_statusJsonMu);
                vftstream::writeSimple(fd, "200 OK", "application/json",
                                       g_statusJson, {});
                return;
            }

            if (route == "/metrics") {
                std::lock_guard<std::mutex> lk(g_statsMu);
                char b[512];
                std::snprintf(b, sizeof(b),
                        "captured %llu\nencoded %llu\nreconnects %llu\n"
                        "encode_ms %.2f\nfps %.2f\n",
                        (unsigned long long)g_stats.captured,
                        (unsigned long long)g_stats.encoded,
                        (unsigned long long)g_stats.reconnects,
                        g_stats.lastEncodeMs, g_stats.actualFps);
                vftstream::writeSimple(fd, "200 OK", "text/plain",
                                       std::string(b), {});
                return;
            }

            if (route == "/snap" || route == "/stream") {
                auto latest = g_topic.latest();
                if (!latest) {
                    vftstream::writeSimple(fd, "503 Service Unavailable",
                                           "text/plain",
                                           "no frame yet (camera starting?)", {});
                    return;
                }
                if (route == "/snap") {
                    vftstream::writeSimpleBytes(fd, "200 OK", "image/jpeg",
                                                latest->jpeg.data(),
                                                latest->jpegLen, {});
                    return;
                }

                // MJPEG stream
                std::string boundary = "vftframe";
                std::map<std::string, std::string> h;
                h["Cache-Control"] = "no-cache, private";
                h["Pragma"] = "no-cache";
                h["Connection"] = "close";
                {
                    // Header with no body: use raw sendAll (writeSimple would
                    // add a Content-Length).
                    std::string hdr = "HTTP/1.1 200 OK\r\n";
                    hdr += "Content-Type: multipart/x-mixed-replace;boundary=" +
                           boundary + "\r\n";
                    for (auto& [k, v] : h) hdr += k + ": " + v + "\r\n";
                    hdr += "\r\n";
                    if (!vftstream::sendAll(fd, hdr.data(), hdr.size())) return;
                }

                // A camera reopen (clean shutdown -> settle -> handshake)
                // can leave ~10 s without a new frame, so the stall timeout
                // is generous enough to ride out a recovery. A camera that
                // is truly gone drops the client and Baballonia reconnects.
                uint64_t lastSeq = latest->seq - 1;
                for (;;) {
                    uint64_t seq = 0;
                    auto f = g_topic.waitFor(lastSeq, 12000, seq);
                    if (!f) break; // stalled camera; client should reconnect
                    lastSeq = seq;

                    // One writev() per frame: part header + jpeg +
                    // terminator in a single syscall (with TCP_NODELAY a
                    // separate 2-byte send would be its own tiny packet).
                    char part[160];
                    int plen = std::snprintf(part, sizeof(part),
                                  "--%s\r\nContent-Type: image/jpeg\r\n"
                                  "Content-Length: %zu\r\n\r\n",
                                  boundary.c_str(), f->jpegLen);
                    struct iovec iov[3] = {
                        {part, static_cast<size_t>(plen)},
                        {const_cast<uint8_t*>(f->jpeg.data()), f->jpegLen},
                        {const_cast<char*>("\r\n"), 2},
                    };
                    if (!vftstream::sendAllIov(fd, iov, 3))
                        return; // client went away
                    f.reset(); // release early so the buffer can be recycled

                    if (!g_run) break;
                }
                return;
            }

            vftstream::writeSimple(fd, "404 Not Found", "text/plain",
                                   "try /stream /snap /status /metrics", {});
        });

    if (!http.start()) {
        std::fprintf(stderr, "failed to bind http port %d\n", cfg.port);
        return 1;
    }

    // ------------------- capture → encode → publish -------------------
    std::vector<uint8_t> yuyv;
    std::vector<uint8_t> gray;

    auto openCamera = [&](bool hard) -> bool {
        if (!cfg.device.empty()) {
            if (cam.open(cfg.device, hard)) {
                camLabel = cfg.device;
                return true;
            }
            vftstream::UsbUvcCamera::resetDevice();
            return false;
        }
        std::string node = vftstream::UsbUvcCamera::findDeviceNode();
        if (node.empty()) {
            std::fprintf(stderr, "[vft] no HTC camera on the bus yet\n");
            return false;
        }
        if (!cam.open(node, hard)) {
            // The tracker wedges hard enough that a port reset revives
            // it - saves the manual replug.
            vftstream::UsbUvcCamera::resetDevice();
            return false;
        }
        camLabel = node;
        std::fprintf(stderr, "[vft] opened %s\n", node.c_str());
        return true;
    };

    auto fpsMeterStart = std::chrono::steady_clock::now();
    uint64_t fpsFrames = 0;
    int consecutiveFails = 0;

    auto loopOnce = [&]() -> bool {
        if (cfg.holdXu) {
            // hold mode: camera idle-opened; wait for /debug/resume
            if (!cam.isOpen()) {
                if (!openCamera(true)) {
                    ++g_stats.reconnects;
                    return false;
                }
                std::fprintf(stderr, "[vft] hold mode: camera idle, use /debug/resume\n");
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
            }
            return true; // do NOT read frames while holding XU
        }
        if (!cam.isOpen()) {
            // Soft reopen when the stream was merely unparseable (data
            // still flowing) - a port reset on a streaming device wedges
            // and eventually kills it. Hard reset only for a dead pipe or
            // when soft reopens repeatedly fail to recover.
            const bool hard = !cam.stallSawData() || consecutiveFails >= 3;
            if (!openCamera(hard)) {
                ++g_stats.reconnects;
                return false;
            }
        }

        if (!cam.readFrame(yuyv, 3000)) {
            cam.close();
            return false;
        }
        if (yuyv.size() == 320000ull) {
            sensorW = 400; sensorH = 400;
        } else if (yuyv.size() == 614400ull) {
            sensorW = 640; sensorH = 480;
        } else {
            // Unknown layout: resync.
            return false;
        }
        ++g_stats.captured;

        auto t0 = std::chrono::steady_clock::now();
        pipe.process(yuyv, sensorW, sensorH, gray);
        if (!enc.encodeGray(gray.data(), 400, 400, cfg.quality, jpeg, jpegLen)) {
            std::fprintf(stderr, "[enc] failed\n");
            return false;
        }
        auto t1 = std::chrono::steady_clock::now();

        // std::milli (std::ratio<1,1000>); std::chrono::milli is a
        // non-standard alias some toolchains no longer provide.
        double ts = std::chrono::duration<double, std::milli>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count();
        // Zero-copy hand-off: the topic swaps a recycled buffer back into
        // `jpeg` with enough capacity that the next encode never reallocs.
        // The test-pattern/dark-frame diagnostic runs in readFrame.
        g_topic.publish(jpeg, jpegLen, ts);
        ++g_stats.encoded;

        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        {
            std::lock_guard<std::mutex> lk(g_statsMu);
            g_stats.lastEncodeMs = ms;
            ++fpsFrames;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - fpsMeterStart).count();
            if (dt > 2.0) {
                g_stats.actualFps = fpsFrames / dt;
                fpsFrames = 0;
                fpsMeterStart = now;
            }
        }

        // Cheap status JSON refresh (about every 100 frames).
        if (g_stats.encoded % 100 == 0) {
            std::lock_guard<std::mutex> lk(g_statsMu);
            char b[512];
            std::snprintf(b, sizeof(b),
                "{\"device\":\"%s\",\"fps\":%.2f,\"encode_ms\":%.2f,"
                "\"frames\":%llu,\"reconnects\":%llu}",
                camLabel.c_str(), g_stats.actualFps, g_stats.lastEncodeMs,
                (unsigned long long)g_stats.captured,
                (unsigned long long)g_stats.reconnects);
            std::lock_guard<std::mutex> jk(g_statusJsonMu);
            g_statusJson = b;
        }
        return true;
    };

    int backoff = 0;
    const int sleeps[] = {1000, 1500, 2000, 3000, 4000, 5000};
    while (g_run) {
        if (!loopOnce()) {
            if (!g_run) break;
            ++consecutiveFails;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(sleeps[std::min(backoff, 5)]));
            backoff = std::min(backoff + 1, 5);
            continue;
        }
        backoff = 0;
        consecutiveFails = 0;
    }

    // ------------------- shutdown -------------------
    // Order matters: disable the tracker's XU stream first, then wake every
    // /stream client blocked in FrameTopic::waitFor, then join the HTTP
    // threads (stop() shuts each client socket down so a blocked send/recv
    // fails fast). After stop() returns no thread is inside the handler, so
    // the stack locals the handler captures by reference stay alive.
    cam.close();
    g_topic.shutdown();
    http.stop();

    // Restore default kill dispositions only after the tracker teardown.
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);

    std::lock_guard<std::mutex> lk(g_statsMu);
    std::cout << "\ncaptured " << g_stats.captured
              << ", encoded " << g_stats.encoded << "\n";
    return 0;
}
