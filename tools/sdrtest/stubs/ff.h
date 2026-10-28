/// @file ff.h
/// @brief PC stand-in for FatFs on stdio (SDR tests): "/" is the folder the test runs in.
#pragma once
#include <stdio.h>
typedef struct { FILE *f; } FIL;          ///< a file
typedef int FRESULT;                      ///< a result
typedef unsigned UINT;                    ///< a count
#define FR_OK 0                           ///< fine
#define FA_READ 1                         ///< read
#define FA_WRITE 2                        ///< write
#define FA_CREATE_ALWAYS 8                ///< truncate
FRESULT f_open(FIL *f, const char *path, int mode);  ///< opens
FRESULT f_close(FIL *f);                             ///< closes
char *f_gets(char *buf, int n, FIL *f);              ///< a line
int f_printf(FIL *f, const char *fmt, ...);          ///< formatted output
