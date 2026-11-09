// RommPS: the Settings tab. See settings_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_page.hpp"

#include "cJSON.h"
#include "core/tween.hpp"
#include "touch.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>

namespace rommps
{

namespace
{

namespace ui = hui::ui;
namespace tween = hui::tween;
using hui::gfx::Rect;

constexpr float kMargin = 96.0f;
constexpr float kArmedFor = 4.0f; // seconds a first press waits for the second
const Rect kRows{kMargin - 32, 290, 1150, 640};
const Rect kAbout{1300, 290, 524, 690};

std::string flag_json(const char *key, bool on)
{
    return std::string("\"") + key + "\":" + (on ? "true" : "false");
}

std::string int_json(const char *key, int value)
{
    return std::string("\"") + key + "\":" + std::to_string(value);
}

std::string str_json(const char *key, const char *value)
{
    return std::string("\"") + key + "\":\"" + value + "\"";
}

} // namespace

void SettingsPage::enter(App &app)
{
    app.refresh_config();
    app.refresh_update();
    armed_ = -1;
    on_credits_ = false;
}

std::vector<Row> SettingsPage::rows(const App &app, std::vector<int> *ids) const
{
    const Config &c = app.config();
    const Update &u = app.update();
    std::vector<Row> r(kCount);
    const Look &l = look(current_look());
    r[kSaveSync] = {RowKind::toggle, "Save sync", "", c.save_sync ? "" : "RomM is only for games",
                    c.save_sync};
    r[kLook] = {RowKind::choice, "Theme", l.name, l.family};
    r[kSounds] = {RowKind::toggle, "Interface sounds", "", "", ui_sounds()};
    r[kInterval] = {RowKind::choice, "Sync every", interval_label(c.sync_interval_min), ""};
    r[kOnExit] = {RowKind::toggle, "Sync when a game closes", "", "", c.sync_on_game_exit};
    r[kOnStart] = {RowKind::toggle, "Sync when a game starts", "", "",
                   c.sync_on_game_start};
    r[kOnChange] = {RowKind::toggle, "Upload saves as they change", "", "While you play", c.sync_on_change};
    r[kStates] = {RowKind::choice, "Save states", label_of(rommps::kStates, c.states), ""};
    r[kPolicy] = {RowKind::choice, "Resolving save conflicts", label_of(kPolicies, c.conflict_policy),
                  "What " ROMM_APP " will do by default whenever a save conflict exists between this " ROMM_MODEL " and your RomM Server"};
    r[kConcurrency] = {RowKind::choice, "Concurrent Game Downloads", std::to_string(c.download_concurrency), ""};
    r[kNotify] = {RowKind::toggle, "Notifications", "", "", c.notify};
    r[kUpdateCheck] = {RowKind::toggle, "Check for updates daily", "", "", c.update_check};
    {
        const Status &s = app.status();
        char detail[96];
        if (!c.cover_cache)
            std::snprintf(detail, sizeof detail, "Covers come from RomM as you browse");
        else if (s.covers_state == "paused")
            std::snprintf(detail, sizeof detail, "Paused while you play or download (%d of %d saved)", s.covers_done,
                          s.covers_total);
        else if (s.covers_state == "full")
            std::snprintf(detail, sizeof detail, "The cover space is nearly full (%d of %d saved)", s.covers_done,
                          s.covers_total);
        else if (s.covers_total > 0 && s.covers_done < s.covers_total)
            std::snprintf(detail, sizeof detail, "Saving covers: %d of %d", s.covers_done, s.covers_total);
        else if (s.covers_total > 0)
            std::snprintf(detail, sizeof detail, "All %d covers are on the " ROMM_HERE, s.covers_total);
        else
            std::snprintf(detail, sizeof detail, "%s", "");
        r[kCovers] = {RowKind::toggle, "Keep all covers on the " ROMM_HERE, "", detail, c.cover_cache};
    }

    Row &update = r[kUpdate];
    update.kind = RowKind::action;
    update.label = ROMM_APP " " + (u.current.empty() ? app.status().version : u.current);
    if (!u.self_update)
    {
        // The app brings its background service with it: the store updates both.
        update.label = std::string(ROMM_APP " ") + App::kMinPayload;
        update.value = u.state == "checking" ? "Checking" : "Check for updates";
        update.enabled = u.state != "checking";
        update.detail = u.available           ? ROMM_APP " " + u.latest + " is out: update it " ROMM_UPDATE_IN
                        : u.state == "error"  ? (u.error.empty() ? u.message : u.error)
                        : u.latest.empty()    ? "Updates come from " ROMM_STORE
                                              : "Up to date";
    }
    else if (u.busy())
    {
        update.value = u.message.empty() ? "Working" : u.message;
        if (u.total > 0)
        {
            char text[32];
            std::snprintf(text, sizeof text, "  %d%%", static_cast<int>(100.0 * u.done / u.total));
            update.value += text;
        }
        update.enabled = false;
    }
    else if (u.available)
    {
        update.value = "Install " + u.latest;
        update.detail = ROMM_APP " restarts with the new version";
    }
    else
    {
        update.value = "Check for updates";
        update.detail = u.state == "error" ? (u.error.empty() ? u.message : u.error)
                        : u.state == "pending" ? "Restart the " ROMM_HERE " to finish installing"
                        : u.latest.empty()   ? ""
                                               : "Up to date";
    }

    r[kEmulators] = {RowKind::action, "Configure Emulators", "", ""};
    r[kSetup] = {RowKind::action, "Re-run setup wizard", "", ""};
    r[kForget] = {RowKind::action, "Unpair from RomM", "", "Files on this " ROMM_HERE " are kept"};
    r[kForget].danger = true;
    r[kDiagnostics] = {RowKind::action, "Diagnostic bundle", diag_ == 1 ? "Making" : "",
                       diag_ == 2 ? "Saved to " + diag_where_
                       : diag_ == 3 ? "Couldn't make it"
                                    : "Gather logs to report an issue. The bundle does not collect any personally identifiable information."};
    r[kDiagnostics].enabled = diag_ != 1;
    if (armed_ == kSetup || armed_ == kForget)
        r[static_cast<std::size_t>(armed_)].value = "Press again to confirm";
    if (!c.known)
        for (int i = kInterval; i <= kCovers; ++i)
            r[static_cast<std::size_t>(i)].enabled = false;

    // In sections.
    static const struct
    {
        const char *name;
        int ids[7];
    } kSections[] = {
        {"Sync", {kSaveSync, kInterval, kOnExit, kOnStart, kOnChange, kPolicy, kStates}},
        {"Downloads", {kConcurrency, kCovers, -1}},
        {"Theme, sounds and notifications", {kLook, kSounds, kNotify, -1}},
        {"Emulators", {kEmulators, -1}},
        {"Updates", {kUpdate, kUpdateCheck, -1}},
        {"RomM", {kSetup, kForget, -1}},
        {"Help", {kDiagnostics, -1}},
    };
    std::vector<Row> out;
    if (ids)
        ids->clear();
    for (const auto &section : kSections)
    {
        out.push_back({RowKind::header, section.name});
        if (ids)
            ids->push_back(kHeader);
        for (int id : section.ids)
        {
            if (id < 0)
                break;
            // Save sync off: just the switch to turn it back on.
            if (!c.save_sync && id >= kInterval && id <= kPolicy)
                continue;
            out.push_back(r[static_cast<std::size_t>(id)]);
            if (ids)
                ids->push_back(id);
        }
    }
    return out;
}

const char *SettingsPage::cross_hint(const App &app) const
{
    if (on_credits_)
        return "Answer";
    const std::vector<Row> r = rows(app);
    const Row &row = r[static_cast<std::size_t>(std::clamp(list_.focus(), 0, static_cast<int>(r.size()) - 1))];
    switch (row.kind)
    {
    case RowKind::toggle:
        return "Switch";
    case RowKind::choice:
        return "Change";
    default:
        return "Choose";
    }
}

void SettingsPage::update(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    avatar_texture(static_cast<hui::gfx::Renderer *>(rommps_renderer()));
    std::vector<int> ids;
    const std::vector<Row> r = rows(app, &ids);
    const int count = static_cast<int>(r.size());
    armed_age_ += dt;
    // The portrait touched: its call. A row touched instead: back to the rows.
    if (const touch::Touch t = touch::take(&on_credits_); t.pressed)
    {
        on_credits_ = true;
        input = touch::with(input, t);
    }
    else if (on_credits_ && touch::pending(&list_))
        on_credits_ = false;
    // Below the last row: the author's portrait, and the call behind it.
    if (on_credits_)
    {
        list_.update(dt, count, kRows.h);
        if (input.nav == hui::Direction::up || input.is_pressed(hui::Action::back))
        {
            on_credits_ = false;
            feedback.play(hui::audio::Cue::focus);
        }
        else if (input.is_pressed(hui::Action::confirm))
        {
            call_.start();
            feedback.play(hui::audio::Cue::modal_open);
        }
        else if (input.nav != hui::Direction::none && !input.nav_repeat)
            list_.refuse(feedback);
        return;
    }
    if (input.nav == hui::Direction::down && !input.nav_repeat && list_.focus() == count - 1)
    {
        on_credits_ = true;
        feedback.play(hui::audio::Cue::focus);
        return;
    }
    const int focused_id = ids[static_cast<std::size_t>(std::clamp(list_.focus(), 0, count - 1))];
    if (armed_ >= 0 && (armed_age_ > kArmedFor || focused_id != armed_))
        armed_ = -1;
    input = list_.touch(input, r);
    const RowEvent e = list_.handle(r, input, feedback);
    list_.update(dt, count, kRows.h);
    if (e.row < 0)
        return;
    const int id = ids[static_cast<std::size_t>(e.row)];
    const Config &c = app.config();
    switch (id)
    {
    case kSaveSync:
        app.set_config(flag_json("save_sync", !c.save_sync), [](Config &k) { k.save_sync = !k.save_sync; });
        break;
    case kLook:
        set_look((current_look() + e.step + look_count()) % look_count());
        break;
    case kSounds:
        set_ui_sounds(!ui_sounds());
        break;
    case kInterval:
    {
        const int minutes = step_interval(c.sync_interval_min, e.step);
        app.set_config(int_json("sync_interval_min", minutes), [minutes](Config &k) { k.sync_interval_min = minutes; });
        break;
    }
    case kOnExit:
        app.set_config(flag_json("sync_on_game_exit", !c.sync_on_game_exit),
                       [](Config &k) { k.sync_on_game_exit = !k.sync_on_game_exit; });
        break;
    case kOnStart:
        app.set_config(flag_json("sync_on_game_start", !c.sync_on_game_start),
                       [](Config &k) { k.sync_on_game_start = !k.sync_on_game_start; });
        break;
    case kOnChange:
        app.set_config(flag_json("sync_on_change", !c.sync_on_change),
                       [](Config &k) { k.sync_on_change = !k.sync_on_change; });
        break;
    case kStates:
    {
        const std::string id = step_option(rommps::kStates, c.states, e.step);
        app.set_config(str_json("states", id.c_str()), [id](Config &k) { k.states = id; });
        break;
    }
    case kPolicy:
    {
        const std::string id = step_option(kPolicies, c.conflict_policy, e.step);
        app.set_config(str_json("conflict_policy", id.c_str()), [id](Config &k) { k.conflict_policy = id; });
        break;
    }
    case kConcurrency:
    {
        const int n = (c.download_concurrency - 1 + e.step + 4) % 4 + 1;
        app.set_config(int_json("download_concurrency", n), [n](Config &k) { k.download_concurrency = n; });
        break;
    }
    case kNotify:
        app.set_config(flag_json("notify", !c.notify), [](Config &k) { k.notify = !k.notify; });
        break;
    case kUpdateCheck:
        app.set_config(flag_json("update_check", !c.update_check), [](Config &k) { k.update_check = !k.update_check; });
        break;
    case kCovers:
        app.set_config(flag_json("cover_cache", !c.cover_cache), [](Config &k) { k.cover_cache = !k.cover_cache; });
        break;
    case kUpdate:
        if (app.update().available && app.update().self_update)
            app.install_update();
        else
            app.check_update();
        break;
    case kEmulators:
        emulators_asked_ = true;
        break;
    case kDiagnostics:
        diag_ = 1;
        app.post("/api/diagnostics", "{}", [this](const Response &r) {
            std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.ok() ? r.json() : nullptr, cJSON_Delete);
            const cJSON *usb = cJSON_GetObjectItemCaseSensitive(j.get(), "usb"),
                        *path = cJSON_GetObjectItemCaseSensitive(j.get(), "path");
            const cJSON *at = cJSON_IsString(usb) ? usb : path;
            diag_ = cJSON_IsString(at) ? 2 : 3;
            diag_where_ = cJSON_IsString(at) ? at->valuestring : "";
        });
        break;
    case kSetup:
    case kForget:
        if (armed_ != id)
        {
            armed_ = id;
            armed_age_ = 0.0f;
            break;
        }
        armed_ = -1;
        if (id == kSetup)
            app.setup_restart();
        else
            app.pair_forget();
        break;
    default:
        break;
    }
}

