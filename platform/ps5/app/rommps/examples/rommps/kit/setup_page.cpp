// RommPS: the setup, on the console. See setup_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_page.hpp"

#include "cJSON.h"
#include "credits.hpp"
#include "touch.hpp"
#include "core/tween.hpp"
#include "ui/motion.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>

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

constexpr float kMargin = 96.0f;
constexpr int kSteps = 5;
const Rect kRows{kMargin - 32, 400, 1150, 540};
const Rect kField{360, 344, 1200, 84};
const Rect kKeys{360, 500, 1200, 400};
const Rect kSide{1300, 400, 524, 470};

using Json = std::unique_ptr<cJSON, void (*)(cJSON *)>;

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

int count(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsArray(v) ? cJSON_GetArraySize(v) : cJSON_IsNumber(v) ? v->valueint : 0;
}

std::string print(cJSON *o)
{
    char *text = cJSON_PrintUnformatted(o);
    std::string out = text ? text : "{}";
    cJSON_free(text);
    return out;
}

// Names for system keys (a platform's first RomM slug): their makers' names, as
// the payload gives RomM's platforms (src/library.c CONSOLE_NAMES) and the web page has them.
std::string system_name(const std::string &key)
{
    static const std::map<std::string, const char *> names = {
        {"3do", "Panasonic 3DO"},
        {"3ds", "Nintendo 3DS"},
        {"amiga", "Commodore Amiga"},
        {"amiga-cd32", "Commodore Amiga CD32"},
        {"arcade", "Arcade"},
        {"atari2600", "Atari 2600"},
        {"atari5200", "Atari 5200"},
        {"atari7800", "Atari 7800"},
        {"c64", "Commodore 64"},
        {"channel-f", "Fairchild Channel F"},
        {"colecovision", "Coleco ColecoVision"},
        {"commodore-cdtv", "Commodore CDTV"},
        {"dc", "Sega Dreamcast"},
        {"dos", "MS-DOS"},
        {"fds", "Nintendo Famicom Disk System"},
        {"gamegear", "Sega Game Gear"},
        {"gb", "Nintendo Game Boy"},
        {"gba", "Nintendo Game Boy Advance"},
        {"gbc", "Nintendo Game Boy Color"},
        {"genesis", "Sega Mega Drive / Genesis"},
        {"jaguar", "Atari Jaguar"},
        {"lynx", "Atari Lynx"},
        {"msx", "MSX"},
        {"n64", "Nintendo 64"},
        {"nds", "Nintendo DS"},
        {"neo-geo-cd", "SNK Neo Geo CD"},
        {"neo-geo-pocket", "SNK Neo Geo Pocket"},
        {"neo-geo-pocket-color", "SNK Neo Geo Pocket Color"},
        {"neogeoaes", "SNK Neo Geo AES"},
        {"neogeomvs", "SNK Neo Geo MVS"},
        {"nes", "Nintendo Entertainment System"},
        {"ngc", "Nintendo GameCube"},
        {"pc-fx", "NEC PC-FX"},
        {"philips-cd-i", "Philips CD-i"},
        {"pokemon-mini", "Nintendo Pokémon Mini"},
        {"ps2", "Sony PlayStation 2"},
        {"ps3", "Sony PlayStation 3"},
        {"psp", "Sony PlayStation Portable"},
        {"psvita", "Sony PlayStation Vita"},
        {"psx", "Sony PlayStation"},
        {"satellaview", "Nintendo Satellaview"},
        {"saturn", "Sega Saturn"},
        {"scummvm", "ScummVM"},
        {"sega-pico", "Sega Pico"},
        {"sega32", "Sega 32X"},
        {"segacd", "Sega CD / Mega-CD"},
        {"sg1000", "Sega SG-1000"},
        {"sms", "Sega Master System"},
        {"snes", "Super Nintendo Entertainment System"},
        {"sufami-turbo", "Bandai SuFami Turbo"},
        {"supergrafx", "NEC SuperGrafx"},
        {"switch", "Nintendo Switch"},
        {"tg16", "NEC PC Engine / TurboGrafx-16"},
        {"turbografx-cd", "NEC PC Engine CD / TurboGrafx-CD"},
        {"virtualboy", "Nintendo Virtual Boy"},
        {"wii", "Nintendo Wii"},
        {"wiiu", "Nintendo Wii U"},
        {"wonderswan", "Bandai WonderSwan"},
        {"wonderswan-color", "Bandai WonderSwan Color"},
        {"xbox", "Microsoft Xbox"},
        {"xbox360", "Microsoft Xbox 360"},
        {"zxs", "Sinclair ZX Spectrum"},
    };
    const auto it = names.find(key);
    if (it != names.end())
        return it->second;
    // Any other: its key, readable ("neo-geo" -> "Neo geo").
    std::string out = key;
    for (char &ch : out)
        if (ch == '-' || ch == '_')
            ch = ' ';
    if (!out.empty() && out[0] >= 'a' && out[0] <= 'z')
        out[0] = static_cast<char>(out[0] - 32);
    return out;
}

bool at_least(const std::string &version, int major, int minor, int patch)
{
    int v[3] = {0, 0, 0};
    std::sscanf(version.c_str(), "%d.%d.%d", &v[0], &v[1], &v[2]);
    const int want[3] = {major, minor, patch};
    for (int i = 0; i < 3; ++i)
        if (v[i] != want[i])
            return v[i] > want[i];
    return true;
}

// The end of a text that fits a width, for a field that shows where typing is.
std::string tail(const hui::gfx::Font &font, const std::string &text, float size, float width)
{
    // A character at a time: a cut inside one would draw as "?"
    std::size_t from = 0;
    while (from < text.size() && font.measure(std::string_view(text).substr(from), size) > width)
        from = char_boundary(text, from + 1);
    return from ? "\xE2\x80\xA6" + text.substr(char_boundary(text, from + 1)) : text;
}

hui::ui::KeyboardLayout url_layout()
{
    using hui::ui::KeyKind;
    using hui::ui::KeyboardKey;
    hui::ui::KeyboardLayout l;
    l.name = "abc";
    l.columns = 11;
    l.add_row("1234567890-");
    l.add_row("qwertyuiop/");
    l.add_row("asdfghjkl:.");
    std::vector<KeyboardKey> letters{{KeyKind::shift, "", "", 2, ""}};
    for (char c : std::string("zxcvbnm_"))
        letters.push_back({KeyKind::character, std::string(1, c),
                           std::string(1, c == '_' ? c : static_cast<char>(c - 'a' + 'A')), 1, ""});
    letters.push_back({KeyKind::backspace, "", "", 1, ""});
    l.add_row(letters);
    l.add_row(std::vector<KeyboardKey>{{KeyKind::character, "http://", "", 3, ""},
                                       {KeyKind::character, "https://", "", 3, ""},
                                       {KeyKind::character, ":8080", "", 2, ""},
                                       {KeyKind::layout, "", "", 1, ""},
                                       {KeyKind::done, "", "", 2, "Connect"}});
    return l;
}

} // namespace

SetupPage::SetupPage()
{
    keys_.set_layouts({url_layout(), hui::ui::KeyboardLayout::symbols()});
    keys_.style.bindings.backspace = Action::west;
    keys_.style.bindings.shift = Action::jump_prev;
    keys_.style.bindings.done = Action::jump_next;
    keys_.style.max_length = 200;
    keys_.style.done_label = "Connect";
    keys_.on_text = [this](std::string_view text) {
        url_ += text;
        url_known_ = true;
        error_.clear();
    };
    keys_.on_backspace = [this] {
        if (!url_.empty())
            url_.pop_back();
        url_known_ = true;
    };
    keys_.set_bounds(kKeys);
    keys_.enter();
}

void SetupPage::edit_emulators()
{
    editing_ = true;
    leaving_ = false;
    saved_.clear();
    go(kEmulators);
}

void SetupPage::enter()
{
    editing_ = false;
    go(kServer);
    error_.clear();
    url_known_ = false;
}

void SetupPage::go(Step step)
{
    step_ = step;
    step_age_ = 0.0f;
    rows_.set_focus(0);
    busy_ = false;
    switch (step)
    {
    case kServer:
        keys_.enter();
        break;
    case kEmulators:
        emulators_loaded_ = false;
        error_.clear();
        break;
    case kPreview:
        preview_ = Preview{};
        backed_up_ = false;
        error_.clear();
        break;
    case kFinish:
        finish_known_ = false;
        error_.clear();
        break;
    default:
        break;
    }
}

