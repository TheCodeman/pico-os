# PC build of the C64 machine and its tests.
#   make            builds ../../../build-host/c64 and c64test
#   make test       runs the tests that need no ROMs, then boots the ROMs
#                   and runs the tests in tests/ (C64ROMS= the folder with the
#                   KERNAL, BASIC and character ROMs; the default is ROMS/ at
#                   the top of the tree; tests/*.args can use $$D64 and $$CRT,
#                   a test disk and cartridge c64test makes)
SRC     := ..
AS      := ../../applesoft
OUT     := ../../../build-host
C64ROMS ?= ../../../ROMS
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -I$(SRC) -I$(AS)

C64_SRCS := $(SRC)/c64.c $(SRC)/c64_cpu.c $(SRC)/c64_cia.c $(SRC)/c64_vic.c $(SRC)/c64_d64.c $(SRC)/c64_sid.c $(SRC)/c64_cart.c $(SRC)/c64_via.c $(SRC)/c64_drive.c
C64_HDRS := $(SRC)/c64.h $(SRC)/c64_cpu.h $(SRC)/c64_cia.h $(SRC)/c64_vic.h $(SRC)/c64_d64.h $(SRC)/c64_sid.h $(SRC)/c64_cart.h $(SRC)/c64_via.h $(SRC)/c64_drive.h

all: $(OUT)/c64 $(OUT)/c64test

$(OUT)/c64: c64_host.c $(C64_SRCS) $(C64_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -o $@ c64_host.c $(C64_SRCS) -lm

$(OUT)/c64test: c64_test.c $(C64_SRCS) $(C64_HDRS) $(AS)/as_cpu.c $(AS)/as_cpu.h | $(OUT)
	$(CC) $(CFLAGS) -o $@ c64_test.c $(C64_SRCS) $(AS)/as_cpu.c -lm

$(OUT):
	mkdir -p $@

test: all
	$(OUT)/c64test
	$(OUT)/c64test --mkd64 $(OUT)/test.d64
	$(OUT)/c64test --mkcrt $(OUT)/test.crt
	D64=$(abspath $(OUT)/test.d64) CRT=$(abspath $(OUT)/test.crt) ./run_tests.sh $(OUT)/c64 "$(C64ROMS)"

.PHONY: all test
