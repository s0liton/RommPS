// RommPS: a list of setting rows. See rows.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rows.hpp"

#include "rommps_app.hpp"
#include "touch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace rommps
{

using hui::Action;
using hui::Direction;
using hui::audio::Cue;
using hui::gfx::Rect;

void RowList::set_focus(int row, bool snap)
{
    focus_ = std::max(0, row);
    free_ = false;
    if (snap)
        position_.snap(static_cast<float>(focus_));
}

void RowList::refuse(hui::ui::Feedback &feedback)
{
    feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
    feedback.rumble(0.25f, 0.05f);
    nudge_.trigger();
}

hui::InputFrame RowList::touch(const hui::InputFrame &input, const std::vector<Row> &rows)
{
    const touch::Touch t = touch::take(this);
    if (touch::scroll(scroll_, t, 1.0f, most_))
        free_ = true;
    const int count = static_cast<int>(rows.size());
    if (t.index < 0 || t.index >= count || rows[static_cast<std::size_t>(t.index)].kind == RowKind::header)
        return input;
    focus_ = t.index;
    free_ = false;
    if (t.part == 1)
    {
        // A choice's left arrow: the option before
        hui::InputFrame back = input;
        if (t.pressed)
        {
            back.nav = Direction::left;
            back.nav_repeat = false;
        }
        return back;
    }
    return touch::with(input, t);
}

RowEvent RowList::handle(const std::vector<Row> &rows, const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    RowEvent event;
    const int count = static_cast<int>(rows.size());
    if (count == 0)
        return event;
    // Scrolled by touch, the focus may be out of view: Up or Down picks it up
    // near the view's edge (the bottom's going up, the top's going down), as
    // far in as update() keeps it, so the list stays where it was put.
    if (free_ && (input.nav == Direction::up || input.nav == Direction::down))
    {
        free_ = false;
        const float top = scroll_.value, bottom = scroll_.value + visible_;
        int at = input.nav == Direction::down ? static_cast<int>(std::ceil(top / kRowHeight)) + 1
                                              : static_cast<int>(std::floor(bottom / kRowHeight)) - 2;
        at = std::clamp(at, 0, count - 1);
        const int step = input.nav == Direction::down ? 1 : -1;
        while (at >= 0 && at < count && rows[static_cast<std::size_t>(at)].kind == RowKind::header)
            at += step;
        if (at >= 0 && at < count && at != focus_)
        {
            focus_ = at;
            feedback.play(Cue::focus);
            return event;
        }
    }
    focus_ = std::clamp(focus_, 0, count - 1);
    // A section's name can't hold the focus: the first row after it does
    // (or, at the end of the list, the last one before it).
    if (rows[static_cast<std::size_t>(focus_)].kind == RowKind::header)
    {
        int at = focus_;
        while (at < count && rows[static_cast<std::size_t>(at)].kind == RowKind::header)
            ++at;
        if (at == count)
            for (at = focus_; at > 0 && rows[static_cast<std::size_t>(at)].kind == RowKind::header;)
                --at;
        focus_ = at;
    }
    const Row &row = rows[static_cast<std::size_t>(focus_)];
    switch (input.nav)
    {
    case Direction::up:
    case Direction::down:
    {
        const int step = input.nav == Direction::down ? 1 : -1;
        int next = focus_ + step;
        while (next >= 0 && next < count && rows[static_cast<std::size_t>(next)].kind == RowKind::header)
            next += step;
        if (next >= 0 && next < count)
        {
            focus_ = next;
            feedback.play(Cue::focus);
        }
        else if (!input.nav_repeat)
            refuse(feedback);
        break;
    }
    case Direction::left:
    case Direction::right:
        if (row.kind == RowKind::choice && row.enabled)
        {
            event.row = focus_;
            event.step = input.nav == Direction::right ? 1 : -1;
            feedback.play(Cue::slider, input.nav == Direction::right ? 1.06f : 0.94f);
        }
        else if (!input.nav_repeat)
            refuse(feedback);
        break;
    case Direction::none:
        break;
    }
    if (input.is_pressed(Action::confirm))
    {
        if (!row.enabled || row.kind == RowKind::info)
            refuse(feedback);
        else if (row.kind == RowKind::choice)
        {
            event.row = focus_;
            event.step = 1;
            feedback.play(Cue::slider, 1.06f);
        }
        else
        {
            event.row = focus_;
            event.activated = true;
            feedback.play(row.kind == RowKind::toggle ? Cue::toggle : Cue::select);
        }
    }
    return event;
}

void RowList::update(float dt, int count, float visible_height)
{
    focus_ = std::clamp(focus_, 0, std::max(0, count - 1));
    position_.target = static_cast<float>(focus_);
    position_.update(dt, 22.0f);
    // Keep the focused row a row away from the edges (unless touch scrolled away).
    const float top = static_cast<float>(focus_ - 1) * kRowHeight;
    const float bottom = static_cast<float>(focus_ + 2) * kRowHeight;
    const float most = std::max(0.0f, static_cast<float>(count) * kRowHeight - visible_height);
    most_ = most;
    visible_ = visible_height;
    if (!free_ && top < scroll_.target)
        scroll_.target = top;
    if (!free_ && bottom > scroll_.target + visible_height)
        scroll_.target = bottom - visible_height;
    scroll_.target = std::clamp(scroll_.target, 0.0f, most);
    scroll_.update(dt, 14.0f);
    nudge_.update(dt, 9.0f);
}

void RowList::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const std::vector<Row> &rows,
                   const Rect &bounds, float clock) const
{
    namespace ui = hui::ui;
    list.push_clip({bounds.x - 40, bounds.y, bounds.w + 80, bounds.h});
    const float shake = ui::shake(nudge_.value, clock, 10.0f, 9.0f);
    const float plate_y = bounds.y + position_.value * kRowHeight - scroll_.value;
    touch::zone(list, bounds, this, -1); // the list's ground: drags scroll it
    if (!rows.empty())
        list.rounded_rect({bounds.x + shake, plate_y + 6, bounds.w, kRowHeight - 12}, 22, look.highlight);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const Row &row = rows[static_cast<std::size_t>(i)];
        const float y = bounds.y + static_cast<float>(i) * kRowHeight - scroll_.value;
        if (y + kRowHeight < bounds.y || y > bounds.y + bounds.h)
            continue;
        if (row.kind == RowKind::header)
        {
            std::string caps = plain_text(row.label.c_str());
            for (char &ch : caps)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            ui::text(list, fonts.semibold, caps, bounds.x + 32, y + kRowHeight - 20, 18, look.muted);
            continue;
        }
        touch::zone(list, {bounds.x, y, bounds.w, kRowHeight}, this, i);
        const bool focused = i == focus_;
        const float dim = row.enabled ? 1.0f : 0.45f;
        const Color ink = focused ? look.on_highlight : row.danger ? look.danger : look.text;
        const Color soft = focused ? look.on_highlight.with_alpha(0.7f) : look.muted;
        const float x = bounds.x + 32 + (focused ? shake : 0.0f);
        const float right = bounds.x + bounds.w - 32 + (focused ? shake : 0.0f);
        const bool two_lines = !row.detail.empty();
        const float label_y = y + (two_lines ? 42.0f : 56.0f);
        // The right-hand part first, so the label knows how much room it has.
        float used = 0.0f;
        if (row.kind == RowKind::toggle)
        {
            const Rect track{right - 68, y + kRowHeight * 0.5f - 18, 68, 36};
            list.rounded_rect(track, 18, row.on ? (focused ? look.on_highlight : look.accent) : soft.with_alpha(0.3f * dim));
            const float knob = row.on ? track.x + track.w - 18 : track.x + 18;
            list.circle(knob, track.cy(), 13, row.on ? (focused ? look.highlight : look.page) : ink.with_alpha(dim));
            used = 90.0f;
        }
        else if (!row.value.empty())
        {
            const float size = 24.0f;
            const float chevrons = row.kind == RowKind::choice ? 30.0f : 0.0f;
            const std::string value = fonts.semibold.font->fit(plain_text(row.value.c_str()), size, bounds.w * 0.45f);
            const float w = ui::text(list, fonts.semibold, value, right - chevrons, y + kRowHeight * 0.5f + 9, size,
                                     (row.kind == RowKind::info ? soft : ink).with_alpha(dim), hui::gfx::Align::right);
            if (row.kind == RowKind::choice && row.enabled)
            {
                const float cy = y + kRowHeight * 0.5f;
                const float lx = right - chevrons - w - 22, rx = right - 8;
                touch::zone(list, {lx - 40, y, 70, kRowHeight}, this, i, touch::kList, 1); // the left arrow
                const Color c = focused ? ink : soft;
                list.line(lx + 6, cy - 9, lx, cy, 3.0f, c);
                list.line(lx, cy, lx + 6, cy + 9, 3.0f, c);
                list.line(rx - 6, cy - 9, rx, cy, 3.0f, c);
                list.line(rx, cy, rx - 6, cy + 9, 3.0f, c);
            }
            used = w + chevrons + (row.kind == RowKind::choice ? 50.0f : 24.0f);
        }
        // A chevron for a row that opens: right when closed, down when open.
        const float indent = row.expander ? 30.0f : 0.0f;
        if (row.expander)
        {
            const float cx = x + 8, cy = label_y - 9;
            const Color c = ink.with_alpha(dim);
            if (row.expander == 1)
            {
                list.line(cx - 3, cy - 8, cx + 4, cy, 3.0f, c);
                list.line(cx + 4, cy, cx - 3, cy + 8, 3.0f, c);
            }
            else
            {
                list.line(cx - 8, cy - 3, cx, cy + 4, 3.0f, c);
                list.line(cx, cy + 4, cx + 8, cy - 3, 3.0f, c);
            }
        }
        // The platform's logo, then the label.
        float lx = x + indent;
        if (row.logo_slot)
        {
            constexpr float kLogo = 34.0f;
            if (row.logo)
            {
                const float w = std::min(kLogo * 1.6f, kLogo * row.logo_aspect), h = w / row.logo_aspect;
                list.image(row.logo, {lx + (kLogo - std::min(w, kLogo)) * 0.5f, label_y - 9 - h * 0.5f, w, h},
                           hui::gfx::kFullUv, hui::gfx::Color::rgb(0xFFFFFF, dim));
            }
            lx += kLogo * 1.6f + 8;
        }
        const float room = std::max(200.0f, bounds.w - 64 - used - (lx - x));
        // Rows carry names, paths and the service's errors as they came
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(plain_text(row.label.c_str()), 26, room), lx, label_y, 26,
                 ink.with_alpha(dim));
        if (two_lines)
            ui::text(list, fonts.regular, fonts.regular.font->fit(plain_text(row.detail.c_str()), 19, room), lx, label_y + 30, 19,
                     soft.with_alpha(dim));
        if (row.progress >= 0.0f)
        {
            const Rect bar{x, y + kRowHeight - 14, right - x, 4};
            list.rounded_rect(bar, 2, soft.with_alpha(0.25f));
            list.rounded_rect({bar.x, bar.y, std::max(4.0f, bar.w * std::min(1.0f, row.progress)), bar.h}, 2,
                              focused ? look.on_highlight : look.accent);
        }
    }
    list.pop_clip();
}

