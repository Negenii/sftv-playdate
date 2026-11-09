HEAP_SIZE      = 8388208
STACK_SIZE     = 61800

PRODUCT = SFTV_RX.pdx

SDK = ${PLAYDATE_SDK_PATH}
ifeq ($(SDK),)
	SDK = $(shell egrep '^\s*SDKRoot' ~/.Playdate/config | head -n 1 | cut -c9-)
endif
ifeq ($(SDK),)
$(error SDK path not found; set ENV value PLAYDATE_SDK_PATH)
endif

VPATH += src ../common

SRC = src/main.c src/mic_ring.c src/sticky_note.c src/export.c ../common/sstv_anim.c ../common/crc8.c ../common/rs.c ../common/sstv_codec.c ../common/sstv_frame.c ../common/sstv_demod.c ../common/sstv_rx.c ../common/sstv_preview.c

UINCDIR = ../common

UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# Rebuild everything when any header changes (common.mk only tracks .c files)
$(OBJS): $(wildcard ../common/*.h) $(wildcard src/*.h)

# The Roobert fonts ship with the Playdate SDK and its license keeps them out of this GPL
# repository: they are copied from the installed SDK into Source/fonts before pdc runs (and so
# end up inside the .pdx, as the SDK allows). Source/fonts/Roobert-* is in .gitignore.
ROOBERT := Roobert-10-Bold Roobert-9-Mono-Condensed Roobert-20-Medium Roobert-11-Medium-extended
SDK_FONTS = $(SDK)/Resources/Fonts/Roobert
sdk_fonts:
	@for f in $(ROOBERT); do cp "$(SDK_FONTS)/$$f.fnt" "$(SDK_FONTS)/$$f"-table-*.png Source/fonts/ || exit 1; done
device_bin simulator_bin: sdk_fonts
.PHONY: sdk_fonts
