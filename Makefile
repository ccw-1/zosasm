# zosasm - cross assembler for an HLASM-like language producing z/OS GOFF.
# Portable C99; the only requirement to build is a C compiler and make.
CC      ?= cc
CFLAGS  ?= -O2
WARN    ?= -Wall -Wextra -pedantic
STD     ?= -std=c99
CFLAGS_ALL = $(STD) $(WARN) $(CFLAGS)

SRC  := $(filter-out src/main.c src/hlasm2za.c,$(wildcard src/*.c))
OBJ  := $(SRC:src/%.c=build/%.o)
TESTS := $(patsubst tests/%.c,build/%,$(wildcard tests/test_*.c))

.PHONY: all test clean isa
all: build/zosasm build/hlasm2za

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

build/hlasm2za.o: src/hlasm2za.c $(wildcard src/*.h) | build
	$(CC) $(CFLAGS_ALL) -c $< -o $@

build/test_%: tests/test_%.c $(OBJ) $(wildcard tests/*.h src/*.h) | build
	$(CC) $(CFLAGS_ALL) -Isrc -o $@ $< $(OBJ) -lm

test: build/zosasm build/hlasm2za $(TESTS)
	@fail=0; for t in $(TESTS); do ./$$t || fail=1; done; \
	if [ -x tests/run_cli_tests.sh ]; then tests/run_cli_tests.sh || fail=1; fi; \
	exit $$fail

# regenerate src/isa_tab.c from isa/isa.json (isa.json itself comes from the
# Principles of Operation: python3 tools/pop2json.py SA22-7832-14.pdf > isa/isa.json)
isa: build/isagen
	build/isagen isa/isa.json > src/isa_tab.c

build/isagen: tools/isagen.c src/util.c src/util.h | build
	$(CC) $(CFLAGS_ALL) -o $@ tools/isagen.c src/util.c

clean:
	rm -rf build