const char *label_of(std::span<const Named> options, const std::string &id)
{
    for (const Named &o : options)
        if (id == o.id)
            return o.label;
    return options.empty() ? "" : options.front().label;
}

const char *step_option(std::span<const Named> options, const std::string &id, int step)
{
    const int n = static_cast<int>(options.size());
    if (n == 0)
        return "";
    int at = 0;
    for (int i = 0; i < n; ++i)
        if (id == options[static_cast<std::size_t>(i)].id)
            at = i;
    return options[static_cast<std::size_t>(((at + step) % n + n) % n)].id;
}

std::string interval_label(int minutes)
{
    char text[32];
    if (minutes <= 0)
        return "Off";
    if (minutes < 60)
        std::snprintf(text, sizeof text, "%d min", minutes);
    else if (minutes % 60 == 0)
        std::snprintf(text, sizeof text, "%d h", minutes / 60);
    else
        std::snprintf(text, sizeof text, "%d h %d min", minutes / 60, minutes % 60);
    return text;
}

int step_interval(int minutes, int step)
{
    constexpr int n = static_cast<int>(std::size(kIntervals));
    int at = 0; // the nearest option at or below the current value
    for (int i = 0; i < n; ++i)
        if (kIntervals[i] <= minutes)
            at = i;
    return kIntervals[((at + step) % n + n) % n];
}

} // namespace rommps
