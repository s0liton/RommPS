# RomM Sync for PS5 and PS4

![Project Screenshot](assets/library.png)

A [RomM](https://romm.app) companion client for jailbroken PS5 consoles (etaHEN + kstuff) and PS4 consoles (GoldHEN). Through my insatiable need to mod everything electronic I encounter, I had my 12.70 PS5 jailbroken the minute that P2JB was out. I maintain a RomM server to centralize all my _legal_ ROMs, saves, and savestates which all my devices sync with, and the PS5 needs similar treatment.

- **Background save sync**
  - Monitors save file changes in the background, and uploads them as soon as they've changed. Periodic syncs also ensure that your PS5 always has the latest saves and savestates.
- **Interactive library**
  - Browse your RomM library from the installed Romm Sync app (or some other device), download games and BIOS files into the right emulator folders, and resolve save conflicts.
- **Pluggable emulator profiles**
  - RetroArch is supported out of the box. Mednafen and a PS2-ISO profile are included but disabled. I'll make sure we update as more emulators become available on the console.

## Supported Emulators and Games

Technically, this RomM client will work with all emulators, but the PS5 is currently limited on emulators available. This is simply a matter of time, and the client will be updated frequently as new emulators are released. Currently, we have focused on using mihawk-99's unofficial implementation of RetroArch. It works beautifully!

## PS4/PS5 Games

If you are interested in downloading PS4 and PS5 games from your RomM server, RomM has direct FPKGi support, simply add it as a library. This sync client does NOT handle PS4/PS5 save syncing at this time, but is being researched for potential future inclusion.

## Requirements

- A PS5 running etaHEN, or a PS4 running GoldHEN
- RomM 5.0 or newer. (5.3.0 if you want to do shared memory card syncing instead of per-game)
- RetroArch installed as a homebrew app (on the PS4, the RetroArch package that keeps its files in `/data/retroarch`)

## Install

1. Download the latest `romm-sync.elf` from the releases page.
2. Send it to the console (or use whichever favorite tool you wish):
   ```sh
   nc -w 1 <ps5-ip> 9021 < romm-sync.elf
   ```
3. A RomM Sync tile appears on the home screen after about 30 seconds. Open it, or go to `http://<ps5-ip>:8780` from another device.

To start RomM Sync with the console, copy the payload to etaHEN's autostart folder or use the autoloader. You can do this from the last setup step, or with `make install PS5_HOST=<ps5-ip>` if you build it yourself.

### PS4

Needs GoldHEN 2.4b18.5 or newer, whose payload menu runs ELF payloads.

1. Download the latest `romm-sync-ps4.pkg` from the releases page and install it with your package installer.
2. Open **RomM Sync** from the home screen. It copies its payload, `romm-sync-ps4.elf`, to `/data/payloads/`.
3. In GoldHEN Settings, open the payloader, pick `romm-sync-ps4.elf` and run it. Add it to the AutoRun queue there and it starts on every jailbreak.
4. Open **RomM Sync** again: it shows the web UI. It's also at `http://<ps4-ip>:8780` from another device.

The app only installs the payload and shows the web UI; the payload does the syncing, in the background, whether or not the app is open. Without the package, copy `romm-sync-ps4.elf` to `/data/payloads/` yourself (GoldHEN's FTP server, or `make install-ps4 PS4_HOST=<ps4-ip>`).

Don't send the payload to GoldHEN's network loader on port 9090: GoldHEN 2.4 crashes on ELFs sent there. Make sure the console's date and time are set (GoldHEN can keep them updated), or HTTPS won't work. RetroArch is found in `/data/retroarch`, with its cores in `/data/self/retroarch/cores`, as its `retroarch.cfg` says.

HTTP and HTTPS servers both work, on both consoles. Common certificate authorities (the Mozilla list) are built in, so there's nothing to copy for HTTPS. If your server uses a self-signed certificate, tick _Skip certificate checks_ during setup, or turn off _Verify TLS certificates_ in Settings. If it uses a certificate from your own CA, you can instead put that CA's certificate in `/data/romm-sync/cacert.pem`; it's trusted in addition to the built-in list.

## Updates

Settings shows when a new version is out (it checks GitHub once a day, which you can turn off) and can install it for you. Nothing installs on its own. The update waits until no sync, download or game is running, replaces the payload in etaHEN's autostart folder (keeping the old one as `romm-sync.elf.bak`), and restarts RomM Sync. On the PS4 it replaces the payload in `/data/payloads`, and the new version starts on the next jailbreak.

Each release is signed, with a manifest per console. RomM Sync only installs an update whose signature matches a key built into it, whose manifest names its own console's payload, and whose file matches the signed checksum, so a modified download (or the other console's build) is rejected.

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
- **Back up the shared card.** Needs RomM 5.3.0 or newer. The whole card is uploaded when it changes, and you can restore it from Settings. It isn't merged with other devices.

PSP, GameCube and Wii saves are synced as one zip per game, the same format other RomM clients use. Depending on the client, they may or may not support unzipping the saves (I don't truly know), but this client does!

My opinion? One card per game is better since 8MB memory cards fill up quick.

## Emulator Profiles

Setup finds RetroArch, plus the standalone emulators the console's catalog knows about (on the PS4: the pEMU family, pSNES, pNES, pGEN, pGBA and pFBN, from `platform/ps4/emulators.json`). When more than one of your emulators plays a system, setup asks which one to use for it, with a shortcut to use RetroArch for everything. Games for that system download to the chosen emulator, and its saves are the ones that sync. Settings has an "Emulator for each system" card to change it later; the others list the system under `"exclude"` in their profile.

Settings has a JSON editor for emulator profiles, and a table showing which folders are used for each platform. A profile lists an emulator's folders and the RomM platforms it plays:

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

For RetroArch, it reads save and state folders from its own config, including its "sort by core" and "sort by content" options. RomM Sync never changes the emulator's settings. A save found outside the folder the emulator reads is listed on the status page and left alone.

## Building

```sh
make host tools       # macOS or Linux build for development, plus tools/release-sign
tools/ps5-build.sh    # PS5 build in Docker, output in build/ps5/
tools/ps4-build.sh    # PS4 payload in Docker, output in build/ps4/
tools/ps4-build.sh ps4-pkg   # plus the PS4 home screen app (platform/ps4/app)
tests/e2e.sh          # end-to-end tests against a mock RomM server
```

Both consoles build from the same code in `src/`. What differs lives in `platform/<console>/`: `platform.c` (the `src/platform.h` interface), the make rules and the Docker SDK image. `platform/console/` has the parts the PS4 and PS5 share, and `platform/host/` is the development build.

## Releasing

Merge a pull request from a branch named with the version, such as `release/v1.0.2`. The release workflow tags the merge commit, builds both payloads, signs them and publishes the release. You can also push a `v1.2.3` tag yourself.

Signing needs the `RELEASE_SIGNING_KEY` repository secret: the 64-character hex key printed by `build/host/release-sign keygen`. Its public half must be in `src/update_keys.h`, which also holds a backup key that is kept offline. If the release key is ever lost or leaked, sign the next release with the backup key, and ship a new key pair in that release.

## P.S to the industry

If buying isn't **owning**, then piracy **isn't** stealing. 🖕

## License

GPL-3.0. Parts of the console code are adapted from [ftpsrv](https://github.com/ps5-payload-dev/ftpsrv) and [websrv](https://github.com/ps5-payload-dev/websrv) by John Törnblom. [cJSON](https://github.com/DaveGamble/cJSON) and [qrcode-generator](https://github.com/kazuhikoarase/qrcode-generator) are MIT licensed. [Monocypher](https://monocypher.org) is CC0 or BSD-2-Clause.
