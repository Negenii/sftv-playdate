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

SRC = src/main.c src/mic_ring.c src/sticky_note.c ../common/crc8.c ../common/rs.c ../common/sstv_codec.c ../common/sstv_frame.c ../common/sstv_demod.c ../common/sstv_rx.c ../common/sstv_preview.c

UINCDIR = ../common

UASRC =
UDEFS =
UADEFS =
ULIBDIR =
ULIBS =

include $(SDK)/C_API/buildsupport/common.mk

# Rebuild everything when any header changes (common.mk only tracks .c files)
$(OBJS): $(wildcard ../common/*.h) $(wildcard src/*.h)