void SetupPage::follow_status(App &app)
{
    if (editing_)
        return; // Settings' Emulators: the setup is done, its steps don't follow the status
    const Status &s = app.status();
    if (!s.paired)
    {
        if (s.pairing == "pending")
        {
            busy_ = false;
            if (step_ != kApprove)
                go(kApprove);
        }
        else if (s.pairing == "error")
        {
            if (busy_ || step_ == kApprove)
            {
                error_ = s.pairing_message.empty() ? "RomM didn't accept the pairing" : s.pairing_message;
                go(kServer);
            }
        }
        else if (s.pairing != "starting" && step_ != kServer)
            go(kServer);
        return;
    }
    if (step_ < kEmulators)
    {
        if (step_ == kApprove)
            app.say("Paired with RomM");
        go(kEmulators);
    }
}

void SetupPage::update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    step_age_ += dt;
    if (step_ == kEmulators && emulators_loaded_)
    {
        auto logo = [&](const std::string &key) {
            const Cover &c = app.platform_logo(key);
            logos_[key] = {c.texture, c.aspect > 0 ? c.aspect : 1.0f};
        };
        for (const Conflict &c : systems())
            logo(c.key);
        for (const auto &[key, o] : own_)
            logo(key);
    }
    follow_status(app);
    switch (step_)
    {
    case kServer:
        update_server(app, input, dt, feedback);
        break;
    case kApprove:
        if (input.is_pressed(Action::back))
        {
            feedback.play(Cue::back);
            app.pair_forget();
            go(kServer);
        }
        break;
    case kEmulators:
        if (picking_)
            update_picker(app, input, dt, feedback);
        else
            update_emulators(app, input, dt, feedback);
        break;
    case kPreview:
        update_preview(app, input, dt, feedback);
        break;
    case kFinish:
        update_finish(app, input, dt, feedback);
        break;
    }
}

void SetupPage::update_server(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    if (!url_known_ && app.config().known)
    {
        url_ = app.config().server_url.empty() ? "http://" : app.config().server_url;
        tls_verify_ = app.config().tls_verify;
        url_known_ = true;
    }
    if (url_.empty() && !url_known_)
        url_ = "http://";
    keys_.style.theme = *look(current_look()).theme;
    keys_.style.reduced_motion = false;
    if (!busy_)
    {
        input = touch::with(input, touch::take(&tls_verify_), Action::north); // the TLS line touched
        input = touch::type(keys_, input);
        if (input.is_pressed(Action::north))
        {
            tls_verify_ = !tls_verify_;
            feedback.play(Cue::toggle);
        }
        keys_.set_length(static_cast<int>(url_.size()));
        if (keys_.handle(input, feedback) == hui::ui::Event::activated)
        {
            while (!url_.empty() && url_.back() == ' ')
                url_.pop_back();
            if (url_.empty() || url_ == "http://" || url_ == "https://")
            {
                error_ = "Type your RomM server's address first";
                feedback.play(Cue::error);
            }
            else
            {
                error_.clear();
                busy_ = true;
                app.pair_start(url_, tls_verify_);
                feedback.play(Cue::launch);
            }
        }
    }
    keys_.update(dt);
}

// ------------------------------------------------------------------ emulators

void SetupPage::load_mounts(App &app)
{
    app.get("/api/paths", [this](const Response &r) {
        if (!r.ok())
            return;
        Json j(r.json(), cJSON_Delete);
        mounts_.clear();
        cards_.clear();
        const cJSON *m;
        cJSON_ArrayForEach(m, j.get())
        {
            if (const cJSON *c = cJSON_GetObjectItemCaseSensitive(m, "cards"))
            {
                CardMethod &cm = cards_[str(m, "system")];
                cm.method = str(c, "method");
                if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "active")))
                    cm.note = std::string(str(m, "profile")) + ": " + str(m, "sync_note");
            }
            if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "extra")))
                continue;
            MountStatus &st = mounts_[std::string(str(m, "system")) + "|" + str(m, "root")];
            st.mounted = *str(m, "mount") != 0;
            st.note = str(m, "mount_note");
            st.own = static_cast<int>(num(m, "own_games"));
        }
    });
}

void SetupPage::load_emulators(App &app)
{
    busy_ = true;
    App *a = &app;
    load_mounts(app);
    // The server's systems, for those no emulator here plays.
    app.get("/api/platforms", [this](const Response &r) {
        if (!r.ok())
            return;
        Json j(r.json(), cJSON_Delete);
        own_order_.clear();
        const cJSON *p;
        cJSON_ArrayForEach(p, j.get())
        {
            if (num(p, "rom_count") <= 0)
                continue;
            const std::string slug = str(p, "slug"), fs_slug = str(p, "fs_slug");
            if (slug.empty())
                continue;
            Own &o = own_[slug];
            o.slugs = {slug};
            if (!fs_slug.empty() && fs_slug != slug)
                o.slugs.push_back(fs_slug);
            o.name = str(p, "display_name", str(p, "name", slug.c_str()));
            o.games = static_cast<int>(num(p, "rom_count"));
            own_order_.push_back(slug);
        }
    });
    app.get("/api/setup/detect", [this, a](const Response &r) {
        emulators_.clear();
        if (!r.ok())
        {
            busy_ = false;
            emulators_loaded_ = true;
            error_ = r.message();
            return;
        }
        error_.clear();
        Json j(r.json(), cJSON_Delete);
        manual_.clear();
        const cJSON *c;
        cJSON_ArrayForEach(c, j.get())
        {
            Emulator e;
            e.root = str(c, "root");
            e.name = str(c, "name");
            e.kind = str(c, "kind");
            e.id = str(c, "id");
            e.ready = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(c, "ready"));
            e.note = str(c, "note");
            e.on = e.ready;
            const cJSON *core;
            cJSON_ArrayForEach(core, cJSON_GetObjectItemCaseSensitive(c, "cores")) if (cJSON_IsString(core))
                e.cores.push_back(core->valuestring);
            const cJSON *d;
            cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(c, "dirs"))
            {
                e.dirs[d->string] = {str(d, "rom_dir"), str(d, "save_dir"), str(d, "state_dir")};
                e.games[d->string] = static_cast<int>(num(d, "games"));
            }
            const cJSON *fx;
            cJSON_ArrayForEach(fx, cJSON_GetObjectItemCaseSensitive(c, "fixed")) if (cJSON_IsString(fx))
                e.fixed.insert(fx->valuestring);
            const cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(c, "profile"), "platforms"))
            {
                const cJSON *key = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "romm"), 0);
                if (!cJSON_IsString(key))
                    continue;
                const std::string core_name = str(p, "core_name");
                System sy{key->valuestring, e.name + (core_name.empty() ? "" : " (" + core_name + ")"), {}};
                const cJSON *slug;
                cJSON_ArrayForEach(slug, cJSON_GetObjectItemCaseSensitive(p, "romm")) if (cJSON_IsString(slug))
                    sy.slugs.push_back(slug->valuestring);
                // A platform entry's key wins over its profile's, as in the payload.
                const char *layout = cJSON_GetObjectItemCaseSensitive(p, "save_layout")
                                         ? str(p, "save_layout")
                                         : str(cJSON_GetObjectItemCaseSensitive(c, "profile"), "save_layout");
                if (sy.key == "ps2" && std::string(layout) == "ps2")
                    e.ps2_game_saves = true;
                e.systems.push_back(std::move(sy));
            }
            // No preset: each of its systems is set up with folders the user picks.
            if (!e.ready)
                for (const System &sy : e.systems)
                {
                    const std::string key = e.id + "-" + sy.key;
                    Own &o = own_[key];
                    o.slugs = sy.slugs;
                    o.name = e.name + " (" + system_name(sy.key) + ")";
                    o.id = key;
                    if (std::find(manual_.begin(), manual_.end(), key) == manual_.end())
                        manual_.push_back(key);
                }
            emulators_.push_back(std::move(e));
        }
        // Start from the emulators set up now (Settings, or setting up again);
        // a first setup starts from what was found.
        a->get("/api/config", [this](const Response &r2) {
            busy_ = false;
            emulators_loaded_ = true;
            Json cfg(r2.json(), cJSON_Delete);
            const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(cfg.get(), "profiles"), *p;
            if (!cJSON_IsArray(profiles) || (!editing_ && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cfg.get(), "profiles_custom"))))
                return;
            {
                // "" follows the emulator; 1.2 saved "backup" for shared.
                const std::string v = str(cfg.get(), "ps2_cards");
                ps2_cards_ = v == "backup" || v == "shared" ? "shared" : v == "per_game" ? "per_game" : "";
            }
            choices_.clear();
            also_.clear();
            custom_dirs_.clear();
            sync_.clear();
            expanded_.clear();
            // Folders picked before: custom-<slug> profiles for systems, and
            // custom-<emulator>-<system> for emulators with no preset.
            cJSON_ArrayForEach(p, profiles)
            {
                const std::string id = str(p, "id");
                if (id.rfind("custom-", 0) != 0)
                    continue;
                const cJSON *plat = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(p, "platforms"), 0);
                const cJSON *slug = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(plat, "romm"), 0);
                if (!cJSON_IsString(slug))
                    continue;
                const std::string key = own_.count(id.substr(7)) ? id.substr(7) : std::string(slug->valuestring);
                Own &o = own_[key];
                o.on = true;
                o.folder[0] = str(p, "rom_dir");
                o.folder[1] = str(p, "save_dir");
                o.folder[2] = str(p, "state_dir");
            }
            for (Emulator &e : emulators_)
            {
                const cJSON *mine = nullptr;
                cJSON_ArrayForEach(p, profiles) if (e.root == str(p, "root") &&
                                                    !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(p, "enabled")))
                    mine = p;
                e.on = e.ready && mine != nullptr;
                if (!mine)
                    continue;
                e.sync = *str(mine, "sync") ? str(mine, "sync") : "all";
                auto listed = [&](const char *list, const std::string &key) {
                    const cJSON *x;
                    cJSON_ArrayForEach(x, cJSON_GetObjectItemCaseSensitive(mine, list)) if (cJSON_IsString(x) &&
                                                                                          key == x->valuestring) return true;
                    return false;
                };
                for (const System &sy : e.systems)
                {
                    if (listed("extra", sy.key))
                        also_[sy.key].insert(e.root);
                    else if (!listed("exclude", sy.key))
                    {
                        choices_[sy.key] = e.root;
                        // Its save sync: on its platform entry, or (before 1.2) the emulator's.
                        std::string mode = *str(mine, "sync") ? str(mine, "sync") : "all";
                        const cJSON *pe;
                        cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(mine, "platforms"))
                        {
                            const cJSON *k = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(pe, "romm"), 0);
                            if (cJSON_IsString(k) && sy.key == k->valuestring && *str(pe, "sync"))
                                mode = str(pe, "sync");
                        }
                        sync_[sy.key] = mode;
                    }
                }
                // Folders picked for a system: set on its platform entry.
                const cJSON *pe;
                cJSON_ArrayForEach(pe, cJSON_GetObjectItemCaseSensitive(mine, "platforms"))
                {
                    const cJSON *key = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(pe, "romm"), 0);
                    if (!cJSON_IsString(key))
                        continue;
                    static const char *const kDirs[] = {"rom_dir", "save_dir", "state_dir"};
                    std::array<std::string, 3> picked;
                    bool any = false;
                    for (int k = 0; k < 3; ++k)
                    {
                        // A template ("{root}/...") is the preset's own, not a pick.
                        const std::string v = str(pe, kDirs[k]);
                        picked[static_cast<std::size_t>(k)] = v.find('{') == std::string::npos ? v : "";
                        any = any || !picked[static_cast<std::size_t>(k)].empty();
                    }
                    if (any)
                        custom_dirs_[key->valuestring][e.root] = picked;
                }
            }
            // A system none of them plays now is off.
            for (const Conflict &c : systems())
                if (!choices_.count(c.key))
                    choices_[c.key] = "";
            saved_ = save_body() + ps2_cards_;
        });
    });
}

