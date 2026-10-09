# Release checklist

Run all of this on a real console before tagging. Tests in CI (`tests/e2e.sh`, `tests/autostart.sh`) cover the logic, not the console.

## Before

- `kMinPayload` in `rommps_app.hpp`, `contentVersion` in RommPS's `param.json` and `APP_VERSION` in `src/util.h` all match the tag.
- `docs/releases/<tag>.md` is written. ProsperoStore shows it as the release notes.
- If this release changes who owns something on the console (the home screen icon, starting the payload, the autostart files, updates, the data folder), the old one gets removed or migrated in this same release.

## Upgrades

For each one, start from the old version actually installed and set up, then update. Check afterwards:

- one icon on the home screen (RommPS), no "RomM Sync" tile
- one RomM Sync running, the new version (`/api/status`), lock file says the same
- every HEN folder that exists has the new `romm-sync.elf` and `.auto_start` (`/api/autostart` lists the copies and their versions)
- a pldmgr copy, if there was one, is the new version
- no `.bak` files left in the payload folders
- a sync runs and finishes without errors
- no RomM Sync notifications while RommPS is installed

| From | How | Also check |
| --- | --- | --- |
| Nothing | ProsperoStore install, open RommPS | setup turns autostart on by default |
| Nothing | zip copied to `/data/homebrew` | same as above |
| 1.0.2 payload only, tile, etaHEN folder | in-app update from the web UI | tile stays (no RommPS), "Install RommPS" notice |
| 1.0.2 as above | install RommPS, open it | tile goes away |
| previous RommPS + payload | store update of RommPS, open it | payload swapped once idle, copies updated |
| previous RommPS + payload | payload's own update first, RommPS later | "RommPS is out of date" notice |
| any, autostart off | update | autostart stays off, nothing added to HEN folders |
| any, old copy in pldmgr's autoload | update | the pldmgr copy is updated, its autoload list untouched |

## Boot and rest mode

- Reboot, jailbreak with onionHEN only, don't open RommPS: RomM Sync starts by itself, one copy.
- Same with etaHEN.
- Rest mode, then wake: same process keeps running (uptime carries on), web UI answers.
- Open RommPS right after a jailbreak: it waits for RomM Sync instead of starting a second copy.

## Never downgrade

- Send an older `romm-sync.elf` to port 9021 while a newer one runs: it exits, the newer one keeps running.
- Upload an older payload in the web UI's autostart: the newer copies on disk stay.

## After

- The release workflow published the payloads, then `tools/release-app.sh <tag> --upload` attached `PPSA76677.zip`.
- The catalog bot's pull request for the new version shows up the next day.
