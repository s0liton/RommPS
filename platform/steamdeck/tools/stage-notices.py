#!/usr/bin/env python3
"""RommDeck's licences: the licenses/ folder package.sh puts beside the app and
RomM Sync. RommPS's shape (platform/ps5/app/rommps/ps5/tools/stage-notices.py),
without the git archaeology: the Deck builds from one tree, in one image.

    stage-notices.py OUT_DIR

Writes OUT_DIR (the packages' licenses/):
  README.txt        what RommDeck is under, then every part: its licence,
                    copyright, where it ends up and its source
  components.json   the same parts, as data
  <part>/...        the licence texts each part requires

The parts are what the builds compile (the Makefile's SRCS for the service,
app/CMakeLists.txt for the app) and the assets the app carries. The texts come
from this tree, from the UI kit's export (setup-kit.sh, as the app's build gets
it) and, for SDL, from licenses/SDL3, which has to match the SDL the build
image linked. Run it in that image (tools/deck-build.sh); a missing text stops it.

SPDX-License-Identifier: GPL-3.0-or-later
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

DECK = Path(__file__).resolve().parent.parent
REPO = DECK.parent.parent
TP = REPO / "third_party"
SOURCE = "https://github.com/s0liton/RommPS"


def kit():
    """The UI kit's export, and its revision: setup-kit.sh has it cached since the app's build."""
    path = subprocess.run(["bash", str(REPO / "platform/ps5/app/rommps/ps5/ui/setup-kit.sh")],
                          stdout=subprocess.PIPE, text=True, check=True).stdout.strip()
    return Path(path), (Path(path) / ".revision").read_text().split()[0]


def sdl():
    """The SDL the image linked into the app (app/docker/Dockerfile), and the commit licenses/SDL3 came from."""
    cfg = Path(os.environ.get("CMAKE_PREFIX_PATH", "/opt/sdl3").split(":")[0]) / "lib/cmake/SDL3/SDL3ConfigVersion.cmake"
    if not cfg.is_file():
        sys.exit(f"stage-notices: no SDL3 at {cfg.parent}: run it in the Deck build image (tools/deck-build.sh)")
    built = re.search(r'set\(PACKAGE_VERSION "([^"]+)"\)', cfg.read_text()).group(1)
    ours = re.match(r"SDL release-(\S+) ([0-9a-f]{40})", (DECK / "licenses/SDL3/SOURCE.txt").read_text())
    if ours.group(1) != built:
        sys.exit(f"stage-notices: the image's SDL is {built}, licenses/SDL3 is {ours.group(1)}'s: copy its texts again")
    return built, ours.group(2)


def pinned(source_txt):
    """The commit a vendored copy came from (its SOURCE.txt: "name <sha> ...")."""
    return re.match(r"\S+ ([0-9a-f]{40})", source_txt.read_text()).group(1)


def tail_licence(header):
    """A single-header library's licence, from the comment at its end (dr_flac's)."""
    text = header.read_text()
    return text[text.rindex("This software is available as a choice"):].rsplit("*/", 1)[0].rstrip() + "\n"


def main():
    out = Path(sys.argv[1])
    hui, hui_rev = kit()
    sdl_version, sdl_rev = sdl()
    build = os.environ.get("BUILD_ID") or None  # git describe, from tools/deck-build.sh
    sdl_texts = sorted(p.name for p in (DECK / "licenses/SDL3").glob("*.txt") if p.name != "SOURCE.txt")
    sdl_owner = (DECK / "licenses/SDL3/LICENSE.txt").read_text().splitlines()[0].split(" <")[0]
    chdr = TP / "libchdr"

    # id, what, licence, copyright, where it ends up, texts (file or text, name), source
    parts = [
        ("rommdeck", "RommDeck and RomM Sync: the app, its background service, the screens they share with RommPS, "
                     "the service's web UI, and an MD5 in the public domain",
         "GPL-3.0-or-later", ["Copyright (C) 2026 s0liton and contributors"], ["rommdeck", "romm-sync"],
         [(REPO / "LICENSE", "LICENSE")], (SOURCE, build)),
        ("ui-kit", "the UI kit: BlackBearReloaded's ps5-homebrew-ui with Mihawk's changes (PS5_VKHomebrewUI), its "
                   "OpenGL renderer, sound mixer, sound effects and music, with the Deck's patch "
                   "(platform/steamdeck/app/kit-deck.patch)",
         "GPL-3.0-or-later", ["Copyright (C) 2026 BlackBearReloaded", "Copyright (C) 2026 Mihawk"],
         ["rommdeck", "assets/audio/"],
         [(hui / "LICENSE", "LICENSE"), (hui / "THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md")],
         ("https://github.com/mihawk-99/PS5_VKHomebrewUI", hui_rev)),
        ("stb", "stb_vorbis (the kit's music) and stb_image (covers)", "MIT OR Unlicense",
         ["Copyright (c) 2017 Sean Barrett"], ["rommdeck"], [(hui / "src/third_party/stb/LICENSE", "LICENSE")],
         ("https://github.com/nothings/stb",
          f"stb_vorbis {(hui / 'src/third_party/stb/UPSTREAM').read_text().strip()}, stb_image 2.21")),
        ("SDL3", f"SDL {sdl_version}, linked in, with the parts of it the app uses: hidapi (controllers), "
                 "yuv2rgb, and dlmalloc in the public domain",
         "Zlib; hidapi GPL-3.0 OR BSD-3-Clause OR its original licence; yuv2rgb BSD-3-Clause",
         [sdl_owner, "Copyright (c) 2010, Alan Ott, Signal 11 Software",
          "Copyright (c) 2016, Adrien Descamps"], ["rommdeck"],
         [(DECK / "licenses/SDL3" / n, n) for n in sdl_texts],
         ("https://github.com/libsdl-org/SDL", f"release-{sdl_version} ({sdl_rev})")),
        ("cJSON", "cJSON", "MIT", ["Copyright (c) 2009-2017 Dave Gamble and cJSON contributors"],
         ["rommdeck", "romm-sync"], [(TP / "cJSON.LICENSE", "LICENSE")], ("https://github.com/DaveGamble/cJSON", None)),
        ("qrcodegen", "Project Nayuki's QR code generator (pairing, credits)", "MIT", ["Copyright (c) Project Nayuki"],
         ["rommdeck"], [(TP / "qrcodegen.LICENSE", "LICENSE")], ("https://github.com/nayuki/QR-Code-generator", None)),
        ("monocypher", "Monocypher (update signatures)", "BSD-2-Clause OR CC0-1.0",
         ["Copyright (c) 2017-2023 Loup Vaillant", "Copyright (c) 2017-2019 Michael Savage"], ["romm-sync"],
         [(TP / "monocypher.LICENSE", "LICENSE")], ("https://monocypher.org", None)),
        ("libchdr", "libchdr (CHD discs), with the LZMA SDK's decoder, zstd's decoder and dr_flac",
         "BSD-3-Clause (libchdr, zstd); public domain (LZMA SDK); Unlicense OR MIT-0 (dr_flac)",
         ["Copyright Romain Tisserand", "Copyright (c) Meta Platforms, Inc. and affiliates", "Igor Pavlov",
          "Copyright 2023 David Reid"], ["romm-sync"],
         [(chdr / "LICENSE.txt", "LICENSE.txt"), (chdr / "deps/zstd/LICENSE", "zstd-LICENSE.txt"),
          (chdr / "deps/lzma/LICENSE", "lzma-LICENSE.txt"),
          (tail_licence(chdr / "include/dr_libs/dr_flac.h"), "dr_flac-LICENSE.txt")],
         ("https://github.com/rtissera/libchdr", pinned(chdr / "SOURCE.txt"))),
        ("sigil", "argosy-sigil (memory card saves), the files of it in third_party/sigil, with tiny-AES-c",
         "MPL-2.0; tiny-AES-c Unlicense", ["the argosy-sigil contributors", "kokke (tiny-AES-c)"], ["romm-sync"],
         [(TP / "sigil/LICENSE", "LICENSE"), (TP / "sigil/third_party/tiny-AES-c/unlicense.txt", "tiny-AES-c-unlicense.txt")],
         ("https://github.com/rommapp/argosy-sigil", pinned(TP / "sigil/SOURCE.txt"))),
        ("web-ui", "the Inter font and qrcode.js, in RomM Sync's web UI", "OFL-1.1 (Inter); MIT (qrcode.js)",
         ["Copyright (c) 2016 The Inter Project Authors", "Copyright (c) 2009 Kazuhiko Arase"], ["romm-sync"],
         [(TP / "Inter-LICENSE.txt", "Inter-LICENSE.txt"), (TP / "qrcode.LICENSE", "qrcode-LICENSE.txt")],
         ("https://github.com/rsms/inter, https://github.com/kazuhikoarase/qrcode-generator", None)),
        ("fonts", "the app's typefaces, baked by the kit: Inter, Montserrat, DejaVu Sans Mono, Press Start 2P, "
                  "Patrick Hand", "OFL-1.1; DejaVu Sans Mono under the Bitstream Vera licence",
         ["(c) 2016 The Inter Project Authors", "(c) 2011 The Montserrat Project Authors", "(c) 2003 Bitstream, Inc.",
          "(c) 2012 The Press Start 2P Project Authors", "(c) 2010-2012 Patrick Wagesreiter"], ["assets/fonts/"],
         [(p, p.name) for p in sorted((hui / "assets/fonts").glob("*-LICENSE.txt"))],
         ("the kit's third_party/fonts (ui-kit above)", None)),
        ("sounds", "the menu sounds: Metal Gear Solid sound effects", "used with Konami's permission, not under "
                   "RommDeck's licence", ["(c) Konami Digital Entertainment"], ["assets/sounds/"],
         [(REPO / "platform/ps5/app/rommps/ps5/SOUNDS-LICENCE.txt", "SOUNDS-LICENCE.txt")], ("supplied by s0liton", None)),
    ]

    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    components = []
    for pid, name, licence, owners, artifacts, texts, (url, rev) in parts:
        (out / pid).mkdir()
        for text, dest in texts:
            if isinstance(text, Path):
                if not text.is_file():
                    sys.exit(f"stage-notices: {text} isn't there")
                shutil.copyfile(text, out / pid / dest)
            else:
                (out / pid / dest).write_text(text)
        components.append(dict(id=pid, name=name, licence=licence, copyright=owners, artifacts=artifacts,
                               texts=f"licenses/{pid}/", source=dict(url=url, revision=rev)))
    (out / "components.json").write_text(json.dumps(components, indent=1, ensure_ascii=False) + "\n")

    lines = ["RommDeck - licences", "===================", "",
             "RommDeck, the app, and RomM Sync, its background service, are under the GNU",
             "General Public License, version 3 or later (licenses/rommdeck/LICENSE). Their",
             f"source is at {SOURCE} (platform/steamdeck, and what it",
             "shares with RommPS); every part they're built from is below, with its licence",
             "and its source. The menu sounds are the exception: Konami's, used with",
             "permission, and not under that licence (licenses/sounds/SOUNDS-LICENCE.txt).", ""]
    for c in components:
        src = c["source"]
        lines += [c["name"], f'  licence:   {c["licence"]}', f'  copyright: {"; ".join(c["copyright"])}',
                  f'  in:        {", ".join(c["artifacts"])}', f'  texts:     {c["texts"]}',
                  f'  source:    {src["url"]}' + (f' at {src["revision"]}' if src["revision"] else ""), ""]
    lines += ["Not in the package: libcurl, OpenSSL, zlib, glibc and OpenGL, which RomM Sync",
              "and the app load from the system, each under its own licence. GCC's C++",
              "runtime is linked into the app under the GCC Runtime Library Exception."]
    (out / "README.txt").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
