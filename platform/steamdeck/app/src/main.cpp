// RommDeck, the Steam Deck app: RommPS's screens in an SDL3 window, drawn by the kit's
// OpenGL renderer. The sync service (platform/steamdeck) does the work; the
// app talks to it on 127.0.0.1, as RommPS talks to the payload on the PS5.
//
// Game Mode (gamescope) gets a full-screen window; Desktop Mode a window of
// the Deck's size that can be resized, or made full screen with F11.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck_kit.hpp"
#include "deck_prelude.h"
#include "deck_touch.hpp"

#include "app/concept.hpp"
#include "gfx/draw_list.hpp"
#include "platform/ps5/system.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdarg>
#include <unistd.h>

namespace ps5ui
{
std::unique_ptr<hui::app::Concept> make_screen(hui::app::Context &context);
}
extern "C" void rommps_set_renderer(void *renderer);

// RommPS's test-run switch (ps5/src/main.cpp on the PS5): never one here.
bool ps5TestRun = false;

extern "C" void say(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "[rommdeck %9.3f] ", now_seconds());
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}

extern "C" double now_seconds(void)
{
    return static_cast<double>(SDL_GetTicksNS()) / 1e9;
}

// The kit's own system calls (platform/ps5/system.hpp names them).
namespace hui::sys
{
std::int64_t monotonic_us()
{
    return static_cast<std::int64_t>(SDL_GetTicksNS() / 1000);
}
void log(const char *format, ...)
{
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    say("%s", line);
}
bool hide_splash_screen()
{
    return true;
}
void sleep_us(std::uint32_t microseconds)
{
    usleep(microseconds);
}
void park()
{
    for (;;)
        usleep(100000);
}
void quit()
{
    std::exit(0);
}
} // namespace hui::sys

namespace
{

// The kit's fonts and sounds: ROMMPS_ASSETS, else assets/ beside the program.
std::string assets_dir()
{
    if (const char *env = std::getenv("ROMMPS_ASSETS"); env && *env)
        return env;
    const char *base = SDL_GetBasePath();
    return std::string(base ? base : "./") + "assets";
}

// The frame just drawn, as a binary PPM (top row first).
void save_frame(const std::string &path, int w, int h)
{
    std::vector<unsigned char> rgb(static_cast<size_t>(w) * h * 3);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f)
        return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
        std::fwrite(rgb.data() + static_cast<size_t>(y) * w * 3, 1, static_cast<size_t>(w) * 3, f);
    std::fclose(f);
    say("saved the frame to %s", path.c_str());
}

// Development: ROMMPS_PRESS="1.5:r1,3:cross,4:down" presses those buttons at
// those seconds, as the PS5's test runs do, to reach a screen for ROMMPS_SHOT.
struct Press
{
    double at;
    std::uint32_t bit;
    hui::Direction nav;
    bool done;
};

std::vector<Press> scripted_presses(double start)
{
    std::vector<Press> out;
    const char *env = std::getenv("ROMMPS_PRESS");
    for (const char *p = env; p && *p;)
    {
        char name[16] = "";
        double t = 0;
        int used = 0;
        if (std::sscanf(p, "%lf:%15[a-z0-9]%n", &t, name, &used) != 2)
            break;
        p += used + (p[used] == ',');
        const std::string n = name;
        Press press{start + t, 0, hui::Direction::none, false};
        if (n == "up")
            press.nav = hui::Direction::up;
        else if (n == "down")
            press.nav = hui::Direction::down;
        else if (n == "left")
            press.nav = hui::Direction::left;
        else if (n == "right")
            press.nav = hui::Direction::right;
        else if (n == "cross" || n == "a")
            press.bit = hui::action_bit(hui::Action::confirm);
        else if (n == "circle" || n == "b")
            press.bit = hui::action_bit(hui::Action::back);
        else if (n == "triangle" || n == "y")
            press.bit = hui::action_bit(hui::Action::north);
        else if (n == "square" || n == "x")
            press.bit = hui::action_bit(hui::Action::west);
        else if (n == "l1")
            press.bit = hui::action_bit(hui::Action::page_prev);
        else if (n == "r1")
            press.bit = hui::action_bit(hui::Action::page_next);
        else if (n == "l2")
            press.bit = hui::action_bit(hui::Action::jump_prev);
        else if (n == "r2")
            press.bit = hui::action_bit(hui::Action::jump_next);
        else if (n == "options")
            press.bit = hui::action_bit(hui::Action::menu);
        else
            continue;
        out.push_back(press);
    }
    return out;
}

// The touch screen and the mouse, in the canvas's coordinates: one pointer,
// the first finger down. The mouse's cursor shows while it moves and hides
// again for the pad, the keyboard or a finger.
class Pointer
{
  public:
    explicit Pointer(SDL_Window *window) : window_(window) {}

