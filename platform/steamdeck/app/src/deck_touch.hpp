// RommDeck's touch screen, the host's half: SDL's fingers and mouse go in, in
// canvas coordinates, and touch.hpp's takers get what they did. One finger
// counts; a second one is ignored while the first is down.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "touch.hpp"

namespace rommps::touch
{

void down(float x, float y);
void move(float x, float y);
void up();
void cancel(); // the system took the finger: nothing it was doing happens
// The mouse wheel at (x, y): notches, positive away from the user.
void wheel(float x, float y, float notches);
// Once a frame, before the screen's update.
void frame(float dt);
// The hint row's buttons under the finger, pressed in the pad's input.
hui::InputFrame buttons(const hui::InputFrame &input);
// Before the screen draws: it registers its zones anew.
void clear_zones();
// A finger is down, or a fling is still coasting.
bool busy();

} // namespace rommps::touch
