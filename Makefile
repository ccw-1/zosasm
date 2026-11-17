# zosasm - cross assembler for an HLASM-like language producing z/OS GOFF.
# Portable C99; the only requirement to build is a C compiler and make.
CC      ?= cc
CFLAGS  ?= -O2
WARN    ?= -Wall -Wextra -pedantic
STD     ?= -std=c99
CFLAGS_ALL = $(STD) $(WARN) $(CFLAGS)

SRC  := $(filter-out src/main.c src/hlasm2za.c src/mcp.c src/zafmt.c,$(wildcard src/*.c))
OBJ  := $(SRC:src/%.c=build/%.o)
TESTS := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

.PHONY: all test test-maclib clean isa pdf
all: build/zosasm build/hlasm2za build/zafmt build/zosasm-mcp

build:
	mkdir -p build

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/zosasm: $(OBJ) build/main.o
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/main.o: src/main.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/hlasm2za: $(OBJ) build/hlasm2za.o
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/zafmt: $(OBJ) build/zafmt.o
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/zafmt.o: src/zafmt.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/zosasm-mcp: build/mcp.o build/util.o
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/mcp.o: src/mcp.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/hlasm2za.o: src/hlasm2za.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/test_%: tests/test_%.c $(OBJ) $(wildcard tests/*.h src/*.h) | build
	$(CC) $(CFLAGS_ALL) -Isrc -o $@ $< $(OBJ) -lm

test: build/zosasm build/hlasm2za build/zafmt build/zosasm-mcp $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	if [ -x tests/run_cli_tests.sh ]; then tests/run_cli_tests.sh || fail=1; fi; \
	exit $$fail

# acceptance run over a real HLASM macro library (local only): ZASM_MACLIB=DIR make test-maclib
test-maclib: build/zosasm build/hlasm2za
	tests/maclib_check.sh

# regenerate src/isa_tab.c from isa/isa.json (isa.json itself comes from the
# Principles of Operation: python3 tools/pop2json.py SA22-7832-14.pdf > isa/isa.json)
isa: build/isagen
	build/isagen isa/isa.json > src/isa_tab.c

build/isagen: tools/isagen.c src/util.c src/util.h | build
	$(CC) $(CFLAGS_ALL) -o $@ tools/isagen.c src/util.c

# User's Guide as PDF (pandoc + XeLaTeX); the version comes from src/version.h.
# The hand written contents list is dropped, pandoc makes its own.
ZASM_VERSION := $(shell sed -n 's/.*ZASM_VERSION_STR "\(.*\)".*/\1/p' src/version.h)
pdf: docs/zosasm-users-guide.pdf
docs/zosasm-users-guide.pdf: docs/GUIDE.md docs/guide-header.tex src/version.h
	awk '/^# /&&!t{t=1;next} /^## Contents/{skip=1;next} /^### Feature index/{skip=0} !skip' docs/GUIDE.md | \
	pandoc -f markdown -o $@ --pdf-engine=xelatex --toc --toc-depth=2 \
	  --shift-heading-level-by=-1 -V documentclass=report \
	  -M title="zosasm User's Guide" -M subtitle="Version $(ZASM_VERSION)" \
	  -M author="zosasm" -M date="$(shell date +%Y-%m-%d)" \
	  -V geometry:margin=2.2cm -V colorlinks=true -V linkcolor=blue \
	  -V mainfont="DejaVu Serif" -V monofont="DejaVu Sans Mono" \
	  -V monofontoptions="Scale=0.85" -V fontsize=10pt \
	  -H docs/guide-header.tex --highlight-style=tango

clean:
	rm -rf build
