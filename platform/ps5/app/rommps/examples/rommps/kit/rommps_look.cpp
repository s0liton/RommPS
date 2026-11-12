// RommPS: the screen's look. See rommps_look.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rommps_look.hpp"
#if defined(ROMMPS_DECK)
#include "rommps_api.hpp"
#endif

#include "core/save_file.hpp"
#include "ui/components/component.hpp"

#include <iterator>
#include <string>
#include <sys/stat.h>
#include <vector>

// Set for a whole test run (ps5/src/main.cpp): its pictures never depend on
// what was picked by hand.
extern bool ps5TestRun;

#ifndef PS5_APP_ROOT
#define PS5_APP_ROOT "/app0"
#endif

namespace rommps
{

namespace
{

#if defined(ROMMPS_DECK)
// The app's own settings, beside RomM Sync's in the XDG data folder.
const std::string kFolder = service_dir() + "/app";
#else
const std::string kFolder = PS5_APP_ROOT "/hui";
#endif
const std::string kPath = kFolder + "/rommps-look.txt";
const std::string kSoundsPath = kFolder + "/rommps-sounds.txt";

// On the PS5, files every app (and FTP) can change, as the console expects;
// in the Deck's home folder they keep the user's own permissions.
void loosen(const std::string &path, mode_t mode)
{
#if defined(ROMMPS_DECK)
    (void)path;
    (void)mode;
#else
    chmod(path.c_str(), mode);
#endif
}

const hui::ui::Theme *theme_named(const char *id)
{
    for (const hui::ui::Theme &t : hui::ui::themes())
        if (std::string(t.id) == id)
            return &t;
    return &hui::ui::default_theme();
}

Color or_else(Color a, Color b)
{
    return a.a > 0.0f ? a : b;
}

// The design RommPS was drawn for: white on the cover's aurora, frosted panels.
Look cover_colours()
{
    Look l;
    l.id = "covers";
    l.name = "Cover colours";
    l.family = "Follows the game you're looking at";
    l.covers = true;
    l.theme = theme_named("acrylic");
    l.backdrop.mode = hui::gfx::BackdropMode::aurora;
    l.page = Color::rgb(0x05060c);
    l.text = Color::rgb(0xffffff);
    l.muted = Color::rgb(0xffffff, 0.72f);
    l.panel = Color::rgb(0x0b1026, 0.5f);
    l.panel_text = l.text;
    l.panel_muted = l.muted;
    l.outline = Color::rgb(0xffffff, 0.2f);
    l.accent = Color::rgb(0x76d6ff);
    l.button = Color::rgb(0xffffff);
    l.on_button = Color::rgb(0x0b0d16);
    l.focus = Color::rgb(0xffffff);
    l.highlight = Color::rgb(0xffffff);
    l.on_highlight = Color::rgb(0x0b0d16);
    l.danger = Color::rgb(0xf87171);
    l.success = Color::rgb(0x4ade80);
    l.warning = Color::rgb(0xfbbf24);
    l.radius = 28.0f;
    l.dark = true;
    return l;
}

Look from_theme(const hui::ui::Theme &t)
{
    Look l;
    l.id = t.id;
    l.name = t.name;
    l.family = t.family;
    l.theme = &t;
    l.backdrop = t.backdrop;
    l.page = t.page;
    l.text = or_else(t.page_text, t.text);
    l.muted = or_else(t.page_text_muted, t.text_muted);
    l.panel = t.surface;
    l.panel_text = t.text;
    l.panel_muted = t.text_muted;
    l.outline = t.outline;
    l.accent = t.accent;
    l.button = t.primary;
    l.on_button = t.on_primary;
    l.focus = t.focus;
    l.highlight = t.primary;
    l.on_highlight = t.on_primary;
    l.danger = t.danger;
    l.success = t.success;
    l.warning = t.warning;
    l.radius = t.radius_card;
    l.dark = t.dark;
    return l;
}

const std::vector<Look> &all()
{
    static const std::vector<Look> looks = [] {
        std::vector<Look> v{cover_colours()};
        for (const hui::ui::Theme &t : hui::ui::themes())
            v.push_back(from_theme(t));
        return v;
    }();
    return looks;
}

int g_current = -1; // -1: not read yet
int g_sounds = -1;  // the same

// The sounds as saved and as shown, in Sounds' order. Before there was a
// choice the file said "on" or "off": "on" was the menu sounds, which is also
// what anything unknown gets. Older versions hear anything but "off" as on.
const struct
{
    const char *id;
    const char *name;
} kSounds[] = {
    {"metal-gear", "Metal Gear Solid"}, {"paper", "Paper"}, {"glass", "Glass"}, {"tones", "Tones"}, {"off", "Off"},
};
constexpr int kSoundsCount = static_cast<int>(Sounds::count);
static_assert(std::size(kSounds) == static_cast<std::size_t>(kSoundsCount), "a name for every Sounds");

} // namespace

int look_count()
{
    return static_cast<int>(all().size());
}

const Look &look(int index)
{
    const std::vector<Look> &v = all();
    return v[index >= 0 && index < static_cast<int>(v.size()) ? static_cast<std::size_t>(index) : 0];
}

int current_look()
{
    if (ps5TestRun && g_current < 0)
        g_current = 0; // a test run starts from the default, and may change it
    if (g_current < 0)
    {
        g_current = 0;
        std::string id;
        if (hui::save::read_file(kPath, &id, 64))
        {
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' '))
                id.pop_back();
            for (int i = 0; i < look_count(); ++i)
                if (id == look(i).id)
                    g_current = i;
        }
    }
    return g_current;
}

