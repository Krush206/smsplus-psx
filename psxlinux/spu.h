/*
 * spu.h - SPU-ADPCM streaming audio (PSXLinux port of SMS Plus).
 */
#ifndef _PSXLINUX_SPU_H
#define _PSXLINUX_SPU_H

void spu_init(void);
void spu_shutdown(void);

/* Hand one emulated frame of samples per channel to the SPU streamer.
 * Properly sequences at 22050 Hz, 367 samples per 59.94 Hz field. */
void spu_frame(const short *left, const short *right, int count);

#endif /* _PSXLINUX_SPU_H */
