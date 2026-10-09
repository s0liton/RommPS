// RommPS: what the screen shows, kept up to date from the RomM Sync payload.
// See rommps_app.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rommps_app.hpp"

#include "cJSON.h"
#include "stb_image.h" // implemented by base/VulkanglTFModel.cpp

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <cctype>
#include <cstdio>
#include <memory>

namespace rommps
{

namespace
{

constexpr float kStatusEvery = 3.0f;
constexpr float kDownloadsEvery = 1.0f;     // while one is running
constexpr float kDownloadsIdleEvery = 5.0f; // otherwise
constexpr int kDecodesPerFrame = 2;         // covers turned into textures each frame
constexpr std::size_t kCoverTextures = 360; // covers kept as textures (about 130 MB); the
                                            // least recently drawn go, and come back from
                                            // the payload's cache when drawn again
constexpr std::size_t kListsKept = 24; // game lists kept at once: every tile on the platforms page, and more

void *g_renderer = nullptr;

// -1, 0 or 1 as a is older, the same as or newer than b ("1.2.3"); one that
// isn't a version ("dev", "") counts as 0.0.0.
int version_cmp(const std::string &a, const std::string &b)
{
    int x[3] = {}, y[3] = {};
    std::sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    std::sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    for (int i = 0; i < 3; ++i)
        if (x[i] != y[i])
            return x[i] < y[i] ? -1 : 1;
    return 0;
}

std::string url_escape(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

const char *str(const cJSON *o, const char *key, const char *fallback = "")
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : fallback;
}

double num(const cJSON *o, const char *key, double fallback = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// The kit's fonts are baked for ASCII: accented letters fold to their plain
// ones ("Pok\xC3\xA9mon" reads "Pokemon"), typographic quotes and dashes to
// their ASCII forms, and anything else to "?".
std::string plain(const char *text)
{
    static const char *const kLatin1 = // U+00C0 to U+00FF
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    std::string out;
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p)
    {
        unsigned cp = *p++;
        int more = cp >= 0xF0 ? 3 : cp >= 0xE0 ? 2 : cp >= 0xC0 ? 1 : 0;
        if (more)
            cp &= 0x3F >> more;
        while (more-- > 0 && (*p & 0xC0) == 0x80)
            cp = (cp << 6) | (*p++ & 0x3F);
        if (cp < 0x80)
            out += static_cast<char>(cp);
        else if (cp >= 0xC0 && cp <= 0xFF)
            out += kLatin1[cp - 0xC0];
        else if (cp == 0x2018 || cp == 0x2019)
            out += '\'';
        else if (cp == 0x201C || cp == 0x201D)
            out += '"';
        else if (cp == 0x2013 || cp == 0x2014)
            out += '-';
        else if (cp == 0x2026)
            out += "...";
        else if (cp == 0xA0)
            out += ' ';
        else if (cp == 0xB7)
            out += "\xC2\xB7"; // the middle dot is in the fonts: the app uses it too
        else
            out += '?';
    }
    return out;
}

bool flag(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

// A cover's colours: its average as the body, a darker tone, and its most
// saturated region's colour as the accent, lifted so it reads on dark glass.
Palette palette_of(const unsigned char *rgba, int w, int h)
{
    double sum[3] = {0, 0, 0}, sat_sum[3] = {0, 0, 0}, sat_weight = 0;
    const int step = std::max(1, (w * h) / 4096);
    int n = 0;
    for (int i = 0; i < w * h; i += step)
    {
        const unsigned char *p = rgba + static_cast<std::size_t>(i) * 4;
        const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        const double sat = mx > 0 ? static_cast<double>(mx - mn) / mx : 0.0;
        for (int c = 0; c < 3; ++c)
        {
            sum[c] += p[c];
            sat_sum[c] += p[c] * sat * sat;
        }
        sat_weight += sat * sat;
        ++n;
    }
    if (n == 0)
        return {};
    auto rgb = [](double r, double g, double b) {
        auto c = [](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0)); };
        return Color::rgb((c(r) << 16) | (c(g) << 8) | c(b));
    };
    Palette out;
    out.mid = rgb(sum[0] / n, sum[1] / n, sum[2] / n);
    out.dark = rgb(sum[0] / n * 0.35, sum[1] / n * 0.35, sum[2] / n * 0.35);
    if (sat_weight > 0.5)
    {
        double a[3] = {sat_sum[0] / sat_weight, sat_sum[1] / sat_weight, sat_sum[2] / sat_weight};
        const double peak = std::max({a[0], a[1], a[2], 1.0});
        const double lift = 230.0 / peak; // as bright as it gets, same hue
        out.accent = rgb(a[0] * lift, a[1] * lift, a[2] * lift);
    }
    return out;
}

} // namespace

