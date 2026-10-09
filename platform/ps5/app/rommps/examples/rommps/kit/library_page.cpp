// RommPS: the Library tab. See library_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library_page.hpp"

#include "ui/components/component.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace rommps
{

namespace
{

namespace ui = hui::ui;
namespace tween = hui::tween;
using hui::Action;
using hui::Direction;
using hui::audio::Cue;
using hui::gfx::Align;
using hui::gfx::Rect;

constexpr float kMargin = 96.0f;

// "On this console"
constexpr float kRecentW = 150.0f, kRecentH = 200.0f, kRecentGap = 22.0f;
// Platform tiles
constexpr int kTileCols = 4;
constexpr float kTileW = 408.0f, kTileH = 220.0f, kTileGap = 32.0f, kTilePitch = kTileH + kTileGap;
// A grid of games
constexpr int kCols = 8;
constexpr float kGridTop = 400.0f, kCardW = 176.0f, kCardH = 235.0f, kColPitch = 200.0f, kRowPitch = 260.0f;
constexpr float kBarX = 1726.0f, kBarW = 98.0f, kBarTop = 392.0f, kBarH = 600.0f;
// The A to Z bar's slots: # (digits and the rest) then A to Z.
constexpr int kSlots = 27;
constexpr float kSearchDelay = 0.35f; // seconds after the last key before searching

using Slots = std::array<int, kSlots>; // each slot's first game, -1 for none

int slot_of(const std::string &key)
{
    const char c = key.empty() ? '#' : key[0];
    if (c >= 'a' && c <= 'z')
        return 1 + (c - 'a');
    if (c >= 'A' && c <= 'Z')
        return 1 + (c - 'A');
    return 0;
}

Slots slots_of(const GameList &l)
{
    Slots s;
    s.fill(-1);
    for (const auto &[key, offset] : l.letters)
    {
        int &first = s[static_cast<std::size_t>(slot_of(key))];
        if (first < 0 || offset < first)
            first = offset;
    }
    return s;
}

char slot_char(int slot)
{
    return slot == 0 ? '#' : static_cast<char>('A' + slot - 1);
}

void pop_utf8(std::string &s)
{
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
        s.pop_back();
    if (!s.empty())
        s.pop_back();
}

// The end of a text that fits a width.
std::string tail(const hui::gfx::Font &font, const std::string &text, float size, float width)
{
    std::size_t from = 0;
    while (from < text.size() && font.measure(std::string_view(text).substr(from), size) > width)
        ++from;
    return from ? "\xE2\x80\xA6" + text.substr(from + 1) : text;
}

} // namespace

// ------------------------------------------------------------------ shared

std::string human_size(double bytes)
{
    char text[32];
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1024.0 && unit < 4)
    {
        bytes /= 1024.0;
        ++unit;
    }
    std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", bytes, units[unit]);
    return text;
}

std::string details_line(const Details &d)
{
    std::string out;
    auto add = [&](const std::string &part) {
        if (!part.empty())
            out += (out.empty() ? "" : "  \xC2\xB7  ") + part;
    };
    if (d.year)
        add(std::to_string(d.year));
    add(d.publishers.empty() ? d.developers : d.publishers);
    add(d.genres);
    return out;
}

std::string play_time(double ms)
{
    const int minutes = static_cast<int>(ms / 60000.0 + 0.5);
    char text[32];
    if (minutes < 60)
        std::snprintf(text, sizeof text, "%d min", std::max(1, minutes));
    else if (minutes % 60 == 0)
        std::snprintf(text, sizeof text, "%d h", minutes / 60);
    else
        std::snprintf(text, sizeof text, "%d h %d min", minutes / 60, minutes % 60);
    return text;
}

void draw_cover(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, const Game *g,
                const Rect &rect, float alpha, float radius, bool name)
{
    const std::uint32_t texture = g ? app.cover(g->cover).texture : 0;
    if (texture)
    {
        list.image(texture, rect, hui::gfx::kFullUv, Color::rgb(0xffffff, alpha), radius);
        return;
    }
    // No cover (yet): a tinted card with the name.
    list.rounded_rect(rect, radius, look.covers ? Color::rgb(0x1c2140, 0.75f * alpha) : look.panel.with_alpha(look.panel.a * alpha));
    list.bordered_rect(rect, radius, Color::rgb(0x000000, 0.0f), 1.5f, look.outline.with_alpha(look.outline.a * alpha));
    if (g && name)
        ui::paragraph(list, fonts.semibold, g->name, rect.x + 16, rect.y + 44, rect.w > 160 ? 22.0f : 18.0f, rect.w - 32,
                      rect.w > 160 ? 30.0f : 25.0f, look.panel_text.with_alpha(0.75f * alpha), 4);
}

