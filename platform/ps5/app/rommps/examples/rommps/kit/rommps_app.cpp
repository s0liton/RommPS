// RommPS: what the screen shows, kept up to date from the RomM Sync payload.
// See rommps_app.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "rommps_app.hpp"

#include "ui/fonts.hpp"

#include "cJSON.h"
#include "stb_image.h" // implemented by base/VulkanglTFModel.cpp

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <thread>
#include <cctype>
#include <cstdio>
#include <memory>

// The payload's own rules (src/version.c, built in by ps5/CMakeLists.txt), so
// RommPS and RomM Sync agree on which copy is newer: -1, 0 or 1, and "1.4.3"
// beats "1.4.3-dev.12". One that isn't a version ("dev", "") counts as 0.0.0.
extern "C" int version_cmp(const char *a, const char *b);

namespace rommps
{

namespace
{

constexpr float kStatusEvery = 3.0f;
constexpr float kQueueEvery = 1.0f;     // while one is running
constexpr float kQueueIdleEvery = 5.0f; // otherwise
constexpr int kCoverRetries = 3;            // a failed cover is tried again after 15, 30, 60 s
constexpr int kDecodesPerFrame = 2;         // covers turned into textures each frame
constexpr std::size_t kCoverTextures = 360; // covers kept as textures (about 130 MB); the
                                            // least recently drawn go, and come back from
                                            // the payload's cache when drawn again
constexpr std::size_t kListsKept = 24; // game lists kept at once: every tile on the platforms page, and more

void *g_renderer = nullptr;

// RomM's platform logos are .ico files holding a PNG (256x256): bytes and len
// are moved onto the biggest PNG inside, which stb_image reads. Anything else
// is left as it is.
static void ico_png(const unsigned char *&bytes, int &len)
{
    auto u16 = [&](int at) { return bytes[at] | bytes[at + 1] << 8; };
    auto u32 = [&](int at) { return static_cast<unsigned>(u16(at)) | static_cast<unsigned>(u16(at + 2)) << 16; };
    if (len < 6 || u16(0) != 0 || u16(2) != 1)
        return;
    const int n = u16(4);
    unsigned best_size = 0, best_at = 0, best_area = 0;
    for (int i = 0; i < n && 6 + 16 * (i + 1) <= len; ++i)
    {
        const int e = 6 + 16 * i;
        const unsigned w = bytes[e] ? bytes[e] : 256, h = bytes[e + 1] ? bytes[e + 1] : 256;
        const unsigned size = u32(e + 8), at = u32(e + 12);
        if (at + size > static_cast<unsigned>(len) || size < 8 || std::memcmp(bytes + at, "\x89PNG", 4) != 0)
            continue;
        if (w * h > best_area)
            best_area = w * h, best_size = size, best_at = at;
    }
    if (best_size)
        bytes += best_at, len = static_cast<int>(best_size);
}

std::string url_escape(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~' || c == '/')
            out += static_cast<char>(c);
        else
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

const char *str(const cJSON *o, const char *key, const char *fallback = "")
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : fallback;
}

double num(const cJSON *o, const char *key, double fallback = 0)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

// The faces the screen draws text with (set_text_fonts). plain() keeps a
// character only when all of them have it.
const hui::gfx::Font *g_text_fonts[4] = {};

// What ps5/ui/setup-kit.sh bakes into them: the kit's "european" set and its
// symbols. Only asked before the fonts are loaded.
bool baked(unsigned cp)
{
    if ((cp >= 0xA1 && cp <= 0x17F) || (cp >= 0x410 && cp <= 0x44F))
        return true;
    switch (cp)
    {
    case 0x401: case 0x451: case 0x2013: case 0x2014: case 0x2018: case 0x2019: case 0x201C: case 0x201D:
    case 0x201E: case 0x2022: case 0x2026: case 0x20AC: case 0x2122: case 0x2190: case 0x2191: case 0x2192:
    case 0x2193: case 0x2713:
        return true;
    }
    return false;
}

bool drawable(unsigned cp)
{
    if (!g_text_fonts[0])
        return baked(cp);
    for (const hui::gfx::Font *f : g_text_fonts)
        if (f && !f->has_glyph(cp))
            return false;
    return true;
}

void put_utf8(std::string &out, unsigned cp)
{
    if (cp < 0x80)
        out += static_cast<char>(cp);
    else if (cp < 0x800)
        out += static_cast<char>(0xC0 | cp >> 6), out += static_cast<char>(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        out += static_cast<char>(0xE0 | cp >> 12), out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)),
            out += static_cast<char>(0x80 | (cp & 0x3F));
    else
        out += static_cast<char>(0xF0 | cp >> 18), out += static_cast<char>(0x80 | (cp >> 12 & 0x3F)),
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F)), out += static_cast<char>(0x80 | (cp & 0x3F));
}

constexpr unsigned kNothing = 0xFFFFFFFFu;

// The next character of text, advancing p. Bytes that aren't UTF-8 are read
// as Latin-1 (old file names are), a stray continuation byte as nothing; a
// broken sequence never becomes a control character.
unsigned next_char(const unsigned char *&p)
{
    const unsigned lead = *p++;
    if (lead < 0x80)
        return lead;
    const int more = lead >= 0xC2 && lead <= 0xDF ? 1 : lead >= 0xE0 && lead <= 0xEF ? 2 : lead >= 0xF0 && lead <= 0xF4 ? 3 : 0;
    if (!more)
        return lead >= 0xC0 ? lead : kNothing;
    unsigned cp = lead & (0x3F >> more);
    for (int i = 0; i < more; ++i)
    {
        if ((p[i] & 0xC0) != 0x80)
            return lead; // the bytes after it stay, as characters of their own
        cp = cp << 6 | (p[i] & 0x3F);
    }
    p += more;
    // Too long a form, a surrogate, past Unicode: nothing to show
    static const unsigned kLeast[] = {0, 0x80, 0x800, 0x10000};
    if (cp < kLeast[more] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
        return kNothing;
    return cp;
}

// Marks that sit on the letter before them (decomposed text: "e" then U+0301).
bool combining(unsigned cp)
{
    return (cp >= 0x300 && cp <= 0x36F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF) ||
           (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F);
}

// What a name can carry that shows as nothing: control codes, a soft hyphen, zero widths,
// direction marks, variation selectors, the byte order mark.
bool invisible(unsigned cp)
{
    return (cp >= 0x80 && cp < 0xA0) || cp == 0xAD || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x202A && cp <= 0x202E) ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0xFE00 && cp <= 0xFE0F) || cp == 0xFEFF;
}

