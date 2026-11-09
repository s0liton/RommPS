// RommPS: what the screen shows, kept up to date from the RomM Sync payload.
//
// The payload does the work (sync, downloads, RomM) and serves its state on
// http://127.0.0.1:<port>/api (src/web.c in the repository). This keeps a
// copy for the screen: status, platforms, collections, the games of the
// platforms and collections looked at, downloads and covers, refreshed on
// timers through the API client (platform/ps5/app/src/rommps_api.hpp) so a
// frame never waits for it.
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
#include <unordered_set>
#include <vector>

namespace hui::ui
{
struct Fonts;
}

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
    float aspect = 0;          // its width over its height, 0 until it's loaded
    Palette palette;
    bool requested = false;
    bool failed = false;
    double used = 0; // when it was last drawn
    double retry_at = 0; // a fetch that failed (not a missing cover) is tried again then
    int retries = 0;     // tries again so far this launch (kCoverRetries at most)
};

struct Game
{
    int id = 0;
    int platform_id = 0;
    std::string name, fs_name, cover; // cover: the RomM asset path, "" if none
    double size = 0;
    bool installed = false;
    bool multi = false;
    bool not_game = false; // a file RomM scanned beside the games (metadata.txt...), or a Switch update or DLC: hidden
    bool archived = false; // a .rar (or any archive, for Switch) nothing here plays: not downloaded
    std::string version;   // what sets this version apart, when RomM has others of the game
    int versions = 0;      // how many versions RomM has, this one included; 0 if just one
    std::vector<int> siblings; // the other versions' ids
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
    std::string archived;    // why it can't be downloaded: it's an archive
    int updates = 0, dlc = 0; // a Switch game's, downloaded with it
    std::string title_id;     // a Switch game's ("0100ABF008968000"), "" otherwise
    struct Addon
    {
        enum Kind { update, dlc, game } kind = update; // game: another one in its folder (a collection)
        std::string name, title_id;
        double size = 0;
    };
    // The update first, then the DLC; then the other games of a collection's
    // folder and theirs, which come with the download but aren't this game's.
    std::vector<Addon> addons;
    int bundle = 0; // how many of addons are those
    struct Version
    {
        int id = 0;
        std::string name, label; // the file's name, and what sets it apart ("USA · Disc 2")
        double size = 0;
        struct Disc
        {
            int disc = 0;
            double size = 0;
        };
        std::vector<Disc> discs; // a folder of discs: they come down together
    };
    // Every version of the game, and a folder's discs; empty when there's
    // nothing to pick (one version, not a folder of discs).
    std::vector<Version> versions;
};

struct Platform
{
    int id = 0;
    std::string name, slug, profile; // profile: the emulator, "" if none plays it
    int rom_count = 0;
};

// One of RomM's collections. kind is the payload's name for its sort
// ("regular" for the user's own; "smart" and "virtual" are RomM's others),
// id RomM's, as text since virtual collections' ids are. owner is the user
// who made it, when that's someone else ("" for the user's own). type is
// what a virtual one gathers by ("genre", "franchise"...).
struct Collection
{
    std::string kind, id, name, description, owner, type;
    int rom_count = 0;
};

// Which games a list holds: a platform's (0: every platform), narrowed to a
// collection's when collection_id is set.
struct Scope
{
    int platform_id = 0;
    std::string collection_kind, collection_id;

    static Scope platform(int id)
    {
        Scope s;
        s.platform_id = id;
        return s;
    }
    static Scope collection(const Collection &c)
    {
        Scope s;
        s.collection_kind = c.kind;
        s.collection_id = c.id;
        return s;
    }
    bool in_collection() const { return !collection_id.empty(); }
    // Tells lists apart in App's cache.
    std::string key() const { return std::to_string(platform_id) + "|" + collection_kind + "|" + collection_id; }
    // The payload's /api/roms parameters for it ("platform_id=1").
    std::string query() const;
};

