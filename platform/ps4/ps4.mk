# PS4 payload (GoldHEN), built with the ps4-payload-dev SDK. Included by the
# top-level Makefile; platform/ps4/docker/Dockerfile is the SDK image.
#
# GoldHEN 2.4b18.5+ runs ELF payloads from /data/payloads through its payload
# menu, which can AutoRun them on every jailbreak. Its network loader (9090)
# crashes on ELFs, so "make install-ps4" copies the payload there over FTP and
# it's started from the menu.
#
# "make ps4-pkg" builds the home screen app (platform/ps4/app) with OpenOrbis:
# a package that copies the bundled payload to /data/payloads and shows the
# web UI. It needs OO_PS4_TOOLCHAIN, which the Docker image sets.

PS4_HOST ?= ps4
PS4_FTP ?= ftp://$(PS4_HOST):2121

PS4_PAYLOAD_SDK ?= /opt/ps4-payload-sdk
PS4_CC      := $(PS4_PAYLOAD_SDK)/bin/orbis-clang
PS4_PKGCONF := $(PS4_PAYLOAD_SDK)/bin/orbis-pkg-config
PS4_ELF     := build/ps4/romm-sync-ps4.elf
# pacbrew's Mozilla CA bundle, built into the payload so HTTPS works out of the box.
PS4_CA      := $(PS4_PAYLOAD_SDK)/target/user/homebrew/etc/ca-bundle.crt
PS4_PLAT    := platform/ps4/platform.c platform/console/console.c

ps4: $(PS4_ELF)

build/ps4/gen/cacert_pem.h: $(PS4_CA) tools/embed.py
	@mkdir -p $(dir $@)
	python3 tools/embed.py $< $@ CACERT_PEM

# Standalone emulators setup can offer (src/detect.c).
build/ps4/gen/emulators_json.h: platform/ps4/emulators.json tools/embed.py
	@mkdir -p $(dir $@)
	python3 -c "import json,sys; json.load(open(sys.argv[1]))" $<
	python3 tools/embed.py $< $@ EMULATORS_JSON

$(PS4_ELF): $(SRCS) $(PS4_PLAT) platform/console/console.h $(HDRS) $(UI_HEADER) build/ps4/gen/cacert_pem.h build/ps4/gen/emulators_json.h
	@mkdir -p $(dir $@)
	$(PS4_CC) $(COMMON_CFLAGS) -Ibuild/ps4/gen -Iplatform/console -DROMM_EMBED_CA $$($(PS4_PKGCONF) --cflags libcurl) -DCURL_STATICLIB \
		-o $@ $(SRCS) $(PS4_PLAT) \
		$$($(PS4_PKGCONF) --static --libs libcurl) -pthread -lSceSystemService

install-ps4: $(PS4_ELF)
	curl -sS --ftp-create-dirs -T $(PS4_ELF) $(PS4_FTP)/data/payloads/romm-sync-ps4.elf

# Home screen app (OpenOrbis)
OO          ?= $(OO_PS4_TOOLCHAIN)
PS4_TITLE_ID   := ROMM00002
PS4_CONTENT_ID := IV0000-$(PS4_TITLE_ID)_00-ROMMSYNCPS400000
PS4_PKG_VER    := 1.00
PS4_PKG_DIR    := build/ps4/pkg
PS4_PKG        := build/ps4/romm-sync-ps4.pkg
PS4_APP_LIBS   := -lc -lkernel -lSceSysmodule -lSceCommonDialog -lSceMsgDialog -lSceWebBrowserDialog \
                  -lSceSystemService -lSceUserService
PKGTOOL        := $(OO)/bin/linux/PkgTool.Core

.PHONY: ps4-pkg
ps4-pkg: $(PS4_PKG)

build/ps4/launcher.elf: platform/ps4/app/launcher.c
	@mkdir -p $(dir $@)
	clang --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -Wextra -Wno-unused-parameter \
		-isysroot $(OO) -isystem $(OO)/include -DAPP_VERSION='"$(or $(VERSION),dev)"' -c -o build/ps4/launcher.o $<
	ld.lld -m elf_x86_64 -pie --script $(OO)/link.x --eh-frame-hdr -L$(OO)/lib $(PS4_APP_LIBS) $(OO)/lib/crt1.o \
		-o $@ build/ps4/launcher.o

# The package tree: the app, its metadata, OpenOrbis's modules and the payload.
$(PS4_PKG): build/ps4/launcher.elf $(PS4_ELF) assets/icon0.png
	rm -rf $(PS4_PKG_DIR) && mkdir -p $(PS4_PKG_DIR)/sce_sys/about $(PS4_PKG_DIR)/sce_module
	cd $(PS4_PKG_DIR) && $(OO)/bin/linux/create-fself -in=$(CURDIR)/build/ps4/launcher.elf \
		-out=launcher.oelf --eboot eboot.bin --paid 0x3800000000000011 && rm launcher.oelf
	cp assets/icon0.png $(PS4_PKG_DIR)/sce_sys/icon0.png
	cp $(OO)/modules/right.sprx $(PS4_PKG_DIR)/sce_sys/about/
	cp $(OO)/modules/libc.prx $(OO)/modules/libSceFios2.prx $(PS4_PKG_DIR)/sce_module/
	cp $(PS4_ELF) $(PS4_PKG_DIR)/
	cd $(PS4_PKG_DIR) && f=sce_sys/param.sfo && $(PKGTOOL) sfo_new $$f \
		&& $(PKGTOOL) sfo_setentry $$f APP_TYPE --type Integer --maxsize 4 --value 1 \
		&& $(PKGTOOL) sfo_setentry $$f APP_VER --type Utf8 --maxsize 8 --value '$(PS4_PKG_VER)' \
		&& $(PKGTOOL) sfo_setentry $$f ATTRIBUTE --type Integer --maxsize 4 --value 0 \
		&& $(PKGTOOL) sfo_setentry $$f CATEGORY --type Utf8 --maxsize 4 --value 'gd' \
		&& $(PKGTOOL) sfo_setentry $$f CONTENT_ID --type Utf8 --maxsize 48 --value '$(PS4_CONTENT_ID)' \
		&& $(PKGTOOL) sfo_setentry $$f DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0 \
		&& $(PKGTOOL) sfo_setentry $$f SYSTEM_VER --type Integer --maxsize 4 --value 0 \
		&& $(PKGTOOL) sfo_setentry $$f TITLE --type Utf8 --maxsize 128 --value 'RomM Sync' \
		&& $(PKGTOOL) sfo_setentry $$f TITLE_ID --type Utf8 --maxsize 12 --value '$(PS4_TITLE_ID)' \
		&& $(PKGTOOL) sfo_setentry $$f VERSION --type Utf8 --maxsize 8 --value '$(PS4_PKG_VER)'
	cd $(PS4_PKG_DIR) && $(OO)/bin/linux/create-gp4 -out pkg.gp4 --content-id=$(PS4_CONTENT_ID) \
		--files "eboot.bin sce_sys/param.sfo sce_sys/icon0.png sce_sys/about/right.sprx sce_module/libc.prx sce_module/libSceFios2.prx romm-sync-ps4.elf" \
		&& $(PKGTOOL) pkg_build pkg.gp4 .
	mv $(PS4_PKG_DIR)/$(PS4_CONTENT_ID).pkg $@
