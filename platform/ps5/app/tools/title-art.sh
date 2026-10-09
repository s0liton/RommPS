#!/usr/bin/env bash
# RommPS's dashboard backgrounds: sce_sys/pic0.dds (the home screen, with the
# tile focused) and pic1.dds (launching), from make-title-art.py.
#
# Runs in the app's build image (tools/ps5-app-build.sh): the picture is drawn
# with Pillow, encoded by PS5_Vulkan's pinned bc7enc_rdo, and its DDS header
# set to the profile the console displays (the same fix-up as PS5_Vulkan's
# tools/prepare-assets.sh: one mip level, straight alpha).
#
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
app=$(dirname "$here")
vulkan=${PS5_VULKAN_DIR:-$app/../../../../ps5-stack/PS5_Vulkan}
sce_sys="$app/rommps/ps5/sce_sys"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

bash "$vulkan/tools/setup-asset-dependencies.sh" >/dev/null
encoder="$vulkan/.deps/native/bc7enc_rdo/build/bc7enc"
python3 "$here/make-title-art.py" "$work/art.png"
"$encoder" -q -g "$work/art.png" "$work/art.dds" >/dev/null
python3 - "$work/art.dds" <<'PY'
import struct, sys
from pathlib import Path
path = Path(sys.argv[1])
data = bytearray(path.read_bytes())
assert data[:4] == b"DDS " and data[84:88] == b"DX10", "not a DX10 DDS"
assert struct.unpack_from("<II", data, 12) == (2160, 3840), "not 3840x2160"
assert struct.unpack_from("<I", data, 128)[0] == 98, "not BC7_UNORM"
assert len(data) == 148 + 960 * 540 * 16, "not a single BC7 level"
struct.pack_into("<I", data, 8, struct.unpack_from("<I", data, 8)[0] | 0x20000)  # DDSD_MIPMAPCOUNT
struct.pack_into("<I", data, 24, 1)   # depth
struct.pack_into("<I", data, 28, 1)   # one mip level
struct.pack_into("<I", data, 144, 1)  # DDS_ALPHA_MODE_STRAIGHT
path.write_bytes(data)
PY
cp "$work/art.dds" "$sce_sys/pic0.dds"
cp "$work/art.dds" "$sce_sys/pic1.dds"
cp "$work/art.png" "$app/rommps/build/title-art.png" 2>/dev/null || true
echo "==> title art: $sce_sys/pic0.dds, pic1.dds"
