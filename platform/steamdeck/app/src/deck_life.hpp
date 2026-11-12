// RommDeck's comings and goings: one copy per user, and a way out that's quick
// and doesn't trip over the AppImage's mount on the way.
//
// Once it's up, the app never leaves through exit() or the end of main().
// Steam closes a game by signalling everything it started, and that includes
// the AppImage's FUSE server, which unmounts the squashfs the program runs
// from as it goes. exit()'s static destructors and atexit handlers are code
// nobody ran yet, so pages nobody read yet: SIGBUS, and a core dump for an
// app that was only asked to close. So every way out ends in _exit(), after
// only what has to happen: settings are already saved as they change, and the
// kernel closes the window, the sockets and the threads with the process.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <SDL3/SDL.h>

namespace deck
{

class Kit;

// SIGTERM (Steam, systemd), SIGINT and SIGHUP end the app on the spot. First
// thing in main(), before SDL_Init: SDL's own handlers would only queue a quit
// event and take the long way out.
void catch_quit_signals();

// True when no other RommDeck of this user is open. Otherwise that one has
// been asked to come forward, and this launch should just go. A stale name
// can't block a launch: it goes with the process that held it, and a copy
// that's stuck makes way when knocked. Before SDL_Init.
bool first_instance();

// After SDL_Init: a knock wakes the main loop wherever it's waiting.
void wake_on_knocks();

// Once a turn of the main loop: true when another launch knocked since the
// last turn. The turns are also how a knock tells a live copy from a stuck one.
bool knocked();

// The way out: the sound and the pad let go (no sound cut off mid-click, no
// rumble left running), then _exit. Two seconds at most. why goes in the log.
[[noreturn]] void leave(Kit &kit, const char *why);

} // namespace deck