// A letter and the mark after it, as one character: 0 when there's none.
unsigned compose(char base, unsigned mark)
{
    static const struct
    {
        unsigned mark;
        const char *bases;
        const char16_t *made;
    } kMarks[] = {
        {0x300, "AEIOUaeiou", u"ÀÈÌÒÙàèìòù"},
        {0x301, "AEIOUYaeiouyCcLlNnRrSsZz",
         u"ÁÉÍÓÚÝáéíóúý"
         u"ĆćĹĺŃńŔŕŚśŹź"},
        {0x302, "AEIOUaeiouCcGgHhJjSsWwYy",
         u"ÂÊÎÔÛâêîôû"
         u"ĈĉĜĝĤĥĴĵŜŝŴŵŶŷ"},
        {0x303, "ANOanoIiUu", u"ÃÑÕãñõĨĩŨũ"},
        {0x304, "AEIOUaeiou", u"ĀĒĪŌŪāēīōū"},
        {0x306, "AEGIOUaegiou", u"ĂĔĞĬŎŬăĕğĭŏŭ"},
        {0x307, "CEGIZcegz", u"ĊĖĠİŻċėġż"},
        {0x308, "AEIOUYaeiouy", u"ÄËÏÖÜŸäëïöüÿ"},
        {0x30A, "AaUu", u"ÅåŮů"},
        {0x30B, "OoUu", u"ŐőŰű"},
        {0x30C, "CDELNRSTZcdelnrstz",
         u"ČĎĚĽŇŘŠŤŽčďěľňřšťž"},
        {0x327, "CcGgKkLlNnRrSsTt",
         u"ÇçĢģĶķĻļŅņŖŗŞşŢţ"},
        {0x328, "AEIUaeiu", u"ĄĘĮŲąęįų"},
    };
    for (const auto &m : kMarks)
        if (m.mark == mark)
            for (int i = 0; m.bases[i]; ++i)
                if (m.bases[i] == base)
                    return m.made[i];
    return 0;
}

// Appends what a character the fonts lack reads as, in ASCII; false for one
// with no stand-in (Japanese, say).
bool fold(unsigned cp, std::string &out)
{
    static const char *const kSymbols[] = {// U+00A0 to U+00BF
        " ", "", "c", "GBP", "", "JPY", "|", "S", "", "(C)", "a", "\"", "-", "", "(R)", "",
        "o", "+/-", "2", "3", "'", "u", "", ".", "", "1", "o", "\"", "1/4", "1/2", "3/4", ""};
    static const char kLatin1[] = // U+00C0 to U+00FF; Æ, Þ, ß, æ and þ are below
        "AAAAAAACEEEEIIII" "DNOOOOOxOUUUUYTs" "aaaaaaaceeeeiiii" "dnooooo/ouuuuyty";
    static const char kExtendedA[] = // U+0100 to U+017F; Ĳ, ĳ, Œ and œ are below
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    static const char kPinyin[] = "AaIiOoUuUuUuUuUu"; // U+01CD to U+01DC: Ǎ ǎ Ǐ ǐ ... ǜ
    // Greek and Cyrillic capitals in Latin letters; the small ones are these in lower case
    static const char *const kGreek[] = {// U+0391 to U+03A9 (U+03A2, unused, as final sigma's slot)
        "A", "B", "G", "D", "E", "Z", "I", "Th", "I", "K", "L", "M", "N", "X", "O", "P", "R", "s", "S",
        "T", "Y", "F", "Ch", "Ps", "O"};
    static const char *const kCyrillic[] = {// U+0410 to U+042F
        "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
        "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "", "Y", "", "E", "Yu", "Ya"};
    auto lower = [&out](const char *s) {
        for (; *s; ++s)
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(*s)));
        return true;
    };
    auto put = [&out](const char *s) { return out += s, true; };
    auto one = [&out](char c) { return out += c, true; };
    if (cp >= 0xA0 && cp <= 0xBF)
        return put(kSymbols[cp - 0xA0]);
    if (cp >= 0xC0 && cp <= 0xFF)
        return cp == 0xC6 ? put("AE") : cp == 0xDE ? put("Th") : cp == 0xDF ? put("ss") : cp == 0xE6 ? put("ae")
               : cp == 0xFE ? put("th") : one(kLatin1[cp - 0xC0]);
    if (cp >= 0x100 && cp <= 0x17F)
        return cp == 0x132 ? put("IJ") : cp == 0x133 ? put("ij") : cp == 0x152 ? put("OE") : cp == 0x153 ? put("oe")
               : one(kExtendedA[cp - 0x100]);
    if (cp >= 0x1CD && cp <= 0x1DC)
        return one(kPinyin[cp - 0x1CD]);
    if (cp >= 0x391 && cp <= 0x3A9)
        return put(kGreek[cp - 0x391]);
    if (cp >= 0x3B1 && cp <= 0x3C9)
        return lower(kGreek[cp - 0x3B1]);
    if (cp >= 0x410 && cp <= 0x42F)
        return put(kCyrillic[cp - 0x410]);
    if (cp >= 0x430 && cp <= 0x44F)
        return lower(kCyrillic[cp - 0x430]);
    if (cp >= 0xFF01 && cp <= 0xFF5E) // full-width ASCII
        return one(static_cast<char>(cp - 0xFEE0));
    if ((cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000)
        return one(' ');
    switch (cp)
    {
    case 0x192: return put("f");
    case 0x1A0: return put("O");
    case 0x1A1: return put("o");
    case 0x1AF: return put("U");
    case 0x1B0: return put("u");
    case 0x218: return put("S");
    case 0x219: return put("s");
    case 0x21A: return put("T");
    case 0x21B: return put("t");
    case 0x2BC: return put("'");
    case 0x2C6: return put("^");
    case 0x2DC: return put("~");
    case 0x386: return put("A");
    case 0x388: return put("E");
    case 0x389: case 0x38A: return put("I");
    case 0x38C: case 0x38F: return put("O");
    case 0x38E: return put("Y");
    case 0x390: case 0x3AE: case 0x3AF: case 0x3CA: return put("i");
    case 0x3AC: return put("a");
    case 0x3AD: return put("e");
    case 0x3CB: case 0x3CD: return put("y");
    case 0x3CC: case 0x3CE: return put("o");
    case 0x401: return put("Yo");
    case 0x451: return put("yo");
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return put("-");
    case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032: return put("'");
    case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033: return put("\"");
    case 0x2039: return put("<");
    case 0x203A: return put(">");
    case 0x2020: return put("+");
    case 0x2022: case 0x2605: case 0x2606: return put("*");
    case 0x2024: return put(".");
    case 0x2025: return put("..");
    case 0x2026: return put("...");
    case 0x2030: return put("%");
    case 0x2044: case 0x2215: return put("/");
    case 0x20AC: return put("EUR");
    case 0x2116: return put("No");
    case 0x2122: return put("TM");
    case 0x2190: return put("<-");
    case 0x2192: return put("->");
    }
    return false;
}

// Text the screen's fonts can draw: what they have stays ("Pokémon",
// "Luigi’s Mansion"), anything else folds to its nearest ASCII ("Ō" reads "O",
// "ß" "ss", "™" "TM" in a font without them); a decomposed accent joins its
// letter, or goes. Only a character with no stand-in at all is "?".
std::string plain(const char *text)
{
    std::string out;
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    std::size_t base_at = 0; // where the last plain letter is in out, for a mark after it
    char base = 0;
    while (*p)
    {
        const unsigned cp = next_char(p);
        if (cp == kNothing || invisible(cp))
            continue;
        if (cp < 0x80)
        {
            base = std::isalpha(static_cast<int>(cp)) ? static_cast<char>(cp) : 0;
            base_at = out.size();
            out += static_cast<char>(cp);
            continue;
        }
        if (combining(cp))
        {
            const unsigned made = base ? compose(base, cp) : 0;
            if (made && drawable(made))
            {
                out.resize(base_at);
                put_utf8(out, made);
            }
            base = 0; // a second mark on the same letter goes
            continue;
        }
        base = 0;
        if (cp != 0xA0 && drawable(cp))
        {
            put_utf8(out, cp);
            continue;
        }
        if (!fold(cp, out))
            out += '?';
    }
    return out;
}

