// The Steam Deck app's host for PS5_VKHomebrewUI's kit: what ps5/ui/kit.hpp is
// on the PS5, on SDL3 and OpenGL. The renderer is the kit's own OpenGL one,
// the pad is SDL's gamepad (Steam Input's, in Game Mode) or the keyboard, and
// the sounds play through SDL's audio stream.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "audio/cues.hpp"
#include "audio/mixer.hpp"
#include "audio/music.hpp"
#include "core/input.hpp"
#include "core/settings.hpp"
#include "demo/catalog.hpp"
#include "gfx/font.hpp"
#include "gfx/gl_renderer.hpp"
#include "ui/feedback.hpp"
#include "ui/fonts.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace deck
{

class Kit
{
  public:
    Kit() = default;
    Kit(const Kit &) = delete;
    Kit &operator=(const Kit &) = delete;
    ~Kit();

    // With the window's GL context current. assets: the kit's fonts and sounds.
    bool init(const std::string &assets);
    void release();

    // An SDL event: gamepads coming and going, keys.
    void event(const SDL_Event &e);
    // This frame's input, from the gamepad and the keyboard together.
    hui::InputFrame input();
    void play(const hui::ui::Feedback &feedback, hui::audio::SoundSet set, bool haptics = true);
    void tick(float dt);
    void apply(const hui::Settings &settings);

    std::string assets;
    hui::gfx::GlRenderer renderer;
    hui::ui::Fonts fonts;
    hui::demo::Catalog catalog;
    hui::audio::Mixer mixer;
    hui::audio::SoundBank sounds;
    hui::audio::MusicPlayer music;
    hui::InputTracker tracker;

  private:
    void open_first_gamepad();
    void load_sound_pack(const std::string &dir);

    hui::gfx::Font faces_[6];
    SDL_Gamepad *pad_ = nullptr;
    SDL_AudioStream *audio_ = nullptr;
    std::uint32_t keys_ = 0; // pad_bits the keyboard holds
    float rumble_left_ = 0.0f;
    bool ready_ = false;
};

} // namespace deck
