/// @file ff.h
/// @brief PC stand-in for FatFs, on stdio and dirent (md viewer tests): "/" is the folder the test runs in.
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
typedef unsigned long long FSIZE_t;       ///< a file size
#define FR_OK 0                           ///< fine
#define FA_READ 1                         ///< read
#define FA_WRITE 2                        ///< write
#define FA_CREATE_ALWAYS 8                ///< truncate
#define AM_DIR 0x10                       ///< a folder
FRESULT f_open(FIL *f, const char *path, int mode);         ///< opens
FRESULT f_read(FIL *f, void *buf, UINT n, UINT *got);      ///< reads
FRESULT f_write(FIL *f, const void *buf, UINT n, UINT *put);  ///< writes
FRESULT f_lseek(FIL *f, FSIZE_t at);                        ///< moves to a byte
FRESULT f_close(FIL *f);                                    ///< closes
FSIZE_t f_size(FIL *f);                                     ///< its size
FSIZE_t f_tell(FIL *f);                                     ///< where it is
FRESULT f_opendir(DIR *d, const char *path);                ///< opens a folder
FRESULT f_readdir(DIR *d, FILINFO *fi);                     ///< the next entry ("" at the end)
FRESULT f_closedir(DIR *d);                                 ///< closes it
FRESULT f_stat(const char *path, FILINFO *fi);              ///< about a file
FRESULT f_getcwd(char *buf, UINT n);                        ///< the folder now
FRESULT f_unlink(const char *path);                         ///< deletes a file
FRESULT f_rename(const char *from, const char *to);         ///< renames one