bool flag(const cJSON *o, const char *key)
{
    return cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, key));
}

// A cover's colours: its average as the body, a darker tone, and its most
// saturated region's colour as the accent, lifted so it reads on dark glass.
Palette palette_of(const unsigned char *rgba, int w, int h)
{
    double sum[3] = {0, 0, 0}, sat_sum[3] = {0, 0, 0}, sat_weight = 0;
    const int step = std::max(1, (w * h) / 4096);
    int n = 0;
    for (int i = 0; i < w * h; i += step)
    {
        const unsigned char *p = rgba + static_cast<std::size_t>(i) * 4;
        const int mx = std::max({p[0], p[1], p[2]}), mn = std::min({p[0], p[1], p[2]});
        const double sat = mx > 0 ? static_cast<double>(mx - mn) / mx : 0.0;
        for (int c = 0; c < 3; ++c)
        {
            sum[c] += p[c];
            sat_sum[c] += p[c] * sat * sat;
        }
        sat_weight += sat * sat;
        ++n;
    }
    if (n == 0)
        return {};
    auto rgb = [](double r, double g, double b) {
        auto c = [](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, 255.0)); };
        return Color::rgb((c(r) << 16) | (c(g) << 8) | c(b));
    };
    Palette out;
    out.mid = rgb(sum[0] / n, sum[1] / n, sum[2] / n);
    out.dark = rgb(sum[0] / n * 0.35, sum[1] / n * 0.35, sum[2] / n * 0.35);
    if (sat_weight > 0.5)
    {
        double a[3] = {sat_sum[0] / sat_weight, sat_sum[1] / sat_weight, sat_sum[2] / sat_weight};
        const double peak = std::max({a[0], a[1], a[2], 1.0});
        const double lift = 230.0 / peak; // as bright as it gets, same hue
        out.accent = rgb(a[0] * lift, a[1] * lift, a[2] * lift);
    }
    return out;
}

} // namespace

std::string plain_text(const char *text)
{
    return plain(text ? text : "");
}

void set_text_fonts(const hui::ui::Fonts &fonts)
{
    const hui::ui::FontRef *faces[] = {&fonts.regular, &fonts.semibold, &fonts.display, &fonts.mono};
    for (int i = 0; i < 4; ++i)
        g_text_fonts[i] = faces[i]->font;
}

App &app()
{
    static App instance;
    return instance;
}

App::App() : api_("127.0.0.1", payload_port())
{
    refresh_status();
}

void App::say(const std::string &message)
{
    // The console's own notification, through RomM Sync (an app can't send
    // one): "Couldn't cancel: <why>" shows "Couldn't cancel" in bold with the
    // reason above it. Drawn in the app when RomM Sync can't be reached.
    if (status_.reachable)
    {
        std::string text = message;
        if (const std::size_t colon = text.find(": "); colon != std::string::npos)
            text.replace(colon, 2, "\n");
        std::unique_ptr<cJSON, void (*)(cJSON *)> body(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(body.get(), "message", text.c_str());
        char *json = cJSON_PrintUnformatted(body.get());
        api_.post("/api/notify", json ? json : "{}", [this, message](const Response &r) {
            if (r.ok())
                return;
            toast_ = plain(message.c_str()); // an older RomM Sync, or it just stopped
            toast_age_ = 0;
        });
        cJSON_free(json);
        return;
    }
    toast_ = plain(message.c_str()); // the service's errors carry names and paths
    toast_age_ = 0;
}

void App::tick(float dt, hui::gfx::Renderer *renderer)
{
    clock_ += dt;
    api_.poll();
    slow_api_.poll();
    for (Api &a : covers_api_)
        a.poll();
    for (Api &a : pages_api_)
        a.poll();
    // The pages the screen wanted last frame, in the order it asked. One it
    // stopped wanting before a worker was free is never sent.
    for (const auto &[key, page] : wanted_pages_)
        if (pages_in_flight_ < kPageWorkers)
            send_page(key, page);
    wanted_pages_.clear();
    // The covers drawn last frame and not asked for yet, in the order they
    // were drawn; covers scrolled away from are never asked for.
    for (const std::string &path : wanted_)
    {
        if (covers_in_flight_ >= kCoversInFlight)
            break;
        Cover &c = covers_[path];
        if (c.failed && c.retry_at > 0 && clock_ >= c.retry_at)
            c.requested = c.failed = false, c.retry_at = 0; // try again
        if (c.requested)
            continue;
        c.requested = true;
        ++covers_in_flight_;
        Api &worker = covers_api_[next_cover_worker_++ % kCoverWorkers];
        worker.get("/cover?p=" + url_escape(path), [this, path](const Response &r) {
            --covers_in_flight_;
            if (r.ok() && !r.body.empty())
                to_decode_.push_back({path, r.body});
            else
            {
                Cover &c = covers_[path];
                c.failed = true;
                // RomM has no cover: that stays. Anything else (the service
                // busy, a network hiccup): again in a little while.
                const bool again = r.status != 404 && c.retries < kCoverRetries;
                c.retry_at = again ? clock_ + 15.0 * (1 << c.retries++) : 0;
            }
        });
    }
    wanted_.clear();
    toast_age_ += dt;
    if (!toast_.empty() && toast_age_ > 4.0f)
        toast_.clear();

    status_timer_ += dt;
    if (status_timer_ >= kStatusEvery && !status_busy_)
        refresh_status();
    update_timer_ += dt;
    if (update_.busy() && !update_busy_ && update_timer_ >= 1.0f)
        refresh_update();
    queue_timer_ += dt;
    if (status_.paired && !queue_busy_ &&
        queue_timer_ >= (active_queue_ ? kQueueEvery : kQueueIdleEvery))
        refresh_queue();

    if (renderer)
        evict_covers(renderer);
    // The payload sent: its API answers within a few seconds.
    if (start_state_ == 2)
    {
        start_state_ = 0;
        status_timer_ = kStatusEvery;
    }
    else if (start_state_ == 3)
    {
        start_state_ = 0;
        say("Couldn't start RommPS's background service: " + start_error_);
    }

    // A few covers a frame become textures; the rest wait their turn, so a
    // shelf full of new covers never costs a frame.
    for (int i = 0; i < kDecodesPerFrame && !to_decode_.empty(); ++i)
    {
        Pending job = std::move(to_decode_.front());
        to_decode_.pop_front();
        Cover &cover = covers_[job.path];
        int w = 0, h = 0, channels = 0;
        const auto *bytes = reinterpret_cast<const unsigned char *>(job.bytes.data());
        int len = static_cast<int>(job.bytes.size());
        ico_png(bytes, len);
        unsigned char *rgba = stbi_load_from_memory(bytes, len, &w, &h, &channels, 4);
        if (!rgba)
        {
            cover.failed = true;
            continue;
        }
        cover.palette = palette_of(rgba, w, h);
        cover.aspect = h > 0 ? static_cast<float>(w) / static_cast<float>(h) : 0.0f;
        if (shapes_[job.path].aspect != cover.aspect)
            shapes_[job.path] = {cover.aspect, clock_};
        if (renderer)
        {
            if (cover.texture)
                renderer->destroy_texture(cover.texture); // asked for twice: the first one goes
            cover.texture = renderer->create_texture(w, h, rgba);
        }
        stbi_image_free(rgba);
    }
}

void App::evict_covers(hui::gfx::Renderer *renderer)
{
    std::size_t loaded = 0;
    for (const auto &[path, c] : covers_)
        loaded += c.texture != 0;
    if (loaded <= kCoverTextures)
        return;
    // Only covers not drawn for a few seconds, so no list being drawn still has one.
    std::vector<std::pair<double, const std::string *>> old;
    for (const auto &[path, c] : covers_)
        if (c.texture && c.used < clock_ - 3.0)
            old.push_back({c.used, &path});
    std::sort(old.begin(), old.end());
    std::vector<std::string> gone;
    for (std::size_t i = 0; i < old.size() && loaded - gone.size() > kCoverTextures * 3 / 4; ++i)
        gone.push_back(*old[i].second);
    for (const std::string &path : gone)
    {
        renderer->destroy_texture(covers_[path].texture);
        covers_.erase(path);
    }
}

const std::string &App::bundled_payload()
{
    if (!bundled_read_)
    {
        bundled_read_ = true;
        std::ifstream f("/app0/assets/rommps/romm-sync.elf", std::ios::binary);
        bundled_.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    return bundled_;
}

// Both HENs load ELF payloads sent to port 9021; RommPS reaches it on the
// console itself, the way it reaches the payload's API.
void App::start_payload()
{
    if (start_state_ == 1)
        return;
    const std::string &elf = bundled_payload();
    if (elf.size() < 4 || elf.compare(0, 4, "\x7f" "ELF") != 0)
    {
        start_error_ = "this RommPS doesn't carry its background service";
        start_state_ = 3;
        return;
    }
    start_error_.clear();
    start_state_ = 1;
    std::thread([this, elf] {
        std::string err;
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(9021);
        ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
        if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr *>(&sa), sizeof sa) != 0)
            err = std::string("the HEN's payload loader (port 9021) didn't answer: ") + std::strerror(errno);
        for (std::size_t sent = 0; err.empty() && sent < elf.size();)
        {
            const ssize_t n = ::send(fd, elf.data() + sent, elf.size() - sent, 0);
            if (n <= 0)
                err = std::string("sending the payload failed: ") + std::strerror(errno);
            else
                sent += static_cast<std::size_t>(n);
        }
        if (fd >= 0)
            ::close(fd);
        start_error_ = err;
        start_state_ = err.empty() ? 2 : 3;
    }).detach();
}