    void event(const SDL_Event &e)
    {
        switch (e.type)
        {
        case SDL_EVENT_FINGER_DOWN:
            cursor(false);
            if (fingered_)
                break;
            fingered_ = true;
            finger_ = e.tfinger.fingerID;
            rommps::touch::down(canvas_x(e.tfinger.x), canvas_y(e.tfinger.y));
            break;
        case SDL_EVENT_FINGER_MOTION:
            if (fingered_ && e.tfinger.fingerID == finger_)
                rommps::touch::move(canvas_x(e.tfinger.x), canvas_y(e.tfinger.y));
            break;
        case SDL_EVENT_FINGER_UP:
            if (fingered_ && e.tfinger.fingerID == finger_)
            {
                rommps::touch::move(canvas_x(e.tfinger.x), canvas_y(e.tfinger.y));
                rommps::touch::up();
                fingered_ = false;
            }
            break;
        case SDL_EVENT_FINGER_CANCELED:
            if (fingered_ && e.tfinger.fingerID == finger_)
            {
                rommps::touch::cancel();
                fingered_ = false;
            }
            break;
        // The mouse, but not the one SDL makes up from the touch screen
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT && !fingered_)
            {
                mouse_down_ = true;
                rommps::touch::down(mouse_x(e.button.x), mouse_y(e.button.y));
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (e.motion.which == SDL_TOUCH_MOUSEID)
                break;
            cursor(true);
            if (mouse_down_)
                rommps::touch::move(mouse_x(e.motion.x), mouse_y(e.motion.y));
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT && mouse_down_)
            {
                mouse_down_ = false;
                rommps::touch::move(mouse_x(e.button.x), mouse_y(e.button.y));
                rommps::touch::up();
            }
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (e.wheel.which != SDL_TOUCH_MOUSEID)
                rommps::touch::wheel(mouse_x(e.wheel.mouse_x), mouse_y(e.wheel.mouse_y),
                                     e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y);
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            rommps::touch::cancel();
            fingered_ = false;
            mouse_down_ = false;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: cursor(false); break;
        default: break;
        }
    }

  private:
    // The canvas sits in the window as the renderer's present() puts it:
    // scaled to fit, centred.
    hui::gfx::Viewport viewport(int *w, int *h) const
    {
        SDL_GetWindowSizeInPixels(window_, w, h);
        return hui::gfx::fit_viewport(*w, *h, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight);
    }
    // From 0..1 across the window
    float canvas_x(float nx) const
    {
        int w = 0, h = 0;
        const hui::gfx::Viewport v = viewport(&w, &h);
        return (nx * static_cast<float>(w) - v.offset_x) / v.scale;
    }
    float canvas_y(float ny) const
    {
        int w = 0, h = 0;
        const hui::gfx::Viewport v = viewport(&w, &h);
        return (ny * static_cast<float>(h) - v.offset_y) / v.scale;
    }
    // From the window's points
    float mouse_x(float x) const
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        return canvas_x(w > 0 ? x / static_cast<float>(w) : 0.0f);
    }
    float mouse_y(float y) const
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(window_, &w, &h);
        return canvas_y(h > 0 ? y / static_cast<float>(h) : 0.0f);
    }
    void cursor(bool shown)
    {
        if (shown == cursor_)
            return;
        cursor_ = shown;
        if (shown)
            SDL_ShowCursor();
        else
            SDL_HideCursor();
    }

    SDL_Window *window_;
    SDL_FingerID finger_ = 0;
    bool fingered_ = false; // finger_ is down
    bool mouse_down_ = false;
    bool cursor_ = false;
};

// Development: ROMMPS_TOUCH="2:tap:960:700,3:drag:960:900:960:300,5:hold:400:500"
// touches the canvas (1920 wide) at those seconds: a tap, a quarter-second
// drag (a fling), or a finger resting a second and a half.
struct Stroke
{
    double at, ends;
    float x, y, x2, y2;
    bool down = false, done = false;
};

std::vector<Stroke> scripted_touches(double start)
{
    std::vector<Stroke> out;
    const char *env = std::getenv("ROMMPS_TOUCH");
    for (const char *p = env; p && *p;)
    {
        char kind[8] = "";
        double t = 0;
        float x = 0, y = 0, x2 = 0, y2 = 0;
        int used = 0;
        if (std::sscanf(p, "%lf:%7[a-z]:%f:%f%n", &t, kind, &x, &y, &used) != 4)
            break;
        p += used;
        const std::string k = kind;
        if (k == "drag")
        {
            if (std::sscanf(p, ":%f:%f%n", &x2, &y2, &used) != 2)
                break;
            p += used;
        }
        p += *p == ',';
        const double length = k == "tap" ? 0.05 : k == "hold" ? 1.5 : 0.25;
        out.push_back({start + t, start + t + length, x, y, k == "drag" ? x2 : x, k == "drag" ? y2 : y});
    }
    return out;
}