void draw_badge(hui::gfx::DrawList &list, const Look &look, App &app, const Game *g, const Rect &rect, Color accent)
{
    if (!g)
        return;
    const Download *d = app.download_for(g->id);
    if (d && d->active())
    {
        const Rect bar{rect.x + 12, rect.y + rect.h - 20, rect.w - 24, 8};
        list.rounded_rect(bar, 4, Color::rgb(0x000000, 0.55f));
        list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * d->progress()), bar.h}, 4, accent);
    }
    else if (g->installed || (d && d->status == "done"))
    {
        const float cx = rect.x + rect.w - 24, cy = rect.y + 24;
        list.circle(cx, cy, 14, look.success);
        list.line(cx - 6, cy + 1, cx - 2, cy + 5, 3.2f, Color::rgb(0x0b0d16));
        list.line(cx - 2, cy + 5, cx + 6, cy - 5, 3.2f, Color::rgb(0x0b0d16));
    }
}

void draw_version(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &, const Game *g,
                  const Rect &rect)
{
    if (!g || g->version.empty())
        return;
    const float size = rect.w > 160 ? 17.0f : 15.0f, pad = 10;
    const std::string text = fonts.semibold.font->fit(g->version, size, rect.w - 24 - 2 * pad);
    const float w = fonts.semibold.font->measure(text, size) + 2 * pad;
    const Rect tag{rect.x + 12, rect.y + rect.h - size - 30, w, size + 14};
    list.rounded_rect(tag, tag.h * 0.5f, Color::rgb(0x0b0d16, 0.78f));
    ui::text(list, fonts.semibold, text, tag.x + pad, tag.y + size + 3, size, Color::rgb(0xffffff));
}

// ------------------------------------------------------------------ the page

LibraryPage::LibraryPage()
{
    keys_.style.bindings = hui::ui::KeyboardBindings::standard();
    keys_.style.bindings.layout = Action::count;
    keys_.style.bindings.done = Action::jump_next; // R2 shows the results
    keys_.style.done_label = "Search";
    keys_.style.max_length = 60;
    keys_.on_text = [this](std::string_view text) {
        query_ += text;
        typed_age_ = 0.0f;
    };
    keys_.on_backspace = [this] {
        pop_utf8(query_);
        typed_age_ = 0.0f;
    };
    keys_.set_bounds({360, 610, 1200, 370});
}

void LibraryPage::enter(App &app)
{
    mode_age_ = 0.0f;
    app.refresh_installed();
    if (mode_ == kSearch)
        mode_ = before_search_;
}

void LibraryPage::refuse(hui::ui::Feedback &feedback)
{
    feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
    feedback.rumble(0.25f, 0.05f);
    nudge_.trigger();
}

const Game *LibraryPage::focused(App &app) const
{
    if (mode_ == kPlatforms)
    {
        const auto &recent = app.installed();
        if (section_ == 0 && recent_ >= 0 && recent_ < static_cast<int>(recent.size()))
            return &recent[static_cast<std::size_t>(recent_)];
        return nullptr;
    }
    if (mode_ == kGrid)
        return grid(app).at(index_);
    return nullptr;
}

std::string LibraryPage::backdrop_cover(App &app) const
{
    if (const Game *g = focused(app))
        return g->cover;
    if (mode_ == kPlatforms && section_ == 1)
    {
        const auto &platforms = app.platforms();
        if (tile_ >= 0 && tile_ < static_cast<int>(platforms.size()))
            if (const Game *first = app.games(platforms[static_cast<std::size_t>(tile_)].id, "").at(0))
                return first->cover;
    }
    return "";
}

