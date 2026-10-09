// RommPS: what the screen shows, kept up to date from the RomM Sync payload.
//
// The payload does the work (sync, downloads, RomM) and serves its state on
// http://127.0.0.1:<port>/api (src/web.c in the repository). This keeps a
// copy for the screen: status, platforms, the games of the platforms looked
// at, downloads and covers, refreshed on timers through the API client
// (platform/ps5/app/src/rommps_api.hpp) so a frame never waits for it.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/draw_list.hpp"
#include "gfx/renderer.hpp"
#include "rommps_api.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace rommps
{

using hui::gfx::Color;

// Colours taken from a cover, for the backdrop and highlights.
struct Palette
{
    Color dark = Color::rgb(0x14182e);
    Color mid = Color::rgb(0x2b3466);
    Color accent = Color::rgb(0x76d6ff);
};

struct Cover
{
    std::uint32_t texture = 0; // 0 until it's loaded (or if there is none)
    Palette palette;
    bool requested = false;
    bool failed = false;
    double used = 0; // when it was last drawn
};

struct Game
{
    int id = 0;
    int platform_id = 0;
    std::string name, fs_name, cover; // cover: the RomM asset path, "" if none
    double size = 0;
    bool installed = false;
    bool multi = false;
    bool not_game = false; // a file RomM scanned beside the games (metadata.txt...): hidden
    std::string version;   // what sets this version apart, when RomM has others of the game
    int versions = 0;      // how many versions RomM has, this one included; 0 if just one
};

// More about a game (/api/roms/<id>/details): RomM's metadata, the user's own
// data and the play time RomM has recorded. Empty fields weren't known.
struct Details
{
    bool loaded = false, failed = false;
    int year = 0;
    std::string publishers, developers, genres, players, summary, status, last_played;
    double rating = 0;       // RomM's average, on its own scale (shown /10 or /100)
    double main_story_s = 0; // How Long to Beat, the main story
    double play_ms = 0;      // every device's sessions RomM has
    int sessions = 0, completion = 0, my_rating = 0;
};

struct Platform
{
    int id = 0;
    std::string name, slug, profile; // profile: the emulator, "" if none plays it
    int rom_count = 0;
};

// A list of games from RomM, A to Z: a platform's, a search's (in one
// platform, or every platform when platform_id is 0). Loaded a page at a time
// as it's looked at.
struct GameList
{
    static constexpr int kPage = 60;
    int platform_id = 0;
    std::string search;
    int total = -1;                   // games to show; unknown until the first page arrives
    std::vector<Game> games;          // RomM's whole list, by RomM's offset; id 0 isn't loaded yet
    std::vector<unsigned char> pages; // per page: 0 not asked, 1 asked, 2 here
    std::vector<std::pair<std::string, int>> letters; // first letter -> the index of its first game
    std::vector<std::pair<std::string, int>> raw_letters; // the same, by RomM's offset
    std::vector<int> hidden;          // RomM offsets of the files that aren't games, sorted
    std::string error;
    bool ready() const { return total >= 0; }
    // Positions in the list as shown, which skips what isn't a game, and
    // RomM's offsets. What isn't a game is known once its page is here.
    int raw(int index) const
    {
        for (int h : hidden)
            if (h <= index)
                ++index;
        return index;
    }
    int shown(int raw_index) const
    {
        int before = 0;
        for (int h : hidden)
            before += h < raw_index;
        return raw_index - before;
    }
    const Game *at(int index) const
    {
        if (index < 0 || index >= total)
            return nullptr;
        const int r = raw(index);
        return r < static_cast<int>(games.size()) && games[static_cast<std::size_t>(r)].id
                   ? &games[static_cast<std::size_t>(r)]
                   : nullptr;
    }
    void reindex() // after a page arrives
    {
        hidden.clear();
        for (std::size_t i = 0; i < games.size(); ++i)
            if (games[i].id && games[i].not_game)
                hidden.push_back(static_cast<int>(i));
        total = static_cast<int>(games.size() - hidden.size());
        letters.clear();
        for (const auto &[key, offset] : raw_letters)
            letters.push_back({key, shown(offset)});
    }
};

struct Download
{
    int id = 0, rom_id = 0;
    std::string name, platform, status, error;
    double done = 0, total = 0, speed = 0;
    float progress() const { return total > 0 ? static_cast<float>(done / total) : 0.0f; }
    // The payload's states: queued, downloading, cancelling, then done, error or cancelled.
    bool running() const { return status == "downloading"; }
    bool active() const { return status == "queued" || running() || status == "cancelling"; }
    bool finished() const { return !active(); }
};

struct Status
{
    bool known = false;     // an answer arrived at least once
    bool reachable = false; // the payload answered the last time
    bool paired = false, setup_complete = false;
    std::string version, server, user, ip, error;
    int port = 8780;
    int tracked_saves = 0, conflicts = 0, sync_count = 0;
    bool syncing = false;
    std::string phase;
    double last_run = 0;     // the payload's clock, seconds
    double uptime = 0;
    std::string last_result; // "OK", or the last sync's error
    int last_up = 0, last_down = 0;
    std::string server_version;
    // Every cover kept on the console (covers.h in the payload).
    std::string covers_state;
    int covers_done = 0, covers_total = 0;
    // Pairing in progress (/api/pair/start): "pending" while RomM waits for
    // the code to be approved, "error" when it failed, "" otherwise.
    std::string pairing, pairing_message, user_code, verification_url;
};

// The payload's settings that RommPS shows (/api/config).
struct Config
{
    bool known = false;
    std::string server_url;
    bool tls_verify = true;
    int sync_interval_min = 0;
    bool sync_on_game_exit = true, sync_on_game_start = false, sync_on_change = false;
    bool notify = true, update_check = true;
    std::string states = "off";          // off, upload, sync
    std::string conflict_policy = "ask"; // ask, newest, local, server
    std::string ps2_cards = "per_game";  // per_game, backup
    bool cover_cache = true;
    int download_concurrency = 1;
};

// The payload's own updates (/api/update).
struct Update
{
    bool known = false;
    std::string state, message, error, current, latest;
    bool available = false, autostart = false;
    // false: RomM Sync doesn't update itself; a new RommPS brings it (PS5).
    bool self_update = true;
    double done = 0, total = 0;
    bool busy() const { return state == "checking" || state == "downloading" || state == "installing" || state == "restarting"; }
};

class App
{
  public:
    App();

    // Once per frame: delivers finished requests, starts the timed ones and
    // turns decoded covers into textures (renderer may be null in tests).
    void tick(float dt, hui::gfx::Renderer *renderer);

    const Status &status() const { return status_; }
    // RommPS carries the RomM Sync payload of its own release
    // (assets/rommps/romm-sync.elf, version kMinPayload): it starts it when
    // none is running, puts it in place of an older one (never of a newer
    // one), and keeps the copies the console starts it from up to date.
    static constexpr const char *kMinPayload = "1.1.1";
    bool payload_too_old() const;
    void start_payload(); // sends the payload to the HEN's loader
    // Sending it, or RomM Sync is running and not answering yet (starting up,
    // or waking from rest mode): either way it'll answer in a moment.
    bool payload_starting() const { return start_state_ == 1 || payload_waiting_; }
    const std::string &payload_error() const { return start_error_; }
    // The payload into the HEN's autostart folder, so it starts with the console.
    void install_autostart(Api::Callback done);
    const std::vector<Platform> &platforms() const { return platforms_; }
    const std::vector<Download> &downloads() const { return downloads_; }
    const Download *download_for(int rom_id) const;

    const Platform *platform_by_id(int id) const;

    // A list of games (made the first time it's asked for), and the pages
    // holding [first, last] loaded. Lists stay for a while, a few at most.
    GameList &games(int platform_id, const std::string &search);
    void ensure(GameList &list, int first, int last);

    // The games on this console, most recently played first.
    const std::vector<Game> &installed() const { return installed_; }
    // A game's details; asks for them the first time.
    const Details &details(int rom_id);
    void refresh_installed();
    // A cover's texture and palette; asks for it the first time.
    const Cover &cover(const std::string &path);

    void download(int rom_id, const std::string &name);
    void cancel_download(int job_id);
    void clear_finished_downloads();
    void sync_now();

    const Config &config() const { return config_; }
    void refresh_config();
    // Changes settings: fields is a JSON object's members ("\"notify\":true").
    // The copy here changes at once; a refusal puts the payload's back.
    void set_config(const std::string &fields, const std::function<void(Config &)> &apply);

    const Update &update() const { return update_; }
    void refresh_update();
    void check_update();
    void install_update();

    // Pairing: asks RomM for a code to approve (status().pairing follows it).
    void pair_start(const std::string &server_url, bool tls_verify);
    void pair_forget();
    // Background syncs wait until the setup is finished again.
    void setup_restart();
    // Asks for the status now rather than at the next tick.
    void refresh_now() { refresh_status(); }

    // Any other call to the payload, for the setup's steps.
    void get(const std::string &path, Api::Callback done) { api_.get(path, std::move(done)); }
    void post(const std::string &path, const std::string &body, Api::Callback done)
    {
        api_.post(path, body, std::move(done));
    }
    void say(const std::string &message);

    // A message for the screen to show for a few seconds ("" for none).
    const std::string &toast() const { return toast_; }
    float toast_age() const { return toast_age_; }

  private:
    void refresh_status();
    // Whether a RomM Sync is running, from the lock it holds while it runs:
    // 1 yes, 0 no, -1 can't tell.
    static int payload_lock();
    // Brings the copies the console starts RomM Sync from up to the one
    // RommPS carries, leaving autostart on or off as it is; turns it on once
    // for anyone who never chose either way (1.1.0's setup left it off).
    void repair_autostart();
    void evict_covers(hui::gfx::Renderer *renderer);
    void send_page(const std::string &key, int page);
    void refresh_platforms();
    void refresh_downloads();

    Api api_;
    // Covers on workers of their own, so lists never wait behind them; asked
    // for only while they're on screen (wanted_), newest first.
    static constexpr int kCoverWorkers = 3;
    static constexpr int kCoversInFlight = 6;
    Api covers_api_[kCoverWorkers]; // each to the payload on 127.0.0.1
    std::vector<std::string> wanted_; // drawn since the last tick, in draw order
    // Pages of games, on workers of their own too; asked for at the tick if
    // the screen still wants them.
    static constexpr int kPageWorkers = 2;
    Api pages_api_[kPageWorkers];
    std::vector<std::pair<std::string, int>> wanted_pages_; // since the last tick
    int pages_in_flight_ = 0, next_page_worker_ = 0;
    int covers_in_flight_ = 0, next_cover_worker_ = 0;
    Status status_;
    std::vector<Platform> platforms_;
    std::vector<Download> downloads_;
    std::vector<Game> installed_;
    std::unordered_map<int, Details> details_;
    struct CachedList
    {
        std::unique_ptr<GameList> list;
        double used = 0;
    };
    std::unordered_map<std::string, CachedList> lists_;
    double clock_ = 0;
    Config config_;
    Update update_;
    float update_timer_ = 0;
    bool update_busy_ = false;
    std::unordered_map<std::string, Cover> covers_;
    struct Pending
    {
        std::string path, bytes;
    };
    std::deque<Pending> to_decode_;
    float status_timer_ = 0, downloads_timer_ = 0;
    bool status_busy_ = false, downloads_busy_ = false, platforms_busy_ = false;
    bool was_paired_ = false;
    const std::string &bundled_payload();
    std::string bundled_;
    bool bundled_read_ = false;
    std::atomic<int> start_state_{0}; // 0 idle, 1 sending, 2 sent, 3 failed
    std::string start_error_;         // written before start_state_ becomes 3
    bool tried_start_ = false, updated_payload_ = false, repaired_ = false;
    bool payload_waiting_ = false;
    int unanswered_ = 0; // status requests in a row RomM Sync didn't answer
    int active_downloads_ = 0;
    std::string toast_;
    float toast_age_ = 0;
};

// The program's one App, created on first use.
App &app();

} // namespace rommps

// The program hands the kit's renderer over (rommps.cpp is compiled in a
// namespace of its own, so this is plain C).
extern "C" void rommps_set_renderer(void *renderer);
extern "C" void *rommps_renderer();
