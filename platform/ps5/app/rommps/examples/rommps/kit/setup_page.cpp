// RommPS: the setup, on the console. See setup_page.hpp.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "setup_page.hpp"

#include "cJSON.h"
#include "credits.hpp"
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

// Names for system keys (a platform's first RomM slug), as the web page has them.
std::string system_name(const std::string &key)
{
    static const std::map<std::string, const char *> names = {
        {"psx", "PlayStation"},       {"ps2", "PlayStation 2"},
        {"psp", "PSP"},               {"n64", "Nintendo 64"},
        {"nds", "Nintendo DS"},       {"gba", "Game Boy Advance"},
        {"gb", "Game Boy"},           {"gbc", "Game Boy Color"},
        {"nes", "NES"},               {"fds", "Famicom Disk System"},
        {"snes", "SNES"},             {"genesis", "Mega Drive / Genesis"},
        {"sms", "Master System"},     {"gamegear", "Game Gear"},
        {"segacd", "Sega CD"},        {"sega32", "32X"},
        {"sg1000", "SG-1000"},        {"saturn", "Saturn"},
        {"dc", "Dreamcast"},          {"arcade", "Arcade"},
        {"tg16", "PC Engine"},        {"ngc", "GameCube"},
        {"wii", "Wii"},               {"dos", "DOS"},
        {"msx", "MSX"},               {"lynx", "Lynx"},
        {"virtualboy", "Virtual Boy"}, {"wonderswan", "WonderSwan"},
        {"neo-geo-pocket", "Neo Geo Pocket"}, {"atari2600", "Atari 2600"},
        {"atari7800", "Atari 7800"},  {"c64", "Commodore 64"},
    };
    const auto it = names.find(key);
    return it != names.end() ? it->second : key;
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
    std::size_t from = 0;
    while (from < text.size() && font.measure(std::string_view(text).substr(from), size) > width)
        ++from;
    return from ? "\xE2\x80\xA6" + text.substr(from + 1) : text;
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

void SetupPage::update_server(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
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

void SetupPage::load_emulators(App &app)
{
    busy_ = true;
    App *a = &app;
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
        if (!editing_)
        {
            busy_ = false;
            emulators_loaded_ = true;
            return;
        }
        // From Settings: start from the emulators set up now, and mark the new ones.
        a->get("/api/config", [this](const Response &r2) {
            busy_ = false;
            emulators_loaded_ = true;
            Json cfg(r2.json(), cJSON_Delete);
            const cJSON *profiles = cJSON_GetObjectItemCaseSensitive(cfg.get(), "profiles"), *p;
            if (!cJSON_IsArray(profiles))
                return;
            choices_.clear();
            also_.clear();
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
                e.fresh = e.ready && !mine;
                e.on = e.ready && mine != nullptr;
                if (!mine)
                    continue;
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
                        choices_[sy.key] = e.root;
                }
            }
        });
    });
}

std::vector<SetupPage::Conflict> SetupPage::conflicts() const
{
    std::vector<Conflict> out;
    for (const Emulator &e : emulators_)
    {
        if (!e.on || !e.ready)
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
    out.erase(std::remove_if(out.begin(), out.end(), [](const Conflict &c) { return c.options.size() < 2; }), out.end());
    return out;
}

std::string SetupPage::chosen(const Conflict &c) const
{
    const auto picked = choices_.find(c.key);
    if (picked != choices_.end())
        for (const Option &o : c.options)
            if (o.root == picked->second)
                return o.root;
    for (const Option &o : c.options)
        if (o.retroarch)
            return o.root;
    return c.options.front().root;
}

bool SetupPage::has_ps2() const
{
    for (const Emulator &e : emulators_)
        if (e.on && std::find(e.cores.begin(), e.cores.end(), "LRPS2") != e.cores.end())
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
                    if (e.on && e.ready && std::find(o.slugs.begin(), o.slugs.end(), slug) != o.slugs.end())
                        covered = true;
        if (!covered)
            out.push_back(key);
    }
    return out;
}

