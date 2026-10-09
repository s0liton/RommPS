# romm-sync
#
#   make host                          macOS/Linux build for development, in build/host/
#   make ps5                           PS5 payload, needs the SDK (see platform/ps5/ps5.mk)
#   make ps4                           PS4 payload, needs the SDK (see platform/ps4/ps4.mk)
#   tools/ps5-build.sh, tools/ps4-build.sh   the same inside the Docker SDK image
#   make deploy PS5_HOST=<ip>          send the PS5 payload to the loader on port 9021
#   make install PS5_HOST=<ip>         same, plus copy it into the HEN's autostart over FTP (PS5_HEN_DIR)
#   make ps4-pkg                       PS4 home screen app (OpenOrbis), with the payload inside
#   make install-ps4 PS4_HOST=<ip>     copy the PS4 payload to /data/payloads over FTP (run it from GoldHEN's payload menu)
#   make ps5 VERSION=1.2.3             override the version baked into the build
#   make tools                         release-sign, for signing releases (tools/release-sign.c)
#
# Code shared by every console is in src/. Each console has its own folder in
# platform/: platform.c (src/platform.h), its make rules and its SDK image.

# libchdr (BSD-3-Clause, third_party/libchdr), for PS1 serials in .chd games:
# its CD codecs, with its LZMA and zstd decoders and the system's zlib.
CHDR_SRCS := $(filter-out %microflac.cpp,$(wildcard third_party/libchdr/src/*.c)) \
        third_party/libchdr/deps/lzma/src/LzmaDec.c third_party/libchdr/deps/zstd/zstddeclib.c
SRCS := src/main.c src/util.c src/http.c src/romm.c src/config.c src/state.c \
        src/profiles.c src/sync.c src/watch.c src/detect.c src/autostart.c src/memcard.c src/zip.c src/bundle.c src/gameid.c src/covers.c src/library.c src/pair.c src/web.c src/update.c \
        third_party/cJSON.c third_party/md5.c third_party/monocypher.c third_party/monocypher-ed25519.c \
        $(CHDR_SRCS)
HDRS := $(wildcard src/*.h) third_party/cJSON.h third_party/md5.h third_party/monocypher.h third_party/monocypher-ed25519.h
UI_HEADER := build/gen/ui_index.h build/gen/icon_png.h
COMMON_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-warning-option -Wno-unreachable-code-generic-assoc -Isrc -Ithird_party -Ibuild/gen \
        -Ithird_party/libchdr/include -DCHDR_SYSTEM_ZLIB
ifdef VERSION
COMMON_CFLAGS += -DAPP_VERSION='"$(VERSION)"'
endif
# Shown in logs and diagnostics. The Docker builds pass it in, as git can't
# read the repo from inside the container.
BUILD_ID ?= $(shell git describe --always --dirty 2>/dev/null)
ifneq ($(BUILD_ID),)
COMMON_CFLAGS += -DAPP_BUILD='"$(BUILD_ID)"'
endif
COMMON_CFLAGS += $(EXTRA_CFLAGS)

.PHONY: all host ps5 ps4 tools deploy install install-ps4 run clean

all: host

build/gen/ui_index.h: ui/index.html ui/style.css $(wildcard ui/themes/*.css) build/gen/inter-font.css third_party/qrcode.js tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ UI_INDEX_HTML

# Inter (SIL Open Font License, third_party/Inter-LICENSE.txt), inlined into the UI.
build/gen/inter-font.css: third_party/inter-latin.woff2 tools/font_css.py
	@mkdir -p $(dir $@)
	python3 tools/font_css.py $< $@ Inter

build/gen/icon_png.h: assets/icon0.png tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ ICON_PNG

# host build
HOST_CC ?= cc
HOST_BIN := build/host/romm-sync

host: $(HOST_BIN)

# Host builds also trust the test key in tests/, so the e2e test can sign updates.
$(HOST_BIN): $(SRCS) platform/host/platform.c $(HDRS) $(UI_HEADER)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(COMMON_CFLAGS) -DROMM_UPDATE_TEST_KEY -o $@ $(SRCS) platform/host/platform.c -lcurl -lz -lpthread

tools: build/host/release-sign

build/host/release-sign: tools/release-sign.c src/update_keys.h third_party/monocypher.c third_party/monocypher-ed25519.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Isrc -Ithird_party -o $@ $(filter %.c,$^)

run: host
	ROMM_SYNC_DATA=$${ROMM_SYNC_DATA:-./host-data} $(HOST_BIN)

include platform/ps5/ps5.mk
include platform/ps4/ps4.mk

clean:
	rm -rf build
