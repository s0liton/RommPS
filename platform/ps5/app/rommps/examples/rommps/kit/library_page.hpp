// RommPS: the Library tab.
//
//   - Platforms: "On this console" (the games here, most recently played
//     first) above a grid of RomM's platforms, each with a few of its covers.
//     Cross opens one, Triangle searches every platform.
//   - A platform (or a search's results): the focused game's details above a
//     grid of covers that loads as it scrolls, and an A to Z bar on the right
//     (Right from the last column to reach it, L2 / R2 to jump a letter).
//     Triangle searches the platform, Circle goes back.
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

// A game's cover, or a card with its name while there's none.
void draw_cover(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, const Game *g,
                const hui::gfx::Rect &rect, float alpha, float radius, bool name = true);
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

    // ---- platforms
    void update_platforms(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details);
    void draw_platforms(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                        float clock, float age) const;
    float platforms_top(App &app) const;
    // ---- a grid of games
    void open_grid(App &app, int platform_id, const std::string &search);
    void update_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details);
    void move_in_grid(App &app, const hui::InputFrame &input, hui::ui::Feedback &feedback, bool &details);
    void draw_grid(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                   float clock, float age) const;
    GameList &grid(App &app) const { return app.games(grid_platform_, grid_search_); }
    int letter_at(const GameList &l, int index) const; // index into l.letters, -1 for none
    void jump_letter(GameList &l, int letter, hui::ui::Feedback &feedback);
    // ---- search
    void open_search(int platform_id);
    void update_search(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_search(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, App &app, Color accent,
                     float clock) const;

    void refuse(hui::ui::Feedback &feedback);

    Mode mode_ = kPlatforms;
    Mode before_search_ = kPlatforms;
    float mode_age_ = 0.0f;
    hui::ui::Pulse nudge_;

    // platforms: section 0 is "On this console", 1 the platform grid
    int section_ = 1;
    int recent_ = 0, tile_ = 0;
    hui::ui::Scroller recent_scroll_;
    hui::tween::Spring tiles_scroll_; // in rows

    // a grid
    int grid_platform_ = 0;
    std::string grid_search_;
    int index_ = 0;
    // The focused game by RomM's offset, so it stays put when a page turns out
    // to hold files that aren't games, which shifts the shown positions.
    const GameList *focus_list_ = nullptr;
    int focus_raw_ = 0;
    std::size_t focus_hidden_ = 0;
    bool on_letters_ = false;
    int letter_ = 0; // the A to Z bar's focus, an index into the list's letters
    hui::tween::Spring grid_scroll_; // in rows
    hui::tween::Spring focus_x_, focus_y_;
    int shown_id_ = -1;
    float shown_age_ = 0.0f;

    // search
    int search_platform_ = 0;
    std::string query_, applied_;
    float typed_age_ = 0.0f;
    hui::ui::Keyboard keys_;
};

} // namespace rommps
