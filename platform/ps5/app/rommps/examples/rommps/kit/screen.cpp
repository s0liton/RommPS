// RommPS - the screen: Home, Library, Downloads, Queue and Settings for the RomM Sync payload.
// Copyright (C) 2026 BlackBearReloaded (the Aurora Shelf design it grew from)
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Grown from PS5_VKHomebrewUI's "Aurora Shelf" (src/concepts/aurora.cpp),
// copied in by PS5_VulkanTemplate's new-title.py. What the screen shows comes
// from the payload through rommps_app.hpp; the screen only draws it and sends
// what the pad asks for.
//
//   - L1 / R1 move between Home, Library, Downloads, Queue and Settings.
//   - Home: the sync at a glance; Cross syncs now. Beside it: the conflicts
//     to settle, the sync log and the saves kept in sync (sync_views.hpp).
//   - Library (library_page.hpp): RomM's platforms; a platform's games in a
//     grid with an A to Z bar; search. The backdrop eases toward the focused
//     cover's colours; Cross opens a frosted sheet to download the game.
//   - Downloads: the games on this console, most recently played first;
//     Cross opens one's details sheet, where a held Cross deletes it.
//   - Queue: what's queued and running; Cross cancels one.
//   - Settings: the look, the payload's settings, its updates, the setup.
//
// Until the payload answers the screen says so; until it's paired with RomM
// and set up, the setup (setup_page.hpp) fills it. Every colour comes from
// the look picked in Settings (rommps_look.hpp): the cover's colours, or one
// of the kit's themes.

#include "cJSON.h"
#include "concepts/concepts.hpp"

