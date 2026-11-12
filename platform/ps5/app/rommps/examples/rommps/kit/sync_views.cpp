// RommPS: lists about the sync. See sync_views.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "sync_views.hpp"

#include "cJSON.h"
#include "core/tween.hpp"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <memory>

namespace rommps
{

namespace
{

namespace ui = hui::ui;
using hui::Action;
using hui::audio::Cue;
using hui::gfx::Rect;
using Json = std::unique_ptr<cJSON, void (*)(cJSON *)>;

constexpr float kMargin = 96.0f;
constexpr float kArmedFor = 4.0f;
const Rect kRows{kMargin - 32, 300, 1792, 650};

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

std::string json_str(const std::string &s)
{
    std::string out = "\"";
    for (char c : s)
    {
        if (c == '"' || c == '\\')
            out += '\\';
        out += c;
    }
    return out + "\"";
}

std::string ago(double t)
{
    if (t <= 0)
        return "never";
    const double s = std::difftime(std::time(nullptr), static_cast<std::time_t>(t));
    char text[32];
    if (s < 60)
        return "just now";
    if (s < 3600)
        std::snprintf(text, sizeof text, "%d min ago", static_cast<int>(s / 60));
    else if (s < 86400)
        std::snprintf(text, sizeof text, "%d h ago", static_cast<int>(s / 3600));
    else
        std::snprintf(text, sizeof text, s < 2 * 86400 ? "%d day ago" : "%d days ago", static_cast<int>(s / 86400));
    return text;
}

// "2026-10-14T06:53:37+00:00" as "2026-10-14 06:53 UTC".
std::string when(const char *iso)
{
    const std::string s = iso ? iso : "";
    if (s.size() < 16)
        return s;
    return s.substr(0, 10) + " " + s.substr(11, 5) + " UTC";
}

std::string local_time(double t)
{
    if (t <= 0)
        return "never";
    const std::time_t tt = static_cast<std::time_t>(t);
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", std::localtime(&tt));
    return text;
}

std::string base_name(const std::string &path)
{
    const std::string p = path.substr(0, path.find('#')); // a save folder synced as a zip
    const std::size_t slash = p.find_last_of('/');
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

std::string size_text(double bytes)
{
    char text[32];
    if (bytes >= 1024 * 1024)
        std::snprintf(text, sizeof text, "%.1f MB", bytes / (1024 * 1024));
    else
        std::snprintf(text, sizeof text, "%.0f KB", bytes / 1024);
    return text;
}

// "uploaded" -> "Uploaded"
std::string capital(std::string s)
{
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z')
        s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

const char *status_label(const std::string &status)
{
    return status == "synced"     ? "In sync"
           : status == "changed"  ? "Changed here"
           : status == "conflict" ? "Conflict"
           : status == "missing"  ? "Missing"
           : status == "no_game"  ? "Game not here"
                                  : status.c_str();
}

// A platform's name: RomM's, else the folder's own.
std::string platform_name(const App &app, const std::string &dir)
{
    for (const Platform &p : app.platforms())
        if (p.slug == dir)
            return p.name;
    std::string up = dir;
    for (char &c : up)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return up.empty() ? std::string("Other") : up;
}

// One log entry as a row: the game, its result ("Downloaded") on the right,
// then "Save · RetroArch (mGBA) · 3 min ago".
Row event_row(const cJSON *e, bool with_game)
{
    const std::string kind = str(e, "kind"), action = str(e, "action");
    const std::string what = kind == "card" ? std::string("Memory card") : kind == "bios" ? std::string("BIOS") : capital(kind);
    // BIOS files and emulators belong to no game: they're named by their kind.
    const bool no_game = kind == "card" || num(e, "rom_id") <= 0;
    std::string label = what;
    if (with_game && !no_game)
    {
        const std::string game = plain_text(str(e, "game"));
        label = game.empty() ? "Game " + std::to_string(static_cast<int>(num(e, "rom_id"))) : game;
    }
    std::string detail = with_game && !no_game ? what : std::string();
    auto more = [&detail](const std::string &part) {
        if (!part.empty())
            detail += (detail.empty() ? "" : "  \xC2\xB7  ") + part;
    };
    more(plain_text(str(e, "detail"))); // file names, the service's errors
    if (action != "skipped")
        more(plain_text(str(e, "emulator")));
    more(ago(num(e, "t")));
    Row r{RowKind::info, label, capital(action), detail};
    r.danger = action == "failed" || action == "conflict";
    return r;
}

// One tracked file as a row: the game (or the kind, inside a game's view),
// its status on the right, then "Save · synced 3 min ago".
Row file_row(const cJSON *f, const std::string &game)
{
    const std::string kind = str(f, "kind");
    std::string detail = (game.empty() ? std::string() : capital(kind) + "  \xC2\xB7  ") + "synced " + ago(num(f, "synced_at"));
    if (kind == "state" && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(f, "from_here")) && num(f, "server_id") > 0)
        detail += "  \xC2\xB7  from another device";
    Row r{RowKind::info, game.empty() ? capital(kind) : game, status_label(str(f, "status")), detail};
    r.danger = std::string(str(f, "status")) == "missing";
    return r;
}

} // namespace

void SyncView::open(App &app, Kind kind, int rom_id, const std::string &name)
{
    active_ = true;
    kind_ = kind;
    rom_id_ = rom_id;
    name_ = name;
    choosing_ = -1;
    armed_ = -1;
    age_ = 0.0f;
    busy_ = false;
    list_.set_focus(0);
    load(app);
}

void SyncView::load(App &app)
{
    const int gen = ++generation_;
    items_.clear();
    conflicts_.clear();
    switch (kind_)
    {
    case kConflicts:
        title_ = "Conflicts";
        subtitle_ = "The copy you don't keep is backed up.";
        break;
    case kActivity:
        title_ = "Activity";
        subtitle_ = "Newest first";
        break;
    case kTracked:
        title_ = "Tracked saves";
        subtitle_ = "";
        break;
    case kGame:
        title_ = plain_text(name_.c_str());
        subtitle_ = ROMM_BTN_CONFIRM " twice restores a version from RomM";
        break;
    }
    add({RowKind::info, "Loading", "", ""});
    waiting_ = 1;

    if (kind_ == kConflicts)
    {
        app.get("/api/status", [this, gen](const Response &r) {
            if (gen != generation_)
                return;
            Json j(r.json(), cJSON_Delete);
            const cJSON *c;
            cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j.get(), "sync"), "conflicts"))
            {
                Conflict k;
                k.path = str(c, "path");
                k.game = plain_text(*str(c, "rom_name") ? str(c, "rom_name") : base_name(k.path).c_str());
                k.local = local_time(num(c, "local_mtime"));
                k.server = when(str(c, "server_updated_at"));
                conflicts_.push_back(std::move(k));
            }
            if (!r.ok())
                conflicts_.clear();
            show_conflicts();
            if (!r.ok())
            {
                items_.clear();
                add({RowKind::info, "Couldn't load them", "", r.message()});
            }
        });
    }
    else if (kind_ == kActivity)
    {
        app.get("/api/events?limit=200", [this, gen](const Response &r) {
            if (gen != generation_)
                return;
            items_.clear();
            Json j(r.json(), cJSON_Delete);
            const bool ok = r.ok() && cJSON_IsArray(j.get());
            const cJSON *e;
            if (ok)
                cJSON_ArrayForEach(e, j.get()) add(event_row(e, true));
            if (items_.empty())
                add({RowKind::info, ok ? "Nothing yet" : "Couldn't load the log", "",
                     ok ? "" : r.ok() ? ROMM_APP "'s background service is out of date" : r.message()});
        });
    }
    else if (kind_ == kTracked)
    {
        app.get("/api/tracked", [this, gen, &app](const Response &r) {
            if (gen != generation_)
                return;
            items_.clear();
            Json j(r.json(), cJSON_Delete);
            const bool ok = r.ok() && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(j.get(), "platforms"));
            struct Plat
            {
                std::string name;
                const cJSON *games;
            };
            std::vector<Plat> plats;
            const cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(j.get(), "platforms"))
                plats.push_back({platform_name(app, str(p, "platform")), cJSON_GetObjectItemCaseSensitive(p, "games")});
            std::sort(plats.begin(), plats.end(), [](const Plat &a, const Plat &b) { return a.name < b.name; });
            for (const Plat &pl : plats)
            {
                add({RowKind::header, pl.name});
                const cJSON *g, *f;
                cJSON_ArrayForEach(g, pl.games)
                {
                    const std::string game = plain_text(str(g, "name"));
                    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(g, "files")) add(file_row(f, game));
                }
            }
            if (items_.empty())
                add({RowKind::info, ok ? "Nothing tracked yet" : "Couldn't load them", "",
                     ok ? "" : r.ok() ? ROMM_APP "'s background service is out of date" : r.message()});
            char sub[96];
            std::snprintf(sub, sizeof sub, "%d saves, %d states", static_cast<int>(num(j.get(), "saves")),
                          static_cast<int>(num(j.get(), "states")));
            if (ok)
                subtitle_ = sub;
        });
    }
    else
    {
        // A game: what's here, RomM's versions and the game's log, shown together.
        waiting_ = 3;
        game_here_.clear();
        game_history_.clear();
        game_log_.clear();
        const std::string id = std::to_string(rom_id_);
        auto done = [this, gen](std::string *slot, const Response &r) {
            if (gen != generation_)
                return;
            *slot = r.ok() ? r.body : std::string("!") + r.message();
            if (--waiting_ > 0)
                return;
            items_.clear();
            Json here(cJSON_Parse(game_here_.c_str()), cJSON_Delete), history(cJSON_Parse(game_history_.c_str()), cJSON_Delete),
                log(cJSON_Parse(game_log_.c_str()), cJSON_Delete);
            add({RowKind::header, "On this " ROMM_HERE});
            const std::size_t before = items_.size();
            const cJSON *p, *g, *f;
            cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(here.get(), "platforms"))
                cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(p, "games"))
                    cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(g, "files")) add(file_row(f, ""));
            if (items_.size() == before)
                add({RowKind::info, "Nothing here yet", "", ""});
            static const char *const kinds[2][2] = {{"saves", "Saves on RomM"}, {"states", "Save states on RomM"}};
            for (int k = 0; k < 2; ++k)
            {
                add({RowKind::header, kinds[k][1]});
                const cJSON *list = cJSON_GetObjectItemCaseSensitive(history.get(), kinds[k][0]);
                std::vector<const cJSON *> versions;
                const cJSON *v;
                cJSON_ArrayForEach(v, list) versions.push_back(v);
                std::sort(versions.begin(), versions.end(), [](const cJSON *a, const cJSON *b) {
                    return std::string(str(a, "updated_at")) > std::string(str(b, "updated_at"));
                });
                for (const cJSON *x : versions)
                {
                    std::string from = *str(x, "device") ? str(x, "device") : str(x, "emulator");
                    std::string detail = plain_text(str(x, "file_name")) + "  \xC2\xB7  " + size_text(num(x, "size"));
                    if (!from.empty())
                        detail += "  \xC2\xB7  from " + plain_text(from.c_str());
                    add({RowKind::action, when(str(x, "updated_at")), "Restore", detail}, k ? kRestoreState : kRestoreSave,
                        static_cast<int>(num(x, "id")));
                }
                if (versions.empty())
                    add({RowKind::info, game_history_[0] == '!' ? "Couldn't ask RomM" : "Nothing on RomM yet", "",
                         game_history_[0] == '!' ? game_history_.substr(1) : ""});
            }
            add({RowKind::header, "Recent"});
            const std::size_t log_at = items_.size();
            const cJSON *e;
            int n = 0;
            cJSON_ArrayForEach(e, log.get()) if (n++ < 30) add(event_row(e, false));
            if (items_.size() == log_at)
                add({RowKind::info, "Nothing synced yet", "", ""});
        };
        app.get("/api/tracked?rom_id=" + id, [done, this](const Response &r) { done(&game_here_, r); });
        app.get("/api/history?rom_id=" + id, [done, this](const Response &r) { done(&game_history_, r); });
        app.get("/api/events?limit=30&rom_id=" + id, [done, this](const Response &r) { done(&game_log_, r); });
    }
}

