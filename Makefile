CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Werror -std=c11
PREFIX   ?= /usr/local
LIBDIR   ?= $(PREFIX)/lib
BINDIR   ?= $(PREFIX)/bin

BUILD_DIR := build
SHIM_SO   := $(BUILD_DIR)/libcz_sdl2_sym.so
TEST_BIN  := $(BUILD_DIR)/test_shim
KEYTEST   := $(BUILD_DIR)/cz_sdl2_keytest

SDL2_CFLAGS := $(shell pkg-config --cflags sdl2 2>/dev/null)
SDL2_LIBS   := $(shell pkg-config --libs sdl2 2>/dev/null)

.PHONY: all keytest test clean install uninstall

all: $(SHIM_SO)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(SHIM_SO): src/cz_sdl2_sym_shim.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -fPIC -shared -fvisibility=hidden $< -o $@ -ldl -lpthread

$(TEST_BIN): tests/test_shim.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $< -o $@

keytest: $(KEYTEST)

$(KEYTEST): tools/cz_sdl2_keytest.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SDL2_CFLAGS) $< -o $@ $(SDL2_LIBS)

test: $(SHIM_SO) $(TEST_BIN)
	LD_PRELOAD=$(abspath $(SHIM_SO)) ./$(TEST_BIN)

install: $(SHIM_SO)
	install -d $(DESTDIR)$(LIBDIR)
	install -m 0755 $(SHIM_SO) $(DESTDIR)$(LIBDIR)/libcz_sdl2_sym.so
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 bin/cz-sdl2-run $(DESTDIR)$(BINDIR)/cz-sdl2-run

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libcz_sdl2_sym.so
	rm -f $(DESTDIR)$(BINDIR)/cz-sdl2-run

clean:
	rm -rf $(BUILD_DIR)
