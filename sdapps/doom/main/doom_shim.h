#pragma once
// Force-included into every DOOM source (#525): the engine is written for a hosted libc and the OS exports only a curated
// part of it (tools/export_allow.txt). What it does not export is mapped here, so the engine's source stays untouched and
// the ELF links against the export table alone:
//   - printf puts putchar and the stdout/stderr half of fprintf: nowhere to print to (the app logs nothing over the console)
//   - fprintf fputs fputc fflush: real files only (the config and the saves), through the file shims' fwrite
//   - malloc calloc realloc free strdup: renamed to counted blocks by -D in CMakeLists.txt (the app's own sources may not
//     name them, check_sdapp_boundary), so stop can free everything the engine still holds (the loader does not sweep an
//     app's blocks yet, #536)
//   - errno: a plain variable (newlib's per-task errno is the OS's)
//   - system: nothing to run; isalpha and friends: the exported functions, not the C library's table
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

int   lichen_doom_fprintf(FILE *f, char const *fmt, ...);
int   lichen_doom_vfprintf(FILE *f, char const *fmt, va_list ap);
int   lichen_doom_fputs(char const *s, FILE *f);
int   lichen_doom_fputc(int c, FILE *f);
int   lichen_doom_fflush(FILE *f);
extern int lichen_doom_errno;

#ifdef __cplusplus
}
#endif

#undef stdout
#undef stderr
#undef stdin
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)
#define stdin  ((FILE *)0)

#undef printf
#undef puts
#undef putchar
#undef fprintf
#undef vfprintf
#undef fputs
#undef fputc
#undef fflush
#define printf(...) ((void)0)
#define puts(s)     ((void)0)
#define putchar(c)  ((void)0)
#define fprintf     lichen_doom_fprintf
#define vfprintf    lichen_doom_vfprintf
#define fputs       lichen_doom_fputs
#define fputc       lichen_doom_fputc
#define fflush      lichen_doom_fflush

#undef errno
#define errno lichen_doom_errno

#define system(cmd) ((void)(cmd), 0)

// the exported functions, not the C library's inline or table versions
#undef isalpha
#undef isdigit
#undef isspace
#undef isalnum
#undef isxdigit
#undef toupper
#undef tolower
int isalpha(int c);
int isdigit(int c);
int isspace(int c);
int isalnum(int c);
int isxdigit(int c);
int toupper(int c);
int tolower(int c);