App &app()
{
    static App instance;
    return instance;
}

App::App() : api_("127.0.0.1", payload_port())
{
    refresh_status();
}

void App::say(const std::string &message)
{
    toast_ = message;
    toast_age_ = 0;
}

void App::tick(float dt, hui::gfx::Renderer *renderer)
{
    clock_ += dt;
    api_.poll();
    for (Api &a : covers_api_)
        a.poll();
    for (Api &a : pages_api_)
        a.poll();
    // The pages the screen wanted last frame, in the order it asked. One it
    // stopped wanting before a worker was free is never sent.
    for (const auto &[key, page] : wanted_pages_)
        if (pages_in_flight_ < kPageWorkers)
            send_page(key, page);
    wanted_pages_.clear();
    // The covers drawn last frame and not asked for yet, in the order they
    // were drawn; covers scrolled away from are never asked for.
    for (const std::string &path : wanted_)
    {
        if (covers_in_flight_ >= kCoversInFlight)
            break;
        Cover &c = covers_[path];
        if (c.requested)
            continue;
        c.requested = true;
        ++covers_in_flight_;
        Api &worker = covers_api_[next_cover_worker_++ % kCoverWorkers];
        worker.get("/cover?p=" + url_escape(path), [this, path](const Response &r) {
            --covers_in_flight_;
            if (r.ok() && !r.body.empty())
                to_decode_.push_back({path, r.body});
            else
                covers_[path].failed = true;
        });
    }
    wanted_.clear();
    toast_age_ += dt;
    if (!toast_.empty() && toast_age_ > 4.0f)
        toast_.clear();

    status_timer_ += dt;
    if (status_timer_ >= kStatusEvery && !status_busy_)
        refresh_status();
    update_timer_ += dt;
    if (update_.busy() && !update_busy_ && update_timer_ >= 1.0f)
        refresh_update();
    downloads_timer_ += dt;
    if (status_.paired && !downloads_busy_ &&
        downloads_timer_ >= (active_downloads_ ? kDownloadsEvery : kDownloadsIdleEvery))
        refresh_downloads();

    if (renderer)
        evict_covers(renderer);
    // The payload sent: its API answers within a few seconds.
    if (start_state_ == 2)
    {
        start_state_ = 0;
        status_timer_ = kStatusEvery;
    }
    else if (start_state_ == 3)
    {
        start_state_ = 0;
        say("Couldn't start RomM Sync: " + start_error_);
    }

    // A few covers a frame become textures; the rest wait their turn, so a
    // shelf full of new covers never costs a frame.
    for (int i = 0; i < kDecodesPerFrame && !to_decode_.empty(); ++i)
    {
        Pending job = std::move(to_decode_.front());
        to_decode_.pop_front();
        Cover &cover = covers_[job.path];
        int w = 0, h = 0, channels = 0;
        unsigned char *rgba = stbi_load_from_memory(reinterpret_cast<const unsigned char *>(job.bytes.data()),
                                                    static_cast<int>(job.bytes.size()), &w, &h, &channels, 4);
        if (!rgba)
        {
            cover.failed = true;
            continue;
        }
        cover.palette = palette_of(rgba, w, h);
        if (renderer)
            cover.texture = renderer->create_texture(w, h, rgba);
        stbi_image_free(rgba);
    }
}

