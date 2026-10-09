// RommPS app: the client for the RomM Sync payload's JSON API.
//
// The app only draws; the payload (src/ in this repository) syncs, downloads
// and talks to RomM, and serves everything the app needs on
// http://127.0.0.1:<web_port>/api/... (src/web.c). Requests run on one worker
// thread, so a slow answer never holds up a frame: the screen submits a
// request with a callback and calls poll() once per frame, which runs the
// callbacks of the requests that finished, on the screen's own thread.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

struct cJSON;

namespace rommps {

struct Response {
    int status = 0;     // HTTP status, 0 if the payload didn't answer
    std::string body;   // the raw body (JSON, or image bytes for /cover)
    std::string error;  // why there's no answer, when status is 0
    bool ok() const { return status >= 200 && status < 300; }
    // The body parsed as JSON, or nullptr. The caller frees it (cJSON_Delete).
    cJSON *json() const;
    // The "error" field of a JSON error body, or the transport error.
    std::string message() const;
};

// One blocking HTTP/1.0 exchange with the payload. Used by the worker, and
// directly where blocking is fine (start-up, tests).
Response http_request(const std::string &host, int port, const std::string &method, const std::string &path,
                      const std::string &body = "", int timeout_ms = 15000);

class Api {
  public:
    using Callback = std::function<void(const Response &)>;

    explicit Api(std::string host = "127.0.0.1", int port = 8780);
    ~Api();
    Api(const Api &) = delete;
    Api &operator=(const Api &) = delete;

    void get(const std::string &path, Callback done);
    void post(const std::string &path, const std::string &json_body, Callback done);
    // Runs the callbacks of finished requests. Call once per frame.
    void poll();
    // Requests sent and not yet delivered by poll().
    int pending() const;

  private:
    struct Job {
        std::string method, path, body;
        Callback done;
        Response result;
    };
    void worker();

    std::string host_;
    int port_;
    mutable std::mutex lock_;
    std::condition_variable wake_;
    std::deque<Job> queue_, finished_;
    int in_flight_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

}  // namespace rommps
