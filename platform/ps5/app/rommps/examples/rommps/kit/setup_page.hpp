// RommPS: the setup, on the console. The same steps as the web page's wizard:
//
//   1. the RomM server's address, typed on the kit's on-screen keyboard;
//   2. the code to approve in RomM (its device pairing), until it's approved;
//   3. the emulators found on the console, and which one plays a system
//      several of them can;
//   4. what the first sync would do, and a word about backups;
//   5. when to sync, then the first sync.
//
// The step follows the payload: unpaired is step 1 or 2, paired but not set
// up is 3 onwards.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "rommps_app.hpp"
#include "rows.hpp"
#include "ui/components/keyboard.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace rommps
{

class SetupPage
{
  public:
    SetupPage();
    SetupPage(const SetupPage &) = delete;
    SetupPage &operator=(const SetupPage &) = delete;
    // Starts over from where the payload is (each time the setup appears).
    void enter();
    // Settings' Emulators: just this step, from the emulators set up now;
    // Save or Circle goes back to Settings.
    void edit_emulators();
    bool editing() const { return editing_; }
    void update(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app, float clock) const;
    // The hints for the step on screen.
    std::vector<hui::ui::Hint> hints() const;

  private:
    enum Step
    {
        kServer,
        kApprove,
        kEmulators,
        kPreview,
        kFinish
    };
    struct System
    {
        std::string key, label; // label: the emulator, and its core when it has one
        std::vector<std::string> slugs; // every RomM slug it plays
    };
    // A system on the RomM server no emulator here plays: off, or the folders picked for it.
    struct Own
    {
        std::vector<std::string> slugs;
        std::string name;
        int games = -1; // the server's count, -1 for an emulator's system
        bool on = false;
        std::string folder[3]; // games, saves, states
        std::string id;        // for an emulator with no preset: its profile, "<emulator>-<system>"
    };
    struct Emulator
    {
        std::string root, name, kind, id;
        std::vector<std::string> cores;
        std::vector<System> systems;
        bool on = true;
        bool ready = true; // false: found, no preset yet (set up by its folders, like a system with no emulator)
        bool fresh = false; // found since the last setup
        std::string note;
    };
    // What a row of the emulator step does.
    struct RowRef
    {
        enum Kind
        {
            kNone,
            kEmulator,
            kMain,
            kAlso,
            kPs2,
            kOwn,
            kOwnFolder,
            kContinue
        } kind = kNone;
        int emulator = -1; // or, for kOwnFolder, which folder (0 games, 1 saves, 2 states)
        std::string key, root;
    };
    struct Option
    {
        std::string root, label;
        bool retroarch = false;
    };
    struct Conflict
    {
        std::string key;
        std::vector<Option> options;
    };
    struct Preview
    {
        bool loaded = false, loading = false;
        std::string error;
        int games = 0, saves = 0, up = 0, down = 0, both = 0, in_sync = 0;
    };

    void go(Step step);
    void follow_status(App &app);
    void update_server(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_emulators(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_preview(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void update_finish(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void load_emulators(App &app);
    void load_preview(App &app);
    void load_autostart(App &app);
    std::vector<Conflict> conflicts() const;
    // The root of the emulator that keeps a system: the one picked, else RetroArch, else the first.
    std::string chosen(const Conflict &conflict) const;
    // Rows above the emulators' (a message instead of them).
    int lead() const { return error_.empty() && !emulators_.empty() ? 0 : 1; }
    std::vector<Row> emulator_rows(const App &app, std::vector<RowRef> *refs = nullptr) const;
    std::vector<Row> preview_rows() const;
    std::vector<Row> finish_rows() const;
    bool has_ps2() const;
    // The server's systems no ticked emulator plays, in the server's order.
    std::vector<std::string> uncovered() const;
    // The folder picker (/api/fs), for one of a system's folders.
    void open_picker(App &app, const std::string &key, int which);
    void browse(App &app, const std::string &path);
    void update_picker(App &app, const hui::InputFrame &input, float dt, hui::ui::Feedback &feedback);
    void draw_picker(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, float clock) const;

    void draw_steps(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look) const;
    void draw_server(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                     float clock) const;
    void draw_approve(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const App &app,
                      float clock) const;
    void draw_heading(hui::gfx::DrawList &list, const hui::ui::Fonts &fonts, const Look &look, const char *title,
                      const std::string &text) const;

    Step step_ = kServer;
    float step_age_ = 0.0f;
    RowList rows_;
    bool busy_ = false; // a step's request is out
    std::string error_;

    // 1-2
    hui::ui::Keyboard keys_;
    std::string url_;
    bool url_known_ = false;
    bool tls_verify_ = true;

    // 3
    std::vector<Emulator> emulators_;
    bool emulators_loaded_ = false;
    std::map<std::string, std::string> choices_; // system key -> the root of its main emulator
    std::map<std::string, std::set<std::string>> also_; // system key -> roots of its extra emulators
    bool editing_ = false;
    std::string ps2_cards_ = "per_game";
    std::map<std::string, Own> own_;    // by system key (first slug)
    std::vector<std::string> own_order_; // the server's order
    std::vector<std::string> manual_;    // own_ keys of emulators with no preset, in the order found
    bool picking_ = false;
    std::string pick_key_;
    int pick_which_ = 0;
    std::string fs_path_, fs_parent_;
    std::vector<std::string> fs_folders_;
    bool fs_loading_ = false;
    std::string fs_error_;
    RowList pick_rows_;

    // 4
    Preview preview_;
    bool backed_up_ = false;

    // 5
    Config finish_;
    bool finish_known_ = false;
    bool autostart_supported_ = false, autostart_installed_ = false, autostart_on_ = false;
};

} // namespace rommps
