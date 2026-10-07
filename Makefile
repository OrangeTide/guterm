# Makefile : builds the guterm examples and tests
#
# guterm.h itself needs no build step. Drop it into a project and define
# GUTERM_IMPLEMENTATION in one C file. This Makefile only exists for the
# examples and tests in this repository.

CC ?= cc
CFLAGS ?= -std=c99 -O2 -g -Wall -Wextra -Wpedantic
SDL_CFLAGS := $(shell pkg-config --cflags sdl3)
SDL_LIBS := $(shell pkg-config --libs sdl3)

OUT := _out
EXAMPLES := $(OUT)/demo $(OUT)/vtdemo
TESTS := $(OUT)/test_vt

UNAME_S := $(shell uname -s 2>/dev/null)
ifneq ($(UNAME_S),)
ifneq ($(findstring MINGW,$(UNAME_S)),MINGW)
EXAMPLES += $(OUT)/term
endif
endif

.PHONY: all test clean

all: $(EXAMPLES) $(TESTS)

$(OUT):
	mkdir -p $(OUT)

$(OUT)/demo: examples/demo.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lm

$(OUT)/vtdemo: examples/vtdemo.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lm

$(OUT)/term: examples/term.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) -D_GNU_SOURCE $(SDL_CFLAGS) -I. -o $@ $< $(SDL_LIBS) -lutil -lm

$(OUT)/test_vt: tests/test_vt.c guterm.h | $(OUT)
	$(CC) $(CFLAGS) -I. -o $@ $<

test: $(TESTS)
	$(OUT)/test_vt

clean:
	rm -rf $(OUT)