bool LibraryPage::update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    mode_age_ += dt;
    shown_age_ += dt;
    nudge_.update(dt, 9.0f);
    bool details = false;
    switch (mode_)
    {
    case kPlatforms:
        update_platforms(app, input, feedback, details);
        break;
    case kGrid:
        update_grid(app, input, feedback, details);
        break;
    case kSearch:
        update_search(app, input, dt, feedback);
        break;
    }
    // Scrolling.
    const int tile_rows = std::max(1, static_cast<int>((1000.0f - platforms_top(app) - 24.0f) / kTilePitch));
    const int tile_row = tile_ / kTileCols;
    if (tile_row < tiles_scroll_.target)
        tiles_scroll_.target = static_cast<float>(tile_row);
    if (tile_row > tiles_scroll_.target + static_cast<float>(tile_rows - 1))
        tiles_scroll_.target = static_cast<float>(tile_row - tile_rows + 1);
    tiles_scroll_.update(dt, 14.0f);
    recent_scroll_.reveal(static_cast<float>(recent_) * (kRecentW + kRecentGap),
                          static_cast<float>(recent_) * (kRecentW + kRecentGap) + kRecentW, 1728.0f, 120.0f);
    recent_scroll_.update(dt, 14.0f);
    grid_scroll_.target = static_cast<float>(std::max(0, index_ / kCols - 1));
    grid_scroll_.update(dt, 14.0f);
    const Game *g = focused(app);
    const int id = g ? g->id : -1;
    if (id != shown_id_)
    {
        shown_id_ = id;
        shown_age_ = 0.0f;
    }
    return details;
}

// ------------------------------------------------------------------ platforms

float LibraryPage::platforms_top(App &app) const
{
    return app.installed().empty() ? 196.0f : 486.0f;
}

void LibraryPage::update_platforms(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details)
{
    const auto &recent = app.installed();
    const auto &platforms = app.platforms();
    const int nrecent = static_cast<int>(recent.size()), ntiles = static_cast<int>(platforms.size());
    if (nrecent == 0)
        section_ = 1;
    recent_ = std::clamp(recent_, 0, std::max(0, nrecent - 1));
    tile_ = std::clamp(tile_, 0, std::max(0, ntiles - 1));

    // The covers of the tiles in view.
    const int first_row = static_cast<int>(tiles_scroll_.value);
    for (int i = first_row * kTileCols; i < std::min(ntiles, (first_row + 3) * kTileCols); ++i)
        app.ensure(app.games(platforms[static_cast<std::size_t>(i)].id, ""), 0, 0);

    const bool repeat = input.nav_repeat;
    if (section_ == 0)
    {
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int next = recent_ + (input.nav == Direction::right ? 1 : -1);
            if (next >= 0 && next < nrecent)
            {
                recent_ = next;
                feedback.play(Cue::focus);
            }
            else if (!repeat)
                refuse(feedback);
            break;
        }
        case Direction::down:
            if (ntiles > 0)
            {
                section_ = 1;
                feedback.play(Cue::tab);
            }
            else if (!repeat)
                refuse(feedback);
            break;
        case Direction::up:
            if (!repeat)
                refuse(feedback);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm) && nrecent > 0)
        {
            details = true;
            feedback.play(Cue::open);
        }
    }
    else
    {
        switch (input.nav)
        {
        case Direction::left:
        case Direction::right:
        {
            const int col = tile_ % kTileCols;
            const int next = tile_ + (input.nav == Direction::right ? 1 : -1);
            if ((input.nav == Direction::right ? col < kTileCols - 1 : col > 0) && next >= 0 && next < ntiles)
            {
                tile_ = next;
                feedback.play(Cue::focus);
            }
            else if (!repeat)
                refuse(feedback);
            break;
        }
        case Direction::up:
            if (tile_ - kTileCols >= 0)
            {
                tile_ -= kTileCols;
                feedback.play(Cue::focus);
            }
            else if (nrecent > 0)
            {
                section_ = 0;
                feedback.play(Cue::tab);
            }
            else if (!repeat)
                refuse(feedback);
            break;
        case Direction::down:
            if (tile_ + kTileCols < ntiles)
            {
                tile_ += kTileCols;
                feedback.play(Cue::focus);
            }
            else if (tile_ / kTileCols < (ntiles - 1) / kTileCols)
            {
                tile_ = ntiles - 1;
                feedback.play(Cue::focus);
            }
            else if (!repeat)
                refuse(feedback);
            break;
        case Direction::none:
            break;
        }
        if (input.is_pressed(Action::confirm) && ntiles > 0)
        {
            feedback.play(Cue::open);
            open_grid(app, platforms[static_cast<std::size_t>(tile_)].id, "");
        }
    }
    if (input.is_pressed(Action::north))
    {
        feedback.play(Cue::modal_open);
        open_search(0);
    }
}

