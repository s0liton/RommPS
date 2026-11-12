// RommDeck in Steam's library, for the setup's "Add RommDeck to Steam": an
// AppImage run from Downloads shouldn't send anyone to Steam's "Add a
// Non-Steam Game" (no artwork, named after the file). add-to-steam.py does the
// work, the same one install.sh and rommdeck-installer.sh use; the AppImage
// and the tarball both keep it beside the program, with steam-art/.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

namespace deck::steam
{

enum class Shortcut
{
    unknown,     // not looked for yet
    looking,     // add-to-steam.py --check is on it
    missing,     // not in Steam: worth offering
    present,     // in Steam already
    unavailable, // nothing to offer: Steam started us (Game Mode), no Steam user, no add-to-steam.py
    adding,      // Steam closing, the shortcut being written, Steam starting again
    added,       // in Steam now, with its artwork
    failed,      // error() says why
};

// Looks for RommDeck in Steam's shortcuts, in the background, once.
void look();
// Adds it, in the background. Steam closes for a moment if it's running: ask first.
void add();
Shortcut state();
std::string error();

} // namespace deck::steam