void App::repair_autostart()
{
    api_.get("/api/autostart", [this](const Response &r) {
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!r.ok() || !j || !flag(j.get(), "supported"))
            return;
        // RommPS needs RomM Sync running, so it starts with the console, from
        // a copy no older than the one RommPS carries.
        bool old = !flag(j.get(), "enabled");
        const cJSON *c;
        cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(j.get(), "copies"))
            old |= version_cmp(str(c, "version"), kMinPayload) < 0;
        if (old)
            install_autostart(nullptr);
    });
}

void App::install_autostart(Api::Callback done)
{
    api_.post("/api/autostart/upload", bundled_payload(), [this, done](const Response &r) {
        if (!r.ok())
            say("Couldn't set up the autostart: " + r.message());
        if (done)
            done(r);
    });
}

// "1.0.2" < "1.1.0" < "1.1.1-dev.4" < "1.1.1". A build without a version
// ("dev") counts as new enough.
bool App::payload_too_old() const
{
    const std::string &v = status_.version;
    if (v.empty() || !std::isdigit(static_cast<unsigned char>(v[0])))
        return false;
    return version_cmp(v.c_str(), kMinPayload) < 0;
}

// RomM Sync holds a lock on this file for as long as it runs (1.1.1 on); a
// copy can be running and not answering yet, after a start or a wake.
int App::payload_lock()
{
    const int fd = ::open("/data/romm-sync/romm-sync.lock", O_RDONLY);
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    int held = 0;
    if (::flock(fd, LOCK_SH | LOCK_NB) != 0)
        held = errno == EWOULDBLOCK ? 1 : -1;
    ::close(fd);
    return held;
}

