# PC build of the Genesis core and its tests.
#   make            builds ../../../build-host/genesis and genesis_test
#   make test       runs the tests on a ROM genesis_test makes, then the
#                   harness on that ROM with timing; GENROM=file.bin also
#                   times a ROM of your own (not shipped)
SRC     := ..
CORE    := ../core
OUT     := ../../../build-host
CC      ?= gcc
CFLAGS  ?= -O2 -g
OUR_CFLAGS  := $(CFLAGS) -std=c11 -Wall -Wextra -Wno-unused-parameter
# The core is third-party code: built as retro-go builds it, without its
# warnings drowning ours.
CORE_CFLAGS := $(CFLAGS) -std=gnu11 -w -Wno-incompatible-pointer-types -DRETRO_GO
INC     := -I$(SRC) -I$(CORE)/bus -I$(CORE)/cpus/M68K -I$(CORE)/cpus/Z80 -I$(CORE)/io \
           -I$(CORE)/savestate -I$(CORE)/sound -I$(CORE)/vdp
OBJ     := $(OUT)/genesis-obj

CORE_SRCS := $(CORE)/bus/gwenesis_bus.c $(CORE)/cpus/M68K/m68kcpu.c $(CORE)/cpus/Z80/Z80.c \
             $(CORE)/io/gwenesis_io.c $(CORE)/savestate/gwenesis_savestate.c \
             $(CORE)/sound/gwenesis_sn76489.c $(CORE)/sound/ym2612.c $(CORE)/sound/z80inst.c \
             $(CORE)/vdp/gwenesis_vdp_gfx.c $(CORE)/vdp/gwenesis_vdp_mem.c
CORE_HDRS := $(wildcard $(CORE)/*/*.h $(CORE)/*/*/*.h)
CORE_OBJS := $(patsubst $(CORE)/%.c,$(OBJ)/core/%.o,$(CORE_SRCS))
OBJS      := $(CORE_OBJS) $(OBJ)/genesis.o

# The harness times the core's entry points by wrapping them.
WRAPS := m68k_run z80_run gwenesis_vdp_render_line ym2612_run YM2612Write YM2612Read \
         gwenesis_SN76489_run gwenesis_SN76489_Write
WRAP_FLAGS := $(foreach f,$(WRAPS),-Wl,--wrap=$(f))

all: $(OUT)/genesis $(OUT)/genesis_test

$(OBJ)/core/%.o: $(CORE)/%.c $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CORE_CFLAGS) $(INC) -c -o $@ $<

# The line drawing reads its own copies of the VDP's registers, VSRAM and
# sprite table (genesis.c), as on the board.
$(OBJ)/core/vdp/gwenesis_vdp_gfx.o: CORE_CFLAGS += -Dgwenesis_vdp_regs=genesis_r_regs \
	-DVSRAM=genesis_r_vsram -DSAT_CACHE=genesis_r_sat

$(OBJ)/genesis.o: $(SRC)/genesis.c $(SRC)/genesis.h $(CORE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(OUR_CFLAGS) -D_DEFAULT_SOURCE $(INC) -c -o $@ $<

$(OUT)/genesis: genesis_host.c $(OBJS)
	$(CC) $(OUR_CFLAGS) -I$(SRC) $(WRAP_FLAGS) -o $@ genesis_host.c $(OBJS) -lm

$(OUT)/genesis_test: genesis_test.c $(OBJS)
	$(CC) $(OUR_CFLAGS) -I$(SRC) -o $@ genesis_test.c $(OBJS) -lm

test: all
	$(OUT)/genesis_test
	$(OUT)/genesis_test --mkrom $(OUT)/genesis_test.bin
	$(OUT)/genesis $(OUT)/genesis_test.bin -n 600 -o $(OUT)/genesis_test.ppm
	$(OUT)/genesis $(OUT)/genesis_test.bin -n 600 -p -q
	@if [ -n "$(GENROM)" ]; then \
		$(OUT)/genesis "$(GENROM)" -n 1200 -S 200 -o $(OUT)/genesis_rom.ppm && \
		$(OUT)/genesis "$(GENROM)" -n 1200 -S 200 -p -q; fi

.PHONY: all test
