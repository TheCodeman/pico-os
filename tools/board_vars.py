#!/usr/bin/env python3
# Reads 32-bit words at symbols over SWD without stopping the board, and
# samples each core's program counter; then switches halting debug off.
#   tools/board_vars.py build/pico2_os.elf dvi_restarts hdmi_audio_underruns
# (Use the ELF that is on the board: addresses move between builds.)
import subprocess, sys, os
import glob
OCD=sorted(glob.glob(os.path.expanduser('~/.pico-sdk/openocd/*')))[-1]; NM=sorted(glob.glob(os.path.expanduser('~/.pico-sdk/toolchain/*/bin/arm-none-eabi-nm')))[-1]
syms={l.split()[2]:l.split()[0] for l in subprocess.run([NM,sys.argv[1]],capture_output=True,text=True).stdout.splitlines() if len(l.split())==3}
args=[OCD+'/openocd','-s',OCD+'/scripts','-f','interface/cmsis-dap.cfg','-f','target/rp2350.cfg','-c','adapter speed 5000','-c','init']
for s in sys.argv[2:]:
    args+=['-c','echo {%s}'%s,'-c','mdw 0x%s'%syms[s]]
for c in '01':
    args+=['-c','targets rp2350.dap.core'+c,'-c','echo {pc%s x4}'%c,'-c','mdw 0xE000101C 4']
args+=['-c','targets rp2350.dap.core1; write_memory 0xE000EDF0 32 0xA05F0000','-c','targets rp2350.dap.core0; write_memory 0xE000EDF0 32 0xA05F0000','-c','shutdown']
out=subprocess.run(args,capture_output=True,text=True,timeout=60)
for l in (out.stdout+out.stderr).splitlines():
    if not l.startswith(('Info','Warn','Open','Licen','For bug','adapter','none sep','cortex_m','[rp')): print(l)