void SetupPage::open_picker(App &app, const std::string &key, int which)
{
    picking_ = true;
    pick_key_ = key;
    pick_which_ = which;
    const Own &o = own_[key];
    // Start where the folder is, or where the games are, or at the drives.
    std::string start = !o.folder[which].empty() ? o.folder[which] : o.folder[0];
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

void SetupPage::update_picker(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    std::vector<Row> rows;
    const bool at_drives = fs_path_.empty();
    if (!at_drives)
    {
        rows.push_back({RowKind::action, "Use this folder", "", fs_path_});
        rows.push_back({RowKind::action, "Up", "", fs_parent_.empty() ? "The drives" : fs_parent_});
    }
    for (const std::string &f : fs_folders_)
        rows.push_back({RowKind::action, f, "", ""});
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
            own_[pick_key_].folder[pick_which_] = fs_path_;
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
    const std::string name = it != own_.end() ? it->second.name : pick_key_;
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
    auto add = [&](Row row, RowRef ref) {
        rows.push_back(std::move(row));
        if (refs)
            refs->push_back(std::move(ref));
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
            std::string value = o.folder[k];
            if (value.size() > 34)
                value = "\xE2\x80\xA6" + value.substr(value.size() - 33);
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
             "Continue with RetroArch's usual folders. You can change them later."},
            {});
    for (std::size_t i = 0; i < emulators_.size(); ++i)
    {
        const Emulator &e = emulators_[i];
        if (!e.ready)
        {
            add({RowKind::info, e.name, "No preset yet", e.note + " If yours works, turn it on below and pick its folders."},
                {RowRef::kEmulator, static_cast<int>(i)});
            for (const System &sy : e.systems)
                add_own(e.id + "-" + sy.key, "Turn on to pick its folders");
            continue;
        }
        std::string detail = e.kind == "retroarch" ? "RetroArch" : e.kind == "mednafen" ? "Mednafen" : "Standalone";
        if (e.fresh)
            detail = "New  \xC2\xB7  " + detail;
        std::vector<std::string> names;
        for (const System &sy : e.systems)
        {
            const std::string n = system_name(sy.key);
            if (std::find(names.begin(), names.end(), n) == names.end())
                names.push_back(n);
        }
        char more[48] = "";
        if (names.size() > 3)
            std::snprintf(more, sizeof more, " and %d more", static_cast<int>(names.size() - 3));
        if (!names.empty())
        {
            detail += "  \xC2\xB7  ";
            for (std::size_t k = 0; k < names.size() && k < 3; ++k)
                detail += (k ? ", " : "") + names[k];
            detail += more;
        }
        add({RowKind::toggle, e.name, "", detail, e.on}, {RowRef::kEmulator, static_cast<int>(i)});
    }
    for (const Conflict &c : conflicts())
    {
        const std::string main = chosen(c);
        std::string label;
        for (const Option &o : c.options)
            if (o.root == main)
                label = o.label;
        add({RowKind::choice, system_name(c.key), label, "Games download here first"},
            {RowRef::kMain, -1, c.key});
        // The others can play it too: the games linked in, one save where they keep the same kind.
        for (const Option &o : c.options)
        {
            if (o.root == main)
                continue;
            const auto set = also_.find(c.key);
            const bool on = set != also_.end() && set->second.count(o.root);
            add({RowKind::toggle, "   Also in " + o.label, "", "Gets the same games too", on},
                {RowRef::kAlso, -1, c.key, o.root});
        }
    }
    // Systems on the server no emulator here plays: off, or folders picked for them.
    for (const std::string &key : uncovered())
    {
        const Own &o = own_.at(key);
        char detail[96];
        std::snprintf(detail, sizeof detail, "%d game%s  \xC2\xB7  no emulator found. Turn on to pick its folders",
                      o.games, o.games == 1 ? "" : "s");
        add_own(key, detail);
    }
    if (has_ps2())
    {
        const bool backup_ok = at_least(app.status().server_version, 5, 3, 0);
        Row r{RowKind::choice, "PS2 memory cards", ps2_cards_ == "backup" ? "One card, backed up" : "One card per game",
              backup_ok ? "How RetroArch's PS2 core keeps its saves" : "Your RomM is older than 5.3: one card per game"};
        r.enabled = backup_ok;
        add(r, {RowRef::kPs2});
    }
    Row next{RowKind::action, editing_ ? "Save" : "Continue", "", ""};
    next.enabled = !busy_;
    add(next, {RowRef::kContinue});
    return rows;
}

