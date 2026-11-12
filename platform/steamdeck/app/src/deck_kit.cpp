// SPDX-License-Identifier: GPL-3.0-or-later
#include "deck_kit.hpp"

#include "audio/wav.hpp"
#include "core/save_file.hpp"
#include "deck_prelude.h"

#include <algorithm>
#include <span>

namespace deck
{

namespace
{

const char *const kFaceFiles[6] = {
    "inter-regular.huifont",    "inter-semibold.huifont", "montserrat-medium.huifont",
    "dejavu-sans-mono.huifont", "press-start-2p.huifont", "patrick-hand.huifont",
};

// SDL asks for more sound; the mixer makes it, in chunks of this many frames.
void SDLCALL feed_audio(void *user, SDL_AudioStream *stream, int additional, int total)
{
    (void)total;
    auto *mixer = static_cast<hui::audio::Mixer *>(user);
    std::int16_t chunk[1024 * 2];
    for (int frames = additional / 4; frames > 0;)
    {
        const int n = std::min(frames, 1024);
        mixer->render(chunk, n);
        SDL_PutAudioStreamData(stream, chunk, n * 4);
        frames -= n;
    }
}

// The keyboard, for Desktop Mode without Steam Input: arrows move, Enter
// confirms, Escape and Backspace go back, and the rest follow the pad's
// face buttons by position (Y X / A B on the right hand: T, F, G, H).
std::uint32_t key_bit(SDL_Keycode key)
{
    using namespace hui::pad_bits;
    switch (key)
    {
    case SDLK_UP: return kUp;
    case SDLK_DOWN: return kDown;
    case SDLK_LEFT: return kLeft;
    case SDLK_RIGHT: return kRight;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE: return kCross;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: return kCircle;
    case SDLK_TAB: return kTriangle;
    case SDLK_F: return kSquare;
    case SDLK_Q:
    case SDLK_PAGEUP: return kL1;
    case SDLK_E:
    case SDLK_PAGEDOWN: return kR1;
    case SDLK_HOME: return kL2;
    case SDLK_END: return kR2;
    case SDLK_F1: return kOptions;
    default: return 0;
    }
}

// A sound pack file's cue: "focus_01.wav" is Cue::focus.
bool cue_of_file(const std::string &file, hui::audio::Cue *cue)
{
    if (file.size() <= 4 || file.compare(file.size() - 4, 4, ".wav") != 0)
        return false;
    std::string stem = file.substr(0, file.size() - 4);
    const size_t underscore = stem.rfind('_');
    if (underscore != std::string::npos && underscore + 1 < stem.size() &&
        stem.find_first_not_of("0123456789", underscore + 1) == std::string::npos)
        stem.resize(underscore);
    for (int i = 0; i < static_cast<int>(hui::audio::Cue::count); i++)
        if (stem == hui::audio::cue_name(static_cast<hui::audio::Cue>(i)))
        {
            *cue = static_cast<hui::audio::Cue>(i);
            return true;
        }
    return false;
}

std::uint8_t axis_byte(Sint16 v)
{
    return static_cast<std::uint8_t>((static_cast<int>(v) + 32768) >> 8);
}

} // namespace

Kit::~Kit()
{
    release();
}

bool Kit::init(const std::string &assets_dir)
{
    assets = assets_dir;
    if (!renderer.init())
    {
        say("ui kit: the OpenGL renderer failed to start");
        return false;
    }
    ready_ = true;
    hui::ui::FontRef *refs[6] = {&fonts.regular, &fonts.semibold, &fonts.display,
                                 &fonts.mono,    &fonts.pixel,    &fonts.hand};
    for (int i = 0; i < 6; i++)
    {
        std::string data;
        const std::string path = assets + "/fonts/" + kFaceFiles[i];
        if (!hui::save::read_file(path, &data) || !faces_[i].load(data))
        {
            say("ui kit: font %s: %s", path.c_str(), faces_[i].error().c_str());
            return false;
        }
        refs[i]->font = &faces_[i];
        refs[i]->texture = renderer.create_font_texture(faces_[i]);
    }
    if (!catalog.build_covers(renderer, fonts))
        say("ui kit: no sample covers");

    const auto bank = sounds.load(assets + "/audio/sfx");
    load_sound_pack(assets + "/sounds");
    const SDL_AudioSpec spec{SDL_AUDIO_S16, 2, hui::audio::kSampleRate};
    audio_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed_audio, &mixer);
    if (audio_)
        SDL_ResumeAudioStreamDevice(audio_);
    say("ui kit: %d sounds (%d rejected), output %s", bank.files, bank.rejected, audio_ ? "playing" : SDL_GetError());
    open_first_gamepad();
    return true;
}

