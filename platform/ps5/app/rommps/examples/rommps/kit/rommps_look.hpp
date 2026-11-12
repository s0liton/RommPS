// RommPS: the screen's look. "Cover colours" (the backdrop and accents follow
// the focused game's cover), or one of the UI kit's themes. Every colour the
// screen draws with comes from here, so a light theme reads as well as a dark
// one. The choice is kept in the title's folder (/app0/hui/rommps-look.txt).
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "gfx/backdrop_spec.hpp"
#include "gfx/draw_list.hpp"
#include "ui/feedback.hpp"
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

// The interface's sounds (focus, select, back...), picked in Settings and kept
// beside the theme. Rumble isn't affected.
enum class Sounds
{
    metal_gear, // RommPS's own menu sounds; the kit's recordings fill the cues they lack
    paper,      // one of the kit's recorded sets
    glass,
    tones, // the kit's plain tones, one for every cue
    off,
    count
};
// As Settings shows it.
const char *sounds_name(Sounds sounds);
// The saved choice: Metal Gear Solid until another is picked.
Sounds ui_sounds();
// Plays from now on and saves the choice (silently, a test run excepted).
void set_ui_sounds(Sounds sounds);

// A panel in the look: its shadow, body and edge.
void draw_panel(hui::gfx::DrawList &list, const Look &look, const hui::gfx::Rect &r, float radius = -1.0f);

} // namespace rommps

// The frame's cues in the interface sounds picked, from the program's banks:
// menu has the menu sounds with the kit's recordings in their gaps, kit the
// kit's sets as recorded; design is the screen's own set. The cues are taken,
// so the kit's own play is left the rumble. (C linkage: rommps.cpp is compiled
// in a namespace of its own.)
extern "C" void rommps_play_sounds(hui::ui::Feedback &feedback, hui::audio::Mixer &mixer, hui::audio::SoundBank &menu,
                                   hui::audio::SoundBank &kit, hui::audio::SoundSet design);
