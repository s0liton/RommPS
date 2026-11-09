// RommPS: the Steam Deck's touch screen (RommDeck), told in the pad's terms.
// Whatever a finger can use registers a zone while it's drawn; the next frame
// its owner takes what the finger did there, moves its own focus, and reads
// Cross from the input as it always has. Owners are addresses: the list, or a
// member standing for one.
//
//   - A button zone is a pad button: Cross goes down with the finger and up
//     with it, so holds (hold to cancel, hold to delete) are holds.
//   - In a list, a tap focuses the row and presses Cross; a finger resting a
//     moment holds Cross; a drag scrolls the list, and a flung one coasts.
//     Index -1 is the list's own ground: it scrolls, a tap does nothing.
//   - A scrub zone (the A to Z bar) hands its owner the zone under the finger
//     each time that changes.
//   - The hint row's buttons press the pad button they show, for everyone.
//
// The PS5 has no touch screen: there, every call is empty and inlined away.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "core/tween.hpp"
#include "gfx/draw_list.hpp"
#include "ui/components/keyboard.hpp"
#include "ui/fonts.hpp"
#include "ui/glyphs.hpp"

#include <algorithm>

namespace rommps::touch
{

enum Kind : unsigned
{
    kButton,
    kList,
    kScrub,
};

struct Touch
{
    int index = -1;                                       // the zone to focus; -1 for none
    int part = 0;                                         // which part of it (a choice row's left arrow is 1)
    bool pressed = false, held = false, released = false; // Cross, as the finger has it
    float scroll = 0.0f; // canvas pixels further down the list (a drag, a fling's coast, the wheel)
    bool direct = false; // the scroll is a finger on the glass: follow it, don't ease
};

// The input with the touch's Cross (or another action) folded in.
inline hui::InputFrame with(hui::InputFrame input, const Touch &t, hui::Action action = hui::Action::confirm)
{
    const std::uint32_t bit = hui::action_bit(action);
    if (t.pressed)
        input.pressed |= bit;
    if (t.held)
        input.held |= bit;
    else if (t.released)
        input.held &= ~bit;
    if (t.released)
        input.released |= bit;
    return input;
}

// A scroll offset moved by t, in units of `unit` canvas pixels, kept within
// 0..most. Whether it moved.
inline bool scroll(hui::tween::Spring &s, const Touch &t, float unit, float most)
{
    if (t.scroll == 0.0f)
        return false;
    s.target = std::clamp(s.target + t.scroll / unit, 0.0f, std::max(0.0f, most));
    if (t.direct)
        s.snap(s.target);
    return true;
}

#if defined(ROMMPS_DECK)

void zone(const hui::gfx::DrawList &list, const hui::gfx::Rect &r, const void *owner, int index, Kind kind = kList,
          int part = 0);
// The hint row ui::draw_hints draws (right-aligned at x when right_align):
// each hint presses its button.
void hints(const hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const hui::ui::Hint *hints, int count, float x,
           bool right_align, const hui::ui::HintLayout &layout = {});
// The kit keyboard's keys, each a button (index: row * 256 + key).
void keys(const hui::gfx::DrawList &list, const hui::ui::Keyboard &keyboard);
// What the finger did on owner's zones this frame (once: a second take is empty).
Touch take(const void *owner);
// Whether there's something for owner, without taking it.
bool pending(const void *owner);

#else

inline void zone(const hui::gfx::DrawList &, const hui::gfx::Rect &, const void *, int, Kind = kList, int = 0)
{
}
inline void hints(const hui::gfx::DrawList &, const hui::ui::Fonts &, const hui::ui::Hint *, int, float, bool,
                  const hui::ui::HintLayout & = {})
{
}
inline void keys(const hui::gfx::DrawList &, const hui::ui::Keyboard &)
{
}
inline Touch take(const void *)
{
    return {};
}
inline bool pending(const void *)
{
    return false;
}

#endif

// A key touched (see keys()): the keyboard's focus on it, and the input with
// its Cross, so handle() types it; held, backspace repeats as on the pad.
inline hui::InputFrame type(hui::ui::Keyboard &keyboard, const hui::InputFrame &input)
{
    const Touch t = take(&keyboard);
    if (t.index < 0)
        return input;
    keyboard.set_focus(t.index / 256, t.index % 256);
    return with(input, t);
}

} // namespace rommps::touch