// RommPS's menu sounds (as rommps.cpp loads them on the PS5): each replaces
// its cue in every sound set; the kit's own play the cues the pack lacks.
void Kit::load_sound_pack(const std::string &dir)
{
    using namespace hui::audio;
    std::vector<std::string> files = hui::save::list_files(dir);
    std::sort(files.begin(), files.end());
    SoundBank bank;
    bool replaced[static_cast<int>(Cue::count)] = {};
    int loaded = 0;
    for (const std::string &name : files)
    {
        Cue cue;
        std::string data;
        if (!cue_of_file(name, &cue) || !hui::save::read_file(dir + "/" + name, &data, 16u << 20))
            continue;
        DecodedWav wav = decode_wav(data);
        if (!wav.ok())
        {
            say("sound pack: %s: %s", name.c_str(), wav.error.c_str());
            continue;
        }
        for (int set = 0; set < static_cast<int>(SoundSet::count); set++)
            bank.add(static_cast<SoundSet>(set), cue, wav.samples, wav.frames);
        replaced[static_cast<int>(cue)] = true;
        loaded++;
    }
    if (!loaded)
        return;
    for (int set = 0; set < static_cast<int>(SoundSet::count); set++)
    {
        const std::string kit_dir = assets + "/audio/sfx/" + sound_set_name(static_cast<SoundSet>(set));
        for (const std::string &name : hui::save::list_files(kit_dir))
        {
            Cue cue;
            std::string data;
            if (!cue_of_file(name, &cue) || replaced[static_cast<int>(cue)] ||
                !hui::save::read_file(kit_dir + "/" + name, &data, 16u << 20))
                continue;
            DecodedWav wav = decode_wav(data);
            if (wav.ok())
                bank.add(static_cast<SoundSet>(set), cue, std::move(wav.samples), wav.frames);
        }
    }
    sounds = std::move(bank);
    say("sound pack: %d sounds from %s", loaded, dir.c_str());
}

void Kit::release()
{
    if (audio_)
    {
        SDL_DestroyAudioStream(audio_);
        audio_ = nullptr;
    }
    if (pad_)
    {
        SDL_CloseGamepad(pad_);
        pad_ = nullptr;
    }
    if (ready_)
    {
        renderer.release();
        ready_ = false;
    }
}

void Kit::open_first_gamepad()
{
    if (pad_ || background_)
        return;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (ids && count > 0)
    {
        pad_ = SDL_OpenGamepad(ids[0]);
        const char *name = pad_ ? SDL_GetGamepadName(pad_) : nullptr;
        if (name && pad_named_ != name)
        {
            pad_named_ = name;
            say("gamepad: %s", name);
        }
    }
    SDL_free(ids);
}

void Kit::set_background(bool background)
{
    if (background == background_)
        return;
    background_ = background;
    if (!background)
        open_first_gamepad();
    else if (pad_)
    {
        SDL_CloseGamepad(pad_);
        pad_ = nullptr;
    }
}

