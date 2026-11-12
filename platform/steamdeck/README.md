# RomM Client for the Steam Deck

A [RomM](https://romm.app) companion client for the Steam Deck, for RetroDECK, EmuDeck and whatever emulators you installed yourself. RommDeck is the Deck's sibling of [RommPS](https://github.com/s0liton/RommPS), the PS4/PS5 client: same sync engine, same app, rebuilt for a 1280x800 screen with a controller in your hands.

Having finally built this damn thing for the PS5, I tried to tame my excitement as I sat down to set up a RomM client on my Steam Deck. Grinning ear to ear, I set everything up and went to play my favorite game on PS2. The smile faded quickly as I realized that the existing options only sync RetroArch-style saves, which left out PS1, PS2 and a shitload of other emulators. I restrained myself from throwing my Steam Deck at the tree outside my office window.

So here's RommPS for the Steam Deck. I guess we'll call it... **RommDeck**. Yeah, sure. Whatever. Enjoy it.

- **Save and state sync**
  - Watches your emulators' save folders in the background and uploads changes as soon as they land. Periodic syncs make sure the Deck always has the latest saves and save states.
  - A game's saves come down the moment it finishes downloading, so it's ready to play.
  - Save states only come down to an emulator that can load them (see [Save states](#save-states)).
  - Optional: turn save sync off in setup or Settings to use RomM just for games.
- **RetroDECK, EmuDeck and standalone emulators, not just RetroArch**
  - 40-odd built-in profiles, from PS1 to Switch, Xbox 360, PS Vita and PS4, each synced in the format the emulator actually reads: PS2 memory cards, GameCube GCI folders, Wii NAND saves, Switch user saves, the works. See [Emulators](#emulators).
  - Saves in the same shape Argosy and RommPS use, so a save made on the Deck loads on your other devices and the other way round.
- **See what synced**
  - An activity log and every save and state kept in sync, by platform, in the app and the Web UI. Settle conflicts and restore older versions from either.
  - Settings > Diagnostic bundle saves the logs for a bug report, with your server, names and addresses left out.
- **Interactive library**
  - Browse your RomM library, download games and BIOS files straight into the folders your emulators read (SD card included), and resolve save conflicts.
- **BIOS and system files**
  - Get the BIOS files and firmware your emulators need: Switch keys and firmware straight into Eden, PS3 and Vita firmware ready to install.
- **Built for the Deck**
  - Controller first, with touch screen and mouse support. Runs in Game Mode as a Steam shortcut and in Desktop Mode as a regular window.
  - Based on BlackBearReloaded's UI kit, the same one RommPS uses, with 15+ themes to pick from.
- **Background service**
  - All the work happens in RommDeck's background service, which starts with your session, so syncing and downloads keep going while you play. Or while the Deck sleeps on the couch, judging you.
- **RomM API compliant**
  - RommDeck sticks strictly to RomM's API and its way of syncing saves and states, so it doesn't break anything and keeps up with RomM's latest features.
- **Secure and human-coded**
  - Each release is signed, and the service checks the signature before it updates itself.

## Requirements

- A Steam Deck (or any x86_64 Linux machine) with your emulators, however you got them:
  - [RetroDECK](https://retrodeck.net) 0.10 or newer, set up at least once
  - [EmuDeck](https://www.emudeck.com), RetroArch and the standalone emulators
  - or plain Flathub Flatpaks and AppImages you installed yourself
  
  All of them at once works too. Each install gets its own profile.
- RomM 5.2.0 or newer (may work on older versions)

## Install

Grab the latest release from the [releases page](https://github.com/s0liton/RommDeck/releases), in Desktop Mode. Pick one:

### Installer

1. Download [`RommDeck-Installer.desktop`](https://github.com/s0liton/RommDeck/releases/latest/download/RommDeck-Installer.desktop).
2. Double-click it, and let KDE run it when it asks.

It installs the latest RommDeck as `~/Applications/RommDeck.AppImage`, checked against the release's `SHA256SUMS`, with its background service, a menu entry and a Steam shortcut with artwork. Steam restarts to pick the shortcut up; it asks first. Run it again to update RommDeck or uninstall it.

### AppImage

1. Download `RommDeck-<version>-x86_64.AppImage`.
2. Right-click it, Properties, Permissions, tick **Is executable** (or `chmod +x` it).
3. Run it. The first launch installs RommDeck's background service and starts it.

To get it into Game Mode, pick **Add RommDeck to Steam** in the setup wizard's last step.

### Tarball

1. Download `rommdeck-<version>-x86_64.tar.gz` and extract it.
2. In a terminal, from the extracted folder:
   ```sh
   ./install.sh --steam
   ```

This puts RommDeck in `~/.local/share/rommdeck`, adds it to Desktop Mode's application menu and, with `--steam`, to your Steam library with artwork. Steam closes for a moment while the shortcut is written; it's the only way Steam lets anyone touch that file.

### How it runs

RommDeck's background service is a systemd user service (`romm-sync.service`) that starts with your session, in Game Mode and Desktop Mode alike. RommDeck is just the window into it: close the app and syncing carries on.

```sh
systemctl --user status romm-sync     # is it running?
journalctl --user -u romm-sync -f     # what's it doing?
```

Settings and pairing live in `~/.local/share/romm-sync`.

## Updates

- **Installer:** run it again and pick **Update**. Your settings and pairing stay put.
- **AppImage:** download the new one and run it. It updates the background service on launch.
- **Tarball:** extract the new one and run `./install.sh` again. Your settings and pairing stay put.

## Setup Wizard

The setup wizard walks you through five steps:

1. Connecting to your server.
2. Approving the Deck in RomM, by scanning a QR code or entering a code.
3. Choosing which emulators to sync.
4. Checking what the first sync will do.
5. Choosing when to sync in the background.

**Back up your saves before the first sync.** Copy RetroDECK's `saves` and `states` folders, EmuDeck's `Emulation/saves` and `Emulation/storage`, and any standalone emulator's save folders somewhere safe. RommDeck keeps a local copy before replacing any file, but your own backup is the safest option.

## Emulators

RommDeck doesn't care how an emulator got onto your Deck. It finds RetroDECK through its own config, EmuDeck through its settings file, and everything else by looking where Flatpaks and AppImages live (`~/Applications` included). Then it reads each emulator's **own** config to see where its games, saves and BIOS actually are, so an emulator you've pointed somewhere odd still syncs, SD card included. Move a folder, insert the card later, or change a setting, and RommDeck notices.

Two things it does for you, so you don't have to go fiddling:

- **Flatpaks that can't see your ROMs.** A Flathub Flatpak often has no access to your files beyond its own folder (Eden's doesn't). RommDeck keeps that emulator's games inside its own folder instead, so it works out of the box, no `flatpak override` needed. Grant it access yourself and RommDeck uses your ROMs folder again. EmuDeck's Flatpaks already have access.
- **Game folders.** Where an emulator keeps a list of game folders (Eden, Citron, Ryujinx, DuckStation, RMG, Snes9x, melonDS), RommDeck adds its folder to that list, only while the emulator is closed, since they all rewrite their settings on exit.

> These are the built-in profiles. You can still set the folders for any emulator or platform by hand, in the app or the Web UI, and use whatever emulator you like.

### RetroArch

RetroDECK's, the Flathub Flatpak (EmuDeck's), and a native install, side by side, each its own profile. Every system you have a core for is picked up: NES, SNES, Game Boy family, Genesis/Mega Drive, N64, PS1, arcade (FinalBurn Neo and MAME), PC Engine, Saturn, Dreamcast and friends. Saves and states sync the way RommPS syncs them.

### Standalone emulators

"RetroDECK" means the copy inside RetroDECK; "Standalone" covers EmuDeck's installs and Flatpaks or AppImages you installed yourself.

| Emulator | Systems | RetroDECK | Standalone | Saves | Comments |
| --- | --- | :---: | :---: | --- | --- |
| DuckStation | PS1 | ✓ | ✓ | One memory card per game | Cards named exactly as DuckStation names them (its game database title by default), read from your own install |
| PCSX2 | PS2 | ✓ | ✓ | Shared cards (`Mcd001.ps2`, `Mcd002.ps2`), or a game's own card | Set a game to its own card in PCSX2's game properties |
| Dolphin | GameCube, Wii | ✓ | ✓ | GameCube saves (GCI folders), Wii saves from its NAND | - |
| PrimeHack | Metroid Prime Trilogy | ✓ | ✓ | Same as Dolphin | - |
| PPSSPP | PSP | ✓ | ✓ | Each game's save folders | - |
| RPCS3 | PS3 | ✓ | ✓ | Each game's save folders | RomM's `PS3UPDAT.PUP` comes down to `bios/ps3`; install it in RPCS3 with File, Install Firmware |
| Vita3K | PS Vita | ✓ | ✓ | Each game's save folder | Games come down as RomM has them: install each once in Vita3K (RommDeck tells you). ES-DE lists them by a `.psvita` file |
| shadPS4 | PS4 | | ✓ | Each game's save folders | - |
| melonDS | DS | ✓ | ✓ | `.sav` | - |
| DeSmuME | DS | | ✓ | `.dsv`, sent to RomM as a plain cartridge save | - |
| Azahar, Citra, Lime3DS | 3DS | ✓ | ✓ | Each game's saves from the SD card folder | - |
| Cemu | Wii U | ✓ | ✓ | Each game's saves from `mlc01` | - |
| Eden, Citron | Switch | | ✓ | Your current Switch user's saves, and the device's | Games with their updates and DLC, keys and firmware. Open Eden once so it makes its user |
| Ryujinx | Switch | | ✓ | Your last-used user's saves, and the device's | Games with their updates and DLC, keys and firmware. Start each game once in Ryujinx before its save can come down (RommDeck tells you) |
| xemu | Xbox | ✓ | ✓ | From xemu's hard disk image | - |
| Xenia | Xbox 360 | | ✓ | The signed-in player's saves | EmuDeck's Proton build and the native Linux build. Sign in to a profile in Xenia first. Very much experimental on the Deck, through no fault of ours |
| Flycast | Dreamcast | | ✓ | Each game's VMU | - |
| mGBA | GBA, GBC, GB | | ✓ | `.sav` | - |
| RMG, simple64, gopher64 | N64 | | ✓ | Joined into one `.srm`, same as RetroArch's | - |
| ares | 16 systems, NES to N64 | | ✓ | Each game's save files | - |
| Snes9x, bsnes | SNES | | ✓ | `.srm` | - |
| MAME | Arcade | ✓ | ✓ | Each game's NVRAM folder | - |
| Supermodel | Sega Model 3 | | ✓ | Each game's NVRAM | EmuDeck ships a starting NVRAM for every game, which goes up as that game's first save |
| Model 2 Emulator | Sega Model 2 | | ✓ | Each game's NVRAM | EmuDeck's Proton build. Same starting-NVRAM note as Supermodel |
| BigPEmu | Atari Jaguar | | ✓ | Cartridge EEPROM | - |
| ScummVM | ScummVM | | ✓ | Each game's save slots | - |
| Stella | Atari 2600 | | ✓ | None | 2600 cartridges keep no saves. Blame 1977 |
| DOSBox Staging | DOS | | ✓ | None | DOS games save inside their own folder |

### First sync and saves you already have

A save RommDeck has never synced doesn't get to quietly replace one RomM already has, however new its timestamp looks: a fresh install, or EmuDeck's starter files, would otherwise beat years of progress. You'll see a conflict instead, and pick which to keep. A save RommDeck watched you make while the emulator ran goes up as usual.

### Save states

Save states typically can't be used across different emulators (cores), and even within the same core the format can change between versions. In my opinion, save states are for quick resume, or for saving your ass when you forgot to save your game, so they truly don't need to be synced across devices. RommDeck syncs them where the emulator can load what it gets, and leaves them alone where it can't.

### Memory cards

PS1 saves, and PS2 games on a card of their own, go up as one card per game holding only that game's saves, which is what RomM's own [argosy-sigil](https://github.com/rommapp/argosy-sigil) makes, so RommPS, Argosy and friends can load them. PCSX2's two shared cards, its default, are backed up to RomM as they are (RomM 5.3 or newer). PSP, GameCube, Wii and PS3 saves are synced as one zip per game, the same format other RomM clients use.

## Web UI

The background service also serves a Web UI at `http://localhost:8780` on the Deck, or `http://<deck-ip>:8780` from another device, with all the same features as the app. Handy when you'd rather type a server address on a real keyboard.

## Uninstall

Run the installer and pick **Uninstall**. It asks before removing your settings and pairing.

By hand:

```sh
pkill -x rommdeck                   # close the app first, or it starts the service again
systemctl --user disable --now romm-sync
rm -rf ~/.local/share/rommdeck ~/.local/share/applications/rommdeck.desktop \
       ~/.config/systemd/user/romm-sync.service ~/Applications/RommDeck.AppImage
systemctl --user daemon-reload
rm -rf ~/.local/share/romm-sync     # settings and pairing too, if you're done for good
```

Remove the Steam shortcut from your library by hand.

## Building

RommDeck's code lives in the [RommPS repository](https://github.com/s0liton/RommPS), next to the sync engine and the screens it shares with the PS5 app. The Deck-specific parts are in `platform/steamdeck/`. Everything builds in a Docker image so the binaries run on SteamOS:

```sh
tools/deck-build.sh make steamdeck                    # the background service, in build/steamdeck/
tools/deck-build.sh bash -c 'cmake -S platform/steamdeck/app -B build/steamdeck/app -G Ninja && cmake --build build/steamdeck/app'
tools/deck-build.sh platform/steamdeck/package.sh 1.0.0   # the tarball and the AppImage
tools/deck-build.sh tests/steamdeck.sh                # the service against a RetroDECK layout and a mock RomM
tools/deck-build.sh tests/steamdeck_install.sh        # the installer, with that AppImage, against a fake release
make deploy-deck DECK_HOST=<deck-ip>                  # swap the service on a Deck over SSH
```

## Watch Development Progress

I stream the development of this app and other side projects on my YouTube channel. Nothing serious, but you can catch me live.

[My Channel](https://www.youtube.com/@soliton_radar)

## Shameless Plug

<a href='https://ko-fi.com/M7G128GENT' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi4.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

## License

GPL-3.0. [SDL3](https://libsdl.org) is zlib licensed, and [stb_image](https://github.com/nothings/stb) is MIT or public domain. [cJSON](https://github.com/DaveGamble/cJSON) and [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) are MIT licensed. [Monocypher](https://monocypher.org) is CC0 or BSD-2-Clause. [argosy-sigil](https://github.com/rommapp/argosy-sigil) is MPL-2.0, with [tiny-AES-c](https://github.com/kokke/tiny-AES-c) (Unlicense).
