// RommDeck's comings and goings (deck_life.hpp). Built with -fno-plt: the
// signal handler reaches _exit through the GOT, filled in at start-up, and not
// through a PLT stub or the lazy binder, which live in the program's own pages.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck_life.hpp"

#include "deck_kit.hpp"
#include "deck_prelude.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace deck
{

namespace
{

// Runs when the mount may already be gone: no logging, no strings, nothing
// but libc (outside the mount). Its page is locked in memory below.
void quit_now(int)
{
    _exit(0);
}

// A request to a service that just went away: an error for the request, not
// the end of the app. A handler rather than SIG_IGN, so xdg-open and systemctl
// don't inherit an ignored SIGPIPE.
void broken_pipe(int)
{
}

// quit_now's code, read in and kept there: it mustn't be the page fault that
// meets the unmounted AppImage.
void keep_resident()
{
    const auto page = static_cast<std::uintptr_t>(sysconf(_SC_PAGESIZE));
    const auto *code = reinterpret_cast<const char *>(&quit_now);
    const char *start = code - (reinterpret_cast<std::uintptr_t>(code) & (page - 1));
    if (mlock(start, static_cast<std::size_t>(code - start) + 256) != 0)
        say("couldn't keep the quit handler in memory: %s", std::strerror(errno));
}

// Linux's abstract socket names: no file to go stale, and the name goes with
// the process holding it, however it ends.
socklen_t instance_name(sockaddr_un *name)
{
    *name = {};
    name->sun_family = AF_UNIX;
    const int n =
        std::snprintf(name->sun_path + 1, sizeof name->sun_path - 1, "rommdeck-%u", static_cast<unsigned>(getuid()));
    return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + n);
}

bool peer_is_us(int fd)
{
    ucred who{};
    socklen_t size = sizeof who;
    return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &who, &size) == 0 && who.uid == getuid();
}

int g_listener = -1;
std::atomic<Uint64> g_beat{0};      // SDL_GetTicksNS() at the main loop's last turn
std::atomic<bool> g_knocked{false}; // another launch knocked since the main loop last looked
std::atomic<Uint32> g_wake{0};      // the SDL event that wakes the main loop, once SDL is up
constexpr Sint64 kStuckNs = 10 * SDL_NS_PER_SECOND;
constexpr Uint64 kStartingNs = 20 * SDL_NS_PER_SECOND; // the first turn may take this much longer
constexpr int kAnswerMs = 3000;

// The name, if it's free: then this copy is the one.
bool claim(const sockaddr_un &name, socklen_t size)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && bind(fd, reinterpret_cast<const sockaddr *>(&name), size) == 0 && listen(fd, 4) == 0)
    {
        g_listener = fd;
        return true;
    }
    const int why = errno;
    if (fd >= 0)
        close(fd);
    errno = why;
    return false;
}

// Each knock from this user gets a byte back at once, and the main loop comes
// forward on its next turn. A copy whose main loop stopped can't come forward
// (a GPU hang after a resume, say): it makes way instead, and the knock's
// launch opens in its place.
void hear_knocks()
{
    for (;;)
    {
        const int knock = accept4(g_listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (knock < 0)
        {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            say("no longer listening for other launches: %s", std::strerror(errno));
            return;
        }
        if (!peer_is_us(knock))
        {
            close(knock);
            continue;
        }
        if (static_cast<Sint64>(SDL_GetTicksNS() - g_beat) > kStuckNs)
        {
            say("launched again while stuck: making way for the new copy");
            _exit(1);
        }
        const char yes = 1;
        send(knock, &yes, 1, MSG_NOSIGNAL);
        close(knock);
        g_knocked = true;
        if (const Uint32 type = g_wake)
        {
            SDL_Event e = {};
            e.type = type;
            SDL_PushEvent(&e);
        }
    }
}

// True when the copy holding the name answered; *theirs when it's another
// user's program sitting on it.
bool knock_on(const sockaddr_un &name, socklen_t size, bool *theirs)
{
    const int knock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    bool answered = false;
    if (knock >= 0 && connect(knock, reinterpret_cast<const sockaddr *>(&name), size) == 0)
    {
        *theirs = !peer_is_us(knock);
        pollfd ready = {knock, POLLIN, 0};
        char yes = 0;
        answered = !*theirs && poll(&ready, 1, kAnswerMs) > 0 && recv(knock, &yes, 1, 0) == 1;
    }
    if (knock >= 0)
        close(knock);
    return answered;
}

} // namespace

void catch_quit_signals()
{
    struct sigaction quit = {};
    quit.sa_handler = quit_now;
    sigfillset(&quit.sa_mask);
    for (const int sig : {SIGTERM, SIGINT, SIGHUP, SIGALRM})
        sigaction(sig, &quit, nullptr);
    struct sigaction shrug = {};
    shrug.sa_handler = broken_pipe;
    sigaction(SIGPIPE, &shrug, nullptr);
    keep_resident();
}

bool first_instance()
{
    sockaddr_un name;
    const socklen_t size = instance_name(&name);
    bool alone = claim(name, size);
    if (!alone && errno != EADDRINUSE)
    {
        say("can't tell whether RommDeck is already open (%s): opening anyway", std::strerror(errno));
        return true;
    }
    if (!alone)
    {
        bool theirs = false;
        if (knock_on(name, size, &theirs))
        {
            say("RommDeck is already open: asked it to come forward");
            return false;
        }
        if (theirs)
        {
            say("another user's program holds RommDeck's name: opening anyway");
            return true;
        }
        // No answer: a stuck copy making way, or one that just closed. The
        // name goes with it.
        for (int tries = 0; tries < 20 && !alone; tries++)
        {
            usleep(100000);
            alone = claim(name, size);
        }
        if (!alone)
        {
            say("RommDeck is already open and not answering: opening anyway");
            return true;
        }
    }
    g_beat = SDL_GetTicksNS() + kStartingNs;
    std::thread(hear_knocks).detach();
    return true;
}

void wake_on_knocks()
{
    if (g_listener >= 0)
        g_wake = SDL_RegisterEvents(1);
}

bool knocked()
{
    g_beat = SDL_GetTicksNS();
    return g_knocked.exchange(false);
}

void leave(Kit &kit, const char *why)
{
    say("closing: %s", why);
    alarm(2); // a sound or pad driver that won't let go: quit_now
    kit.release();
    _exit(0);
}

} // namespace deck
