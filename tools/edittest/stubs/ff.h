/// @file ff.h
/// @brief PC stand-in for FatFs, on stdio (text editor tests): "/" is the folder the test runs in.
#pragma once
#include <stdio.h>
typedef struct { FILE *f; } FIL;          ///< a file
typedef int FRESULT;                      ///< a result
typedef unsigned UINT;                    ///< a count
#define FR_OK 0                           ///< fine
#define FR_NO_FILE 4                      ///< not there
#define FR_DENIED 7                       ///< full or read-only
#define FA_READ 1                         ///< read
#define FA_WRITE 2                        ///< write
#define FA_CREATE_ALWAYS 8                ///< truncate
FRESULT f_open(FIL *f, const char *path, int mode);            ///< opens
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got);          ///< reads
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put);   ///< writes
FRESULT f_close(FIL *f);                                       ///< closes
unsigned long f_size(FIL *f);                                  ///< its size
FRESULT f_unlink(const char *path);                            ///< deletes a file
FRESULT f_rename(const char *from, const char *to);            ///< renames one
