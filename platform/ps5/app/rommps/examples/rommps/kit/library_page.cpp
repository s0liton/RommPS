// RommPS: the Library tab. See library_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "library_page.hpp"

#include "touch.hpp"
#include "ui/components/component.hpp"

#include <algorithm>
#include <array>
#include <cctype>
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

// The platform tiles start right under the page's heading.
constexpr float kPlatformTop = 196.0f;

// Platform tiles
constexpr int kTileCols = 4;
constexpr float kTileW = 408.0f, kTileH = 220.0f, kTileGap = 32.0f, kTilePitch = kTileH + kTileGap;
// The covers fanned on a platform's tile, and the room its name has beside
// them. The Deck's screen is small: the fan is tighter and the name wider.
#if defined(ROMMPS_DECK)
constexpr float kFanStep = 28.0f, kFanScale = 0.86f, kTileNameW = 190.0f;
#else
constexpr float kFanStep = 40.0f, kFanScale = 1.0f, kTileNameW = 165.0f;
#endif
constexpr float kHeadH = 64.0f; // a section's heading, above its first row
constexpr float kInstallArmed = 4.0f; // seconds the second Square of "Install all" has
// A grid of games: its columns and cards follow the shape of its covers
constexpr int kMinCols = 4, kMaxCols = 8;
constexpr float kGridTop = 400.0f, kGridW = 1620.0f, kCardTallest = 235.0f, kColGap = 24.0f, kRowGap = 25.0f;
// Search results, in a row
constexpr float kResultH = 224.0f, kResultGap = 24.0f, kResultsEnd = 1824.0f;
constexpr float kBarX = 1726.0f, kBarW = 98.0f, kBarTop = 392.0f, kBarH = 600.0f;
// The A to Z bar's slots: # (digits and the rest) then A to Z.
constexpr int kSlots = 27;
constexpr float kSearchDelay = 0.35f; // seconds after the last key before searching

using Slots = std::array<int, kSlots>; // each slot's first game, -1 for none

// The heading of a collection's section: its kind, or a virtual one's type.
std::string section_of(const Collection &c)
{
    if (c.kind == "regular")
        return "Collections";
    if (c.kind == "smart")
        return "Smart collections";
    static const struct
    {
        const char *type, *heading;
    } named[] = {{"franchise", "Franchises"}, {"collection", "Series"}, {"genre", "Genres"},
                 {"company", "Companies"},    {"mode", "Game modes"}};
    for (const auto &n : named)
        if (c.type == n.type)
            return n.heading;
    if (c.type.empty())
        return "More collections";
    std::string t = c.type;
    std::replace(t.begin(), t.end(), '_', ' ');
    t[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(t[0])));
    return t;
}

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
    // A character at a time: a cut inside one would draw as "?"
    std::size_t from = 0;
    while (from < text.size() && font.measure(std::string_view(text).substr(from), size) > width)
        from = char_boundary(text, from + 1);
    return from ? "\xE2\x80\xA6" + text.substr(char_boundary(text, from + 1)) : text;
}

struct GridLayout
{
    float shape; // of the cards, width over height
    int cols;
    float card_w, card_h, col_pitch, row_pitch;
};

