// RommDeck in Steam's library: see deck_steam.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck_steam.hpp"
#include "deck_prelude.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <initializer_list>
#include <mutex>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char **environ;

namespace deck::steam
{

namespace
{

// Never freed: the threads below are detached, and may outlive main().
struct State
{
    std::atomic<Shortcut> shortcut{Shortcut::unknown};
    std::mutex lock;
    std::string error;
};
State &state_of()
{
    static State *s = new State;
    return *s;
}

// The program's folder: add-to-steam.py and steam-art/ are beside it.
std::string here()
{
    const char *base = SDL_GetBasePath();
    return base ? base : "./";
}

// What Steam should start: the AppImage when this is one (its runtime says
// where it is, not where it's mounted), else this program.
std::string target()
{
    const char *appimage = std::getenv("APPIMAGE");
    return appimage && *appimage ? std::string(appimage) : here() + "rommdeck";
}

// Steam started us: in Game Mode it's the whole session, and in Desktop Mode
// closing it would take us down with it. Either way, it has us already.
bool from_steam()
{
    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const char *game = std::getenv("SteamGameId");
    return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || (desktop && !std::strcmp(desktop, "gamescope")) ||
           (game && *game);
}

// add-to-steam.py with these flags, for target(): its exit status (-1 when it
// didn't run or didn't exit), and what it printed in out.
int run(std::initializer_list<const char *> flags, std::string &out)
{
    const std::string script = here() + "add-to-steam.py", art = here() + "steam-art", app = target();
    std::vector<const char *> argv{"python3", script.c_str(), "--art", art.c_str()};
    argv.insert(argv.end(), flags.begin(), flags.end());
    argv.push_back(app.c_str());
    argv.push_back(nullptr);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0)
        return -1;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
    pid_t pid = 0;
    const int spawned = posix_spawnp(&pid, argv[0], &actions, nullptr, const_cast<char *const *>(argv.data()), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);
    if (spawned != 0)
    {
        close(fds[0]);
        return -1;
    }
    char buf[512];
    for (;;)
    {
        const ssize_t n = read(fds[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    close(fds[0]);
    int status = -1;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR)
            return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// The line that says why, without the script's name in front.
std::string last_line(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
        text.pop_back();
    if (const std::size_t nl = text.rfind('\n'); nl != std::string::npos)
        text.erase(0, nl + 1);
    if (text.rfind("add-to-steam: ", 0) == 0)
        text.erase(0, 14);
    return text.empty() ? "add-to-steam.py failed" : text;
}

// --check: 0 there, 1 not yet, 2 no Steam user.
void check_now()
{
    std::string out;
    const int rc = run({"--check"}, out);
    state_of().shortcut = rc == 0 ? Shortcut::present : rc == 1 ? Shortcut::missing : Shortcut::unavailable;
    say("Steam shortcut for %s: add-to-steam.py --check says %d", target().c_str(), rc);
}

void add_now()
{
    State &s = state_of();
    std::string out;
    const int rc = run({"--restart-steam"}, out);
    say("Adding to Steam: add-to-steam.py says %d, %s", rc, last_line(out).c_str());
    if (rc != 0)
    {
        const std::lock_guard<std::mutex> g(s.lock);
        s.error = rc < 0 ? "python3 didn't run" : last_line(out);
    }
    s.shortcut = rc == 0 ? Shortcut::added : Shortcut::failed;
}

} // namespace

void look()
{
    State &s = state_of();
    Shortcut expected = Shortcut::unknown;
    if (!s.shortcut.compare_exchange_strong(expected, Shortcut::looking))
        return;
    if (from_steam() || access((here() + "add-to-steam.py").c_str(), R_OK) != 0)
    {
        s.shortcut = Shortcut::unavailable;
        say("Steam shortcut: not offered (%s)", from_steam() ? "Steam started us" : "no add-to-steam.py here");
    }
    else
        std::thread(check_now).detach();
}

void add()
{
    State &s = state_of();
    Shortcut from = s.shortcut.load();
    if (from != Shortcut::missing && from != Shortcut::failed)
        return;
    if (s.shortcut.compare_exchange_strong(from, Shortcut::adding))
        std::thread(add_now).detach();
}

Shortcut state()
{
    return state_of().shortcut.load();
}

std::string error()
{
    State &s = state_of();
    const std::lock_guard<std::mutex> g(s.lock);
    return s.error;
}

} // namespace deck::steam