std::vector<SetupPage::Conflict> SetupPage::conflicts() const
{
    std::vector<Conflict> out = systems();
    out.erase(std::remove_if(out.begin(), out.end(), [](const Conflict &c) { return c.options.size() < 2; }), out.end());
    return out;
}

std::vector<SetupPage::Conflict> SetupPage::systems() const
{
    std::vector<Conflict> out;
    for (const Emulator &e : emulators_)
    {
        if (!e.ready)
            continue;
        for (const System &s : e.systems)
        {
            auto it = std::find_if(out.begin(), out.end(), [&](const Conflict &c) { return c.key == s.key; });
            if (it == out.end())
            {
                out.push_back({s.key, {}});
                it = out.end() - 1;
            }
            const bool listed = std::any_of(it->options.begin(), it->options.end(),
                                            [&](const Option &o) { return o.root == e.root; });
            if (!listed)
                it->options.push_back({e.root, s.label, e.kind == "retroarch"});
        }
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const Conflict &a, const Conflict &b) { return system_name(a.key) < system_name(b.key); });
    return out;
}

std::string SetupPage::chosen(const Conflict &c) const
{
    const auto picked = choices_.find(c.key);
    if (picked != choices_.end())
    {
        if (picked->second.empty())
            return ""; // turned off
        for (const Option &o : c.options)
            if (o.root == picked->second)
                return o.root;
    }
    // Not picked yet: the emulator the platform's games are already in (most
    // of them), else RetroArch, else the first found.
    const Option *best = nullptr;
    int most = 0;
    for (const Option &o : c.options)
        for (const Emulator &e : emulators_)
            if (e.root == o.root && e.games.count(c.key))
            {
                const int n = e.games.at(c.key);
                if (n > most || (n == most && n > 0 && o.retroarch))
                    best = &o, most = n;
            }
    if (best)
        return best->root;
    for (const Option &o : c.options)
        if (o.retroarch)
            return o.root;
    return c.options.front().root;
}

bool SetupPage::used(const Emulator &e) const
{
    if (!e.ready)
        return false;
    for (const Conflict &c : systems())
    {
        if (chosen(c) == e.root)
            return true;
        const auto set = also_.find(c.key);
        if (!chosen(c).empty() && set != also_.end() && set->second.count(e.root))
            for (const Option &o : c.options)
                if (o.root == e.root)
                    return true;
    }
    return false;
}

bool SetupPage::has_ps2() const
{
    for (const Emulator &e : emulators_)
        if (used(e) && (e.ps2_game_saves || std::find(e.cores.begin(), e.cores.end(), "LRPS2") != e.cores.end()))
            return true;
    return false;
}

bool SetupPage::has_ps2_game_saves() const
{
    for (const Emulator &e : emulators_)
        if (used(e) && e.ps2_game_saves)
            return true;
    return false;
}

std::vector<std::string> SetupPage::uncovered() const
{
    std::vector<std::string> out;
    for (const std::string &key : own_order_)
    {
        const Own &o = own_.at(key);
        bool covered = false;
        for (const Emulator &e : emulators_)
            for (const System &sy : e.systems)
                for (const std::string &slug : sy.slugs)
                    if (e.ready && std::find(o.slugs.begin(), o.slugs.end(), slug) != o.slugs.end())
                        covered = true;
        if (!covered)
            out.push_back(key);
    }
    return out;
}

void SetupPage::open_picker(App &app, const std::string &key, int which, const std::string &root)
{
    picking_ = true;
    pick_key_ = key;
    pick_which_ = which;
    pick_root_ = root;
    // Start where the folder is, or where the games are, or at the drives.
    std::string start;
    if (root.empty())
    {
        const Own &o = own_[key];
        start = !o.folder[which].empty() ? o.folder[which] : o.folder[0];
    }
    else
    {
        const std::array<std::string, 3> &d = custom_dirs_[key][root];
        start = !d[static_cast<std::size_t>(which)].empty() ? d[static_cast<std::size_t>(which)] : d[0];
        if (start.empty())
            for (const Emulator &e : emulators_)
                if (e.root == root && e.dirs.count(key))
                    start = e.dirs.at(key)[static_cast<std::size_t>(which)];
    }
    browse(app, start);
}

void SetupPage::browse(App &app, const std::string &path)
{
    fs_loading_ = true;
    fs_error_.clear();
    std::string q;
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : path)
    {
        if (isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.')
            q += static_cast<char>(c);
        else
        {
            q += '%';
            q += hex[c >> 4];
            q += hex[c & 15];
        }
    }
    const std::string asked = path;
    App *a = &app;
    app.get("/api/fs?path=" + q, [this, a, asked](const Response &r) {
        fs_loading_ = false;
        if (!r.ok())
        {
            // A folder that's gone: back to the drives.
            if (!asked.empty())
                browse(*a, "");
            else
                fs_error_ = r.message();
            return;
        }
        Json j(r.json(), cJSON_Delete);
        fs_path_ = str(j.get(), "path");
        fs_parent_ = str(j.get(), "parent");
        fs_folders_.clear();
        const cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItemCaseSensitive(j.get(), "folders")) if (cJSON_IsString(f))
            fs_folders_.push_back(f->valuestring);
        pick_rows_.set_focus(0);
    });
}

