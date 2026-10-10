# RomM Server Client for PS4 and PS5

![Project Screenshot](assets/library.png)

A [RomM](https://romm.app) companion client for jailbroken PS5 consoles (etaHEN or onionHEN, with kstuff) and PS4 consoles (GoldHEN). Through my insatiable need to mod everything electronic I encounter, I had my 12.70 PS5 jailbroken the minute that P2JB was out. I maintain a RomM server to centralize all my _legal_ ROMs, saves, and savestates, which all my devices sync with, and the PS5 needs similar treatment.

- **Save and sate sync**
  - Monitors save file changes in the background, and uploads them as soon as they've changed. Periodic syncs also ensure that your PS4/5 always has the latest saves and save states.
- **Interactive library**
  - Browse your RomM library from the RommPS app (or some other device via the Web UI), download games and BIOS files into the right emulator folders, and resolve save conflicts.
- **Pluggable emulator profiles and multiple emulators per platform**
  - Fully customizable sync destinations, but includes frequently updated preset profiles for existing homebrew emulators. 
  - Can sync games and saves and states for multiple emulators per platform (PS2 on RetroArch and PS5SX2 as an example) using hardlinks.
- **Beautiful and Responsive UI**
  - Based on mihawk-99's Vulkan homebrew apps and BlackBearReloaded's UI kit, we get a nice, responsive feel. So worth it, it looks 1000x better. 15+ different themes to pick from.
- **Background Service**
  - All operations are handled by a backend payload, which means syncing, downloading, and updating keep working in the background, so you can keep **gaming**.
- **RomM API Compliant**
  - RommPS is meant to strictly stick to RomM's API and method of syncing saves and states, ensuring we don't break anything and support RomM's latest features.
- **Secure and Human-Coded**
  - Each release is signed. The payload checks the signature before it updates itself, and ProsperoStore checks every download against the release's checksum. This app was primarily coded by a **human**, and will be frequently updated and maintained. AI was primarily used for explaining concepts, creating documentation, and converting data. Ya know, the tedious stuff.
- **Easter Eggs**
  - Check in the Settings, scroll the settings list, see if you find the X. This **hint** won't be here forever.

## Supported Emulators and Games

Technically, RomM Sync on both PS4 and PS5 will support **any** emulator as long as you map the game, save, and save state folders correctly during emulator setup. Most should just work, but some emulators/consoles may have weird formats, or Romm may not fully support all features. 

As a general rule, Game library syncing will pretty much work with anything. Saves/States are the more complex side of things.

### PS5 ###

| Emulator | Systems | Game Downloads | Saves | Comments |
| --- | --- | --- | --- | --- |
| [RetroArch](https://github.com/mihawk-99/PS5_RetroArch) | NES, Famicom Disk System, SNES, Game Boy, Game Boy Color, Game Boy Advance, Genesis/Mega Drive, Master System, Game Gear, Sega CD, SG-1000, arcade (FinalBurn Neo), PS2 (LRPS2), PSP (PPSSPP), GameCube and Wii (Dolphin) | Full Support | Full Support | - |
| [PSXS5](https://github.com/SynoPiia/PSXS5) | PS1 | Full Support | One memory card per game, and states | - |
| [SwanStationPS5](https://github.com/darkxex/SwanStationPS5) | PS1 | Full Support | One memory card per game, and states | - |
| [PS5SX2](https://github.com/Swordpdf/PS5SX2) | PS2 | Full Support | Its two shared memory cards, backed up to RomM (5.3 or newer) | - |
| [PS5N64](https://github.com/Globaleliteee/PS5N64) | N64 | Full Support | Full Support | - |
| [PS5NES](https://github.com/Globaleliteee/PS5NES) | NES | Broken | Broken | Broken, tried to report issue, but GH page gone |
| [Porpoise](https://github.com/elripalda/Porpoise-Dolphin-Emulator-for-PS5) | Wii/GameCube | N/A | N/A | Couldn't exit sandbox w/o crash. [Issue](https://github.com/elripalda/Porpoise-Dolphin-Emulator-for-PS5/issues/18) reported |
| [ProsperoEden](https://github.com/blackbearreloaded/ProsperoEden) | Switch | Manual Support | Researching | - |
| [PS5X360](https://github.com/BrinooTk/PS5X360) | Xbox 360 | Manual Support | Researching | - |

To give some meaning to the table above. **Full support** means RommPS can detect and properly set game/save directories for that emulator. **Manual Support** means you can use it to sync games and/or saves, but you'll need to point it to the directories yourself. This typically means that full support is coming shortly. Broken or N/A means there was a problem keeping us from sync games and saves, or the emulator just fall out doesn't work or crashes. **Researching** means I am seeing what is possible when it comes to that sync type for the given emulator or game system.

### PS4 (experimental) ###

| Emulator | Systems |
| --- | --- |
| RetroArch | The RetroArch systems above, wherever the PS4 build has the core |
| pSNES, pNES, pGEN, pGBA, pFBN | SNES; NES; Genesis, Master System, Game Gear, Sega CD, SG-1000; GBA, GBC, GB; arcade |

Only RetroArch has been tested on the PS4 so far.

## PS4/PS5 Games

If you are interested in downloading PS4 and PS5 games from your RomM server, RomM has direct FPKGi support, simply add it as a library. This sync client does NOT handle PS4/PS5 save syncing at this time, but is being researched for potential future inclusion.

## Requirements

- A PS5 running etaHEN or onionHEN, or a PS4 running GoldHEN
- RomM 5.2.0 or newer (5.3.0 if you want to do shared memory card syncing instead of per-game)
- RetroArch installed
- Whichever other emulators you want

## Install

### Install via ProsperoStore

[ProsperoStoreQRCode](assets/prospero-store-rommps.png

Install **RommPS** from [ProsperoStore](https://homebrew.page) and open it. It'll set up the background daemon, and take you through a setup wizard. That's it: RommPS carries the RomM Sync payload and starts it for you. From then on it starts with your HEN, so after a jailbreak or waking the console it's ready to go.

Not using the store? Download `PPSA76677.zip` from the releases page, unzip it and copy the `PPSA76677` folder to `/data/homebrew/` on the console.

If you'd rather run just the payload, without the app:

1. Download the latest `romm-sync.elf` from the releases page.
2. Send it to the console (or use whichever favorite tool you wish):
   ```sh
   nc -w 1 <ps5-ip> 9021 < romm-sync.elf
   ```
3. Set it up at `http://<ps5-ip>:8780` from another device.

On the PS5 the payload doesn't update itself: new versions come with RommPS. Without the app, download the new `romm-sync.elf` yourself.

RomM Sync starts with your HEN, and setup turns this on for you. Both HENs start `payloads/romm-sync.elf` when `romm-sync.elf.auto_start` sits next to it, and RomM Sync puts both in every HEN's folder on the console (`/data/etaHEN/payloads` and `/data/OnionHEN/payloads`), so switching HENs keeps working. If a payload manager like pldmgr has its own copy, that copy is kept up to date too. If you build it yourself, `make install PS5_HOST=<ps5-ip>` copies it over FTP (add `PS5_FTP=ftp://<ps5-ip>:1337 PS5_HEN_DIR=/data/OnionHEN` for onionHEN's FTP plugin).

### PS4

> **Experimental.** The PS4 build has only been tested with RetroArch so far. Standalone emulators may work, but haven't been confirmed. I spent all my focus on the PS5 build, so I didn't get to test as much, so consider it unstable and there may be gremlins for now.

Needs GoldHEN 2.4b18.5 or newer, whose payload menu runs ELF payloads.

1. Download the latest `romm-sync-ps4.pkg` from the releases page and install it with your package installer.
2. Open **RomM Sync** from the home screen. It copies its payload, `romm-sync-ps4.elf`, to `/data/payloads/`.
3. In GoldHEN Settings, open the payloader, pick `romm-sync-ps4.elf` and run it. Add it to the AutoRun queue there and it starts on every jailbreak.
4. Open **RomM Sync** again: it shows the web UI. It's also at `http://<ps4-ip>:8780` from another device.

The app only installs the payload and shows the web UI; the payload does the syncing, in the background, whether or not the app is open. Without the package, copy `romm-sync-ps4.elf` to `/data/payloads/` yourself (GoldHEN's FTP server, or `make install-ps4 PS4_HOST=<ps4-ip>`).

Don't send the payload to GoldHEN's network loader on port 9090: GoldHEN 2.4 crashes on ELFs sent there. Make sure the console's date and time are set (GoldHEN can keep them updated), or HTTPS won't work. RetroArch is found in `/data/retroarch`, with its cores in `/data/self/retroarch/cores`, as its `retroarch.cfg` says.

## Updates

On the PS5, just update RommPS: from ProsperoStore (or other homebrew storefronts eventually), or by copying the new zip's folder over the old one. The background service comes with it, and RommPS swaps it in the next time you open it, once nothing is syncing or downloading. An older version never replaces a newer one.

On the PS4, the payload updates itself from Settings.

## Setup

The setup walks you through five steps:

1. Connecting to your server.
2. Approving the console in RomM, by scanning a QR code or entering a code.
3. Choosing which emulators to sync.
4. Checking what the first sync will do.
5. Choosing when to sync in the background.

**Back up your saves before the first sync.** Copy your emulator save folders to a computer or USB drive. RomM Sync keeps a local copy before replacing any file, but your own backup is the safest option.

## PS2 Memory Cards

LRPS2 keeps every game on one shared memory card by default. RomM stores saves per game, so pick one of these during setup:

- **One card per game.** Works with any RomM version. In RetroArch, open the LRPS2 core options and turn off _Shared Memory Cards_. After that, each game's card syncs like any other save. Saves already on the shared card stay there.
- **Back up the shared card.** Needs RomM 5.3.0 or newer. The whole card is uploaded when it changes, and you can restore it from Settings in the web UI. It isn't merged with other devices.

PSP, GameCube and Wii saves are synced as one zip per game, the same format other RomM clients use. Depending on the client, they may or may not support unzipping the saves (I don't truly know), but this client does!

My opinion? One card per game is better since 8MB memory cards fill up quick.

## Emulator Profiles

I've included direct support for most of the emulators that are out right now, except for the ones that won't even launch... you can sync games, states, and saves anywhere you want, but the presets should do it for most people.

Settings in the web UI has a JSON editor for emulator profiles, and a table showing which folders are used for each platform. A profile lists an emulator's folders and the RomM platforms it plays:

```json
{
  "id": "retroarch",
  "name": "RetroArch",
  "root": "/data/homebrew/PPSA99169",
  "retroarch_cfg": "{root}/config/retroarch.cfg",
  "rom_dir": "{root}/content/{platform}",
  "save_dir": "{root}/savefiles",
  "state_dir": "{root}/savestates",
  "bios_dir": "{root}/system",
  "save_exts": ".srm,.sav",
  "platforms": [{ "romm": ["snes", "sfam"], "dir": "snes", "emulator": "snes9x", "core_name": "Snes9x" }]
}
```


## Building

```sh
make host tools       # macOS or Linux build for development, plus tools/release-sign
tools/ps5-build.sh    # PS5 build in Docker, output in build/ps5/
tools/ps4-build.sh    # PS4 payload in Docker, output in build/ps4/
tools/ps4-build.sh ps4-pkg   # plus the PS4 home screen app (platform/ps4/app)
tests/e2e.sh          # end-to-end tests against a mock RomM server
tests/autostart.sh    # autostart, version checks and HEN folders
```

Both consoles build from the same code in `src/`. What differs lives in `platform/<console>/`: `platform.c` (the `src/platform.h` interface), the make rules and the Docker SDK image. `platform/console/` has the parts the PS4 and PS5 share, and `platform/host/` is the development build.

## P.S to the industry

If buying isn't **owning**, then piracy **isn't** stealing. 🖕

## Shameless Plug

<a href='https://ko-fi.com/M7G128GENT' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://storage.ko-fi.com/cdn/kofi4.png?v=6' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a>

## License

GPL-3.0. Parts of the console code are adapted from [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) and [websrv](https://github.com/ps5-payload-dev/websrv) by John Törnblom. [cJSON](https://github.com/DaveGamble/cJSON) and [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) are MIT licensed. [Monocypher](https://monocypher.org) is CC0 or BSD-2-Clause.
