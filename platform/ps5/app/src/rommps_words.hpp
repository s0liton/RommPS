// The words RommPS and RommDeck say differently: the app's name, what the
// device is called and its button names. Each is a string literal, so they
// join the text around them ("Press " ROMM_BTN_CONFIRM " to try again").
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#if defined(ROMMPS_DECK)
#define ROMM_APP "RommDeck"
#define ROMM_APP_UPPER "ROMMDECK" // the Home tab's version tile
#define ROMM_HERE "Deck"         // "this Deck", "on the Deck"
#define ROMM_HERE_UPPER "DECK"
#define ROMM_MODEL "Deck"        // "this Deck"
#define ROMM_PEERS "devices"     // "between your devices"
#define ROMM_DEVICE "device"     // "one device remembers", where a PS5 is a console
#define ROMM_BTN_CONFIRM "A"
#define ROMM_BTN_BACK "B"
#define ROMM_BTN_NORTH "Y"
#define ROMM_BTN_WEST "X"
#define ROMM_KEY_CONFIRM "A" // upper case, for the Codec's key line
#define ROMM_KEY_BACK "B"
#define ROMM_STORE "GitHub"        // where the app's updates come from
#define ROMM_UPDATE_IN "on GitHub"
#define ROMM_SOURCE_URL "https://github.com/s0liton/RommDeck" // the credits' QR code
#define ROMM_SERVICE_DOWN ROMM_APP " couldn't start its background service. Press " ROMM_BTN_CONFIRM " to try again, or " \
                          "start it in Desktop Mode with: systemctl --user start romm-sync"
#else
#define ROMM_APP "RommPS"
#define ROMM_APP_UPPER "ROMMPS"
#define ROMM_HERE "console"
#define ROMM_HERE_UPPER "CONSOLE"
#define ROMM_MODEL "PS5"
#define ROMM_PEERS "consoles"
#define ROMM_DEVICE "console"
#define ROMM_BTN_CONFIRM "Cross"
#define ROMM_BTN_BACK "Circle"
#define ROMM_BTN_NORTH "Triangle"
#define ROMM_BTN_WEST "Square"
#define ROMM_KEY_CONFIRM "CROSS"
#define ROMM_KEY_BACK "CIRCLE"
#define ROMM_STORE "ProsperoStore"
#define ROMM_UPDATE_IN "in ProsperoStore"
#define ROMM_SOURCE_URL "https://github.com/s0liton/RommPS"
#define ROMM_SERVICE_DOWN "RommPS couldn't start its background service. Press Cross to try again, or load romm-sync.elf " \
                          "with your HEN's payload loader."
#endif