void SetupPage::update_picker(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    std::vector<Row> rows;
    const bool at_drives = fs_path_.empty();
    if (!at_drives)
    {
        rows.push_back({RowKind::action, "Use this folder", "", fs_path_});
        rows.push_back({RowKind::action, "Up", "", fs_parent_.empty() ? "The drives" : fs_parent_});
    }
    for (const std::string &f : fs_folders_)
        rows.push_back({RowKind::action, f, "", ""});
    input = pick_rows_.touch(input, rows);
    const RowEvent e = pick_rows_.handle(rows, input, feedback);
    pick_rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        picking_ = false;
        return;
    }
    if (e.row < 0 || fs_loading_)
        return;
    int i = e.row;
    if (!at_drives)
    {
        if (i == 0)
        {
            if (pick_root_.empty())
                own_[pick_key_].folder[pick_which_] = fs_path_;
            else
                custom_dirs_[pick_key_][pick_root_][static_cast<std::size_t>(pick_which_)] = fs_path_;
            picking_ = false;
            return;
        }
        if (i == 1)
        {
            browse(app, fs_parent_);
            return;
        }
        i -= 2;
    }
    if (i >= 0 && i < static_cast<int>(fs_folders_.size()))
        browse(app, at_drives ? fs_folders_[static_cast<std::size_t>(i)]
                              : fs_path_ + "/" + fs_folders_[static_cast<std::size_t>(i)]);
}

void SetupPage::draw_picker(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock) const
{
    static const char *const kWhat[] = {"games", "saves", "save states"};
    const auto it = own_.find(pick_key_);
    std::string name = it != own_.end() ? it->second.name : system_name(pick_key_);
    for (const Emulator &e : emulators_)
        if (!pick_root_.empty() && e.root == pick_root_)
            name = system_name(pick_key_) + " in " + e.name;
    char title[160];
    std::snprintf(title, sizeof title, "Choose the %s folder", kWhat[pick_which_]);
    draw_heading(list, fonts, look, title,
                 "For " + name + (fs_path_.empty() ? ": pick a drive, then the folder." : ": " + fs_path_));
    std::vector<Row> rows;
    if (!fs_path_.empty())
    {
        rows.push_back({RowKind::action, "Use this folder", "", fs_path_});
        rows.push_back({RowKind::action, "Up", "", fs_parent_.empty() ? "The drives" : fs_parent_});
    }
    for (const std::string &f : fs_folders_)
        rows.push_back({RowKind::action, f, "", ""});
    if (rows.empty())
        rows.push_back({RowKind::info, fs_loading_ ? "Looking" : fs_error_.empty() ? "No folders here" : fs_error_, "", ""});
    pick_rows_.draw(list, fonts, look, rows, kRows, clock);
}

std::vector<Row> SetupPage::emulator_rows(const App &app, std::vector<RowRef> *refs) const
{
    std::vector<Row> rows;
    // Save sync off: only the games folders matter. In the first setup it's on until the question is answered.
    const bool saves = editing_ ? app.config().save_sync : save_sync_;
    auto add = [&](Row row, RowRef ref) {
        rows.push_back(std::move(row));
        if (refs)
            refs->push_back(std::move(ref));
    };
    auto shorten = [](std::string value) {
        if (value.size() > 34)
            value = "\xE2\x80\xA6" + value.substr(char_boundary(value, value.size() - 33));
        return value;
    };
    // Something set up by its folders: a switch, then its three folders when on.
    auto add_own = [&](const std::string &key, const std::string &detail) {
        const auto it = own_.find(key);
        if (it == own_.end())
            return;
        const Own &o = it->second;
        add({RowKind::toggle, o.name, "", detail, o.on}, {RowRef::kOwn, -1, key});
        if (!o.on)
            return;
        static const char *const kLabels[] = {"   Games folder", "   Saves folder", "   States folder"};
        for (int k = 0; k < 3; ++k)
        {
            std::string value = shorten(o.folder[k]);
            if (value.empty())
                value = k == 0 ? "Choose" : "Same as games";
            add({RowKind::action, kLabels[k], value, ""}, {RowRef::kOwnFolder, k, key});
        }
    };
    if (!emulators_loaded_)
    {
        add({RowKind::info, "Looking for emulators", "", ""}, {});
        return rows;
    }
    if (!error_.empty())
        add({RowKind::info, "Couldn't look for emulators", "", error_}, {});
    else if (emulators_.empty())
        add({RowKind::info, "No emulators found", "",
             "RetroArch's usual folders. You can change them later."},
            {});
    // Every platform an emulator here plays: the one playing it (or off).
    // Cross shows its details: the others that also play it, its save sync
    // and its folders.
    const std::vector<Conflict> all_systems = systems();
    if (!all_systems.empty())
        add({RowKind::header, "On this " ROMM_HERE}, {});
    static const char *const kSyncLabel[] = {"Saves and states", "Saves", "Off"};
    auto sync_index = [&](const std::string &key) {
        const auto it = sync_.find(key);
        const std::string mode = it == sync_.end() ? "all" : it->second;
        return mode == "off" ? 2 : mode == "saves" ? 1 : 0;
    };
    for (const Conflict &c : all_systems)
    {
        const std::string main = chosen(c);
        std::string label = "Off", also_names;
        for (const Option &o : c.options)
        {
            if (o.root == main)
                label = o.label;
            const auto set = also_.find(c.key);
            if (!main.empty() && o.root != main && set != also_.end() && set->second.count(o.root))
                also_names += (also_names.empty() ? "Also in " : ", ") + o.label;
        }
        const bool open = expanded_ == c.key && !main.empty();
        std::string detail;
        if (!open && !main.empty())
        {
            detail = also_names;
            if (saves && sync_index(c.key))
                detail += std::string(detail.empty() ? "" : "  \xC2\xB7  ") + "Sync: " + kSyncLabel[sync_index(c.key)];
        }
        Row platform{RowKind::choice, system_name(c.key), label, detail};
        platform.expander = main.empty() ? 0 : open ? 2 : 1; // off: nothing to open
        add(platform, {RowRef::kMain, -1, c.key});
        if (!open)
            continue;
        std::vector<const Option *> on = {};
        for (const Option &o : c.options)
            if (o.root == main)
                on.push_back(&o);
        // The others can play it too: the games linked in, one save where they keep the same kind.
        for (const Option &o : c.options)
        {
            if (o.root == main)
                continue;
            const auto set = also_.find(c.key);
            const bool also = set != also_.end() && set->second.count(o.root);
            // Saved as an extra: whether it plays the main one's games without copies.
            const auto ms = also ? mounts_.find(c.key + "|" + o.root) : mounts_.end();
            std::string how;
            if (ms != mounts_.end())
                how = ms->second.mounted ? "Uses " + label + "'s games, no copies" : ms->second.note;
            add({RowKind::toggle, "   Also in " + o.label, "", how, also}, {RowRef::kAlso, -1, c.key, o.root});
            if (ms != mounts_.end() && ms->second.own > 0)
            {
                const int n = ms->second.own;
                const std::string games = std::to_string(n) + (n == 1 ? " game" : " games");
                add({RowKind::action, "      Move its " + games + " to " + label, "", ""},
                    {RowRef::kOwnMove, -1, c.key, o.root});
                Row del{RowKind::action,
                        confirm_delete_ == c.key + "|" + o.root ? "      Press again to delete them" : "      Delete its " + games,
                        "", ""};
                del.danger = true;
                add(del, {RowRef::kOwnDelete, -1, c.key, o.root});
            }
            if (also)
                on.push_back(&o);
        }
        // Its folders in each emulator that plays it: the preset's, or picked.
        for (const Option *o : on)
        {
            const Emulator *em = nullptr;
            for (const Emulator &e : emulators_)
                if (e.root == o->root)
                    em = &e;
            if (!em)
                continue;
            const std::string who = on.size() > 1 ? "   Folders in " + em->name : std::string("   Folders");
            if (em->fixed.size() >= 3)
            {
                add({RowKind::info, who, "Set by the emulator", ""}, {});
                continue;
            }
            const auto found = em->dirs.find(c.key);
            const auto sys = custom_dirs_.find(c.key);
            const bool custom = sys != custom_dirs_.end() && sys->second.count(o->root);
            std::string folder_detail = found != em->dirs.end() && !found->second[0].empty() ? "    Games in " + found->second[0] : "";
            add({RowKind::choice, who, custom ? "Manual" : "Preset", custom ? "    Pick each folder below" : folder_detail},
                {RowRef::kFolders, -1, c.key, o->root});
            if (!custom)
                continue;
            static const char *const kLabels[] = {"      Games folder", "      Saves folder", "      States folder"};
            static const char *const kKeys[] = {"rom_dir", "save_dir", "state_dir"};
            for (int k = 0; k < (saves ? 3 : 1); ++k)
            {
                const std::string picked = sys->second.at(o->root)[static_cast<std::size_t>(k)];
                const std::string def = found != em->dirs.end() ? found->second[static_cast<std::size_t>(k)] : "";
                if (em->fixed.count(kKeys[k]))
                {
                    add({RowKind::info, kLabels[k], "Set by the emulator", shorten(def)}, {});
                    continue;
                }
                add({RowKind::action, kLabels[k], picked.empty() ? "Preset" : shorten(picked), picked.empty() ? shorten(def) : ""},
                    {RowRef::kSysFolder, k, c.key, o->root});
            }
        }
        // Then what's this platform's own: its save sync, PS2's memory cards.
        if (saves)
            add({RowKind::choice, "   Sync", kSyncLabel[sync_index(c.key)], ""}, {RowRef::kSysSync, -1, c.key});
        // Memory cards: one method per system, never both. PS2 can be chosen
        // when an emulator keeps either (LRPS2), or when RomM Sync gives each
        // game its own card (PS5SX2); otherwise it's the emulator's.
        if (saves && cards_.count(c.key))
        {
            const CardMethod &cm = cards_.at(c.key);
            const bool choice = c.key == "ps2" && has_ps2();
            const bool game_saves = c.key == "ps2" && has_ps2_game_saves();
            const std::string method = choice && !ps2_cards_.empty() ? ps2_cards_ : cm.method;
            const bool shared_ok = at_least(app.status().server_version, 5, 3, 0);
            std::string detail = !cm.note.empty()                  ? "    " + cm.note
                                 : method == "shared" && !shared_ok ? "    Shared cards need RomM 5.3 or newer"
                                 : game_saves && method != "shared" ? "    Each game gets its own card, which syncs as its save"
                                 : method == "shared"               ? "    The whole cards sync between your " ROMM_PEERS
                                                                    : "    Each game's card syncs as its save";
            Row r{RowKind::choice, "   Memory cards", method == "shared" ? "Shared, synced" : "One per game", detail};
            r.enabled = choice;
            add(r, {RowRef::kPs2});
        }
    }
    // Set up by hand: emulators found with no preset yet, and the systems on
    // RomM no emulator here plays. On, then their folders.
    const std::vector<std::string> left = uncovered();
    if (!left.empty() || !manual_.empty())
        add({RowKind::header, "Set up manually"}, {});
    for (const std::string &key : manual_)
        add_own(key, "No preset for this emulator yet");
    for (const std::string &key : left)
    {
        const Own &o = own_.at(key);
        char detail[96];
        std::snprintf(detail, sizeof detail, "%d game%s  \xC2\xB7  no emulator found", o.games, o.games == 1 ? "" : "s");
        add_own(key, detail);
    }
    Row next{RowKind::action, editing_ ? "Save" : "Continue", "", ""};
    next.enabled = !busy_;
    add(next, {RowRef::kContinue});
    return rows;
}

