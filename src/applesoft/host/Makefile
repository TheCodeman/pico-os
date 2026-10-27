# PC build of the Applesoft interpreter and its test oracle.
#   make            builds ../../../build-host/applesoft and a2oracle
#   make test       runs the tests in tests/ (expected output in *.out)
#   make fuzz A2ROM=path/to/apple2e_enhanced_16k.rom
#                   checks the floating point against the ROM's
#   make expected A2ROM=path/to/apple2e_enhanced_16k.rom
#                   regenerates the *.out files from a real ROM
SRC     := ..
OUT     := ../../../build-host
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -I$(SRC) -I. -I../..
LIBS    := -lm

AS_SRCS := $(filter-out $(SRC)/as_port_pico.c,$(wildcard $(SRC)/*.c))
AS_HDRS := $(wildcard $(SRC)/*.h)

all: $(OUT)/applesoft $(OUT)/a2oracle $(OUT)/fpfuzz

$(OUT)/applesoft: $(AS_SRCS) as_port_host.c $(AS_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $(AS_SRCS) as_port_host.c $(LIBS)

$(OUT)/a2oracle: a2oracle.c $(SRC)/as_cpu.c $(SRC)/as_cpu.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ a2oracle.c $(SRC)/as_cpu.c

$(OUT)/fpfuzz: fpfuzz.c $(SRC)/as_float.c $(SRC)/as_cpu.c $(SRC)/as_float.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ fpfuzz.c $(SRC)/as_float.c $(SRC)/as_cpu.c $(LIBS)

$(OUT):
	mkdir -p $@

test: all
	./run_tests.sh $(OUT)

fuzz: all
	@test -n "$(A2ROM)" || { echo "set A2ROM to the enhanced IIe ROM (16 KB)"; exit 2; }
	$(OUT)/fpfuzz "$(A2ROM)" 100000

expected: all
	@test -n "$(A2ROM)" || { echo "set A2ROM to the enhanced IIe ROM (16 KB)"; exit 2; }
	./run_tests.sh $(OUT) --expected "$(A2ROM)"

.PHONY: all test expected fuzz