void LibraryPage::draw_platforms(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                                 Color accent, float clock, float age) const
{
    const auto &recent = app.installed();
    const auto &platforms = app.platforms();
    const float shake = ui::shake(nudge_.value, clock, 10.0f, 9.0f);

    // On this console
    if (!recent.empty())
    {
        const float in = tween::stagger(age, 1, 0.08f, 0.6f);
        list.push_opacity(in);
        ui::text(list, fonts.semibold, "On this console", kMargin, 190, 24, section_ == 0 ? look.text : look.muted);
        list.push_clip({0, 150, 1920, 330});
        for (int i = 0; i < static_cast<int>(recent.size()); ++i)
        {
            const Game &g = recent[static_cast<std::size_t>(i)];
            Rect r{kMargin + static_cast<float>(i) * (kRecentW + kRecentGap) - recent_scroll_.offset(), 214, kRecentW, kRecentH};
            if (r.x > 1920 || r.x + r.w < 0)
                continue;
            const bool on = section_ == 0 && i == recent_;
            if (on)
            {
                r.x += shake;
                list.glow(r, 18, 22, accent.with_alpha(0.45f));
            }
            draw_cover(list, fonts, look, app, &g, r, on || section_ == 0 ? 1.0f : 0.8f, 16);
            if (on)
                list.bordered_rect(r.inset(-5), 20, Color::rgb(0x000000, 0.0f), 4, look.focus.with_alpha(1.0f));
        }
        list.pop_clip();
        if (section_ == 0)
        {
            const Game &g = recent[static_cast<std::size_t>(recent_)];
            const Platform *p = app.platform_by_id(g.platform_id);
            std::string line = g.name + (p ? "  \xC2\xB7  " + p->name : "");
            ui::text(list, fonts.regular, fonts.regular.font->fit(line, 22, 1700), kMargin, 452, 22, look.muted);
        }
        list.pop_opacity();
    }

    // Platforms
    const float top = platforms_top(app);
    const float in = tween::stagger(age, 2, 0.08f, 0.6f);
    list.push_opacity(in);
    char label[64];
    std::snprintf(label, sizeof label, "Platforms  \xC2\xB7  %d", static_cast<int>(platforms.size()));
    ui::text(list, fonts.semibold, label, kMargin, top, 24, section_ == 1 ? look.text : look.muted);
    if (platforms.empty())
        ui::text(list, fonts.regular, "Loading your platforms", kMargin, top + 60, 26, look.muted);
    const float grid_top = top + 24;
    list.push_clip({0, grid_top - 20, 1920, 1080 - grid_top + 20});
    for (int i = 0; i < static_cast<int>(platforms.size()); ++i)
    {
        const Platform &p = platforms[static_cast<std::size_t>(i)];
        const int row = i / kTileCols, col = i % kTileCols;
        const float y = grid_top + (static_cast<float>(row) - tiles_scroll_.value) * kTilePitch;
        if (y > 1080 || y + kTileH < grid_top - 20)
            continue;
        const bool on = section_ == 1 && i == tile_;
        Rect r{kMargin + static_cast<float>(col) * (kTileW + kTileGap) + (on ? shake : 0.0f), y, kTileW, kTileH};
        if (on)
            list.glow(r, 26, 26, accent.with_alpha(0.4f));
        draw_panel(list, look, r, 26);
        // A few of its covers, fanned on the right.
        GameList &games = app.games(p.id, "");
        for (int k = 2; k >= 0; --k)
        {
            const Game *g = games.at(k);
            if (!g)
                continue;
            const Rect c{r.x + r.w - 128 - static_cast<float>(k) * 40, r.y + 28 + static_cast<float>(k) * 10, 100,
                         133};
            list.shadow({c.x, c.y + 8, c.w, c.h}, 10, 16, Color::rgb(0x000000, 0.35f));
            draw_cover(list, fonts, look, app, g, c, 1.0f - 0.15f * static_cast<float>(k), 10, false);
        }
        ui::paragraph(list, fonts.semibold, p.name, r.x + 28, r.y + 56, 28, 165, 34, look.panel_text, 2);
        char count[48];
        std::snprintf(count, sizeof count, p.rom_count == 1 ? "%d game" : "%d games", p.rom_count);
        ui::text(list, fonts.regular, count, r.x + 28, r.y + 150, 22, look.panel_muted);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(p.profile.empty() ? "No emulator" : p.profile, 18, 200),
                 r.x + 28, r.y + 190, 18, p.profile.empty() ? look.panel_muted.with_alpha(0.6f) : accent, Align::left,
                 1.5f);
        if (on)
            list.bordered_rect(r.inset(-6), 30, Color::rgb(0x000000, 0.0f), 4, look.focus.with_alpha(1.0f));
    }
    list.pop_clip();
    list.pop_opacity();
}

// ------------------------------------------------------------------ a grid