void set_look(int index)
{
    if (index < 0 || index >= look_count())
        return;
    g_current = index;
    if (ps5TestRun)
        return;
    hui::save::ensure_directory(kFolder);
    loosen(kFolder, 0777);
    if (hui::save::write_atomic(kPath, std::string(look(index).id) + "\n").empty())
        loosen(kPath, 0666);
}

const char *sounds_name(Sounds sounds)
{
    const int i = static_cast<int>(sounds);
    return i >= 0 && i < kSoundsCount ? kSounds[i].name : "";
}

Sounds ui_sounds()
{
    if (g_sounds < 0)
    {
        g_sounds = 0; // a test run hears the default, as does a file that says "on"
        std::string id;
        if (!ps5TestRun && hui::save::read_file(kSoundsPath, &id, 16))
        {
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' '))
                id.pop_back();
            for (int i = 0; i < kSoundsCount; ++i)
                if (id == kSounds[i].id)
                    g_sounds = i;
        }
    }
    return static_cast<Sounds>(g_sounds);
}

void set_ui_sounds(Sounds sounds)
{
    const int i = static_cast<int>(sounds);
    if (i < 0 || i >= kSoundsCount)
        return;
    g_sounds = i;
    if (ps5TestRun)
        return;
    hui::save::ensure_directory(kFolder);
    loosen(kFolder, 0777);
    if (hui::save::write_atomic(kSoundsPath, std::string(kSounds[i].id) + "\n").empty())
        loosen(kSoundsPath, 0666);
}

void draw_panel(hui::gfx::DrawList &list, const Look &look, const hui::gfx::Rect &r, float radius)
{
    if (radius < 0.0f)
        radius = look.radius;
    list.shadow({r.x, r.y + 16, r.w, r.h}, radius, 34, Color::rgb(0x000000, look.dark ? 0.4f : 0.12f));
    list.rounded_rect(r, radius, look.panel);
    list.bordered_rect(r, radius, Color::rgb(0x000000, 0.0f), 1.5f, look.outline);
}

} // namespace rommps

extern "C" void rommps_play_sounds(hui::ui::Feedback &feedback, hui::audio::Mixer &mixer, hui::audio::SoundBank &menu,
                                   hui::audio::SoundBank &kit, hui::audio::SoundSet design)
{
    using hui::audio::SoundSet;
    using rommps::Sounds;
    static hui::audio::SoundBank tones; // records nothing: every cue falls through to its tone
    const Sounds sounds = rommps::ui_sounds();
    for (const hui::audio::CueEvent &event : feedback.cues)
    {
        const SoundSet set = event.set == SoundSet::count ? design : event.set;
        if (sounds == Sounds::metal_gear)
            menu.play(mixer, set, event);
        else if (sounds == Sounds::paper || sounds == Sounds::glass)
            kit.play(mixer, sounds == Sounds::paper ? SoundSet::paper : SoundSet::glass, event);
        else if (sounds == Sounds::tones)
        {
            // The tones were meant to stand in for a missing recording, not
            // beside the sets, and sit about 12 dB over them: a quarter evens it out.
            hui::audio::CueEvent quieter = event;
            quieter.gain *= 0.25f;
            tones.play(mixer, set, quieter);
        }
    }
    feedback.cues.clear();
}
