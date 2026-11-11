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
| Nothing | ProsperoStore install, open RommPS | autostart is on, with no switch to turn it off |
| Nothing | zip copied to `/data/homebrew` | same as above |
| 1.0.2 payload only, tile, etaHEN folder | in-app update from the web UI | tile stays (no RommPS), "Install RommPS" notice |
| 1.0.2 as above | install RommPS, open it | tile goes away |
| previous RommPS + payload | store update of RommPS, open it | payload swapped once idle, copies updated |
| previous RommPS + payload | payload's own update first, RommPS later | "RommPS is out of date" notice |
| any, autostart off or its flag renamed to `.off` | update, reboot | autostart is back on in every HEN folder, no `.off` left |
| any, old copy in pldmgr's autoload | update | the pldmgr copy is updated, its autoload list untouched |
| a dev build, RommPS and payload | store update of RommPS, open it | payload swapped once idle, no dev copy left in any HEN folder |

## Boot and rest mode

- Reboot, jailbreak with onionHEN only, don't open RommPS: RomM Sync starts by itself, one copy.
- Same with etaHEN.
- Rest mode, then wake: same process keeps running (uptime carries on), web UI answers.
- Open RommPS right after a jailbreak: it waits for RomM Sync instead of starting a second copy.

## Never downgrade

- Send an older `romm-sync.elf` to port 9021 while a newer one runs: it exits, the newer one keeps running.
- Upload an older payload in the web UI's autostart: the newer copies on disk stay.

## After

Every PS5 release, alone or with the PS4, ships `PPSA76677.zip`. Only a PS4-only re-release (the manual Release run) skips it, and that never touches the PS5's files.

- The release workflow builds the payloads, then RommPS (`.github/workflows/app.yml`), and publishes the release only once both have built. No zip, no release: rerun the failed job.
- The store: the catalog bot proposes the new version within 8 hours. For sooner, open the pull request yourself on the catalog (`apps/PPSA76677.json`: `version`, `artifact_url`, `sha256`, `icon_url`), from the account that owns the repository; it's merged and live within minutes.
- The Store workflow checks every hour and opens an issue if the latest release has no zip, or the store still lists the old version 12 hours after publishing.
- A release that went out without the zip anyway: `git checkout <tag> && tools/release-app.sh <tag> --upload` builds it here and attaches it.

## Dev builds

Every push to the `dev` branch publishes a dev build (`.github/workflows/dev.yml`) as the `dev-build` pre-release, in place of the last one. Its version is the next patch after the latest release plus the run number: after 1.4.2, `1.4.3-dev.12`. That's newer than 1.4.2 and older than 1.4.3, so whichever release comes next installs over it the usual way and nobody cleans up by hand. GitHub never makes a pre-release the latest release, and the in-app update and the Store workflow only look at that one, so nothing offers it to anyone who didn't go looking.

For testers:

- PS5: unzip `PPSA76677-dev.zip`, copy its `PPSA76677` folder to `/data/homebrew/` over the old one and open RommPS. It swaps in its payload once nothing is syncing, autostart copies included.
- PS4: install `romm-sync-ps4.pkg` and open RommPS, or copy `romm-sync-ps4.elf` to `/data/payloads/`. The package keeps the release's version, so if the installer won't put it over that release, copy the payload instead.
- A newer dev build goes over an older one the same way; an older one never replaces a newer one.
- When the release is out: ProsperoStore offers it on the PS5 (a dev RommPS keeps the last release's `contentVersion`), Settings offers it on the PS4.

After a release, merge `main` into `dev` before pushing to it again: the next dev build counts as newer than that release whether or not it has its fixes.

Leave alone:

- The tag: ProsperoStore's catalog bot picks up pre-releases too, and only skips a tag without a digit in it. The zip's name keeps it out of RecompStore.
- `contentVersion` on `dev` until the release is out: a dev build of 1.4.3 refuses to build with 01.004.003, since the 1.4.3 release couldn't install over it.
- `kMinPayload` and `APP_VERSION`: the dev build sets its own.
