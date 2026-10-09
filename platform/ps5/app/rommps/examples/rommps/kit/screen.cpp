// RommPS - the screen: Home, Library, Downloads and Settings for the RomM Sync payload.
// Copyright (C) 2026 BlackBearReloaded (the Aurora Shelf design it grew from)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grown from PS5_VKHomebrewUI's "Aurora Shelf" (src/concepts/aurora.cpp),
// copied in by PS5_VulkanTemplate's new-title.py. What the screen shows comes
// from the payload through rommps_app.hpp; the screen only draws it and sends
// what the pad asks for.
//
//   - L1 / R1 move between Home, Library and Downloads.
//   - Home: the sync at a glance; Cross syncs now.
//   - Library (library_page.hpp): the games on this console and RomM's
//     platforms; a platform's games in a grid with an A to Z bar; search. The
//     backdrop eases toward the focused cover's colours; Cross opens a frosted
//     sheet to download the game.
//   - Downloads: what's queued and running; Cross cancels one.
//   - Settings: the look, the payload's settings, its updates, the setup.
//
// Until the payload answers the screen says so; until it's paired with RomM
// and set up, the setup (setup_page.hpp) fills it. Every colour comes from
// the look picked in Settings (rommps_look.hpp): the cover's colours, or one
// of the kit's themes.

