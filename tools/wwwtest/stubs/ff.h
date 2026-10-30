/// @file ff.h
/// @brief PC stand-in for FatFs on stdio (web browser tests): "/" is the folder the test runs in.
#pragma once
#include <stdio.h>
typedef struct { FILE *f; } FIL;          ///< a file
typedef struct { void *d; } DIR;          ///< a folder being read
/// A folder entry.
typedef struct {
    char fname[256];         ///< its name
    unsigned char fattrib;   ///< AM_DIR for a folder
    unsigned long long fsize;  ///< its size
} FILINFO;
typedef int FRESULT;                      ///< a result
typedef unsigned UINT;                    ///< a count
typedef unsigned char BYTE;               ///< a byte
typedef unsigned short WORD;              ///< two
typedef unsigned long long FSIZE_t;       ///< a file size
#define FR_OK 0                           ///< fine
#define FR_NO_FILE 4                      ///< no such file
#define FR_EXIST 8                        ///< it's there already
#define FA_READ 1                         ///< read
#define FA_WRITE 2                        ///< write
#define FA_CREATE_NEW 4                   ///< a new file only
#define FA_CREATE_ALWAYS 8                ///< truncate
#define FA_OPEN_APPEND 0x30               ///< add to the end
#define AM_DIR 0x10                       ///< a folder
FRESULT f_open(FIL *f, const char *path, int mode);         ///< opens
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got);      ///< reads
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put);  ///< writes
FRESULT f_close(FIL *f);                                    ///< closes
FSIZE_t f_size(FIL *f);                                     ///< its size
FRESULT f_stat(const char *path, FILINFO *fi);              ///< about a file
FRESULT f_getcwd(char *buf, UINT n);                        ///< the folder now
FRESULT f_unlink(const char *path);                         ///< deletes a file
FRESULT f_mkdir(const char *path);                          ///< makes a folder
int f_printf(FIL *f, const char *fmt, ...);                 ///< prints
int f_putc(char c, FIL *f);                                 ///< puts a character