void SyncView::show_conflicts()
{
    items_.clear();
    choosing_ = -1;
    for (std::size_t i = 0; i < conflicts_.size(); ++i)
    {
        const Conflict &c = conflicts_[i];
        add({RowKind::action, c.game, "Choose", "This " ROMM_HERE ": " + c.local + "  \xC2\xB7  RomM: " + c.server}, kPickConflict,
            static_cast<int>(i));
    }
    if (items_.empty())
        add({RowKind::info, "No conflicts", "", ""});
}

std::vector<Row> SyncView::rows() const
{
    std::vector<Row> out;
    out.reserve(items_.size());
    for (const Item &it : items_)
        out.push_back(it.row);
    if (armed_ >= 0 && armed_ < static_cast<int>(out.size()))
        out[static_cast<std::size_t>(armed_)].value = "Press again to restore";
    if (busy_ && busy_row_ >= 0 && busy_row_ < static_cast<int>(out.size()))
        out[static_cast<std::size_t>(busy_row_)].value = busy_text_;
    return out;
}

void SyncView::update(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    age_ += dt;
    armed_age_ += dt;
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        if (choosing_ >= 0)
            show_conflicts();
        else
            active_ = false;
        return;
    }
    const std::vector<Row> r = rows();
    if (armed_ >= 0 && (armed_age_ > kArmedFor || list_.focus() != armed_))
        armed_ = -1;
    input = list_.touch(input, r);
    const RowEvent e = list_.handle(r, input, feedback);
    list_.update(dt, static_cast<int>(r.size()), kRows.h);
    if (e.row < 0 || !e.activated || busy_)
        return;
    const Item it = items_[static_cast<std::size_t>(e.row)];
    switch (it.action)
    {
    case kPickConflict:
    {
        const Conflict &c = conflicts_[static_cast<std::size_t>(it.arg)];
        items_.clear();
        choosing_ = it.arg;
        add({RowKind::info, c.game, "", "Which copy should stay?"});
        add({RowKind::action, "Keep this " ROMM_HERE "'s", "", "Changed here " + c.local + ". RomM's copy is replaced."}, kKeepLocal);
        add({RowKind::action, "Keep RomM's", "", "Changed on RomM " + c.server + ". This " ROMM_HERE "'s copy is backed up."},
            kKeepServer);
        add({RowKind::action, "Not now"}, kCancel);
        list_.set_focus(1);
        break;
    }
    case kKeepLocal:
    case kKeepServer:
    {
        const Conflict c = conflicts_[static_cast<std::size_t>(choosing_)];
        const bool local = it.action == kKeepLocal;
        busy_ = true;
        busy_row_ = e.row;
        busy_text_ = "Resolving\xE2\x80\xA6";
        app.post("/api/conflicts/resolve",
                 "{\"path\":" + json_str(c.path) + ",\"keep\":\"" + (local ? "local" : "server") + "\"}",
                 [this, &app, local](const Response &r) {
                     busy_ = false;
                     app.say(r.ok() ? (local ? "Kept this " ROMM_HERE "'s save" : "Kept RomM's save") : "Couldn't settle it: " + r.message());
                     app.refresh_now();
                     load(app);
                 });
        break;
    }
    case kCancel:
        show_conflicts();
        list_.set_focus(0);
        break;
    case kRestoreSave:
    case kRestoreState:
        if (armed_ != e.row)
        {
            armed_ = e.row;
            armed_age_ = 0.0f;
            break;
        }
        armed_ = -1;
        busy_ = true;
        busy_row_ = e.row;
        busy_text_ = "Restoring\xE2\x80\xA6";
        app.post("/api/restore",
                 std::string("{\"kind\":\"") + (it.action == kRestoreSave ? "save" : "state") + "\",\"id\":" +
                     std::to_string(it.arg) + "}",
                 [this, &app](const Response &r) {
                     busy_ = false;
                     app.say(r.ok() ? "Restored. The current file was backed up" : "Couldn't restore it: " + r.message());
                     if (r.ok())
                         load(app);
                 });
        break;
    default:
        list_.refuse(feedback);
        break;
    }
}

void SyncView::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                    float clock) const
{
    (void)app;
    const float in = hui::tween::stagger(age_, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    ui::text(list, fonts.display, fonts.display.font->fit(title_, 64, 1700), kMargin, 220, 64, look.text);
    ui::text(list, fonts.regular, fonts.regular.font->fit(subtitle_, 24, 1700), kMargin, 268, 24, look.muted);
    list_.draw(list, fonts, look, rows(), {kRows.x, kRows.y + 24 * (1.0f - in), kRows.w, kRows.h}, clock);
    list.pop_opacity();
}

std::vector<hui::ui::Hint> SyncView::hints() const
{
    std::vector<hui::ui::Hint> h;
    const int f = list_.focus();
    const bool can = f >= 0 && f < static_cast<int>(items_.size()) && items_[static_cast<std::size_t>(f)].action != kNothing;
    if (can)
        h.push_back({hui::ui::Button::cross, choosing_ >= 0 ? "Choose" : kind_ == kGame ? "Restore" : "Choose"});
    h.push_back({hui::ui::Button::circle, "Back"});
    return h;
}

} // namespace rommps