#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "rommps_app.hpp"
#include "rommps_look.hpp"
#include "library_page.hpp"
#include "settings_page.hpp"
#include "setup_page.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace hui::screen
{

namespace
{

using gfx::Color;
using gfx::Rect;
using rommps::App;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

constexpr float kMargin = 96.0f;

enum Tab
{
    kHome,
    kLibrary,
    kDownloads,
    kSettings,
    kTabs
};
constexpr const char *kTabNames[kTabs] = {"Home", "Library", "Downloads", "Settings"};

constexpr const char *kTechniques[] = {
    "The RomM Sync payload's state, read through its local API on a worker thread",
    "Covers fetched once, decoded a few per frame, their colours driving the backdrop",
    "Spring-driven shelves, card growth and a gliding focus ring (from Aurora Shelf)",
    "Frosted details sheet over the blurred screen",
    "The setup and settings on the pad, with the kit's keyboard and its thirty themes",
};

std::string ago(double seconds)
{
    char text[32];
    if (seconds < 60)
        return "just now";
    if (seconds < 3600)
        std::snprintf(text, sizeof text, "%d min ago", static_cast<int>(seconds / 60));
    else if (seconds < 86400)
        std::snprintf(text, sizeof text, "%d h ago", static_cast<int>(seconds / 3600));
    else
        std::snprintf(text, sizeof text, seconds < 2 * 86400 ? "%d day ago" : "%d days ago",
                      static_cast<int>(seconds / 86400));
    return text;
}

class RommPS final : public app::Concept
{
  public:
    explicit RommPS(app::Context &context) : context_(context), app_(rommps::app())
    {
        apply_palette(rommps::Palette{}, true);
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "rommps",
            "RommPS",
            "Your RomM library and save sync",
            "examples/rommps/kit/screen.cpp",
            audio::SoundSet::glass,
            Color::rgb(0x76d6ff),
            kTechniques,
        };
        return kInfo;
    }

    void enter() override
    {
        age_ = 0.0f;
        sheet_open_ = false;
    }

    void update(const InputFrame &input, float dt, app::Feedback &feedback) override
    {
        age_ += dt;
        clock_ += dt;
        app_.tick(dt, static_cast<gfx::Renderer *>(rommps_renderer()));
        cancel_seen_ = false;
        const bool ready = app_.status().paired && app_.status().setup_complete;

        // The Codec call (Settings, behind the author's portrait) takes the pad.
        const bool calling = settings_.call().active();
        if (calling)
            settings_.call().update(input, dt, feedback);
        if (ready && !sheet_open_ && !calling && !setup_.editing())
        {
            if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
            {
                tab_ = (tab_ + (input.is_pressed(Action::page_next) ? 1 : kTabs - 1)) % kTabs;
                tab_age_ = 0.0f;
                feedback.play(audio::Cue::tab);
                if (tab_ == kSettings)
                    settings_.enter(app_);
                if (tab_ == kLibrary)
                    library_.enter(app_);
            }
        }
        // Settings' Emulators opens that setup step on its own.
        if (settings_.take_emulators_request())
            setup_.edit_emulators();
        const bool setting_up = (!ready && app_.status().known && app_.status().reachable) || setup_.editing();
        if (setting_up && !setting_up_ && !setup_.editing())
            setup_.enter();
        setting_up_ = setting_up;
        if (calling)
            ;
        else if (setting_up)
            setup_.update(app_, input, dt, feedback);
        else if (!ready)
            ;
        else if (tab_ == kHome)
            update_home(input, feedback);
        else if (tab_ == kLibrary)
        {
            if (sheet_open_)
                update_sheet(input, dt, feedback);
            else if (library_.update(app_, input, dt, feedback))
                open_sheet();
        }
        else if (tab_ == kDownloads)
            update_downloads(input, dt, feedback);
        else
            settings_.update(app_, input, dt, feedback);

        // ---- animation state ----
        tab_age_ += dt;
        tab_position_.target = static_cast<float>(tab_);
        tab_position_.update(dt, 16.0f);
        // The backdrop eases toward the focused game's cover (or platform's).
        if (tab_ == kLibrary)
        {
            const std::string cover = sheet_open_ ? sheet_game_.cover : library_.backdrop_cover(app_);
            if (!cover.empty())
                apply_palette(app_.cover(cover).palette, false);
        }
        for (ui::SpringColor &colour : palette_)
            colour.update(dt, 4.0f);
        nudge_.update(dt, 9.0f);
        sheet_.target = sheet_open_ ? 1.0f : 0.0f;
        sheet_.update(dt, context_.settings.reduced_motion ? 40.0f : 13.0f);
        action_position_.target = static_cast<float>(action_);
        action_position_.update(dt, 22.0f);
        download_row_ = std::clamp(download_row_, 0, std::max(0, static_cast<int>(app_.downloads().size()) - 1));
        if (!cancel_seen_) // focus left the download (another row, tab or button): the hold is gone
        {
            cancel_hold_ = 0;
            cancel_job_ = 0;
        }
        download_position_.target = static_cast<float>(download_row_);
        download_position_.update(dt, 20.0f);
    }

    void draw(app::Frame &frame) const override
    {
        const rommps::Look &L = look();
        if (L.covers)
        {
            frame.backdrop.mode = gfx::BackdropMode::aurora;
            for (int i = 0; i < 4; ++i)
                frame.backdrop.colors[i] = palette_[i].value();
        }
        else
            frame.backdrop = L.backdrop;
        frame.backdrop.time = clock_;

        gfx::DrawList &list = frame.scene;
        const float back = sheet_.value;
        list.push_transform(1.0f - 0.035f * back, 960, 540, 0, 0);
        draw_top_bar(list);
        const bool ready = app_.status().paired && app_.status().setup_complete;
        if (setting_up_)
            setup_.draw(list, context_.fonts, L, app_, clock_);
        else if (!ready)
            draw_not_ready(list);
        else if (tab_ == kHome)
            draw_home(list);
        else if (tab_ == kLibrary)
            draw_library(list);
        else if (tab_ == kDownloads)
            draw_downloads(list);
        else
            settings_.draw(list, context_.fonts, L, app_, clock_, tab_age_);
        list.pop_transform();
        if (back > 0.01f)
        {
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, L.page.with_alpha(0.45f * back));
            frame.glass = true;
            draw_sheet(frame.overlay, frame.glass_texture);
        }
        if (settings_.call().active())
        {
            settings_.call().draw(frame.overlay, context_.fonts, clock_);
            return;
        }
        draw_toast(back > 0.5f ? frame.overlay : frame.scene);
        draw_hints(back > 0.5f ? frame.overlay : frame.scene);
    }

  private:
    // ---------------------------------------------------------------- look

    static const rommps::Look &look()
    {
        return rommps::look(rommps::current_look());
    }

    // Highlights: the focused cover's colour, or the theme's accent.
    Color accent() const
    {
        return look().covers ? palette_[3].value() : look().accent;
    }

    Color cover_accent(const rommps::Palette &palette) const
    {
        return look().covers ? palette.accent : look().accent;
    }

    // ---------------------------------------------------------------- data

    const rommps::Game *focused_game() const
    {
        return tab_ == kLibrary && !sheet_open_ ? library_.focused(app_) : nullptr;
    }

    // The backdrop's four colours, from the focused cover (or the default).
    void apply_palette(const rommps::Palette &p, bool snap)
    {
        const Color targets[4] = {
            gfx::mix(p.dark, Color::rgb(0x05060c), 0.35f),
            gfx::mix(p.dark, p.mid, 0.35f),
            p.mid,
            gfx::mix(p.mid, p.accent, 0.55f),
        };
        for (int i = 0; i < 4; ++i)
        {
            if (snap)
                palette_[i].snap(targets[i]);
            else
                palette_[i].target(targets[i]);
        }
    }

    // ---------------------------------------------------------------- input

    void refuse(app::Feedback &feedback, float direction)
    {
        feedback.play(audio::Cue::error, 1.0f, 0.0f, 0.6f);
        feedback.rumble(0.25f, 0.05f);
        nudge_direction_ = direction;
        nudge_.trigger();
    }

    void update_home(const InputFrame &input, app::Feedback &feedback)
    {
        if (input.is_pressed(Action::confirm))
        {
            if (app_.status().syncing)
            {
                refuse(feedback, 0.0f);
                return;
            }
            app_.sync_now();
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.5f, 0.12f);
        }
    }

    void open_sheet()
    {
        const rommps::Game *g = library_.focused(app_);
        if (!g)
            return;
        sheet_game_ = *g;
        sheet_open_ = true;
        action_ = 0;
        action_position_.snap(0.0f);
    }

    // The sheet's actions for the focused game, and whether each can be used.
    struct SheetAction
    {
        std::string label;
        bool enabled;
        int kind; // 0 download, 1 cancel, 2 close
    };

    std::vector<SheetAction> sheet_actions() const
    {
        std::vector<SheetAction> actions;
        const rommps::Game *g = &sheet_game_;
        const rommps::Platform *p = app_.platform_by_id(sheet_game_.platform_id);
        const rommps::Download *d = g ? app_.download_for(g->id) : nullptr;
        if (d && d->active())
            actions.push_back({"Hold to cancel download", true, 1});
        else if (g && g->installed)
            actions.push_back({"Downloaded", false, 0});
        else if (p && p->profile.empty())
            actions.push_back({"No emulator for this platform", false, 0});
        else
            actions.push_back({"Download", true, 0});
        actions.push_back({"Close", true, 2});
        return actions;
    }

    // Cancelling a download deletes what came so far, so it takes holding
    // Cross for a second, not a press: the row (or the sheet's button) fills
    // up meanwhile, and letting go before the end keeps the download.
    static constexpr float kCancelHold = 1.0f;

    bool hold_to_cancel(const InputFrame &input, float dt, const rommps::Download *d, app::Feedback &feedback)
    {
        cancel_seen_ = true;
        if (!d || !d->active() || !input.is_held(Action::confirm))
        {
            cancel_hold_ = 0;
            cancel_job_ = 0;
            return false;
        }
        if (input.is_pressed(Action::confirm))
        {
            cancel_job_ = d->id; // every hold starts with a press, from nothing
            cancel_hold_ = 0;
            feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.5f);
        }
        else if (cancel_job_ != d->id)
            return false; // held from before, or from another row
        if (cancel_hold_ < 0)
            return false; // done: waits for the button to come up
        cancel_hold_ += dt;
        if (cancel_hold_ < kCancelHold)
            return false;
        app_.cancel_download(d->id);
        feedback.play(audio::Cue::back);
        feedback.rumble(0.8f, 0.2f);
        cancel_hold_ = -1;
        return true;
    }

    // 0..1 while Cross is held on this download.
    float cancel_progress(int job_id) const
    {
        return job_id && job_id == cancel_job_ && cancel_hold_ > 0 ? std::min(1.0f, cancel_hold_ / kCancelHold) : 0.0f;
    }

    void update_sheet(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const std::vector<SheetAction> actions = sheet_actions();
        const int count = static_cast<int>(actions.size());
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int next = std::clamp(action_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != action_)
            {
                action_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
        action_ = std::clamp(action_, 0, count - 1);
        if (actions[static_cast<std::size_t>(action_)].kind == 1)
        {
            if (hold_to_cancel(input, dt, app_.download_for(sheet_game_.id), feedback))
                sheet_open_ = false;
        }
        else if (input.is_pressed(Action::confirm))
        {
            const SheetAction &a = actions[static_cast<std::size_t>(action_)];
            const rommps::Game *g = &sheet_game_;
            if (!a.enabled)
                refuse(feedback, 0.0f);
            else if (a.kind == 0 && g)
            {
                app_.download(g->id, g->name);
                feedback.play(audio::Cue::launch);
                feedback.rumble(0.7f, 0.18f);
                sheet_open_ = false;
            }
            else
            {
                feedback.play(audio::Cue::modal_close);
                sheet_open_ = false;
            }
        }
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            sheet_open_ = false;
        }
    }

    void update_downloads(const InputFrame &input, float dt, app::Feedback &feedback)
    {
        const int count = static_cast<int>(app_.downloads().size());
        // The list can get shorter at any tick (finished ones cleared, the payload restarted).
        download_row_ = std::clamp(download_row_, 0, std::max(0, count - 1));
        if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
        {
            const int next = std::clamp(download_row_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != download_row_)
            {
                download_row_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
        }
        if (input.is_pressed(Action::west) && count > 0)
        {
            const auto &downloads = app_.downloads();
            if (std::any_of(downloads.begin(), downloads.end(), [](const rommps::Download &d) { return d.finished(); }))
            {
                app_.clear_finished_downloads();
                download_row_ = 0;
                feedback.play(audio::Cue::back);
            }
            else
                refuse(feedback, 0.0f);
        }
        if (count > 0 && download_row_ < static_cast<int>(app_.downloads().size()))
        {
            const rommps::Download &d = app_.downloads()[static_cast<std::size_t>(download_row_)];
            if (d.active())
                hold_to_cancel(input, dt, &d, feedback);
            else if (input.is_pressed(Action::confirm))
                refuse(feedback, 0.0f);
        }
    }

    // ---------------------------------------------------------------- drawing

    void draw_top_bar(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const float in = tween::stagger(age_, 0, 0.05f, 0.5f);
        list.push_opacity(in);
        float x = kMargin;
        const bool ready = app_.status().paired && app_.status().setup_complete;
        ui::text(list, fonts.display, "RommPS", x, 96 - 10 * (1.0f - in), 34, L.text);
        x += 220;
        if (ready)
        {
            for (int i = 0; i < kTabs; ++i)
            {
                const bool active = i == tab_;
                const float w = ui::text(list, active ? fonts.semibold : fonts.regular, kTabNames[i], x,
                                         92 - 10 * (1.0f - in), 26, active ? L.text : L.muted);
                if (active)
                    list.rounded_rect({x, 104, w, 4}, 2, accent());
                x += w + 44;
            }
        }
        // Right: the server, or what's wrong.
        const rommps::Status &s = app_.status();
        const bool old_payload = s.reachable && app_.payload_too_old();
        std::string where = !s.known       ? "Connecting"
                            : !s.reachable ? "RomM Sync isn't running"
                            : old_payload  ? "RomM Sync " + s.version + " is too old: update it in Settings"
                            : s.paired     ? s.user + " @ " + s.server
                                           : "Not paired";
        const float dot_x = 1824 - 12 - ui::text(list, fonts.regular, context_.fonts.regular.font->fit(where, 22, 640), 1824, 90,
                                                 22, L.muted, gfx::Align::right);
        list.circle(dot_x - 12, 83, 7,
                    s.reachable && s.paired && !old_payload ? L.success : s.reachable ? L.warning : L.danger);
        list.pop_opacity();
    }

    void draw_panel(gfx::DrawList &list, const Rect &r, float radius = -1.0f) const
    {
        rommps::draw_panel(list, look(), r, radius);
    }

    // The payload isn't answering (the setup takes over once it does).
    void draw_not_ready(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const rommps::Status &s = app_.status();
        const float in = tween::stagger(age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const Rect panel{360, 300, 1200, 440};
        draw_panel(list, panel, 36);
        const float x = panel.x + 72;
        ui::text(list, fonts.display, s.known ? "RomM Sync isn't running" : "Connecting to RomM Sync", x, panel.y + 120, 56,
                 L.panel_text);
        ui::paragraph(list, fonts.regular,
                      "RommPS shows what the RomM Sync payload is doing. Load romm-sync.elf with your HEN's "
                      "payload loader, or turn on its autostart, and this screen fills in by itself.",
                      x, panel.y + 190, 28, 1060, 42, L.panel_muted, 4);
        if (s.known && !s.error.empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(s.error, 22, 1060), x, panel.y + 380, 22,
                     L.panel_muted.with_alpha(0.6f));
        list.pop_opacity();
    }

    void draw_home(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const rommps::Status &s = app_.status();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        const float x = kMargin + 30.0f * (1.0f - in);
        ui::text(list, fonts.semibold, "SAVE SYNC", x, 212, 20, accent(), gfx::Align::left, 4.0f);
        // The headline only says synced when the last sync went through.
        std::string headline = s.syncing                         ? "Syncing"
                               : !s.sync_count                   ? "Not synced yet"
                               : s.last_result != "OK"           ? "The last sync failed"
                               : s.conflicts == 1                ? "1 save needs you"
                               : s.conflicts > 1                 ? std::to_string(s.conflicts) + " saves need you"
                                                                 : "Your saves are synced";
        ui::text(list, fonts.display, headline, x - 4, 304, 80, L.text);
        std::string line = s.syncing         ? (s.phase.empty() ? "Working" : s.phase)
                           : s.sync_count    ? "Last sync " + ago(s.uptime - s.last_run) + "  \xC2\xB7  " + s.last_result
                                             : "No sync yet";
        ui::text(list, fonts.regular, context_.fonts.regular.font->fit(line, 28, 1500), x, 360, 28, L.muted);

        // Sync now
        const Rect button{x, 410, 260, 68};
        list.glow(button, 34, 18, accent().with_alpha(0.35f));
        list.rounded_rect(button, 34, s.syncing ? L.button.with_alpha(0.4f) : L.button);
        ui::draw_button(list, fonts, L.covers ? ui::GlyphStyle::light() : ui::GlyphStyle::dark(), ui::Button::cross,
                        button.x + 24, button.cy(), 34);
        ui::text(list, fonts.semibold, "Sync now", button.x + 76, button.cy() + 10, 28, L.on_button);

        // Tiles
        char value[64];
        struct Tile
        {
            const char *label;
            std::string value;
        } tiles[4];
        std::snprintf(value, sizeof value, "%d", s.tracked_saves);
        tiles[0] = {"SAVES TRACKED", value};
        std::snprintf(value, sizeof value, "%d", s.conflicts);
        tiles[1] = {"CONFLICTS", value};
        std::snprintf(value, sizeof value, "%d up \xC2\xB7 %d down", s.last_up, s.last_down);
        tiles[2] = {"LAST SYNC", s.sync_count ? value : "-"};
        tiles[3] = {"ROMM SYNC", "v" + s.version};
        for (int i = 0; i < 4; ++i)
        {
            const float appear = tween::stagger(tab_age_, 2 + i, 0.07f, 0.5f);
            const Rect tile{kMargin + static_cast<float>(i) * 432, 580 + 24 * (1.0f - appear), 408, 190};
            list.push_opacity(appear);
            draw_panel(list, tile, 26);
            ui::text(list, fonts.semibold, tiles[i].label, tile.x + 32, tile.y + 52, 17, L.panel_muted, gfx::Align::left,
                     3.0f);
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(tiles[i].value, 46, 350), tile.x + 32, tile.y + 128,
                     46, i == 1 && s.conflicts ? L.warning : L.panel_text);
            list.pop_opacity();
        }
        ui::text(list, fonts.regular, context_.fonts.regular.font->fit(s.server, 22, 1700), kMargin, 850, 22,
                 L.muted.with_alpha(0.7f * L.muted.a));
        list.pop_opacity();
    }

    void draw_library(gfx::DrawList &list) const
    {
        library_.draw(list, context_.fonts, look(), app_, accent(), clock_, tab_age_);
    }

    void draw_downloads(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const auto &downloads = app_.downloads();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.display, "Downloads", kMargin, 240, 64, L.text);
        // Top right: everything downloading at once, and what's waiting.
        double total_speed = 0;
        int running = 0, queued = 0;
        for (const rommps::Download &d : downloads)
        {
            if (d.running())
            {
                total_speed += d.speed;
                ++running;
            }
            else if (d.status == "queued")
                ++queued;
        }
        if (running)
        {
            ui::text(list, fonts.semibold, rommps::human_size(total_speed) + "/s", 1824, 228, 40, accent(), gfx::Align::right);
            char count[64];
            std::snprintf(count, sizeof count, queued ? "%d downloading  \xC2\xB7  %d waiting" : "%d downloading", running,
                          queued);
            ui::text(list, fonts.regular, count, 1824, 262, 22, L.muted, gfx::Align::right);
        }
        else if (queued)
        {
            char count[48];
            std::snprintf(count, sizeof count, "%d waiting", queued);
            ui::text(list, fonts.regular, count, 1824, 240, 24, L.muted, gfx::Align::right);
        }
        if (downloads.empty())
        {
            ui::text(list, fonts.regular, "Nothing downloading. Pick a game in Library to download it.", kMargin, 310, 28,
                     L.muted);
            list.pop_opacity();
            return;
        }
        constexpr float kRowH = 110.0f;
        const float top = 300.0f;
        const int first = std::max(0, download_row_ - 5);
        list.rounded_rect({kMargin - 24, top + (download_position_.value - static_cast<float>(first)) * kRowH, 1776, kRowH - 14},
                          26, L.text.with_alpha(0.1f));
        for (int i = first; i < static_cast<int>(downloads.size()) && i < first + 6; ++i)
        {
            const rommps::Download &d = downloads[static_cast<std::size_t>(i)];
            const float y = top + static_cast<float>(i - first) * kRowH;
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(d.name, 28, 1100), kMargin, y + 42, 28, L.text);
            std::string meta = d.platform + "  \xC2\xB7  ";
            if (d.running())
            {
                char text[96];
                std::snprintf(text, sizeof text, "%s of %s", rommps::human_size(d.done).c_str(),
                              rommps::human_size(d.total).c_str());
                meta += text;
            }
            else if (d.status == "error")
                meta += d.error;
            else
            {
                static const char *const kStates[][2] = {{"queued", "Waiting"},
                                                         {"cancelling", "Cancelling"},
                                                         {"cancelled", "Cancelled, nothing kept"},
                                                         {"done", "Downloaded"}};
                for (const auto &st : kStates)
                    if (d.status == st[0])
                        meta += st[1];
            }
            const float hold = cancel_progress(d.id);
            if (hold > 0)
                meta = "Keep holding to cancel";
            ui::text(list, fonts.regular, context_.fonts.regular.font->fit(meta, 22, 1200), kMargin, y + 80, 22,
                     d.status == "error" || hold > 0 ? L.danger : L.muted);
            const Rect bar{1360, y + 54, 420, 10};
            // Above the bar: the download's speed and how far it is.
            if (d.running())
            {
                ui::text(list, fonts.semibold, rommps::human_size(d.speed) + "/s", bar.x, y + 40, 24, L.text);
                char pct[16];
                std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(d.progress() * 100.0f));
                ui::text(list, fonts.regular, pct, bar.x + bar.w, y + 40, 22, L.muted, gfx::Align::right);
            }
            list.rounded_rect(bar, 5, L.muted.with_alpha(0.25f));
            const float progress = d.status == "done" ? 1.0f : d.progress();
            if (progress > 0)
                list.rounded_rect({bar.x, bar.y, std::max(10.0f, bar.w * progress), bar.h}, 5,
                                  d.status == "done" ? L.success : accent());
            if (hold > 0)
                list.rounded_rect({bar.x, bar.y, std::max(10.0f, bar.w * hold), bar.h}, 5, L.danger);
        }
        list.pop_opacity();
    }

    void draw_sheet(gfx::DrawList &list, std::uint32_t glass) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Game *g = &sheet_game_;
        if (!g)
            return;
        const rommps::Look &L = look();
        const rommps::Palette palette = app_.cover(g->cover).palette;
        const Color acc = cover_accent(palette);
        const rommps::Platform *p = app_.platform_by_id(sheet_game_.platform_id);
        const float t = sheet_.value;
        constexpr float kHeight = 520.0f;
        const Rect sheet{120, gfx::kVirtualHeight - kHeight * t - 40.0f * t + 60.0f * (1.0f - t), 1680, kHeight};
        list.push_opacity(tween::clamp01(t * 1.4f));
        list.shadow({sheet.x, sheet.y + 20, sheet.w, sheet.h}, 44, 60, Color::rgb(0x000000, 0.5f));
        list.glass(glass, sheet, 44, kWhite);
        list.rounded_rect(sheet, 44, L.covers ? gfx::mix(palette.dark, kInk, 0.5f).with_alpha(0.62f) : L.panel);
        list.bordered_rect(sheet, 44, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);

        const Rect art{sheet.x + 56, sheet.y + 56, 300, 400};
        rommps::draw_cover(list, fonts, L, app_, g, art, 1.0f, 26);
        const float x = art.x + art.w + 56;
        ui::text(list, fonts.semibold, ui::upper(p ? p->name : ""), x, sheet.y + 92, 20, acc, gfx::Align::left, 4.0f);
        float y = ui::paragraph(list, fonts.display, g->name, x - 2, sheet.y + 156, 54, 640, 62, L.panel_text, 2) - 18;
        if (g->versions > 1)
        {
            const std::string v = g->version + "  \xC2\xB7  1 of " + std::to_string(g->versions) + " versions";
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(v, 22, 660), x, y, 22, L.panel_text.with_alpha(0.7f));
            y += 38;
        }
        // RomM's details: year, publisher, genres; the summary; then the numbers.
        const rommps::Details &info = app_.details(g->id);
        const std::string about = rommps::details_line(info);
        if (!about.empty())
        {
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(about, 22, 660), x, y, 22, acc);
            y += 38;
        }
        if (!info.summary.empty())
            y = ui::paragraph(list, fonts.regular, info.summary, x, y, 20, 660, 28, L.panel_text.with_alpha(0.85f), 3) + 6;
        std::vector<std::string> facts;
        if (info.play_ms > 0)
            facts.push_back("Played " + rommps::play_time(info.play_ms));
        if (!info.last_played.empty())
            facts.push_back("Last played " + info.last_played.substr(0, 10));
        static const char *const kStatus[][2] = {{"incomplete", "Playing"}, {"finished", "Finished"},
                                                 {"completed_100", "Completed 100%"}, {"retired", "Retired"},
                                                 {"never_playing", "Not for me"}};
        for (const auto &st : kStatus)
            if (info.status == st[0])
                facts.push_back(st[1]);
        if (info.main_story_s > 0)
            facts.push_back("Main story ~" + std::to_string(static_cast<int>(info.main_story_s / 3600.0 + 0.5)) + " h");
        if (!info.players.empty())
            facts.push_back(info.players + (info.players == "1" ? " player" : " players"));
        if (info.rating > 0)
        {
            char r[24];
            std::snprintf(r, sizeof r, info.rating > 10 ? "%.0f/100" : "%.1f/10", info.rating);
            facts.push_back(r);
        }
        std::string line;
        for (const std::string &f : facts)
            line += (line.empty() ? "" : "  \xC2\xB7  ") + f;
        // The file and emulator, last.
        std::string file = rommps::human_size(g->size) + (g->multi ? ", several files" : "");
        if (p && !p->profile.empty())
            file += "  \xC2\xB7  plays with " + p->profile;
        y = std::max(y, sheet.y + 330.0f);
        if (!line.empty())
        {
            ui::paragraph(list, fonts.regular, line, x, y, 20, 660, 28, L.panel_muted, 2);
            y += 60;
        }
        ui::text(list, fonts.regular, context_.fonts.regular.font->fit(file, 20, 660), x, std::min(y, sheet.y + 470.0f), 20,
                 L.panel_muted);

        const std::vector<SheetAction> actions = sheet_actions();
        const float ax = sheet.x + sheet.w - 56 - 460;
        const float ay = sheet.y + 76;
        list.rounded_rect({ax, ay + action_position_.value * 84, 460, 72}, 36, L.highlight);
        for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        {
            const bool focused = i == action_;
            const float y = ay + static_cast<float>(i) * 84;
            if (!focused)
                list.bordered_rect({ax, y, 460, 72}, 36, L.panel_text.with_alpha(0.06f), 1.5f, L.outline);
            const SheetAction &a = actions[static_cast<std::size_t>(i)];
            if (a.kind == 1 && focused)
                if (const rommps::Download *d = app_.download_for(g->id))
                    if (const float hold = cancel_progress(d->id); hold > 0)
                        list.rounded_rect({ax, y, std::max(72.0f, 460 * hold), 72}, 36, L.danger.with_alpha(0.85f));
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(a.label, 26, 400), ax + 36, y + 46, 26,
                     (focused ? L.on_highlight : L.panel_text).with_alpha(a.enabled ? 1.0f : 0.5f));
        }
        list.pop_opacity();
    }

    void draw_toast(gfx::DrawList &list) const
    {
        const std::string &message = app_.toast();
        if (message.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float a = tween::clamp01(app_.toast_age() * 5.0f) * tween::clamp01((4.0f - app_.toast_age()) * 2.0f);
        list.push_opacity(a);
        const std::string fitted = fonts.regular.font->fit(message, 24, 1100);
        const rommps::Look &L = look();
        const Rect r{kMargin, 960, 1180, 64};
        list.rounded_rect(r, 32, L.covers ? Color::rgb(0x14182e, 0.92f) : L.page.with_alpha(0.95f));
        list.bordered_rect(r, 32, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);
        ui::text(list, fonts.regular, fitted, r.x + 32, r.y + 41, 24, L.text);
        list.pop_opacity();
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = look().glyphs();
        if (setting_up_)
        {
            const std::vector<ui::Hint> hints = setup_.hints();
            ui::draw_hints(list, fonts, style, hints.data(), static_cast<int>(hints.size()), 1824, true);
            return;
        }
        const bool ready = app_.status().paired && app_.status().setup_complete;
        if (!ready)
            return;
        if (sheet_open_)
        {
            const ui::Hint hints[] = {{ui::Button::cross, "Choose"}, {ui::Button::circle, "Back"}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
            return;
        }
        list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - sheet_.value));
        const ui::Hint pages{ui::Button::l1, "Pages", ui::Button::r1};
        if (tab_ == kHome)
        {
            const ui::Hint hints[] = {pages, {ui::Button::cross, "Sync now"}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
        }
        else if (tab_ == kLibrary)
        {
            const std::vector<ui::Hint> hints = library_.hints();
            ui::draw_hints(list, fonts, style, hints.data(), static_cast<int>(hints.size()), 1824, true);
        }
        else if (tab_ == kDownloads)
        {
            // Cross on a download that's still going stops it and deletes what
            // came so far; Square takes the finished ones off the list.
            const auto &downloads = app_.downloads();
            const bool on_active = download_row_ < static_cast<int>(downloads.size()) &&
                                   downloads[static_cast<std::size_t>(download_row_)].active();
            const bool any_finished = std::any_of(downloads.begin(), downloads.end(),
                                                  [](const rommps::Download &d) { return d.finished(); });
            std::vector<ui::Hint> hints{pages};
            if (any_finished)
                hints.push_back({ui::Button::square, "Clear finished"});
            if (on_active)
                hints.push_back({ui::Button::cross, "Hold to cancel and delete"});
            ui::draw_hints(list, fonts, style, hints.data(), static_cast<int>(hints.size()), 1824, true);
        }
        else
        {
            const ui::Hint hints[] = {pages, {ui::Button::cross, settings_.cross_hint(app_)}};
            ui::draw_hints(list, fonts, style, hints, 2, 1824, true);
        }
        list.pop_opacity();
    }

    app::Context &context_;
    App &app_;
    int tab_ = kLibrary;
    float tab_age_ = 0.0f;
    tween::Spring tab_position_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    int shown_id_ = -1;
    ui::SpringColor palette_[4];
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    bool sheet_open_ = false;
    tween::Spring sheet_;
    int action_ = 0;
    tween::Spring action_position_;
    int download_row_ = 0;
    float cancel_hold_ = 0; // seconds Cross has been held to cancel cancel_job_; -1 once done
    int cancel_job_ = 0;
    bool cancel_seen_ = false; // hold_to_cancel ran this frame
    tween::Spring download_position_;
    rommps::SettingsPage settings_;
    rommps::LibraryPage library_;
    rommps::Game sheet_game_; // the game the details sheet is for
    rommps::SetupPage setup_;
    bool setting_up_ = false;
};

} // namespace

std::unique_ptr<app::Concept> make_rommps(app::Context &context)
{
    return std::make_unique<RommPS>(context);
}

} // namespace hui::screen

// The title's screen, for its program (ps5/ui/kit.hpp declares it)
namespace ps5ui
{
std::unique_ptr<hui::app::Concept> make_screen(hui::app::Context &context)
{
    return hui::screen::make_rommps(context);
}
} // namespace ps5ui
