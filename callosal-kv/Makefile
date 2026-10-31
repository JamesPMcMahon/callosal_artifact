# Simple Makefile for callosal-kv.
#
# Targets:
#   all            build release binary (default)
#   test           build and run sanitizer-instrumented test binary
#   bench          run the local benchmark sweep
#   tidy           run clang-tidy over all sources
#   format         run clang-format over all sources
#   clean          remove build artifacts
#   setup          install toolchain deps (Ubuntu 24.04; idempotent)
#   sync           rsync tree to $(CLOUDLAB_HOST)
#   remote-<tgt>   sync, then run `make <tgt>` on $(CLOUDLAB_HOST)
#                  e.g. make remote-setup, make remote-test, make remote-bench
#
# Config file (optional, gitignored): config.env
#   CLOUDLAB_HOST, CLOUDLAB_USER, CLOUDLAB_REMOTE_DIR, CLOUDLAB_DATA_DIR
# See config.env.example.

-include config.env

CC        ?= clang
BIN       := callosal-kv
SRC_DIR   := src
TEST_DIR  := tests
BUILD     := build

SRCS      := $(wildcard $(SRC_DIR)/*.c)
LIB_SRCS  := $(filter-out $(SRC_DIR)/main.c,$(SRCS))
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)

REL_OBJS  := $(SRCS:$(SRC_DIR)/%.c=$(BUILD)/rel/%.o)
SAN_LIB   := $(LIB_SRCS:$(SRC_DIR)/%.c=$(BUILD)/san/%.o)
SAN_TESTS := $(TEST_SRCS:$(TEST_DIR)/%.c=$(BUILD)/san-tests/%.o)
TEST_BIN  := $(BUILD)/test_runner

WARN := -Wall -Wextra -Wpedantic -Werror \
        -Wconversion -Wshadow -Wstrict-prototypes -Wwrite-strings \
        -Wmissing-prototypes -Wold-style-definition -Wvla \
        -Wformat=2 -Wnull-dereference -Wdouble-promotion

#CFLAGS_BASE := -std=c2x $(WARN) -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I$(SRC_DIR)
CFLAGS_BASE := -std=c1x $(WARN) -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE -I$(SRC_DIR)
CFLAGS_REL  := $(CFLAGS_BASE) -O3 -march=native -pipe
CFLAGS_SAN  := $(CFLAGS_BASE) -O1 -g3 -fno-omit-frame-pointer \
               -fsanitize=address,undefined -fno-sanitize-recover=all

LDFLAGS := -lpthread -lm

.PHONY: all test bench sweep tidy format clean setup sync remote-%

all: $(BIN)

$(BIN): $(REL_OBJS)
	$(CC) $(CFLAGS_REL) $^ -o $@ $(LDFLAGS)

$(BUILD)/rel/%.o: $(SRC_DIR)/%.c | $(BUILD)/rel
	$(CC) $(CFLAGS_REL) -c $< -o $@

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): $(SAN_LIB) $(SAN_TESTS) | $(BUILD)
	$(CC) $(CFLAGS_SAN) $^ -o $@ $(LDFLAGS)

$(BUILD)/san/%.o: $(SRC_DIR)/%.c | $(BUILD)/san
	$(CC) $(CFLAGS_SAN) -c $< -o $@

$(BUILD)/san-tests/%.o: $(TEST_DIR)/%.c | $(BUILD)/san-tests
	$(CC) $(CFLAGS_SAN) -I$(TEST_DIR) -c $< -o $@

$(BUILD) $(BUILD)/rel $(BUILD)/san $(BUILD)/san-tests:
	mkdir -p $@

bench: $(BIN)
	scripts/sweep.sh

sweep: $(BIN)
	scripts/sweep.sh

tidy:
	@set -e; for f in $(SRCS) $(TEST_SRCS); do \
		echo "tidy $$f"; \
		clang-tidy $$f -- $(CFLAGS_BASE); \
	done

format:
	@files="$(wildcard $(SRC_DIR)/*.c $(SRC_DIR)/*.h $(TEST_DIR)/*.c $(TEST_DIR)/*.h)"; \
	if [ -n "$$files" ]; then clang-format -i $$files; fi

clean:
	rm -rf $(BUILD) $(BIN)

setup:
	scripts/setup-cloudlab.sh

sync:
	@[ -n "$(CLOUDLAB_HOST)" ] || { echo "CLOUDLAB_HOST unset; copy config.env.example to config.env and edit" >&2; exit 1; }
	scripts/sync.sh

remote-%:
	@[ -n "$(CLOUDLAB_HOST)" ] || { echo "CLOUDLAB_HOST unset; copy config.env.example to config.env and edit" >&2; exit 1; }
	scripts/sync.sh
	scripts/remote.sh make $*