// As many columns as cards of the grid's shape fit across (8 of 3:4 covers,
// 6 of square ones, 5 of the wide SNES and N64 boxes), the cards a little
// shorter when that fills the width better. Every card the same size.
GridLayout grid_layout(const GameList &l)
{
    const float shape = std::clamp(l.shape > 0 ? l.shape : kPortrait, 0.7f, kWidest);
    const int cols = std::clamp(static_cast<int>(std::lround((kGridW + kColGap) / (kCardTallest * shape + kColGap))),
                                kMinCols, kMaxCols);
    const float w = std::min(kCardTallest * shape, (kGridW - kColGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
    return {shape, cols, w, w / shape, w + kColGap, w / shape + kRowGap};
}

// A cover in a grid's card: its own shape, centred. One much wider than the
// card keeps most of the card's height, cropped at the sides.
Rect in_card(float shape, const Rect &card)
{
    Rect r = fit_cover(shape, card);
    const float least = card.h * 0.75f;
    if (r.h < least)
        r.y = card.y + (card.h - least) * 0.5f, r.h = least;
    return r;
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

float cover_shape(App &app, const Game *g, float fallback)
{
    double since = 0;
    const float own = g && !g->cover.empty() ? app.cover_aspect(g->cover, &since) : 0.0f;
    if (own <= 0)
        return fallback;
    const float t = tween::smoothstep(tween::clamp01(static_cast<float>(app.clock() - since) / 0.3f));
    return fallback + (std::clamp(own, kNarrowest, kWidest) - fallback) * t;
}

Rect fit_cover(float shape, const Rect &box)
{
    shape = std::clamp(shape, kNarrowest, kWidest);
    float w = box.h * shape, h = box.h;
    if (w > box.w)
        w = box.w, h = box.w / shape;
    return {box.x + (box.w - w) * 0.5f, box.y + (box.h - h) * 0.5f, w, h};
}

bool draw_logo(hui::gfx::DrawList &list, App &app, const std::string &slug, const Rect &box, float alpha)
{
    const Cover &logo = app.platform_logo(slug);
    if (!logo.texture)
        return false;
    list.image(logo.texture, fit_cover(logo.aspect > 0 ? logo.aspect : 1.0f, box), hui::gfx::kFullUv,
               Color::rgb(0xFFFFFF, alpha));
    return true;
}

void draw_cover(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, const Game *g,
                const Rect &rect, float alpha, float radius, bool name)
{
    static const Cover kNone;
    const Cover &c = g ? app.cover(g->cover) : kNone;
    if (c.texture)
    {
        // The middle of the cover in the rectangle's shape: cropped, never stretched.
        Rect uv = hui::gfx::kFullUv;
        const float want = rect.h > 0 ? rect.w / rect.h : 1.0f;
        if (c.aspect > want)
            uv.w = want / c.aspect, uv.x = (1.0f - uv.w) * 0.5f;
        else if (c.aspect > 0 && c.aspect < want)
            uv.h = c.aspect / want, uv.y = (1.0f - uv.h) * 0.5f;
        list.image(c.texture, rect, uv, Color::rgb(0xffffff, alpha), radius);
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
    else if (app.on_console(*g))
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
    if (!g || g->versions < 2)
        return;
    const float size = rect.w > 160 ? 17.0f : 15.0f, pad = 10;
    const std::string text =
        fonts.semibold.font->fit(std::to_string(g->versions) + " versions", size, rect.w - 24 - 2 * pad);
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

void LibraryPage::enter(App &)
{
    mode_age_ = 0.0f;
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
    if (mode_ == kGrid)
        return grid(app).at(index_);
    return nullptr;
}

std::string LibraryPage::backdrop_cover(App &app) const
{
    if (const Game *g = focused(app))
        return g->cover;
    if (mode_ == kPlatforms)
    {
        const std::vector<Tile> all = tiles(app);
        if (tile_ >= 0 && tile_ < static_cast<int>(all.size()))
            if (const Game *first = app.games(all[static_cast<std::size_t>(tile_)].scope, "").at(0))
                return first->cover;
    }
    return "";
}

std::vector<LibraryPage::Tile> LibraryPage::tiles(App &app) const
{
    std::vector<Tile> out;
    if (browse_ == kByPlatform)
        for (const Platform &p : app.platforms())
            out.push_back({Scope::platform(p.id), p.name, p.slug, p.profile.empty() ? "No emulator" : p.profile,
                           !p.profile.empty(), p.rom_count});
    else
        for (const Collection &c : app.collections())
            out.push_back({Scope::collection(c), c.name, "",
                           !c.owner.empty()         ? "by " + c.owner
                           : c.description.empty() ? std::string(c.kind == "virtual" ? "" : "Collection")
                                                   : c.description,
                           false, c.rom_count, section_of(c)});
    return out;
}

LibraryPage::Placement LibraryPage::place(const std::vector<Tile> &all)
{
    Placement p;
    int row = -1, col = kTileCols;
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const bool starts = i == 0 || all[i].section != all[i - 1].section;
        if (starts || col == kTileCols)
        {
            ++row;
            col = 0;
            if (starts && !all[i].section.empty())
                p.heads.emplace_back(row, all[i].section);
        }
        p.row.push_back(row);
        p.col.push_back(col++);
    }
    return p;
}

float LibraryPage::Placement::top(int r) const
{
    int heads_on = 0;
    for (const auto &h : heads)
        heads_on += h.first <= r;
    return static_cast<float>(r) * kTilePitch + static_cast<float>(heads_on) * kHeadH;
}

int LibraryPage::Placement::first_of(int r) const
{
    for (std::size_t i = 0; i < row.size(); ++i)
        if (row[i] == r)
            return static_cast<int>(i);
    return -1;
}

int LibraryPage::Placement::last_of(int r) const
{
    int last = -1;
    for (std::size_t i = 0; i < row.size(); ++i)
        if (row[i] == r)
            last = static_cast<int>(i);
    return last;
}

std::string LibraryPage::scope_name(App &app, const Scope &scope)
{
    if (scope.in_collection())
    {
        const Collection *c = app.collection_by_id(scope.collection_kind, scope.collection_id);
        return c ? c->name : "Collection";
    }
    const Platform *p = app.platform_by_id(scope.platform_id);
    return p ? p->name : "";
}

bool LibraryPage::update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    mode_age_ += dt;
    shown_age_ += dt;
    install_armed_ = std::max(0.0f, install_armed_ - dt);
    nudge_.update(dt, 9.0f);
    bool details = false;
    switch (mode_)
    {
    case kPlatforms:
        update_platforms(app, input, feedback);
        break;
    case kGrid:
        update_grid(app, input, feedback, details);
        break;
    case kSearch:
        update_search(app, input, dt, feedback, details);
        break;
    }
    // Scrolling: the focused tile's row in view, with its section's heading
    // when the row is the section's first.
    if (mode_ == kPlatforms && !tiles_free_)
    {
        const Placement at = place(tiles(app));
        if (tile_ >= 0 && tile_ < static_cast<int>(at.row.size()))
        {
            const float view = std::max(1.0f, std::floor((kScreenH - 80.0f - kPlatformTop - 24.0f) / kTilePitch)) * kTilePitch;
            const int r = at.row[static_cast<std::size_t>(tile_)];
            float top = at.top(r);
            const float bottom = top + kTileH;
            for (const auto &h : at.heads)
                if (h.first == r)
                    top -= kHeadH;
            if (r == 0)
                top = 0.0f;
            if (top < tiles_scroll_.target)
                tiles_scroll_.target = top;
            if (bottom > tiles_scroll_.target + view)
                tiles_scroll_.target = bottom - view;
        }
    }
    tiles_scroll_.update(dt, 14.0f);
    if (mode_ == kGrid && !grid_free_)
        grid_scroll_.target = static_cast<float>(std::max(0, index_ / grid_layout(grid(app)).cols - 1));
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

void LibraryPage::update_platforms(App &app, const hui::InputFrame &pad, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    if (browse_ == kByCollection)
        app.want_collections(); // the first time they're looked at, or after pairing again
    const std::vector<Tile> all = tiles(app);
    const int ntiles = static_cast<int>(all.size());
    tile_ = std::clamp(tile_, 0, std::max(0, ntiles - 1));
    const Placement at = place(all);

    // The covers of the tiles in view.
    for (int i = 0; i < ntiles; ++i)
    {
        const float y = at.top(at.row[static_cast<std::size_t>(i)]) - tiles_scroll_.value;
        if (y > -kTilePitch && y < 3 * kTilePitch)
            app.ensure(app.games(all[static_cast<std::size_t>(i)].scope, ""), 0, 0);
    }

    // Touch: a tile tapped opens; a drag scrolls the tiles, and the pad then
    // picks up from the first row in view.
    {
        const float view =
            std::max(1.0f, std::floor((kScreenH - 80.0f - kPlatformTop - 24.0f) / kTilePitch)) * kTilePitch;
        const float most = ntiles ? at.top(at.row.back()) + kTileH - view : 0.0f;
        const touch::Touch t = touch::take(&tiles_scroll_);
        if (touch::scroll(tiles_scroll_, t, 1.0f, most))
            tiles_free_ = true;
        if (t.index >= 0 && t.index < ntiles)
        {
            tile_ = t.index;
            tiles_free_ = false;
            input = touch::with(input, t);
        }
        if (tiles_free_ && input.nav != Direction::none && ntiles > 0)
        {
            tiles_free_ = false;
            const int col = at.col[static_cast<std::size_t>(tile_)];
            for (int r = 0; r <= at.row.back(); ++r)
                if (at.top(r) >= tiles_scroll_.value - 1.0f && at.first_of(r) >= 0)
                {
                    tile_ = std::min(at.first_of(r) + col, at.last_of(r));
                    break;
                }
            feedback.play(Cue::focus);
            input.nav = Direction::none;
        }
    }
    const bool repeat = input.nav_repeat;
    const int row = ntiles ? at.row[static_cast<std::size_t>(tile_)] : 0;
    const int col = ntiles ? at.col[static_cast<std::size_t>(tile_)] : 0;
    // The tile on another row under (or over) this one: the same column, or
    // that row's last when it's shorter.
    auto on_row = [&](int r) {
        const int first = at.first_of(r);
        return first < 0 ? -1 : std::min(first + col, at.last_of(r));
    };
    switch (input.nav)
    {
    case Direction::left:
    case Direction::right:
    {
        const int next = tile_ + (input.nav == Direction::right ? 1 : -1);
        if (next >= 0 && next < ntiles && at.row[static_cast<std::size_t>(next)] == row)
        {
            tile_ = next;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    }
    case Direction::up:
    case Direction::down:
    {
        const int next = ntiles ? on_row(row + (input.nav == Direction::down ? 1 : -1)) : -1;
        if (next >= 0)
        {
            tile_ = next;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    }
    case Direction::none:
        break;
    }
    // L2 / R2: the section before or after (back to the start of this one
    // first, when the focus is past it).
    if (ntiles > 0 && !at.heads.empty() && (input.is_pressed(Action::jump_prev) || input.is_pressed(Action::jump_next)))
    {
        int here = 0; // this section, an index into at.heads
        for (std::size_t h = 0; h < at.heads.size(); ++h)
            if (at.heads[h].first <= row)
                here = static_cast<int>(h);
        int to = here + (input.is_pressed(Action::jump_next) ? 1 : -1);
        if (input.is_pressed(Action::jump_prev) && tile_ != at.first_of(at.heads[static_cast<std::size_t>(here)].first))
            to = here;
        if (to >= 0 && to < static_cast<int>(at.heads.size()))
        {
            tile_ = at.first_of(at.heads[static_cast<std::size_t>(to)].first);
            feedback.play(Cue::focus);
        }
        else
            refuse(feedback);
    }
    if (input.is_pressed(Action::confirm) && ntiles > 0)
    {
        feedback.play(Cue::open);
        open_grid(app, all[static_cast<std::size_t>(tile_)].scope, "");
    }
    if (input.is_pressed(Action::west))
    {
        browse_ = browse_ == kByPlatform ? kByCollection : kByPlatform;
        tile_ = 0;
        tiles_free_ = false;
        tiles_scroll_.snap(0.0f);
        tiles_scroll_.target = 0.0f;
        mode_age_ = 0.0f;
        feedback.play(Cue::tab);
        if (browse_ == kByCollection)
        {
            app.reload_platforms(); // a collection's games show their platform's emulator
            app.refresh_collections(); // which may have changed in RomM
        }
    }
    if (input.is_pressed(Action::north))
    {
        feedback.play(Cue::modal_open);
        open_search(Scope{});
    }
}

#if defined(ROMMPS_DECK)
// RommDeck's tiles: the Deck's screen is small, so a platform's name is split
// into its maker and its model, and the focused one scrolls when it's long.
namespace
{

// The maker that leads a platform's name ("Sony PlayStation 2"): shown on a
// line of its own, so the model, which tells similar ones apart, gets the room.
constexpr const char *kMakers[] = {"Nintendo", "Sega", "Sony", "SNK", "Atari", "NEC", "Commodore", "Bandai",
                                   "Microsoft", "Sinclair", "Philips", "Panasonic", "Fairchild", "Coleco",
                                   "Mattel", "Magnavox", "Amstrad", "Apple", "Acorn", "Casio", "Epoch", "Tiger"};

// Names whose maker isn't their first word, or that are long for a tile.
constexpr const char *kAliases[][3] = {
    {"Super Nintendo Entertainment System", "Nintendo", "Super Nintendo"},
};

void split_maker(const std::string &name, std::string *maker, std::string *model)
{
    maker->clear();
    *model = name;
    for (const auto &a : kAliases)
        if (name == a[0])
        {
            *maker = a[1];
            *model = a[2];
            return;
        }
    const std::size_t space = name.find(' ');
    if (space == std::string::npos || space + 1 >= name.size())
        return;
    const std::string rest = name.substr(space + 1);
    // "Nintendo 64", "Nintendo DS": the model alone says too little.
    if (rest.size() <= 3 || std::isdigit(static_cast<unsigned char>(rest[0])))
        return;
    for (const char *m : kMakers)
        if (name.compare(0, space, m) == 0)
        {
            *maker = m;
            *model = rest;
            return;
        }
}

} // namespace

// A tile's name: the maker small and upper case, the model under it in up to
// two lines, a little smaller when its longest word wouldn't fit. On the
// focused tile, a model that still doesn't fit scrolls: it rests, slides to
// its end, rests, and slides back.
void LibraryPage::draw_tile_name(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                                 const std::string &name, float x, float top, int focused) const
{
    std::string maker, model;
    split_maker(name, &maker, &model);
    const float width = kTileNameW;
    float y = top + 56;
    if (!maker.empty())
    {
        ui::text(list, fonts.semibold, ui::upper(maker), x, top + 44, 17, look.panel_muted, Align::left, 1.5f);
        y = top + 82;
    }
    // The longest word sets the size: 28, down to 24.
    float size = 28.0f;
    for (std::size_t start = 0; start < model.size();)
    {
        std::size_t end = model.find(' ', start);
        if (end == std::string::npos)
            end = model.size();
        while (size > 24.0f && fonts.semibold.font->measure(model.substr(start, end - start), size) > width)
            size -= 1.0f;
        start = end + 1;
    }
    const std::vector<std::string> lines = fonts.semibold.font->wrap(model, size, width);
    const bool fits = lines.size() <= 2 && fonts.semibold.font->measure(lines.empty() ? "" : lines[0], size) <= width;
    if (fits || focused < 0)
    {
        // At most two lines, each cut short with an ellipsis rather than
        // running past the tile (a word wider than the room isn't broken).
        const float line_h = maker.empty() ? 34.0f : 31.0f;
        for (std::size_t i = 0; i < lines.size() && i < 2; ++i)
        {
            const bool more = i == 1 && lines.size() > 2;
            ui::text(list, fonts.semibold, fonts.semibold.font->fit(more ? lines[i] + " \xE2\x80\xA6" : lines[i], size, width),
                     x, y + static_cast<float>(i) * line_h, size, look.panel_text);
        }
        return;
    }
    if (marquee_tile_ != focused)
    {
        marquee_tile_ = focused;
        marquee_since_ = app.clock();
    }
    const float overflow = fonts.semibold.font->measure(model, size) - width;
    const float rest = 1.2f, speed = 60.0f, slide = overflow / speed;
    const float t = std::fmod(static_cast<float>(app.clock() - marquee_since_), 2.0f * (rest + slide));
    const float shift = t < rest ? 0.0f
                        : t < rest + slide ? (t - rest) * speed
                        : t < 2.0f * rest + slide ? overflow
                                                  : overflow - (t - 2.0f * rest - slide) * speed;
    list.push_clip({x, y - size - 6, width, size + 16});
    ui::text(list, fonts.semibold, model, x - shift, y, size, look.panel_text);
    list.pop_clip();
}
#endif

void LibraryPage::draw_platforms(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                                 Color accent, float clock, float age) const
{
    const std::vector<Tile> all = tiles(app);
    const float shake = ui::shake(nudge_.value, clock, 10.0f, 9.0f);

    // Platforms or collections
    const float top = kPlatformTop;
    const float in = tween::stagger(age, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    char label[64];
    std::snprintf(label, sizeof label, "%s  \xC2\xB7  %d", browse_ == kByPlatform ? "Platforms" : "Collections",
                  static_cast<int>(all.size()));
    ui::text(list, fonts.semibold, label, kMargin, top, 24, look.text);
    if (all.empty())
    {
        const bool failed = browse_ == kByCollection && !app.collections_error().empty();
        std::string empty = "Loading your platforms";
        if (browse_ == kByCollection)
            empty = !app.collections_loaded() ? "Loading your collections"
                    : failed                  ? "Couldn't load your collections: " + app.collections_error()
                                              : "No collections with games in RomM yet";
        ui::text(list, fonts.regular, fonts.regular.font->fit(empty, 26, 1700), kMargin, top + 60, 26,
                 failed ? look.danger : look.muted);
    }
    const float grid_top = top + 24;
    const Placement at = place(all);
    list.push_clip({0, grid_top - 20, kScreenW, kScreenH - grid_top + 20});
    touch::zone(list, {0, grid_top - 20, kScreenW, kScreenH - grid_top + 20}, &tiles_scroll_, -1);
    for (const auto &[row, heading] : at.heads)
    {
        const float y = grid_top + at.top(row) - kHeadH - tiles_scroll_.value;
        if (y > kScreenH || y + kHeadH < grid_top - 20)
            continue;
        ui::text(list, fonts.semibold, heading, kMargin, y + 40, 22, look.muted);
    }
    for (int i = 0; i < static_cast<int>(all.size()); ++i)
    {
        const Tile &p = all[static_cast<std::size_t>(i)];
        const int row = at.row[static_cast<std::size_t>(i)], col = at.col[static_cast<std::size_t>(i)];
        const float y = grid_top + at.top(row) - tiles_scroll_.value;
        if (y > kScreenH || y + kTileH < grid_top - 20)
            continue;
        const bool on = i == tile_;
        Rect r{kMargin + static_cast<float>(col) * (kTileW + kTileGap) + (on ? shake : 0.0f), y, kTileW, kTileH};
        touch::zone(list, r, &tiles_scroll_, i);
        if (on)
            list.glow(r, 26, 26, accent.with_alpha(0.4f));
        draw_panel(list, look, r, 26);
        // A few of its covers, fanned on the right.
        GameList &games = app.games(p.scope, "");
        for (int k = 2; k >= 0; --k)
        {
            const Game *g = games.at(k);
            if (!g)
                continue;
            // Each its own shape, as tall as fits 120 wide, standing on the same line.
            const float shape = cover_shape(app, g);
            float h = std::min(133.0f, 120.0f / shape) * kFanScale, w = h * shape;
            // Just loaded, it pops in: from a little smaller, past its size and back.
            double since = 0;
            if (!g->cover.empty() && app.cover_aspect(g->cover, &since) > 0)
            {
                const float pop = 0.85f + 0.15f * tween::back_out(tween::clamp01(static_cast<float>(app.clock() - since) / 0.35f));
                w *= pop;
                h *= pop;
            }
            const float right = r.x + r.w - 28 - static_cast<float>(k) * kFanStep - std::min(133.0f, 120.0f / shape) * shape * 0.5f;
            const Rect c{right - w * 0.5f, r.y + 161 + static_cast<float>(k) * 10 - h, w, h};
            list.shadow({c.x, c.y + 8, c.w, c.h}, 10, 16, Color::rgb(0x000000, 0.35f));
            draw_cover(list, fonts, look, app, g, c, 1.0f - 0.15f * static_cast<float>(k), 10, false);
        }
        // The system's logo, small, in the corner under the covers.
        if (!p.slug.empty())
            draw_logo(list, app, p.slug, {r.x + r.w - 28 - 30, r.y + 172, 30, 30}, 0.9f);
#if defined(ROMMPS_DECK)
        draw_tile_name(list, fonts, look, app, p.name, r.x + 28, r.y, on ? i : -1);
#else
        ui::paragraph(list, fonts.semibold, p.name, r.x + 28, r.y + 56, 28, 165, 34, look.panel_text, 2);
#endif
        char count[48];
        std::snprintf(count, sizeof count, p.count == 1 ? "%d game" : "%d games", p.count);
        ui::text(list, fonts.regular, count, r.x + 28, r.y + 150, 22, look.panel_muted);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(p.note, 18, 200), r.x + 28, r.y + 190, 18,
                 p.note_on ? accent : look.panel_muted.with_alpha(0.6f), Align::left, 1.5f);
        if (on)
            list.bordered_rect(r.inset(-6), 30, Color::rgb(0x000000, 0.0f), 4, look.focus.with_alpha(1.0f));
    }
    list.pop_clip();
    list.pop_opacity();
}

// ------------------------------------------------------------------ a grid

void LibraryPage::open_grid(App &app, const Scope &scope, const std::string &search)
{
    grid_scope_ = scope;
    grid_search_ = search;
    install_armed_ = 0.0f;
    mode_ = kGrid;
    mode_age_ = 0.0f;
    index_ = 0;
    on_letters_ = false;
    grid_free_ = false;
    grid_scroll_.snap(0.0f);
    app.retry(grid(app)); // opened again: what failed last time is asked for again
    app.ensure(grid(app), 0, kMaxCols * 4);
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
    settle_shape(app, l);
    move_in_grid(app, input, feedback, details);
    GameList &now = grid(app);
    focus_list_ = &now;
    focus_raw_ = now.ready() ? now.raw(index_) : index_;
    focus_hidden_ = now.hidden.size();
    grid_failed_ = !now.error.empty() && !now.at(index_);
}

void LibraryPage::settle_shape(App &app, GameList &l) const
{
    if (l.shape_settled || !l.ready())
        return;
    // The covers of the first rows: the grid takes their middle shape.
    std::vector<float> seen;
    int wanted = 0;
    for (int i = 0; i < std::min(l.total, 2 * kMaxCols); ++i)
        if (const Game *g = l.at(i); g && !g->cover.empty())
        {
            ++wanted;
            if (const float a = app.cover_aspect(g->cover); a > 0)
                seen.push_back(std::clamp(a, kNarrowest, kWidest));
        }
    if (!seen.empty())
    {
        std::nth_element(seen.begin(), seen.begin() + static_cast<std::ptrdiff_t>(seen.size() / 2), seen.end());
        l.shape = seen[seen.size() / 2];
    }
    // It settles once those covers are all here, or enough of them are and
    // the grid is in use, so it never changes under the focus.
    const int known = static_cast<int>(seen.size());
    l.shape_settled = known >= wanted || (known >= 4 && (index_ > 0 || mode_age_ > 1.5f)) || mode_age_ > 4.0f;
}

void LibraryPage::move_in_grid(App &app, const hui::InputFrame &pad, hui::ui::Feedback &feedback, bool &details)
{
    hui::InputFrame input = pad;
    GameList &l = grid(app);
    const int cols = grid_layout(l).cols;
    // Touch: a cover tapped opens its details; a drag scrolls the grid; the
    // A to Z bar jumps to the letter under the finger as it slides.
    {
        const int last_row = l.total > 0 ? (l.total - 1) / cols : 0;
        const touch::Touch t = touch::take(&grid_scroll_);
        if (touch::scroll(grid_scroll_, t, grid_layout(l).row_pitch, static_cast<float>(last_row - 1)))
            grid_free_ = true;
        if (t.index >= 0 && t.index < l.total)
        {
            index_ = t.index;
            on_letters_ = false;
            grid_free_ = false;
            input = touch::with(input, t);
        }
        if (const touch::Touch bar = touch::take(&letter_);
            bar.index >= 0 && bar.index < kSlots && slots_of(l)[static_cast<std::size_t>(bar.index)] >= 0)
        {
            on_letters_ = false;
            grid_free_ = false;
            letter_ = bar.index;
            jump_letter(l, bar.index, feedback);
        }
        // The pad after a touch scroll: from the first row in view.
        if (grid_free_ && !on_letters_ && input.nav != Direction::none && l.total > 0)
        {
            grid_free_ = false;
            const int top_row = std::clamp(static_cast<int>(std::lround(grid_scroll_.value)) + 1, 0, last_row);
            index_ = std::min(l.total - 1, top_row * cols + index_ % cols);
            feedback.play(Cue::focus);
            input.nav = Direction::none;
        }
    }
    // The games around where the grid is going, not the rows it scrolls past
    // on the way (a jump to a letter animates through everything between).
    const int target_row = grid_free_ ? std::max(0, static_cast<int>(grid_scroll_.target) - 1)
                                      : std::max(0, index_ / cols - 2);
    app.ensure(l, target_row * cols, (target_row + 7) * cols);
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
        open_search(grid_scope_);
        return;
    }
    // A collection: Square, then Square again, downloads every game of it
    // that isn't here yet. Not from a search in it: that's a few of its games.
    if (input.is_pressed(Action::west) && grid_scope_.in_collection() && grid_search_.empty() && !on_letters_)
    {
        const Collection *c = app.collection_by_id(grid_scope_.collection_kind, grid_scope_.collection_id);
        if (!c)
            refuse(feedback);
        else if (install_armed_ <= 0.0f)
        {
            install_armed_ = kInstallArmed;
            feedback.play(Cue::toggle);
        }
        else
        {
            install_armed_ = 0.0f;
            app.download_collection(*c);
            feedback.play(Cue::launch);
            feedback.rumble(0.7f, 0.18f);
        }
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
    // RomM failed on the games here: Cross asks again. Nothing else does.
    if (!l.error.empty() && !l.at(index_) && input.is_pressed(Action::confirm))
    {
        if (app.retry(l))
            feedback.play(Cue::open);
        else
            refuse(feedback);
        return;
    }
    if (total == 0)
        return;
    switch (input.nav)
    {
    case Direction::left:
        if (index_ % cols > 0)
        {
            --index_;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::right:
        if (index_ % cols < cols - 1 && index_ + 1 < total)
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
        if (index_ - cols >= 0)
        {
            index_ -= cols;
            feedback.play(Cue::focus);
        }
        else if (!repeat)
            refuse(feedback);
        break;
    case Direction::down:
        if (index_ + cols < total)
        {
            index_ += cols;
            feedback.play(Cue::focus);
        }
        else if (index_ / cols < (total - 1) / cols)
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
    // A platform's own games, not a collection's (which mixes platforms).
    const Platform *own = grid_scope_.in_collection() ? nullptr : app.platform_by_id(grid_scope_.platform_id);
    const Platform *p = app.platform_by_id(g ? g->platform_id : grid_scope_.platform_id);
    if (!p)
        p = own;

    // The heading: where we are, then the focused game.
    const float in = tween::stagger(age, 1, 0.08f, 0.6f);
    list.push_opacity(in);
    const std::string name = scope_name(app, grid_scope_);
    std::string where = grid_search_.empty() ? (name.empty() ? "Library" : name)
                                             : "\xE2\x80\x9C" + grid_search_ + "\xE2\x80\x9D  \xC2\xB7  " +
                                                   (name.empty() ? "every platform" : "in " + name);
    char count[48] = "";
    if (l.ready())
        std::snprintf(count, sizeof count, l.total == 1 ? "1 game" : "%d games", l.total);
    // What was typed, in curly quotes (plain_text turns them straight in a font without them)
    ui::text(list, fonts.semibold, ui::upper(fonts.semibold.font->fit(plain_text(where.c_str()), 20, 1300)), kMargin, 196, 20,
             accent, Align::left, 4.0f);
    ui::text(list, fonts.regular, count, 1824, 196, 22, look.muted, Align::right);
    const float fade = tween::smoothstep(tween::clamp01(shown_age_ / 0.25f));
    if (g)
    {
        list.push_opacity(fade);
        ui::text(list, fonts.display, fonts.display.font->fit(g->name, 60, 1600), kMargin - 3 + 12.0f * (1.0f - fade), 268,
                 60, look.text);
        std::string meta = human_size(g->size);
        if (g->versions > 1)
            meta = std::to_string(g->versions) + " versions  \xC2\xB7  " + meta;
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
            if (d->syncing())
                ui::text(list, fonts.semibold, "Syncing saves", bar.x + bar.w + 20, 344, 20, look.muted);
        }
        else
        {
            // "Downloaded" when it is; with the details on the line above, the file and emulator follow.
            const bool here = app.on_console(*g);
            const float w = here ? ui::text(list, fonts.semibold, "Downloaded", kMargin, 352, 20, look.success) + 14 : 0.0f;
            if (!about.empty())
                ui::text(list, fonts.regular, fonts.regular.font->fit((here ? "\xC2\xB7  " : "") + meta, 20, 1200),
                         kMargin + w, 352, 20, look.muted);
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
    const GridLayout layout = grid_layout(l);
    const int cols = layout.cols;
    const int first_row = std::max(0, static_cast<int>(grid_scroll_.value) - 1);
    const int rows = static_cast<int>((kScreenH - kGridTop) / layout.row_pitch) + 3;
    list.push_clip({0, kGridTop - 30, kBarX - 10, kScreenH - kGridTop + 30});
    touch::zone(list, {0, kGridTop - 30, kBarX - 10, kScreenH - kGridTop + 30}, &grid_scroll_, -1);
    Rect focus_rect{};
    for (int row = first_row; row < first_row + rows; ++row)
        for (int col = 0; col < cols; ++col)
        {
            const int i = row * cols + col;
            if (i >= total)
                break;
            const float y = kGridTop + (static_cast<float>(row) - grid_scroll_.value) * layout.row_pitch + 24 * (1.0f - appear);
            if (y > kScreenH || y + layout.card_h < kGridTop - 40)
                continue;
            const Game *game = l.at(i);
            const Rect card{kMargin + static_cast<float>(col) * layout.col_pitch, y, layout.card_w, layout.card_h};
            const Rect r = game ? in_card(cover_shape(app, game, layout.shape), card) : card;
            touch::zone(list, r, &grid_scroll_, i);
            if (i == index_)
            {
                focus_rect = r;
                continue; // drawn last, on top
            }
            // Rows above where the grid is going are dimmed: one scrolling in
            // brightens as soon as the move starts, not once it has settled.
            const float above = tween::clamp01(grid_scroll_.target - static_cast<float>(row));
            const float dim = 0.85f - 0.5f * above;
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
    for (int i = (first_row + rows) * cols; i < (first_row + rows + 2) * cols && i < total; ++i)
        if (const Game *game = l.at(i))
            app.cover(game->cover);
    // The row below fades out under the button hints.
    list.gradient_rect({0, kScreenH - 150, kBarX - 10, 150}, 0, look.page.with_alpha(0.0f), look.page.with_alpha(0.85f));

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
            touch::zone(list, {kBarX - 20, cy - pitch * 0.5f, kBarW + 40, pitch}, &letter_, s, touch::kScrub);
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

void LibraryPage::open_search(const Scope &scope)
{
    before_search_ = mode_ == kSearch ? before_search_ : mode_;
    mode_ = kSearch;
    mode_age_ = 0.0f;
    search_scope_ = scope;
    query_.clear();
    applied_.clear();
    typed_age_ = 0.0f;
    keys_.set_length(0);
    keys_.enter();
}

void LibraryPage::update_search(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback,
                                bool &details)
{
    hui::InputFrame input = touch::type(keys_, pad);
    // A result touched: the search's grid, and that game's details
    if (const touch::Touch t = touch::take(&applied_); t.pressed && t.index >= 0 && !applied_.empty())
    {
        open_grid(app, search_scope_, applied_);
        GameList &l = grid(app);
        index_ = t.index;
        focus_list_ = &l;
        focus_raw_ = l.ready() ? l.raw(index_) : index_;
        focus_hidden_ = l.hidden.size();
        details = true;
        feedback.play(Cue::open);
        return;
    }
    keys_.style.theme = *look(current_look()).theme;
    keys_.set_length(static_cast<int>(query_.size()));
    const hui::ui::Event e = keys_.handle(input, feedback);
    keys_.update(dt);
    typed_age_ += dt;
    if (query_ != applied_ && typed_age_ > kSearchDelay)
        applied_ = query_;
    if (!applied_.empty())
        app.ensure(app.games(search_scope_, applied_), 0, 9);
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
            open_grid(app, search_scope_, query_);
    }
}

void LibraryPage::draw_search(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app,
                              Color accent, float clock) const
{
    const std::string name = scope_name(app, search_scope_);
    const float in = tween::stagger(mode_age_, 0, 0.06f, 0.5f);
    list.push_opacity(in);
    ui::text(list, fonts.semibold, ui::upper(name.empty() ? std::string("Search every platform") : "Search " + name),
             kMargin, 196, 20, accent, Align::left, 4.0f);
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
    const GameList *l = applied_.empty() ? nullptr : &app.games(search_scope_, applied_);
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
        float x = kMargin;
        for (int i = 0; i < 9; ++i)
        {
            const Game *g = l->at(i);
            if (!g)
                break;
            const Rect r{x, 340, kResultH * cover_shape(app, g), kResultH};
            if (r.x + r.w > kResultsEnd)
                break;
            x += r.w + kResultGap;
            touch::zone(list, r, &applied_, i);
            draw_cover(list, fonts, look, app, g, r, 1.0f, 14);
            draw_badge(list, look, app, g, r, accent);
            draw_version(list, fonts, look, g, r);
        }
    }
    ui::Canvas canvas{list, fonts, 0, clock};
    keys_.draw(canvas);
    touch::keys(list, keys_);
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
        if (browse_ == kByCollection)
            return {{Button::l1, "Pages", Button::r1},
                    {Button::l2, "Sections", Button::r2},
                    {Button::square, "Platforms"},
                    {Button::triangle, "Search"},
                    {Button::cross, "Open"}};
        return {{Button::l1, "Pages", Button::r1},
                {Button::square, "Collections"},
                {Button::triangle, "Search"},
                {Button::cross, "Open"}};
    case kGrid:
        if (on_letters_)
            return {{Button::cross, "Jump"}, {Button::circle, "Back"}};
        if (grid_scope_.in_collection() && grid_search_.empty())
            return {{Button::l2, "Letters", Button::r2},
                    {Button::square, install_armed_ > 0.0f ? "Press again to install all" : "Install all"},
                    {Button::triangle, "Search"},
                    {Button::cross, grid_failed_ ? "Try again" : "Details"},
                    {Button::circle, "Back"}};
        return {{Button::l2, "Letters", Button::r2},
                {Button::triangle, "Search"},
                {Button::cross, grid_failed_ ? "Try again" : "Details"},
                {Button::circle, "Back"}};
    case kSearch:
        return {{Button::square, "Delete"}, {Button::r2, "Show all"}, {Button::circle, "Back"}};
    }
    return {};
}

} // namespace rommps