void App::evict_covers(hui::gfx::Renderer *renderer)
{
    std::size_t loaded = 0;
    for (const auto &[path, c] : covers_)
        loaded += c.texture != 0;
    if (loaded <= kCoverTextures)
        return;
    // Only covers not drawn for a few seconds, so no list being drawn still has one.
    std::vector<std::pair<double, const std::string *>> old;
    for (const auto &[path, c] : covers_)
        if (c.texture && c.used < clock_ - 3.0)
            old.push_back({c.used, &path});
    std::sort(old.begin(), old.end());
    std::vector<std::string> gone;
    for (std::size_t i = 0; i < old.size() && loaded - gone.size() > kCoverTextures * 3 / 4; ++i)
        gone.push_back(*old[i].second);
    for (const std::string &path : gone)
    {
        renderer->destroy_texture(covers_[path].texture);
        covers_.erase(path);
    }
}

const std::string &App::bundled_payload()
{
    if (!bundled_read_)
    {
        bundled_read_ = true;
        std::ifstream f("/app0/assets/rommps/romm-sync.elf", std::ios::binary);
        bundled_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return bundled_;
}

// Both HENs load ELF payloads sent to port 9021; RommPS reaches it on the
// console itself, the way it reaches the payload's API.
void App::start_payload()
{
    if (start_state_ == 1)
        return;
    const std::string &elf = bundled_payload();
    if (elf.size() < 4 || elf.compare(0, 4, "\x7f" "ELF") != 0)
    {
        start_error_ = "this RommPS doesn't carry the RomM Sync payload";
        start_state_ = 3;
        return;
    }
    start_error_.clear();
    start_state_ = 1;
    std::thread([this, elf] {
        std::string err;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(9021);
        ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0)
            err = std::string("the HEN's payload loader (port 9021) didn't answer: ") + std::strerror(errno);
        for (std::size_t sent = 0; err.empty() && sent < elf.size();)
        {
            const ssize_t n = ::send(fd, elf.data() + sent, elf.size() - sent, 0);
            if (n <= 0)
                err = std::string("sending the payload failed: ") + std::strerror(errno);
            else
                sent += static_cast<std::size_t>(n);
        }
        if (fd >= 0)
            ::close(fd);
        start_error_ = err;
        start_state_ = err.empty() ? 2 : 3;
    }).detach();
}

void App::repair_autostart()
{
    api_.get("/api/autostart", [this](const Response &r) {
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!r.ok() || !j || !flag(j.get(), "supported"))
            return;
        // Setup used to leave autostart off unless it was turned on, so most
        // people never did. It's switched on once for anyone who never chose
        // either way; after that their choice stands.
        if (!flag(j.get(), "enabled") && cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j.get(), "chosen")) &&
            status_.setup_complete)
        {
            install_autostart([this](const Response &a) {
                if (a.ok())
                    say("RomM Sync now starts with the console. You can turn this off in Set up again.");
            });
            return;
        }
        bool old = false;
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j.get(), "copies"))
            old |= version_cmp(str(c, "version"), kMinPayload) < 0;
        if (!old)
            return;
        api_.post("/api/autostart/install", bundled_payload(), [this](const Response &a) {
            if (!a.ok())
                say("Couldn't update RomM Sync's autostart: " + a.message());
        });
    });
}

void App::install_autostart(Api::Callback done)
{
    api_.post("/api/autostart/upload", bundled_payload(), [this, done](const Response &r) {
        if (!r.ok())
            say("Couldn't set up the autostart: " + r.message());
        if (done)
            done(r);
    });
}

// "1.0.2" < "1.1.0", number by number. A build without a version ("dev")
// counts as new enough.
bool App::payload_too_old() const
{
    const std::string &v = status_.version;
    if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0])))
        return false;
    int have[3] = {}, want[3] = {};
    std::sscanf(v.c_str(), "%d.%d.%d", &have[0], &have[1], &have[2]);
    std::sscanf(kMinPayload, "%d.%d.%d", &want[0], &want[1], &want[2]);
    for (int i = 0; i < 3; ++i)
        if (have[i] != want[i])
            return have[i] < want[i];
    return false;
}

