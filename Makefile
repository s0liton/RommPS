# romm-sync
#
#   make host                          macOS/Linux build for development, in build/host/
#   make ps5                           PS5 payload, needs the SDK (see tools/ps5-build.sh)
#   tools/ps5-build.sh                 "make ps5" inside the Docker SDK image
#   make deploy PS5_HOST=<ip>          send the payload to the loader on port 9021
#   make install PS5_HOST=<ip>         same, plus copy it into etaHEN's autostart over FTP
#   make ps5 VERSION=1.2.3             override the version baked into the build
#   make tools                         release-sign, for signing releases (tools/release-sign.c)

SRCS := src/main.c src/util.c src/http.c src/romm.c src/config.c src/state.c \
        src/profiles.c src/sync.c src/watch.c src/detect.c src/autostart.c src/memcard.c src/zip.c src/bundle.c src/gameid.c src/library.c src/pair.c src/web.c src/update.c \
        third_party/cJSON.c third_party/md5.c third_party/monocypher.c third_party/monocypher-ed25519.c
HDRS := $(wildcard src/*.h) third_party/cJSON.h third_party/md5.h third_party/monocypher.h third_party/monocypher-ed25519.h
UI_HEADER := build/gen/ui_index.h build/gen/icon_png.h
COMMON_CFLAGS := -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unknown-warning-option -Wno-unreachable-code-generic-assoc -Isrc -Ithird_party -Ibuild/gen
ifdef VERSION
COMMON_CFLAGS += -DAPP_VERSION='"$(VERSION)"'
endif
COMMON_CFLAGS += $(EXTRA_CFLAGS)

PS5_HOST ?= ps5
PS5_PORT ?= 9021

.PHONY: all host ps5 tools deploy install run clean

all: host

build/gen/ui_index.h: ui/index.html third_party/qrcode.js tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ UI_INDEX_HTML

build/gen/icon_png.h: assets/icon0.png tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ ICON_PNG

# host build
HOST_CC ?= cc
HOST_BIN := build/host/romm-sync

host: $(HOST_BIN)

# Host builds also trust the test key in tests/, so the e2e test can sign updates.
$(HOST_BIN): $(SRCS) src/platform_host.c $(HDRS) $(UI_HEADER)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(COMMON_CFLAGS) -DROMM_UPDATE_TEST_KEY -o $@ $(SRCS) src/platform_host.c -lcurl -lz -lpthread

tools: build/host/release-sign

build/host/release-sign: tools/release-sign.c src/update_keys.h third_party/monocypher.c third_party/monocypher-ed25519.c
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=gnu11 -O2 -Wall -Isrc -Ithird_party -o $@ $(filter %.c,$^)

run: host
	ROMM_SYNC_DATA=$${ROMM_SYNC_DATA:-./host-data} $(HOST_BIN)

# PS5 build
PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_CC      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_PKGCONF := $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
PS5_ELF     := build/ps5/romm-sync.elf
# The SDK's Mozilla CA bundle, built into the payload so HTTPS works out of the box.
PS5_CA      := $(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt

ps5: $(PS5_ELF)

build/gen/cacert_pem.h: $(PS5_CA) tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ CACERT_PEM

$(PS5_ELF): $(SRCS) src/platform_ps5.c $(HDRS) $(UI_HEADER) build/gen/cacert_pem.h
	@mkdir -p $(dir $@)
	$(PS5_CC) $(COMMON_CFLAGS) -DROMM_EMBED_CA $$($(PS5_PKGCONF) --cflags libcurl) -DCURL_STATICLIB \
		-o $@ $(SRCS) src/platform_ps5.c \
		$$($(PS5_PKGCONF) --static --libs libcurl) -pthread -lSceSystemService -lSceAppInstUtil

deploy: $(PS5_ELF)
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)

# Copy the payload into etaHEN's autostart folder over FTP (port 2121), then run it.
PS5_FTP ?= ftp://$(PS5_HOST):2121
install: $(PS5_ELF)
	curl -sS --ftp-create-dirs -T $(PS5_ELF) $(PS5_FTP)/data/etaHEN/payloads/romm-sync.elf
	printf '' | curl -sS -T - $(PS5_FTP)/data/etaHEN/payloads/romm-sync.elf.auto_start
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)

clean:
	rm -rf build
