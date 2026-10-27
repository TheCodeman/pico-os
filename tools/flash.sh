#!/bin/sh
# Flashes build/pico2_os.elf through the Debug Probe, then restarts the board
# through its watchdog. (OpenOCD's own "reset" can leave core 1, which runs
# USB and audio, asleep in the boot ROM until the next power cycle.)
#
# Before restarting it also switches halting debug off on both cores
# (DHCSR.C_DEBUGEN, which survives the reboot): while it is on, a failed
# TinyUSB assertion executes a breakpoint and stops the core instead of just
# returning.
#
# If the probe can't hold the link at 5 MHz (it happens), it tries again at
# 1 MHz.
set -e
cd "$(dirname "$0")/.."
OCD=$(ls -d ~/.pico-sdk/openocd/*/ | tail -1)
flash() {
    "$OCD/openocd" -s "$OCD/scripts" -f interface/cmsis-dap.cfg -f target/rp2350.cfg \
        -c "adapter speed $1" -c "program $2 verify" \
        -c "targets rp2350.dap.core1; write_memory 0xE000EDF0 32 0xA05F0000" \
        -c "targets rp2350.dap.core0; write_memory 0xE000EDF0 32 0xA05F0000" \
        -c "write_memory 0x40018008 32 0x01fffff3; log_output /dev/null; catch {write_memory 0x400d8000 32 0x80000000}; shutdown"
}
flash 5000 "${1:-build/pico2_os.elf}" || flash 1000 "${1:-build/pico2_os.elf}"
