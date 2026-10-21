
#ifndef _SHARED_H_
#define _SHARED_H_

#define VERSION     "0.9.3"

/* Data types */
typedef unsigned long int dword;
typedef unsigned short int word;
typedef unsigned char byte;


/* To keep the MAME code happy */
#define HAS_YM3812  1
typedef signed short int FMSAMPLE;

/* Nasty hacks so the PSX version will compile */
#if PSXLINUX
#include "psxlinux/port.h"  /* PSXLinux port: types + tiny libc (see psxlinux/) */
#define fread               /* Save states unsupported: the ISO9660 root fs is read-only */
#define fwrite
#elif PSX
#include "psx/syscall.h"    /* for NULL, malloc(), etc. */
#define fread               /* No real filesystem :) */
#define fwrite
#define sin                 /* No equivelant */
#define pow
#define log10
#else
#include <malloc.h>
#include <math.h>
#endif

#if DOS
#include <stdio.h>
#endif

#include "z80.h"
#include "sms.h"
#include "vdp.h"
#include "render.h"
#include "sn76496.h"
#include "fmopl.h"
#include "ym2413.h"
#include "system.h"

#endif /* _SHARED_H_ */

