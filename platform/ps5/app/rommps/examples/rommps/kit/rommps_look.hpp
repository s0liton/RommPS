// RommPS: the screen's look. "Cover colours" (the backdrop and accents follow
// the focused game's cover), or one of the UI kit's themes. Every colour the
// screen draws with comes from here, so a light theme reads as well as a dark
// one. The choice is kept in the title's folder (/app0/hui/rommps-look.txt).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/glyphs.hpp"
#include "ui/theme.hpp"

namespace rommps
{

using hui::gfx::Color;

struct Look
{
    const char *id = "";
    const char *name = "";
    const char *family = "";
    bool covers = false;         // the backdrop and accent follow the focused cover
    const hui::ui::Theme *theme; // what the kit's components (the keyboard) are drawn in
    hui::gfx::BackdropSpec backdrop;
    Color page;       // the page's own colour, for fades over it
    Color text;       // text straight on the page
    Color muted;      // secondary text on the page
    Color panel;      // panels and cards
    Color panel_text; // text on a panel
    Color panel_muted;
    Color outline;   // panel edges
    Color accent;    // highlights, selections (Cover colours: the cover's)
    Color button;    // the main action's body
    Color on_button; // ... and its text
    Color focus;     // the focus ring
    Color highlight; // a focused row's plate
    Color on_highlight;
    Color danger, success, warning;
    float radius = 28.0f; // panels
    bool dark = true;

    hui::ui::GlyphStyle glyphs() const
    {
        return dark ? hui::ui::GlyphStyle::dark() : hui::ui::GlyphStyle::light();
    }
};

int look_count();
const Look &look(int index);
// The look in use: the saved one, Cover colours until one is picked.
int current_look();
// Uses the look from now on and saves the choice (silently, a test run excepted).
void set_look(int index);

// A panel in the look: its shadow, body and edge.
void draw_panel(hui::gfx::DrawList &list, const Look &look, const hui::gfx::Rect &r, float radius = -1.0f);

} // namespace rommps