void SetupPage::update_emulators(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    if (emulators_loaded_ && !busy_ && !error_.empty() && (retry_in_ -= dt) <= 0.0f)
        emulators_loaded_ = false; // look again; the error stays up until it works
    if (!emulators_loaded_ && !busy_)
    {
        retry_in_ = 3.0f;
        load_emulators(app);
    }
    if (leaving_)
    {
        const std::vector<Row> choices = leave_rows();
        input = touch::with(input, touch::take(&leaving_), Action::back);
        input = leave_list_.touch(input, choices);
        const RowEvent pick = leave_list_.handle(choices, input, feedback);
        leave_list_.update(dt, static_cast<int>(choices.size()), 3 * RowList::kRowHeight);
        if (input.is_pressed(Action::back) || pick.row == 2)
        {
            feedback.play(Cue::back);
            leaving_ = false; // keep editing
        }
        else if (pick.row == 0)
        {
            leaving_ = false;
            save_emulators(app, feedback); // back to Settings once saved
        }
        else if (pick.row == 1)
        {
            feedback.play(Cue::back);
            leaving_ = false;
            editing_ = false; // discarded
        }
        return;
    }
    std::vector<RowRef> refs;
    const std::vector<Row> rows = emulator_rows(app, &refs);
    input = rows_.touch(input, rows);
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    const int at = rows_.focus();
    focus_expander_ = at >= 0 && at < static_cast<int>(rows.size()) ? rows[static_cast<std::size_t>(at)].expander : 0;
    if (editing_ && input.is_pressed(Action::back) && !busy_)
    {
        if (dirty())
        {
            feedback.play(Cue::modal_open);
            leaving_ = true;
            leave_list_.set_focus(0);
            return;
        }
        feedback.play(Cue::back);
        editing_ = false; // back to Settings, nothing changed
        return;
    }
    if (editing_ && input.is_pressed(Action::north) && dirty())
    {
        save_emulators(app, feedback);
        return;
    }
    if (e.row < 0 || !emulators_loaded_ || e.row >= static_cast<int>(refs.size()))
        return;
    const RowRef &ref = refs[static_cast<std::size_t>(e.row)];
    const std::vector<Conflict> list = conflicts();
    switch (ref.kind)
    {
    case RowRef::kEmulator:
    {
        Emulator &em = emulators_[static_cast<std::size_t>(ref.emulator)];
        if (em.ready)
            em.on = !em.on;
        return;
    }
    case RowRef::kEmuSync:
    {
        static const char *const kModes[] = {"all", "saves", "off"};
        Emulator &em = emulators_[static_cast<std::size_t>(ref.emulator)];
        int at = 0;
        for (int i = 0; i < 3; ++i)
            if (em.sync == kModes[i])
                at = i;
        em.sync = kModes[((at + e.step) % 3 + 3) % 3];
        return;
    }
    case RowRef::kMain:
        // Cross: its details; left and right: the emulator that plays it, or off.
        if (input.is_pressed(Action::confirm))
        {
            expanded_ = expanded_ == ref.key ? "" : ref.key;
            return;
        }
        for (const Conflict &c : systems())
        {
            if (c.key != ref.key)
                continue;
            std::vector<std::string> roots;
            for (const Option &o : c.options)
                roots.push_back(o.root);
            roots.push_back(""); // off
            const std::string current = chosen(c);
            int at = 0;
            for (int i = 0; i < static_cast<int>(roots.size()); ++i)
                if (roots[static_cast<std::size_t>(i)] == current)
                    at = i;
            const int n = static_cast<int>(roots.size());
            const std::string next = roots[static_cast<std::size_t>(((at + e.step) % n + n) % n)];
            choices_[c.key] = next;
            if (next.empty())
                also_[c.key].clear(); // off everywhere
            else
                also_[c.key].erase(next); // the main one isn't also an extra
        }
        return;
    case RowRef::kSysSync:
    {
        static const char *const kModes[] = {"all", "saves", "off"};
        const auto it = sync_.find(ref.key);
        const std::string now = it == sync_.end() ? "all" : it->second;
        int at = 0;
        for (int i = 0; i < 3; ++i)
            if (now == kModes[i])
                at = i;
        sync_[ref.key] = kModes[((at + e.step) % 3 + 3) % 3];
        return;
    }
    case RowRef::kAlso:
    {
        auto &set = also_[ref.key];
        if (set.count(ref.root))
            set.erase(ref.root);
        else
            set.insert(ref.root);
        return;
    }
    case RowRef::kOwnMove:
    case RowRef::kOwnDelete:
    {
        const std::string which = ref.key + "|" + ref.root;
        const bool del = ref.kind == RowRef::kOwnDelete;
        if (del && confirm_delete_ != which)
        {
            confirm_delete_ = which; // it can't be undone: a second Cross
            return;
        }
        confirm_delete_.clear();
        App *a = &app;
        Json body(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddStringToObject(body.get(), "root", ref.root.c_str());
        cJSON_AddStringToObject(body.get(), "system", ref.key.c_str());
        cJSON_AddStringToObject(body.get(), "action", del ? "delete" : "move");
        app.post("/api/emulators/own-games", print(body.get()), [this, a, del](const Response &r) {
            if (!r.ok())
            {
                a->say("Couldn't do it: " + r.message());
                return;
            }
            Json j(r.json(), cJSON_Delete);
            const int n = static_cast<int>(num(j.get(), "games"));
            a->say(std::to_string(n) + (n == 1 ? " game " : " games ") + (del ? "deleted" : "moved"));
            load_mounts(*a);
        });
        return;
    }
    case RowRef::kPs2:
    {
        const std::string now = !ps2_cards_.empty() ? ps2_cards_ : cards_.count("ps2") ? cards_.at("ps2").method : "";
        ps2_cards_ = now == "shared" ? "per_game" : "shared";
        return;
    }
    case RowRef::kOwn:
        own_[ref.key].on = !own_[ref.key].on;
        return;
    case RowRef::kOwnFolder:
        open_picker(app, ref.key, ref.emulator);
        return;
    case RowRef::kFolders:
    {
        auto &sys = custom_dirs_[ref.key];
        if (sys.count(ref.root))
            sys.erase(ref.root); // back to the folders found
        else
            sys[ref.root] = {};
        return;
    }
    case RowRef::kSysFolder:
        open_picker(app, ref.key, ref.emulator, ref.root);
        return;
    case RowRef::kContinue:
        save_emulators(app, feedback);
        return;
    default:
        return;
    }
}

// What Save sends: the profiles for the emulators kept (the PS2 cards go
// apart). Also what tells a change from what was saved.
std::string SetupPage::save_body() const
{
    const std::vector<Conflict> list = conflicts();
    std::vector<std::string> by_folders = uncovered();
    by_folders.insert(by_folders.end(), manual_.begin(), manual_.end());
    Json body(cJSON_CreateObject(), cJSON_Delete);
    cJSON *roots = cJSON_AddArrayToObject(body.get(), "roots");
    for (const Emulator &em : emulators_)
        if (used(em))
            cJSON_AddItemToArray(roots, cJSON_CreateString(em.root.c_str()));
    cJSON *choices = cJSON_AddObjectToObject(body.get(), "choices");
    cJSON *also = cJSON_AddObjectToObject(body.get(), "also");
    for (const Conflict &c : list)
    {
        const std::string main = chosen(c);
        cJSON_AddStringToObject(choices, c.key.c_str(), main.c_str());
        cJSON *extras = cJSON_AddArrayToObject(also, c.key.c_str());
        const auto set = also_.find(c.key);
        for (const Option &o : c.options)
            if (o.root != main && set != also_.end() && set->second.count(o.root))
                cJSON_AddItemToArray(extras, cJSON_CreateString(o.root.c_str()));
    }
    // Each system: its main emulator, the ones on, and folders picked (the
    // payload's "systems", which wins over "choices" and "also").
    cJSON *systems_json = cJSON_AddObjectToObject(body.get(), "systems");
    for (const Conflict &c : systems())
    {
        const std::string main = chosen(c);
        cJSON *sy = cJSON_AddObjectToObject(systems_json, c.key.c_str());
        cJSON_AddStringToObject(sy, "main", main.c_str());
        const auto mode = sync_.find(c.key);
        cJSON_AddStringToObject(sy, "sync", mode != sync_.end() ? mode->second.c_str() : "all");
        cJSON *on = cJSON_AddArrayToObject(sy, "on");
        cJSON *folders = cJSON_AddObjectToObject(sy, "folders");
        if (main.empty())
            continue; // off: every emulator leaves it out
        const auto set = also_.find(c.key);
        const auto picked = custom_dirs_.find(c.key);
        for (const Option &o : c.options)
        {
            if (o.root != main && (set == also_.end() || !set->second.count(o.root)))
                continue;
            cJSON_AddItemToArray(on, cJSON_CreateString(o.root.c_str()));
            if (picked == custom_dirs_.end() || !picked->second.count(o.root))
                continue;
            const std::array<std::string, 3> &d = picked->second.at(o.root);
            cJSON *f = cJSON_AddObjectToObject(folders, o.root.c_str());
            cJSON_AddStringToObject(f, "rom_dir", d[0].c_str());
            cJSON_AddStringToObject(f, "save_dir", d[1].c_str());
            cJSON_AddStringToObject(f, "state_dir", d[2].c_str());
        }
    }
    cJSON *custom = cJSON_AddArrayToObject(body.get(), "custom");
    for (const std::string &key : by_folders)
    {
        const Own &o = own_.at(key);
        if (!o.on)
            continue;
        cJSON *c = cJSON_CreateObject(), *slugs = cJSON_AddArrayToObject(c, "slugs");
        for (const std::string &slug : o.slugs)
            cJSON_AddItemToArray(slugs, cJSON_CreateString(slug.c_str()));
        cJSON_AddStringToObject(c, "name", o.name.c_str());
        cJSON_AddStringToObject(c, "rom_dir", o.folder[0].c_str());
        cJSON_AddStringToObject(c, "save_dir", o.folder[1].c_str());
        cJSON_AddStringToObject(c, "state_dir", o.folder[2].c_str());
        if (!o.id.empty())
            cJSON_AddStringToObject(c, "id", o.id.c_str());
        cJSON_AddItemToArray(custom, c);
    }
    return print(body.get());
}

std::vector<Row> SetupPage::leave_rows() const
{
    return {{RowKind::action, "Save changes", "", ""},
            {RowKind::action, "Discard changes", "", ""},
            {RowKind::action, "Keep editing", "", ""}};
}

bool SetupPage::dirty() const
{
    return editing_ && emulators_loaded_ && !saved_.empty() && save_body() + ps2_cards_ != saved_;
}

void SetupPage::save_emulators(App &app, hui::ui::Feedback &feedback)
{
    if (busy_)
        return;
    // Everything set up by its folders: the systems no emulator plays, and the
    // systems of emulators with no preset.
    std::vector<std::string> by_folders = uncovered();
    by_folders.insert(by_folders.end(), manual_.begin(), manual_.end());
    // Each one turned on needs its games folder.
    for (const std::string &key : by_folders)
        if (own_[key].on && own_[key].folder[0].empty())
        {
            app.say("Choose a games folder for " + own_[key].name + ", or turn it off");
            rows_.refuse(feedback);
            return;
        }
    busy_ = true;
    const std::string ps2 = "{\"ps2_cards\":\"" + (has_ps2() ? ps2_cards_ : std::string()) + "\"}";
    App *a = &app;
    app.post("/api/setup/emulators", save_body(), [this, a, ps2](const Response &r) {
        if (!r.ok())
        {
            busy_ = false;
            a->say("Couldn't save the emulators: " + r.message());
            return;
        }
        a->post("/api/config", ps2, [this, a](const Response &) {
            if (editing_)
            {
                saved_.clear();
                editing_ = false;
                busy_ = false;
                a->say("Emulators saved");
                a->refresh_config();
                a->reload_platforms();
                load_mounts(*a);
                return;
            }
            go(kPreview);
        });
    });
}

// ------------------------------------------------------------------ preview

void SetupPage::load_preview(App &app)
{
    preview_.loading = true;
    // A first setup matches every game here with RomM first: it can take a while.
    app.get("/api/setup/preview", [this](const Response &r) {
        preview_.loading = false;
        preview_.loaded = true;
        if (!r.ok())
        {
            preview_.error = r.message();
            return;
        }
        Json j(r.json(), cJSON_Delete);
        preview_.error.clear();
        preview_.games = count(j.get(), "local_games");
        preview_.saves = count(j.get(), "local_saves");
        preview_.up = count(j.get(), "uploads");
        preview_.down = count(j.get(), "downloads");
        preview_.both = count(j.get(), "conflicts");
        preview_.in_sync = count(j.get(), "in_sync");
    }, 120000);
}

// Preview rows: 0 save sync, 1 backed up, 2 continue, 3 check again.
std::vector<Row> SetupPage::preview_rows(std::vector<int> *ids) const
{
    std::vector<Row> rows;
    auto add = [&](Row r, int id) {
        rows.push_back(std::move(r));
        if (ids)
            ids->push_back(id);
    };
    add({RowKind::toggle, "Sync saves with RomM", "", save_sync_ ? "Saves and states" : "Off: RomM is only for games",
         save_sync_},
        0);
    if (!save_sync_)
    {
        add({RowKind::action, "Continue", "", ""}, 2);
        return rows;
    }
    add({RowKind::toggle, "My saves are backed up", "", "The first sync can replace saves", backed_up_}, 1);
    Row next{RowKind::action, "Continue", "", backed_up_ ? "" : "Once your saves are backed up"};
    next.enabled = backed_up_;
    add(next, 2);
    Row again{RowKind::action, "Check again", "", ""};
    again.enabled = !preview_.loading;
    add(again, 3);
    return rows;
}

void SetupPage::update_preview(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    if (save_sync_ && !preview_.loaded && !preview_.loading)
        load_preview(app);
    std::vector<int> ids;
    const std::vector<Row> rows = preview_rows(&ids);
    input = rows_.touch(input, rows);
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    const int id = e.row >= 0 ? ids[static_cast<std::size_t>(e.row)] : -1;
    if (id == 0)
        save_sync_ = !save_sync_;
    else if (id == 1)
        backed_up_ = !backed_up_;
    else if (id == 2)
        go(kFinish);
    else if (id == 3)
        preview_ = Preview{};
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        go(kEmulators);
    }
}

