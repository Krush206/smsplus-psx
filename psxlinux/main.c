/*
 * main.c - PSXLinux port of SMS Plus (frontend).
 *
 * A fully self-contained frontend: raw MIPS o32 syscalls (psxsys.h)
 * for opening the ROM image and console, direct GPU register writes
 * for video, a direct SIO0 pad poller (miniglut protocol) for input,
 * and the SPU-ADPCM streamer (spu.c) for sound.  The core emulator is
 * unchanged except for two documented tweaks (see shared.h and
 * cpu/z80.c).
 *
 * Video: 320x240 15bpp NTSC, double buffered at (0,0) and (0,240).
 * The SMS 8-bit bitmap (256x192, GG 160x144) is uploaded to VRAM as an
 * 8bpp texture at halfword (512,0), its 32-entry palette as a 128-entry
 * CLUT at halfword (512,208) (each entry repeated 4 times, since
 * rendered pixel values carry priority/sprite-marker bits), then
 * presented with two GPU sprites scaled x1.25 to fill the screen.
 */
#include "shared.h"
#include "hw.h"
#include "spu.h"
#include "psxsys.h"

/* spu.c: produced-slot counter (for the audio breadcrumb) */
extern int spu_dbg_slots;

/* not in psxsys.h (the reference SDK header stays untouched) */
static int sys_lseek(int fd, int offset, int whence)
{
    return sys_call3(SYS_lseek, fd, offset, whence);
}

/* The PSXLinux kernel allocates task structures, kernel stacks and page
 * caches from free RAM that overlaps the top of the 1MB user window, so
 * the ECOFF image (text + data + bss + the 16K user stack the loader
 * places above it) must stay below ~0x18A000.  Boot-load probes on this
 * kernel validated image spans up to ~0x186000 (bss <= ~550KB); the
 * renderer's fixed tables (render.c pattern cache 128K + led lut 64K)
 * plus emulator contexts leave room, so the ROM buffer is 128K after
 * that: larger SMS images (256K/512K) are not supported here. */
#define ROM_SIZE    (128 * 1024)
#define CONSOLE_W   320
#define CONSOLE_H   240

/* VRAM layout */
#define TEX_X       512             /* 8bpp texture, halfword coords */
#define TEX_Y       0
#define CLUT_X      512
#define CLUT_Y      208

/* display area Y offsets of the two framebuffers */
#define BUF_Y0      0
#define BUF_Y1      240

static byte  base_rom[ROM_SIZE];
static byte  bitmap_data[256 * 192];
static int   exit_flag;

static int back_y;                  /* buffer being drawn into            */
static int front_y;                 /* buffer currently shown on screen   */
static int game_activity;           /* bit0: game wrote CRAM (palette)    */
static unsigned page_sums[8];       /* ROM per-16K-page checksums (loader proof) */

static void rom_checksums(void)
{
    const unsigned char *rom = cart.rom;
    int p;
    size_t i;

    for (p = 0; p < 8; p++) {
        unsigned s = 0;
        for (i = (size_t)p * 0x4000; i < (size_t)(p + 1) * 0x4000; i++)
            s += rom[i];
        page_sums[p] = s;
    }
}

static int pad_inited;

/* ------------------------------------------------------------------ */
/* GPU low level                                                      */
/* ------------------------------------------------------------------ */

/* PCSX's gpulib status only exposes bit28 ("ready to receive"), and
 * draws can queue while blitting on real hardware too, so all waits
 * here are bounded: on timeout we proceed anyway rather than wedging
 * the emulator loop. */
static void gpu_ready_wait(void)
{
    int i;

    for (i = 0; i < 200000; i++)
        if (GPUSTAT & 0x10000000u)      /* bit28: ready to receive */
            return;
}

static void gp0_cmd(unsigned v)
{
    gpu_ready_wait();
    GP0 = v;
}

/* GPU DMA channel 2 (block mode, RAM -> GP0).  Preparing a packet in
 * RAM and letting DMA feed it to the GPU costs one call into the GPU
 * plugin instead of one per word: the 12K word texture upload used to
 * dominate the frame time. */
#define DMA2_MADR   (*(volatile unsigned int *)0x1F8010A0)
#define DMA2_BCR    (*(volatile unsigned int *)0x1F8010A4)
#define DMA2_CHCR   (*(volatile unsigned int *)0x1F8010A8)
#define DPCR        (*(volatile unsigned int *)0x1F8010F0)

#define DMA_CHCR_START  0x01000000u
#define DMA2_MEM2GPU    0x01000201u    /* block, forward, 32-bit       */
#define DPCR_CH2        0x0F00u        /* channel 2 priority 7 + on    */

/* One channel-2 block DMA to the GPU data port.  The transfer itself is
 * done by the time this returns (the plugin consumes it synchronously),
 * so there is nothing to poll. */
static void dma_to_gpu(const void *addr, unsigned words)
{
    DMA2_MADR = (unsigned)addr;
    DMA2_BCR  = (1u << 16) | words;         /* one block */
    DMA2_CHCR = DMA2_MEM2GPU;
    I_STAT = ~IRQ_DMA;                      /* ack the completion IRQ */
}

static unsigned ds_spins;               /* diagnostics: poll iterations */

static void drawsync_wait(void)
{
    int i;

    for (i = 0; i < 200000; i++) {
        if (GPUSTAT & 0x04000000u)          /* bit26: GPU ready, not busy */
            return;
        ds_spins++;
    }
}

/* ------------------------------------------------------------------ */
/* Frame pacing through Timer 1 (0x1F801110)                          */
/* ------------------------------------------------------------------ */

