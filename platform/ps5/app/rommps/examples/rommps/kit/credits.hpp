// RommPS: the credits card in Settings (who made it, a QR code to its
// source) and the Codec call behind the author's portrait.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "core/input.hpp"
#include "gfx/renderer.hpp"
#include "rommps_look.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rommps
{

inline constexpr const char *kSourceUrl = "https://github.com/s0liton/ps5-romm";

// A QR code's modules, true for dark. Empty if the text didn't fit.
struct QrCode
{
    int size = 0;
    std::vector<bool> modules;
    static QrCode encode(const std::string &text);
    bool dark(int x, int y) const { return modules[static_cast<std::size_t>(y * size + x)]; }
};

// Draws a QR code on a white plate (its quiet zone included) filling rect.
void draw_qr(hui::gfx::DrawList &list, const QrCode &qr, const hui::gfx::Rect &rect);

// The author's portrait, as a texture once the renderer is there (0 before).
std::uint32_t avatar_texture(hui::gfx::Renderer *renderer);

void draw_credits(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const hui::gfx::Rect &rect,
                  bool focused, float clock);

// A call on 140.85: the author's portrait, a voice with no face, and a
// conversation about what a save file is, an original homage to a famous
// Codec call. It plays by itself; Cross skips a line, Circle hangs up. At the
// end it asks two questions, and a yes opens a video in the console's browser
// (with its QR code on screen, for a phone); back from the browser, Cross
// carries on.
class CodecCall
{
  public:
    void start();
    bool active() const { return active_; }
    void update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, float clock) const;

  private:
    void go(int step, hui::ui::Feedback &feedback);

    bool active_ = false;
    float age_ = 0.0f;   // since the call started
    int step_ = -1;      // -1 while it rings
    float step_age_ = 0.0f;
    int typed_ = 0;      // characters of the line shown
    int choice_ = 0;     // a question's answer: 0 yes, 1 no
    bool opened_ = false; // the browser took the video
    float closing_ = -1.0f;
};

} // namespace rommps
