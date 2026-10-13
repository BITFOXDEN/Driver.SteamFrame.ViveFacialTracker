// http_server.cpp - see http_server.hpp
#include "http_server.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>

namespace vftstream {

namespace {
constexpr int kMaxHeaderBytes = 8 * 1024;
constexpr int kMaxBodyBytes = 1 * 1024 * 1024;
// One or two acceptors are plenty for this load (each connection gets its
// own handler thread anyway).
constexpr int kWorkerCount = 2;
// Deadlines: a client that never finishes its request (or stops reading
// its stream) gets dropped so its thread and socket buffers are
// reclaimed; MJPEG clients reconnect automatically.
constexpr int kRecvTimeoutS = 60;
constexpr int kSendTimeoutS = 30;

struct SigBlock {
    sigset_t old;
    SigBlock() {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &set, &old);
    }
    ~SigBlock() { pthread_sigmask(SIG_SETMASK, &old, nullptr); }
};
} // namespace

HttpServer::HttpServer(int port, Handler handler)
    : port_(port), handler_(std::move(handler)) {}

HttpServer::~HttpServer() { stop(); }

bool HttpServer::start() {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return false;
    }
    if (listen(fd, 16) != 0) {
        ::close(fd);
        return false;
    }

    listenFd_ = fd;
    running_ = true;

    for (int i = 0; i < kWorkerCount; ++i)
        workers_.emplace_back([this] { acceptLoop(); });

    std::fprintf(stderr, "[http] listening on port %d (%d workers)\n", port_, kWorkerCount);
    return true;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;
    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    for (auto& t : workers_)
        if (t.joinable()) t.join();
    workers_.clear();

    // Acceptors are dead (joined above), so no new client can appear:
    // every registry entry is final. Wake each one first - the socket
    // shutdown fails a blocked send()/recv() immediately, and the owner's
    // FrameTopic::shutdown() (called before stop()) wakes waitFor() - then
    // join, so stop() returns only once no thread is inside the handler.
    std::list<std::unique_ptr<ClientEntry>> rest;
    {
        std::lock_guard<std::mutex> lk(clientsMu_);
        rest.splice(rest.end(), clients_);
    }
    for (auto& e : rest) {
        if (!e->done.load(std::memory_order_acquire))
            ::shutdown(e->fd, SHUT_RDWR); // may race a concurrent close:
                                          // EBADF at worst, harmless
        if (e->t.joinable()) e->t.join();
    }
}

void HttpServer::acceptLoop() {
    SigBlock sb;
    while (running_) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = accept4(listenFd_, reinterpret_cast<sockaddr*>(&peer),
                         &plen, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            if (errno == EBADF || errno == EINVAL) return;
            continue;
        }
        if (!running_.load(std::memory_order_acquire)) {
            ::close(fd); // raced stop(): drop it instead of serving it
            return;
        }

        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        timeval rtv{kRecvTimeoutS, 0};
        timeval stv{kSendTimeoutS, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &stv, sizeof(stv));

        spawnClient(fd);
    }
}

void HttpServer::spawnClient(int fd) {
    reapDoneClients(); // opportunistic: join+remove finished entries
    std::lock_guard<std::mutex> lk(clientsMu_);
    clients_.push_back(std::make_unique<ClientEntry>());
    ClientEntry& e = *clients_.back();
    e.fd = fd;
    e.t = std::thread([this, fd, self = &e] { clientMain(fd, self); });
}

void HttpServer::clientMain(int fd, ClientEntry* self) {
    handleClient(fd); // closes fd before returning
    // Last act under the lock: with done set, a reaper may remove the entry
    // and join this thread - the entry outlives the join, never the reverse.
    std::lock_guard<std::mutex> lk(clientsMu_);
    self->done = true;
}

