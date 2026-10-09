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
};

struct Game
{
    int id = 0;
    int platform_id = 0;
    std::string name, fs_name, cover; // cover: the RomM asset path, "" if none
    double size = 0;
    bool installed = false;
    bool multi = false;
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
    int total = -1;                   // unknown until the first page arrives
    std::vector<Game> games;          // sized to total; a game with id 0 isn't loaded yet
    std::vector<unsigned char> pages; // per page: 0 not asked, 1 asked, 2 here
    std::vector<std::pair<std::string, int>> letters; // first letter -> the index of its first game
    std::string error;
    bool ready() const { return total >= 0; }
    const Game *at(int index) const
    {
        return index >= 0 && index < static_cast<int>(games.size()) && games[static_cast<std::size_t>(index)].id
                   ? &games[static_cast<std::size_t>(index)]
                   : nullptr;
    }
};

struct Download
{
    int id = 0, rom_id = 0;
    std::string name, platform, status, error;
    double done = 0, total = 0, speed = 0;
    float progress() const { return total > 0 ? static_cast<float>(done / total) : 0.0f; }
    bool active() const { return status == "queued" || status == "running" || status == "cancelling"; }
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
    int download_concurrency = 1;
};

// The payload's own updates (/api/update).
struct Update
{
    bool known = false;
    std::string state, message, error, current, latest;
    bool available = false, autostart = false;
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

    void download(int rom_id);
    void cancel_download(int job_id);
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
    void refresh_platforms();
    void refresh_downloads();

    Api api_;
    Api covers_api_; // covers on a worker of their own, so lists never wait behind them
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