void LibraryPage::open_grid(App &app, int platform_id, const std::string &search)
{
    grid_platform_ = platform_id;
    grid_search_ = search;
    mode_ = kGrid;
    mode_age_ = 0.0f;
    index_ = 0;
    on_letters_ = false;
    grid_scroll_.snap(0.0f);
    app.ensure(grid(app), 0, kCols * 4);
}

int LibraryPage::letter_at(const GameList &l, int index) const
{
    const Slots s = slots_of(l);
    int best = -1, best_offset = -1;
    for (int i = 0; i < kSlots; ++i)
        if (s[static_cast<std::size_t>(i)] >= 0 && s[static_cast<std::size_t>(i)] <= index &&
            s[static_cast<std::size_t>(i)] >= best_offset)
        {
            best = i;
            best_offset = s[static_cast<std::size_t>(i)];
        }
    return best;
}

void LibraryPage::jump_letter(GameList &l, int slot, hui::ui::Feedback &feedback)
{
    const Slots s = slots_of(l);
    if (slot < 0 || slot >= kSlots || s[static_cast<std::size_t>(slot)] < 0)
    {
        refuse(feedback);
        return;
    }
    index_ = std::min(s[static_cast<std::size_t>(slot)], std::max(0, l.total - 1));
    feedback.play(Cue::focus);
}

void LibraryPage::update_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details)
{
    GameList &l = grid(app);
    if (&l == focus_list_ && l.hidden.size() != focus_hidden_)
        index_ = l.shown(focus_raw_);
    if (l.ready())
        index_ = std::min(index_, std::max(0, l.total - 1)); // the list can come out shorter than first thought
    move_in_grid(app, input, feedback, details);
    GameList &now = grid(app);
    focus_list_ = &now;
    focus_raw_ = now.ready() ? now.raw(index_) : index_;
    focus_hidden_ = now.hidden.size();
}