void SettingsPage::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                        float clock, float age) const
{
    const float in = tween::stagger(age, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    ui::text(list, fonts.display, "Settings", kMargin, 240, 64, look.text);
    list_.draw(list, fonts, look, rows(app), {kRows.x, kRows.y + 24 * (1.0f - in), kRows.w, kRows.h}, clock);

    // About
    const float appear = tween::stagger(age, 3, 0.08f, 0.6f);
    list.push_opacity(appear);
    const Rect panel{kAbout.x, kAbout.y + 24 * (1.0f - appear), kAbout.w, kAbout.h};
    draw_panel(list, look, panel);
    const Status &s = app.status();
    const float x = panel.x + 36;
    float y = panel.y + 64;
    auto item = [&](const char *label, const std::string &value) {
        ui::text(list, fonts.semibold, label, x, y, 16, look.panel_muted, hui::gfx::Align::left, 3.0f);
        // The server's address is as it was typed
        ui::text(list, fonts.regular, fonts.regular.font->fit(value.empty() ? "-" : plain_text(value.c_str()), 24, panel.w - 72), x,
                 y + 36, 24, look.panel_text);
        y += 80;
    };
    item("ROMM SERVER", s.server);
    item("SIGNED IN AS", s.user);
    item("ROMM VERSION", s.server_version);
    char web[96];
    std::snprintf(web, sizeof web, "http://%s:%d", s.ip.c_str(), s.port);
    item("ALSO ON YOUR PHONE OR COMPUTER", s.ip.empty() ? "" : web);
    const Update &u = app.update();
    item("STARTS WITH THE " ROMM_HERE_UPPER, !u.known ? "" : u.autostart ? "Yes" : "No: reopen " ROMM_APP " to set it up");
    list.line(panel.x + 36, panel.y + panel.h - 196, panel.x + panel.w - 36, panel.y + panel.h - 196, 1.5f, look.outline);
    draw_credits(list, fonts, look, {x, panel.y + panel.h - 168, panel.w - 72, 136}, on_credits_, clock);
    touch::zone(list, {x - 12, panel.y + panel.h - 180, 380, 160}, &on_credits_, 0, touch::kButton); // the portrait
    list.pop_opacity();
    list.pop_opacity();
}

} // namespace rommps