// RomM Sync holds a lock on this file for as long as it runs (1.1.1 on); a
// copy can be running and not answering yet, after a start or a wake.
int App::payload_lock()
{
    const int fd = ::open("/data/romm-sync/romm-sync.lock", O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    int held = 0;
    if (::flock(fd, LOCK_SH | LOCK_NB) != 0)
        held = errno == EWOULDBLOCK ? 1 : -1;
    ::close(fd);
    return held;
}

void App::refresh_status()
{
    status_timer_ = 0;
    status_busy_ = true;
    api_.get("/api/status", [this](const Response &r) {
        status_busy_ = false;
        status_.known = true;
        status_.reachable = r.status != 0;
        // Not answering: RommPS starts the payload it carries, once by itself
        // (Cross on the screen tries again), but only when none is running.
        // One that's running is left to come up; with no way to tell, it
        // waits a few tries first.
        unanswered_ = status_.reachable ? 0 : unanswered_ + 1;
        payload_waiting_ = false;
        if (!status_.reachable && !tried_start_)
        {
            const int lock = payload_lock();
            if (lock == 1)
                payload_waiting_ = true;
            else if (lock == 0 || unanswered_ >= 3)
            {
                tried_start_ = true;
                start_payload();
            }
        }
        if (!r.ok())
        {
            status_.error = r.message();
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        status_.error.clear();
        status_.version = str(j.get(), "version");
        // An older payload is replaced by the one RommPS carries once it's
        // idle: the new one takes over from it and then, being this
        // RommPS's own, updates the copies on disk (below). A newer one is
        // left alone.
        const bool idle = !flag(cJSON_GetObjectItemCaseSensitive(j.get(), "sync"), "running") && active_downloads_ == 0;
        if (payload_too_old() && !updated_payload_ && idle)
        {
            updated_payload_ = true;
            say("Updating RomM Sync to " + std::string(kMinPayload));
            start_payload();
        }
        else if (!repaired_ && version_cmp(status_.version, kMinPayload) == 0)
        {
            repaired_ = true;
            repair_autostart();
        }
        status_.ip = str(j.get(), "ip");
        status_.port = static_cast<int>(num(j.get(), "web_port", 8780));
        status_.paired = flag(j.get(), "paired");
        status_.setup_complete = flag(j.get(), "setup_complete");
        status_.server = str(j.get(), "server_url");
        status_.user = str(j.get(), "username");
        status_.uptime = num(j.get(), "uptime");
        const cJSON *sync = cJSON_GetObjectItemCaseSensitive(j.get(), "sync");
        status_.syncing = flag(sync, "running");
        status_.phase = str(sync, "phase");
        status_.last_run = num(sync, "last_run");
        status_.sync_count = static_cast<int>(num(sync, "sync_count"));
        status_.tracked_saves = static_cast<int>(num(sync, "tracked_saves"));
        status_.conflicts = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sync, "conflicts"));
        // The payload adds each sync at the end of its history: the last one is the newest.
        const cJSON *history = cJSON_GetObjectItemCaseSensitive(sync, "history");
        const cJSON *last = cJSON_GetArrayItem(history, cJSON_GetArraySize(history) - 1);
        status_.last_result = last ? (str(last, "error")[0] ? str(last, "error") : "OK") : "";
        status_.last_up = static_cast<int>(num(last, "uploaded"));
        status_.last_down = static_cast<int>(num(last, "downloaded"));
        status_.server_version = str(j.get(), "server_version");
        const cJSON *covers = cJSON_GetObjectItemCaseSensitive(j.get(), "covers");
        status_.covers_state = str(covers, "state");
        status_.covers_done = static_cast<int>(num(covers, "done"));
        status_.covers_total = static_cast<int>(num(covers, "total"));
        const cJSON *pairing = cJSON_GetObjectItemCaseSensitive(j.get(), "pairing");
        if (status_.pairing == "starting")
            pairing = nullptr; // the answer to /api/pair/start says how it went
        else
            status_.pairing = str(pairing, "status");
        if (pairing)
        {
            status_.pairing_message = str(pairing, "message");
            status_.user_code = str(pairing, "user_code");
            status_.verification_url = str(pairing, "verification_url");
        }
        if (!config_.known)
            refresh_config();
        const bool ready = status_.paired && status_.setup_complete;
        if (ready && (!was_paired_ || platforms_.empty()) && !platforms_busy_)
        {
            refresh_platforms();
            refresh_installed();
        }
        was_paired_ = ready;
    });
}

