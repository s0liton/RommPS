// RommPS app: the client for the RomM Sync payload's JSON API (rommps_api.hpp).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rommps_api.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"

namespace rommps {

cJSON *Response::json() const { return body.empty() ? nullptr : cJSON_ParseWithLength(body.data(), body.size()); }

std::string Response::message() const {
    if (status == 0) return error.empty() ? "RomM Sync isn't answering" : error;
    std::string out;
    if (cJSON *j = json()) {
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(j, "error");
        if (cJSON_IsString(e)) out = e->valuestring;
        cJSON_Delete(j);
    }
    return out.empty() ? "HTTP " + std::to_string(status) : out;
}

static bool send_all(int fd, const char *p, size_t n) {
    while (n) {
        ssize_t w = ::send(fd, p, n, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

Response http_request(const std::string &host, int port, const std::string &method, const std::string &path,
                      const std::string &body, int timeout_ms) {
    Response r;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        r.error = std::string("socket: ") + std::strerror(errno);
        return r;
    }
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (::inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1 || ::connect(fd, (sockaddr *)&sa, sizeof sa) != 0) {
        r.error = "RomM Sync isn't running (" + std::string(std::strerror(errno)) + ")";
        ::close(fd);
        return r;
    }
    char head[1024];
    int hn = std::snprintf(head, sizeof head,
                           "%s %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n"
                           "Content-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                           method.c_str(), path.c_str(), host.c_str(), body.size());
    if (hn <= 0 || hn >= (int)sizeof head || !send_all(fd, head, (size_t)hn) || !send_all(fd, body.data(), body.size())) {
        r.error = "couldn't send the request";
        ::close(fd);
        return r;
    }
    std::string in;
    char buf[16384];
    for (;;) {
        ssize_t n = ::recv(fd, buf, sizeof buf, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) {
            r.error = errno == EAGAIN || errno == EWOULDBLOCK ? "RomM Sync took too long to answer" : std::strerror(errno);
            ::close(fd);
            return r;
        }
        if (n == 0) break;
        in.append(buf, (size_t)n);
    }
    ::close(fd);
    size_t split = in.find("\r\n\r\n");
    int status = 0;
    if (split == std::string::npos || std::sscanf(in.c_str(), "HTTP/%*d.%*d %d", &status) != 1) {
        r.error = "RomM Sync sent a reply the app can't read";
        return r;
    }
    r.status = status;
    r.body = in.substr(split + 4);
    return r;
}

int payload_port()
{
    static const int port = [] {
        int p = 8780;
        if (FILE *f = std::fopen("/data/romm-sync/config.json", "rb"))
        {
            char buf[8192];
            const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            buf[n] = 0;
            if (const char *k = std::strstr(buf, "\"web_port\""))
                if (const char *colon = std::strchr(k, ':'))
                {
                    const long v = std::strtol(colon + 1, nullptr, 10);
                    if (v > 0 && v < 65536)
                        p = static_cast<int>(v);
                }
        }
        return p;
    }();
    return port;
}

Api::Api(std::string host, int port) : host_(std::move(host)), port_(port), thread_(&Api::worker, this) {}

Api::~Api() {
    {
        std::lock_guard<std::mutex> g(lock_);
        stopping_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

void Api::get(const std::string &path, Callback done) {
    {
        std::lock_guard<std::mutex> g(lock_);
        queue_.push_back(Job{"GET", path, "", std::move(done), {}});
    }
    wake_.notify_one();
}

void Api::post(const std::string &path, const std::string &json_body, Callback done) {
    {
        std::lock_guard<std::mutex> g(lock_);
        queue_.push_back(Job{"POST", path, json_body.empty() ? "{}" : json_body, std::move(done), {}});
    }
    wake_.notify_one();
}

void Api::poll() {
    std::deque<Job> done;
    {
        std::lock_guard<std::mutex> g(lock_);
        done.swap(finished_);
    }
    for (Job &j : done)
        if (j.done) j.done(j.result);
}

int Api::pending() const {
    std::lock_guard<std::mutex> g(lock_);
    return (int)(queue_.size() + finished_.size()) + in_flight_;
}

void Api::worker() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> g(lock_);
            wake_.wait(g, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            in_flight_ = 1;
        }
        job.result = http_request(host_, port_, job.method, job.path, job.body);
        std::lock_guard<std::mutex> g(lock_);
        in_flight_ = 0;
        finished_.push_back(std::move(job));
    }
}

}  // namespace rommps
