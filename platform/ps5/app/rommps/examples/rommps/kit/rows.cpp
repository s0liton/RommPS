// RommPS: a list of setting rows. See rows.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rows.hpp"

#include <algorithm>
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
    if (snap)
        position_.snap(static_cast<float>(focus_));
}

void RowList::refuse(hui::ui::Feedback &feedback)
{
    feedback.play(Cue::error, 1.0f, 0.0f, 0.6f);
    feedback.rumble(0.25f, 0.05f);
    nudge_.trigger();
}

RowEvent RowList::handle(const std::vector<Row> &rows, const hui::InputFrame &input, hui::ui::Feedback &feedback)
{
    RowEvent event;
    const int count = static_cast<int>(rows.size());
    if (count == 0)
        return event;
    focus_ = std::clamp(focus_, 0, count - 1);
    const Row &row = rows[static_cast<std::size_t>(focus_)];
    switch (input.nav)
    {
    case Direction::up:
    case Direction::down:
    {
        const int next = focus_ + (input.nav == Direction::down ? 1 : -1);
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
    // Keep the focused row a row away from the edges.
    const float top = static_cast<float>(focus_ - 1) * kRowHeight;
    const float bottom = static_cast<float>(focus_ + 2) * kRowHeight;
    const float most = std::max(0.0f, static_cast<float>(count) * kRowHeight - visible_height);
    if (top < scroll_.target)
        scroll_.target = top;
    if (bottom > scroll_.target + visible_height)
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
    if (!rows.empty())
        list.rounded_rect({bounds.x + shake, plate_y + 6, bounds.w, kRowHeight - 12}, 22, look.highlight);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i)
    {
        const Row &row = rows[static_cast<std::size_t>(i)];
        const float y = bounds.y + static_cast<float>(i) * kRowHeight - scroll_.value;
        if (y + kRowHeight < bounds.y || y > bounds.y + bounds.h)
            continue;
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
            const std::string value = fonts.semibold.font->fit(row.value, size, bounds.w * 0.45f);
            const float w = ui::text(list, fonts.semibold, value, right - chevrons, y + kRowHeight * 0.5f + 9, size,
                                     (row.kind == RowKind::info ? soft : ink).with_alpha(dim), hui::gfx::Align::right);
            if (row.kind == RowKind::choice && row.enabled)
            {
                const float cy = y + kRowHeight * 0.5f;
                const float lx = right - chevrons - w - 22, rx = right - 8;
                const Color c = focused ? ink : soft;
                list.line(lx + 6, cy - 9, lx, cy, 3.0f, c);
                list.line(lx, cy, lx + 6, cy + 9, 3.0f, c);
                list.line(rx - 6, cy - 9, rx, cy, 3.0f, c);
                list.line(rx, cy, rx - 6, cy + 9, 3.0f, c);
            }
            used = w + chevrons + (row.kind == RowKind::choice ? 50.0f : 24.0f);
        }
        const float room = std::max(200.0f, bounds.w - 64 - used);
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(row.label, 26, room), x, label_y, 26, ink.with_alpha(dim));
        if (two_lines)
            ui::text(list, fonts.regular, fonts.regular.font->fit(row.detail, 19, room), x, label_y + 30, 19,
                     soft.with_alpha(dim));
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
