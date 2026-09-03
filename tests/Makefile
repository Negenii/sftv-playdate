CC ?= clang
CFLAGS ?= -std=c11 -O2 -g -Wall -Wextra -Werror -Wdouble-promotion -I../common -I../playdate/src -I. $(EXTRA_CFLAGS)
COMMON_SRC := $(wildcard ../common/*.c)
PD_SRC := $(wildcard ../playdate/src/mic_ring.c)
HOST_SRC := $(wildcard wav_io.c)
TESTS := $(patsubst %.c,%,$(wildcard test_*.c))
TOOLS := $(patsubst %.c,%,$(wildcard sstv_*.c))
BIN := bin

all: $(addprefix $(BIN)/,$(TESTS) $(TOOLS))

$(BIN):
	mkdir -p $(BIN)

$(BIN)/%: %.c $(COMMON_SRC) $(PD_SRC) $(HOST_SRC) $(wildcard ../common/*.h) $(wildcard ../playdate/src/*.h) $(wildcard *.h) | $(BIN)
	$(CC) $(CFLAGS) -o $@ $< $(COMMON_SRC) $(PD_SRC) $(HOST_SRC)

test: $(addprefix $(BIN)/,$(TESTS))
	@for t in $(TESTS); do ./$(BIN)/$$t || exit 1; done
	@echo "ALL TESTS PASSED"

clean:
	rm -rf $(BIN)

.PHONY: all test clean
