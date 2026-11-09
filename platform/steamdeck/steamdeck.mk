# Steam Deck build (x86_64 SteamOS, or any Linux): the sync service, built with
# the machine's own compiler and libcurl. Included by the top-level Makefile.
#
#   make steamdeck                 build/steamdeck/romm-sync
#   make deploy-deck DECK_HOST=<ip>  put it where install.sh did (~/.local/share/rommdeck) and restart the service

DECK_HOST ?= steamdeck
DECK_USER ?= deck
DECK_BIN  := build/steamdeck/romm-sync
# RommDeck ships from its own repository, on its own version numbers.
DECK_RELEASES := https://api.github.com/repos/s0liton/RommDeck/releases/latest

steamdeck: $(DECK_BIN)

build/steamdeck/gen/emulators_json.h: platform/steamdeck/emulators.json tools/embed.py
	@mkdir -p $(dir $@)
	python3 -c "import json,sys; json.load(open(sys.argv[1]))" $<
	python3 tools/embed.py $< $@ EMULATORS_JSON

$(DECK_BIN): $(SRCS) platform/steamdeck/platform.c $(HDRS) $(GEN_HEADERS) build/steamdeck/gen/emulators_json.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(COMMON_CFLAGS) -DRELEASES_URL='"$(DECK_RELEASES)"' -Ibuild/steamdeck/gen -o $@ $(SRCS) platform/steamdeck/platform.c -lcurl -lz -lpthread

deploy-deck: $(DECK_BIN)
	scp $(DECK_BIN) $(DECK_USER)@$(DECK_HOST):.local/share/rommdeck/romm-sync.new
	ssh $(DECK_USER)@$(DECK_HOST) 'mv -f .local/share/rommdeck/romm-sync.new .local/share/rommdeck/romm-sync && systemctl --user restart romm-sync.service'