void LibraryPage::move_in_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details)
{
    GameList &l = grid(app);
    // The games around where the grid is going, not the rows it scrolls past
    // on the way (a jump to a letter animates through everything between).
    const int target_row = std::max(0, index_ / kCols - 2);
    app.ensure(l, target_row * kCols, (target_row + 7) * kCols);
    const int total = std::max(0, l.total);
    const Slots slots = slots_of(l);
    const bool repeat = input.nav_repeat;

    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        if (on_letters_)
            on_letters_ = false;
        else
        {
            mode_ = kPlatforms;
            mode_age_ = 0.0f;
        }
        return;
    }
    if (input.is_pressed(Action::north))
    {
        feedback.play(Cue::modal_open);
        open_search(grid_platform_);
        return;
    }
    if (on_letters_)
    {
        if (input.nav == Direction::up || input.nav == Direction::down)
        {
            const int step = input.nav == Direction::down ? 1 : -1;
            int next = letter_ + step;
            while (next >= 0 && next < kSlots && slots[static_cast<std::size_t>(next)] < 0)
                next += step;
            if (next >= 0 && next < kSlots)
            {
                letter_ = next;
                feedback.play(Cue::focus, 1.0f, 0.0f, 0.6f);
            }
            else if (!repeat)
                refuse(feedback);
        }
        else if (input.nav == Direction::left || input.is_pressed(Action::confirm))
        {
            jump_letter(l, letter_, feedback);
            on_letters_ = false;
        }
        else if (input.nav == Direction::right && !repeat)
            refuse(feedback);
        return;
    }
    if (total == 0)
        return;
    switch (input.nav)
    {
    case Direction::left:
        if (index_ % kCols > 0)
        {
            --index_;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::right:
        if (index_ % kCols < kCols - 1 && index_ + 1 < total)
        {
            ++index_;
            feedback.play(Cue::focus);
        }
        else if (!l.letters.empty())
        {
            on_letters_ = true;
            letter_ = std::max(0, letter_at(l, index_));
            feedback.play(Cue::tab);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::up:
        if (index_ - kCols >= 0)
        {
            index_ -= kCols;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::down:
        if (index_ + kCols < total)
        {
            index_ += kCols;
            feedback.play(Cue::focus);
        }
        else if (index_ / kCols < (total - 1) / kCols)
        {
            index_ = total - 1;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::none:
        break;
    }
    if (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next))
    {
        const int here = std::max(0, letter_at(l, index_));
        const int step = input.is_pressed(Action::jump_next) ? 1 : -1;
        int next = here + step;
        // Back: the start of this letter first, when the focus is past it.
        if (step < 0 && here >= 0 && slots[static_cast<std::size_t>(here)] >= 0 &&
            slots[static_cast<std::size_t>(here)] < index_)
            next = here;
        while (next >= 0 && next < kSlots && slots[static_cast<std::size_t>(next)] < 0)
            next += step;
        jump_letter(l, next, feedback);
    }
    if (input.is_pressed(Action::confirm))
    {
        if (l.at(index_))
        {
            details = true;
            feedback.play(Cue::open);
        }
        else
            refuse(feedback);
    }
}

void LibraryPage::draw_grid(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                            Color accent, float clock, float age) const
{
    GameList &l = grid(app);
    const Game *g = l.at(index_);
    const Platform *own = app.platform_by_id(grid_platform_);
    const Platform *p = app.platform_by_id(g ? g->platform_id : grid_platform_);
    if (!p)
        p = own;

    // The heading: where we are, then the focused game.
    const float in = tween::stagger(age, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    std::string where = grid_search_.empty() ? (own ? own->name : "Library")
                                             : "\xE2\x80\x9C" + grid_search_ + "\xE2\x80\x9D  \xC2\xB7  " +
                                                   (own ? "in " + own->name : "every platform");
    char count[48] = "";
    if (l.ready())
        std::snprintf(count, sizeof count, l.total == 1 ? "1 game" : "%d games", l.total);
    ui::text(list, fonts.semibold, ui::upper(fonts.semibold.font->fit(where, 20, 1300)), kMargin, 196, 20, accent,
             Align::left, 4.0f);
    ui::text(list, fonts.regular, count, 1824, 196, 22, look.muted, Align::right);
    const float fade = tween::smoothstep(tween::clamp01(shown_age_ / 0.25f));
    if (g)
    {
        list.push_opacity(fade);
        ui::text(list, fonts.display, fonts.display.font->fit(g->name, 60, 1600), kMargin - 3 + 12.0f * (1.0f - fade), 268,
                 60, look.text);
        std::string meta = human_size(g->size);
        if (g->versions > 1)
            meta = g->version + ", 1 of " + std::to_string(g->versions) + " versions  \xC2\xB7  " + meta;
        if (!grid_search_.empty() || !own)
            meta += "  \xC2\xB7  " + (p ? p->name : std::string());
        meta += "  \xC2\xB7  " + (p && !p->profile.empty() ? p->profile : std::string("No emulator"));
        // RomM's details when it has some: year, publisher, genres first.
        const Details &info = app.details(g->id);
        const std::string about = details_line(info);
        if (!about.empty())
        {
            if (info.play_ms > 0)
                meta += "  \xC2\xB7  played " + play_time(info.play_ms);
            ui::text(list, fonts.regular, fonts.regular.font->fit(about, 24, 1300), kMargin, 314, 24, look.muted);
        }
        else
            ui::text(list, fonts.regular, fonts.regular.font->fit(meta, 24, 1300), kMargin, 314, 24, look.muted);
        const Download *d = app.download_for(g->id);
        if (d && d->active())
        {
            const Rect bar{kMargin, 336, 420, 8};
            list.rounded_rect(bar, 4, look.muted.with_alpha(0.25f));
            list.rounded_rect({bar.x, bar.y, std::max(8.0f, bar.w * d->progress()), bar.h}, 4, accent);
        }
        else
        {
            const float w = ui::text(list, fonts.semibold, g->installed ? "Downloaded" : "In your RomM library", kMargin,
                                     352, 20, g->installed ? look.success : look.muted);
            // With the details on the line above, the file and emulator follow the state.
            if (!about.empty())
                ui::text(list, fonts.regular, fonts.regular.font->fit("\xC2\xB7  " + meta, 20, 1200), kMargin + w + 14,
                         352, 20, look.muted);
        }
        list.pop_opacity();
    }
    else if (!l.ready())
        ui::text(list, fonts.display, l.error.empty() ? "Loading" : "Couldn't load these games", kMargin - 3, 268, 60,
                 look.text);
    else if (l.total == 0)
    {
        ui::text(list, fonts.display, grid_search_.empty() ? "No games here" : "Nothing matches", kMargin - 3, 268, 60,
                 look.text);
        ui::text(list, fonts.regular, grid_search_.empty() ? "" : "Try fewer letters, or search every platform",
                 kMargin, 314, 24, look.muted);
    }
    if (!l.error.empty())
        ui::text(list, fonts.regular, fonts.regular.font->fit(l.error, 20, 1200), kMargin, 352, 20, look.danger);
    list.pop_opacity();

    // The grid.
    const float appear = tween::stagger(age, 2, 0.08f, 0.6f);
    list.push_opacity(appear);
    const float shake = ui::shake(nudge_.value, clock, 10.0f, 9.0f);
    const int total = std::max(0, l.total);
    const int first_row = std::max(0, static_cast<int>(grid_scroll_.value) - 1);
    list.push_clip({0, kGridTop - 30, kBarX - 10, 1080 - kGridTop + 30});
    Rect focus_rect{};
    for (int row = first_row; row < first_row + 5; ++row)
        for (int col = 0; col < kCols; ++col)
        {
            const int i = row * kCols + col;
            if (i >= total)
                break;
            const float y = kGridTop + (static_cast<float>(row) - grid_scroll_.value) * kRowPitch + 24 * (1.0f - appear);
            if (y > 1080 || y + kCardH < kGridTop - 40)
                continue;
            const Rect r{kMargin + static_cast<float>(col) * kColPitch, y, kCardW, kCardH};
            if (i == index_)
            {
                focus_rect = r;
                continue; // drawn last, on top
            }
            const Game *game = l.at(i);
            const float dim = y < kGridTop - 4 ? 0.35f : 0.85f;
            if (game)
            {
                draw_cover(list, fonts, look, app, game, r, dim, 14);
                draw_badge(list, look, app, game, r, accent);
                draw_version(list, fonts, look, game, r);
            }
            else
                list.rounded_rect(r, 14, look.panel.with_alpha(look.panel.a * 0.6f));
        }
    if (focus_rect.w > 0)
    {
        const float grow = 1.08f;
        Rect r{focus_rect.x - focus_rect.w * (grow - 1) * 0.5f + (on_letters_ ? 0.0f : shake),
               focus_rect.y - focus_rect.h * (grow - 1) * 0.5f, focus_rect.w * grow, focus_rect.h * grow};
        list.shadow({r.x, r.y + 16, r.w, r.h}, 18, 30, Color::rgb(0x000000, 0.55f));
        if (!on_letters_)
            list.glow(r, 18, 24, accent.with_alpha(0.4f + 0.15f * ui::breathe(clock)));
        draw_cover(list, fonts, look, app, l.at(index_), r, 1.0f, 18);
        draw_badge(list, look, app, l.at(index_), r, accent);
        draw_version(list, fonts, look, l.at(index_), r);
        list.bordered_rect(r.inset(-5), 22, Color::rgb(0x000000, 0.0f), 4,
                           look.focus.with_alpha(on_letters_ ? 0.35f : 1.0f));
    }
    list.pop_clip();
    // The covers of the two rows below are asked for too (after the ones on
    // screen), so scrolling down finds them ready.
    for (int i = (first_row + 5) * kCols; i < (first_row + 7) * kCols && i < total; ++i)
        if (const Game *game = l.at(i))
            app.cover(game->cover);
    // The row below fades out under the button hints.
    list.gradient_rect({0, 930, kBarX - 10, 150}, 0, look.page.with_alpha(0.0f), look.page.with_alpha(0.85f));

    // The A to Z bar.
    if (!l.letters.empty())
    {
        const Slots slots = slots_of(l);
        const int here = letter_at(l, index_);
        list.rounded_rect({kBarX, kBarTop, kBarW, kBarH}, 24, look.panel);
        list.bordered_rect({kBarX, kBarTop, kBarW, kBarH}, 24, Color::rgb(0x000000, 0.0f), 1.5f, look.outline);
        const float pitch = (kBarH - 24) / kSlots;
        for (int s = 0; s < kSlots; ++s)
        {
            const float cy = kBarTop + 12 + pitch * (static_cast<float>(s) + 0.5f);
            const bool has = slots[static_cast<std::size_t>(s)] >= 0;
            const bool focused = on_letters_ && s == letter_;
            if (focused)
                list.rounded_rect({kBarX + 16, cy - pitch * 0.5f - 2, kBarW - 32, pitch + 4}, 8, look.highlight);
            else if (s == here)
                list.rounded_rect({kBarX + 22, cy - pitch * 0.5f, kBarW - 44, pitch}, 8, accent.with_alpha(0.25f));
            const char text[2] = {slot_char(s), 0};
            ui::text(list, fonts.semibold, text, kBarX + kBarW * 0.5f, cy + 7, 19,
                     focused ? look.on_highlight
                     : has   ? (s == here ? look.panel_text : look.panel_muted)
                             : look.panel_muted.with_alpha(0.25f),
                     Align::center);
        }
    }
    list.pop_opacity();
}

// ------------------------------------------------------------------ search

void LibraryPage::open_search(int platform_id)
{
    before_search_ = mode_ == kSearch ? before_search_ : mode_;
    mode_ = kSearch;
    mode_age_ = 0.0f;
    search_platform_ = platform_id;
    query_.clear();
    applied_.clear();
    typed_age_ = 0.0f;
    keys_.set_length(0);
    keys_.enter();
}

void LibraryPage::update_search(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    keys_.style.theme = *look(current_look()).theme;
    keys_.set_length(static_cast<int>(query_.size()));
    const hui::ui::Event e = keys_.handle(input, feedback);
    keys_.update(dt);
    typed_age_ += dt;
    if (query_ != applied_ && typed_age_ > kSearchDelay)
        applied_ = query_;
    if (!applied_.empty())
        app.ensure(app.games(search_platform_, applied_), 0, 9);
    if (e == hui::ui::Event::cancelled)
    {
        mode_ = before_search_;
        mode_age_ = 0.0f;
    }
    else if (e == hui::ui::Event::activated)
    {
        if (query_.empty())
            refuse(feedback);
        else
            open_grid(app, search_platform_, query_);
    }
}

void LibraryPage::draw_search(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                              Color accent, float clock) const
{
    const Platform *p = app.platform_by_id(search_platform_);
    const float in = tween::stagger(mode_age_, 0, 0.06f, 0.5f);
    list.push_opacity(in);
    ui::text(list, fonts.semibold, ui::upper(p ? "Search " + p->name : std::string("Search every platform")), kMargin,
             196, 20, accent, Align::left, 4.0f);
    const Rect field{kMargin, 220, 1300, 84};
    draw_panel(list, look, field, 22);
    const float size = 36.0f;
    const std::string shown = tail(*fonts.regular.font, query_, size, field.w - 80);
    const float w = query_.empty() ? 0.0f
                                   : ui::text(list, fonts.regular, shown, field.x + 32, field.cy() + 13, size, look.panel_text);
    if (query_.empty())
        ui::text(list, fonts.regular, "A few letters of the name", field.x + 32, field.cy() + 13, size,
                 look.panel_muted.with_alpha(0.6f));
    if (std::fmod(clock, 1.0f) < 0.6f)
        list.rounded_rect({field.x + 34 + w, field.y + 20, 3, field.h - 40}, 1.5f, accent);

    // The first results, as you type.
    std::string status = "Type to search";
    const GameList *l = applied_.empty() ? nullptr : &app.games(search_platform_, applied_);
    if (l)
    {
        char text[64];
        if (!l->ready())
            std::snprintf(text, sizeof text, "Searching");
        else
            std::snprintf(text, sizeof text, l->total == 1 ? "1 game" : "%d games", l->total);
        status = l->error.empty() ? text : l->error;
    }
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(status, 24, 440), 1824, field.cy() + 9, 24, look.muted,
             Align::right);
    if (l)
    {
        for (int i = 0; i < 9; ++i)
        {
            const Game *g = l->at(i);
            if (!g)
                break;
            const Rect r{kMargin + static_cast<float>(i) * 192.0f, 340, 168, 224};
            draw_cover(list, fonts, look, app, g, r, 1.0f, 14);
            draw_badge(list, look, app, g, r, accent);
            draw_version(list, fonts, look, g, r);
        }
    }
    ui::Canvas canvas{list, fonts, 0, clock};
    keys_.draw(canvas);
    list.pop_opacity();
}

// ------------------------------------------------------------------ drawing

void LibraryPage::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                       float clock, float age) const
{
    switch (mode_)
    {
    case kPlatforms:
        draw_platforms(list, fonts, look, app, accent, clock, std::min(age, mode_age_));
        break;
    case kGrid:
        draw_grid(list, fonts, look, app, accent, clock, std::min(age, mode_age_));
        break;
    case kSearch:
        draw_search(list, fonts, look, app, accent, clock);
        break;
    }
}

std::vector<hui::ui::Hint> LibraryPage::hints() const
{
    using hui::ui::Button;
    switch (mode_)
    {
    case kPlatforms:
        return {{Button::l1, "Pages", Button::r1},
                {Button::triangle, "Search"},
                {Button::cross, section_ == 0 ? "Details" : "Open"}};
    case kGrid:
        if (on_letters_)
            return {{Button::cross, "Jump"}, {Button::circle, "Back"}};
        return {{Button::l2, "Letters", Button::r2},
                {Button::triangle, "Search"},
                {Button::cross, "Details"},
                {Button::circle, "Back"}};
    case kSearch:
        return {{Button::square, "Delete"}, {Button::r2, "Show all"}, {Button::circle, "Back"}};
    }
    return {};
}

} // namespace rommps
