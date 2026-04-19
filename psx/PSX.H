
#ifndef _PSX_H_
#define _PSX_H_

#include "syscall.h"

#define ROM_SIZE                (524288)
#define gp0                     (*(volatile long *)0x1F801810)
#define gp1                     (*(volatile long *)0x1F801814)
#define PKT_HDR(cmd, r, g, b)   (cmd << 24 | b << 16 | g << 8 | r)
#define PKT_XY(x, y)            (y << 16 | x)
#define PKT_WH(w, h)            (h << 16 | w)

/* Function prototypes */
void init_machine(void);
void trash_machine(void);
void update_video(void);
void update_input(void);
void load_rom(void);
void putpixel(int x, int y, int r, int g, int b);
void fb_rect(int x, int y, int w, int h, int r, int g, int b);

#endif /* _PSX_H_ */
