// test_http.cpp - end-to-end HTTP + MJPEG test over real loopback sockets:
// endpoints, multipart framing, data integrity, concurrent stream clients,
// a slow reader (partial writev), and a request dribbled byte-by-byte
// (header accumulation across recv()s).
#include "../src/frame_topic.hpp"
#include "../src/http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

using vftstream::FrameTopic;
using vftstream::HttpServer;

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                        \
    do {                                                        \
        if (cond) { std::printf("  [ ok ] %s\n", msg); }         \
        else      { std::printf("  [FAIL] %s\n", msg); ++g_failures; } \
    } while (0)

constexpr int kPort = 18097;
constexpr const char* kBoundary = "vftframe";

FrameTopic g_topic;
std::atomic<bool> g_stop{false};

void fillSeqTagged(std::vector<uint8_t>& buf, uint64_t seq, size_t len) {
    buf.resize(len);
    for (size_t i = 0; i < len; ++i)
        buf[i] = (uint8_t)((seq * 131 + i * 7) & 0xff);
    for (int i = 0; i < 8; ++i) // overwrite tag for seq recovery
        buf[i] = (uint8_t)((seq >> (56 - 8 * i)) & 0xff);
}
size_t frameLen(uint64_t seq) { return 3000 + ((seq - 1) % 500) * 7; }

int connectClient() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(kPort);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) != 0) {
        ::close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

bool sendStr(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0) return false;
        off += (size_t)n;
    }
    return true;
}

// Reads a full HTTP response (status line + headers + Content-Length body).
bool readResponse(int fd, std::string& statusOut, std::string& bodyOut,
                  std::string* headersOut = nullptr) {
    std::string all;
    char buf[16384];
    size_t hdrEnd = std::string::npos;
    while (hdrEnd == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        all.append(buf, (size_t)n);
        hdrEnd = all.find("\r\n\r\n");
    }
    statusOut = all.substr(0, all.find("\r\n"));
    if (headersOut) *headersOut = all.substr(0, hdrEnd);
    size_t clPos = all.find("Content-Length:");
    if (clPos == std::string::npos || clPos > hdrEnd) return false;
    size_t cl = strtoul(all.c_str() + clPos + 15, nullptr, 10);
    bodyOut = all.substr(hdrEnd + 4);
    while (bodyOut.size() < cl) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        bodyOut.append(buf, (size_t)n);
    }
    return bodyOut.size() == cl;
}

// Parses one multipart part; returns bytes consumed, or 0 if incomplete.
size_t parsePart(const std::string& s, size_t off, uint64_t& seqOut,
                 std::vector<uint8_t>& jpegOut) {
    size_t p = s.find("--" + std::string(kBoundary), off);
    if (p == std::string::npos) return 0;
    size_t ct = s.find("Content-Type: image/jpeg", p);
    if (ct == std::string::npos) return 0;
    size_t clPos = s.find("Content-Length: ", ct);
    if (clPos == std::string::npos) return 0;
    size_t len = strtoul(s.c_str() + clPos + 16, nullptr, 10);
    size_t body = s.find("\r\n\r\n", clPos);
    if (body == std::string::npos) return 0;
    body += 4;
    if (s.size() < body + len + 2) return 0;
    jpegOut.assign(s.begin() + body, s.begin() + body + len);
    // Recover the seq from the frame contents (first 8 bytes, big endian).
    seqOut = 0;
    for (int i = 0; i < 8; ++i)
        seqOut = (seqOut << 8) | jpegOut[i];
    return body + len + 2 - off;
}

} // namespace

