CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra
SDL_CFLAGS := $(shell pkg-config --cflags sdl2)
SDL_LIBS   := $(shell pkg-config --libs sdl2)
BUILD   := build
INC     := -Iinclude -Isrc $(SDL_CFLAGS)

# Everything is built -fPIC so the static library can be linked into a shared
# object: that is how a plugin ships as one file with the extender inside it.
CORE_SRC := src/main.c src/host.c src/patch.c src/scan.c src/symres.c
CORE_OBJ := $(CORE_SRC:src/%.c=$(BUILD)/%.o)

.PHONY: all test clean install-headers
all: $(BUILD)/libbg3lese.a $(BUILD)/bg3lese.so

$(BUILD):
	@mkdir -p $@

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -fPIC $(INC) -c -o $@ $<

# For embedding: a plugin links this in and gets a single self-contained .so.
$(BUILD)/libbg3lese.a: $(CORE_OBJ)
	ar rcs $@ $^

# Standalone: the extender alone, loading only dynamic plugins.
$(BUILD)/bg3lese.so: $(CORE_OBJ)
	$(CC) $(CFLAGS) -shared -o $@ $^ -ldl

# ---------------------------------------------------------------- tests

$(BUILD)/test_scan: test/test_scan.c src/scan.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -o $@ $^

$(BUILD)/test_symres: test/test_symres.c src/symres.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -fPIE -pie -o $@ $^

# -fcf-protection=none because our own compiler emits the endbr64 that the
# target binary does not have; the probe must reproduce BG3's exact prologue.
$(BUILD)/test_patch: test/test_patch.c src/patch.c | $(BUILD)
	$(CC) $(CFLAGS) $(INC) -fcf-protection=none -o $@ $^

# A stand-in game plus two test plugins, to prove priority order and event
# consumption without needing BG3.
#
# --whole-archive is mandatory and not an optimisation detail: a plugin never
# *references* anything in the core, so ordinary archive semantics would drop
# main.o entirely and with it the constructor and the SDL_PollEvent interposer.
# The library would load and do nothing at all. Embedders need this same flag —
# see BG3LESE_LINK in the README.
$(BUILD)/test_dispatch.so: test/test_dispatch_plugins.c $(BUILD)/libbg3lese.a | $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared $(INC) -o $@ $< \
	  -Wl,--whole-archive $(BUILD)/libbg3lese.a -Wl,--no-whole-archive -ldl

$(BUILD)/harness: test/harness.c | $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $< $(SDL_LIBS)

test: $(BUILD)/test_scan $(BUILD)/test_symres $(BUILD)/test_patch \
      $(BUILD)/test_dispatch.so $(BUILD)/harness
	@./$(BUILD)/test_scan
	@./$(BUILD)/test_symres
	@./$(BUILD)/test_patch
	@echo
	@./test/run_dispatch_test.sh $(BUILD)

clean:
	rm -rf $(BUILD)
