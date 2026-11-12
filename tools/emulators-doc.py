#!/usr/bin/env python3
"""Write docs/emulators.md from the emulator catalogs: emulators-doc.py

For every emulator and system: where games, saves, states and BIOS files go,
which files count and what they're called, and its memory cards (one per
game or shared, the default, where the user switches). The catalogs are the
source; this only makes them readable. RetroArch's cores come from
platform/retroarch_cores.json, which src/detect.c applies to the cores it finds."""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CATALOGS = [("PS5", "platform/ps5/emulators.json"), ("PS4", "platform/ps4/emulators.json")]
# profiles.c's defaults, where a catalog says nothing.
DEFAULTS = {"rom_dir": "{root}/roms/{platform}", "save_dir": "{rom_dir}", "state_dir": "{rom_dir}",
            "bios_dir": "{root}/bios", "save_exts": ".srm,.sav", "state_exts": ".state*", "save_name": "{rom_stem}{ext}"}
MODES = {"per_game": "one per game", "shared": "shared by every game"}

def merged(profile, platform):
    out = dict(DEFAULTS)
    out.update({k: v for k, v in profile.items() if k != "platforms"})
    out.update(platform)
    return out


def code(v):
    return "`%s`" % v if v else ""


def cards_cell(c):
    cards = c.get("cards")
    if not cards:
        return ""
    modes = cards.get("modes", cards.get("default", "per_game")).split(",")
    text = MODES.get(cards.get("default", "per_game"), cards.get("default"))
    if len(modes) > 1:
        text += "; can be " + " or ".join(MODES.get(m, m) for m in modes if m != cards.get("default"))
        if cards.get("change"):
            text += ". One per game: " + cards["change"]
    setting = cards.get("setting") or {}
    if setting.get("file"):
        text += " (read from %s, `%s`)" % (code(setting["file"]), setting.get("key", ""))
    if c.get("pcsx2_cards", {}).get("own_card"):
        text += ". One per game: RomM Sync gives each game its own"
    if c.get("memcards"):
        text += ". Shared cards (synced whole, while the system uses shared cards): " + ", ".join(code(p) for p in c["memcards"].split(","))
    return text


def section(console, path):
    cat = json.load(open(os.path.join(ROOT, path)))["emulators"]
    lines = ["## %s" % console, ""]
    for e in cat:
        profile = e.get("profile", {})
        lines.append("### %s" % e["name"])
        lines.append("")
        if e.get("note"):
            lines.append(e["note"])
            lines.append("")
        # {user_dir}: the emulator's, or a system's own (PS5-ECAM keeps one folder per system)
        homes = [("", profile)] if "user_dir" in profile else []
        homes += [(" for " + ", ".join(p["romm"]), p) for p in profile.get("platforms", []) if "user_dir" in p]
        if homes:
            lines.append("; ".join("`{user_dir}`%s: %s, the first %s" % (
                who, ", else ".join(code(x) for x in o["user_dir"].split(",")),
                "with %s in it, else the first that's there" % code(o["user_dir_has"]) if o.get("user_dir_has") else "that's there") for who, o in homes) + ".")
            lines.append("")
        lines.append("| System | Games | Saves | Save files | Saved as | States | State files | Memory cards |")
        lines.append("|---|---|---|---|---|---|---|---|")
        for p in profile.get("platforms", []):
            c = merged(profile, p)
            sync = c.get("sync_saves", True) and c.get("sync") != "off"
            layout = c.get("save_layout")
            saves = code(c["save_dir"]) if sync else "not synced"
            files = ("a zip per game (%s), from %s" % (layout, code(c.get("bundle_dirs", "")))) if layout else code(c["save_exts"])
            if layout == "ps2":  # src/ps2card.c: each game's own card, synced as sigil's one-game card
                pc = c.get("pcsx2_cards", {})
                saves = code(pc.get("dir", "")) if sync else "not synced"
                files = "its own card, %s, named in %s, synced as one 8 MB card of the game's saves" % (
                    code(pc.get("own_card", "")), code(pc.get("game_ini", "")))
            if layout == "xbox":  # src/sync.c bundle_bases: the hard disk's folders when there, else xemu's image
                hdd = "its hard disk's folders, %s, when there; else " % code(c["bundle_dirs"]) if c.get("bundle_dirs") else ""
                files = "a zip per game (xbox), from %sthe hard disk image `xemu.toml` names" % hdd
                saves = "E:\\UDATA on its hard disk" if sync else "not synced"
            games = code(c["rom_dir"])
            if c.get("old_rom_dirs"):  # src/sync.c scan_rom_dir: played and synced there, never downloaded into
                games += " (games already in %s stay there)" % ", ".join(code(x) for x in c["old_rom_dirs"].split(","))
            names = c["save_name"].split(",")
            stems = [x for x in c.get("save_stems", "").split(",") if x and x + "{ext}" not in names]
            name = "" if layout or not sync else ", else ".join(code(x) for x in names) + (" (also %s)" % ", ".join(code(x) for x in stems) if stems else "")
            states = code(c["state_dir"]) if sync and c.get("state_exts", ".state*") else "not synced"
            lines.append("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
                ", ".join(p["romm"]), games, saves, files if sync else "", name, states,
                code(c.get("state_exts", ".state*")) if states != "not synced" else "", cards_cell(c)))
        lines.append("")
    return lines


def main():
    lines = ["# Emulators", "",
             "Generated by `tools/emulators-doc.py` from `platform/*/emulators.json` and `platform/retroarch_cores.json`: "
             "edit those, then run it again.",
             "`{root}` is the emulator's folder, `{game_id}` the disc's id, `{serial}` a PS1 disc's serial, "
             "`{rom_stem}` the game file's name without its extension.", "",
             "Memory cards: one method per system, never both. A card of each game's own syncs as that game's save. Shared cards hold every game's "
             "saves at once: they sync whole, as RomM memory cards, between the consoles that keep them (RetroArch's cores on cards of their own); when both sides changed, the newer wins and the other is kept.", ""]
    for console, path in CATALOGS:
        lines += section(console, path)
    rules = json.load(open(os.path.join(ROOT, "platform/retroarch_cores.json")))["rules"]
    lines += ["## RetroArch (both consoles)", "",
              "From `platform/retroarch_cores.json`. Saves default to `<game>.srm` (`.sav`, `.rtc`) in RetroArch's save "
              "folder and states to `<game>.state*`, unless a rule says otherwise.", "",
              "| Core | System | Save files | Saved as | Memory cards | Notes |", "|---|---|---|---|---|---|"]
    for r in rules:
        e = r.get("entry", {})
        core = ", ".join(r.get("names", []) + ["*%s*" % n for n in r.get("contains", [])]) or "any"
        files = ("a zip per game (%s)" % e["save_layout"]) if e.get("save_layout") else code(e.get("save_exts", ""))
        if e.get("save_layout") == "ps2":
            files = "each game's own card, synced as one 8 MB card of its saves"
        lines.append("| %s | %s | %s | %s | %s | %s |" % (core, ", ".join(r.get("systems", [])) or "any", files,
                                                          code(e.get("save_name", "")), cards_cell(e), r.get("note", "")))
    open(os.path.join(ROOT, "docs/emulators.md"), "w").write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