// ------------------------------------------------------------------ finish

// Finish rows: 0 policy, 1 on change, 2 on exit, 3 on start, 4 notify,
// 5 interval, 6 finish. The sync ones only with save sync on.
std::vector<Row> SetupPage::finish_rows(std::vector<int> *ids) const
{
    const Config &c = finish_;
    std::vector<Row> rows;
    auto add = [&](Row r, int id) {
        rows.push_back(std::move(r));
        if (ids)
            ids->push_back(id);
    };
    if (save_sync_)
    {
        add({RowKind::choice, "Resolving save conflicts", label_of(kPolicies, c.conflict_policy),
             "What " ROMM_APP " will do by default whenever a save conflict exists between this " ROMM_MODEL " and your RomM Server"}, 0);
        add({RowKind::toggle, "Upload saves as they change", "", "While you play", c.sync_on_change}, 1);
        add({RowKind::toggle, "Sync when a game closes", "", "", c.sync_on_game_exit}, 2);
        add({RowKind::toggle, "Sync when a game starts", "", "", c.sync_on_game_start}, 3);
    }
    add({RowKind::toggle, "Notifications", "", "", c.notify}, 4);
    if (save_sync_)
        add({RowKind::choice, "Sync every", interval_label(c.sync_interval_min), ""}, 5);
    Row done{RowKind::action, save_sync_ ? "Finish and sync" : "Finish", "", ""};
    done.enabled = !busy_;
    add(done, 6);
    return rows;
}

