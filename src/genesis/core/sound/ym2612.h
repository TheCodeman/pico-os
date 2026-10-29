/*
**
** software implementation of Yamaha FM sound generator (YM2612/YM3438)
**
** Original code (MAME fm.c)
**
** Copyright (C) 2001, 2002, 2003 Jarek Burczynski (bujar at mame dot net)
** Copyright (C) 1998 Tatsuyuki Satoh , MultiArcadeMachineEmulator development
**
** Version 1.4 (final beta)
**
** Additional code & fixes by Eke-Eke for Genesis Plus GX
**
*/

#ifndef _H_YM2612_
#define _H_YM2612_

extern int16_t *gwenesis_ym2612_buffer; /* pico-os: placed by the platform */
extern int ym2612_index;
extern int ym2612_clock;

/* pico-os: the tables' sizes, and where they go (ym2612.c) */
#define YM2612_FAST_TABLES (13 * 2 * 256 * 2 + 1024 * 2)
#define YM2612_SLOW_TABLES (128 * 8 * 16)
extern void YM2612SetTables(void *fast, void *slow);
extern int ym2612_half_rate; /* pico-os: see ym2612.c */
extern int ym2612_timer_index; /* pico-os: set to 0 with ym2612_index; see ym2612.c */
extern void ym2612_timers_to(int index);
extern void YM2612Init(void);
extern void YM2612Config(unsigned char dac_bits); //,unsigned int AUDIO_FREQ_DIVISOR);
extern void YM2612ResetChip(void);
//extern void YM2612Update(int16_t *buffer, int length);
extern void YM2612Write(unsigned int a, unsigned int v, int target);
extern void ym2612_run(int target);
extern unsigned int YM2612Read(int target);

#if 0
extern int YM2612LoadContext(unsigned char *state);
extern int YM2612SaveContext(unsigned char *state);
#endif

//extern void YM2612LoadRegs(uint8_t *regs);
//extern void YM2612SaveRegs(uint8_t *regs);

void gwenesis_ym2612_save_state();
void gwenesis_ym2612_load_state();

#endif /* _YM2612_ */