void App::refresh_status()
{
    status_timer_ = 0;
    status_busy_ = true;
    api_.get("/api/status", [this](const Response &r) {
        status_busy_ = false;
        status_.known = true;
        status_.reachable = r.status != 0;
        // Not answering: RommPS starts the payload it carries, once by itself
        // (Cross on the screen tries again), but only when none is running.
        // One that's running is left to come up; with no way to tell, it
        // waits a few tries first.
        unanswered_ = status_.reachable ? 0 : unanswered_ + 1;
        payload_waiting_ = false;
        if (!status_.reachable && !tried_start_)
        {
            const int lock = payload_lock();
            if (lock == 1)
                payload_waiting_ = true;
            else if (lock == 0 || unanswered_ >= 3)
            {
                tried_start_ = true;
                start_payload();
            }
        }
        if (!r.ok())
        {
            status_.error = plain(r.message().c_str());
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        status_.error.clear();
        status_.version = str(j.get(), "version");
        // An older payload is replaced by the one RommPS carries once it's
        // idle: the new one takes over from it and then, being this
        // RommPS's own, updates the copies on disk (below). A newer one is
        // left alone.
        const bool idle = !flag(cJSON_GetObjectItemCaseSensitive(j.get(), "sync"), "running") && active_queue_ == 0;
        if (payload_too_old() && !updated_payload_ && idle)
        {
            updated_payload_ = true;
            say("Updating RommPS's background service to " + std::string(kMinPayload));
            start_payload();
        }
        else if (!repaired_ && version_cmp(status_.version.c_str(), kMinPayload) == 0)
        {
            repaired_ = true;
            repair_autostart();
        }

        if (!presets_checked_ && !payload_too_old()) {
            presets_checked_ = true;
            api_.post("/api/presets/check", "{}", [](const Response &) {});
        }
        status_.ip = str(j.get(), "ip");
        status_.port = static_cast<int>(num(j.get(), "web_port", 8780));
        status_.paired = flag(j.get(), "paired");
        status_.setup_complete = flag(j.get(), "setup_complete");
        status_.server = str(j.get(), "server_url");
        status_.user = plain(str(j.get(), "username"));
        status_.server_trouble = plain(str(j.get(), "server_trouble"));
        status_.uptime = num(j.get(), "uptime");
        const cJSON *sync = cJSON_GetObjectItemCaseSensitive(j.get(), "sync");
        status_.syncing = flag(sync, "running");
        status_.phase = plain(str(sync, "phase"));
        status_.last_run = num(sync, "last_run");
        status_.sync_count = static_cast<int>(num(sync, "sync_count"));
        status_.tracked_saves = static_cast<int>(num(sync, "tracked_saves"));
        status_.conflicts = cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(sync, "conflicts"));
        // The payload adds each sync at the end of its history: the last one is the newest.
        const cJSON *history = cJSON_GetObjectItemCaseSensitive(sync, "history");
        const cJSON *last = cJSON_GetArrayItem(history, cJSON_GetArraySize(history) - 1);
        status_.last_result = last ? (str(last, "error")[0] ? plain(str(last, "error")) : "OK") : "";
        status_.last_up = static_cast<int>(num(last, "uploaded"));
        status_.last_down = static_cast<int>(num(last, "downloaded"));
        status_.server_version = str(j.get(), "server_version");
        const cJSON *move = cJSON_GetObjectItemCaseSensitive(j.get(), "retroarch_move"), *folder;
        status_.retroarch_move = static_cast<int>(num(move, "games"));
        status_.retroarch_folders.clear();
        cJSON_ArrayForEach(folder, cJSON_GetObjectItemCaseSensitive(move, "folders"))
        {
            const std::string from = str(folder, "from");
            status_.retroarch_folders += (status_.retroarch_folders.empty() ? "" : ", ") + plain(from.substr(from.find_last_of('/') + 1).c_str());
        }
        const cJSON *covers = cJSON_GetObjectItemCaseSensitive(j.get(), "covers");
        status_.covers_state = str(covers, "state");
        status_.covers_done = static_cast<int>(num(covers, "done"));
        status_.covers_total = static_cast<int>(num(covers, "total"));
        // A game came or went (deleted by hand included): the lists load again.
        const double sig = num(j.get(), "installed_sig", -1);
        if (sig != installed_sig_ && installed_sig_ >= 0 && sig >= 0)
        {
            refresh_lists();
            refresh_installed();
        }
        installed_sig_ = sig;
        const cJSON *pairing = cJSON_GetObjectItemCaseSensitive(j.get(), "pairing");
        if (status_.pairing == "starting")
            pairing = nullptr; // the answer to /api/pair/start says how it went
        else
            status_.pairing = str(pairing, "status");
        if (pairing)
        {
            status_.pairing_message = plain(str(pairing, "message"));
            status_.user_code = str(pairing, "user_code");
            status_.verification_url = str(pairing, "verification_url");
        }
        if (!config_.known)
            refresh_config();
        const bool ready = status_.paired && status_.setup_complete;
        if (ready && (!was_paired_ || platforms_.empty()) && !platforms_busy_)
        {
            refresh_platforms();
            refresh_installed();
        }
        // Paired again (as someone else, perhaps): the collections are asked
        // for again when next looked at.
        if (ready && !was_paired_)
        {
            collections_.clear();
            collections_error_.clear();
            collections_loaded_ = false;
        }
        was_paired_ = ready;
    });
}

