// http_server.hpp - minimal threaded HTTP server with MJPEG support.
// Endpoints (see main.cpp for handlers):
//   GET /stream   -> multipart/x-mixed-replace MJPEG (Baballonia-compatible)
//   GET /snap     -> single JPEG
//   GET /status   -> JSON health info
//   GET /metrics  -> simple counters
//
// FrameTopic (the producer -> consumers mailbox) lives in frame_topic.hpp.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

namespace vftstream {

class HttpServer {
public:
    // target = "/path?query=value..."; body = raw request body (may be empty).
    using Handler = std::function<void(
        int clientFd,
        const std::string& method,
        const std::string& target,
        const std::map<std::string, std::string>& headers,
        const std::string& body)>;

    HttpServer(int port, Handler handler);
    ~HttpServer();
    bool start();
    void stop();

private:
    int port_;
    Handler handler_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    std::vector<std::thread> workers_;

    // Client-thread registry. stop() joins every live client thread so none
    // can outlive this object (its handler_). A thread marks its entry done
    // under clientsMu_ as its last act, so a reaper only removes and joins
    // entries whose thread is at its return statement; the entry is
    // destroyed only after the join returns.
    struct ClientEntry {
        std::thread t;
        int fd = -1;
        std::atomic<bool> done{false};
    };
    std::mutex clientsMu_;
    std::list<std::unique_ptr<ClientEntry>> clients_;

    void acceptLoop();
    void handleClient(int fd);
    void spawnClient(int fd);
    void clientMain(int fd, ClientEntry* self);
    void reapDoneClients();
};

// Free functions (used by endpoint code outside this translation unit):
bool sendAll(int fd, const void* data, size_t len);
// One writev() for all parts; robust across partial writes (the segment
// cursor advances correctly when the kernel accepts only a prefix).
bool sendAllIov(int fd, const struct iovec* parts, int cnt);
void writeSimple(int fd, const char* status, const char* contentType,
                 const std::vector<uint8_t>& body,
                 const std::map<std::string, std::string>& extraHeaders);
void writeSimple(int fd, const char* status, const char* contentType,
                 const std::string& body,
                 const std::map<std::string, std::string>& extraHeaders = {});
// writeSimple for a (pointer, length) body: for frames whose buffer is
// capacity-backed storage (vector::size() != logical length; FrameData).
void writeSimpleBytes(int fd, const char* status, const char* contentType,
                      const uint8_t* body, size_t bodyLen,
                      const std::map<std::string, std::string>& extraHeaders = {});

} // namespace vftstream