void App::refresh_platforms()
{
    platforms_busy_ = true;
    api_.get("/api/platforms", [this](const Response &r) {
        platforms_busy_ = false;
        if (!r.ok())
        {
            say("Couldn't load your platforms: " + r.message());
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Platform> list;
        const cJSON *p;
        cJSON_ArrayForEach(p, j.get())
        {
            Platform plat;
            plat.id = static_cast<int>(num(p, "id"));
            plat.name = plain(str(p, "name"));
            plat.slug = str(p, "slug");
            plat.profile = str(p, "profile");
            plat.rom_count = static_cast<int>(num(p, "rom_count"));
            if (plat.rom_count > 0)
                list.push_back(std::move(plat));
        }
        // Platforms with an emulator first, then by name.
        std::stable_sort(list.begin(), list.end(), [](const Platform &a, const Platform &b) {
            if (a.profile.empty() != b.profile.empty())
                return !a.profile.empty();
            return a.name < b.name;
        });
        platforms_ = std::move(list);
    });
}

const Platform *App::platform_by_id(int id) const
{
    for (const Platform &p : platforms_)
        if (p.id == id)
            return &p;
    return nullptr;
}

GameList &App::games(int platform_id, const std::string &search)
{
    const std::string key = std::to_string(platform_id) + "|" + search;
    CachedList &entry = lists_[key];
    if (!entry.list)
    {
        entry.list = std::make_unique<GameList>();
        entry.list->platform_id = platform_id;
        entry.list->search = search;
        // Keep a few lists; the oldest goes when there are more.
        if (lists_.size() > kListsKept)
        {
            auto oldest = lists_.end();
            for (auto it = lists_.begin(); it != lists_.end(); ++it)
                if (it->first != key && (oldest == lists_.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest != lists_.end())
                lists_.erase(oldest);
        }
    }
    CachedList &kept = lists_[key];
    kept.used = clock_;
    return *kept.list;
}

void App::ensure(GameList &list, int first, int last)
{
    if (!list.ready())
        last = 0; // the first page says how many there are
    else
        last = list.raw(std::min(last, list.total - 1));
    first = list.ready() ? list.raw(std::max(first, 0)) : 0;
    for (int page = first / GameList::kPage; page <= last / GameList::kPage && last >= 0; ++page)
    {
        if (static_cast<int>(list.pages.size()) <= page)
            list.pages.resize(static_cast<std::size_t>(page) + 1, 0);
        if (list.pages[static_cast<std::size_t>(page)])
            continue;
        const std::string key = std::to_string(list.platform_id) + "|" + list.search;
        const std::pair<std::string, int> want{key, page};
        if (std::find(wanted_pages_.begin(), wanted_pages_.end(), want) == wanted_pages_.end())
            wanted_pages_.push_back(want);
    }
}

void App::send_page(const std::string &key, int page)
{
    auto found = lists_.find(key);
    if (found == lists_.end() || !found->second.list)
        return;
    GameList &list = *found->second.list;
    if (static_cast<int>(list.pages.size()) <= page || list.pages[static_cast<std::size_t>(page)])
        return;
    list.pages[static_cast<std::size_t>(page)] = 1;
    ++pages_in_flight_;
    {
        char path[512];
        std::snprintf(path, sizeof path, "/api/roms?platform_id=%d&offset=%d&limit=%d&search=%s", list.platform_id,
                      page * GameList::kPage, GameList::kPage, url_escape(list.search).c_str());
        pages_api_[next_page_worker_++ % kPageWorkers].get(path, [this, key, page](const Response &r) {
            --pages_in_flight_;
            auto it = lists_.find(key);
            if (it == lists_.end() || !it->second.list)
                return; // let go of meanwhile
            GameList &l = *it->second.list;
            if (!r.ok())
            {
                l.error = r.message();
                if (static_cast<int>(l.pages.size()) > page)
                    l.pages[static_cast<std::size_t>(page)] = 0; // asked again when looked at
                return;
            }
            std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
            l.error.clear();
            const int total = static_cast<int>(num(j.get(), "total"));
            if (!l.ready() || total != static_cast<int>(l.games.size()))
            {
                l.games.resize(static_cast<std::size_t>(total));
                l.total = total; // until reindex() below takes out what isn't a game
                l.pages.resize(static_cast<std::size_t>((total + GameList::kPage - 1) / GameList::kPage), 0);
                if (page < static_cast<int>(l.pages.size()))
                    l.pages[static_cast<std::size_t>(page)] = 1;
            }
            if (l.raw_letters.empty())
            {
                const cJSON *ci = cJSON_GetObjectItemCaseSensitive(j.get(), "char_index"), *c;
                cJSON_ArrayForEach(c, ci) if (cJSON_IsNumber(c)) l.raw_letters.push_back({c->string, c->valueint});
                std::sort(l.raw_letters.begin(), l.raw_letters.end(),
                          [](const auto &a, const auto &b) { return a.second < b.second; });
            }
            int index = page * GameList::kPage;
            const cJSON *g;
            cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(j.get(), "items"))
            {
                if (index >= total)
                    break;
                Game &game = l.games[static_cast<std::size_t>(index++)];
                game.id = static_cast<int>(num(g, "id"));
                game.platform_id = static_cast<int>(num(g, "platform_id", l.platform_id));
                game.name = plain(str(g, "name"));
                game.fs_name = str(g, "fs_name");
                game.cover = str(g, "cover");
                game.size = num(g, "size");
                game.installed = flag(g, "installed");
                game.multi = flag(g, "multi");
                game.not_game = flag(g, "not_game");
                game.version = plain(str(g, "version"));
                game.versions = static_cast<int>(num(g, "versions"));
            }
            l.reindex();
            if (page < static_cast<int>(l.pages.size()))
                l.pages[static_cast<std::size_t>(page)] = 2;
        });
    }
}

const Details &App::details(int rom_id)
{
    auto found = details_.find(rom_id);
    if (found != details_.end())
        return found->second;
    Details &d = details_[rom_id];
    char path[64];
    std::snprintf(path, sizeof path, "/api/roms/%d/details", rom_id);
    api_.get(path, [this, rom_id](const Response &r) {
        Details &out = details_[rom_id];
        out.loaded = true;
        if (!r.ok())
        {
            out.failed = true;
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        out.year = static_cast<int>(num(j.get(), "year"));
        out.publishers = plain(str(j.get(), "publishers"));
        out.developers = plain(str(j.get(), "developers"));
        out.genres = plain(str(j.get(), "genres"));
        out.players = plain(str(j.get(), "players"));
        out.summary = plain(str(j.get(), "summary"));
        out.status = str(j.get(), "status");
        out.last_played = str(j.get(), "last_played");
        out.rating = num(j.get(), "rating");
        out.main_story_s = num(j.get(), "main_story_s");
        out.play_ms = num(j.get(), "play_ms");
        out.sessions = static_cast<int>(num(j.get(), "sessions"));
        out.completion = static_cast<int>(num(j.get(), "completion"));
        out.my_rating = static_cast<int>(num(j.get(), "my_rating"));
    });
    return d;
}

void App::refresh_installed()
{
    api_.get("/api/installed?limit=30", [this](const Response &r) {
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Game> list;
        const cJSON *g;
        cJSON_ArrayForEach(g, j.get())
        {
            Game game;
            game.id = static_cast<int>(num(g, "id"));
            game.platform_id = static_cast<int>(num(g, "platform_id"));
            game.name = plain(str(g, "name"));
            game.fs_name = str(g, "fs_name");
            game.cover = str(g, "cover");
            game.installed = true;
            list.push_back(std::move(game));
        }
        installed_ = std::move(list);
    });
}

const Cover &App::cover(const std::string &path)
{
    static const Cover kNone;
    if (path.empty())
        return kNone;
    Cover &c = covers_[path];
    c.used = clock_;
    // Asked for at the next tick, if it's still on screen then.
    if (!c.requested && wanted_.size() < 64)
        wanted_.push_back(path);
    return c;
}

void App::refresh_downloads()
{
    downloads_timer_ = 0;
    downloads_busy_ = true;
    api_.get("/api/downloads", [this](const Response &r) {
        downloads_busy_ = false;
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Download> list;
        int active = 0;
        bool finished_one = false;
        const cJSON *d;
        cJSON_ArrayForEach(d, j.get())
        {
            Download dl;
            dl.id = static_cast<int>(num(d, "id"));
            dl.rom_id = static_cast<int>(num(d, "rom_id"));
            dl.name = plain(str(d, "name"));
            dl.platform = plain(str(d, "platform"));
            dl.status = str(d, "status");
            dl.error = str(d, "error");
            dl.done = num(d, "done");
            dl.total = num(d, "total");
            dl.speed = num(d, "speed");
            active += dl.active();
            // A game that just finished shows as downloaded on its shelf.
            for (const Download &old : downloads_)
                if (old.id == dl.id && old.active() && dl.status == "done")
                    finished_one = true;
            list.push_back(std::move(dl));
        }
        downloads_ = std::move(list);
        active_downloads_ = active;
        // A game that finished shows as downloaded: the lists load again.
        if (finished_one)
        {
            lists_.clear();
            refresh_installed();
        }
    });
}

const Download *App::download_for(int rom_id) const
{
    const Download *found = nullptr;
    for (const Download &d : downloads_)
        if (d.rom_id == rom_id)
            found = &d; // the newest one wins
    return found;
}

void App::download(int rom_id, const std::string &name)
{
    // The name goes along, so the download shows it from the start.
    std::unique_ptr<cJSON, void (*)(cJSON *)> j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddNumberToObject(j.get(), "rom_id", rom_id);
    cJSON_AddStringToObject(j.get(), "name", name.c_str());
    char *text = cJSON_PrintUnformatted(j.get());
    const std::string body = text ? text : "{}";
    cJSON_free(text);
    api_.post("/api/download", body, [this](const Response &r) {
        if (!r.ok())
            say("Couldn't start the download: " + r.message());
        refresh_downloads();
    });
}

void App::clear_finished_downloads()
{
    api_.post("/api/downloads/clear", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't clear the list: " + r.message());
        refresh_downloads();
    });
}

void App::cancel_download(int job_id)
{
    char body[64];
    std::snprintf(body, sizeof body, "{\"id\":%d}", job_id);
    api_.post("/api/downloads/cancel", body, [this](const Response &r) {
        if (!r.ok())
            say("Couldn't cancel: " + r.message());
        refresh_downloads();
    });
}

void App::sync_now()
{
    api_.post("/api/sync", "{}", [this](const Response &r) {
        say(r.ok() ? "Syncing" : "Couldn't start a sync: " + r.message());
        refresh_status();
    });
}

void App::refresh_config()
{
    api_.get("/api/config", [this](const Response &r) {
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        Config c;
        c.known = true;
        c.server_url = str(j.get(), "server_url");
        c.tls_verify = flag(j.get(), "tls_verify");
        c.sync_interval_min = static_cast<int>(num(j.get(), "sync_interval_min"));
        c.sync_on_game_exit = flag(j.get(), "sync_on_game_exit");
        c.sync_on_game_start = flag(j.get(), "sync_on_game_start");
        c.sync_on_change = flag(j.get(), "sync_on_change");
        c.notify = flag(j.get(), "notify");
        c.update_check = flag(j.get(), "update_check");
        c.states = str(j.get(), "states", "off");
        c.conflict_policy = str(j.get(), "conflict_policy", "ask");
        c.ps2_cards = str(j.get(), "ps2_cards", "per_game");
        c.cover_cache = flag(j.get(), "cover_cache");
        c.download_concurrency = static_cast<int>(num(j.get(), "download_concurrency", 1));
        config_ = c;
    });
}

void App::set_config(const std::string &fields, const std::function<void(Config &)> &apply)
{
    if (apply)
        apply(config_);
    api_.post("/api/config", "{" + fields + "}", [this](const Response &r) {
        if (!r.ok())
        {
            say("Couldn't save that: " + r.message());
            refresh_config();
        }
    });
}

void App::refresh_update()
{
    update_timer_ = 0;
    update_busy_ = true;
    api_.get("/api/update", [this](const Response &r) {
        update_busy_ = false;
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        const bool was_busy = update_.busy();
        Update u;
        u.known = true;
        u.state = str(j.get(), "state");
        u.message = str(j.get(), "message");
        u.error = str(j.get(), "error");
        u.current = str(j.get(), "current");
        u.latest = str(j.get(), "latest");
        u.available = flag(j.get(), "available");
        u.autostart = flag(j.get(), "autostart");
        // Payloads before 1.1.1 don't say: they update themselves.
        const cJSON *su = cJSON_GetObjectItemCaseSensitive(j.get(), "self_update");
        u.self_update = !cJSON_IsBool(su) || cJSON_IsTrue(su);
        u.done = num(j.get(), "done");
        u.total = num(j.get(), "total");
        update_ = u;
        if (was_busy && !u.busy())
        {
            if (u.state == "error")
                say("Update: " + (u.error.empty() ? u.message : u.error));
            else if (u.available)
                say(u.self_update ? "RomM Sync " + u.latest + " is available"
                                  : "RommPS " + u.latest + " is available in ProsperoStore");
            else if (u.state == "idle")
                say("RomM Sync is up to date");
            else if (!u.message.empty())
                say(u.message);
        }
    });
}

void App::check_update()
{
    api_.post("/api/update/check", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't look for updates: " + r.message());
        update_.state = "checking";
        refresh_update();
    });
}

