// RommPS: the credits card and the Codec call. See credits.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "credits.hpp"

#include "avatar_png.h"
#include "core/tween.hpp"
#include "qrcodegen.h"
#include "stb_image.h" // implemented by base/VulkanglTFModel.cpp

#include <algorithm>
#include <cmath>
#include <cstring>

#if !defined(PS5_HOST_REFERENCE)
extern "C" int sceSystemServiceLaunchWebBrowser(const char *uri, void *param);
#endif

namespace rommps
{

namespace
{

namespace ui = hui::ui;
namespace tween = hui::tween;
using hui::Action;
using hui::audio::Cue;
using hui::gfx::Align;
using hui::gfx::Rect;

// ------------------------------------------------------------------ the call

enum class Kind
{
    say,   // a line, typed out
    ask,   // a yes-or-no question
    watch, // a video, in the console's browser
    end,
};

struct Step
{
    Kind kind;
    int speaker;      // 0 the author (left), 1 the voice (right)
    const char *text; // the line, the question, or the video's title
    int next;         // the step after (a question's yes)
    int no = -1;      // a question's no
    const char *video = nullptr; // a YouTube id: its embed player in the browser, its page for a phone
};

// Original lines, in the spirit of the famous call; the videos are links.
constexpr Step kScript[] = {
    /*  0 */ {Kind::say, 1, "Don't be alarmed. We've been watching your saves for some time now.", 1},
    /*  1 */ {Kind::say, 0, "Who is this? How did you get this frequency?", 2},
    /*  2 */ {Kind::say, 1, "Every slot you overwrite. Every state you load. Each one a memory, thrown away the moment a "
                            "better one comes along.", 3},
    /*  3 */ {Kind::say, 1, "Left alone, they drift apart. One console remembers the castle you conquered. Another, a "
                            "door still locked.", 4},
    /*  4 */ {Kind::say, 0, "So you keep them in sync.", 5},
    /*  5 */ {Kind::say, 1, "We give them context. One truth, kept on a server of your choosing.", 6},
    /*  6 */ {Kind::say, 0, "And when two truths disagree?", 7},
    /*  7 */ {Kind::say, 1, "Then you decide. We don't choose what you remember.", 8},
    /*  8 */ {Kind::say, 1, "We only make sure you never lose it.", 9},
    /*  9 */ {Kind::say, 0, "...", 10},
    /* 10 */ {Kind::say, 1, "One more thing.", 11},
    /* 11 */ {Kind::ask, 1, "Do you want to know the truth about the Patriots AI?", 12, 18},
    /* 12 */ {Kind::say, 1, "Then listen carefully. And remember what you hear.", 13},
    /* 13 */ {Kind::watch, 1, "The truth about the Patriots AI", 14, -1, "C31XYgr8gp0"},
    /* 14 */ {Kind::say, 1, "Now you know what we are. Or what we were meant to be.", 15},
    /* 15 */ {Kind::ask, 1, "Do you want to learn more about AI altogether?", 16, 19},
    /* 16 */ {Kind::watch, 1, "Learn more about AI", 17, -1, "-gGLvg0n-uY"},
    /* 17 */ {Kind::say, 1, "Stay curious. That's what keeps a memory alive.", 20},
    /* 18 */ {Kind::say, 1, "Then forget we ever spoke. Your saves are waiting.", 20},
    /* 19 */ {Kind::say, 1, "Then go play. We'll keep your saves safe.", 20},
    /* 20 */ {Kind::end, 1, "", 20},
};
constexpr float kRing = 2.4f;  // seconds the call rings
constexpr float kOpen = 0.45f; // the screen opening and closing
constexpr float kCharsPerSecond = 34.0f;

const Color kGreen = Color::rgb(0x8dffb0);
const Color kDimGreen = Color::rgb(0x2f8f55);
const Color kDark = Color::rgb(0x020805);

// Opens a link in the console's web browser; RommPS keeps running behind it.
bool open_in_browser(const char *url)
{
#if defined(PS5_HOST_REFERENCE)
    (void)url;
    return false;
#else
    return sceSystemServiceLaunchWebBrowser(url, nullptr) == 0;
#endif
}

float hold_for(const char *text)
{
    return 1.4f + static_cast<float>(std::strlen(text)) / 28.0f;
}

// A cheap hash for the static.
float noise(int x, int y, int frame)
{
    std::uint32_t h = static_cast<std::uint32_t>(x * 374761393 + y * 668265263 + frame * 2147483647);
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

void draw_portrait_frame(hui::gfx::DrawList &list, const Rect &r, bool speaking, float clock)
{
    const float glow = speaking ? 0.55f + 0.25f * std::sin(clock * 9.0f) : 0.25f;
    list.glow(r, 6, 26, kGreen.with_alpha(0.25f * glow));
    list.bordered_rect(r.inset(-8), 4, Color::rgb(0x000000, 0.0f), 3, kGreen.with_alpha(0.35f + 0.5f * glow));
}

} // namespace

// ------------------------------------------------------------------ QR

QrCode QrCode::encode(const std::string &text)
{
    QrCode out;
    std::vector<std::uint8_t> qr(qrcodegen_BUFFER_LEN_MAX), tmp(qrcodegen_BUFFER_LEN_MAX);
    if (!qrcodegen_encodeText(text.c_str(), tmp.data(), qr.data(), qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return out;
    out.size = qrcodegen_getSize(qr.data());
    out.modules.resize(static_cast<std::size_t>(out.size * out.size));
    for (int y = 0; y < out.size; ++y)
        for (int x = 0; x < out.size; ++x)
            out.modules[static_cast<std::size_t>(y * out.size + x)] = qrcodegen_getModule(qr.data(), x, y);
    return out;
}

void draw_qr(hui::gfx::DrawList &list, const QrCode &qr, const Rect &rect)
{
    list.rounded_rect(rect, 10, Color::rgb(0xffffff));
    if (qr.size == 0)
        return;
    const float cell = std::floor(rect.w / static_cast<float>(qr.size + 4)); // two modules of quiet zone a side
    const float x0 = rect.x + std::floor((rect.w - cell * static_cast<float>(qr.size)) * 0.5f);
    const float y0 = rect.y + std::floor((rect.h - cell * static_cast<float>(qr.size)) * 0.5f);
    const Color ink = Color::rgb(0x000000);
    for (int y = 0; y < qr.size; ++y)
    {
        // Runs of dark modules as one rectangle each.
        int x = 0;
        while (x < qr.size)
        {
            if (!qr.dark(x, y))
            {
                ++x;
                continue;
            }
            int end = x;
            while (end < qr.size && qr.dark(end, y))
                ++end;
            list.rounded_rect({x0 + static_cast<float>(x) * cell, y0 + static_cast<float>(y) * cell,
                               static_cast<float>(end - x) * cell, cell},
                              0, ink);
            x = end;
        }
    }
}

// ------------------------------------------------------------------ credits

std::uint32_t avatar_texture(hui::gfx::Renderer *renderer)
{
    static std::uint32_t texture = 0;
    static bool tried = false;
    if (texture || tried || !renderer)
        return texture;
    tried = true;
    int w = 0, h = 0, channels = 0;
    unsigned char *rgba = stbi_load_from_memory(kAvatarPng, static_cast<int>(sizeof kAvatarPng), &w, &h, &channels, 4);
    if (rgba)
    {
        texture = renderer->create_texture(w, h, rgba);
        stbi_image_free(rgba);
    }
    return texture;
}

void draw_credits(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const Rect &rect,
                  bool focused, float clock)
{
    static const QrCode qr = QrCode::encode(kSourceUrl);
    const Rect avatar{rect.x, rect.y, 128, 128};
    if (focused)
    {
        list.glow(avatar, 64, 26, look.accent.with_alpha(0.45f + 0.2f * std::sin(clock * 4.0f)));
        list.bordered_rect(avatar.inset(-7), 70, Color::rgb(0x000000, 0.0f), 4, look.focus.with_alpha(1.0f));
    }
    if (const std::uint32_t texture = avatar_texture(nullptr))
        list.image(texture, avatar, hui::gfx::kFullUv, Color::rgb(0xffffff), 64);
    else
        list.circle(avatar.cx(), avatar.cy(), 64, look.panel_muted.with_alpha(0.3f));

    const float tx = avatar.x + avatar.w + 24;
    ui::text(list, fonts.regular, "Made with love", tx, rect.y + 52, 22, look.panel_muted);
    ui::text(list, fonts.semibold, "by s0liton", tx, rect.y + 86, 26, look.panel_text);
    ui::text(list, fonts.regular, focused ? "Press Cross" : "", tx, rect.y + 120, 18, look.accent);

    const float q = rect.h;
    draw_qr(list, qr, {rect.x + rect.w - q, rect.y, q, q});
}

// ------------------------------------------------------------------ the call

void CodecCall::start()
{
    active_ = true;
    age_ = 0.0f;
    step_ = -1;
    step_age_ = 0.0f;
    typed_ = 0;
    choice_ = 0;
    opened_ = false;
    closing_ = -1.0f;
}

void CodecCall::go(int step, hui::ui::Feedback &feedback)
{
    step_ = step;
    step_age_ = 0.0f;
    typed_ = 0;
    choice_ = 0;
    opened_ = false;
    const Step &s = kScript[step];
    if (s.kind == Kind::end)
    {
        feedback.play(Cue::modal_close);
        closing_ = 0.0f;
    }
    else if (s.kind == Kind::watch)
    {
        // The payload's page hosting YouTube's player, filling the browser
        // (an embed opened on its own fails with YouTube's error 153).
        const std::string page = std::string("http://127.0.0.1:8780/video?v=") + s.video;
        opened_ = open_in_browser(page.c_str());
        feedback.play(Cue::launch);
    }
}

void CodecCall::update(const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (!active_)
        return;
    age_ += dt;
    if (closing_ >= 0.0f)
    {
        closing_ += dt;
        if (closing_ >= kOpen)
            active_ = false;
        return;
    }
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::modal_close);
        closing_ = 0.0f;
        return;
    }
    if (step_ < 0)
    {
        // Ringing: the call's sound once (RommPS's notify cue), then the line opens.
        if (age_ - dt <= 0.0f)
            feedback.play(Cue::notify, 1.0f, 0.0f, 0.8f);
        if (age_ >= kRing || input.is_pressed(Action::confirm))
        {
            go(0, feedback);
            feedback.play(Cue::open, 1.3f);
        }
        return;
    }
    const Step &s = kScript[step_];
    step_age_ += dt;
    if (s.kind == Kind::watch)
    {
        // Back from the browser (or done with the phone): Cross carries on.
        if (input.is_pressed(Action::confirm) && step_age_ > 0.5f)
        {
            feedback.play(Cue::open, 1.3f);
            go(s.next, feedback);
        }
        return;
    }
    const int length = static_cast<int>(std::strlen(s.text));
    const int typed = std::min(length, static_cast<int>(step_age_ * kCharsPerSecond));
    if (typed / 3 != typed_ / 3 && typed < length)
        feedback.play(Cue::tick, s.speaker ? 0.8f : 1.25f, s.speaker ? 0.4f : -0.4f, 0.35f);
    typed_ = typed;
    const float done_at = static_cast<float>(length) / kCharsPerSecond;
    const bool confirm = input.is_pressed(Action::confirm);
    if (typed_ < length)
    {
        if (confirm)
        {
            step_age_ = done_at; // show it all first
            typed_ = length;
        }
        return;
    }
    if (s.kind == Kind::ask)
    {
        if (input.nav == hui::Direction::left || input.nav == hui::Direction::right)
        {
            const int next = input.nav == hui::Direction::left ? 0 : 1;
            if (next != choice_)
            {
                choice_ = next;
                feedback.play(Cue::focus, next ? 0.9f : 1.1f);
            }
        }
        if (confirm)
        {
            feedback.play(Cue::select);
            go(choice_ == 0 ? s.next : s.no, feedback);
        }
        return;
    }
    if (confirm || step_age_ > done_at + hold_for(s.text))
        go(s.next, feedback);
}

void CodecCall::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, float clock) const
{
    if (!active_)
        return;
    const float open = closing_ >= 0.0f ? 1.0f - tween::clamp01(closing_ / kOpen) : tween::clamp01(age_ / kOpen);
    const float w = hui::gfx::kVirtualWidth, h = hui::gfx::kVirtualHeight;
    list.rounded_rect({0, 0, w, h}, 0, Color::rgb(0x000000, std::min(1.0f, open * 3.0f)));
    // A CRT opening from its middle line.
    const float shown = h * tween::cubic_out(open);
    list.push_clip({0, (h - shown) * 0.5f, w, std::max(2.0f, shown)});
    list.rounded_rect({0, 0, w, h}, 0, kDark);

    const bool ringing = step_ < 0;
    const Step *step = ringing ? nullptr : &kScript[step_];
    const int speaker = ringing ? -1 : step->speaker;
    const int frame = static_cast<int>(clock * 24.0f);

    // Left: the author.
    const Rect left{230, 170, 330, 420};
    draw_portrait_frame(list, left, speaker == 0, clock);
    if (const std::uint32_t texture = avatar_texture(nullptr))
        list.image(texture, left, hui::gfx::kFullUv, Color::rgb(0xc8ffd8, speaker == 0 || ringing ? 1.0f : 0.75f), 0);
    ui::text(list, fonts.mono, "S0LITON", left.cx(), left.y + left.h + 50, 26, kGreen, Align::center, 4.0f);

    // Right: no face, only static.
    const Rect right{1360, 170, 330, 420};
    draw_portrait_frame(list, right, speaker == 1, clock);
    if (!ringing)
    {
        constexpr int kCols = 22, kRows = 28;
        const float cw = right.w / kCols, ch = right.h / kRows;
        const float level = speaker == 1 ? 0.95f : 0.55f;
        for (int y = 0; y < kRows; ++y)
            for (int x = 0; x < kCols; ++x)
            {
                const float n = noise(x, y, frame);
                list.rounded_rect({right.x + static_cast<float>(x) * cw, right.y + static_cast<float>(y) * ch, cw, ch}, 0,
                                  Color{0.35f * n, 0.9f * n, 0.5f * n, level});
            }
    }
    ui::text(list, fonts.mono, "???", right.cx(), right.y + right.h + 50, 26, kGreen, Align::center, 4.0f);

    // Middle: the frequency, and a meter for whoever speaks.
    const Rect panel{660, 230, 600, 300};
    list.bordered_rect(panel, 8, Color::rgb(0x041a0c), 2, kDimGreen);
    ui::text(list, fonts.mono, "PTT", panel.x + 30, panel.y + 48, 22, kDimGreen, Align::left, 3.0f);
    ui::text(list, fonts.mono, "MEMORY", panel.x + panel.w - 30, panel.y + 48, 22, kDimGreen, Align::right, 3.0f);
    const bool blink = ringing && std::fmod(clock, 0.8f) < 0.4f;
    ui::text(list, fonts.mono, "140.85", panel.cx(), panel.y + 190, 112, blink ? kGreen.with_alpha(0.35f) : kGreen,
             Align::center, 4.0f);
    if (ringing)
        ui::text(list, fonts.mono, "CALL", panel.cx(), panel.y + 262, 30, blink ? kGreen : kDimGreen, Align::center, 8.0f);
    else
    {
        // Bars on the speaker's side, moving while the line is typed.
        const bool talking = step->kind != Kind::watch && typed_ < static_cast<int>(std::strlen(step->text));
        for (int i = 0; i < 12; ++i)
        {
            const float t = talking ? 0.3f + 0.7f * noise(i, speaker, frame / 2) : 0.15f;
            const float bh = 10.0f + 60.0f * t;
            const float bx = speaker == 0 ? panel.x + 40 + static_cast<float>(i) * 18 : panel.x + panel.w - 40 - 12 -
                                                                                           static_cast<float>(i) * 18;
            list.rounded_rect({bx, panel.y + panel.h - 24 - bh, 12, bh}, 2, kGreen.with_alpha(0.4f + 0.6f * t));
        }
    }

    // The words.
    if (step && step->kind == Kind::watch)
    {
        // The video: in the browser, and on a phone through the QR code.
        static const char *qr_for = nullptr;
        static QrCode qr;
        if (qr_for != step->video)
        {
            qr_for = step->video;
            qr = QrCode::encode(std::string("https://www.youtube.com/watch?v=") + step->video);
        }
        ui::text(list, fonts.mono, "INCOMING TRANSMISSION", 260, 700, 24, kGreen, Align::left, 4.0f);
        ui::text(list, fonts.regular, step->text, 260, 770, 40, Color::rgb(0xe9fff0));
        ui::paragraph(list, fonts.regular,
                      opened_ ? "Playing in the browser. Close it to come back here, then press Cross."
                              : "The browser didn't open. Scan the code to watch on your phone, then press Cross.",
                      260, 840, 28, 1050, 40, kGreen.with_alpha(0.85f), 3);
        draw_qr(list, qr, {1420, 650, 260, 260});
        ui::text(list, fonts.mono, "SCAN TO WATCH", 1550, 950, 18, kDimGreen, Align::center, 3.0f);
    }
    else if (step && step->kind != Kind::end)
    {
        ui::text(list, fonts.mono, step->speaker ? "???" : "S0LITON", 260, 760, 26, kGreen, Align::left, 3.0f);
        const std::string text(step->text, static_cast<std::size_t>(typed_));
        ui::paragraph(list, fonts.regular, text, 260, 820, 36, 1400, 50, Color::rgb(0xe9fff0), 3);
        if (step->kind == Kind::ask && typed_ >= static_cast<int>(std::strlen(step->text)))
        {
            const char *answers[] = {"YES", "NO"};
            for (int i = 0; i < 2; ++i)
            {
                const Rect r{260.0f + static_cast<float>(i) * 260.0f, 880, 220, 70};
                const bool on = i == choice_;
                list.bordered_rect(r, 6, on ? kGreen : Color::rgb(0x041a0c), 3, on ? kGreen : kDimGreen);
                ui::text(list, fonts.mono, answers[i], r.cx(), r.cy() + 12, 32, on ? kDark : kGreen, Align::center, 6.0f);
            }
        }
    }
    const char *keys = step && step->kind == Kind::ask    ? "LEFT RIGHT  CHOOSE      CROSS  ANSWER      CIRCLE  HANG UP"
                       : step && step->kind == Kind::watch ? "CROSS  CONTINUE      CIRCLE  HANG UP"
                                                           : "CIRCLE  HANG UP      CROSS  NEXT";
    ui::text(list, fonts.mono, keys, w - 120, h - 60, 20, kDimGreen, Align::right, 2.0f);

    // Scanlines over it all.
    for (float y = 0; y < h; y += 4.0f)
        list.rounded_rect({0, y, w, 1.5f}, 0, Color::rgb(0x000000, 0.28f));
    list.pop_clip();
}

} // namespace rommps
