# Built inside the romm-sync PS5 SDK image: tools/ps5-build.sh --test (platform/ps5/docker/test)
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CURL_CFLAGS := $(shell $(PKG_CONFIG) --cflags libcurl)
CURL_LIBS   := $(shell $(PKG_CONFIG) --static --libs libcurl)

CFLAGS := -Wall -Werror -O2 $(CURL_CFLAGS)
# also compiles and links platform/ps5/platform.c against a throwaway main
PLAT_LIBS := -lSceSystemService -lSceAppInstUtil

all: curltest.elf platcheck.elf

curltest.elf: curltest.c
	$(CC) $(CFLAGS) -o $@ $< $(CURL_LIBS)

platcheck.elf: platcheck.c ../../platform.c ../../../console/console.c
	$(CC) -Wall -Werror -Wextra -Wno-unused-parameter -O2 -I../../../../src -I../../../console -o $@ $^ $(PLAT_LIBS)

clean:
	rm -f *.elf *.o