int main() {
    std::printf("== HTTP + MJPEG test ==\n");

    HttpServer http(kPort, [](int fd, const std::string& method,
                              const std::string& target,
                              const std::map<std::string, std::string>& headers,
                              const std::string& body) {
        (void)headers; (void)body;
        std::string route = target;
        size_t qm = route.find('?');
        if (qm != std::string::npos) route = route.substr(0, qm);

        if (method != "GET" && method != "HEAD") {
            vftstream::writeSimple(fd, "405 Method Not Allowed", "text/plain",
                                   "method not allowed", {});
            return;
        }
        if (route == "/hello") {
            vftstream::writeSimple(fd, "200 OK", "text/plain", "hello world", {});
            return;
        }
        if (route == "/snap") {
            auto f = g_topic.latest();
            if (!f) {
                vftstream::writeSimple(fd, "503 Service Unavailable", "text/plain",
                                       "no frame yet", {});
                return;
            }
            vftstream::writeSimpleBytes(fd, "200 OK", "image/jpeg",
                                        f->jpeg.data(), f->jpegLen, {});
            return;
        }
        if (route == "/stream") {
            auto latest = g_topic.latest();
            if (!latest) {
                vftstream::writeSimple(fd, "503 Service Unavailable", "text/plain",
                                       "no frame yet", {});
                return;
            }
            std::string hdr = "HTTP/1.1 200 OK\r\n";
            hdr += std::string("Content-Type: multipart/x-mixed-replace;boundary=") +
                   kBoundary + "\r\n";
            hdr += "Connection: close\r\n\r\n";
            if (!vftstream::sendAll(fd, hdr.data(), hdr.size())) return;

            uint64_t lastSeq = latest->seq - 1;
            for (;;) {
                uint64_t seq = 0;
                auto f = g_topic.waitFor(lastSeq, 500, seq);
                if (!f) break;
                lastSeq = seq;
                char part[160];
                int plen = std::snprintf(part, sizeof(part),
                                         "--%s\r\nContent-Type: image/jpeg\r\n"
                                         "Content-Length: %zu\r\n\r\n",
                                         kBoundary, f->jpegLen);
                struct iovec iov[3] = {
                    {part, (size_t)plen},
                    {const_cast<uint8_t*>(f->jpeg.data()), f->jpegLen},
                    {const_cast<char*>("\r\n"), 2},
                };
                if (!vftstream::sendAllIov(fd, iov, 3)) return;
                f.reset();
                if (g_stop) break;
            }
            return;
        }
        vftstream::writeSimple(fd, "404 Not Found", "text/plain", "nope", {});
    });
    CHECK(http.start(), "server started");

    // -- simple endpoints --
    {
        int fd = connectClient();
        CHECK(fd >= 0, "client connected");
        CHECK(sendStr(fd, "GET /hello HTTP/1.1\r\nHost: t\r\n\r\n"), "request sent");
        std::string status, body;
        CHECK(readResponse(fd, status, body), "response parsed");
        CHECK(status.find("200") != std::string::npos && body == "hello world",
              "/hello body correct");
        ::close(fd);
    }
    {
        int fd = connectClient();
        CHECK(sendStr(fd, "POST /hello HTTP/1.1\r\nHost: t\r\nContent-Length: 0\r\n\r\n"),
              "POST sent");
        std::string status, body;
        CHECK(readResponse(fd, status, body), "405 response parsed");
        CHECK(status.find("405") != std::string::npos, "405 for non-GET");
        ::close(fd);
    }
    {
        int fd = connectClient();
        CHECK(sendStr(fd, "GET /nope HTTP/1.1\r\nHost: t\r\n\r\n"), "404 request sent");
        std::string status, body;
        CHECK(readResponse(fd, status, body) && status.find("404") != std::string::npos,
              "404 for unknown route");
        ::close(fd);
    }

    // -- request dribbled byte-by-byte (header accumulation across recvs) --
    {
        int fd = connectClient();
        std::string req = "GET /hello HTTP/1.1\r\nHost: t\r\nX-Pad: 0123456789\r\n\r\n";
        bool sent = true;
        for (char c : req) {
            if (send(fd, &c, 1, 0) != 1) { sent = false; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CHECK(sent, "dribbled request sent");
        std::string status, body;
        CHECK(readResponse(fd, status, body) && body == "hello world",
              "dribbled request handled");
        ::close(fd);
    }

    // -- /snap before any frame --
    {
        int fd = connectClient();
        sendStr(fd, "GET /snap HTTP/1.1\r\nHost: t\r\n\r\n");
        std::string status, body;
        CHECK(readResponse(fd, status, body) && status.find("503") != std::string::npos,
              "/snap 503 before first frame");
        ::close(fd);
    }

    // -- producer thread: publish frames continuously --
    std::thread producer([&] {
        std::vector<uint8_t> buf;
        for (uint64_t i = 1; !g_stop; ++i) {
            fillSeqTagged(buf, i, frameLen(i));
            g_topic.publish(buf, buf.size(), (double)i);
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let frames flow

    // -- /snap --
    {
        int fd = connectClient();
        sendStr(fd, "GET /snap HTTP/1.1\r\nHost: t\r\n\r\n");
        std::string status, body, headers;
        CHECK(readResponse(fd, status, body, &headers) &&
                  status.find("200") != std::string::npos &&
                  headers.find("Content-Type: image/jpeg") != std::string::npos,
              "/snap 200 image/jpeg");
        uint64_t seq = 0;
        for (int i = 0; i < 8; ++i) seq = (seq << 8) | (uint8_t)body[i];
        CHECK(body.size() == frameLen(seq),
              "/snap body is exactly the published frame");
        ::close(fd);
    }

    // -- MJPEG: normal reader --
    {
        int fd = connectClient();
        sendStr(fd, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
        std::string all;
        char buf[65536];
        // Read the response header first.
        while (all.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            all.append(buf, (size_t)n);
        }
        CHECK(all.find("200") != std::string::npos &&
                  all.find("multipart/x-mixed-replace") != std::string::npos,
              "multipart response header");

        size_t consumed = all.find("\r\n\r\n") + 4;
        int goodParts = 0;
        uint64_t lastSeq = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (goodParts < 30 && std::chrono::steady_clock::now() < deadline) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            all.append(buf, (size_t)n);
            for (;;) {
                uint64_t seq = 0;
                std::vector<uint8_t> jpeg;
                size_t used = parsePart(all, consumed, seq, jpeg);
                if (!used) break;
                consumed += used;
                ++goodParts;
                if (seq <= lastSeq) { CHECK(false, "multipart seq regression"); break; }
                if (jpeg.size() < 3000) { CHECK(false, "multipart jpeg truncated"); break; }
                lastSeq = seq;
                // Content-Length must equal the parsed frame length.
                if (jpeg.size() != frameLen(seq)) {
                    CHECK(false, "multipart jpeg content mismatch");
                    break;
                }
            }
        }
        CHECK(goodParts >= 30, "MJPEG: >=30 frames parsed, all well-formed");
        ::close(fd);
    }

    // -- MJPEG: slow reader (forces partial writev / window stalls) --
    {
        int fd = connectClient();
        // Small receive buffer to slow the server down.
        int rcvbuf = 8192;
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        sendStr(fd, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
        size_t got = 0;
        char buf[2048];
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) {
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            got += (size_t)n;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(got > 100 * 1024, "slow reader still streamed >100KB");
        ::close(fd);
    }

    // -- MJPEG: three concurrent fast readers --
    {
        std::atomic<int> totalGood{0};
        std::atomic<bool> anyBad{false};
        std::vector<std::thread> clients;
        for (int c = 0; c < 3; ++c) {
            clients.emplace_back([&] {
                int fd = connectClient();
                if (fd < 0) { anyBad = true; return; }
                sendStr(fd, "GET /stream HTTP/1.1\r\nHost: t\r\n\r\n");
                std::string all;
                char buf[65536];
                while (all.find("\r\n\r\n") == std::string::npos) {
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    all.append(buf, (size_t)n);
                }
                size_t consumed = all.find("\r\n\r\n") + 4;
                int goodParts = 0;
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < deadline) {
                    ssize_t n = recv(fd, buf, sizeof(buf), 0);
                    if (n <= 0) break;
                    all.append(buf, (size_t)n);
                    for (;;) {
                        uint64_t seq = 0;
                        std::vector<uint8_t> jpeg;
                        size_t used = parsePart(all, consumed, seq, jpeg);
                        if (!used) break;
                        consumed += used;
                        if (jpeg.size() != frameLen(seq)) {
                            anyBad = true;
                        } else {
                            ++goodParts;
                        }
                    }
                }
                totalGood += goodParts;
                ::close(fd);
            });
        }
        for (auto& t : clients) t.join();
        CHECK(!anyBad && totalGood > 60,
              "3 concurrent MJPEG clients, frames consistent");
    }

    g_stop = true;
    producer.join();
    // Same sequence as the daemon: wake every /stream client blocked in
    // waitFor, then join all HTTP threads (stop() shuts the sockets down).
    g_topic.shutdown();
    http.stop();

    if (g_failures) {
        std::printf("FAILED (%d)\n", g_failures);
        return 1;
    }
    std::printf("ALL PASS\n");
    return 0;
}
