// RommPS: a list of setting rows for the pad: switches, choices and actions.
// Settings and the setup's steps build their rows every frame from what the
// payload says, and react to what handle() reports.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include "core/input.hpp"
#include "core/tween.hpp"
#include "rommps_look.hpp"
#include "rommps_words.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"
#include "ui/motion.hpp"

#include <span>
#include <string>
#include <vector>

namespace rommps
{

enum class RowKind
{
    toggle, // Cross switches it
    choice, // left and right step through its options
    action, // Cross does it
    info,   // only shows something
    header // a section's name
};

struct Row
{
    RowKind kind = RowKind::action;
    std::string label;
    std::string value;  // a choice's option, an action's or info's right-hand text
    std::string detail; // a second, smaller line
    bool on = false;    // a toggle's state
    bool enabled = true;
    bool danger = false; // an action that can't be undone
    float progress = -1.0f; // a download's share done, a slim bar along the row's foot; below 0 for none
    int expander = 0;    // 1: closed, 2: open: a chevron before the label (Cross opens what's under it)
    // A small icon before the label (a platform's logo): the room is kept with
    // logo_slot even while its texture (0) is still loading.
    bool logo_slot = false;
    std::uint32_t logo = 0;
    float logo_aspect = 1.0f;
};

struct RowEvent
{
    int row = -1;
    int step = 0;           // a choice: -1 or +1
    bool activated = false; // a toggle or action: Cross
};

class RowList
{
  public:
    static constexpr float kRowHeight = 92.0f;

    // The touch screen's doing on the rows (RommDeck), before handle(): a
    // tapped row takes the focus and the input gets its Cross (or, on a
    // choice's left arrow, Left); a drag scrolls. What comes back is the input
    // to use from here on.
    hui::InputFrame touch(const hui::InputFrame &input, const std::vector<Row> &rows);
    RowEvent handle(const std::vector<Row> &rows, const hui::InputFrame &input, hui::ui::Feedback &feedback);
    void update(float dt, int count, float visible_height);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const std::vector<Row> &rows,
              const hui::gfx::Rect &bounds, float clock) const;

    int focus() const { return focus_; }
    void set_focus(int row, bool snap = true);
    void refuse(hui::ui::Feedback &feedback);

  private:
    int focus_ = 0;
    // Scrolled by touch, away from the focus: the pad picks up from what's in view.
    bool free_ = false;
    float most_ = 0.0f, visible_ = 0.0f; // the scroll's end and the view's height, from update()
    hui::tween::Spring position_, scroll_;
    hui::ui::Pulse nudge_;
};

// The options the payload's settings take, as Settings and the setup show them.
struct Named
{
    const char *id;
    const char *label;
};
inline constexpr Named kPolicies[] = {
    {"ask", "Ask me"}, {"newest", "Keep the newest"}, {"local", "Keep this " ROMM_HERE "'s"}, {"server", "Keep RomM's"}};
inline constexpr Named kStates[] = {{"off", "Off"}, {"upload", "Back up to RomM"}, {"sync", "Back up and restore"}};
inline constexpr int kIntervals[] = {0, 15, 30, 60, 120, 360};

const char *label_of(std::span<const Named> options, const std::string &id);
// The option before (-1) or after (+1) id, wrapping round.
const char *step_option(std::span<const Named> options, const std::string &id, int step);
std::string interval_label(int minutes);
int step_interval(int minutes, int step);

} // namespace rommps