void App::install_update()
{
    api_.post("/api/update/install", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't install the update: " + r.message());
        refresh_update();
    });
}

void App::pair_start(const std::string &server_url, bool tls_verify)
{
    std::unique_ptr<cJSON, void (*)(cJSON *)> body(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(body.get(), "server_url", server_url.c_str());
    char *text = cJSON_PrintUnformatted(body.get());
    const std::string json = text ? text : "{}";
    cJSON_free(text);
    status_.pairing = "starting";
    status_.pairing_message.clear();
    // The certificate setting has to be in place before the server is first contacted.
    set_config(tls_verify ? "\"tls_verify\":true" : "\"tls_verify\":false",
               [tls_verify](Config &c) { c.tls_verify = tls_verify; });
    api_.post("/api/pair/start", json, [this](const Response &r) {
        if (!r.ok())
        {
            status_.pairing = "error";
            status_.pairing_message = r.message();
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        status_.pairing = str(j.get(), "status");
        status_.pairing_message = str(j.get(), "message");
        status_.user_code = str(j.get(), "user_code");
        status_.verification_url = str(j.get(), "verification_url");
        config_.known = false; // the server's address changed
    });
}

void App::pair_forget()
{
    api_.post("/api/pair/forget", "{}", [this](const Response &) {
        platforms_.clear();
        config_.known = false;
        refresh_status();
    });
}

void App::setup_restart()
{
    api_.post("/api/setup/restart", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't restart the setup: " + r.message());
        refresh_status();
    });
}

} // namespace rommps

extern "C" void rommps_set_renderer(void *renderer)
{
    rommps::g_renderer = renderer;
}

extern "C" void *rommps_renderer()
{
    return rommps::g_renderer;
}
