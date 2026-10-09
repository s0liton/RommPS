# PS5 payload (etaHEN or onionHEN), built with the ps5-payload-dev SDK. Included by the
# top-level Makefile; platform/ps5/docker/Dockerfile is the SDK image.

PS5_HOST ?= ps5
PS5_PORT ?= 9021

PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk
PS5_CC      := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
PS5_PKGCONF := $(PS5_PAYLOAD_SDK)/bin/prospero-pkg-config
PS5_ELF     := build/ps5/romm-sync.elf
# The SDK's Mozilla CA bundle, built into the payload so HTTPS works out of the box.
PS5_CA      := $(PS5_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt

ps5: $(PS5_ELF)

build/ps5/gen/cacert_pem.h: $(PS5_CA) tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ CACERT_PEM

# Standalone emulators setup can offer (src/detect.c).
build/ps5/gen/emulators_json.h: platform/ps5/emulators.json tools/embed.py
	@mkdir -p $(dir $@)
	python3 -c "import json,sys; json.load(open(sys.argv[1]))" $<
	python3 tools/embed.py $< $@ EMULATORS_JSON

PS5_PLAT := platform/ps5/platform.c platform/console/console.c

$(PS5_ELF): $(SRCS) $(PS5_PLAT) platform/console/console.h $(HDRS) $(UI_HEADER) build/ps5/gen/cacert_pem.h build/ps5/gen/emulators_json.h
	@mkdir -p $(dir $@)
	$(PS5_CC) $(COMMON_CFLAGS) -Ibuild/ps5/gen -Iplatform/console -DROMM_EMBED_CA $$($(PS5_PKGCONF) --cflags libcurl) -DCURL_STATICLIB \
		-o $@ $(SRCS) $(PS5_PLAT) \
		$$($(PS5_PKGCONF) --static --libs libcurl) -pthread -lSceSystemService -lSceAppInstUtil

deploy: $(PS5_ELF)
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)

# Copy the payload into the HEN's autostart folder over FTP, then run it.
# etaHEN's FTP server is on 2121; onionHEN's FTP plugin on 1337, with
# PS5_HEN_DIR=/data/OnionHEN.
PS5_FTP ?= ftp://$(PS5_HOST):2121
PS5_HEN_DIR ?= /data/etaHEN
install: $(PS5_ELF)
	curl -sS --ftp-create-dirs -T $(PS5_ELF) $(PS5_FTP)$(PS5_HEN_DIR)/payloads/romm-sync.elf
	printf '' | curl -sS -T - $(PS5_FTP)$(PS5_HEN_DIR)/payloads/romm-sync.elf.auto_start
	nc -w 1 $(PS5_HOST) $(PS5_PORT) < $(PS5_ELF)
