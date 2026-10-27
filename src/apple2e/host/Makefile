# PC build of the Apple IIe machine and its tests.
#   make            builds ../../../build-host/apple2e
#   make test       tests the USB gamepad parser, then boots the ROM and runs
#                   the tests in tests/
#                   (A2ROM= the enhanced IIe ROM, 16 KB or the 32 KB APPLE2E.ROM;
#                   the default is ROMS/APPLE2E.ROM at the top of the tree)
SRC     := ..
AS      := ../../applesoft
OUT     := ../../../build-host
A2ROM   ?= ../../../ROMS/APPLE2E.ROM
CC      ?= gcc
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -std=c11 -D_DEFAULT_SOURCE -I$(SRC) -I$(AS) -I../..

A2E_SRCS := $(SRC)/a2e.c $(SRC)/a2e_video.c $(SRC)/a2e_disk2.c $(SRC)/a2e_mockingboard.c \
            ../../ay38910.c $(AS)/as_cpu.c
A2E_HDRS := $(SRC)/a2e.h $(SRC)/a2e_disk2.h $(SRC)/a2e_mockingboard.h ../../ay38910.h $(AS)/as_cpu.h $(AS)/as_font.h

all: $(OUT)/apple2e $(OUT)/hidtest

$(OUT)/apple2e: a2e_host.c $(A2E_SRCS) $(A2E_HDRS) | $(OUT)
	$(CC) $(CFLAGS) -o $@ a2e_host.c $(A2E_SRCS)

$(OUT)/hidtest: hidtest.c ../../hid_gamepad.c ../../hid_gamepad.h | $(OUT)
	$(CC) $(CFLAGS) -I../.. -o $@ hidtest.c ../../hid_gamepad.c

$(OUT):
	mkdir -p $@

test: all
	$(OUT)/hidtest
	./run_tests.sh $(OUT)/apple2e "$(A2ROM)"

.PHONY: all test