void Kit::event(const SDL_Event &e)
{
    switch (e.type)
    {
    case SDL_EVENT_GAMEPAD_ADDED: open_first_gamepad(); break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (pad_ && SDL_GetGamepadID(pad_) == e.gdevice.which)
        {
            SDL_CloseGamepad(pad_);
            pad_ = nullptr;
            open_first_gamepad();
        }
        break;
    case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat)
            keys_ |= key_bit(e.key.key);
        break;
    case SDL_EVENT_KEY_UP: keys_ &= ~key_bit(e.key.key); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST: keys_ = 0; break;
    default: break;
    }
}

hui::InputFrame Kit::input()
{
    using namespace hui::pad_bits;
    hui::PadSample s;
    s.connected = true;
    s.buttons = keys_;
    s.timestamp_us = SDL_GetTicksNS() / 1000;
    if (pad_)
    {
        // Positions, not labels: south confirms, as Cross does on the PS5.
        static const struct
        {
            SDL_GamepadButton button;
            std::uint32_t bit;
        } kMap[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, kCross},      {SDL_GAMEPAD_BUTTON_EAST, kCircle},
            {SDL_GAMEPAD_BUTTON_WEST, kSquare},      {SDL_GAMEPAD_BUTTON_NORTH, kTriangle},
            {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, kL1}, {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, kR1},
            {SDL_GAMEPAD_BUTTON_LEFT_STICK, kL3},    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, kR3},
            {SDL_GAMEPAD_BUTTON_START, kOptions},    {SDL_GAMEPAD_BUTTON_BACK, kTouchpad},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, kUp},       {SDL_GAMEPAD_BUTTON_DPAD_DOWN, kDown},
            {SDL_GAMEPAD_BUTTON_DPAD_LEFT, kLeft},   {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, kRight},
        };
        for (const auto &m : kMap)
            if (SDL_GetGamepadButton(pad_, m.button))
                s.buttons |= m.bit;
        s.left_x = axis_byte(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX));
        s.left_y = axis_byte(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY));
        s.right_x = axis_byte(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTX));
        s.right_y = axis_byte(SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHTY));
        s.l2 =
            static_cast<std::uint8_t>(std::max<int>(0, SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) >> 7);
        s.r2 =
            static_cast<std::uint8_t>(std::max<int>(0, SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) >> 7);
        if (s.l2 > 128)
            s.buttons |= kL2;
        if (s.r2 > 128)
            s.buttons |= kR2;
    }
    return tracker.update(std::span<const hui::PadSample>(&s, 1), s.timestamp_us);
}

void Kit::play(const hui::ui::Feedback &feedback, hui::audio::SoundSet set, bool haptics)
{
    for (const hui::audio::CueEvent &event : feedback.cues)
        sounds.play(mixer, event.set == hui::audio::SoundSet::count ? set : event.set, event);
    if (haptics && pad_ && feedback.rumble_strength > 0.0f && feedback.rumble_seconds > 0.0f)
    {
        const float strength = std::min(feedback.rumble_strength, 1.0f);
        const auto level = [](float v) { return static_cast<Uint16>(std::clamp(v, 0.0f, 1.0f) * 65535.0f); };
        SDL_RumbleGamepad(pad_, level(strength > 0.6f ? (strength - 0.6f) * 2.5f : 0.0f), level(strength),
                          static_cast<Uint32>(feedback.rumble_seconds * 1000.0f));
    }
}

void Kit::tick(float dt)
{
    music.pump(dt);
}

void Kit::apply(const hui::Settings &settings)
{
    mixer.set_bus_gain(hui::audio::Bus::music, hui::Settings::gain(settings.music_volume));
    mixer.set_bus_gain(hui::audio::Bus::sfx, hui::Settings::gain(settings.sfx_volume));
    mixer.set_bus_gain(hui::audio::Bus::ui, hui::Settings::gain(settings.ui_volume));
    hui::InputSettings input = tracker.settings();
    input.swap_confirm = settings.swap_confirm;
    tracker.set_settings(input);
}

} // namespace deck