void App::refresh_platforms()
{
    platforms_busy_ = true;
    api_.get("/api/platforms", [this](const Response &r) {
        platforms_busy_ = false;
        if (!r.ok())
        {
            say("Couldn't load your platforms: " + r.message());
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Platform> list;
        const cJSON *p;
        cJSON_ArrayForEach(p, j.get())
        {
            Platform plat;
            plat.id = static_cast<int>(num(p, "id"));
            plat.name = plain(str(p, "name"));
            plat.slug = str(p, "slug");
            plat.profile = plain(str(p, "profile"));
            plat.rom_count = static_cast<int>(num(p, "rom_count"));
            if (plat.rom_count > 0)
                list.push_back(std::move(plat));
        }
        // Platforms with an emulator first, then by name.
        std::stable_sort(list.begin(), list.end(), [](const Platform &a, const Platform &b) {
            if (a.profile.empty() != b.profile.empty())
                return !a.profile.empty();
            return a.name < b.name;
        });
        platforms_ = std::move(list);
    });
}

void App::refresh_collections()
{
    if (collections_busy_)
        return;
    collections_busy_ = true;
    // Every kind the payload offers, already in order: by kind and type, in
    // each the user's own, then other users' public ones.
    api_.get("/api/collections", [this](const Response &r) {
        collections_busy_ = false;
        collections_loaded_ = true;
        collections_need_pair_ = r.status == 403;
        if (!r.ok())
        {
            collections_error_ = r.message();
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        collections_.clear();
        collections_error_.clear();
        const cJSON *c;
        cJSON_ArrayForEach(c, j.get())
        {
            Collection coll;
            coll.kind = str(c, "kind");
            coll.id = str(c, "id");
            coll.name = plain(str(c, "name"));
            coll.description = plain(str(c, "description"));
            coll.owner = plain(str(c, "owner"));
            coll.type = str(c, "type");
            coll.rom_count = static_cast<int>(num(c, "rom_count"));
            if (coll.rom_count > 0 && !coll.kind.empty() && !coll.id.empty())
                collections_.push_back(std::move(coll));
        }
    });
}

const Platform *App::platform_by_id(int id) const
{
    for (const Platform &p : platforms_)
        if (p.id == id)
            return &p;
    return nullptr;
}

void App::refresh_lists()
{
    for (auto &[key, entry] : lists_)
        if (entry.list)
            std::fill(entry.list->pages.begin(), entry.list->pages.end(), 0);
}

const Collection *App::collection_by_id(const std::string &kind, const std::string &id) const
{
    for (const Collection &c : collections_)
        if (c.kind == kind && c.id == id)
            return &c;
    return nullptr;
}

std::string Scope::query() const
{
    std::string q = "platform_id=" + std::to_string(platform_id);
    if (in_collection())
        q += "&collection_kind=" + url_escape(collection_kind) + "&collection_id=" + url_escape(collection_id);
    return q;
}

GameList &App::games(const Scope &scope, const std::string &search)
{
    const std::string key = scope.key() + "|" + search;
    CachedList &entry = lists_[key];
    if (!entry.list)
    {
        entry.list = std::make_unique<GameList>();
        entry.list->scope = scope;
        entry.list->search = search;
        // Keep a few lists; the oldest goes when there are more.
        if (lists_.size() > kListsKept)
        {
            auto oldest = lists_.end();
            for (auto it = lists_.begin(); it != lists_.end(); ++it)
                if (it->first != key && (oldest == lists_.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest != lists_.end())
                lists_.erase(oldest);
        }
    }
    CachedList &kept = lists_[key];
    kept.used = clock_;
    return *kept.list;
}

void App::ensure(GameList &list, int first, int last)
{
    if (!list.ready())
        last = 0; // the first page says how many there are
    else
        last = list.raw(std::min(last, list.total - 1));
    first = list.ready() ? list.raw(std::max(first, 0)) : 0;
    for (int page = first / GameList::kPage; page <= last / GameList::kPage && last >= 0; ++page)
    {
        if (static_cast<int>(list.pages.size()) <= page)
            list.pages.resize(static_cast<std::size_t>(page) + 1, 0);
        unsigned char &state = list.pages[static_cast<std::size_t>(page)];
        if (state == 4 && clock_ >= list.retry_at)
            state = 0; // the service didn't answer last time: it may now
        if (state)
            continue;
        const std::string key = list.scope.key() + "|" + list.search;
        const std::pair<std::string, int> want{key, page};
        if (std::find(wanted_pages_.begin(), wanted_pages_.end(), want) == wanted_pages_.end())
            wanted_pages_.push_back(want);
    }
}

bool App::retry(GameList &list)
{
    bool any = false;
    for (unsigned char &p : list.pages)
        if (p == 3 || p == 4)
        {
            p = 0;
            any = true;
        }
    if (any)
        list.error.clear();
    return any;
}

void App::send_page(const std::string &key, int page)
{
    auto found = lists_.find(key);
    if (found == lists_.end() || !found->second.list)
        return;
    GameList &list = *found->second.list;
    if (static_cast<int>(list.pages.size()) <= page || list.pages[static_cast<std::size_t>(page)])
        return;
    list.pages[static_cast<std::size_t>(page)] = 1;
    ++pages_in_flight_;
    {
        char path[640];
        std::snprintf(path, sizeof path, "/api/roms?%s&offset=%d&limit=%d&search=%s", list.scope.query().c_str(),
                      page * GameList::kPage, GameList::kPage, url_escape(list.search).c_str());
        pages_api_[next_page_worker_++ % kPageWorkers].get(path, [this, key, page](const Response &r) {
            --pages_in_flight_;
            auto it = lists_.find(key);
            if (it == lists_.end() || !it->second.list)
                return; // let go of meanwhile
            GameList &l = *it->second.list;
            if (!r.ok() && r.status == 0)
            {
                // Nobody answered: the service swapping itself for a newer one
                // (the first open after an update asks for pages in exactly
                // that second), or not up yet. Asked again by itself, after a
                // wait that doubles up to 15 s, and only said after a few.
                const int n = l.unanswered++;
                l.retry_at = clock_ + std::min(15.0, static_cast<double>(1 << std::min(n, 4)));
                if (n >= 3)
                    l.error = plain(r.message().c_str());
                if (static_cast<int>(l.pages.size()) > page)
                    l.pages[static_cast<std::size_t>(page)] = 4;
                return;
            }
            if (!r.ok())
            {
                // An answer, and an error: said once, and asked for again
                // only when the user says so (App::retry). Asking again
                // whenever it was looked at meant asking every frame, which
                // RomM's 500s on franchise collections (Oct 29) did nothing
                // to discourage.
                l.error = plain(r.message().c_str());
                if (static_cast<int>(l.pages.size()) > page)
                    l.pages[static_cast<std::size_t>(page)] = 3;
                return;
            }
            l.unanswered = 0;
            std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
            l.error.clear();
            const int total = static_cast<int>(num(j.get(), "total"));
            if (!l.ready() || total != static_cast<int>(l.games.size()))
            {
                l.games.resize(static_cast<std::size_t>(total));
                l.total = total; // until reindex() below takes out what isn't a game
                l.pages.resize(static_cast<std::size_t>((total + GameList::kPage - 1) / GameList::kPage), 0);
                if (page < static_cast<int>(l.pages.size()))
                    l.pages[static_cast<std::size_t>(page)] = 1;
            }
            if (l.raw_letters.empty())
            {
                const cJSON *ci = cJSON_GetObjectItemCaseSensitive(j.get(), "char_index"), *c;
                cJSON_ArrayForEach(c, ci) if (cJSON_IsNumber(c)) l.raw_letters.push_back({c->string, c->valueint});
                std::sort(l.raw_letters.begin(), l.raw_letters.end(),
                          [](const auto &a, const auto &b) { return a.second < b.second; });
            }
            int index = page * GameList::kPage;
            const cJSON *g;
            cJSON_ArrayForEach(g, cJSON_GetObjectItemCaseSensitive(j.get(), "items"))
            {
                if (index >= total)
                    break;
                Game &game = l.games[static_cast<std::size_t>(index++)];
                game.id = static_cast<int>(num(g, "id"));
                game.platform_id = static_cast<int>(num(g, "platform_id", l.scope.platform_id));
                game.name = plain(str(g, "name"));
                game.fs_name = str(g, "fs_name");
                game.cover = str(g, "cover");
                game.size = num(g, "size");
                game.installed = flag(g, "installed");
                game.multi = flag(g, "multi");
                game.not_game = flag(g, "not_game");
                game.archived = flag(g, "archived");
                game.version = plain(str(g, "version"));
                game.versions = static_cast<int>(num(g, "versions"));
                game.siblings.clear();
                const cJSON *s;
                cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(g, "siblings"))
                    game.siblings.push_back(s->valueint);
            }
            l.reindex();
            if (page < static_cast<int>(l.pages.size()))
                l.pages[static_cast<std::size_t>(page)] = 2;
        });
    }
}

const Details &App::details(int rom_id)
{
    auto found = details_.find(rom_id);
    if (found != details_.end())
        return found->second;
    Details &d = details_[rom_id];
    char path[64];
    std::snprintf(path, sizeof path, "/api/roms/%d/details", rom_id);
    api_.get(path, [this, rom_id](const Response &r) {
        Details &out = details_[rom_id];
        out.loaded = true;
        if (!r.ok())
        {
            out.failed = true;
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        out.year = static_cast<int>(num(j.get(), "year"));
        out.publishers = plain(str(j.get(), "publishers"));
        out.developers = plain(str(j.get(), "developers"));
        out.genres = plain(str(j.get(), "genres"));
        out.players = plain(str(j.get(), "players"));
        out.summary = plain(str(j.get(), "summary"));
        out.status = str(j.get(), "status");
        out.last_played = str(j.get(), "last_played");
        out.rating = num(j.get(), "rating");
        out.main_story_s = num(j.get(), "main_story_s");
        out.play_ms = num(j.get(), "play_ms");
        out.sessions = static_cast<int>(num(j.get(), "sessions"));
        out.completion = static_cast<int>(num(j.get(), "completion"));
        out.my_rating = static_cast<int>(num(j.get(), "my_rating"));
        out.archived = plain(str(j.get(), "archived"));
        out.updates = static_cast<int>(num(j.get(), "updates"));
        out.dlc = static_cast<int>(num(j.get(), "dlc"));
        out.title_id = str(j.get(), "title_id");
        const auto addon = [](const cJSON *a) {
            const char *k = str(a, "kind");
            return Details::Addon{!std::strcmp(k, "update") ? Details::Addon::update
                                  : !std::strcmp(k, "dlc")  ? Details::Addon::dlc
                                                            : Details::Addon::game,
                                  plain(str(a, "name")), str(a, "title_id"), num(a, "size")};
        };
        const cJSON *a;
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j.get(), "addons"))
            out.addons.push_back(addon(a));
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j.get(), "bundle"))
        {
            out.addons.push_back(addon(a));
            out.bundle++;
        }
        cJSON_ArrayForEach(a, cJSON_GetObjectItemCaseSensitive(j.get(), "version_list"))
        {
            Details::Version &v = out.versions.emplace_back();
            v.id = static_cast<int>(num(a, "id"));
            v.name = plain(str(a, "name"));
            v.label = plain(str(a, "label"));
            v.size = num(a, "size");
            const cJSON *disc;
            cJSON_ArrayForEach(disc, cJSON_GetObjectItemCaseSensitive(a, "discs"))
                v.discs.push_back({static_cast<int>(num(disc, "disc")), num(disc, "size")});
        }
        // By name, so a game's discs come in order.
        std::sort(out.versions.begin(), out.versions.end(),
                  [](const Details::Version &a, const Details::Version &b) { return a.name < b.name; });
    });
    return d;
}

