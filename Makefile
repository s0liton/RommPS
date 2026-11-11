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
#   make ps4-pkg NETLOG=1              test build that broadcasts its log on the LAN (tools/netlog.py)
#   make tools                         release-sign, for signing releases (tools/release-sign.c)
#
# Code shared by every console is in src/. Each console has its own folder in
# platform/: platform.c (src/platform.h), its make rules and its SDK image.

# libchdr (BSD-3-Clause, third_party/libchdr), for PS1 serials in .chd games:
# its CD codecs, with its LZMA and zstd decoders and the system's zlib.
CHDR_SRCS := $(filter-out %microflac.cpp,$(wildcard third_party/libchdr/src/*.c)) \
        third_party/libchdr/deps/lzma/src/LzmaDec.c third_party/libchdr/deps/zstd/zstddeclib.c
# argosy-sigil (MPL-2.0), a copy of the files used (third_party/sigil/SOURCE.txt),
# for PS2 memory card saves one game at a time (src/ps2card.c): its save sync
# only. Its disc readers and sigil.c would link a second libchdr, so they're left
# out, and ps2card.c stands in for sigil_platform_from_slug. tiny-AES-c
# (Unlicense) is for its PS1 cards, and Switch NCA headers (src/switchid.c).
SIGIL := third_party/sigil
SIGIL_SRCS := $(addprefix $(SIGIL)/src/,sync.c sync_state.c sync_kinds.c sync_kind_sony.c sync_kind_sega.c \
        sync_kind_gamecube.c sync_units.c sync_cards.c sync_folder_cards.c sync_volumes.c sync_gci_folder.c \
        sync_profiles.c sync_gb.c sync_n64.c save_layout.c save_unit.c save_profiles.c save_names.c card.c \
        card_ps1.c card_ps2.c card_gamecube.c card_dreamcast.c card_saturn.c card_segacd.c zip_mem.c \
        zip_entries.c md5.c sha1.c io_file.c n64_save.c clock_gb.c rzip.c sfo_parser.c psp_sfo.c) \
        $(SIGIL)/third_party/tiny-AES-c/aes.c
SRCS := src/main.c src/util.c src/http.c src/clientcert.c src/romm.c src/config.c src/state.c \
        src/profiles.c src/sync.c src/watch.c src/detect.c src/ra2.c src/playlist.c src/autostart.c src/memcard.c src/zip.c src/bundle.c src/gcmem.c src/statefmt.c src/events.c src/gameid.c src/covers.c src/library.c src/firmware.c src/pair.c src/web.c src/update.c src/presets.c src/jsonlean.c src/ps2card.c src/xboxhdd.c src/n64.c src/switchid.c \
        third_party/cJSON.c third_party/md5.c third_party/monocypher.c third_party/monocypher-ed25519.c \
        $(CHDR_SRCS) $(SIGIL_SRCS)
HDRS := $(wildcard src/*.h) third_party/cJSON.h third_party/md5.h third_party/monocypher.h third_party/monocypher-ed25519.h \
        $(wildcard third_party/sigil/src/*.h) third_party/sigil/include/sigil.h
GEN_HEADERS := build/gen/ui_index.h build/gen/retroarch_cores_json.h
COMMON_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-warning-option -Wno-unreachable-code-generic-assoc -Isrc -Ithird_party -Ibuild/gen \
        -Ithird_party/libchdr/include -DCHDR_SYSTEM_ZLIB \
        -Ithird_party/sigil/include -Ithird_party/sigil/src -Ithird_party/sigil/third_party/tiny-AES-c -Ithird_party/libchdr/deps/zstd \
        -DAES128=1 -DCBC=0 -DCTR=1 -DECB=1
ifdef VERSION
COMMON_CFLAGS += -DAPP_VERSION='"$(VERSION)"'
endif
# Shown in logs and diagnostics. The Docker builds pass it in, as git can't
# read the repo from inside the container.
BUILD_ID ?= $(shell git describe --always --dirty 2>/dev/null)
ifneq ($(BUILD_ID),)
COMMON_CFLAGS += -DAPP_BUILD='"$(BUILD_ID)"'
endif
# Test builds: NETLOG=1 also broadcasts the log on the LAN, curl's details
# included. tools/netlog.py shows it. Never in a release.
ifdef NETLOG
COMMON_CFLAGS += -DROMM_NETLOG
endif
COMMON_CFLAGS += $(EXTRA_CFLAGS)

.PHONY: unit all host ps5 ps4 tools deploy install install-ps4 run clean

all: host

build/gen/ui_index.h: ui/index.html ui/style.css $(wildcard ui/themes/*.css) build/gen/inter-font.css third_party/qrcode.js tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ UI_INDEX_HTML

# What RetroArch's cores need beyond the defaults (src/detect.c), on every console.
build/gen/retroarch_cores_json.h: platform/retroarch_cores.json tools/embed.py
	@mkdir -p $(dir $@)
	python3 -c "import json,sys; json.load(open(sys.argv[1]))" $<
	python3 tools/embed.py $< $@ RETROARCH_CORES_JSON

# Inter (SIL Open Font License, third_party/Inter-LICENSE.txt), inlined into the UI.
build/gen/inter-font.css: third_party/inter-latin.woff2 tools/font_css.py
	@mkdir -p $(dir $@)
	python3 tools/font_css.py $< $@ Inter

# host build
HOST_CC ?= cc
HOST_BIN := build/host/romm-sync

host: $(HOST_BIN)

# OpenSSL, where pkg-config finds it, for client certificates (mTLS); the
# consoles always have it.
HOST_SSL := $(shell pkg-config --exists openssl 2>/dev/null && echo -DROMM_OPENSSL `pkg-config --cflags --libs openssl`)

# Host builds also trust the test key in tests/, so the e2e test can sign updates.
$(HOST_BIN): $(SRCS) platform/host/platform.c $(HDRS) $(GEN_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(COMMON_CFLAGS) -DROMM_UPDATE_TEST_KEY -o $@ $(SRCS) platform/host/platform.c -lcurl $(HOST_SSL) -lz -lpthread

# Unit tests for the parts with file formats of their own.
unit: build/host/gcmem_test build/host/jsonlean_cli build/host/ps2card_test build/host/gameid_cli build/host/switchid_test
	build/host/gcmem_test
	build/host/ps2card_test
	tests/gameid_test.sh build/host/gameid_cli
	build/host/switchid_test
	python3 tests/jsonlean_test.py build/host/jsonlean_cli

build/host/jsonlean_cli: tests/jsonlean_cli.c src/jsonlean.c src/jsonlean.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o $@ tests/jsonlean_cli.c src/jsonlean.c

build/host/gcmem_test: tests/gcmem_test.c src/gcmem.c src/gcmem.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -g -Wall -Wextra -fsanitize=address,undefined -Isrc -o $@ tests/gcmem_test.c src/gcmem.c

build/host/gameid_cli: tests/gameid_cli.c src/gameid.c src/gameid.h src/util.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(filter-out -O2,$(COMMON_CFLAGS)) -g -fsanitize=address,undefined -o $@ tests/gameid_cli.c src/gameid.c src/util.c third_party/cJSON.c \
		third_party/md5.c $(CHDR_SRCS) -lz -lpthread

build/host/ps2card_test: tests/ps2card_test.c src/ps2card.c src/ps2card.h src/util.c src/zip.c $(SIGIL_SRCS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(filter-out -O2,$(COMMON_CFLAGS)) -g -fsanitize=address,undefined -o $@ $(filter %.c,$^) \
		third_party/md5.c third_party/libchdr/deps/zstd/zstddeclib.c -lz -lpthread

# Switch title ids, from made-up files under a made-up key: no Nintendo data here.
build/host/switchid_test: tests/switchid_test.c src/switchid.c src/switchid.h src/util.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(filter-out -O2,$(COMMON_CFLAGS)) -g -fsanitize=address,undefined -o $@ $(filter %.c,$^) \
		third_party/sigil/third_party/tiny-AES-c/aes.c third_party/md5.c -lpthread

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
