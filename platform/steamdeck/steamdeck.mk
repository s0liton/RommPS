# Steam Deck build (x86_64 SteamOS, or any Linux): the sync service, built with
# the machine's own compiler and libcurl. Included by the top-level Makefile.
#
#   make steamdeck                 build/steamdeck/romm-sync
#   make deploy-deck DECK_HOST=<ip>  copy it to the Deck over SSH and restart the service

DECK_HOST ?= steamdeck
DECK_USER ?= deck
DECK_BIN  := build/steamdeck/romm-sync

steamdeck: $(DECK_BIN)

build/steamdeck/gen/emulators_json.h: platform/steamdeck/emulators.json tools/embed.py
	@mkdir -p $(dir $@)
	python3 -c "import json,sys; json.load(open(sys.argv[1]))" $<
	python3 tools/embed.py $< $@ EMULATORS_JSON

$(DECK_BIN): $(SRCS) platform/steamdeck/platform.c $(HDRS) $(GEN_HEADERS) build/steamdeck/gen/emulators_json.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(COMMON_CFLAGS) -Ibuild/steamdeck/gen -o $@ $(SRCS) platform/steamdeck/platform.c -lcurl -lz -lpthread

deploy-deck: $(DECK_BIN)
	scp $(DECK_BIN) $(DECK_USER)@$(DECK_HOST):.local/bin/romm-sync
	ssh $(DECK_USER)@$(DECK_HOST) 'systemctl --user restart romm-sync.service'