void play_touches(std::vector<Stroke> &strokes, double now)
{
    for (Stroke &s : strokes)
    {
        if (s.done || now < s.at)
            continue;
        const float f = static_cast<float>(std::min(1.0, (now - s.at) / (s.ends - s.at)));
        const float x = s.x + (s.x2 - s.x) * f, y = s.y + (s.y2 - s.y) * f;
        if (!s.down)
        {
            s.down = true;
            rommps::touch::down(s.x, s.y);
        }
        rommps::touch::move(x, y);
        if (now >= s.ends)
        {
            rommps::touch::up();
            s.done = true;
        }
    }
}

bool in_game_mode()
{
    const char *desktop = std::getenv("XDG_CURRENT_DESKTOP");
    return std::getenv("GAMESCOPE_WAYLAND_DISPLAY") || (desktop && !std::strcmp(desktop, "gamescope"));
}

} // namespace

int main(int argc, char **argv)
{
    bool fullscreen = in_game_mode();
    for (int i = 1; i < argc; i++)
    {
        if (!std::strcmp(argv[i], "--fullscreen"))
            fullscreen = true;
        else if (!std::strcmp(argv[i], "--windowed"))
            fullscreen = false;
    }
    SDL_SetAppMetadata("RommDeck", nullptr, "rommdeck");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO))
    {
        say("SDL: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 6);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window *window = SDL_CreateWindow("RommDeck", 1280, 800,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                              (fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    SDL_GLContext gl = window ? SDL_GL_CreateContext(window) : nullptr;
    if (!gl)
    {
        say("OpenGL 4.6: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_HideCursor();
    say("OpenGL %s on %s, %s", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
        reinterpret_cast<const char *>(glGetString(GL_RENDERER)), fullscreen ? "full screen" : "windowed");

    deck::Kit kit;
    if (!kit.init(assets_dir()))
        return 1;
    rommps_set_renderer(static_cast<hui::gfx::Renderer *>(&kit.renderer));

    hui::Settings choices;
    hui::app::Telemetry telemetry;
    hui::app::Context context{kit.fonts, kit.catalog, telemetry, choices};
    std::unique_ptr<hui::app::Concept> screen = ps5ui::make_screen(context);
    screen->enter();
    kit.apply(choices);
    hui::ui::Feedback feedback;
    feedback.play(hui::audio::Cue::welcome);
    hui::app::Frame frame;

    // Development: ROMMPS_SHOT=<file.ppm> saves the frame drawn ROMMPS_SHOT_AFTER
    // seconds in (3 by default) and quits, to see the screens without a display.
    struct
    {
        std::string path;
        double at = 0.0;
    } shot;
    if (const char *p = std::getenv("ROMMPS_SHOT"); p && *p)
    {
        shot.path = p;
        const char *a = std::getenv("ROMMPS_SHOT_AFTER");
        shot.at = now_seconds() + (a && *a ? std::atof(a) : 3.0);
    }

    std::vector<Press> presses = scripted_presses(now_seconds());
    Pointer pointer(window);
    std::vector<Stroke> strokes = scripted_touches(now_seconds());

    Uint64 last = SDL_GetTicksNS();
    float options_held = 0.0f; // holding the menu button a second closes, as on the PS5
    for (bool running = true; running;)
    {
        for (SDL_Event e; SDL_PollEvent(&e);)
        {
            if (e.type == SDL_EVENT_QUIT)
                running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11 && !e.key.repeat)
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            kit.event(e);
            pointer.event(e);
        }
        const Uint64 now = SDL_GetTicksNS();
        const float dt = std::min(static_cast<float>(now - last) / 1e9f, 0.05f);
        last = now;

        play_touches(strokes, now_seconds());
        rommps::touch::frame(dt);
        hui::InputFrame input = rommps::touch::buttons(kit.input());
        for (Press &p : presses)
            if (!p.done && now_seconds() >= p.at)
            {
                p.done = true;
                input.pressed |= p.bit;
                input.held |= p.bit;
                if (p.nav != hui::Direction::none)
                    input.nav = p.nav;
                break; // one a frame
            }
        options_held = (input.held & hui::action_bit(hui::Action::menu)) ? options_held + dt : 0.0f;
        if (options_held >= 1.0f)
            running = false;
        telemetry.push(dt * 1000.0f);
        screen->update(input, dt, feedback);
        if (context.settings_changed)
        {
            context.settings_changed = false;
            kit.apply(choices);
        }
        kit.play(feedback, screen->info().sounds, choices.haptics);
        feedback.clear();
        kit.tick(dt);

        frame.reset();
        frame.glass_texture = kit.renderer.glass_texture();
        rommps::touch::clear_zones();
        screen->draw(frame);
        kit.renderer.begin();
        kit.renderer.backdrop(frame.backdrop);
        kit.renderer.draw(frame.scene);
        if (frame.glass)
            kit.renderer.glass();
        kit.renderer.draw(frame.overlay);
        kit.renderer.backdrop(frame.post);

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        kit.renderer.present(0, w, h, hui::gfx::kVirtualWidth, hui::gfx::kVirtualHeight);
        if (!shot.path.empty() && now_seconds() >= shot.at)
        {
            save_frame(shot.path, w, h);
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }
    screen.reset();
    kit.release();
    SDL_GL_DestroyContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
