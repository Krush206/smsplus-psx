/*
 * port.h - PSXLinux port support header for SMS Plus.
 *
 * Included by shared.h when PSXLINUX is defined.  Provides the basic
 * types and the tiny-libc prototypes implemented in portlib.c, since
 * the mipsel-linux cross toolchain has no target libc and the port is
 * linked with -nostdlib.
 *
 * The kernel side interface (raw syscalls) comes from the PSXLinux
 * psxdev SDK header, which main.c includes separately.
 */
#ifndef _PSXLINUX_PORT_H
#define _PSXLINUX_PORT_H

typedef unsigned int   size_t;

/* Basic types (subset of the old psx/syscall.h the core expects) */
typedef unsigned char  u_char;
typedef unsigned short u_short;
typedef unsigned long  u_long;

#ifndef NULL
#define NULL  ((void *)0)
#endif

#ifndef TRUE
#define TRUE  1
#define FALSE 0
#endif

/* ---- memory (bump allocator over a static pool) ---- */
void  *malloc(size_t size);
void   free(void *p);
void  *calloc(size_t n, size_t size);
void  *memset(void *s, int c, size_t n);
void  *memcpy(void *d, const void *s, size_t n);
void  *memmove(void *d, const void *s, size_t n);
int    memcmp(const void *a, const void *b, size_t n);

/* ---- strings ---- */
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
char  *strcpy(char *d, const char *s);
char  *strncpy(char *d, const char *s, size_t n);
char  *strcat(char *d, const char *s);
char  *strchr(const char *s, int c);
char  *strstr(const char *hay, const char *needle);
int    atoi(const char *s);

/* ---- console / misc ----
 * printf writes to the kernel console (/dev/console), which the
 * PSXLinux kernel renders on screen via its GPU text console.
 */
int    printf(const char *fmt, ...);
int    sprintf(char *buf, const char *fmt, ...);
void   exit(int code);

/* ---- math (soft-float, used by the YM3812 table init in fmopl.c) ---- */
double sin(double x);
double cos(double x);
double pow(double x, double y);
double log10(double x);
double fabs(double x);
double floor(double x);
int    abs(int x);

/*
 * fread/fwrite are #define'd away by shared.h: save states need a
 * writable filesystem and the CD root is read-only, so
 * system_save_state()/system_load_state() become no-ops.
 */

#endif /* _PSXLINUX_PORT_H */
