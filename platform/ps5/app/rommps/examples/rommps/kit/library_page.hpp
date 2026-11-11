// RommPS: the Library tab.
//
//   - Platforms: a grid of RomM's platforms, each with a few of its covers.
//     Square shows RomM's collections there instead (and back), in sections
//     by kind and type (L2 / R2 skip between them). Cross opens one,
//     Triangle searches every platform.
//   - A platform or collection (or a search's results): the focused game's
//     details above a grid of covers that loads as it scrolls, and an A to Z
//     bar on the right (Right from the last column to reach it, L2 / R2 to
//     jump a letter). Triangle searches the platform or collection, Circle
//     goes back. In a collection, Square twice downloads all of it.
//   - Search: the kit's keyboard, the first results updating as you type;
//     R2 (or Done) shows them all.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "core/tween.hpp"
#include "rommps_app.hpp"
#include "rommps_look.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"
#include "ui/motion.hpp"

#include <string>
#include <vector>

namespace rommps
{

// Covers keep their own shape (width over height) within these: narrower or
// wider ones are cropped, never stretched.
constexpr float kPortrait = 0.75f, kNarrowest = 0.6f, kWidest = 1.45f;
// A game's cover shape: fallback until its cover has been loaded, then its
// own, eased in over a moment so what's around it slides rather than jumps.
float cover_shape(App &app, const Game *g, float fallback = kPortrait);
// The largest rectangle of a shape that fits in box, centred in it.
hui::gfx::Rect fit_cover(float shape, const hui::gfx::Rect &box);
// A game's cover, or a card with its name while there's none.
void draw_cover(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, const Game *g,
                const hui::gfx::Rect &rect, float alpha, float radius, bool name = true);
// A platform's logo from RomM, as large as fits box; nothing while it loads or
// when RomM has none. Whether one was drawn.
bool draw_logo(hui::gfx::DrawList &list, App &app, const std::string &slug, const hui::gfx::Rect &box, float alpha);
// "Downloaded", or a running download's progress, on a card.
void draw_badge(hui::gfx::DrawList &list, const Look &look, App &app, const Game *g, const hui::gfx::Rect &rect,
                Color accent);
// When RomM has more than one version of the game, what sets this one apart
// ("USA, Rev 1"), in a tag along the cover's bottom edge.
void draw_version(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const Game *g,
                  const hui::gfx::Rect &rect);
std::string human_size(double bytes);
// "1994 · Nintendo · Platform, Adventure" ("" while there's nothing to say).
std::string details_line(const Details &d);
// "1 h 30 min" of play time.
std::string play_time(double ms);

class LibraryPage
{
  public:
    LibraryPage();
    LibraryPage(const LibraryPage &) = delete;
    LibraryPage &operator=(const LibraryPage &) = delete;

    void enter(App &app);
    // True when Cross asks for the focused game's details.
    bool update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
              float clock, float age) const;
    // The game the focus is on (nullptr on a platform tile or a placeholder).
    const Game *focused(App &app) const;
    // A cover to colour the backdrop with ("" for none).
    std::string backdrop_cover(App &app) const;
    std::vector<hui::ui::Hint> hints() const;

  private:
    enum Mode
    {
        kPlatforms,
        kGrid,
        kSearch
    };
    // What the tiles below "On this console" are.
    enum Browse
    {
        kByPlatform,
        kByCollection
    };
    // A tile: a platform or a collection, whichever is browsed.
    struct Tile
    {
        Scope scope;
        std::string name;
        std::string slug; // the platform's, for its logo ("" for a collection)
        std::string note; // the emulator; a collection's owner (someone else's) or description
        bool note_on = false; // the note in the accent colour (a platform an emulator plays)
        int count = 0;
        std::string section; // a collection's kind or type ("Smart collections", "Genres"); "" for platforms
    };
    std::vector<Tile> tiles(App &app) const;
    // Where the tiles go: rows of kTileCols, each section starting a row of
    // its own under its heading.
    struct Placement
    {
        std::vector<int> row, col; // each tile's
        std::vector<std::pair<int, std::string>> heads; // the row a section starts on, and its heading
        float top(int r) const; // a row's offset from the first's, headings included
        int first_of(int r) const; // the first tile on a row, -1 past the last
        int last_of(int r) const;
    };
    static Placement place(const std::vector<Tile> &all);
    // What a list is of: a platform's or collection's name ("" for every platform).
    static std::string scope_name(App &app, const Scope &scope);

    // ---- platforms
    void update_platforms(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback);
    void draw_platforms(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                        float clock, float age) const;
    // ---- a grid of games
    void open_grid(App &app, const Scope &scope, const std::string &search);
    void update_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details);
    // Picks the grid's shape from its first covers, until it settles.
    void settle_shape(App &app, GameList &l) const;
    void move_in_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details);
    void draw_grid(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                   float clock, float age) const;
    GameList &grid(App &app) const { return app.games(grid_scope_, grid_search_); }
    int letter_at(const GameList &l, int index) const; // index into l.letters, -1 for none
    void jump_letter(GameList &l, int letter, hui::ui::Feedback &feedback);
    // ---- search
    void open_search(const Scope &scope);
    void update_search(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_search(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                     float clock) const;

    void refuse(hui::ui::Feedback &feedback);

    Mode mode_ = kPlatforms;
    Mode before_search_ = kPlatforms;
    float mode_age_ = 0.0f;
    hui::ui::Pulse nudge_;

    // platforms: the tiles, of platforms or collections
    Browse browse_ = kByPlatform;
    int tile_ = 0;
    hui::tween::Spring tiles_scroll_; // in pixels

    // a grid
    Scope grid_scope_;
    std::string grid_search_;
    int index_ = 0;
    // The focused game by RomM's offset, so it stays put when a page turns out
    // to hold files that aren't games, which shifts the shown positions.
    const GameList *focus_list_ = nullptr;
    int focus_raw_ = 0;
    std::size_t focus_hidden_ = 0;
    bool on_letters_ = false;
    bool grid_failed_ = false; // RomM failed on the focused game's page: Cross asks again
    float install_armed_ = 0.0f; // seconds a second Square installs the whole collection
    int letter_ = 0; // the A to Z bar's focus, an index into the list's letters
    hui::tween::Spring grid_scroll_; // in rows
    hui::tween::Spring focus_x_, focus_y_;
    int shown_id_ = -1;
    float shown_age_ = 0.0f;

    // search
    Scope search_scope_;
    std::string query_, applied_;
    float typed_age_ = 0.0f;
    hui::ui::Keyboard keys_;
};

} // namespace rommps