void App::refresh_installed()
{
    api_.get("/api/installed?limit=5000", [this](const Response &r) { // every one: the Downloads tab counts them
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Game> list;
        std::unordered_set<int> ids;
        const cJSON *g;
        cJSON_ArrayForEach(g, j.get())
        {
            Game game;
            game.id = static_cast<int>(num(g, "id"));
            game.platform_id = static_cast<int>(num(g, "platform_id"));
            game.name = plain(str(g, "name"));
            game.fs_name = str(g, "fs_name");
            game.cover = str(g, "cover");
            game.size = num(g, "size");
            game.addon = str(g, "addon");
            game.installed = true;
            ids.insert(game.id);
            list.push_back(std::move(game));
        }
        installed_ = std::move(list);
        on_console_ = std::move(ids);
        installed_known_ = true;
    });
}

void App::check_on_console(int rom_id)
{
    char path[64];
    std::snprintf(path, sizeof path, "/api/roms/%d/on_console", rom_id);
    api_.get(path, [this, rom_id](const Response &r) {
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!r.ok() || !j) // an older payload doesn't know it
            return;
        const cJSON *discs = cJSON_GetObjectItemCaseSensitive(j.get(), "discs"), *disc;
        if (cJSON_IsArray(discs))
        {
            std::vector<int> &here = discs_here_[rom_id];
            here.clear();
            cJSON_ArrayForEach(disc, discs)
                here.push_back(disc->valueint);
        }
        else
            discs_here_.erase(rom_id);
        if (!installed_known_)
            return;
        const bool here = flag(j.get(), "installed");
        if (here == (on_console_.count(rom_id) > 0))
            return;
        if (here)
        {
            refresh_installed(); // its size and cover for the shelf
            on_console_.insert(rom_id);
            return;
        }
        on_console_.erase(rom_id);
        installed_.erase(std::remove_if(installed_.begin(), installed_.end(),
                                        [rom_id](const Game &g) { return g.id == rom_id; }),
                         installed_.end());
    });
}

const Cover &App::platform_logo(const std::string &slug)
{
    std::string lower = slug;
    for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return cover(slug.empty() ? std::string() : "/assets/platforms/" + lower + ".ico");
}

const Cover &App::cover(const std::string &path)
{
    static const Cover kNone;
    if (path.empty())
        return kNone;
    Cover &c = covers_[path];
    c.used = clock_;
    // Asked for at the next tick if it's still on screen then; one that
    // failed, again once its retry_at has come. Without the second half a
    // cover that failed once (the service swapping itself on an update, a
    // timeout, a 502) stayed blank until the app was restarted.
    const bool due = c.failed && c.retry_at > 0 && clock_ >= c.retry_at;
    if ((!c.requested || due) && wanted_.size() < 64)
        wanted_.push_back(path);
    return c;
}

float App::cover_aspect(const std::string &path, double *since) const
{
    auto found = shapes_.find(path);
    if (found == shapes_.end())
        return 0.0f;
    if (since)
        *since = found->second.since;
    return found->second.aspect;
}

void App::refresh_queue()
{
    queue_timer_ = 0;
    queue_busy_ = true;
    api_.get("/api/downloads", [this](const Response &r) {
        queue_busy_ = false;
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        std::vector<Download> list;
        int active = 0;
        bool finished_one = false;
        const cJSON *d;
        cJSON_ArrayForEach(d, j.get())
        {
            Download dl;
            dl.id = static_cast<int>(num(d, "id"));
            dl.rom_id = static_cast<int>(num(d, "rom_id"));
            dl.disc = static_cast<int>(num(d, "disc"));
            dl.name = plain(str(d, "name"));
            dl.platform = plain(str(d, "platform"));
            dl.status = str(d, "status");
            dl.error = plain(str(d, "error"));
            dl.done = num(d, "done");
            dl.total = num(d, "total");
            dl.speed = num(d, "speed");
            dl.saves = static_cast<int>(num(d, "saves"));
            dl.states = static_cast<int>(num(d, "states"));
            dl.save_conflicts = static_cast<int>(num(d, "save_conflicts"));
            dl.saves_failed = flag(d, "saves_failed");
            active += dl.active();
            // A game that just finished shows as downloaded on its shelf.
            for (const Download &old : queue_)
                if (old.id == dl.id && old.active() && dl.status == "done")
                    finished_one = true;
            list.push_back(std::move(dl));
        }
        queue_ = std::move(list);
        active_queue_ = active;
        // A game that finished shows as downloaded: the lists load again.
        if (finished_one)
        {
            refresh_lists();
            refresh_installed();
        }
    });
}

std::string Download::saves_note() const
{
    if (saves_failed)
        return "Saves couldn't sync";
    if (save_conflicts)
        return "Save conflict to resolve";
    auto count = [](int n, const char *what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); };
    std::string got;
    if (saves)
        got = count(saves, "save");
    if (states)
        got += (got.empty() ? "" : " and ") + count(states, "state");
    return got.empty() ? "" : got + " synced";
}

bool App::disc_downloading(int rom_id, int disc) const
{
    for (const Download &d : queue_)
        if (d.rom_id == rom_id && (d.disc == 0 || d.disc == disc) && d.active())
            return true;
    return false;
}

const Download *App::download_for(int rom_id, int disc) const
{
    const Download *found = nullptr;
    for (const Download &d : queue_)
        if (d.rom_id == rom_id && d.disc == disc)
            found = &d; // the newest one wins
    return found;
}

const Download *App::download_for(int rom_id) const
{
    const Download *found = nullptr;
    for (const Download &d : queue_)
        if (d.rom_id == rom_id)
            found = &d; // the newest one wins
    return found;
}

void App::download(int rom_id, const std::string &name, int disc)
{
    // The name goes along, so the download shows it from the start.
    std::unique_ptr<cJSON, void (*)(cJSON *)> j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddNumberToObject(j.get(), "rom_id", rom_id);
    if (disc > 0)
        cJSON_AddNumberToObject(j.get(), "disc", disc);
    cJSON_AddStringToObject(j.get(), "name", name.c_str());
    char *text = cJSON_PrintUnformatted(j.get());
    const std::string body = text ? text : "{}";
    cJSON_free(text);
    api_.post("/api/download", body, [this](const Response &r) {
        if (!r.ok())
            say("Couldn't start the download: " + r.message());
        refresh_queue();
    });
}