/* The kernel's tick runs on Timer 2 (IRQ0 = TIMER2, mask 0x40) and
 * nothing claims Timer 1, so it is ours.  Mode bit8 selects the
 * horizontal-blank clock (262 hblanks per NTSC 240-line field), which
 * is identical on the real console and in pcsx (whose counter model
 * is (cpu_cycles - start)/rate regardless of the display mode, so it
 * works even though GPUSTAT bit31 is frozen for non-interlaced
 * displays).  With 262 hblanks per wrap the counter needs ~4 s to
 * overflow, so single-wrap detection is always valid.        */

#define T1_COUNT     (*(volatile unsigned short *)0x1F801110)

/* DIAGNOSTIC: wall clock.  PSXLinux has no MMU, so the program can read
 * the kernel's 100 Hz jiffies counter directly (System.map: 0xb72b4).
 * Used only to report real frames/second; not part of the emulator. */
#define KJIFFIES     (*(volatile unsigned *)0x800B72B4)
#define T1_MODE      (*(volatile unsigned short *)0x1F801114)
#define T1_HBLANK    0x0100                    /* bit8: hblank source */
#define HBLANKS_FRAME    262

static unsigned pace_prev;                  /* last T1 sample */
static int      pace_acc;                   /* accumulated hblank time */
static int      pace_stalled;               /* pace_frame give-ups so far */

static void pace_init(void)
{
    T1_MODE = (unsigned short)T1_HBLANK;
    T1_COUNT = 0;                           /* sync phase */
    pace_prev = 0;
    pace_acc  = 0;
    pace_stalled = 0;
}

/* Blocks until 262 hblank ticks (= one NTSC field) have passed since
 * the previous return.  Skipped budget after a long stall is dropped
 * instead of catching up in a burst. */