void SetupPage::update_finish(App &app, const hui::InputFrame &pad, float dt, hui::ui::Feedback &feedback)
{
    hui::InputFrame input = pad;
    if (!finish_known_ && app.config().known)
    {
        finish_ = app.config();
        finish_known_ = true;
    }
    std::vector<int> ids;
    const std::vector<Row> rows = finish_rows(&ids);
    input = rows_.touch(input, rows);
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        go(kPreview);
        return;
    }
    switch (e.row >= 0 ? ids[static_cast<std::size_t>(e.row)] : -1)
    {
    case 0:
        finish_.conflict_policy = step_option(kPolicies, finish_.conflict_policy, e.step);
        break;
    case 1:
        finish_.sync_on_change = !finish_.sync_on_change;
        break;
    case 2:
        finish_.sync_on_game_exit = !finish_.sync_on_game_exit;
        break;
    case 3:
        finish_.sync_on_game_start = !finish_.sync_on_game_start;
        break;
    case 4:
        finish_.notify = !finish_.notify;
        break;
    case 5:
        finish_.sync_interval_min = step_interval(finish_.sync_interval_min, e.step);
        break;
    case 6:
    {
        if (e.row < 0)
            break;
        Json body(cJSON_CreateObject(), cJSON_Delete);
        cJSON_AddBoolToObject(body.get(), "save_sync", save_sync_);
        cJSON_AddStringToObject(body.get(), "conflict_policy", finish_.conflict_policy.c_str());
        cJSON_AddBoolToObject(body.get(), "sync_on_change", finish_.sync_on_change);
        cJSON_AddBoolToObject(body.get(), "sync_on_game_exit", finish_.sync_on_game_exit);
        cJSON_AddBoolToObject(body.get(), "sync_on_game_start", finish_.sync_on_game_start);
        cJSON_AddBoolToObject(body.get(), "notify", finish_.notify);
        cJSON_AddNumberToObject(body.get(), "sync_interval_min", finish_.sync_interval_min);
        busy_ = true;
        App *a = &app;
        app.post("/api/setup/finish", print(body.get()), [this, a](const Response &r) {
            if (!r.ok())
            {
                busy_ = false;
                a->say("Couldn't finish the setup: " + r.message());
                return;
            }
            a->say(save_sync_ ? "All set. Let the first sync finish before you play" : "All set");
            a->refresh_config();
            a->refresh_now();
        });
        break;
    }
    default:
        break;
    }
}

// ------------------------------------------------------------------ drawing

std::vector<hui::ui::Hint> SetupPage::hints() const
{
    using hui::ui::Button;
    switch (step_)
    {
    case kServer:
        return {{Button::square, "Delete"}, {Button::triangle, tls_verify_ ? "Don't verify TLS certificate" : "Verify TLS certificate"}, {Button::r2, "Connect"}};
    case kApprove:
        return {{Button::circle, "Use another server"}};
    case kEmulators:
        if (picking_)
            return {{Button::cross, "Choose"}, {Button::circle, "Back"}};
        if (leaving_)
            return {{Button::cross, "Choose"}, {Button::circle, "Keep editing"}};
        {
            // On a platform: Cross opens or closes it.
            const char *cross = focus_expander_ == 1 ? "Expand" : focus_expander_ == 2 ? "Collapse" : "Choose";
            if (editing_ && dirty())
                return {{Button::triangle, "Save"}, {Button::cross, cross}, {Button::circle, "Back"}};
            if (editing_)
                return {{Button::cross, cross}, {Button::circle, "Back"}};
            return {{Button::cross, cross}};
        }
    default:
        return {{Button::cross, "Choose"}, {Button::circle, "Back"}};
    }
}

void SetupPage::draw_heading(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const char *title,
                             const std::string &text) const
{
    const float in = tween::stagger(step_age_, 0, 0.06f, 0.5f);
    list.push_opacity(in);
    const float x = kMargin + 30.0f * (1.0f - in);
    char label[32];
    if (editing_)
        std::snprintf(label, sizeof label, "SETTINGS");
    else
        std::snprintf(label, sizeof label, "STEP %d OF %d", static_cast<int>(step_) + 1, kSteps);
    ui::text(list, fonts.semibold, label, x, 196, 20, look.accent, Align::left, 4.0f);
    // The picker's heading names a platform as RomM spells it, and the folder
    ui::text(list, fonts.display, plain_text(title), x - 3, 262, 56, look.text);
    ui::paragraph(list, fonts.regular, plain_text(text.c_str()), x, 312, 26, 1500, 36, look.muted, 2);
    list.pop_opacity();
}

void SetupPage::draw_steps(hui::gfx::DrawList &list, const hui::ui::Fonts &, const Look &look) const
{
    // Five dots on the right of the heading: done, here, to come.
    const float y = 190, x0 = 1824 - 4 * 34;
    for (int i = 0; i < kSteps; ++i)
    {
        const float x = x0 + static_cast<float>(i) * 34;
        if (i < step_)
            list.circle(x, y, 8, look.accent);
        else if (i == step_)
            list.rounded_rect({x - 14, y - 8, 28, 16}, 8, look.text);
        else
            list.circle(x, y, 7, look.muted.with_alpha(0.35f));
    }
}

void SetupPage::draw_server(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                            float clock) const
{
    draw_heading(list, fonts, look, "Connect to your RomM server",
                 "Its address as you'd open it in a browser, like http://192.168.1.20:8080 or https://romm.example.com");
    draw_panel(list, look, kField, 22);
    const float size = 34.0f;
    const std::string shown = tail(*fonts.regular.font, url_, size, kField.w - 80);
    const float w = ui::text(list, fonts.regular, shown, kField.x + 32, kField.cy() + 12, size, look.panel_text);
    if (!busy_ && std::fmod(clock, 1.0f) < 0.6f)
        list.rounded_rect({kField.x + 36 + w, kField.y + 22, 3, kField.h - 44}, 1.5f, look.accent);

    // Under the field: what happened, or the certificate setting.
    const float y = kField.y + kField.h + 40;
    if (busy_)
    {
        const char *dots[] = {"", ".", "..", "..."};
        ui::text(list, fonts.semibold, std::string("Contacting RomM") + dots[static_cast<int>(clock * 3) % 4],
                 kField.x + 4, y, 24, look.text);
    }
    else if (!error_.empty())
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(plain_text(error_.c_str()), 24, kField.w), kField.x + 4, y, 24,
                 look.danger);
    else
    {
        ui::draw_button(list, fonts, look.glyphs(), ui::Button::triangle, kField.x + 4, y - 9, 30);
        touch::zone(list, {kField.x - 8, y - 36, 520, 52}, &tls_verify_, 0, touch::kButton);
        ui::text(list, fonts.regular,
                 tls_verify_ ? "Verify TLS certificate" : "Don't verify TLS certificate",
                 kField.x + 46, y, 22, look.muted);
    }
    (void)app;
    list.push_opacity(busy_ ? 0.4f : 1.0f);
    ui::Canvas canvas{list, fonts, 0, clock};
    keys_.draw(canvas);
    if (!busy_)
        touch::keys(list, keys_);
    list.pop_opacity();
}

