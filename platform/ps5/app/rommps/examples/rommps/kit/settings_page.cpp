// RommPS: the Settings tab. See settings_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_page.hpp"

#include "core/tween.hpp"

#include <algorithm>
#include <cstdio>

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

std::vector<Row> SettingsPage::rows(const App &app) const
{
    const Config &c = app.config();
    const Update &u = app.update();
    std::vector<Row> r(kCount);
    const Look &l = look(current_look());
    r[kLook] = {RowKind::choice, "Look", l.name, l.family};
    r[kInterval] = {RowKind::choice, "Sync every", interval_label(c.sync_interval_min), "Besides the syncs below"};
    r[kOnExit] = {RowKind::toggle, "Sync when a game closes", "", "Uploads what the game saved", c.sync_on_game_exit};
    r[kOnStart] = {RowKind::toggle, "Sync when a game starts", "", "Fetches newer saves before you play",
                   c.sync_on_game_start};
    r[kOnChange] = {RowKind::toggle, "Upload saves as they change", "", "While you play", c.sync_on_change};
    r[kStates] = {RowKind::choice, "Save states", label_of(rommps::kStates, c.states), "Besides the saves themselves"};
    r[kPolicy] = {RowKind::choice, "A save changed in both places", label_of(kPolicies, c.conflict_policy),
                  "On this console and on RomM since the last sync"};
    r[kConcurrency] = {RowKind::choice, "Downloads at once", std::to_string(c.download_concurrency), ""};
    r[kNotify] = {RowKind::toggle, "Notifications", "", "On the console when a sync ends or fails", c.notify};
    r[kUpdateCheck] = {RowKind::toggle, "Look for updates every day", "", "", c.update_check};
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
            std::snprintf(detail, sizeof detail, "All %d covers are on the console", s.covers_total);
        else
            std::snprintf(detail, sizeof detail, "Faster browsing: every game's cover, kept up to date");
        r[kCovers] = {RowKind::toggle, "Keep all covers on the console", "", detail, c.cover_cache};
    }

    Row &update = r[kUpdate];
    update.kind = RowKind::action;
    update.label = "RomM Sync " + (u.current.empty() ? app.status().version : u.current);
    if (!u.self_update)
    {
        // RommPS brings RomM Sync with it: the store updates both.
        update.label = std::string("RommPS ") + App::kMinPayload;
        update.value = u.state == "checking" ? "Checking" : "Check for updates";
        update.enabled = u.state != "checking";
        update.detail = u.available           ? "RommPS " + u.latest + " is out: update it in ProsperoStore"
                        : u.state == "error"  ? (u.error.empty() ? u.message : u.error)
                        : u.latest.empty()    ? "Updates come from ProsperoStore, with RomM Sync in them"
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
        update.detail = "RomM Sync restarts with the new version";
    }
    else
    {
        update.value = "Check for updates";
        update.detail = u.state == "error" ? (u.error.empty() ? u.message : u.error)
                        : u.state == "pending" ? "Restart the console to finish installing"
                        : u.latest.empty()   ? ""
                                               : "Up to date";
    }

    r[kEmulators] = {RowKind::action, "Emulators", "", "Which emulators RomM Sync uses, and for which systems"};
    r[kSetup] = {RowKind::action, "Set up again", "", "Pick your emulators again. Background syncs wait until it's done"};
    r[kForget] = {RowKind::action, "Unpair from RomM", "", "Files on this console are kept"};
    r[kForget].danger = true;
    if (armed_ == kSetup || armed_ == kForget)
        r[static_cast<std::size_t>(armed_)].value = "Press again to confirm";
    if (!c.known)
        for (int i = kInterval; i <= kCovers; ++i)
            r[static_cast<std::size_t>(i)].enabled = false;
    return r;
}

const char *SettingsPage::cross_hint(const App &app) const
{
    if (on_credits_)
        return "Answer";
    const std::vector<Row> r = rows(app);
    const Row &row = r[static_cast<std::size_t>(std::clamp(list_.focus(), 0, kCount - 1))];
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

void SettingsPage::update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    avatar_texture(static_cast<hui::gfx::Renderer *>(rommps_renderer()));
    const std::vector<Row> r = rows(app);
    armed_age_ += dt;
    // Below the last row: the author's portrait, and the call behind it.
    if (on_credits_)
    {
        list_.update(dt, kCount, kRows.h);
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
    if (input.nav == hui::Direction::down && !input.nav_repeat && list_.focus() == kCount - 1)
    {
        on_credits_ = true;
        feedback.play(hui::audio::Cue::focus);
        return;
    }
    if (armed_ >= 0 && (armed_age_ > kArmedFor || list_.focus() != armed_))
        armed_ = -1;
    const RowEvent e = list_.handle(r, input, feedback);
    list_.update(dt, kCount, kRows.h);
    if (e.row < 0)
        return;
    const Config &c = app.config();
    switch (e.row)
    {
    case kLook:
        set_look((current_look() + e.step + look_count()) % look_count());
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
    case kSetup:
    case kForget:
        if (armed_ != e.row)
        {
            armed_ = e.row;
            armed_age_ = 0.0f;
            break;
        }
        armed_ = -1;
        if (e.row == kSetup)
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
        ui::text(list, fonts.regular, fonts.regular.font->fit(value.empty() ? "-" : value, 24, panel.w - 72), x, y + 36, 24,
                 look.panel_text);
        y += 80;
    };
    item("ROMM SERVER", s.server);
    item("SIGNED IN AS", s.user);
    item("ROMM VERSION", s.server_version);
    char web[96];
    std::snprintf(web, sizeof web, "http://%s:%d", s.ip.c_str(), s.port);
    item("ALSO ON YOUR PHONE OR COMPUTER", s.ip.empty() ? "" : web);
    const Update &u = app.update();
    item("STARTS WITH THE CONSOLE", !u.known ? "" : u.autostart ? "Yes" : "No: turn it on in Set up again");
    list.line(panel.x + 36, panel.y + panel.h - 196, panel.x + panel.w - 36, panel.y + panel.h - 196, 1.5f, look.outline);
    draw_credits(list, fonts, look, {x, panel.y + panel.h - 168, panel.w - 72, 136}, on_credits_, clock);
    list.pop_opacity();
    list.pop_opacity();
}

} // namespace rommps