void App::download_collection(const Collection &c)
{
    std::unique_ptr<cJSON, void (*)(cJSON *)> j(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(j.get(), "collection_kind", c.kind.c_str());
    cJSON_AddStringToObject(j.get(), "collection_id", c.id.c_str());
    char *text = cJSON_PrintUnformatted(j.get());
    const std::string body = text ? text : "{}";
    cJSON_free(text);
    api_.post("/api/download", body, [this, name = c.name](const Response &r) {
        std::unique_ptr<cJSON, void (*)(cJSON *)> res(r.ok() ? r.json() : nullptr, cJSON_Delete);
        const int queued = static_cast<int>(num(res.get(), "queued"));
        if (!r.ok())
            say("Couldn't download " + name + ": " + r.message());
        else if (queued == 0)
            say("Everything in " + name + " that can play here is on this console");
        else
            say("Downloading " + std::to_string(queued) + (queued == 1 ? " game from " : " games from ") + name);
        refresh_queue();
    });
}

void App::clear_finished_queue()
{
    api_.post("/api/downloads/clear", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't clear the list: " + r.message());
        refresh_queue();
    });
}

void App::cancel_download(int job_id)
{
    char body[64];
    std::snprintf(body, sizeof body, "{\"id\":%d}", job_id);
    api_.post("/api/downloads/cancel", body, [this](const Response &r) {
        if (!r.ok())
            say("Couldn't cancel: " + r.message());
        refresh_queue();
    });
}

void App::retry_download(int job_id)
{
    char body[64];
    std::snprintf(body, sizeof body, "{\"id\":%d}", job_id);
    api_.post("/api/downloads/retry", body, [this](const Response &r) {
        if (!r.ok())
            say("Couldn't retry: " + r.message());
        refresh_queue();
    });
}

void App::remove_game(int rom_id, bool quiet, int disc)
{
    char body[64];
    std::snprintf(body, sizeof body, "{\"rom_id\":%d,\"disc\":%d}", rom_id, disc);
    api_.post("/api/delete", body, [this, quiet, rom_id](const Response &r) {
        if (!r.ok())
        {
            say("Couldn't remove the game: " + r.message());
            return;
        }
        if (!quiet)
            say("Game removed. Its saves are kept.");
        check_on_console(rom_id); // its discs, for the versions list
        refresh_lists();
        refresh_installed();
        refresh_queue();
    });
}

void App::sync_now()
{
    api_.post("/api/sync", "{}", [this](const Response &r) {
        say(r.ok() ? "Syncing" : "Couldn't start a sync: " + r.message());
        refresh_status();
    });
}

void App::refresh_config()
{
    api_.get("/api/config", [this](const Response &r) {
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        Config c;
        c.known = true;
        c.server_url = str(j.get(), "server_url");
        c.tls_verify = flag(j.get(), "tls_verify");
        c.sync_interval_min = static_cast<int>(num(j.get(), "sync_interval_min"));
        c.sync_on_game_exit = flag(j.get(), "sync_on_game_exit");
        c.sync_on_game_start = flag(j.get(), "sync_on_game_start");
        c.sync_on_change = flag(j.get(), "sync_on_change");
        c.save_sync = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(j.get(), "save_sync")); // older payloads: on
        c.notify = flag(j.get(), "notify");
        c.update_check = flag(j.get(), "update_check");
        c.states = str(j.get(), "states", "off");
        c.conflict_policy = str(j.get(), "conflict_policy", "ask");
        c.ps2_cards = str(j.get(), "ps2_cards", "per_game");
        c.cover_cache = flag(j.get(), "cover_cache");
        c.download_concurrency = static_cast<int>(num(j.get(), "download_concurrency", 1));
        config_ = c;
    });
}

void App::set_config(const std::string &fields, const std::function<void(Config &)> &apply)
{
    if (apply)
        apply(config_);
    api_.post("/api/config", "{" + fields + "}", [this](const Response &r) {
        if (!r.ok())
        {
            say("Couldn't save that: " + r.message());
            refresh_config();
        }
    });
}

void App::refresh_update()
{
    update_timer_ = 0;
    update_busy_ = true;
    api_.get("/api/update", [this](const Response &r) {
        update_busy_ = false;
        if (!r.ok())
            return;
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        if (!j)
            return;
        const bool was_busy = update_.busy();
        Update u;
        u.known = true;
        u.state = str(j.get(), "state");
        u.message = plain(str(j.get(), "message"));
        u.error = plain(str(j.get(), "error"));
        u.current = str(j.get(), "current");
        u.latest = str(j.get(), "latest");
        u.available = flag(j.get(), "available");
        u.autostart = flag(j.get(), "autostart");
        // Payloads before 1.1.1 don't say: they update themselves.
        const cJSON *su = cJSON_GetObjectItemCaseSensitive(j.get(), "self_update");
        u.self_update = !cJSON_IsBool(su) || cJSON_IsTrue(su);
        u.done = num(j.get(), "done");
        u.total = num(j.get(), "total");
        update_ = u;
        if (was_busy && !u.busy())
        {
            if (u.state == "error")
                say("Update: " + (u.error.empty() ? u.message : u.error));
            else if (u.available)
                say(u.self_update ? "RommPS " + u.latest + " is available"
                                  : "RommPS " + u.latest + " is available in ProsperoStore");
            else if (u.state == "idle")
                say("RommPS is up to date");
            else if (!u.message.empty())
                say(u.message);
        }
    });
}

void App::check_update()
{
    api_.post("/api/update/check", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't look for updates: " + r.message());
        update_.state = "checking";
        refresh_update();
    });
}

void App::install_update()
{
    api_.post("/api/update/install", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't install the update: " + r.message());
        refresh_update();
    });
}

void App::pair_start(const std::string &server_url, bool tls_verify)
{
    std::unique_ptr<cJSON, void (*)(cJSON *)> body(cJSON_CreateObject(), cJSON_Delete);
    cJSON_AddStringToObject(body.get(), "server_url", server_url.c_str());
    char *text = cJSON_PrintUnformatted(body.get());
    const std::string json = text ? text : "{}";
    cJSON_free(text);
    status_.pairing = "starting";
    status_.pairing_message.clear();
    // The certificate setting has to be in place before the server is first contacted.
    set_config(tls_verify ? "\"tls_verify\":true" : "\"tls_verify\":false",
               [tls_verify](Config &c) { c.tls_verify = tls_verify; });
    api_.post("/api/pair/start", json, [this](const Response &r) {
        if (!r.ok())
        {
            status_.pairing = "error";
            status_.pairing_message = plain(r.message().c_str());
            return;
        }
        std::unique_ptr<cJSON, void (*)(cJSON *)> j(r.json(), cJSON_Delete);
        status_.pairing = str(j.get(), "status");
        status_.pairing_message = plain(str(j.get(), "message"));
        status_.user_code = str(j.get(), "user_code");
        status_.verification_url = str(j.get(), "verification_url");
        config_.known = false; // the server's address changed
    });
}

void App::pair_forget()
{
    api_.post("/api/pair/forget", "{}", [this](const Response &) {
        platforms_.clear();
        config_.known = false;
        refresh_status();
    });
}

void App::setup_restart()
{
    api_.post("/api/setup/restart", "{}", [this](const Response &r) {
        if (!r.ok())
            say("Couldn't restart the setup: " + r.message());
        refresh_status();
    });
}

} // namespace rommps

extern "C" void rommps_set_renderer(void *renderer)
{
    rommps::g_renderer = renderer;
}

extern "C" void *rommps_renderer()
{
    return rommps::g_renderer;
}
