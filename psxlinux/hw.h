/*
 * hw.h - PlayStation hardware register definitions for the PSXLinux
 * port of SMS Plus (subset used by the port: I_STAT/I_MASK, GPU and
 * controller pads).  Values follow the psxdev SDK convention.
 */
#ifndef _PSXLINUX_HW_H
#define _PSXLINUX_HW_H

/* interrupt / status */
#define I_STAT      (*(volatile unsigned int *)0x1F801070)
#define I_MASK      (*(volatile unsigned int *)0x1F801074)

#define IRQ_VBL     0x001u          /* not used (VSync polls GPUSTAT) */
#define IRQ_CTRL    0x080u
#define IRQ_SPU     0x200u

/* GPU */
#define GP0         (*(volatile unsigned int *)0x1F801810)
#define GP1         (*(volatile unsigned int *)0x1F801814)
#define GPUSTAT     (*(volatile unsigned int *)0x1F801814)

#define GPUSTAT_READY_CMD   0x10000000u
#define GPUSTAT_FIFO_FULL   0x20000000u
#define GPUSTAT_FIELD       0x80000000u

/* controller (SIO0) - protocol in main.c follows miniglut.c */
#define PAD_DATA    (*(volatile unsigned char *)0x1F801040) /* byte access! */
#define PAD_STAT    (*(volatile unsigned short *)0x1F801044)
#define PAD_MODE    (*(volatile unsigned short *)0x1F801048)
#define PAD_CTRL    (*(volatile unsigned short *)0x1F80104A)
#define PAD_BAUD    (*(volatile unsigned short *)0x1F80104E)

#define PAD1_CTRL   0x1003u
#define PAD2_CTRL   0x2003u

#endif /* _PSXLINUX_HW_H */