#include "core/tween.hpp"
#include "rommps_app.hpp"
#include "rommps_look.hpp"
#include "library_page.hpp"
#include "settings_page.hpp"
#include "setup_page.hpp"
#include "sync_views.hpp"
#include "touch.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace hui::screen
{

namespace
{

using gfx::Color;
using gfx::Rect;
using rommps::App;
namespace touch = rommps::touch;

const Color kWhite = Color::rgb(0xffffff);
const Color kInk = Color::rgb(0x0b0d16);

constexpr float kMargin = 96.0f;

enum Tab
{
    kHome,
    kLibrary,
    kDownloads,
    kQueue,
    kSettings,
    kTabs
};
constexpr const char *kTabNames[kTabs] = {"Home", "Library", "Downloads", "Queue", "Settings"};

constexpr const char *kTechniques[] = {
    "The background service's state, read through its local API on a worker thread",
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
        // Before the first answer is read (that's in update): names keep what these fonts can draw
        rommps::set_text_fonts(context.fonts);
        apply_palette(rommps::Palette{}, true);
    }

    const app::ConceptInfo &info() const override
    {
        static const app::ConceptInfo kInfo{
            "rommps",
            ROMM_APP,
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
        update_frame(input, dt, feedback);
        // Interface sounds turned off in Settings: what this frame asked for
        // isn't played. Rumble still is.
        if (!rommps::ui_sounds())
            feedback.cues.clear();
    }

    void update_frame(const InputFrame &input, float dt, app::Feedback &feedback)
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
        if (ready && !sheet_open_ && !calling && !setup_.editing() && !view_.active())
        {
            int to = -1;
            if (input.is_pressed(Action::page_prev) || input.is_pressed(Action::page_next))
                to = (tab_ + (input.is_pressed(Action::page_next) ? 1 : kTabs - 1)) % kTabs;
            // A tab touched in the top bar
            if (const touch::Touch t = touch::take(&tab_); t.pressed && t.index >= 0 && t.index < kTabs && t.index != tab_)
                to = t.index;
            if (to >= 0)
            {
                tab_ = to;
                tab_age_ = 0.0f;
                feedback.play(audio::Cue::tab);
                if (tab_ == kSettings)
                    settings_.enter(app_);
                if (tab_ == kDownloads)
                    app_.refresh_installed();
                if (tab_ == kLibrary)
                {
                    library_.enter(app_);
                    app_.reload_platforms(); // emulators may have changed meanwhile
                }
            }
        }
        // RetroArch 2.0 looks for games in new folders: asked once a launch.
        if (ready && !move_asked_ && app_.status().retroarch_move > 0 && !calling && !setup_.editing())
        {
            move_asked_ = move_open_ = true;
            move_list_.set_focus(0);
            feedback.play(audio::Cue::modal_open);
        }
        if (move_open_)
        {
            update_move(input, dt, feedback);
            return;
        }
        // Collections, new in 1.4.0, need a pairing that asked for them: said
        // once a launch while RomM won't show them.
        if (ready && !collections_checked_)
        {
            collections_checked_ = true;
            app_.want_collections();
        }
        if (ready && !pair_asked_ && app_.collections_need_pair() && !calling && !setup_.editing())
        {
            pair_asked_ = pair_open_ = true;
            pair_list_.set_focus(0);
            feedback.play(audio::Cue::modal_open);
        }
        if (pair_open_)
        {
            update_pair(input, dt, feedback);
            return;
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
        {
            // RomM Sync not running and RommPS couldn't start it: Cross tries again.
            if (app_.status().known && !app_.status().reachable && !app_.payload_starting() &&
                (input.is_pressed(Action::confirm) || touch::take(this).pressed))
            {
                app_.start_payload();
                feedback.play(audio::Cue::focus);
            }
        }
        else if (view_.active())
            view_.update(app_, input, dt, feedback);
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
        {
            if (sheet_open_)
                update_sheet(input, dt, feedback);
            else
                update_downloads(input, dt, feedback);
        }
        else if (tab_ == kQueue)
            update_queue(input, dt, feedback);
        else
            settings_.update(app_, input, dt, feedback);

        // ---- animation state ----
        tab_age_ += dt;
        tab_position_.target = static_cast<float>(tab_);
        tab_position_.update(dt, 16.0f);
        // The backdrop eases toward the focused game's cover (or the platform's).
        if (tab_ == kLibrary || tab_ == kDownloads)
        {
            const std::string cover = sheet_open_         ? sheet_game_.cover
                                      : tab_ == kLibrary  ? library_.backdrop_cover(app_)
                                                          : (downloaded_game() ? downloaded_game()->cover : "");
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
        summary_scroll_.target = summary_target_;
        summary_scroll_.update(dt, 14.0f);
        queue_row_ = std::clamp(queue_row_, 0, std::max(0, static_cast<int>(app_.queue().size()) - 1));
        if (!cancel_seen_) // focus left the download (another row, tab or button): the hold is gone
        {
            cancel_hold_ = 0;
            cancel_job_ = 0;
        }
        queue_position_.target = static_cast<float>(queue_row_);
        queue_position_.update(dt, 20.0f);
        // The six-row window, eased so the list scrolls with the focus.
        const int qcount = static_cast<int>(app_.queue().size());
        if (!queue_free_)
            queue_scroll_.target = static_cast<float>(std::clamp(queue_row_ - 5, 0, std::max(0, qcount - 6)));
        queue_scroll_.update(dt, 14.0f);

        installed_position_.target = static_cast<float>(installed_row_);
        installed_position_.update(dt, 20.0f);
        const int icount = static_cast<int>(app_.installed().size());
        if (!installed_free_)
            installed_scroll_.target = static_cast<float>(std::clamp(installed_row_ - 5, 0, std::max(0, icount - 6)));
        installed_scroll_.update(dt, 14.0f);
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
        list.push_transform(1.0f - 0.035f * back, kScreenW / 2, kScreenH / 2, 0, 0);
        draw_top_bar(list);
        const bool ready = app_.status().paired && app_.status().setup_complete;
        if (setting_up_)
            setup_.draw(list, context_.fonts, L, app_, clock_);
        else if (!ready)
            draw_not_ready(list);
        else if (view_.active())
            view_.draw(list, context_.fonts, L, app_, clock_);
        else if (tab_ == kHome)
            draw_home(list);
        else if (tab_ == kLibrary)
            draw_library(list);
        else if (tab_ == kDownloads)
            draw_downloads(list);
        else if (tab_ == kQueue)
            draw_queue(list);
        else
            settings_.draw(list, context_.fonts, L, app_, clock_, tab_age_);
        list.pop_transform();
        if (back > 0.01f)
        {
            list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, L.page.with_alpha(0.45f * back));
            frame.glass = true;
            draw_sheet(frame.overlay, frame.glass_texture);
            if (versions_open_)
                draw_versions(frame.overlay);
        }
        if (settings_.call().active())
        {
            settings_.call().draw(frame.overlay, context_.fonts, clock_);
            return;
        }
        if (move_open_)
            draw_move(frame.overlay);
        if (pair_open_)
            draw_pair(frame.overlay);
        // The hints first: the toast keeps clear of them.
        hints_left_ = kScreenW;
        const bool asking = move_open_ || pair_open_;
        if (!asking)
            draw_hints(back > 0.5f ? frame.overlay : frame.scene);
        draw_toast(back > 0.5f || asking ? frame.overlay : frame.scene);
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

    // The game the Downloads list's focus is on (nullptr when there is none).
    const rommps::Game *downloaded_game() const
    {
        const auto &games = app_.installed();
        return installed_row_ >= 0 && installed_row_ < static_cast<int>(games.size())
                   ? &games[static_cast<std::size_t>(installed_row_)]
                   : nullptr;
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

    // Home's buttons: Sync now, then the lists beside it.
    static constexpr int kHomeButtons = 4;

    void update_home(const InputFrame &pad, app::Feedback &feedback)
    {
        InputFrame input = pad;
        if (!app_.config().save_sync)
            return; // nothing to sync: Settings turns it back on
        if (const touch::Touch t = touch::take(&home_focus_); t.index >= 0 && t.index < kHomeButtons)
        {
            home_focus_ = t.index;
            input = touch::with(input, t);
        }
        if (input.nav == Direction::left || input.nav == Direction::right)
        {
            const int next = home_focus_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < kHomeButtons)
            {
                home_focus_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, input.nav == Direction::right ? 1.0f : -1.0f);
        }
        if (!input.is_pressed(Action::confirm))
            return;
        if (home_focus_ == 0)
        {
            if (app_.status().syncing)
            {
                refuse(feedback, 0.0f);
                return;
            }
            app_.sync_now();
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.5f, 0.12f);
            return;
        }
        static const rommps::SyncView::Kind kinds[] = {rommps::SyncView::kConflicts, rommps::SyncView::kActivity,
                                                       rommps::SyncView::kTracked};
        view_.open(app_, kinds[home_focus_ - 1]);
        feedback.play(audio::Cue::select);
    }

    void open_sheet(const rommps::Game &g)
    {
        sheet_game_ = g;
        app_.check_on_console(g.id);
        sheet_open_ = true;
        addons_open_ = false;
        addons_drag_ = 0.0f;
        versions_open_ = false;
        remove_hold_ = -1; // a hold from before doesn't carry over
        action_ = 0;
        action_position_.snap(0.0f);
        summary_scroll_.snap(0.0f);
        summary_target_ = summary_hold_ = summary_max_ = 0.0f;
    }

    void open_sheet()
    {
        if (const rommps::Game *g = library_.focused(app_))
            open_sheet(*g);
    }

    // The sheet's actions for the focused game, and whether each can be used.
    struct SheetAction
    {
        std::string label;
        bool enabled;
        int kind; // 0 download, 1 cancel, 3 remove, 4 saves and states, 5 updates and DLC, 6 versions (Circle closes the sheet)
    };

    std::vector<SheetAction> sheet_actions() const
    {
        std::vector<SheetAction> actions;
        const rommps::Game *g = &sheet_game_;
        const rommps::Platform *p = app_.platform_by_id(sheet_game_.platform_id);
        const rommps::Download *d = g ? version_download() : nullptr;
        const rommps::Details &info = app_.details(sheet_game_.id);
        if (!info.versions.empty() && !(p && p->profile.empty()))
        {
            // Every version of the game, in a list of its own, which shows
            // (and cancels) what's downloading too, so another can be picked.
            actions.push_back({"Download", true, 6});
            if (version_on_console())
                actions.push_back({"Saves and states", true, 4});
        }
        else if (d && d->syncing())
            actions.push_back({"Syncing saves", false, 0});
        else if (d && d->active())
            actions.push_back({"Hold to cancel download", true, 1});
        else if (g && app_.on_console(*g))
        {
            actions.push_back({"Hold to delete this game", true, 3});
            actions.push_back({"Saves and states", true, 4});
        }
        else if (p && p->profile.empty())
            actions.push_back({"No emulator for this platform", false, 0});
        else if (g->archived || !info.archived.empty())
            actions.push_back({"Archived: can't download", false, 0});
        else
            actions.push_back({"Download", true, 0});
        // A Switch game's update and DLC, which come with it.
        if (!info.addons.empty())
            actions.push_back({std::string(info.bundle ? "In this folder (" : "Updates and DLC (") +
                                   std::to_string(info.addons.size()) + ")",
                               true, 5});
        return actions;
    }

    // A download of the sheet's game, whichever of its versions it is.
    const rommps::Download *version_download() const
    {
        if (const rommps::Download *d = app_.download_for(sheet_game_.id))
            return d;
        for (const rommps::Details::Version &v : app_.details(sheet_game_.id).versions)
            if (const rommps::Download *d = app_.download_for(v.id))
                return d;
        return nullptr;
    }

    // How far the sheet's game's downloads are, every version's together; -1
    // when none is going.
    float download_progress() const
    {
        std::vector<int> ids{sheet_game_.id};
        for (const rommps::Details::Version &v : app_.details(sheet_game_.id).versions)
            ids.push_back(v.id);
        double done = 0, total = 0;
        bool any = false;
        for (const rommps::Download &d : app_.queue())
            if (d.active() && std::find(ids.begin(), ids.end(), d.rom_id) != ids.end())
                any = true, done += d.done, total += d.total;
        return !any ? -1.0f : total > 0 ? static_cast<float>(done / total) : 0.0f;
    }

    // The version of the sheet's game that's on the console (its saves are
    // that one's), else 0.
    int version_on_console() const
    {
        if (app_.on_console(sheet_game_.id))
            return sheet_game_.id;
        for (const rommps::Details::Version &v : app_.details(sheet_game_.id).versions)
            if (app_.on_console(v.id))
                return v.id;
        return 0;
    }

    // Cancelling a download deletes what came so far, so it takes holding
    // Cross for a second, not a press: the row (or the sheet's button) fills
    // up meanwhile, and letting go before the end keeps the download.
    static constexpr float kCancelHold = 1.0f;

    bool hold_to_cancel(const InputFrame &input, float dt, const rommps::Download *d, app::Feedback &feedback)
    {
        cancel_seen_ = true;
        if (!d || !d->cancellable() || !input.is_held(Action::confirm))
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

    // rom_ids: the game, or every disc of a set; one notification for them
    // all. disc: one disc of a folder of discs.
    bool hold_to_remove(const InputFrame &input, float dt, app::Feedback &feedback, const std::vector<int> &rom_ids,
                        int disc = 0)
    {
        if (!input.is_held(Action::confirm))
        {
            remove_hold_ = -1;
            return false;
        }

        if (input.is_pressed(Action::confirm))
        {
            remove_hold_ = 0; // every hold begins with a press
            feedback.play(audio::Cue::tick, 1.0f, 0.0f, 0.5f);
        }

        if (remove_hold_ < 0)
            return false; // was held from before or is already done

        remove_hold_ += dt;

        if (remove_hold_ < kCancelHold)
            return false;

        for (std::size_t i = 0; i < rom_ids.size(); ++i)
            app_.remove_game(rom_ids[i], i + 1 < rom_ids.size(), disc);
        feedback.play(audio::Cue::back);
        feedback.rumble(0.8f, 0.2f);
        remove_hold_ = -1;
        return true;
    }

    // Rows of the update and DLC list that fit the sheet at once.
    static constexpr int kAddonRows = 6;

    void update_sheet(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        // Touch: outside the sheet is Circle; a drag on it scrolls what it shows.
        input = touch::with(input, touch::take(&sheet_open_), Action::back);
        const touch::Touch panel = touch::take(&summary_scroll_);
        if (versions_open_)
        {
            update_versions(input, dt, feedback);
            return;
        }
        if (addons_open_)
        {
            const int n = static_cast<int>(app_.details(sheet_game_.id).addons.size());
            // Dragged: a row (40 pixels) at a time.
            addons_drag_ += panel.scroll;
            for (; addons_drag_ >= 40.0f; addons_drag_ -= 40.0f)
                addons_top_ = std::min(addons_top_ + 1, std::max(0, n - kAddonRows));
            for (; addons_drag_ <= -40.0f; addons_drag_ += 40.0f)
                addons_top_ = std::max(addons_top_ - 1, 0);
            if (input.nav == Direction::up || input.nav == Direction::down)
            {
                const int next = std::clamp(addons_top_ + (input.nav == Direction::down ? 1 : -1), 0,
                                            std::max(0, n - kAddonRows));
                if (next != addons_top_)
                {
                    addons_top_ = next;
                    feedback.play(audio::Cue::focus, 1.0f, 0.35f);
                }
            }
            if (input.is_pressed(Action::back) || input.is_pressed(Action::confirm))
            {
                feedback.play(audio::Cue::back);
                addons_open_ = false;
            }
            return;
        }
        const std::vector<SheetAction> actions = sheet_actions();
        const int count = static_cast<int>(actions.size());
        if (const touch::Touch t = touch::take(&action_); t.index >= 0 && t.index < count)
        {
            action_ = t.index;
            input = touch::with(input, t);
        }
        if (panel.scroll != 0.0f && summary_max_ > 0.0f)
        {
            summary_target_ = std::clamp(summary_target_ + panel.scroll, 0.0f, summary_max_);
            if (panel.direct)
                summary_scroll_.snap(summary_target_);
        }
        // A press of Up or Down moves between the actions; holding it (D-pad or
        // left stick) past the repeat delay scrolls a long description instead.
        const float held = input.is_held(Action::down) || input.stick_y > 0.5f ? 1.0f
                           : input.is_held(Action::up) || input.stick_y < -0.5f ? -1.0f
                                                                                 : 0.0f;
        summary_hold_ = held != 0.0f ? summary_hold_ + dt : 0.0f;
        const bool scrolling = summary_max_ > 0.0f && summary_hold_ > 0.35f;
        if (scrolling)
            summary_target_ = std::clamp(summary_target_ + held * 160.0f * dt, 0.0f, summary_max_);
        if ((input.nav == Direction::up || input.nav == Direction::down) && !(scrolling || (input.nav_repeat && summary_max_ > 0.0f)))
        {
            const int next = std::clamp(action_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != action_)
            {
                action_ = next;
                feedback.play(audio::Cue::focus, 1.0f, 0.35f);
            }
        }
        action_ = std::clamp(action_, 0, count - 1);
        if (actions[static_cast<std::size_t>(action_)].kind != 3)
            remove_hold_ = -1;
        if (actions[static_cast<std::size_t>(action_)].kind == 1)
        {
            if (hold_to_cancel(input, dt, version_download(), feedback))
                sheet_open_ = false;
        }
        else if (actions[static_cast<std::size_t>(action_)].kind == 3)
        {
            if (hold_to_remove(input, dt, feedback, {sheet_game_.id}))
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
            else if (a.kind == 4 && g)
            {
                feedback.play(audio::Cue::select);
                sheet_open_ = false;
                const int id = version_on_console();
                view_.open(app_, rommps::SyncView::kGame, id ? id : g->id, g->name);
            }
            else if (a.kind == 5)
            {
                feedback.play(audio::Cue::modal_open);
                addons_open_ = true;
                addons_top_ = 0;
            }
            else if (a.kind == 6)
            {
                feedback.play(audio::Cue::modal_open);
                versions_open_ = true;
                versions_list_.set_focus(0);
                check_versions();
                remove_hold_ = -1;
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

    // ---------------------------------------------------------------- a game's versions

    // Rows of a game's versions that fit the picker at once.
    static constexpr int kVersionRows = 4;

    // A picker row and what Cross does on it: hold to cancel job while it
    // downloads; hold to remove ids (every one of them together, a set of
    // discs) once here is set; else download fetch, alone or as one disc of a
    // folder.
    struct VersionRow
    {
        rommps::Row row;
        std::vector<int> ids, fetch;
        int disc = 0, job = 0;
        bool here = false;
    };

    // The disc a version's name says ("Parasite Eve (USA) (Disc 2)" is 2),
    // and the name without it, which the set's other discs share; 0 if none.
    static int disc_of(const std::string &name, std::string &set)
    {
        const std::size_t at = name.find("(Disc ");
        const int disc = at == std::string::npos ? 0 : std::atoi(name.c_str() + at + 6);
        if (disc <= 0)
            return 0;
        const std::size_t end = name.find(')', at);
        set = name.substr(0, at) + (end == std::string::npos ? "" : name.substr(end + 1));
        for (std::size_t twice; (twice = set.find("  ")) != std::string::npos;)
            set.erase(twice, 1);
        while (!set.empty() && set.back() == ' ')
            set.pop_back();
        return disc;
    }

    // Whether a folder of discs has disc on the console. Before the payload
    // says (or one that can't), the folder being here counts for all of them.
    bool disc_here(const rommps::Details::Version &v, int disc) const
    {
        const std::vector<int> *here = app_.discs_here(v.id);
        return here ? std::find(here->begin(), here->end(), disc) != here->end() : app_.on_console(v.id);
    }

    // Every version, by name. A set of discs RomM keeps as games of their own
    // gets a row first, for all of them; a folder of discs lists its discs,
    // each one downloadable, the folder's row fetching the rest.
    std::vector<VersionRow> version_rows() const
    {
        std::vector<VersionRow> out;
        const std::vector<rommps::Details::Version> &versions = app_.details(sheet_game_.id).versions;
        // busy: something of it downloads; d: its own download, with a bar.
        const auto fill = [&](VersionRow &r, bool busy, const rommps::Download *d, bool all) {
            const bool focused = static_cast<int>(out.size()) == versions_list_.focus();
            const bool removable = r.here;
            if (d && d->active())
            {
                busy = true;
                r.job = d->cancellable() ? d->id : 0;
                r.row.progress = d->progress();
            }
            r.row.value = busy ? (focused && r.job ? "Hold to cancel" : d && d->syncing() ? "Syncing saves" : "Downloading")
                          : removable ? (focused ? "Hold to remove" : "Downloaded")
                          : all       ? "Download all"
                                      : "Download";
            r.row.danger = removable && focused;
            if (busy || r.here)
                r.fetch.clear();
        };
        for (std::size_t i = 0; i < versions.size(); ++i)
        {
            const rommps::Details::Version &v = versions[i];
            std::string set, other;
            // The first disc of a set: the set's row, when there's more than one.
            if (disc_of(v.name, set) && (i == 0 || disc_of(versions[i - 1].name, other) == 0 || other != set))
            {
                VersionRow all;
                double size = 0, done = 0, total = 0;
                bool busy = false;
                all.here = true;
                for (std::size_t j = i; j < versions.size() && disc_of(versions[j].name, other) && other == set; ++j)
                {
                    const int id = versions[j].id;
                    all.ids.push_back(id);
                    if (!app_.on_console(id))
                        all.fetch.push_back(id), all.here = false;
                    if (const rommps::Download *d = app_.download_for(id); d && d->active())
                        busy = true, done += d->done, total += d->total;
                    size += versions[j].size;
                }
                if (all.ids.size() > 1)
                {
                    all.row.label = set;
                    all.row.detail = std::to_string(all.ids.size()) + " discs  \xC2\xB7  " + rommps::human_size(size);
                    if (busy) // every disc's download together
                        all.row.progress = total > 0 ? static_cast<float>(done / total) : 0.0f;
                    fill(all, busy, nullptr, true);
                    out.push_back(std::move(all));
                }
            }
            VersionRow r;
            r.ids = r.fetch = {v.id};
            r.here = app_.on_console(v.id);
            for (const rommps::Details::Version::Disc &d : v.discs)
                r.here = r.here && disc_here(v, d.disc);
            r.row.label = v.name;
            r.row.detail = rommps::human_size(v.size) + (v.label.empty() ? "" : "  \xC2\xB7  " + v.label) +
                           (v.discs.empty() ? "" : "  \xC2\xB7  " + std::to_string(v.discs.size()) + " discs");
            fill(r, false, app_.download_for(v.id, 0), !v.discs.empty());
            out.push_back(std::move(r));
            for (const rommps::Details::Version::Disc &d : v.discs)
            {
                VersionRow disc;
                disc.ids = disc.fetch = {v.id};
                disc.disc = d.disc;
                disc.here = disc_here(v, d.disc);
                disc.row.label = "    Disc " + std::to_string(d.disc);
                disc.row.detail = rommps::human_size(d.size);
                fill(disc, app_.disc_downloading(v.id, d.disc), app_.download_for(v.id, d.disc), false);
                out.push_back(std::move(disc));
            }
        }
        return out;
    }

    static std::vector<rommps::Row> rows_of(const std::vector<VersionRow> &picker)
    {
        std::vector<rommps::Row> rows;
        for (const VersionRow &r : picker)
            rows.push_back(r.row);
        return rows;
    }

    // Asks which discs of each folder of discs are on the console, for the
    // picker's rows.
    void check_versions()
    {
        for (const rommps::Details::Version &v : app_.details(sheet_game_.id).versions)
            if (!v.discs.empty())
                app_.check_on_console(v.id);
    }

    // Cross downloads a version, a set of discs or a disc; what's on the
    // console comes off with a hold, like the sheet's own button.
    void update_versions(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        const std::vector<VersionRow> picker = version_rows();
        const std::vector<rommps::Row> rows = rows_of(picker);
        // Touch: outside the list is Circle; a row touched takes the focus first,
        // so a hold on it starts at once.
        input = touch::with(input, touch::take(&versions_open_), Action::back);
        input = versions_list_.touch(input, rows);
        const int before = versions_list_.focus();
        const rommps::RowEvent pick = versions_list_.handle(rows, input, feedback);
        versions_list_.update(dt, static_cast<int>(rows.size()), kVersionRows * rommps::RowList::kRowHeight);
        if (input.is_pressed(Action::back))
        {
            feedback.play(audio::Cue::back);
            versions_open_ = false;
            return;
        }
        const int focus = versions_list_.focus();
        if (focus != before || focus >= static_cast<int>(picker.size()))
        {
            remove_hold_ = -1; // a hold doesn't carry over to another row
            return;
        }
        const VersionRow &row = picker[static_cast<std::size_t>(focus)];
        if (row.job)
        {
            const rommps::Download *job = nullptr;
            for (const rommps::Download &d : app_.queue())
                if (d.id == row.job)
                    job = &d;
            hold_to_cancel(input, dt, job, feedback);
        }
        else if (row.here)
            hold_to_remove(input, dt, feedback, row.ids, row.disc); // the row turns back to Download
        else if (pick.activated && !row.fetch.empty())
        {
            for (const rommps::Details::Version &v : app_.details(sheet_game_.id).versions)
                if (std::find(row.fetch.begin(), row.fetch.end(), v.id) != row.fetch.end())
                    app_.download(v.id,
                                  sheet_game_.name + (v.label.empty() ? "" : " (" + v.label + ")") +
                                      (row.disc ? " (Disc " + std::to_string(row.disc) + ")" : ""),
                                  row.disc);
            // The picker stays: another disc or version can follow, and
            // the row shows this one coming down.
            feedback.play(audio::Cue::launch);
            feedback.rumble(0.7f, 0.18f);
        }
    }

    void draw_versions(gfx::DrawList &list) const
    {
        const rommps::Look &L = look();
        const ui::Fonts &fonts = context_.fonts;
        const std::vector<rommps::Row> rows = rows_of(version_rows());
        const float rows_h = static_cast<float>(std::min<int>(static_cast<int>(rows.size()), kVersionRows)) *
                             rommps::RowList::kRowHeight;
        const Rect box{360, kScreenH / 2 - (rows_h + 220) / 2, 1200, rows_h + 220};
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000, 0.55f));
        modal_zones(list, box, &versions_open_);
        Color solid = L.page; // nothing shows through the panel
        solid.a = 1.0f;
        list.rounded_rect(box, L.radius, solid);
        draw_panel(list, box);
        ui::text(list, fonts.display, fonts.display.font->fit(sheet_game_.name, 40, box.w - 96), box.x + 48, box.y + 84,
                 40, L.panel_text);
        const std::vector<rommps::Details::Version> &versions = app_.details(sheet_game_.id).versions;
        const std::string what = versions.size() > 1 ? std::to_string(versions.size()) + " versions"
                                 : versions.empty()  ? std::string()
                                                     : std::to_string(versions[0].discs.size()) + " discs";
        ui::text(list, fonts.regular, what, box.x + 48, box.y + 128, 24, L.muted);
        versions_list_.draw(list, fonts, L, rows, {box.x + 16, box.y + 160, box.w - 32, rows_h}, clock_);
        // Cross held to remove what's on the console, or to cancel a
        // download: the bar fills, then it's done.
        const std::vector<VersionRow> picker = version_rows();
        const int focus = versions_list_.focus();
        const float hold = remove_hold_ > 0 ? std::min(1.0f, remove_hold_ / kCancelHold)
                           : focus < static_cast<int>(picker.size())
                               ? cancel_progress(picker[static_cast<std::size_t>(focus)].job)
                               : 0.0f;
        if (hold > 0)
        {
            const Rect bar{box.x + 48, box.y + box.h - 40, box.w - 96, 8};
            list.rounded_rect(bar, 4, L.outline);
            list.rounded_rect({bar.x, bar.y, bar.w * hold, bar.h}, 4, L.danger);
        }
    }

    // ---------------------------------------------------------------- RetroArch 2.0 folders

    std::vector<rommps::Row> move_rows() const
    {
        const int n = app_.status().retroarch_move;
        return {{rommps::RowKind::action, n == 1 ? "Move it" : "Move them"}, {rommps::RowKind::action, "Not now"}};
    }

    void update_move(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        const std::vector<rommps::Row> rows = move_rows();
        input = touch::with(input, touch::take(&move_open_), Action::back);
        input = move_list_.touch(input, rows);
        const rommps::RowEvent pick = move_list_.handle(rows, input, feedback);
        move_list_.update(dt, static_cast<int>(rows.size()), 2 * rommps::RowList::kRowHeight);
        if (input.is_pressed(Action::back) || pick.row == 1)
        {
            feedback.play(audio::Cue::back);
            move_open_ = false;
        }
        else if (pick.row == 0)
        {
            feedback.play(audio::Cue::select);
            move_open_ = false;
            app_.post("/api/retroarch/move", "{}", [this](const rommps::Response &r) {
                cJSON *j = r.ok() ? r.json() : nullptr;
                const cJSON *moved = cJSON_GetObjectItemCaseSensitive(j, "moved");
                const int n = cJSON_IsNumber(moved) ? moved->valueint : 0;
                if (!r.ok())
                    app_.say("Couldn't move the games: " + r.message());
                else
                    app_.say("Moved " + std::to_string(n) + (n == 1 ? " game" : " games") +
                             (cJSON_GetObjectItemCaseSensitive(j, "error") ? ", some couldn't be" : ""));
                cJSON_Delete(j);
                app_.refresh_now();
            });
        }
    }

    void draw_move(gfx::DrawList &list) const
    {
        const rommps::Look &L = look();
        const rommps::Status &s = app_.status();
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000, 0.55f));
        const Rect box{460, (kScreenH - 520) / 2, 1000, 520};
        modal_zones(list, box, &move_open_);
        Color solid = L.page; // nothing shows through the panel
        solid.a = 1.0f;
        list.rounded_rect(box, L.radius, solid);
        draw_panel(list, box);
        ui::text(list, context_.fonts.display, "RetroArch 2.0 has new game folders", box.x + 48, box.y + 84, 40, L.panel_text);
        const std::string text = "It no longer sees " + std::to_string(s.retroarch_move) +
                                 (s.retroarch_move == 1 ? " game" : " games") + " in its old folders (" + s.retroarch_folders +
                                 "). Move " + (s.retroarch_move == 1 ? "it" : "them") + " to the new ones?";
        ui::paragraph(list, context_.fonts.regular, text, box.x + 48, box.y + 146, 26, box.w - 96, 36, L.muted, 3);
        move_list_.draw(list, context_.fonts, L, move_rows(), {box.x + 16, box.y + 300, box.w - 32, 2 * rommps::RowList::kRowHeight},
                        clock_);
    }

    // ---------------------------------------------------------------- collections need pairing again

    static std::vector<rommps::Row> pair_rows()
    {
        return {{rommps::RowKind::action, "Open Settings"}, {rommps::RowKind::action, "Not now"}};
    }

    void update_pair(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        const std::vector<rommps::Row> rows = pair_rows();
        input = touch::with(input, touch::take(&pair_open_), Action::back);
        input = pair_list_.touch(input, rows);
        const rommps::RowEvent pick = pair_list_.handle(rows, input, feedback);
        pair_list_.update(dt, static_cast<int>(rows.size()), 2 * rommps::RowList::kRowHeight);
        if (input.is_pressed(Action::back) || pick.row == 1)
        {
            feedback.play(audio::Cue::back);
            pair_open_ = false;
        }
        else if (pick.row == 0)
        {
            feedback.play(audio::Cue::select);
            pair_open_ = false;
            tab_ = kSettings;
            tab_age_ = 0.0f;
            settings_.enter(app_);
        }
    }

    void draw_pair(gfx::DrawList &list) const
    {
        const rommps::Look &L = look();
        list.rounded_rect({0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, 0, Color::rgb(0x000000, 0.55f));
        const Rect box{460, (kScreenH - 520) / 2, 1000, 520};
        modal_zones(list, box, &pair_open_);
        Color solid = L.page; // nothing shows through the panel
        solid.a = 1.0f;
        list.rounded_rect(box, L.radius, solid);
        draw_panel(list, box);
        ui::text(list, context_.fonts.display, "Collections are here", box.x + 48, box.y + 84, 40, L.panel_text);
        ui::paragraph(list, context_.fonts.regular,
                      "RomM's collections now show in the Library. To see them, pair with RomM again: in Settings, "
                      "Unpair from RomM, then pair. Your games and saves stay.",
                      box.x + 48, box.y + 146, 26, box.w - 96, 36, L.muted, 4);
        pair_list_.draw(list, context_.fonts, L, pair_rows(), {box.x + 16, box.y + 300, box.w - 32, 2 * rommps::RowList::kRowHeight},
                        clock_);
    }

    // The transfer tab: what's queued and running.
    void update_queue(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        const int count = static_cast<int>(app_.queue().size());
        // The list can get shorter at any tick (finished ones cleared, the payload restarted).
        queue_row_ = std::clamp(queue_row_, 0, std::max(0, count - 1));
        input = touch_list(input, &queue_row_, queue_scroll_, queue_free_, count, feedback);
        if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
        {
            const int next = std::clamp(queue_row_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != queue_row_)
            {
                queue_row_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
        }
        if (input.is_pressed(Action::west) && count > 0)
        {
            const auto &downloads = app_.queue();
            if (std::any_of(downloads.begin(), downloads.end(), [](const rommps::Download &d) { return d.finished(); }))
            {
                app_.clear_finished_queue();
                queue_row_ = 0;
                feedback.play(audio::Cue::back);
            }
            else
                refuse(feedback, 0.0f);
        }
        if (count > 0 && queue_row_ < static_cast<int>(app_.queue().size()))
        {
            const rommps::Download &d = app_.queue()[static_cast<std::size_t>(queue_row_)];
            if (d.cancellable())
                hold_to_cancel(input, dt, &d, feedback);
            else if (d.retryable() && input.is_pressed(Action::confirm))
            {
                app_.retry_download(d.id);
                feedback.play(audio::Cue::select);
            }
            else if (input.is_pressed(Action::confirm))
                refuse(feedback, 0.0f);
        }
    }

    // The Downloads tab: the games on this console, one per row.
    void update_downloads(const InputFrame &pad, float dt, app::Feedback &feedback)
    {
        InputFrame input = pad;
        const int count = static_cast<int>(app_.installed().size());
        // The list can get shorter at any tick (a game removed on its sheet).
        installed_row_ = std::clamp(installed_row_, 0, std::max(0, count - 1));
        input = touch_list(input, &installed_row_, installed_scroll_, installed_free_, count, feedback);
        if ((input.nav == Direction::up || input.nav == Direction::down) && count > 0)
        {
            const int next = std::clamp(installed_row_ + (input.nav == Direction::down ? 1 : -1), 0, count - 1);
            if (next != installed_row_)
            {
                installed_row_ = next;
                feedback.play(audio::Cue::focus);
            }
            else if (!input.nav_repeat)
                refuse(feedback, 0.0f);
        }
        if (input.is_pressed(Action::confirm) && count > 0)
        {
            feedback.play(audio::Cue::open);
            open_sheet(*downloaded_game());
        }
    }

    // ---------------------------------------------------------------- touch

    // Downloads' and Queue's lists (six rows in view, kListRowH apart): a row
    // tapped takes the focus and Cross; a drag scrolls the list, and Up or
    // Down then picks up from the bottom row in view, where the focus sits
    // when the pad scrolls them.
    static constexpr float kListRowH = 110.0f;

    InputFrame touch_list(InputFrame input, int *row, tween::Spring &scroll, bool &free, int count, app::Feedback &feedback)
    {
        const touch::Touch t = touch::take(row);
        if (touch::scroll(scroll, t, kListRowH, static_cast<float>(count - 6)))
            free = true;
        if (t.index >= 0 && t.index < count)
        {
            *row = t.index;
            free = false;
            return touch::with(input, t);
        }
        if (free && (input.nav == Direction::up || input.nav == Direction::down) && count > 0)
        {
            free = false;
            *row = std::clamp(static_cast<int>(std::lround(scroll.value)) + 5, 0, count - 1);
            feedback.play(audio::Cue::focus);
            input.nav = Direction::none;
        }
        return input;
    }

    // A box over the dimmed screen: outside it is Circle (owner takes that),
    // the box itself keeps touches from what's under it.
    static void modal_zones(gfx::DrawList &list, const Rect &box, const void *owner)
    {
        touch::zone(list, {0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, owner, 0);
        touch::zone(list, box, nullptr, -1, touch::kButton);
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
        const float title_w = ui::text(list, fonts.display, ROMM_APP, x, 96 - 10 * (1.0f - in), 34, L.text);
        x += std::max(220.0f, title_w + 48); // RommPS's 220; a longer name pushes the tabs along
        if (ready)
        {
            for (int i = 0; i < kTabs; ++i)
            {
                const bool active = i == tab_;
                const float w = ui::text(list, active ? fonts.semibold : fonts.regular, kTabNames[i], x,
                                         92 - 10 * (1.0f - in), 26, active ? L.text : L.muted);
                touch::zone(list, {x - 22, 44, w + 44, 76}, &tab_, i, touch::kButton);
                if (active)
                    list.rounded_rect({x, 104, w, 4}, 2, accent());
                x += w + 44;
            }
        }
        // Right: the server, or what's wrong.
        const rommps::Status &s = app_.status();
        const bool old_payload = s.reachable && app_.payload_too_old();
        const bool connected = s.known && s.reachable && !old_payload && s.paired;
        float left = 1824; // where the line starts, for the dot
        if (connected)
        {
            // "<user> @ host:port", the user in bold, and a lock when it's HTTPS.
            const std::size_t scheme = s.server.find("://");
            std::string host = scheme == std::string::npos ? s.server : s.server.substr(scheme + 3);
            host = host.substr(0, host.find('/'));
            if (s.server.rfind("https://", 0) == 0)
            {
                const Color c = L.muted;
                list.bordered_rect({left - 11, 74, 8, 12}, 4, Color::rgb(0x000000, 0.0f), 2.0f, c); // shackle
                list.rounded_rect({left - 14, 80, 14, 11}, 2, c);                                   // body
                left -= 14 + 10;
            }
            left -= ui::text(list, fonts.regular, fonts.regular.font->fit(rommps::plain_text((" @ " + host).c_str()), 22, 460), left,
                             90, 22, L.muted, gfx::Align::right);
            left -= ui::text(list, fonts.semibold, fonts.semibold.font->fit(s.user, 22, 240), left, 90, 22, L.text,
                             gfx::Align::right);
        }
        else
        {
            const std::string where = !s.known       ? "Connecting"
                                      : !s.reachable ? "The background service isn't running"
                                      : old_payload  ? "Updating the background service " + s.version
                                                     : "Not paired";
            left -= ui::text(list, fonts.regular, context_.fonts.regular.font->fit(where, 22, 640), left, 90, 22, L.muted,
                             gfx::Align::right);
        }
        const float dot_x = left - 12;
        // RomM not answering: the dot turns amber, and a line under it pulses.
        const bool trouble = s.reachable && s.paired && !old_payload && !s.server_trouble.empty();
        const float pulse = 0.55f + 0.45f * std::sin(age_ * 3.0f);
        list.circle(dot_x - 12, 83, 7,
                    trouble ? L.warning.with_alpha(pulse)
                    : s.reachable && s.paired && !old_payload ? L.success
                    : s.reachable ? L.warning
                                  : L.danger);
        if (trouble)
            ui::text(list, fonts.semibold, "Difficulties connecting to server", 1824, 122, 20, L.warning.with_alpha(pulse),
                     gfx::Align::right);
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
        touch::zone(list, panel, this, 0, touch::kButton); // touched: try starting it again
        const float x = panel.x + 72;
        const bool starting = app_.payload_starting() || (s.known && !s.reachable && app_.payload_error().empty());
        ui::text(list, fonts.display,
                 !s.known ? "Connecting to the background service" : starting ? "Starting the background service" : "The background service isn't running", x,
                 panel.y + 120, 56, L.panel_text);
        ui::paragraph(list, fonts.regular,
                      starting ? ROMM_APP " syncs through a background service, and starts it for you. "
                                 "This takes a few seconds."
                               : ROMM_SERVICE_DOWN,
                      x, panel.y + 190, 28, 1060, 42, L.panel_muted, 4);
        if (!starting && !app_.payload_error().empty())
            ui::text(list, fonts.regular, fonts.regular.font->fit(rommps::plain_text(app_.payload_error().c_str()), 22, 1060), x,
                     panel.y + 340, 22, L.panel_muted.with_alpha(0.8f));
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
        if (!app_.config().save_sync)
        {
            ui::text(list, fonts.display, "Save sync is off", x - 4, 304, 80, L.text);
            ui::text(list, fonts.regular, "RomM is only for games. Turn it on in Settings.", x, 360, 28, L.muted);
            list.pop_opacity();
            return;
        }
        // The headline only says synced when the last sync went through.
        std::string headline = s.syncing                         ? "Syncing"
                               : !s.sync_count                   ? "Not synced yet"
                               : s.last_result != "OK"           ? "The last sync failed"
                               : s.conflicts == 1                ? "1 save needs you"
                               : s.conflicts > 1                 ? std::to_string(s.conflicts) + " saves need you"
                                                                 : "Your saves are synced";
        ui::text(list, fonts.display, headline, x - 4, 304, 80, L.text);
        std::string line = s.syncing         ? (s.phase.empty() ? "Working" : s.phase)
                           : s.conflicts > 0 ? std::string("Open Conflicts to choose which copy to keep")
                           : s.sync_count    ? "Last sync " + ago(std::difftime(std::time(nullptr), s.last_run)) +
                                                "  \xC2\xB7  " + s.last_result
                                             : "No sync yet";
        ui::text(list, fonts.regular, context_.fonts.regular.font->fit(line, 28, 1500), x, 360, 28, L.muted);

        // Sync now, and the lists beside it; the focused one is lit.
        std::string labels[kHomeButtons] = {"Sync now", "Conflicts", "Activity", "Tracked saves"};
        if (s.conflicts > 0)
            labels[1] += " (" + std::to_string(s.conflicts) + ")";
        float bx = x;
        for (int i = 0; i < kHomeButtons; ++i)
        {
            const bool focused = i == home_focus_;
            const float w = fonts.semibold.font->measure(labels[i], 28) + (focused ? 104.0f : 64.0f);
            const Rect button{bx, 410, w, 68};
            touch::zone(list, button, &home_focus_, i, touch::kButton);
            if (focused)
            {
                list.glow(button, 34, 18, accent().with_alpha(0.35f));
                list.rounded_rect(button, 34, i == 0 && s.syncing ? L.button.with_alpha(0.4f) : L.button);
                ui::draw_button(list, fonts, L.covers ? ui::GlyphStyle::light() : ui::GlyphStyle::dark(), ui::Button::cross,
                                button.x + 24, button.cy(), 34);
                ui::text(list, fonts.semibold, labels[i], button.x + 76, button.cy() + 10, 28, L.on_button);
            }
            else
            {
                draw_panel(list, button, 34);
                ui::text(list, fonts.semibold, labels[i], button.x + 32, button.cy() + 10, 28,
                         i == 1 && s.conflicts ? L.warning : L.panel_text);
            }
            bx += w + 20;
        }

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
        tiles[3] = {ROMM_APP_UPPER, "v" + s.version};
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
        list.pop_opacity();
    }

    void draw_library(gfx::DrawList &list) const
    {
        library_.draw(list, context_.fonts, look(), app_, accent(), clock_, tab_age_);
    }

    void draw_queue(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const auto &downloads = app_.queue();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.display, "Queue", kMargin, 240, 64, L.text);
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
            ui::text(list, fonts.regular, "Nothing downloading.", kMargin, 310, 28,
                     L.muted);
            list.pop_opacity();
            return;
        }
        constexpr float kRowH = 110.0f;
        const float top = 300.0f;
        const float scroll = queue_scroll_.value;
        // One row past the band on each side, hidden by the clip below.
        const int first = std::max(0, static_cast<int>(std::floor(scroll)) - 1);
        list.rounded_rect({kMargin - 24, top + (queue_position_.value - scroll) * kRowH, 1776, kRowH - 14},
                          26, L.text.with_alpha(0.1f));
        list.push_clip({0, top, kScreenW, 6 * kRowH});
        touch::zone(list, {0, top, kScreenW, 6 * kRowH}, &queue_row_, -1);
        for (int i = first; i < static_cast<int>(downloads.size()) && i < first + 9; ++i)
        {
            const rommps::Download &d = downloads[static_cast<std::size_t>(i)];
            const float y = top + (static_cast<float>(i) - scroll) * kRowH;
            touch::zone(list, {kMargin - 24, y, 1776, kRowH - 14}, &queue_row_, i);
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(d.name, 28, 1100), kMargin, y + 42, 28, L.text);
            // "PlayStation  ·  Waiting". A download that failed before RomM said
            // which platform the game is on has none: no dot in front then.
            std::string meta = d.platform.empty() ? "" : d.platform + "  \xC2\xB7  ";
            if (d.running())
            {
                char text[96];
                std::snprintf(text, sizeof text, "%s of %s", rommps::human_size(d.done).c_str(),
                              rommps::human_size(d.total).c_str());
                meta += text;
            }
            else if (d.status == "error" && !d.error.empty())
            {
                // The service's errors are sentences in lower case; the row's
                // other states start with a capital.
                std::string error = d.error;
                error[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(error[0])));
                meta += error;
            }
            else if (d.syncing())
                meta += "Syncing saves";
            else if (d.status == "done" && !d.saves_note().empty())
                meta += "Downloaded  \xC2\xB7  " + d.saves_note();
            else
            {
                static const char *const kStates[][2] = {{"queued", "Waiting"},
                                                         {"cancelling", "Cancelling"},
                                                         {"cancelled", "Cancelled"},
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
        list.pop_clip();
        list.pop_opacity();
    }

    // The Downloads tab: every game this console keeps, as a list.
    void draw_downloads(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const auto &games = app_.installed();
        const float in = tween::stagger(tab_age_, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.display, "Downloads", kMargin, 240, 64, L.text);
        // Top right: how many games this console keeps.
        if (!games.empty())
        {
            char count[48];
            std::snprintf(count, sizeof count, games.size() == 1 ? "1 game" : "%d games",
                          static_cast<int>(games.size()));
            ui::text(list, fonts.regular, count, 1824, 240, 24, L.muted, gfx::Align::right);
        }
        if (games.empty())
        {
            ui::text(list, fonts.regular, "Nothing on this " ROMM_HERE " yet.", kMargin, 310, 28, L.muted);
            ui::text(list, fonts.regular, "Games you download from your library land here.", kMargin, 348, 22,
                     L.muted.with_alpha(0.7f));
            list.pop_opacity();
            return;
        }
        constexpr float kRowH = 110.0f;
        const float top = 300.0f;
        const float scroll = installed_scroll_.value;
        // One row past the band on each side, hidden by the clip below.
        const int first = std::max(0, static_cast<int>(std::floor(scroll)) - 1);
        list.rounded_rect(
            {kMargin - 24, top + (installed_position_.value - scroll) * kRowH, 1776, kRowH - 14}, 26,
            L.text.with_alpha(0.1f));
        list.push_clip({0, top, kScreenW, 6 * kRowH});
        touch::zone(list, {0, top, kScreenW, 6 * kRowH}, &installed_row_, -1);
        for (int i = first; i < static_cast<int>(games.size()) && i < first + 9; ++i)
        {
            const rommps::Game &g = games[static_cast<std::size_t>(i)];
            const float y = top + (static_cast<float>(i) - scroll) * kRowH;
            touch::zone(list, {kMargin - 24, y, 1776, kRowH - 14}, &installed_row_, i);
            const Rect thumb{kMargin, y + 8, 66, 88};
            rommps::draw_cover(list, fonts, L, app_, &g, thumb, 1.0f, 10, false);
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(g.name, 28, 1160), kMargin + 96, y + 42,
                     28, L.text);
            const rommps::Platform *p = app_.platform_by_id(g.platform_id);
            std::string meta = (p ? p->name + "  \xC2\xB7  " : std::string()) + rommps::human_size(g.size);
            Color meta_colour = L.muted;
            if (const rommps::Download *d = app_.download_for(g.id); d && d->active())
            {
                char pct[16];
                std::snprintf(pct, sizeof pct, "%d%%", static_cast<int>(d->progress() * 100.0f));
                meta = (p ? p->name + "  \xC2\xB7  " : std::string()) + "Downloading " + pct;
                meta_colour = L.text;
            }
            ui::text(list, fonts.regular, context_.fonts.regular.font->fit(meta, 22, 1200), kMargin + 96, y + 80, 22,
                     meta_colour);
        }
        list.pop_clip();
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
        constexpr float kHeight = 680.0f;
        const Rect sheet{120, gfx::kVirtualHeight - kHeight * t - 40.0f * t + 60.0f * (1.0f - t), 1680, kHeight};
        // Touch: outside the sheet closes it; a drag on it scrolls the description.
        touch::zone(list, {0, 0, gfx::kVirtualWidth, gfx::kVirtualHeight}, &sheet_open_, 0);
        touch::zone(list, sheet, &summary_scroll_, -1);
        list.push_opacity(tween::clamp01(t * 1.4f));
        list.shadow({sheet.x, sheet.y + 20, sheet.w, sheet.h}, 44, 60, Color::rgb(0x000000, 0.5f));
        list.glass(glass, sheet, 44, kWhite);
        list.rounded_rect(sheet, 44, L.covers ? gfx::mix(palette.dark, kInk, 0.5f).with_alpha(0.62f) : L.panel);
        list.bordered_rect(sheet, 44, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);

        // The cover in its own shape, as large as fits the space for it.
        const Rect space{sheet.x + 56, sheet.y + 64, 330, 480};
        rommps::draw_cover(list, fonts, L, app_, g, rommps::fit_cover(rommps::cover_shape(app_, g), space), 1.0f, 26);
        const float x = space.x + space.w + 56;
        ui::text(list, fonts.semibold, ui::upper(p ? p->name : ""), x, sheet.y + 100, 20, acc, gfx::Align::left, 4.0f);
        float y = ui::paragraph(list, fonts.display, g->name, x - 2, sheet.y + 170, 54, 640, 62, L.panel_text, 2) - 10;
        const rommps::Details &info = app_.details(g->id);
        // What there is to pick from, once the details say (RomM's count
        // includes versions whose files are gone), else RomM's count.
        const int versions = info.loaded ? static_cast<int>(info.versions.size()) : g->versions;
        const int discs = info.versions.size() == 1 ? static_cast<int>(info.versions[0].discs.size()) : 0;
        if (versions > 1 || discs > 1)
        {
            const std::string v = versions > 1 ? std::to_string(versions) + " versions" : std::to_string(discs) + " discs";
            ui::text(list, fonts.semibold, v, x, y, 22, L.panel_text.with_alpha(0.7f));
            y += 42;
        }
        // A Switch game's title id, as a tag: what to look for in ProsperoEden.
        if (!info.title_id.empty())
        {
            const std::string tag = "TITLE ID  " + info.title_id;
            const float w = fonts.semibold.font->measure(tag, 18) + 28;
            list.bordered_rect({x, y - 24, w, 34}, 17, acc.with_alpha(0.12f), 1.5f, acc.with_alpha(0.5f));
            ui::text(list, fonts.semibold, tag, x + 14, y - 1, 18, L.panel_text.with_alpha(0.85f));
            y += 50;
        }
        if (addons_open_)
            draw_addons(list, info, x, y, sheet.y + sheet.h - 60.0f, acc);
        else
            draw_about(list, info, g, p, x, y, sheet, acc);
        draw_sheet_actions(list, g, sheet);
        list.pop_opacity();
    }

    // A Switch game's update and DLC, a row each with its title id; then a
    // collection's other games and theirs.
    void draw_addons(gfx::DrawList &list, const rommps::Details &info, float x, float y, float bottom, Color acc) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        ui::text(list, fonts.semibold, info.bundle ? "IN THIS FOLDER" : "UPDATES AND DLC", x, y, 20, acc,
                 gfx::Align::left, 4.0f);
        y += 46;
        const int n = static_cast<int>(info.addons.size());
        for (int i = addons_top_; i < n && i < addons_top_ + kAddonRows && y < bottom; ++i, y += 40)
        {
            const rommps::Details::Addon &a = info.addons[static_cast<std::size_t>(i)];
            const bool own = i < n - info.bundle;
            static const char *const kKind[] = {"Update", "DLC", "Game"};
            // From the right: the size, the title id, then the name in what's left.
            const std::string size = rommps::human_size(a.size);
            const float id_right = x + 660 - fonts.regular.font->measure(size, 20) - 24;
            const float id_left = a.title_id.empty() ? id_right : id_right - fonts.regular.font->measure(a.title_id, 18);
            ui::text(list, fonts.semibold, kKind[a.kind], x, y, 20, own ? acc : L.panel_muted);
            ui::text(list, fonts.regular, fonts.regular.font->fit(a.name, 20, std::max(80.0f, id_left - 20 - (x + 100))),
                     x + 100, y, 20, L.panel_text);
            if (!a.title_id.empty())
                ui::text(list, fonts.regular, a.title_id, id_right, y, 18, L.panel_muted, gfx::Align::right);
            ui::text(list, fonts.regular, size, x + 660, y, 20, L.panel_muted, gfx::Align::right);
        }
        if (n > kAddonRows)
            ui::text(list, fonts.regular,
                     std::to_string(addons_top_ + 1) + "-" + std::to_string(std::min(n, addons_top_ + kAddonRows)) +
                         " of " + std::to_string(n),
                     x, y + 6, 20, L.panel_muted);
    }

    // RomM's details: year, publisher, genres; the summary; then the numbers.
    void draw_about(gfx::DrawList &list, const rommps::Details &info, const rommps::Game *g, const rommps::Platform *p,
                    float x, float y, const Rect &sheet, Color acc) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const std::string about = rommps::details_line(info);
        // RomM's rating closes the line, after a star.
        char rating[24] = "";
        if (info.rating > 0)
            std::snprintf(rating, sizeof rating, info.rating > 10 ? "Rating %.0f/100" : "Rating %.1f/10", info.rating);
        const float rating_w = rating[0] ? fonts.semibold.font->measure(rating, 22) + 30 : 0.0f;
        if (!about.empty() || rating[0])
        {
            float w = about.empty() ? -24.0f
                                    : ui::text(list, fonts.semibold, fonts.semibold.font->fit(about, 22, 660 - rating_w - 24),
                                               x, y, 22, acc);
            if (rating[0])
            {
                list.star(x + w + 24 + 9, y - 8, 10, acc);
                ui::text(list, fonts.semibold, rating, x + w + 24 + 26, y, 22, L.panel_text.with_alpha(0.85f));
            }
            y += 52;
        }
        if (!info.summary.empty())
        {
            // Five lines show; holding Up or Down scrolls the rest through.
            constexpr float kRows = 5.0f, kLine = 30.0f, kShown = kRows * kLine;
            const float lines = static_cast<float>(fonts.regular.font->wrap(info.summary, 20, 660).size());
            summary_max_ = std::max(0.0f, lines * kLine - kShown);
            list.push_clip({x - 4, y - 22, 680, kShown});
            ui::paragraph(list, fonts.regular, info.summary, x, y - summary_scroll_.value, 20, 660, kLine,
                          L.panel_text.with_alpha(0.85f));
            list.pop_clip();
            // More below: a small marker under the window's right edge.
            if (summary_max_ > 0.0f && summary_scroll_.value < summary_max_ - 1.0f)
                list.triangle({x + 660 - 14, y - 22 + kShown + 2, 14, 8}, L.panel_text.with_alpha(0.5f), 0.0f, 3.14159f);
            y += std::min(lines, kRows) * kLine + 22;
        }
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
        std::string line;
        for (const std::string &f : facts)
            line += (line.empty() ? "" : "  \xC2\xB7  ") + f;
        // The file and emulator, last; for an archive, why it can't come down.
        std::string file = rommps::human_size(g->size) + (g->multi ? ", several files" : "");
        if (info.updates || info.dlc)
            file += " + " + std::string(info.updates ? "update" : "") + (info.updates && info.dlc ? " and " : "") +
                    (info.dlc ? std::to_string(info.dlc) + " DLC" : "");
        if (p && !p->profile.empty())
            file += "  \xC2\xB7  plays with " + p->profile;
        const bool archived = g->archived || !info.archived.empty();
        if (archived)
            file = info.archived.empty() ? "This download is archived. Decompress it on your RomM server first."
                                         : info.archived;
        if (!line.empty())
            ui::paragraph(list, fonts.regular, line, x, y, 20, 660, 32, L.panel_muted, 2);
        // The file line keeps to the sheet's foot.
        ui::text(list, fonts.regular, context_.fonts.regular.font->fit(file, 20, 660), x, sheet.y + sheet.h - 60, 20,
                 archived ? L.danger : L.panel_muted);
    }

    void draw_sheet_actions(gfx::DrawList &list, const rommps::Game *g, const Rect &sheet) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const rommps::Look &L = look();
        const std::vector<SheetAction> actions = sheet_actions();
        const float ax = sheet.x + sheet.w - 56 - 460;
        const float ay = sheet.y + 76;
        list.rounded_rect({ax, ay + action_position_.value * 84, 460, 72}, 36, L.highlight);
        for (int i = 0; i < static_cast<int>(actions.size()); ++i)
        {
            const bool focused = i == action_;
            const float y = ay + static_cast<float>(i) * 84;
            touch::zone(list, {ax, y, 460, 72}, &action_, i, touch::kButton);
            if (!focused)
                list.bordered_rect({ax, y, 460, 72}, 36, L.panel_text.with_alpha(0.06f), 1.5f, L.outline);
            const SheetAction &a = actions[static_cast<std::size_t>(i)];
            if (a.kind == 1 && focused)
                if (const rommps::Download *d = version_download())
                    if (const float hold = cancel_progress(d->id); hold > 0)
                        list.rounded_rect({ax, y, std::max(72.0f, 460 * hold), 72}, 36, L.danger.with_alpha(0.85f));
            if (a.kind == 3 && focused && remove_hold_ > 0)
                list.rounded_rect({ax, y, std::max(72.0f, 460 * std::min(1.0f, remove_hold_ / kCancelHold)), 72}, 36, L.danger.with_alpha(0.85f));
            const Color ink = (focused ? L.on_highlight : L.panel_text).with_alpha(a.enabled ? 1.0f : 0.5f);
            // The game downloading: a slim bar along the button's foot.
            if (const float done = a.kind == 1 || a.kind == 6 ? download_progress() : -1.0f; done >= 0.0f)
            {
                const Rect bar{ax + 36, y + 58, 460 - 72, 4};
                list.rounded_rect(bar, 2, ink.with_alpha(0.2f));
                list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * done), bar.h}, 2, focused ? L.on_highlight : L.accent);
            }
            float label_x = ax + 36;
            if (a.kind == 6) // opens a list: a caret before the label
            {
                list.triangle({label_x, y + 31, 16, 10}, ink, 0.0f, 3.14159f);
                label_x += 30;
            }
            ui::text(list, fonts.semibold, context_.fonts.semibold.font->fit(a.label, 26, 400), label_x, y + 46, 26, ink);
        }
    }

    void draw_toast(gfx::DrawList &list) const
    {
        const std::string &message = app_.toast();
        if (message.empty())
            return;
        const ui::Fonts &fonts = context_.fonts;
        const float a = tween::clamp01(app_.toast_age() * 5.0f) * tween::clamp01((4.0f - app_.toast_age()) * 2.0f);
        list.push_opacity(a);
        // Beside the button hints, on their line, when there's room for it;
        // above them when they leave too little.
        const float room = std::min(1180.0f, hints_left_ - 24.0f - kMargin);
        const Rect r = room >= 480.0f ? Rect{kMargin, kScreenH - 102, room, 64} : Rect{kMargin, kScreenH - 188, 1180, 64};
        const std::string fitted = fonts.regular.font->fit(message, 24, r.w - 64);
        const rommps::Look &L = look();
        list.rounded_rect(r, 32, L.covers ? Color::rgb(0x14182e, 0.92f) : L.page.with_alpha(0.95f));
        list.bordered_rect(r, 32, Color::rgb(0x000000, 0.0f), 1.5f, L.outline);
        ui::text(list, fonts.regular, fitted, r.x + 32, r.y + 41, 24, L.text);
        list.pop_opacity();
    }

    // The hint row floats over the page's bottom corner: it gets the panel
    // treatment (draw_panel: shadow, fill, outline), so it reads over the
    // covers it crosses instead of hovering on them.
    void hint_row(gfx::DrawList &list, const ui::Fonts &fonts, const ui::GlyphStyle &style,
                  const ui::Hint *hints, int count) const
    {
        const ui::HintLayout layout; // the kit's bottom row: glyphs 40 tall, cy 1010
        const float width = ui::measure_hints(fonts, hints, count, layout);
        const Rect plate{1824.0f - width - 30.0f, layout.cy - 38.0f, width + 60.0f, 76.0f};
        hints_left_ = std::min(hints_left_, plate.x);
        draw_panel(list, plate);
        ui::draw_hints(list, fonts, style, hints, count, 1824, true);
        touch::hints(list, fonts, hints, count, 1824, true);
    }

    void draw_hints(gfx::DrawList &list) const
    {
        const ui::Fonts &fonts = context_.fonts;
        const ui::GlyphStyle style = look().glyphs();
        if (setting_up_)
        {
            const std::vector<ui::Hint> hints = setup_.hints();
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        const bool ready = app_.status().paired && app_.status().setup_complete;
        if (!ready)
            return;
        if (sheet_open_ && versions_open_)
        {
            // What Cross does on the focused row of a game's versions.
            const std::vector<VersionRow> picker = version_rows();
            const int focus = versions_list_.focus();
            const VersionRow *row = focus < static_cast<int>(picker.size()) ? &picker[static_cast<std::size_t>(focus)] : nullptr;
            const char *cross = !row                    ? nullptr
                                : row->job              ? "Hold to cancel"
                                : row->here             ? "Hold to remove"
                                : !row->fetch.empty()   ? "Download"
                                                        : nullptr;
            std::vector<ui::Hint> hints;
            if (cross)
                hints.push_back({ui::Button::cross, cross});
            hints.push_back({ui::Button::circle, "Back"});
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        if (sheet_open_)
        {
            // Scrolling, with the D-pad or the left stick, when there's something to scroll.
            const bool scroll = addons_open_ || summary_max_ > 0.0f;
            const ui::Hint hints[] = {{ui::Button::dpad, addons_open_ ? "Scroll" : "Hold to scroll", ui::Button::left_stick},
                                      {ui::Button::cross, "Choose"},
                                      {ui::Button::circle, "Back"}};
            hint_row(list, fonts, style, scroll ? hints : hints + 1, scroll ? 3 : 2);
            return;
        }
        if (view_.active())
        {
            const std::vector<ui::Hint> hints = view_.hints();
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
            return;
        }
        list.push_opacity(tween::stagger(age_, 8, 0.08f, 0.5f) * (1.0f - sheet_.value));
        const ui::Hint pages{ui::Button::l1, "Pages", ui::Button::r1};
        const ui::Hint close{ui::Button::options, "Hold to close"}; // rommps.cpp's HoldToLeave
        if (tab_ == kHome)
        {
            std::vector<ui::Hint> hints{close, pages};
            if (app_.config().save_sync)
                hints.push_back({ui::Button::cross, home_focus_ == 0 ? "Sync now" : "Open"});
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
        }
        else if (tab_ == kLibrary)
        {
            const std::vector<ui::Hint> hints = library_.hints();
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
        }
        else if (tab_ == kDownloads)
        {
            const ui::Hint hints[] = {pages, {ui::Button::cross, "Details"}};
            hint_row(list, fonts, style, hints, 2);
        }
        else if (tab_ == kQueue)
        {
            // Cross on a download that's still going stops it and deletes what
            // came so far, on one that failed or was cancelled tries it again;
            // Square takes the finished ones off the list.
            const auto &downloads = app_.queue();
            const bool on_row = queue_row_ < static_cast<int>(downloads.size());
            const bool on_active = on_row && downloads[static_cast<std::size_t>(queue_row_)].cancellable();
            const bool on_failed = on_row && downloads[static_cast<std::size_t>(queue_row_)].retryable();
            const bool any_finished = std::any_of(downloads.begin(), downloads.end(),
                                                  [](const rommps::Download &d) { return d.finished(); });
            std::vector<ui::Hint> hints{pages};
            if (any_finished)
                hints.push_back({ui::Button::square, "Clear finished"});
            if (on_active)
                hints.push_back({ui::Button::cross, "Hold to cancel and delete"});
            if (on_failed)
                hints.push_back({ui::Button::cross, "Retry"});
            hint_row(list, fonts, style, hints.data(), static_cast<int>(hints.size()));
        }
        else
        {
            const ui::Hint hints[] = {close, pages, {ui::Button::cross, settings_.cross_hint(app_)}};
            hint_row(list, fonts, style, hints, 3);
        }
        list.pop_opacity();
    }

    app::Context &context_;
    App &app_;
    int tab_ = kLibrary;
    int home_focus_ = 0;     // Home's button with the focus
    rommps::SyncView view_;  // a list opened from Home or a game's sheet
    float tab_age_ = 0.0f;
    tween::Spring tab_position_;
    float age_ = 0.0f;
    float clock_ = 0.0f;
    int shown_id_ = -1;
    ui::SpringColor palette_[4];
    ui::Pulse nudge_;
    float nudge_direction_ = 0.0f;
    mutable float hints_left_ = kScreenW; // where this frame's button hints start, for the toast
    bool sheet_open_ = false;
    bool addons_open_ = false; // the sheet lists the game's update and DLC
    int addons_top_ = 0;       // the first of them shown
    float addons_drag_ = 0.0f; // a touch drag on them, not yet a row's worth
    bool versions_open_ = false; // the sheet's game's versions, in a list over it
    rommps::RowList versions_list_;
    tween::Spring sheet_;
    int action_ = 0;
    tween::Spring action_position_;
    // The description, a few lines at a time: how far it's scrolled (eased),
    // where to, how long Up or Down has been held, and how far it can go (from
    // the last draw).
    tween::Spring summary_scroll_;
    float summary_target_ = 0.0f;
    float summary_hold_ = 0.0f;
    mutable float summary_max_ = 0.0f;
    int queue_row_ = 0;
    float cancel_hold_ = 0; // seconds Cross has been held to cancel cancel_job_; -1 once done
    float remove_hold_ = -1; // seconds Cross has been held on remove; -1 waits for a press
    int cancel_job_ = 0;
    bool cancel_seen_ = false; // hold_to_cancel ran this frame
    tween::Spring queue_position_;
    tween::Spring queue_scroll_; // the window's first row, eased
    bool queue_free_ = false;    // scrolled by touch, away from the focus
    int installed_row_ = 0;    // the Downloads list's focus
    tween::Spring installed_position_;
    tween::Spring installed_scroll_;
    bool installed_free_ = false; // scrolled by touch, away from the focus
    rommps::SettingsPage settings_;
    rommps::LibraryPage library_;
    rommps::Game sheet_game_; // the game the details sheet is for
    rommps::SetupPage setup_;
    bool setting_up_ = false;
    bool move_asked_ = false; // this launch
    bool move_open_ = false;  // asking whether to move games to RetroArch 2.0's folders
    rommps::RowList move_list_;
    bool collections_checked_ = false; // asked RomM for them, this launch
    bool pair_asked_ = false;          // this launch
    bool pair_open_ = false;           // saying collections need pairing again
    rommps::RowList pair_list_;
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