void SetupPage::update_emulators(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (!emulators_loaded_ && !busy_)
        load_emulators(app);
    std::vector<RowRef> refs;
    const std::vector<Row> rows = emulator_rows(app, &refs);
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    if (editing_ && input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        editing_ = false; // back to Settings, nothing changed
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
    case RowRef::kMain:
        for (const Conflict &c : list)
        {
            if (c.key != ref.key)
                continue;
            const std::string current = chosen(c);
            int at = 0;
            for (int i = 0; i < static_cast<int>(c.options.size()); ++i)
                if (c.options[static_cast<std::size_t>(i)].root == current)
                    at = i;
            const int n = static_cast<int>(c.options.size());
            const std::string next = c.options[static_cast<std::size_t>(((at + e.step) % n + n) % n)].root;
            choices_[c.key] = next;
            also_[c.key].erase(next); // the main one isn't also an extra
        }
        return;
    case RowRef::kAlso:
    {
        auto &set = also_[ref.key];
        if (set.count(ref.root))
            set.erase(ref.root);
        else
            set.insert(ref.root);
        return;
    }
    case RowRef::kPs2:
        ps2_cards_ = ps2_cards_ == "backup" ? "per_game" : "backup";
        return;
    case RowRef::kOwn:
        own_[ref.key].on = !own_[ref.key].on;
        return;
    case RowRef::kOwnFolder:
        open_picker(app, ref.key, ref.emulator);
        return;
    case RowRef::kContinue:
        break;
    default:
        return;
    }
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
    // Continue: the profiles for the emulators kept, then the PS2 cards.
    Json body(cJSON_CreateObject(), cJSON_Delete);
    cJSON *roots = cJSON_AddArrayToObject(body.get(), "roots");
    for (const Emulator &em : emulators_)
        if (em.on && em.ready)
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
    cJSON *custom = cJSON_AddArrayToObject(body.get(), "custom");
    for (const std::string &key : by_folders)
    {
        const Own &o = own_[key];
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
    busy_ = true;
    const std::string ps2 = "{\"ps2_cards\":\"" + (has_ps2() ? ps2_cards_ : std::string("per_game")) + "\"}";
    App *a = &app;
    app.post("/api/setup/emulators", print(body.get()), [this, a, ps2](const Response &r) {
        if (!r.ok())
        {
            busy_ = false;
            a->say("Couldn't save the emulators: " + r.message());
            return;
        }
        a->post("/api/config", ps2, [this, a](const Response &) {
            if (editing_)
            {
                editing_ = false;
                busy_ = false;
                a->say("Emulators saved");
                a->refresh_config();
                a->reload_platforms();
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
    });
}

std::vector<Row> SetupPage::preview_rows() const
{
    std::vector<Row> rows;
    rows.push_back({RowKind::toggle, "My saves are backed up", "",
                    "The first sync can replace saves here or on RomM. Keep a copy of any that matter", backed_up_});
    Row next{RowKind::action, "Continue", "", backed_up_ ? "" : "Once your saves are backed up"};
    next.enabled = backed_up_;
    rows.push_back(next);
    Row again{RowKind::action, "Check again", "", ""};
    again.enabled = !preview_.loading;
    rows.push_back(again);
    return rows;
}

void SetupPage::update_preview(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (!preview_.loaded && !preview_.loading)
        load_preview(app);
    const std::vector<Row> rows = preview_rows();
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    if (e.row == 0)
        backed_up_ = !backed_up_;
    else if (e.row == 1)
        go(kFinish);
    else if (e.row == 2)
        preview_ = Preview{};
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        go(kEmulators);
    }
}

// ------------------------------------------------------------------ finish

void SetupPage::load_autostart(App &app)
{
    app.get("/api/autostart", [this, &app](const Response &r) {
        Json j(r.json(), cJSON_Delete);
        autostart_supported_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(), "supported"));
        autostart_installed_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(), "installed"));
        autostart_on_ = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j.get(), "enabled"));
        // On by default the first time: RomM Sync then starts with the
        // console and nobody has to load it. Setting up again keeps the choice.
        if (autostart_supported_ && !autostart_on_ && !autostart_defaulted_ && !app.status().setup_complete)
        {
            autostart_defaulted_ = true;
            autostart_on_ = true;
            app.install_autostart([this, &app](const Response &) { load_autostart(app); });
        }
    });
}

