// RommDeck's touch screen: zones, and what a finger does on them. See
// touch.hpp for what the screens see, deck_touch.hpp for what the host feeds.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck_touch.hpp"

#include <cmath>
#include <vector>

namespace rommps::touch
{

namespace
{

using hui::gfx::Rect;

// Canvas pixels (1920 across the Deck's 1280): how far a finger goes before
// it's a drag, not a tap; how long it rests before it's a hold.
constexpr float kSlop = 28.0f;
constexpr float kHoldAfter = 0.4f;
// A button lets go when the finger slides this far off it.
constexpr float kButtonMargin = 48.0f;
// Flings: the speed (canvas pixels a second) that coasts, how fast it fades,
// and where it stops.
constexpr float kFlingFrom = 400.0f, kFriction = 3.2f, kFlingTo = 40.0f;
constexpr float kWheelStep = 110.0f; // canvas pixels a notch

struct Zone
{
    Rect r;
    const void *owner = nullptr;
    int index = -1, part = 0;
    Kind kind = kList;
};

// The hint row's buttons: one owner for all, the action as the index.
const char kHintOwner = 0;

enum class Phase
{
    idle,
    button, // a button held down
    scrub,
    waiting, // in a list: a tap, a hold or a drag, not known yet
    holding,
    dragging,
    coasting, // a fling after the finger left
};

// The last frame's, which the finger sees; the screen draws them anew each frame.
std::vector<Zone> zones;
Phase phase = Phase::idle;
Zone grabbed;
float x0 = 0, y0 = 0, x = 0, y = 0, last_y = 0;
float age = 0, velocity = 0, moved = 0; // moved: drag pixels since the last frame
bool finger = false;
Touch out; // this frame's, being gathered
Touch now; // this frame's, for the takers
const void *now_owner = nullptr;
bool taken = false;

bool inside(const Rect &r, float px, float py, float margin = 0.0f)
{
    return px >= r.x - margin && px < r.x + r.w + margin && py >= r.y - margin && py < r.y + r.h + margin;
}

const Zone *hit(float px, float py)
{
    for (auto it = zones.rbegin(); it != zones.rend(); ++it)
        if (inside(it->r, px, py))
            return &*it;
    return nullptr;
}

// The owner's zone level with py: the A to Z bar follows a finger that strays sideways.
const Zone *scrub_at(float py)
{
    const Zone *best = nullptr;
    for (const Zone &z : zones)
        if (z.owner == grabbed.owner && py >= z.r.y && py < z.r.y + z.r.h)
            best = &z;
    return best;
}

void aim(const Zone &z)
{
    out.index = z.index;
    out.part = z.part;
}

hui::Action action_of(hui::ui::Button b)
{
    using hui::Action;
    using hui::ui::Button;
    switch (b)
    {
    case Button::cross: return Action::confirm;
    case Button::circle: return Action::back;
    case Button::square: return Action::west;
    case Button::triangle: return Action::north;
    case Button::l1: return Action::page_prev;
    case Button::r1: return Action::page_next;
    case Button::l2: return Action::jump_prev;
    case Button::r2: return Action::jump_next;
    case Button::options: return Action::menu;
    case Button::touchpad: return Action::touch;
    default: return Action::count; // the D-pad and the sticks: nothing to press
    }
}

} // namespace

void zone(const hui::gfx::DrawList &list, const Rect &r, const void *owner, int index, Kind kind, int part)
{
    const Rect on = list.on_canvas(r);
    if (on.w > 0.0f && on.h > 0.0f)
        zones.push_back({on, owner, index, part, kind});
}

void hints(const hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const hui::ui::Hint *hints, int count, float at,
           bool right_align, const hui::ui::HintLayout &layout)
{
    float cursor = right_align ? at - hui::ui::measure_hints(fonts, hints, count, layout) : at;
    const float half_gap = layout.item_gap * 0.5f;
    for (int i = 0; i < count; ++i)
    {
        const hui::ui::Hint &h = hints[i];
        const float w = hui::ui::measure_hints(fonts, &h, 1, layout);
        const Rect r{cursor - half_gap, layout.cy - 38.0f, w + layout.item_gap, 76.0f};
        const hui::Action first = action_of(h.button), second = action_of(h.second);
        // A pair (L1 R1 Pages): the left half is the first button, the right the second.
        if (second != hui::Action::count && first != hui::Action::count)
        {
            zone(list, {r.x, r.y, r.w * 0.5f, r.h}, &kHintOwner, static_cast<int>(first), kButton);
            zone(list, {r.x + r.w * 0.5f, r.y, r.w * 0.5f, r.h}, &kHintOwner, static_cast<int>(second), kButton);
        }
        else if (first != hui::Action::count)
            zone(list, r, &kHintOwner, static_cast<int>(first), kButton);
        cursor += w + layout.item_gap;
    }
}

void keys(const hui::gfx::DrawList &list, const hui::ui::Keyboard &keyboard)
{
    if (keyboard.layouts().empty())
        return;
    const auto &rows = keyboard.layouts()[static_cast<std::size_t>(keyboard.layout())].rows;
    const float half_gap = keyboard.style.gap * 0.5f; // the gaps between keys belong to them
    for (int r = 0; r < static_cast<int>(rows.size()); ++r)
        for (int k = 0; k < static_cast<int>(rows[static_cast<std::size_t>(r)].size()); ++k)
            zone(list, keyboard.key_rect(r, k).inset(-half_gap), &keyboard, r * 256 + k, kButton);
}

Touch take(const void *owner)
{
    if (taken || owner != now_owner)
        return {};
    taken = true;
    return now;
}

bool pending(const void *owner)
{
    return !taken && owner == now_owner && now_owner != nullptr;
}

// ------------------------------------------------------------------ the host's

void down(float px, float py)
{
    if (finger)
        return;
    finger = true;
    x0 = x = px;
    y0 = y = last_y = py;
    age = 0;
    velocity = moved = 0;
    const Zone *z = hit(px, py);
    if (!z)
    {
        phase = Phase::idle; // nothing there: the finger does nothing until it lifts
        grabbed = {};
        return;
    }
    grabbed = *z;
    switch (z->kind)
    {
    case kButton:
        phase = Phase::button;
        aim(*z);
        out.pressed = out.held = true;
        break;
    case kScrub:
        phase = Phase::scrub;
        aim(*z);
        out.pressed = out.held = true;
        break;
    case kList: phase = Phase::waiting; break;
    }
}

void move(float px, float py)
{
    if (!finger)
        return;
    x = px;
    y = py;
    switch (phase)
    {
    case Phase::button:
        if (!inside(grabbed.r, px, py, kButtonMargin))
        {
            out.released = true; // slid off: the button comes up
            out.held = false;
            phase = Phase::idle;
        }
        break;
    case Phase::scrub:
        if (const Zone *z = scrub_at(py); z && z->index != grabbed.index)
        {
            grabbed.index = z->index;
            aim(*z);
        }
        break;
    case Phase::waiting:
        if (std::hypot(px - x0, py - y0) > kSlop)
        {
            phase = Phase::dragging;
            last_y = py;
        }
        break;
    case Phase::holding:
        if (std::hypot(px - x0, py - y0) > 2.0f * kSlop)
        {
            out.released = true; // a hold that wanders off isn't one
            out.held = false;
            phase = Phase::idle;
        }
        break;
    case Phase::dragging:
        out.scroll += last_y - py;
        moved += last_y - py;
        out.direct = true;
        last_y = py;
        break;
    default: break;
    }
}

void up()
{
    if (!finger)
        return;
    finger = false;
    switch (phase)
    {
    case Phase::button:
    case Phase::scrub:
    case Phase::holding:
        out.released = true;
        out.held = false;
        phase = Phase::idle;
        break;
    case Phase::waiting:
        // A tap: the row is focused and Cross goes down and up. On a list's
        // own ground, nothing.
        if (grabbed.index >= 0)
        {
            aim(grabbed);
            out.pressed = out.released = true;
        }
        phase = Phase::idle;
        break;
    case Phase::dragging: phase = std::fabs(velocity) >= kFlingFrom ? Phase::coasting : Phase::idle; break;
    default: phase = Phase::idle; break;
    }
}

void cancel()
{
    if (!finger)
        return;
    finger = false;
    if (phase == Phase::button || phase == Phase::scrub || phase == Phase::holding)
    {
        out.released = true;
        out.held = false;
    }
    phase = Phase::idle;
}

void wheel(float px, float py, float notches)
{
    if (finger)
        return;
    const Zone *z = hit(px, py);
    if (!z || z->kind != kList)
        return;
    // Another list than the one coasting: that one stops.
    if (phase == Phase::coasting && z->owner != grabbed.owner)
        phase = Phase::idle;
    if (phase != Phase::coasting)
        grabbed = *z;
    out.scroll -= notches * kWheelStep;
    out.direct = false;
}

void frame(float dt)
{
    // A coast its owner stopped taking (the screen moved on) ends.
    if (phase == Phase::coasting && !taken && now_owner == grabbed.owner)
        phase = Phase::idle;
    if (finger)
        age += dt;
    switch (phase)
    {
    case Phase::waiting:
        // Rested on a row: Cross goes down on it, and stays down with the
        // finger. On the list's ground it can still become a drag.
        if (age >= kHoldAfter && grabbed.index >= 0)
        {
            phase = Phase::holding;
            aim(grabbed);
            out.pressed = out.held = true;
        }
        break;
    case Phase::button:
    case Phase::holding:
        aim(grabbed);
        out.held = true;
        break;
    case Phase::scrub:
        out.held = true; // the zone only when it changes
        break;
    case Phase::dragging:
        if (dt > 0.0f) // the finger's speed, smoothed over a few frames
            velocity += (moved / dt - velocity) * 0.5f;
        moved = 0;
        break;
    case Phase::coasting:
        out.scroll += velocity * dt;
        out.direct = false;
        velocity *= std::exp(-kFriction * dt);
        if (std::fabs(velocity) < kFlingTo)
            phase = Phase::idle;
        break;
    default: break;
    }
    const bool any = out.index >= 0 || out.pressed || out.held || out.released || out.scroll != 0.0f;
    now = out;
    now_owner = any ? grabbed.owner : nullptr;
    taken = false;
    out = {};
}

hui::InputFrame buttons(const hui::InputFrame &input)
{
    if (now_owner != &kHintOwner || now.index < 0 || now.index >= static_cast<int>(hui::Action::count))
        return input;
    const Touch t = take(&kHintOwner);
    return with(input, t, static_cast<hui::Action>(t.index));
}

void clear_zones()
{
    zones.clear();
}

bool busy()
{
    return finger || phase == Phase::coasting;
}

} // namespace rommps::touch