// A list of games from RomM, A to Z: a platform's, a collection's, a
// search's (in one of those, or every platform). Loaded a page at a time as
// it's looked at.
struct GameList
{
    static constexpr int kPage = 60;
    Scope scope;
    std::string search;
    int total = -1;                   // games to show; unknown until the first page arrives
    std::vector<Game> games;          // RomM's whole list, by RomM's offset; id 0 isn't loaded yet
    std::vector<unsigned char> pages; // per page: 0 not asked, 1 asked, 2 here
    std::vector<std::pair<std::string, int>> letters; // first letter -> the index of its first game
    std::vector<std::pair<std::string, int>> raw_letters; // the same, by RomM's offset
    std::vector<int> hidden;          // RomM offsets of the files that aren't games, sorted
    std::string error;
    // The shape of the grid's cards (width over height), from the covers at
    // the top; 0 until the library has seen some. Settled: it stays.
    float shape = 0;
    bool shape_settled = false;
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
    int disc = 0; // one disc of a game kept as a folder of discs; 0: all of it
    std::string name, platform, status, error;
    double done = 0, total = 0, speed = 0;
    // What the sync after the download brought.
    int saves = 0, states = 0, save_conflicts = 0;
    bool saves_failed = false;
    float progress() const { return total > 0 ? static_cast<float>(done / total) : 0.0f; }
    // The payload's states: queued, downloading, cancelling, syncing (the
    // game's saves, once it's downloaded), then done, error or cancelled.
    bool running() const { return status == "downloading"; }
    bool syncing() const { return status == "syncing"; }
    bool active() const { return status == "queued" || running() || status == "cancelling" || syncing(); }
    bool cancellable() const { return active() && !syncing(); }
    bool finished() const { return !active(); }
    bool retryable() const { return status == "error" || status == "cancelled"; }
    std::string saves_note() const;
};

struct Status
{
    bool known = false;     // an answer arrived at least once
    bool reachable = false; // the payload answered the last time
    bool paired = false, setup_complete = false;
    std::string version, server, user, ip, error;
    std::string server_trouble; // RomM isn't answering: what the payload says, "" while it does
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
    // Games left in the folders RetroArch used before 2.0, and those
    // folders' names (/api/retroarch/move moves them to the new ones).
    int retroarch_move = 0;
    std::string retroarch_folders;
};

// The payload's settings that RommPS shows (/api/config).
struct Config
{
    bool known = false;
    std::string server_url;
    bool tls_verify = true;
    int sync_interval_min = 0;
    bool save_sync = true; // false: RomM only as a game source
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
    static constexpr const char *kMinPayload = "1.4.1";
    bool payload_too_old() const;
    void start_payload(); // sends the payload to the HEN's loader
    // Sending it, or RomM Sync is running and not answering yet (starting up,
    // or waking from rest mode): either way it'll answer in a moment.
    bool payload_starting() const { return start_state_ == 1 || payload_waiting_; }
    const std::string &payload_error() const { return start_error_; }
    // The payload into the HEN's autostart folder, so it starts with the console.
    void install_autostart(Api::Callback done);
    const std::vector<Platform> &platforms() const { return platforms_; }
    // The collections of every kind the payload offers: the user's own, then
    // other users' public ones, each by name. Asked for when first looked at
    // (want_collections) and when refresh_collections says so.
    const std::vector<Collection> &collections() const { return collections_; }
    // Why the last refresh got none ("" if it did).
    const std::string &collections_error() const { return collections_error_; }
    // RomM won't show this pairing the collections: it's from before RommPS asked for them.
    bool collections_need_pair() const { return collections_need_pair_; }
    // Every game of a collection not on the console yet, downloaded.
    void download_collection(const Collection &c);
    bool collections_loaded() const { return collections_loaded_; }
    void want_collections()
    {
        if (!collections_loaded_)
            refresh_collections();
    }
    // Asks for them again: they change in RomM. Nothing while asking already.
    void refresh_collections();
    const std::vector<Download> &queue() const { return queue_; }
    const Download *download_for(int rom_id) const;
    // One disc's download (0: the whole game's), the newest.
    const Download *download_for(int rom_id, int disc) const;

    const Platform *platform_by_id(int id) const;
    const Collection *collection_by_id(const std::string &kind, const std::string &id) const;

    // A list of games (made the first time it's asked for), and the pages
    // holding [first, last] loaded. Lists stay for a while, a few at most.
    GameList &games(const Scope &scope, const std::string &search);
    void ensure(GameList &list, int first, int last);

    // The games on this console, most recently played first.
    const std::vector<Game> &installed() const { return installed_; }
    // Whether a game is on the console now. A list's copy can be older than
    // a game deleted by hand, so this goes by the latest installed list.
    bool on_console(const Game &g) const
    {
        if (!installed_known_)
            return g.installed;
        if (on_console_.count(g.id))
            return true;
        for (int id : g.siblings)
            if (on_console_.count(id))
                return true;
        return false;
    }
    bool on_console(int rom_id) const { return on_console_.count(rom_id) > 0; }
    // Asks the payload about one game, so a sheet's actions are right even
    // before the next look at every game (the payload does that sparingly).
    void check_on_console(int rom_id);
    // A game kept as a folder of discs: the discs on the console, as the last
    // check_on_console found them; nullptr before one, or for any other game.
    const std::vector<int> *discs_here(int rom_id) const
    {
        const auto found = discs_here_.find(rom_id);
        return found == discs_here_.end() ? nullptr : &found->second;
    }
    // Whether a disc of a game is on its way, alone or with the whole game.
    bool disc_downloading(int rom_id, int disc) const;
    // A game's details; asks for them the first time.
    const Details &details(int rom_id);
    void refresh_installed();
    // Asks for the platforms again: which emulator plays each, and its
    // folder, change when emulators are set up here or in the web UI.
    void reload_platforms()
    {
        if (!platforms_busy_)
            refresh_platforms();
    }
    // A cover's texture and palette; asks for it the first time.
    const Cover &cover(const std::string &path);
    // A platform's logo from RomM, by its slug ("psx"), as a cover.
    const Cover &platform_logo(const std::string &slug);
    // A cover's width over its height, kept after its texture is let go;
    // 0 until it has been loaded once. since: when it became known.
    float cover_aspect(const std::string &path, double *since = nullptr) const;
    double clock() const { return clock_; }

