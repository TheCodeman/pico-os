/**
 * @file as_port.h
 * @brief What the Applesoft interpreter needs from the machine.
 *
 * as_port_pico.c implements it on the board; host/as_port_host.c on a PC for
 * testing.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---- Memory ------------------------------------------------------------------

/// The emulated Apple's RAM (64 KB, zeroed), and other working memory. NULL if
/// there is no room.
/// @param n bytes wanted
/// @return the memory, or NULL
void *asp_alloc(size_t n);
/// Frees memory from asp_alloc.
/// @param p the memory
void asp_free(void *p);

// ---- Screen ------------------------------------------------------------------

/// Width of the screen Applesoft draws on, in pixels.
#define ASP_FB_WIDTH  640
/// Height of the screen Applesoft draws on, in pixels.
#define ASP_FB_HEIGHT 480

/// The 640x480 RGB332 framebuffer the Apple display is drawn into.
/// @return the framebuffer
uint8_t *asp_framebuffer(void);
/// Taking the screen over from the OS console, and giving it back (cleared).
void asp_screen_begin(void);
/// Ends drawing a frame: the screen is shown.
void asp_screen_end(void);

/// Every character the interpreter prints, as plain ASCII with '\r' for a new
/// line, for a serial terminal (on the board) or stdout (on a PC).
/// @param c the character
void asp_mirror(int c);

// ---- Keyboard, paddles -------------------------------------------------------

/// Next key as ASCII (arrow keys arrive as ANSI escape sequences), or -1 if
/// none arrives within timeout_us (0: don't wait).
/// @param timeout_us how long to wait, in microseconds (0: don't)
/// @return the key, or -1
int asp_key(uint32_t timeout_us);
/// True when the interpreter has nothing more to read and should stop (end of
/// a test script). Always false on the board.
/// @return true when the input has run out
bool asp_input_done(void);
/// Game controller: paddle 0-3 as 0-255, push buttons 0-2.
/// @param n 0-3
/// @return 0-255
int asp_paddle(int n);
/// True while paddle button `n` is pressed.
/// @param n the button (0-2)
/// @return true while pressed
bool asp_button(int n);

// ---- Time and sound ----------------------------------------------------------

/// Microseconds since start-up.
/// @return microseconds
uint64_t asp_micros(void);
/// Waits `us` microseconds.
/// @param us microseconds
void asp_sleep_us(uint32_t us);
/// A tone (the bell is 1 kHz for 0.1 s).
/// @param freq_hz the frequency
/// @param ms how long
void asp_tone(unsigned freq_hz, unsigned ms);
/// The speaker cone flips (a touch of $C030) at this asp_micros() time.
/// @param when_us when, in asp_micros() terms
void asp_speaker_click(uint64_t when_us);

// ---- Files on the drive ------------------------------------------------------

/// An open file on the drive (opaque).
typedef struct asp_file asp_file;
/// Open modes for asp_open.
enum { ASP_READ, ASP_WRITE, ASP_APPEND, ASP_UPDATE };  ///< UPDATE: read/write, created if missing

/// Opens file `name` with ASP_READ, ASP_WRITE, ASP_APPEND or ASP_UPDATE; NULL if it can't.
/// @param name the file's name
/// @param mode ASP_READ, ASP_WRITE, ASP_APPEND or ASP_UPDATE
/// @return the file, or NULL
asp_file *asp_open(const char *name, int mode);
/// Reads from the file.
/// @param f the file
/// @param buf where the bytes go
/// @param n how many
/// @return the bytes read, 0 at the end, -1 on error
int asp_read(asp_file *f, void *buf, int n);          ///< bytes read, 0 at the end, -1 error
/// Writes to the file.
/// @param f the file
/// @param buf the bytes
/// @param n how many
/// @return the bytes written, or -1
int asp_write(asp_file *f, const void *buf, int n);   ///< bytes written, -1 error
/// Moves to byte `pos` of the file.
/// @param f the file
/// @param pos the position
/// @return false on failure
bool asp_seek(asp_file *f, uint32_t pos);
/// The file's current position.
/// @param f the file
/// @return the position
uint32_t asp_tell(asp_file *f);
/// The file's size in bytes.
/// @param f the file
/// @return its size in bytes
uint32_t asp_size(asp_file *f);
/// Closes the file; false if writing it failed.
/// @param f the file
/// @return false if writing it failed
bool asp_close(asp_file *f);
/// True if file `name` exists.
/// @param name the file's name
/// @return true if it exists
bool asp_exists(const char *name);
/// Deletes file `name`.
/// @param name the file's name
/// @return false on failure
bool asp_remove(const char *name);
/// Renames file `from` to `to`.
/// @param from the old name
/// @param to the new name
/// @return false on failure
bool asp_rename(const char *from, const char *to);
/// Calls fn for each file in the current folder; false if it can't be read.
/// @param fn called with each file's name, size and whether it is a folder
/// @param ctx passed to fn
/// @return false if the folder can't be read
bool asp_list(void (*fn)(void *ctx, const char *name, uint32_t size, bool is_dir), void *ctx);
/// Why the last file call failed, in a few words.
/// @return a few words saying why
const char *asp_file_error(void);

// ---- Other -------------------------------------------------------------------

/// True when the C stack is nearly used up (the expression evaluator recurses).
/// @return true when the stack is nearly used up
bool asp_stack_low(void);