void HttpServer::reapDoneClients() {
    std::vector<std::unique_ptr<ClientEntry>> dead;
    {
        std::lock_guard<std::mutex> lk(clientsMu_);
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->done.load(std::memory_order_acquire)) {
                dead.push_back(std::move(*it));
                it = clients_.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Joins happen outside the lock: each done thread is at its return
    // statement and never touches clientsMu_ again.
    for (auto& e : dead)
        if (e->t.joinable()) e->t.join();
}

void HttpServer::handleClient(int fd) {
    SigBlock sb;
    std::string header;
    header.reserve(1024);
    char buf[2048];

    while (header.find("\r\n\r\n") == std::string::npos &&
           header.size() < kMaxHeaderBytes) {
        ssize_t n;
        do {
            n = recv(fd, buf, sizeof(buf), 0);
        } while (n < 0 && errno == EINTR); // e.g. URB-completion signals
        if (n <= 0) { ::close(fd); return; }
        header.append(buf, static_cast<size_t>(n));
    }
    if (header.size() >= kMaxHeaderBytes) { ::close(fd); return; }

    size_t lineEnd = header.find("\r\n");
    if (lineEnd == std::string::npos) { ::close(fd); return; }
    std::string reqLine = header.substr(0, lineEnd);

    size_t sp1 = reqLine.find(' ');
    size_t sp2 = reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) { ::close(fd); return; }
    std::string method = reqLine.substr(0, sp1);
    std::string target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);

    if (target.find("..") != std::string::npos) { ::close(fd); return; }

    std::map<std::string, std::string> headers;
    size_t pos = lineEnd + 2;
    while (pos < header.size()) {
        size_t eol = header.find("\r\n", pos);
        if (eol == std::string::npos) eol = header.size();
        std::string line = header.substr(pos, eol - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            auto trimWS = [](std::string& s) {
                size_t a = s.find_first_not_of(" \t");
                size_t b = s.find_last_not_of(" \t");
                s = (a == std::string::npos) ? "" : s.substr(a, b - a + 1);
            };
            trimWS(k); trimWS(v);
            for (auto& ch : k) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
            headers[k] = v;
        }
        pos = eol + 2;
    }

    auto itCl = headers.find("content-length");
    std::string body;
    if (itCl != headers.end()) {
        long cl = atol(itCl->second.c_str());
        if (cl > 0 && cl < kMaxBodyBytes) {
            size_t have = header.size() > pos ? header.size() - pos : 0;
            size_t take = std::min((size_t)cl, have);
            body.assign(header.data() + pos, header.data() + pos + take);
            if (body.size() < (size_t)cl) {
                size_t more = (size_t)cl - body.size();
                size_t old = body.size();
                body.resize(old + more);
                ssize_t got = 0;
                do {
                    got = recv(fd, &body[old], more, 0);
                } while (got < 0 && errno == EINTR);
                if (got > 0) body.resize(old + (size_t)got);
            }
        }
    }

    if (handler_) handler_(fd, method, target, headers, body);
    ::close(fd);
}

bool sendAll(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, p + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue; // e.g. URB-completion signals
            return false;
        }
        if (n == 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
}

bool sendAllIov(int fd, const struct iovec* parts, int cnt) {
    size_t total = 0;
    for (int i = 0; i < cnt; ++i) total += parts[i].iov_len;

    size_t sent = 0;
    while (sent < total) {
        struct iovec v[8];
        int n = 0;
        size_t skip = sent; // bytes already sent within earlier segments
        for (int i = 0; i < cnt && n < 8; ++i) {
            if (parts[i].iov_len <= skip) {
                skip -= parts[i].iov_len;
                continue;
            }
            v[n].iov_base = static_cast<char*>(parts[i].iov_base) + skip;
            v[n].iov_len = parts[i].iov_len - skip;
            skip = 0;
            ++n;
        }
        if (n == 0) return false;
        ssize_t w = writev(fd, v, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        sent += static_cast<size_t>(w);
    }
    return true;
}

void writeSimple(int fd, const char* status, const char* contentType,
                 const std::vector<uint8_t>& body,
                 const std::map<std::string, std::string>& extraHeaders) {
    std::string h = std::string("HTTP/1.1 ") + status + "\r\n";
    h += "Content-Type: " + std::string(contentType) + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& [k, v] : extraHeaders) h += k + ": " + v + "\r\n";
    h += "\r\n";
    struct iovec iov[2] = {
        {h.data(), h.size()},
        {const_cast<uint8_t*>(body.data()), body.size()},
    };
    sendAllIov(fd, iov, 2);
}

void writeSimple(int fd, const char* status, const char* contentType,
                 const std::string& body,
                 const std::map<std::string, std::string>& extraHeaders) {
    std::string h = std::string("HTTP/1.1 ") + status + "\r\n";
    h += "Content-Type: " + std::string(contentType) + "\r\n";
    h += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    for (auto& [k, v] : extraHeaders) h += k + ": " + v + "\r\n";
    h += "\r\n";
    struct iovec iov[2] = {
        {h.data(), h.size()},
        {const_cast<char*>(body.data()), body.size()},
    };
    sendAllIov(fd, iov, 2);
}

void writeSimpleBytes(int fd, const char* status, const char* contentType,
                      const uint8_t* body, size_t bodyLen,
                      const std::map<std::string, std::string>& extraHeaders) {
    std::string h = std::string("HTTP/1.1 ") + status + "\r\n";
    h += "Content-Type: " + std::string(contentType) + "\r\n";
    h += "Content-Length: " + std::to_string(bodyLen) + "\r\n";
    for (auto& [k, v] : extraHeaders) h += k + ": " + v + "\r\n";
    h += "\r\n";
    struct iovec iov[2] = {
        {h.data(), h.size()},
        {const_cast<uint8_t*>(body), bodyLen},
    };
    sendAllIov(fd, iov, 2);
}

} // namespace vftstream
