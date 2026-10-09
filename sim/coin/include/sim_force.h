#pragma once
// Force-included before every firmware source in the simulator build (-include sim_force.h).
//
// The coin's LittleFS is mounted at "/c". On Windows "/c/00001.ogg" would mean C:\c\00001.ogg, so
// the file calls the firmware makes are routed through functions that map the mount point onto the
// simulator's data directory. Function-like macros leave `struct stat` alone.

#include <assert.h>  // newlib's headers bring it in on the chip
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

typedef long suseconds_t;  // POSIX, absent from mingw
size_t strlcpy(char *dst, const char *src, size_t size);  // BSD, in newlib; esp_core.c

FILE *sim_fopen(const char *path, const char *mode);
DIR *sim_opendir(const char *path);
int sim_stat(const char *path, struct stat *st);
int sim_remove(const char *path);
int sim_rename(const char *from, const char *to);
int sim_truncate(const char *path, off_t len);
int sim_fsync(int fd);
int sim_gettimeofday(struct timeval *tv, void *tz);
int sim_settimeofday(const struct timeval *tv, const void *tz);

#define fopen(p, m) sim_fopen(p, m)
#define opendir(p) sim_opendir(p)
#define stat(p, s) sim_stat(p, s)
#define remove(p) sim_remove(p)
#define rename(a, b) sim_rename(a, b)
#define truncate(p, l) sim_truncate(p, l)
#define fsync(fd) sim_fsync(fd)
#define gettimeofday(tv, tz) sim_gettimeofday(tv, tz)
#define settimeofday(tv, tz) sim_settimeofday(tv, tz)