static void pace_frame(void)
{
    int pace_guard = 0;

    for (;;) {
        unsigned c = T1_COUNT;
        int d = (int)(c - pace_prev);

        if (d < 0)
            d += 0x10000;                   /* counter wrapped */
        pace_prev = c;
        pace_acc += d;
        if (pace_acc >= HBLANKS_FRAME) {
            if (pace_acc > 4 * HBLANKS_FRAME)
                pace_acc = 0;
            else
                pace_acc -= HBLANKS_FRAME;
            return;
        }
        /* Timer 1 must tick ~262 times per 16.7 ms; if it does not
         * (broken hblank clock, mode write lost, ...) give up after
         * ~2 s of spinning rather than freezing the whole frontend */
        if (++pace_guard > 4000000) {
            pace_acc = 0;
            pace_stalled++;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* GPU breadcrumbs: small absolute-coordinate color marks along the   */
/* top edge of VRAM (y 0..8), visible in the frontend frame dumps so   */
/* hangs can be localized without a working console.                   */
/* ------------------------------------------------------------------ */

/* A0-based breadcrumb: an 8-pixel colored strip uploaded through a
 * tiny CPU->VRAM transfer.  The old fill-based marks turned out to be
 * invisible in the frontend dumps (peops only schedules a presentation
 * when a VRAM transfer completes or the display mode changes), so each
 * mark here completes an A0 transfer and is guaranteed to be dumped. */
static void vram_upload_img(int x, int y, int w, int h, const void *pix, int pitch);

static void bc_row(int slot, int row, int r, int g, int b)
{
    unsigned short hw[4];
    unsigned x = 8u * (unsigned)slot;
    unsigned y = 8u * (unsigned)row;
    unsigned c = (unsigned short)(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));

    hw[0] = (unsigned short)c;
    hw[1] = (unsigned short)c;
    hw[2] = (unsigned short)c;
    hw[3] = (unsigned short)c;
    vram_upload_img((int)x, (int)y, 4, 1, hw, 8);   /* 4 halfwords x 1 row */
}

static void bc(int slot, int r, int g, int b)
{
    bc_row(slot, 0, r, g, b);
}

static void set_draw_area(void);
static void set_draw_offset(void);

static void gpu_init(void)
{
    back_y  = BUF_Y1;
    front_y = BUF_Y1;

    /* Display setup mirrors the proven frontends (old real-hardware
     * smsplus psx port and the kernel's psxgame demo). */
    GP1 = 0x00000000;                   /* soft reset */
    GP1 = 0x03000000;                   /* display enable */
    GP1 = 0x06C60260;                   /* 320 pixel horizontal range */
    GP1 = 0x07040010;                   /* 240 line vertical range */
    GP1 = 0x08000001;                   /* 320x240, 15bpp, NTSC */
    GP1 = 0x05000000;                   /* display area (0,0) */

    /* clip both buffers and use an absolute (0,0) offset for the
     * initial clear */
    GP0 = 0xE3000000;                       /* top-left (0,0) */
    GP0 = 0xE4000000 | (((unsigned)(BUF_Y1 + CONSOLE_H)) << 10) | CONSOLE_W;
    GP0 = 0xE5000000;

    /* blank every visible line so the first frames are black: fill
     * (0,0)-(320,480) (02h: word0 = color, w1 = abs XY, w2 = (H<<16)|W;
     * fill ignores the draw offset and clip area) */
    GP0 = 0x02000000;
    GP0 = 0x00000000;                                   /* xy (0,0) */
    GP0 = ((unsigned)CONSOLE_H << 16) | (unsigned)CONSOLE_W;
}

/* set clip area for the back buffer (0..320 x back_y..back_y+240) */
static void set_draw_area(void)
{
    GP0 = 0xE3000000 | ((unsigned)back_y << 10);
    GP0 = 0xE4000000 | (((unsigned)(back_y + CONSOLE_H)) << 10) | CONSOLE_W;
}

static void set_draw_offset(void)
{
    /* E5 drawing offset packs 11-bit signed fields: X = bits 0-10,
     * Y = bits 11-21 (unlike E3/E4 clip and GP1(05) display position,
     * which place Y at bit 10).  Verified against pcsx's own parser
     * (plugins/dfxvideo/prim.c: DrawOffset.y = (gdata>>11)&0x7ff).
     * With <<10, buffer 1's sprites were offset to y=120 instead of
     * 240, so that buffer showed only a shifted sliver and the screen
     * appeared to blink between game and black. */
    GP0 = 0xE5000000 | ((unsigned)back_y << 11);
}

/* show the given buffer */
static void gpu_flip(int y)
{
    GP1 = 0x05000000 | (y << 10);
}

/* CPU -> VRAM transfer.  The packet (A0 header + pixel data) is built
 * in dma_buf and fed to the GPU with one channel-2 DMA call:
 *   A0 layout: word0 = 0xA0000000, word1 = (y<<16)|x, word2 = (h<<16)|w
 *   (w in halfwords), then w*h halfwords, two per 32-bit word.
 * Getting the packing wrong leaves the GPU mid-transfer, swallowing
 * every later GP0 word as pixel data (observed the hard way). */
static void vram_upload_img(int x, int y, int w, int h, const void *pix, int pitch)
{
    const unsigned char *src = (const unsigned char *)pix;
    int wpr = (w + 1) / 2;              /* 32-bit words per image row */

    DPCR |= DPCR_CH2;
    I_MASK &= ~IRQ_DMA;                 /* the kernel has no DMA handler */

    /* The GPU data port takes two 8bpp texels per 32-bit word, which is
     * exactly the little-endian layout of a byte-per-pixel bitmap row.
     * When a row is contiguous with the next (pitch == w halfwords) the
     * whole image goes out as one DMA straight from the caller's buffer,
     * with no staging copy at all. */
    if (pitch == w * 2) {
        gp0_cmd(0xA0000000u);
        gp0_cmd(((unsigned)y << 16) | (unsigned)x);
        gp0_cmd(((unsigned)h << 16) | (unsigned)w);
        dma_to_gpu(pix, (unsigned)(wpr * h));
        return;
    }

    /* Otherwise (Game Gear in a 256 byte pitch) send it a row at a time
     * through a small packet on the stack: a 128 halfword row is 64
     * words, so the buffer never needs to be larger than that. */
    {
        unsigned pkt[3 + 64];
        int row;

        for (row = 0; row < h; row++) {
            const unsigned char *s = src + (size_t)row * (size_t)pitch;

            pkt[0] = 0xA0000000u;
            pkt[1] = ((unsigned)(y + row) << 16) | (unsigned)x;
            pkt[2] = (1u << 16) | (unsigned)w;
            memcpy(&pkt[3], s, (size_t)wpr * 4u);
            dma_to_gpu(pkt, 3u + (unsigned)wpr);
        }
    }
}

/* CLUT attribute word for the palette at (CLUT_X, CLUT_Y) */
#define CLUT_ATTR   (((unsigned)CLUT_Y << 6) | ((unsigned)(CLUT_X >> 4) & 0x3F))
/* tpage for the 8bpp texture at halfword (TEX_X, TEX_Y):
 * bits 0-3 = page X (64 halfwords), bit 4 = page Y (256 lines),
 * bits 7-8 = color mode (1 = 8bpp) */
#define TEX_TPAGE   ((((unsigned)(TEX_X >> 6)) & 0x0F) | \
                     ((((unsigned)(TEX_Y >> 8)) & 0x01) << 4) | (1u << 7))

/* Textured rectangle (64h, variable size, raw).  Rectangles sample
 * texture 1:1 (U/V advance one texel per pixel), so this cannot scale;
 * it is used for the self-test sprite only. */
static void draw_sprite(int x, int y, int u, int v, int w, int h,
                        unsigned tpage, unsigned clut)
{
    gp0_cmd(0xE1000000u | tpage);           /* tpage + 8bpp color mode  */
    gp0_cmd(0x64808080u);                   /* modulate = identity      */
    gp0_cmd(((unsigned)y << 16) | x);
    gp0_cmd((clut << 16) | ((unsigned)(v & 0xFF) << 8) | (unsigned)(u & 0xFF));
    gp0_cmd(((unsigned)h << 16) | w);
}

/* Textured quad (2Ch).  Unlike a rectangle this carries a UV per
 * corner, so the GPU interpolates U/V.  Kept for reference: peops'
 * 8bpp polygon path does not draw our packets (the rectangle path
 * does), so the frontend presents the bitmap with a 1:1 sprite. */
static void draw_bitmap_quad(int tex_w, int tex_h, int dst_w, int dst_h,
                             unsigned tpage, unsigned clut)
{
    int x0 = (CONSOLE_W - dst_w) / 2;
    int y0 = (CONSOLE_H - dst_h) / 2;
    int x1 = x0 + dst_w;
    int y1 = y0 + dst_h;
    unsigned u1 = (unsigned)(tex_w - 1);    /* U is 8 bit: 0..255 */
    unsigned v1 = (unsigned)(tex_h - 1);    /* V is 8 bit: 0..255 */

    /* packet: cmd|color, xy0, uv0|clut, xy1, uv1|tpage, xy2, uv2,
     * xy3, uv3  (the tpage lives in the high half of the 2nd UV word,
     * the CLUT in the high half of the 1st, as on real hardware) */
    gp0_cmd(0x2C808080u);                       /* modulate identity */
    gp0_cmd(((unsigned)y0 << 16) | (unsigned)x0);
    gp0_cmd((clut << 16) | 0u);                 /* top-left  (0,0)  */
    gp0_cmd(((unsigned)y0 << 16) | (unsigned)x1);
    gp0_cmd((tpage << 16) | u1);                /* top-right        */
    gp0_cmd(((unsigned)y1 << 16) | (unsigned)x1);
    gp0_cmd((v1 << 8) | u1);                    /* bottom-right     */
    gp0_cmd(((unsigned)y1 << 16) | (unsigned)x0);
    gp0_cmd((v1 << 8) | 0u);                    /* bottom-left      */
}

/* ------------------------------------------------------------------ */
/* video                                                              */
/* ------------------------------------------------------------------ */

static void update_clut(void)
{
    static unsigned short clut_hw[128];
    int i;

    /* always re-upload: 128 halfwords per frame is free compared with
     * the 24K halfword texture, and it makes the palette independent
     * of the core's dirty flag (a never-cleared flag used to leave the
     * GPU CLUT all zero, which makes every texel fully transparent and
     * the whole screen black) */
    if (bitmap.pal.update) {
        game_activity |= 1;                 /* game wrote its palette */
        bitmap.pal.update = 0;
    }

    for (i = 0; i < 128; i++) {
        int c = i & 0x1F;
        unsigned r = bitmap.pal.color[c][0] >> 3;
        unsigned g = bitmap.pal.color[c][1] >> 3;
        unsigned b = bitmap.pal.color[c][2] >> 3;
        clut_hw[i] = (unsigned short)((b << 10) | (g << 5) | r);
    }
    vram_upload_img(CLUT_X, CLUT_Y, 128, 1, clut_hw, 256);
}

/* Self-test sprite: 16x16 8bpp texture of color index 1 with its own
 * two-entry CLUT, drawn at a fixed screen position.  If the game area
 * stays black but this green block appears, the sprite/texture/CLUT
 * path is fine and the bitmap data (not the presentation) is empty. */
#define ST_X        640                  /* self-test texture  */
#define ST_Y        0
#define ST_CLUT_X   640
#define ST_CLUT_Y   208
#define ST_TPAGE    ((((unsigned)(ST_X >> 6)) & 0x0F) | (1u << 7))
#define ST_CLUT_ATTR (((unsigned)ST_CLUT_Y << 6) | ((unsigned)(ST_CLUT_X >> 4) & 0x3F))

static void draw_selftest(void)
{
    static unsigned short tex[8 * 16];   /* 16 texels = 8 halfwords/row */
    static unsigned short clut[16];
    static int ready;
    int i;

    if (!ready) {
        for (i = 0; i < 8 * 16; i++)
            tex[i] = 0x0101;             /* texels 1,1 -> CLUT entry 1 */
        clut[0] = 0x0000;                /* transparent */
        clut[1] = 0x03E0;                /* pure green  */
        for (i = 2; i < 16; i++)
            clut[i] = 0x7FFF;
        vram_upload_img(ST_X, ST_Y, 8, 16, tex, 16);
        vram_upload_img(ST_CLUT_X, ST_CLUT_Y, 16, 1, clut, 32);
        ready = 1;
    }
    draw_sprite(200, 150, 0, 0, 16, 16, ST_TPAGE, ST_CLUT_ATTR);
}

static void update_video(void)
{
    int w_tex, h_tex, w_dst, h_dst;

    /* target the current back buffer (clip + draw offset must be reset
     * every frame since the buffer alternates) */
    set_draw_area();
    set_draw_offset();

    /* GPU idle before touching texture; then refresh CLUT + texture */
    drawsync_wait();

    update_clut();

    w_tex = cart.type ? 160 : 256;
    h_tex = cart.type ? 144 : 192;
    vram_upload_img(TEX_X, TEX_Y, w_tex / 2, h_tex, bitmap.data, bitmap.pitch);

    /* clear the logical back buffer: fill packets use absolute
     * coordinates and ignore the draw offset */
    gp0_cmd(0x02000000u);
    gp0_cmd((unsigned)back_y << 16);
    gp0_cmd(((unsigned)CONSOLE_H << 16) | (unsigned)CONSOLE_W);

    /* present: stretch the bitmap to the full display with a textured
     * quad (a rectangle cannot scale: it samples U/V 1:1 per pixel) */
    w_dst = (w_tex * 5) / 4;
    h_dst = (h_tex * 5) / 4;
    draw_bitmap_quad(w_tex, h_tex, w_dst, h_dst, TEX_TPAGE, CLUT_ATTR);

    draw_selftest();

    /* identity marker: steady visibility in both buffers (offsets are
     * ignored by fill rects, so absolute coords cover each buffer's
     * bottom-right corner).  Marker stays until a flush; it blinks
     * exactly when the wrong buffer is drawn. */
    GP0 = 0x02A000F0u;                    /* B=0xA0 G=0x00 R=0xF0 */
    GP0 = ((unsigned)(CONSOLE_H - 16) << 16) | (unsigned)(CONSOLE_W - 4);
    GP0 = (16u << 16) | 4u;               /* buffer 0 (H<<16)|W */
    GP0 = ((unsigned)(CONSOLE_H * 2 - 16) << 16) | (unsigned)(CONSOLE_W - 4);
    GP0 = (16u << 16) | 4u;               /* buffer 1 */
}

/* ------------------------------------------------------------------ */
/* input                                                              */
/* ------------------------------------------------------------------ */

#define TX_RDY   0x0001
#define RX_RDY   0x0002

/* The data port must be accessed as a BYTE: pcsx (and the real SIO)
 * dispatch each byte separately, and a 16-bit access would push or
 * pop two protocol bytes at once. */
static unsigned char pad_xmit(unsigned char val)
{
    int i;

    for (i = 0; i < 1000 && !(PAD_STAT & TX_RDY); i++)
        ;
    PAD_DATA = val;
    /* pcsx delivers the reply ~535 cycles later through the SIO
     * interrupt; a generous bounded wait covers it */
    for (i = 0; i < 10000 && !(PAD_STAT & RX_RDY); i++)
        ;
    return PAD_DATA;
}

static void pad_init_once(void)
{
    if (pad_inited)
        return;
    pad_inited = 1;

    /* The kernel has no handler for the controller (pad-ACK)
     * interrupt: mask it (bit clear in I_MASK disables it under the
     * kernel's convention) and clear any pending instance before the
     * first poll, or the kernel would panic. */
    I_MASK &= ~IRQ_CTRL;
    I_STAT  = ~IRQ_CTRL;

    PAD_MODE = 0x000D;              /* 8N1, factor 1 */
    PAD_BAUD = 0x0088;              /* 250 kHz */
    PAD_CTRL = 0x0000;              /* reset */
}

/* Poll one pad slot.  Returns the raw button word (1 = released,
 * standard Sony bit layout) or 0xFFFF when no digital pad answered
 * the handshake, so garbage can never fake button presses. */
static unsigned poll_pad_raw(unsigned ctrl_val)
{
    unsigned char id;
    unsigned b0, b1;

    PAD_CTRL = (unsigned short)ctrl_val;    /* select + TX enable */
    (void)pad_xmit(0x01);                   /* access       -> 0xFF */
    id = pad_xmit(0x42);                    /* read buttons -> 0x5A */
    b0 = pad_xmit(0x00);                    /* buttons low  */
    b1 = pad_xmit(0x00);                    /* buttons high */
    PAD_CTRL = 0x1010;                      /* deselect + clear error/ACK */
    I_STAT = ~IRQ_CTRL;                     /* ack the pending pad IRQ */

    if (id != 0x5A)
        return 0xFFFF;                      /* no valid digital pad */
    return (b1 << 8) | b0;
}

static void update_input(void)
{
    unsigned p1, p2;
    static int select_hold;

    pad_init_once();

    p1 = poll_pad_raw(PAD1_CTRL);     /* raw: 1 = released */
    p2 = poll_pad_raw(PAD2_CTRL);

    memset(&input, 0, sizeof(t_input));

    /* Standard digital-pad bit layout of the 16-bit response word
     * (0 = pressed).  SELECT is a exit hotkey and must be held for
     * half a second (30 frames) so a glitchy read can never quit. */
    if (!(p1 & 0x0001)) {             /* SELECT */
        if (++select_hold >= 30)
            exit_flag = 1;
    } else {
        select_hold = 0;
    }

    /* PSXLinux port: START while holding SELECT toggles the sound
     * stream on and off (see the snd.enabled note in main()).  START on
     * its own keeps its console meaning (pause), so it is ignored as a
     * toggle key whenever SELECT is held. */
    {
        static int start_was_down;
        int start_down = !(p1 & 0x0008);
        int select_held = !(p1 & 0x0001);

        if (start_down && !start_was_down && select_held) {
            snd.enabled ^= 1;
            printf("sound %s\n", snd.enabled ? "on" : "off");
        }
        start_was_down = start_down;
    }
    if (!(p1 & 0x0008) && (p1 & 0x0001))  /* START alone */
        input.system |= cart.type ? INPUT_START : INPUT_PAUSE;
    if (!(p1 & 0x0010))
        input.pad[0] |= INPUT_UP;
    else
    if (!(p1 & 0x0040))
        input.pad[0] |= INPUT_DOWN;
    if (!(p1 & 0x0080))
        input.pad[0] |= INPUT_LEFT;
    else
    if (!(p1 & 0x0020))
        input.pad[0] |= INPUT_RIGHT;
    if (!(p1 & 0x4000))                /* CROSS  -> button 1 */
        input.pad[0] |= INPUT_BUTTON1;
    if (!(p1 & 0x8000))                /* SQUARE -> button 2 */
        input.pad[0] |= INPUT_BUTTON2;

    if (!(p2 & 0x0010))
        input.pad[1] |= INPUT_UP;
    else
    if (!(p2 & 0x0040))
        input.pad[1] |= INPUT_DOWN;
    if (!(p2 & 0x0080))
        input.pad[1] |= INPUT_LEFT;
    else
    if (!(p2 & 0x0020))
        input.pad[1] |= INPUT_RIGHT;
    if (!(p2 & 0x4000))
        input.pad[1] |= INPUT_BUTTON1;
    if (!(p2 & 0x8000))
        input.pad[1] |= INPUT_BUTTON2;
}

/* ------------------------------------------------------------------ */
/* ROM loading                                                        */
/* ------------------------------------------------------------------ */

static const char *rom_names[] = {
    "game.sms", "game.gg", "rom.sms", "rom.gg", "cart.sms", "cart.gg"
};

/* MIPS o32 kernel stat layout (include/asm-mipsnommu/stat.h):
 *   st_dev(0), st_pad1[3](4-16), st_ino(16), st_mode(20),
 *   st_nlink(24), st_uid(28), st_gid(32), st_rdev(36),
 *   st_pad2[2](40-48), st_size(48)
 * The old i386-based layout (size at offset 28) does not match. */
struct stat_image {
    int dev;
    int pad1[3];
    int ino;
    int mode;
    int nlink;
    int uid;
    int gid;
    int rdev;
    int pad2[2];
    int size;          /* offset 48 */
};

static void load_rom(void)
{
    struct stat_image st;
    unsigned char *dst = base_rom;
    unsigned i;
    int size, fd = -1;

    for (i = 0; i < 6; i++) {
        fd = sys_open(rom_names[i], 0);         /* O_RDONLY */
        if (fd >= 0)
            break;
    }
    if (fd < 0 || i == 6) {
        bc(20, 255, 255, 255);           /* no ROM found */
        printf("No ROM image found (place game.sms on the CD root)\n");
        for (;;) ;                              /* stop here, visibly */
    }
    bc(3, 255, 255, 0);                 /* sys_open succeeded */

    st.size = 0;
    {
        int fstat_rc = sys_fstat(fd, &st);
        printf("fstat rc=%d size=%d\n", fstat_rc, st.size);
    }
    size = st.size;
    bc(4, 0, 255, 255);                 /* sys_fstat returned */

    printf("ROM: %s, %d bytes\n", rom_names[i], size);

    /* Strip a 512 byte Master System header (file size % 0x4000 == 0x200) */
    if ((size & 0x3FFF) == 0x200) {
        char hdr[512];
        sys_read(fd, hdr, 512);
        size -= 512;
        printf("Stripped 512 byte header from image\n");
    }

    if (size > ROM_SIZE) {
        size = ROM_SIZE;
        printf("Image too large, truncated to %d bytes\n", size);
    }

    bc(5, 255, 0, 255);                  /* before the big sys_read */

    /* The kernel's ISO read path can return short, and - worse - its
     * CD data-interrupt handling can race and deliver zeroed sectors
     * while the byte count still advances.  A partially corrupted ROM
     * boots but renders nothing but black, so every 8KB chunk is
     * verified by re-reading it: a chunk counts as good once a fresh
     * read matches what is in memory, and mismatching re-reads are
     * installed and confirmed by the next pass (races do not repeat
     * with the same content). */
    {
        static unsigned char vprobe[8192];
        int base_off = (st.size & 0x3FFF) == 0x200 ? 512 : 0;
        int got = 0, off, repairs = 0, unconfirmed = 0;

        while (got < size) {
            int n = sys_read(fd, dst + got, size - got);
            if (n <= 0)
                break;
            got += n;
        }

        for (off = 0; off < size; off += 8192) {
            int n = size - off > 8192 ? 8192 : size - off;
            int try, ok = 0;

            for (try = 0; try < 4; try++) {
                sys_lseek(fd, base_off + off, 0);
                if (sys_read(fd, vprobe, n) != n)
                    continue;
                if (memcmp(vprobe, dst + off, n) == 0) {
                    ok = 1;
                    break;
                }
                memcpy(dst + off, vprobe, n);   /* candidate data */
                if (try > 0)
                    repairs++;
            }
            if (!ok) {
                /* last chance: one more read to confirm the final
                 * candidate */
                sys_lseek(fd, base_off + off, 0);
                if (sys_read(fd, vprobe, n) == n &&
                    memcmp(vprobe, dst + off, n) == 0)
                    ok = 1;
                else
                    unconfirmed++;
            }
        }

        if (got == size && unconfirmed == 0) {
            if (repairs == 0)
                bc(19, 0, 255, 0);       /* clean read */
            else
                bc(19, 255, 128, 0);      /* verified, repairs needed */
        } else {
            bc(19, 255, 0, 0);           /* unreadable chunk(s) */
        }
    }
    sys_close(fd);

    cart.rom   = dst;
    cart.pages = (size / 0x4000);
    cart.type  = (i & 1) ? TYPE_GG : TYPE_SMS;

    printf("Console: %s, %d pages\n",
           cart.type ? "Game Gear" : "Master System", cart.pages);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

void system_load_sram(void)
{
    /* no SRAM support: the CD root is read-only */
}

int main(int argc, char *argv[])
{
    /* Minimal GPU bring-up first so the breadcrumb marks themselves
     * become visible: reset, display on, 320x240, display area (0,0),
     * clip 0..320/0..240, no offset. */
    GP1 = 0x00000000;
    GP1 = 0x03000000;
    GP1 = 0x08000001;
    GP1 = 0x05000000;
    GP0 = 0xE3000000;
    GP0 = 0xE4000000 | ((unsigned)CONSOLE_H << 10) | CONSOLE_W;
    GP0 = 0xE5000000;

    bc(0, 255, 0, 0);                   /* reached main() */
    printf("SMS Plus %s PSXLinux port\n", VERSION);
    bc(1, 0, 255, 0);                  /* printf survived */
    gpu_init();
    bc(2, 0, 0, 255);
    load_rom();
    bc(6, 255, 255, 255);

    /* Set up display bitmap (dimensions depend on the cartridge type) */
    memset(&bitmap, 0, sizeof(t_bitmap));
    bitmap.width  = cart.type ? 160 : 256;
    bitmap.height = cart.type ? 144 : 192;
    bitmap.depth  = 8;
    bitmap.pitch  = bitmap.width;       /* 8bpp, one byte per pixel */
    bitmap.data   = &bitmap_data[0];
    memset(bitmap.data, 0, bitmap.pitch * bitmap.height);

    system_init(22050);                /* PSG audio at 22050 Hz */
    /* PSXLinux port: the PSG update plus the SPU streaming cost about
     * 16% of a frame on this target, and under the usual host setups
     * the PSX SPU output is inaudible anyway, so the stream starts out
     * disabled for speed.  The sound buffers are still allocated here
     * (snd.enabled only gates the per-frame PSG and SPU work), which
     * is what makes the SELECT+START toggle in update_input() safe at
     * any time. */
    snd.enabled = 0;
    bc(7, 255, 128, 0);
    if (snd.enabled)
        bc(17, 0, 255, 0);              /* sound buffers allocated OK */
    else
        bc(17, 255, 0, 0);

    /* Reset the console AFTER the cart is loaded: cpu_reset() maps the
     * ROM pages (and vdp/sms/render state) for the loaded image.  The
     * original frontend does system_init -> load_rom -> system_reset;
     * skipping this left the Z80 executing an unmapped memory map, so
     * the game never produced any video. */
    system_reset();

    spu_init();
    bc(8, 128, 0, 0);
    pace_init();                        /* frame pacing via Timer 1 */
    bc(9, 0, 128, 128);

    /* Timer 1 health report (green = good, red = bad):
     *   slot 13: T1_MODE bit8 (hblank clock select) reads back
     *   slot 14: T1_COUNT changes while burning CPU cycles
     *   slot 15: T1 advances >= 10 ticks in the burn window
     *   slot 16: T1 advances >= 262 ticks (one field) in ~40 ms */
    {
        unsigned c0, c1;
        int j;

        if (T1_MODE & T1_HBLANK)
            bc(13, 0, 255, 0);
        else
            bc(13, 255, 0, 0);

        c0 = T1_COUNT;
        for (j = 0; j < 400000; j++)    /* ~ 15 ms */
            ;
        c1 = T1_COUNT;
        if (c1 != c0)
            bc(14, 0, 255, 0);
        else
            bc(14, 255, 0, 0);
        {
            int d = (int)(c1 - c0);
            if (d < 0) d += 0x10000;
            if (d >= 10)
                bc(15, 0, 255, 0);
            else
                bc(15, 255, 0, 0);
        }

        c0 = T1_COUNT;
        for (j = 0; j < 1200000; j++)   /* ~ 45 ms */
            ;
        c1 = T1_COUNT;
        {
            int d = (int)(c1 - c0);
            if (d < 0) d += 0x10000;
            if (d >= HBLANKS_FRAME)
                bc(16, 0, 255, 0);
            else
                bc(16, 255, 0, 0);
        }
    }

    /* Main emulation loop */
    {
        int first = 1;
        int audio_reported = 0;
        int cram_reported = 0;
        int last_stall = 0;
        unsigned frame = 0;
        unsigned last_pc = 0;
        unsigned t_video = 0, t_sms = 0, t_loop = 0;
        unsigned t_wait = 0, t_in = 0, t_spu = 0, t_dbg = 0;
        unsigned last_j = KJIFFIES;
        unsigned prev_top = T1_COUNT;  /* loop start of last iteration */
        unsigned iter_ticks = 0;       /* its length in hblank ticks */
        int skip_run = 0;              /* consecutive skipped renders */
        int skip_count = 0;            /* skipped renders this window */

        rom_checksums();
        printf("ROM %08x %08x %08x %08x %08x %08x %08x %08x\n",
               page_sums[0], page_sums[1], page_sums[2], page_sums[3],
               page_sums[4], page_sums[5], page_sums[6], page_sums[7]);

        for (;;) {
            unsigned t0_loop;

            if (exit_flag)
                break;

            /* Timer 1 runs at one tick per hblank (262 per NTSC field),
             * so ticks are the natural unit of emulated frame cost.
             * It wraps every 65536 ticks, hence the 16-bit delta. */
            t0_loop = T1_COUNT;
            iter_ticks = (unsigned short)(t0_loop - prev_top);
            prev_top = t0_loop;

            /* Make sure the GPU finished the frame we are about to show
             * (bounded), then wait out one field period (Timer 1). */
            {
                unsigned t0 = T1_COUNT;
                drawsync_wait();
                pace_frame();

                /* what we just finished drawing becomes visible */
                gpu_flip(back_y);
                t_wait += (unsigned short)(T1_COUNT - t0);
            }
            front_y = back_y;
            back_y  = (back_y == BUF_Y0) ? BUF_Y1 : BUF_Y0;

            /* fill the new back buffer */
            {
                unsigned t0 = T1_COUNT;
                update_video();
                t_video += (unsigned short)(T1_COUNT - t0);
            }

            /* Get current input */
            {
                unsigned t0 = T1_COUNT;
                update_input();
                t_in += (unsigned short)(T1_COUNT - t0);
            }

            /* Run the SMS emulation for a frame.  Rendering is skipped
             * adaptively: while an iteration fits into two fields
             * (about 30 fps) every frame is drawn, and once the
             * emulator falls behind alternate frames drop their
             * rendering - never more than three in a row, so the
             * display can not freeze - trading display rate for game
             * speed.  (Upstream's PSX frontend shipped a fixed
             * frame_skip = 32, i.e. it drew one frame in 32.) */
            {
                unsigned t0 = T1_COUNT;
                int skip = (iter_ticks >= 2 * HBLANKS_FRAME) && (skip_run < 3);

                skip_run = skip ? skip_run + 1 : 0;
                skip_count += skip;
                sms_frame(skip);
                t_sms += (unsigned short)(T1_COUNT - t0);
            }

            /* Hand the frame's samples to the SPU streamer */
            {
                unsigned t0 = T1_COUNT;
                if (snd.enabled)
                    spu_frame(snd.buffer[0], snd.buffer[1], snd.bufsize);
                t_spu += (unsigned short)(T1_COUNT - t0);
            }

            {
            unsigned t0 = T1_COUNT;

            /* breadcrumbs for the loop phase: each mark is an A0
             * upload, so it is guaranteed to appear in a dump */
            if (first) {
                bc(10, 128, 255, 0);            /* first frame done */
                first = 0;
            }
            if (!audio_reported && spu_dbg_slots >= 16) {
                bc(18, 0, 255, 0);               /* 16 ADPCM slots streamed */
                audio_reported = 1;
            }
            if ((++frame & 255) == 0)
                bc(11, 128, 0, 255);            /* still alive */
            if (pace_stalled != last_stall) {
                last_stall = pace_stalled;
                bc(12, 255, 0, 255);
            }
            if (!cram_reported && (game_activity & 1)) {
                bc(21, 0, 128, 255);         /* game is executing + writing CRAM */
                cram_reported = 1;
            }
            /* Rendering/speed diagnostics, once per 128 frames:
             *   nz  = non-zero bytes in the SMS bitmap (core output?)
             *   act = game wrote CRAM, st = pacing give-ups
             *   t1  = Timer-1 hblanks per 128 frames: 33536 means the
             *         loop keeps up with 60 emulated fields */
            if ((frame & 127) == 0) {
                int i, nz = 0;

                for (i = 0; i < 256 * 192; i++)
                    if (bitmap.data[i])
                        nz++;
                printf("F%u nz=%d ms=%u t1=%u u=%u s=%u w=%u i=%u a=%u d=%u q=%u sk=%d act=%d st=%d p0=%02x%02x%02x\n",
                       frame, nz, (KJIFFIES - last_j) * 10u, t_loop, t_video, t_sms,
                       t_wait, t_in, t_spu, t_dbg, ds_spins, skip_count,
                       game_activity, pace_stalled,
                       bitmap.pal.color[0][0], bitmap.pal.color[0][1],
                       bitmap.pal.color[0][2]);
                t_video = 0;
                t_sms = 0;
                t_loop = 0;
                t_wait = 0;
                t_in = 0;
                t_spu = 0;
                t_dbg = 0;
                ds_spins = 0;
                skip_count = 0;
                last_j = KJIFFIES;
            }
            /* Z80 liveness: sample the PC every 64 frames; mark the low
             * 8 bits as bars plus "changed since last sample". */
            if (frame == 1 || (frame & 15) == 0) {
                unsigned pc = z80_get_pc();
                unsigned sp = z80_get_reg(Z80_SP);
                unsigned af = z80_get_reg(Z80_AF);
                int b;

                if (pc != last_pc)
                    bc(23, 0, 255, 0);       /* PC moved */
                else
                    bc(23, 255, 0, 0);       /* PC stuck */
                last_pc = pc;
                for (b = 0; b < 8; b++)
                    if (pc & (1u << b))
                        bc(24 + b, 0, 255, 0);
                /* PC high byte (bits 8-15) as bars on row 1, slots 0-7
                 * (row 0 would be full) */
                for (b = 0; b < 8; b++)
                    if (pc & (0x100u << b))
                        bc_row(b, 1, 0, 255, 0);

                /* VDP state: display enable (reg1 bit6), backdrop
                 * colour (reg7 low 4 bits), vblank flag */
                if (vdp.reg[1] & 0x40)
                    bc(32, 0, 255, 0);       /* display ON */
                else
                    bc(32, 255, 0, 0);       /* still blanked */
                for (b = 0; b < 4; b++)
                    if (vdp.reg[7] & (1u << b))
                        bc(33 + b, 0, 255, 0);
                if (vdp.status & 0x80)
                    bc(37, 0, 255, 0);       /* vblank seen */

                /* console text channel (appears in the pcsx log) */
                printf("Z8 pc=%04x sp=%04x af=%04x bc=%02x%02x%02x%02x d=%d s=%02x\n",
                       pc, sp, af, sms.fcr[0], sms.fcr[1], sms.fcr[2],
                       sms.fcr[3], (vdp.reg[1] & 0x40) != 0, vdp.status);
            }

            /* one-shot loader-proof: green bars, rows 0-1: per-page
             * checksum bits (page 0-4 row 0, page 5-7 row 1) so the
             * host can compare the port's ROM against the file's */
            if (frame == 45) {
                int p, b;

                for (p = 0; p < 8; p++)
                    for (b = 0; b < 8; b++)
                        if ((page_sums[p] >> b) & 1u)
                            bc_row((p < 5) ? p * 8 + b : (p - 5) * 8 + b,
                                   p < 5 ? 0 : 1, 0, 255, 0);
            }

            /* one-shot crash-state snapshot (cyan bars, rows 0-1):
             * 64 bits = PC[15:0], SP[15:0], AF[15:0], fcr[0..3] 4 bits
             * each.  Fired at frame 97 so it lands between periodic
             * samples and never collides with the green marks. */
            if (frame == 97) {
                unsigned sp = z80_get_reg(Z80_SP);
                unsigned af = z80_get_reg(Z80_AF);
                unsigned words[4];
                int s, b;

                words[0] = z80_get_pc();
                words[1] = sp;
                words[2] = af;
                words[3] = ((unsigned)sms.fcr[3] << 12) |
                           ((unsigned)sms.fcr[2] << 8)  |
                           ((unsigned)sms.fcr[1] << 4)  |
                            (unsigned)sms.fcr[0];
                s = 0;
                for (b = 0; b < 4; b++)
                    for (; s < (b + 1) * 16; s++)
                        if (words[b] & (1u << (s & 15)))
                            bc_row(s % 40, s / 40, 0, 255, 255);
            }

            t_dbg += (unsigned short)(T1_COUNT - t0);
            }

            t_loop += (unsigned short)(T1_COUNT - t0_loop);
        }
    }

    spu_shutdown();
    printf("Quit\n");
    exit(0);
    return 0;
}

/* C entry point (ECOFF loader puts usermode EIP here) */
void __main(void) { }

__attribute__((section(".text.start")))
void _start(void)
{
    int ret = main(0, 0);
    sys_exit(ret);
    for (;;) ;
}