    // disc: one disc of a game kept as a folder of discs, 0 for all of it.
    void download(int rom_id, const std::string &name, int disc = 0);
    void cancel_download(int job_id);
    void retry_download(int job_id);
    // quiet: no notification, one of several going together. disc: one disc
    // of a game kept as a folder of discs, 0 for all of it.
    void remove_game(int rom_id, bool quiet = false, int disc = 0);
    void clear_finished_queue();
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

    // Any other call to the payload, for the setup's steps. A long one (the
    // sync preview) goes on a worker of its own, so the status never waits.
    void get(const std::string &path, Api::Callback done, int timeout_ms = 15000)
    {
        (timeout_ms > 15000 ? slow_api_ : api_).get(path, std::move(done), timeout_ms);
    }
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
    // Turns autostart on wherever it's off and brings the copies the console
    // starts RomM Sync from up to the one RommPS carries.
    void repair_autostart();
    void evict_covers(hui::gfx::Renderer *renderer);
    void send_page(const std::string &key, int page);
    void refresh_platforms();
    void refresh_queue();
    // A game came or went: every list's pages are asked for again, drawn as
    // they were meanwhile.
    void refresh_lists();

    Api api_;
    Api slow_api_; // calls that may take minutes
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
    std::vector<Collection> collections_;
    std::string collections_error_;
    bool collections_loaded_ = false;
    bool collections_need_pair_ = false;
    bool collections_busy_ = false;
    std::vector<Download> queue_;
    std::vector<Game> installed_;
    std::unordered_set<int> on_console_; // installed_'s ids
    std::unordered_map<int, std::vector<int>> discs_here_; // check_on_console's discs
    bool installed_known_ = false;
    double installed_sig_ = -1; // the payload's, from /api/status: changes when a game comes or goes
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
    struct Shape
    {
        float aspect = 0;
        double since = 0;
    };
    std::unordered_map<std::string, Shape> shapes_; // every cover loaded this launch
    struct Pending
    {
        std::string path, bytes;
    };
    std::deque<Pending> to_decode_;
    float status_timer_ = 0, queue_timer_ = 0;
    bool status_busy_ = false, queue_busy_ = false, platforms_busy_ = false;
    bool was_paired_ = false;
    const std::string &bundled_payload();
    std::string bundled_;
    bool bundled_read_ = false;
    std::atomic<int> start_state_{0}; // 0 idle, 1 sending, 2 sent, 3 failed
    std::string start_error_;         // written before start_state_ becomes 3
    bool tried_start_ = false, updated_payload_ = false, repaired_ = false, presets_checked_ = false;
    bool payload_waiting_ = false;
    int unanswered_ = 0; // status requests in a row RomM Sync didn't answer
    int active_queue_ = 0;
    std::string toast_;
    float toast_age_ = 0;
};

// The program's one App, created on first use.
App &app();

// Text the screen's fonts can draw: the characters they have stay ("Pokémon",
// "Luigi’s Mansion"), the others fold to their nearest ASCII ("Ō" reads "O" in
// a font without it), and only one with no stand-in at all reads "?".
std::string plain_text(const char *text);

// The faces plain_text asks: the ones the screen draws text with. Set once the
// kit has loaded them, before the payload's first answer is read.
void set_text_fonts(const hui::ui::Fonts &fonts);

// The first byte at or after at that starts a character of UTF-8 text: where
// to cut it without halving one.
inline std::size_t char_boundary(const std::string &text, std::size_t at)
{
    while (at < text.size() && (static_cast<unsigned char>(text[at]) & 0xC0) == 0x80)
        ++at;
    return at;
}

} // namespace rommps

// The program hands the kit's renderer over (rommps.cpp is compiled in a
// namespace of its own, so this is plain C).
extern "C" void rommps_set_renderer(void *renderer);
extern "C" void *rommps_renderer();
