# Makefile : builds the guterm examples and tests
#
# guterm.h itself needs no build step. Drop it into a project and define
# GUTERM_IMPLEMENTATION in one C file. This Makefile only exists for the
# examples and tests in this repository.
#
# Targets:
#   all      examples and tests
#   test     unit tests and a short torture run
#   configs  compile the header with each feature left out
#   release-check  the header, manual, tag and tree agree for a release
#   torture  longer torture run (TORTURE_ITER and TORTURE_SEED)
#   asan     tests under address sanitizer
#   ubsan    tests under undefined behavior sanitizer
#   cov      tests with gcov, report in _out/cov/
#   clean

CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

OUT := _out
EXAMPLES := $(OUT)/demo $(OUT)/vtdemo $(OUT)/reversi
TESTS := $(OUT)/test_vt $(OUT)/torture

TORTURE_ITER ?= 200000
TORTURE_SEED ?= 1

UNAME_S := $(shell uname -s 2>/dev/null)
ifneq ($(UNAME_S),)
ifneq ($(findstring MINGW,$(UNAME_S)),MINGW)
EXAMPLES += $(OUT)/term
endif
endif

ASAN_FLAGS := -O1 -g -fsanitize=address -fno-omit-frame-pointer
UBSAN_FLAGS := -O1 -g -fsanitize=undefined -fno-sanitize-recover=all
COV_FLAGS := -O0 -g --coverage

.PHONY: all test configs torture asan ubsan cov release-check clean

all: $(EXAMPLES) $(TESTS)

$(OUT) $(OUT)/asan $(OUT)/ubsan $(OUT)/cov:
	mkdir -p $@

$(OUT)/demo: examples/demo.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lm

$(OUT)/vtdemo: examples/vtdemo.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lm

$(OUT)/reversi: examples/reversi.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lm

$(OUT)/term: examples/term.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) -pthread -D_GNU_SOURCE $(SDL_CFLAGS) -I. -o $@ $< \
	    $(SDL_LIBS) -lutil -lm

$(OUT)/test_vt: tests/test_vt.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) -I. -o $@ $<

$(OUT)/torture: tests/torture.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) -I. -o $@ $<

test: $(TESTS) configs
	$(OUT)/test_vt
	$(OUT)/torture 20000 $(TORTURE_SEED)

# Every feature macro, alone and all together, must compile cleanly.
# GUTERM_NO_WINDOW builds need no SDL, the others only its headers.
CONFIGS := GUTERM_NO_WINDOW GUTERM_NO_VT GUTERM_NO_IMAGES GUTERM_NO_SIXEL \
    GUTERM_NO_GAMEPAD GUTERM_NO_DEFAULT_FONT

configs: | $(OUT)
	printf '#define GUTERM_IMPLEMENTATION\n#include "guterm.h"\n' \
	    > $(OUT)/config.c
	for m in $(CONFIGS) all; do \
	    if [ $$m = all ]; then flags="$(CONFIGS:%=-D%)"; else flags=-D$$m; fi; \
	    echo "configs: $$flags"; \
	    $(CC) $(CFLAGS) -Werror $$flags $(SDL_CFLAGS) -I. -c \
	        -o $(OUT)/config.o $(OUT)/config.c || exit 1; \
	done

torture: $(OUT)/torture
	$(OUT)/torture $(TORTURE_ITER) $(TORTURE_SEED)

# Sanitizer and coverage builds go in their own directories so they never
# mix with the normal objects.

$(OUT)/asan/test_vt: tests/test_vt.c guterm.h | $(OUT)/asan
	$(CC) -std=c99 -Wall -Wextra $(ASAN_FLAGS) -I. -o $@ $<

$(OUT)/asan/torture: tests/torture.c guterm.h | $(OUT)/asan
	$(CC) -std=c99 -Wall -Wextra $(ASAN_FLAGS) -I. -o $@ $<

asan: $(OUT)/asan/test_vt $(OUT)/asan/torture
	ASAN_OPTIONS=detect_leaks=1 $(OUT)/asan/test_vt
	ASAN_OPTIONS=detect_leaks=1 $(OUT)/asan/torture 50000 $(TORTURE_SEED)

$(OUT)/ubsan/test_vt: tests/test_vt.c guterm.h | $(OUT)/ubsan
	$(CC) -std=c99 -Wall -Wextra $(UBSAN_FLAGS) -I. -o $@ $<

$(OUT)/ubsan/torture: tests/torture.c guterm.h | $(OUT)/ubsan
	$(CC) -std=c99 -Wall -Wextra $(UBSAN_FLAGS) -I. -o $@ $<

ubsan: $(OUT)/ubsan/test_vt $(OUT)/ubsan/torture
	UBSAN_OPTIONS=print_stacktrace=1 $(OUT)/ubsan/test_vt
	UBSAN_OPTIONS=print_stacktrace=1 $(OUT)/ubsan/torture 50000 \
	    $(TORTURE_SEED)

$(OUT)/cov/test_vt: tests/test_vt.c guterm.h | $(OUT)/cov
	$(CC) -std=c99 $(COV_FLAGS) -I. -o $@ $<

$(OUT)/cov/torture: tests/torture.c guterm.h | $(OUT)/cov
	$(CC) -std=c99 $(COV_FLAGS) -I. -o $@ $<

cov: $(OUT)/cov/test_vt $(OUT)/cov/torture
	rm -f $(OUT)/cov/*.gcda $(OUT)/cov/*.gcov
	cd $(OUT)/cov && ./test_vt && ./torture 50000 $(TORTURE_SEED)
	gcov -o $(OUT)/cov $(OUT)/cov/*.gcda | \
	    awk '/^File .*guterm\.h/ {f = 1; next} f {print; exit}'
	mv *.gcov $(OUT)/cov/
	@echo "line report: $(OUT)/cov/guterm.h.gcov"

# A release is the version in guterm.h, repeated by the manual, tagged as
# v<version> at HEAD on a clean tree. Cut one with: set GUT_VERSION and
# the manual line, commit "version x.y.z", tag -a vx.y.z, then run this.
release-check:
	@v=$$(sed -n 's/^#define GUT_VERSION "\(.*\)"/\1/p' guterm.h); \
	m=$$(sed -n 's/^This manual covers version \([0-9.]*\) of.*/\1/p' guterm.md); \
	if [ -z "$$v" ]; then \
	    echo "release-check: no GUT_VERSION in guterm.h" >&2; exit 1; \
	fi; \
	if [ "$$m" != "$$v" ]; then \
	    echo "release-check: guterm.md says '$$m', guterm.h says $$v" >&2; \
	    exit 1; \
	fi; \
	if ! git rev-parse --verify --quiet "refs/tags/v$$v" >/dev/null; then \
	    echo "release-check: no tag v$$v" >&2; exit 1; \
	fi; \
	if [ "$$(git rev-parse "v$$v^{commit}")" != "$$(git rev-parse HEAD)" ]; then \
	    echo "release-check: tag v$$v is not at HEAD" >&2; exit 1; \
	fi; \
	if [ -n "$$(git status --porcelain)" ]; then \
	    echo "release-check: working tree is dirty" >&2; exit 1; \
	fi; \
	echo "release-check: v$$v ok"

clean:
	rm -rf $(OUT)
