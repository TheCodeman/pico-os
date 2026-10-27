cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pimoroni_pico_plus2_rp2350 CACHE STRING "Board type")
# WiFi comes from Pimoroni's RM2 breakout on the SP/CE connector, which the
# plain Pico Plus 2 board file doesn't know about; the pins are set below.
set(PICO_CYW43_SUPPORTED 1)

include(pico_sdk_import.cmake)

project(pico2_os C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(pico2_os
    src/main.c
    src/dvi.c
    src/editor.c
    src/console.c
    src/hdmi_audio.c
    src/audio.c
    src/basic/basic.c
    src/basic/basic_port_pico.c
    src/applesoft/as_core.c
    src/applesoft/as_cpu.c
    src/applesoft/as_dos.c
    src/applesoft/as_float.c
    src/applesoft/as_gfx.c
    src/applesoft/as_mach.c
    src/applesoft/as_port_pico.c
    src/applesoft/as_text.c
    src/applesoft/as_video.c
    src/apple2e/a2e.c
    src/apple2e/a2e_disk2.c
    src/apple2e/a2e_mockingboard.c
    src/apple2e/a2e_pico.c
    src/apple2e/a2e_video.c
    src/c64/c64.c
    src/c64/c64_cart.c
    src/c64/c64_cia.c
    src/c64/c64_cpu.c
    src/c64/c64_d64.c
    src/c64/c64_drive.c
    src/c64/c64_pico.c
    src/c64/c64_sid.c
    src/c64/c64_via.c
    src/c64/c64_vic.c
    src/shell.c
    src/term.c
    src/glob.c
    src/mc/mc.c
    src/mc/mc_scr.c
    src/mc/mc_ui.c
    src/mc/mc_panel.c
    src/mc/mc_ops.c
    src/mc/mc_view.c
    src/mod/mod.c
    src/mod/xm.c
    src/mod/mod_port_pico.c
    src/mod/trk.c
    src/mod/trk_gfx.c
    src/mod/trk_sample.c
    src/mod/trk_file.c
    src/mod/trk_xm.c
    src/mod/trk_xmi.c
    src/mod/trk_ui.c
    src/mod/trk_undo.c
    src/mod/mod2xm.c
    src/mouse.c
    src/mouse_cmd.c
    src/screenshot.c
    src/chime.c
    src/xfer/xfer.c
    src/stack_switch.c
    src/serial_console.c
    src/image/image.c
    src/image/lodepng.c
    src/image/tjpgd.c
    src/net/clock.c
    src/net/net_conn.c
    src/net/scp.c
    src/net/ssh_cmd.c
    src/smb/smb.c
    src/smb/smb_vfs.c
    src/smb/smb_port_pico.c
    src/net/ssh.c
    src/net/telnet.c
    src/net/telnet_menu.c
    src/net/dialdir.c
    src/net/wget.c
    src/app_overlay.c
    src/radio/radio.c
    src/radio/radio_browser.c
    src/radio/radio_ui.c
    src/net/tls.c
    src/net/wifi.c
    src/psram_heap.c
    src/usb_hub_kick.c
    src/usb_keyboard.c
    src/hid_gamepad.c
    src/ay38910.c
    src/usb_log.c
    src/usb_storage.c
    src/fatfs/ff.c
    src/fatfs/ffunicode.c
)

# tusb_config.h lives in src/; FatFs R0.15 (with our ffconf.h) in src/fatfs/.
target_include_directories(pico2_os PRIVATE src src/fatfs src/basic src/net src/image src/applesoft src/xfer src/apple2e src/c64 src/smb src/mc src/mod src/radio src/radio/helix
    src/genesis src/genesis/core/bus src/genesis/core/cpus/M68K src/genesis/core/cpus/Z80
    src/genesis/core/io src/genesis/core/savestate src/genesis/core/sound src/genesis/core/vdp)

# FatFs is third-party code; don't drown our own warnings in its.
set_source_files_properties(src/fatfs/ff.c src/fatfs/ffunicode.c src/image/lodepng.c src/image/tjpgd.c
    PROPERTIES COMPILE_OPTIONS -w)

# The Helix MP3 decoder (RealNetworks, RPSL: src/radio/helix/LICENSE.txt),
# for internet radio: third-party, at -O2, its buffers from radio.c's pool.
file(GLOB HELIX_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/radio/helix/*.c)
target_sources(pico2_os PRIVATE ${HELIX_SRCS})
set_source_files_properties(${HELIX_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w;-O2;-DHELIX_ALLOC=radio_helix_alloc;-DHELIX_FREE=radio_helix_free")
# And Helix AAC (RPSL too: src/radio/aac/LICENSE.txt), AAC-LC: its buffers
# from that pool too. Without SBR (HE-AAC's high band), whose state is too big
# for SRAM and too slow from PSRAM: HE-AAC streams play their AAC core.
file(GLOB HELIX_AAC_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/radio/aac/*.c)
list(FILTER HELIX_AAC_SRCS EXCLUDE REGEX "/sbr[^/]*\\.c$")
target_sources(pico2_os PRIVATE ${HELIX_AAC_SRCS})
set_source_files_properties(${HELIX_AAC_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w;-O2;-DHELIX_NO_SBR;-DHELIX_ALLOC=radio_helix_alloc;-DHELIX_FREE=radio_helix_free")

# The Sega Genesis emulator. Its core is AGPL-3.0 with non-commercial parts
# (THIRD_PARTY_NOTICES.md): -DPICO_OS_GENESIS=OFF builds without it.
option(PICO_OS_GENESIS "Build the Sega Genesis emulator (see THIRD_PARTY_NOTICES.md)" ON)
if(PICO_OS_GENESIS)
    target_sources(pico2_os PRIVATE
        src/genesis/genesis.c
        src/genesis/genesis_pico.c
        src/genesis/genesis_launcher.c
        src/genesis/core/bus/gwenesis_bus.c
        src/genesis/core/cpus/M68K/m68kcpu.c
        src/genesis/core/cpus/Z80/Z80.c
        src/genesis/core/io/gwenesis_io.c
        src/genesis/core/savestate/gwenesis_savestate.c
        src/genesis/core/sound/gwenesis_sn76489.c
        src/genesis/core/sound/ym2612.c
        src/genesis/core/sound/z80inst.c
        src/genesis/core/vdp/gwenesis_vdp_gfx.c
        src/genesis/core/vdp/gwenesis_vdp_mem.c
    )
    target_compile_definitions(pico2_os PRIVATE PICO_OS_GENESIS=1)
else()
    target_compile_definitions(pico2_os PRIVATE PICO_OS_GENESIS=0)
endif()

# The Genesis core (Gwenesis, src/genesis/README.md) is third-party too,
# built as retro-go builds it: the ROM used where it is loaded, at -O2.
file(GLOB GENESIS_CORE_SRCS ${CMAKE_CURRENT_LIST_DIR}/src/genesis/core/*/*.c ${CMAKE_CURRENT_LIST_DIR}/src/genesis/core/*/*/*.c)
# Its hot code goes in the genesis_fast overlay, run from SRAM
# (src/genesis/genesis_fast.h).
set_source_files_properties(${GENESIS_CORE_SRCS}
    PROPERTIES COMPILE_OPTIONS "-w;-O2;-Wno-incompatible-pointer-types;-DRETRO_GO;-include;${CMAKE_CURRENT_LIST_DIR}/src/genesis/genesis_fast.h")
# The line drawing reads its own copies of the VDP's registers, VSRAM and
# sprite table (src/genesis/genesis.c), so it can run on core 1.
set_property(SOURCE ${CMAKE_CURRENT_LIST_DIR}/src/genesis/core/vdp/gwenesis_vdp_gfx.c APPEND PROPERTY COMPILE_OPTIONS
    "-O3;-Dgwenesis_vdp_regs=genesis_r_regs;-DVSRAM=genesis_r_vsram;-DSAT_CACHE=genesis_r_sat")

target_compile_options(pico2_os PRIVATE -Wall -Wextra -Wno-unused-parameter)

# TinyUSB's RP2040/RP2350 buffer-control code, patched to wait the required
# cycles in host mode too (needed at 252 MHz; see the file), and its host
# driver, patched to run bulk transfers on EPX (a flash drive reads about ten
# times as fast; see the file).
set_source_files_properties(
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/rp2040_usb.c
    ${PICO_SDK_PATH}/lib/tinyusb/src/portable/raspberrypi/rp2040/hcd_rp2040.c
    PROPERTIES HEADER_FILE_ONLY TRUE)
target_sources(pico2_os PRIVATE src/usb_patch/rp2040_usb.c src/usb_patch/hcd_rp2040.c)

# Network shares: FatFs calls made outside ff.c go through src/smb/smb_vfs.c,
# which sends paths on a mounted share to the SMB client.
target_link_options(pico2_os PRIVATE
    -Wl,--wrap=f_open,--wrap=f_close,--wrap=f_read,--wrap=f_write,--wrap=f_lseek,--wrap=f_sync
    -Wl,--wrap=f_opendir,--wrap=f_closedir,--wrap=f_readdir,--wrap=f_stat,--wrap=f_unlink
    -Wl,--wrap=f_rename,--wrap=f_mkdir,--wrap=f_chdir,--wrap=f_getcwd,--wrap=f_gets,--wrap=f_printf)

target_link_libraries(pico2_os
    pico_rand
    pico_stdlib
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_sntp
    pico_mbedtls
    tinyusb_host
    hardware_adc
    hardware_dma
    hardware_pwm
    hardware_i2c
    hardware_spi
    hardware_irq
    hardware_psram
    hardware_xip_cache
    hardware_vreg
)

# The single USB port will run in host mode for the keyboard and flash drive,
# so the console goes over UART to the Debug Probe instead.
pico_enable_stdio_uart(pico2_os 1)

# The system clock: 252 MHz (the default, an overclock: the core voltage goes
# up to 1.20 V) or 126 MHz; both give the HDMI 640x480 pixel clock exactly.
set(OS_CLOCK_MHZ 252 CACHE STRING "System clock in MHz: 252 or 126")
if(NOT OS_CLOCK_MHZ EQUAL 126 AND NOT OS_CLOCK_MHZ EQUAL 252)
    message(FATAL_ERROR "OS_CLOCK_MHZ must be 126 or 252")
endif()

# Debug Probe UART: the board default, UART0 on GP0 (TX) and GP1 (RX).
target_compile_definitions(pico2_os PRIVATE
    OS_CLOCK_MHZ=${OS_CLOCK_MHZ}
    # Core 0's boot stack: the whole 4 KB SCRATCH_Y bank (the SDK default is
    # 2 KB). The shell then moves to a 16 KB stack of its own (main.c).
    PICO_STACK_SIZE=0x1000
    CYW43_HOST_NAME="pico-os"
    # So the radio can be retried at a slower bus clock (wifi.c).
    CYW43_PIO_CLOCK_DIV_DYNAMIC=1
    # RM2 breakout on the SP/CE connector: power GP32, CS GP33, clock GP34,
    # data (and its interrupt) GP35. Keep the debug UART off these pins.
    CYW43_DEFAULT_PIN_WL_REG_ON=32
    CYW43_DEFAULT_PIN_WL_CS=33
    CYW43_DEFAULT_PIN_WL_CLOCK=34
    CYW43_DEFAULT_PIN_WL_DATA_OUT=35
    CYW43_DEFAULT_PIN_WL_DATA_IN=35
    CYW43_DEFAULT_PIN_WL_HOST_WAKE=35
    CYW43_WL_GPIO_COUNT=3
    CYW43_WL_GPIO_LED_PIN=0
    # Check the PSRAM chip's ID at boot instead of assuming it is fitted.
    PICO_AUTO_DETECT_PSRAM_SIZE=1
    # malloc returns NULL when the heap is full (callers say "not enough
    # memory") instead of stopping the board with a panic.
    PICO_MALLOC_PANIC=0
)
pico_enable_stdio_usb(pico2_os 0)

# The framebuffer gets a section of its own, with the Genesis's SRAM code
# overlaid on its end (src/ld/section_extra_post_data.incl).
pico_add_linker_script_override_path(pico2_os ${CMAKE_CURRENT_LIST_DIR}/src/ld FILES section_extra_post_data.incl)

pico_add_extra_outputs(pico2_os)
