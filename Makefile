# zosasm - cross assembler for an HLASM-like language producing z/OS GOFF.
# Portable C99; the only requirement to build is a C compiler and make.
#
# build/libzosasm.a holds the assembler core; the four programs and the test
# binaries link it. Dependencies are tracked per object with -MMD -MP (the
# .d files in build/), so touching a header rebuilds only what includes it.
CC      ?= cc
AR      ?= ar
CFLAGS  ?= -O2
WARN    ?= -Wall -Wextra -pedantic
STD     ?= -std=c99
CFLAGS_ALL = $(STD) $(WARN) $(CFLAGS) -MMD -MP

LIBSRC := $(filter-out src/main.c src/hlasm2za.c src/mcp.c src/zafmt.c,$(wildcard src/*.c))
OBJ    := $(LIBSRC:src/%.c=build/%.o)
TESTS  := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

.PHONY: all test test-maclib test-mcp-asan clean isa isa-check pdf
all: build/zosasm build/hlasm2za build/zafmt build/zosasm-mcp

build:
	mkdir -p build

build/%.o: src/%.c | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/libzosasm.a: $(OBJ)
	$(AR) rcs $@ $^

build/zosasm: build/main.o build/libzosasm.a
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/hlasm2za: build/hlasm2za.o build/libzosasm.a
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/zafmt: build/zafmt.o build/libzosasm.a
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

# the MCP server keeps its own minimal core (mcp.c + util.c): it must never
# grow assembler state
build/zosasm-mcp: build/mcp.o build/util.o
	$(CC) $(CFLAGS_ALL) -o $@ $^ -lm

build/test_%: tests/test_%.c build/libzosasm.a
	$(CC) $(CFLAGS_ALL) -Isrc -MF $(@:%=%.d) -o $@ $< build/libzosasm.a -lm

test: build/zosasm build/hlasm2za build/zafmt build/zosasm-mcp $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	if [ -x tests/run_cli_tests.sh ]; then tests/run_cli_tests.sh || fail=1; fi; \
	$(MAKE) isa-check || fail=1; \
	exit $$fail

# acceptance run over a real HLASM macro library (local only): ZASM_MACLIB=DIR make test-maclib
test-maclib: build/zosasm build/hlasm2za
	tests/maclib_check.sh

# MCP server leak check: build the test harness against src/mcp.c+util.c with
# ASan/UBSan and require leak detection. Separate from core tests because
# assembler-core process-lifetime allocations are intentional (one-shot CLI).
# MCP is long-lived, so per-request/request-scope leaks are bugs; process-
# lifetime caches (guide/tools_json/workspace strings) are reset in locate_reset().
test-mcp-asan: build/zosasm-mcp
	$(CC) $(STD) $(WARN) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
		-DZA_MCP_NO_MAIN -Isrc -I. -o build/test_mcp_asan tests/test_mcp.c src/util.c -lm \
		-fsanitize=address,undefined
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:halt_on_error=1 ./build/test_mcp_asan

# regenerate src/isa_tab.c from isa/isa.json (isa.json itself comes from the
# Principles of Operation: python3 tools/pop2json.py SA22-7832-14.pdf > isa/isa.json)
isa: build/isagen
	build/isagen isa/isa.json > src/isa_tab.c

# self-check: regenerating src/isa_tab.c from isa/isa.json must produce
# identical output (the checked-in file is the source of truth)
isa-check: build/isagen
	@build/isagen isa/isa.json > build/isa_tab.check
	@if cmp -s build/isa_tab.check src/isa_tab.c; then \
		echo "isa-check: src/isa_tab.c is in sync with isa/isa.json"; \
	else \
		echo "isa-check: src/isa_tab.c differs from regenerated output; run 'make isa'"; \
		diff src/isa_tab.c build/isa_tab.check | head -20; \
		exit 1; \
	fi
	@rm -f build/isa_tab.check

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

-include $(wildcard build/*.d)