void SetupPage::draw_approve(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                             float clock) const
{
    const Status &s = app.status();
    draw_heading(list, fonts, look, "Approve this " ROMM_HERE " in RomM",
                 "On your phone or computer, open the address below, sign in to RomM and enter the code.");
    const float in = tween::stagger(step_age_, 2, 0.08f, 0.6f);
    list.push_opacity(in);
    const Rect panel{260, 380 + 24 * (1.0f - in), 1400, 470};
    draw_panel(list, look, panel);
    // The address as a QR code too, for a phone's camera.
    static std::string qr_for;
    static QrCode qr;
    if (qr_for != s.verification_url)
    {
        qr_for = s.verification_url;
        qr = QrCode::encode(qr_for);
    }
    const bool has_qr = !s.verification_url.empty() && qr.size > 0;
    if (has_qr)
        draw_qr(list, qr, {panel.x + panel.w - 330, panel.y + 70, 290, 290});
    const float cx = has_qr ? panel.x + (panel.w - 360) * 0.5f : panel.cx();
    ui::text(list, fonts.semibold,
             fonts.semibold.font->fit(plain_text(s.verification_url.c_str()), 30, has_qr ? panel.w - 460 : panel.w - 120),
             cx, panel.y + 92, 30,
             look.accent, Align::center);
    std::string code;
    for (char c : s.user_code)
        code += c, code += ' ';
    if (!code.empty())
        code.pop_back();
    // As big as fits beside the QR code, with room to spare at the edges.
    const float room = (has_qr ? panel.w - 360 : panel.w) - 120;
    float size = has_qr ? 100.0f : 120.0f;
    const float wide = fonts.display.measure(code, size, 6.0f);
    if (wide > room)
        size *= room / wide;
    ui::text(list, fonts.display, code, cx, panel.y + 270, size, look.panel_text, Align::center, 6.0f);
    const float pulse = 0.5f + 0.5f * std::sin(clock * 3.0f);
    list.circle(cx - 150, panel.y + 382, 7, look.accent.with_alpha(0.4f + 0.6f * pulse));
    ui::text(list, fonts.regular, "Waiting for approval", cx - 128, panel.y + 390, 26, look.panel_muted);
    list.pop_opacity();
}

void SetupPage::draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                     float clock) const
{
    if (!editing_)
        draw_steps(list, fonts, look);
    switch (step_)
    {
    case kServer:
        draw_server(list, fonts, look, app, clock);
        return;
    case kApprove:
        draw_approve(list, fonts, look, app, clock);
        return;
    default:
        break;
    }

    const float in = tween::stagger(step_age_, 2, 0.08f, 0.6f);
    std::vector<Row> rows;
    if (step_ == kEmulators && picking_)
    {
        draw_picker(list, fonts, look, clock);
        return;
    }
    if (step_ == kEmulators)
    {
        draw_heading(list, fonts, look, editing_ ? "Emulators" : "Your emulators",
                     dirty() ? "Unsaved changes. Press " ROMM_BTN_NORTH " to save them." : "Choose the emulator for each platform.");
        std::vector<RowRef> refs;
        rows = emulator_rows(app, &refs);
        // Each platform's row, with its logo before its name.
        for (std::size_t i = 0; i < rows.size() && i < refs.size(); ++i)
            if (refs[i].kind == RowRef::kMain || refs[i].kind == RowRef::kOwn)
            {
                rows[i].logo_slot = true;
                if (const auto it = logos_.find(refs[i].key); it != logos_.end())
                    rows[i].logo = it->second.first, rows[i].logo_aspect = it->second.second;
            }
    }
    else if (step_ == kPreview)
    {
        draw_heading(list, fonts, look, save_sync_ ? "Before the first sync" : "Games only",
                     !save_sync_          ? "Turn save sync on any time in Settings."
                     : preview_.loading     ? "Comparing your saves with RomM's"
                     : preview_.error.empty() ? "What the first sync will do."
                                              : "Couldn't compare: " + preview_.error);
        rows = preview_rows();
        struct Tile
        {
            const char *label;
            int value;
            bool warn;
        } tiles[] = {{"GAMES HERE", preview_.games, false},   {"SAVES HERE", preview_.saves, false},
                     {"TO UPLOAD", preview_.up, false},       {"TO DOWNLOAD", preview_.down, false},
                     {"CHANGED ON BOTH", preview_.both, true}, {"IN SYNC", preview_.in_sync, false}};
        for (int i = 0; i < 6 && save_sync_; ++i)
        {
            const float appear = tween::stagger(step_age_, 2 + i, 0.06f, 0.5f);
            const Rect r{kMargin + static_cast<float>(i) * 292, 372 + 20 * (1.0f - appear), 270, 150};
            list.push_opacity(appear);
            draw_panel(list, look, r, 22);
            ui::text(list, fonts.semibold, tiles[i].label, r.x + 26, r.y + 46, 15, look.panel_muted, Align::left, 2.5f);
            if (!preview_.loaded && save_sync_)
            {
                // Still comparing: a spinner, eight dots fading round.
                const float cx = r.x + 46, cy = r.y + 98;
                for (int k = 0; k < 8; ++k)
                {
                    const float a = 6.2831853f * static_cast<float>(k) / 8.0f;
                    const float phase = std::fmod(clock * 1.4f - static_cast<float>(k) / 8.0f + 1.0f, 1.0f);
                    list.circle(cx + 18 * std::cos(a), cy + 18 * std::sin(a), 4.0f, look.panel_text.with_alpha(0.15f + 0.85f * phase));
                }
            }
            else
            {
                char value[16];
                std::snprintf(value, sizeof value, preview_.loaded && preview_.error.empty() ? "%d" : "-", tiles[i].value);
                ui::text(list, fonts.semibold, value, r.x + 26, r.y + 114, 48,
                         tiles[i].warn && tiles[i].value ? look.warning : look.panel_text);
            }
            list.pop_opacity();
        }
        list.push_opacity(in);
        rows_.draw(list, fonts, look, rows, {kRows.x, 570, kRows.w, 360}, clock);
        list.pop_opacity();
        return;
    }
    else
    {
        draw_heading(list, fonts, look, "When to sync", "You can change all of this later in Settings.");
        rows = finish_rows();
    }
    list.push_opacity(in);
    // The emulators' list takes the full width: no panel beside it.
    const float width = step_ == kEmulators ? kScreenW - 2 * kRows.x : kRows.w;
    rows_.draw(list, fonts, look, rows, {kRows.x, kRows.y + 24 * (1.0f - in), width, kRows.h}, clock);
    list.pop_opacity();
    if (step_ == kEmulators && leaving_)
    {
        // Over everything: save before going back?
        list.rounded_rect({0, 0, kScreenW, kScreenH}, 0, hui::gfx::Color::rgb(0x000000, 0.55f));
        const Rect box{560, 330, 800, 470};
        // Touch: outside the box is Circle (keep editing); the box keeps touches from the rows under it.
        touch::zone(list, {0, 0, kScreenW, kScreenH}, &leaving_, 0);
        touch::zone(list, box, nullptr, -1, touch::kButton);
        hui::gfx::Color solid = look.page; // the page's colour under the panel, so nothing shows through
        solid.a = 1.0f;
        list.rounded_rect(box, look.radius, solid);
        draw_panel(list, look, box);
        ui::text(list, fonts.display, "Save your changes?", box.x + 48, box.y + 84, 40, look.panel_text);
        leave_list_.draw(list, fonts, look, leave_rows(), {box.x + 16, box.y + 130, box.w - 32, 3 * RowList::kRowHeight},
                         clock);
    }
}

} // namespace rommps