std::vector<Row> SetupPage::finish_rows() const
{
    const Config &c = finish_;
    std::vector<Row> rows;
    rows.push_back({RowKind::choice, "A save changed in both places", label_of(kPolicies, c.conflict_policy),
                    "On this console and on RomM since the last sync"});
    rows.push_back({RowKind::toggle, "Upload saves as they change", "", "While you play", c.sync_on_change});
    rows.push_back({RowKind::toggle, "Sync when a game closes", "", "Uploads what the game saved", c.sync_on_game_exit});
    rows.push_back({RowKind::toggle, "Sync when a game starts", "", "Fetches newer saves before you play",
                    c.sync_on_game_start});
    rows.push_back({RowKind::toggle, "Notifications", "", "On the console when a sync ends or fails", c.notify});
    rows.push_back({RowKind::choice, "Sync every", interval_label(c.sync_interval_min), "Besides the syncs above"});
    if (autostart_supported_)
        rows.push_back({RowKind::toggle, "Start RomM Sync with the console", "", "With your HEN's payload autostart",
                        autostart_on_});
    Row done{RowKind::action, "Finish and sync", "", ""};
    done.enabled = !busy_;
    rows.push_back(done);
    return rows;
}

void SetupPage::update_finish(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback)
{
    if (!finish_known_ && app.config().known)
    {
        finish_ = app.config();
        finish_known_ = true;
        load_autostart(app);
    }
    const std::vector<Row> rows = finish_rows();
    const RowEvent e = rows_.handle(rows, input, feedback);
    rows_.update(dt, static_cast<int>(rows.size()), kRows.h);
    if (input.is_pressed(Action::back))
    {
        feedback.play(Cue::back);
        go(kPreview);
        return;
    }
    const bool autostart_row = autostart_supported_;
    switch (e.row)
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
        if (autostart_row)
        {
            autostart_on_ = !autostart_on_;
            // On: the payload RommPS carries goes to every HEN's folder that
            // lacks it or has an older one, and autostart is turned on there.
            if (autostart_on_)
            {
                app.install_autostart([this, &app](const Response &) { load_autostart(app); });
                break;
            }
            app.post("/api/autostart", "{\"enable\":false}",
                     [this, &app](const Response &r) {
                         if (!r.ok())
                             app.say("Couldn't change the autostart: " + r.message());
                         load_autostart(app);
                     });
            break;
        }
        [[fallthrough]];
    case 7:
    {
        if (e.row < 0)
            break;
        Json body(cJSON_CreateObject(), cJSON_Delete);
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
            a->say("All set. The first sync has started");
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
        return {{Button::square, "Delete"}, {Button::triangle, "Certificate check"}, {Button::r2, "Connect"}};
    case kApprove:
        return {{Button::circle, "Use another server"}};
    case kEmulators:
        if (picking_)
            return {{Button::cross, "Choose"}, {Button::circle, "Back"}};
        if (editing_)
            return {{Button::cross, "Choose"}, {Button::circle, "Back"}};
        return {{Button::cross, "Choose"}};
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
    ui::text(list, fonts.display, title, x - 3, 262, 56, look.text);
    ui::paragraph(list, fonts.regular, text, x, 312, 26, 1500, 36, look.muted, 2);
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
        ui::text(list, fonts.semibold, fonts.semibold.font->fit(error_, 24, kField.w), kField.x + 4, y, 24, look.danger);
    else
    {
        ui::draw_button(list, fonts, look.glyphs(), ui::Button::triangle, kField.x + 4, y - 9, 30);
        ui::text(list, fonts.regular,
                 tls_verify_ ? "Checking the server's certificate" : "Not checking the certificate (self-signed servers)",
                 kField.x + 46, y, 22, look.muted);
    }
    (void)app;
    list.push_opacity(busy_ ? 0.4f : 1.0f);
    ui::Canvas canvas{list, fonts, 0, clock};
    keys_.draw(canvas);
    list.pop_opacity();
}

void SetupPage::draw_approve(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                             float clock) const
{
    const Status &s = app.status();
    draw_heading(list, fonts, look, "Approve this console in RomM",
                 "On your phone or computer, open the address below, sign in to RomM and enter the code.");
    const float in = tween::stagger(step_age_, 2, 0.08f, 0.6f);
    list.push_opacity(in);
    const Rect panel{360, 380 + 24 * (1.0f - in), 1200, 470};
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
    ui::text(list, fonts.semibold, fonts.semibold.font->fit(s.verification_url, 30, has_qr ? panel.w - 460 : panel.w - 120),
             cx, panel.y + 92, 30,
             look.accent, Align::center);
    std::string code;
    for (char c : s.user_code)
        code += c, code += ' ';
    if (!code.empty())
        code.pop_back();
    ui::text(list, fonts.display, code, cx, panel.y + 270, has_qr ? 100 : 120, look.panel_text, Align::center, 6.0f);
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
                     "Saves sync for the emulators you leave on, and games download to their folders.");
        rows = emulator_rows(app);
        // Beside the list: the focused emulator's systems.
        const int focus = rows_.focus() - lead();
        if (emulators_loaded_ && focus >= 0 && focus < static_cast<int>(emulators_.size()))
        {
            const Emulator &e = emulators_[static_cast<std::size_t>(focus)];
            list.push_opacity(in);
            draw_panel(list, look, kSide);
            ui::text(list, fonts.semibold, "SYSTEMS", kSide.x + 36, kSide.y + 56, 16, look.panel_muted, Align::left, 3.0f);
            float y = kSide.y + 100;
            std::vector<std::string> seen;
            for (const System &s : e.systems)
            {
                const std::string n = system_name(s.key);
                if (std::find(seen.begin(), seen.end(), n) != seen.end())
                    continue;
                seen.push_back(n);
                if (y > kSide.y + kSide.h - 30)
                {
                    ui::text(list, fonts.regular, "\xE2\x80\xA6", kSide.x + 36, y, 22, look.panel_muted);
                    break;
                }
                ui::text(list, fonts.regular, fonts.regular.font->fit(n, 22, kSide.w - 72), kSide.x + 36, y, 22,
                         look.panel_text);
                y += 34;
            }
            if (e.systems.empty())
                ui::text(list, fonts.regular, "None that RomM Sync knows of", kSide.x + 36, y, 22, look.panel_muted);
            list.pop_opacity();
        }
    }
    else if (step_ == kPreview)
    {
        draw_heading(list, fonts, look, "Before the first sync",
                     preview_.loading ? "Comparing the saves on this console with RomM's"
                     : preview_.error.empty()
                         ? "What the first sync will do. Nothing has changed yet."
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
        for (int i = 0; i < 6; ++i)
        {
            const float appear = tween::stagger(step_age_, 2 + i, 0.06f, 0.5f);
            const Rect r{kMargin + static_cast<float>(i) * 292, 372 + 20 * (1.0f - appear), 270, 150};
            list.push_opacity(appear);
            draw_panel(list, look, r, 22);
            ui::text(list, fonts.semibold, tiles[i].label, r.x + 26, r.y + 46, 15, look.panel_muted, Align::left, 2.5f);
            char value[16];
            std::snprintf(value, sizeof value, preview_.loaded && preview_.error.empty() ? "%d" : "-", tiles[i].value);
            ui::text(list, fonts.semibold, value, r.x + 26, r.y + 114, 48,
                     tiles[i].warn && tiles[i].value ? look.warning : look.panel_text);
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
    rows_.draw(list, fonts, look, rows, {kRows.x, kRows.y + 24 * (1.0f - in), kRows.w, kRows.h}, clock);
    list.pop_opacity();
}

} // namespace rommps
