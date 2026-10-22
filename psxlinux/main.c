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
#define CONSOLE_W   256
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
/* The GPU keeps these resources in VRAM across buffer flips.  An
 * adaptively skipped core render leaves the bitmap untouched, so avoid
 * submitting the same 24 KiB texture again on those frames. */
static int texture_dirty = 1;
static int clut_valid;

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


static void drawsync_wait(void)
{
    int i;

    for (i = 0; i < 200000; i++)
        if (GPUSTAT & 0x04000000u)          /* bit26: GPU ready, not busy */
            return;
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

static void vram_upload_img(int x, int y, int w, int h, const void *pix, int pitch);

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
    GP1 = 0x08000000;                   /* 256x240, 15bpp, NTSC */
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
/* GPU tile renderer (SMS)                                             */
/* ------------------------------------------------------------------ */

/* The PSX GPU draws the SMS picture itself: every 8x8 pattern lives in
 * VRAM as a 4bpp texture and every on-screen tile is a textured quad.
 * That removes the software renderer's per-pixel memory traffic, which
 * measurement showed to be the most expensive part of a frame under
 * the host interpreter.  Behaviour mirrors render.c:
 *
 *   - a background texel 0 shows its bank's palette entry 0 (opaque);
 *     a sprite texel 0 never writes (the LUT leaves the background),
 *   - a priority tile (name bit 12) wins over every sprite pixel: the
 *     software LUT returns the input for pixels marked 0x40, so
 *     priority tiles are drawn in a second pass after the sprites,
 *   - sprites are drawn back to front, so the first sprite of the
 *     list keeps its pixels, like the LUT's "sprite already here" rule.
 *
 * Not reproduced: the per-line sprite limit (purely visual), the sprite
 * collision flag and per-line palette changes.  A game writing the
 * scroll registers during the active display is caught by the
 * vdp_raster hook (vdp.c) and served by the software path on the next
 * frame. */

#define TILE_TPAGE      0x05            /* 4bpp pages: +0 none, +1 H, +2 V, +3 HV */
#define TILE_HW         320             /* halfword x of pattern page 0x05 */
#define TILE_CLUT_N0    CLUT_ATTR       /* bank 0 tiles: entry 0 opaque */
#define TILE_CLUT_N1    (CLUT_ATTR + 1) /* bank 1 tiles: entry 0 opaque */
#define TILE_CLUT_S     (CLUT_ATTR + 2) /* sprites: entry 0 transparent */

static int gpu_tiles;                    /* this frame uses the tile path */
static int raster_pending;               /* last frame saw a raster effect */
static int raster_reported;
static int tile_clut_valid;
static int was_gpu;                      /* tile path used by the last frame */
static int gpu_quads;                    /* diagnostics: quads per window */

extern int vdp_raster;                   /* set by vdp_ctrl_w() in vdp.c */
extern byte vram_dirty[];                /* render.c, one flag per pattern */
extern byte is_vram_dirty;

/* CRAM entry (2 bits per gun, as palette_sync decodes it) to a PSX
 * 15bpp halfword with the STP bit set: only an exact 0x0000 CLUT entry
 * is transparent, so a game colour of pure black must not map to the
 * bare colour (it would punch a hole to the backdrop).  The software
 * path never noticed because its clear fill was black anyway. */
static unsigned cram15(unsigned c)
{
    return 0x8000u | (((c >> 0) & 3) << 3) | (((c >> 2) & 3) << 8) |
           (((c >> 4) & 3) << 13);
}

/* Fill command word for the backdrop colour (palette entry 16 +
 * reg[7], i.e. BACKDROP_COLOR). */
static unsigned backdrop_fill(void)
{
    unsigned c = vdp.cram[0x10 | (vdp.reg[7] & 0x0F)];
    unsigned r = ((c >> 0) & 3) << 6;    /* fill fields are 8 bit */
    unsigned g = ((c >> 2) & 3) << 6;
    unsigned b = ((c >> 4) & 3) << 6;

    return 0x02000000u | (b << 16) | (g << 8) | r;
}

/* Pack one 8x8 pattern (four bit planes) into 32 bytes of 4bpp texels,
 * optionally mirrored.  A halfword holds four texels; peops' 4bpp
 * sampler takes texel 2b from the LOW nibble of byte b and texel
 * 2b+1 from the high nibble. */
static void tile_pack(unsigned *dst, int n, int fh, int fv)
{
    const unsigned char *p = &vdp.vram[(n & 0x1FF) << 5];
    unsigned char *out = (unsigned char *)dst;
    int r;

    memset(out, 0, 32);
    for (r = 0; r < 8; r += 1) {
        const unsigned char *row = &p[(fv ? 7 - r : r) << 2];
        unsigned b0 = row[0], b1 = row[1], b2 = row[2], b3 = row[3];
        unsigned char *o = &out[r << 2];
        int x;

        for (x = 0; x < 8; x += 1) {
            int sx = fh ? 7 - x : x;
            unsigned t = (((b0 >> (7 - sx)) & 1) << 0) |
                         (((b1 >> (7 - sx)) & 1) << 1) |
                         (((b2 >> (7 - sx)) & 1) << 2) |
                         (((b3 >> (7 - sx)) & 1) << 3);
            o[x >> 1] |= (unsigned char)(t << ((x & 1) ? 4 : 0));
        }
    }
}


/* Per-pattern staleness of the four mirrored copies (bits: 1 none,
 * 2 H, 4 V, 8 HV).  A variant is uploaded lazily, into the frame's
 * packet list, the first time a quad samples it after a rewrite -
 * animated games only pay for the variants they actually show.  The
 * scratch pattern buffer lives on the stack. */
static unsigned char tile_stale[0x200];

/* Append the A0 that refreshes one variant of pattern n (pack +
 * transfer), and clear its stale bit.  The A0 header is three words:
 * command, (y<<16)|x, (h<<16)|w - same as the proven vram_upload_img. */
static unsigned *tile_refresh(unsigned *pk, int n, int variant)
{
    unsigned pat[8];                     /* 32 bytes of 4bpp texels */

    tile_pack(pat, n, variant & 1, variant >> 1);
    pk[0] = 0xA0000000u;
    pk[1] = ((unsigned)((n >> 5) << 3) << 16)
          | (unsigned)(TILE_HW + (variant << 6) + ((n & 31) << 1));
    pk[2] = (8u << 16) | 2u;             /* h = 8 rows, w = 2 halfwords */
    memcpy(&pk[3], pat, 32);
    tile_stale[n] &= (unsigned char)~(1u << variant);
    return pk + 11;
}

/* Refresh the three tile CLUTs (48 halfwords at the old CLUT spot).
 * Entries are plain opaque colours like the proven 8bpp CLUT; the
 * sprite CLUT's entry 0 is 0x0000, the only fully transparent value. */
static void tiles_update_clut(void)
{
    static unsigned short hw[48];
    int i;

    if (tile_clut_valid && !bitmap.pal.update)
        return;
    if (bitmap.pal.update)
        game_activity |= 1;              /* game wrote its palette */
    bitmap.pal.update = 0;
    tile_clut_valid = 1;

    for (i = 0; i < 16; i += 1) {
        unsigned c1 = cram15(vdp.cram[0x10 + i]);

        hw[i] = cram15(vdp.cram[i]);     /* tiles, bank 0 */
        hw[16 + i] = c1;                 /* tiles, bank 1 */
        hw[32 + i] = i ? c1 : 0x0000;    /* sprites: 0 = transparent */
    }
    vram_upload_img(CLUT_X, CLUT_Y, 48, 1, hw, 96);
}

/* One E1 texture-page select + one textured rectangle: the tile
 * cache's flip variants live on different pages, and rectangles take
 * their page from the global register only. */
static unsigned *rect_put(unsigned *pk, int x, int y, int w, int h,
                          int u, int v, unsigned tpage, unsigned clut)
{
    pk[0] = 0xE1000000u | tpage;
    pk[1] = 0x64808080u;                 /* textured rect, identity modulate */
    pk[2] = ((unsigned)y << 16) | (unsigned)(x & 0xFFFF);
    pk[3] = (clut << 16) | ((unsigned)v << 8) | (unsigned)u;
    pk[4] = ((unsigned)h << 16) | (unsigned)w;
    gpu_quads += 1;
    return pk + 5;
}

/* Build and submit one frame: backdrop fill, plain background tiles,
 * sprites (back to front), priority tiles, and the blank left column.
 * The name-table walk mirrors render_bg_sms:
 *
 *   attr       = nt[((k + vs>>3) % 28) * 32 + (j + hs>>3) & 31]
 *   screen pos = (j*8 - hs&7, k*8 - vs&7)  [bitmap pixels]
 *
 * with hs forced to 0 for the top two tile rows when reg[0] bit 6 is
 * set, and vs frozen to 0 for screen columns >= 24 when bit 7 is set.
 * The list is bounded by construction: 25x34 tiles + 64x2 sprites. */
static void tiles_submit_frame(int yoff)
{
    unsigned *pk = (unsigned *)bitmap.data;
    unsigned *prio = pk + 11200;         /* side list: priority tiles */
    unsigned fill = backdrop_fill();
    int hs = (0x100 - vdp.reg[8]) & 0xFF;
    int vs = vdp.reg[9];
    int display = (vdp.reg[1] & 0x40) && (vdp.reg[2] & 1);
    int lock_v = vdp.reg[0] & 0x80;
    int lock_h = vdp.reg[0] & 0x40;
    int k, j, i, x, nprio = 0;

    *pk++ = fill;                         /* backdrop fill, absolute xy */
    *pk++ = (unsigned)(back_y + yoff) << 16;
    *pk++ = ((unsigned)(CONSOLE_H - yoff) << 16) | (unsigned)CONSOLE_W;

    if (display) {
        const word *nt0 = (const word *)&vdp.vram[vdp.ntab];
        int vrow = vs >> 3;                    /* whole-tile v scroll */
        int yfin = vs & 7;                     /* fine v scroll, px */

        /* Pass 1: plain tiles straight into the list; priority tiles
         * are parked in a side list (attr + position, one word each)
         * and drawn after the sprites - the software LUT's rule that a
         * pixel marked 0x40 is final. */
        for (k = 0; k < 25; k += 1) {
            int hsl = (lock_h && k < 2) ? 0 : hs;
            int xoff = hsl & 7;                    /* fine h scroll, px */
            int col0 = hsl >> 3;
            const word *ntr = nt0 + (((k + vrow) % 28) << 5);
            const word *ntr_locked = nt0 + (k << 5);
            int y = 8 * k - yfin + yoff;

            for (j = -1, x = -8 - xoff; j < 33; j += 1, x += 8) {
                int col = (j + col0) & 0x1F;
                word attr = (lock_v && (j >= 24)) ? ntr_locked[col & 0x1F]
                                                  : ntr[col & 0x1F];
                int n = attr & 0x1FF;
                int variant = ((attr >> 9) & 1) + ((attr >> 10) & 1) * 2;
                unsigned tp = TILE_TPAGE + variant;
                unsigned cl = (attr & 0x800) ? TILE_CLUT_N1 : TILE_CLUT_N0;
                int yy = (lock_v && (j >= 24)) ? 8 * k + yoff : y;

                if (vram_dirty[n]) {
                    tile_stale[n] = 0x0F;    /* the game redrew it */
                    vram_dirty[n] = 0;
                }
                if (tile_stale[n] & (1u << variant))
                    pk = tile_refresh(pk, n, variant);
                if (attr & 0x1000) {
                    prio[nprio++] = (unsigned)(attr & 0x1FFF)
                                  | ((unsigned)(x + 8) << 16)
                                  | ((unsigned)k << 25);
                } else {
                    pk = rect_put(pk, x, yy, 8, 8,
                                  (n & 31) << 3, (n >> 5) << 3, tp, cl);
                }
            }
        }

        /* Sprites, back to front so the first of the list wins.
         * Sizes follow render_obj: zoom (reg[1] bit 0) doubles both
         * axes of the 8x8 pattern, 8x16 mode (bit 1) stacks patterns
         * n and n+1 vertically. */
        {
            const byte *st = (const byte *)&vdp.vram[vdp.satb];
            int zoom = vdp.reg[1] & 0x01;
            int tall = vdp.reg[1] & 0x02;
            int r;

            i = 0;
            while ((i < 64) && (st[i] != 208))
                i += 1;

            for (i = i - 1; i >= 0; i -= 1) {
                int yp = st[i] + 1;
                int xp = st[0x80 + (i << 1)];
                int n = st[0x81 + (i << 1)];
                int w = zoom ? 16 : 8;     /* screen px */
                int h = zoom ? 16 : 8;
                int rows = tall ? 2 : 1;

                if (yp > 240) yp -= 256;
                if (vdp.reg[0] & 0x08) xp -= 8;
                if (vdp.reg[6] & 0x04) n |= 0x0100;
                if (tall) n &= 0x01FE;

                for (r = 0; r < rows; r += 1) {
                    int pn = n + r;

                    /* sprites share the tile cache; refresh their
                     * pattern lazily like the background does */
                    if (vram_dirty[pn]) {
                        tile_stale[pn] = 0x0F;
                        vram_dirty[pn] = 0;
                    }
                    if (tile_stale[pn] & 1u)   /* sprites never flip */
                        pk = tile_refresh(pk, pn, 0);

                    pk = rect_put(pk, xp, yp + r * 8 + yoff, w, h,
                                  (pn & 31) << 3, (pn >> 5) << 3,
                                  TILE_TPAGE, TILE_CLUT_S);
                }
            }
        }

        /* Pass 2: the parked priority tiles, over the sprites. */
        for (i = 0; i < nprio; i += 1) {
            unsigned e = prio[i];
            int n = e & 0x1FF;
            int x = (int)((e >> 16) & 0x1FF) - 8;
            int kk = (int)((e >> 25) & 0x1F);
            int yfin = vs & 7;
            int yy = ((lock_v && (((e >> 16) & 0x1FF) - 8) >= 24)
                      ? 8 * kk : 8 * kk - yfin) + yoff;
            int variant = ((e >> 9) & 1) + ((e >> 10) & 1) * 2;
            unsigned tp = TILE_TPAGE + variant;
            unsigned cl = (e & 0x800) ? TILE_CLUT_N1 : TILE_CLUT_N0;

            if (tile_stale[n] & (1u << variant))
                pk = tile_refresh(pk, n, variant);
            pk = rect_put(pk, x, yy, 8, 8,
                          (n & 31) << 3, (n >> 5) << 3, tp, cl);
        }
    }

    if (display && (vdp.reg[0] & 0x20)) {
        /* blank the leftmost column (8 bitmap pixels = 10 screen) */
        *pk++ = fill;
        *pk++ = (unsigned)(back_y + yoff) << 16;
        *pk++ = ((unsigned)(CONSOLE_H - yoff) << 16) | 8u;
    }

    dma_to_gpu((unsigned *)bitmap.data, pk - (unsigned *)bitmap.data);
}


/* ------------------------------------------------------------------ */
/* video                                                              */
/* ------------------------------------------------------------------ */

static void update_clut(void)
{
    static unsigned short clut_hw[128];
    int i;

    if (!clut_valid || bitmap.pal.update) {
        if (bitmap.pal.update)
            game_activity |= 1;             /* game wrote its palette */
        bitmap.pal.update = 0;
        clut_valid = 1;
    } else {
        return;
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

static void update_video(void)
{
    int w_tex, h_tex, w_dst, h_dst;

    /* target the current back buffer (clip + draw offset must be reset
     * every frame since the buffer alternates) */
    set_draw_area();
    set_draw_offset();

    if (gpu_tiles) {
        /* SMS frame drawn by the GPU: refresh the tile cache and the
         * CLUTs, then submit one packet list (backdrop fill, tiles,
         * sprites, priority tiles) straight into the back buffer.
         * The 8bpp bitmap is unused here and its storage serves as
         * the packet scratch space. */
        if (!was_gpu) {
            /* the software path overwrote our VRAM: repaint it all */
            memset(tile_stale, 0x0F, sizeof(tile_stale));
            tile_clut_valid = 0;
        }
        was_gpu = 1;
        clut_valid = 0;   /* its 128-entry CLUT replaced ours */

        tiles_update_clut();
        tiles_submit_frame(0);
        return;
    }
    was_gpu = 0;

    /* The main loop waited for the previous buffer before flipping it,
     * so this buffer's GPU work is already complete here. */
    update_clut();

    w_tex = cart.type ? 160 : 256;
    h_tex = cart.type ? 144 : 192;
    if (texture_dirty) {
        vram_upload_img(TEX_X, TEX_Y, w_tex / 2, h_tex, bitmap.data, bitmap.pitch);
        texture_dirty = 0;
    }

    /* clear the logical back buffer: fill packets use absolute
     * coordinates and ignore the draw offset */
    gp0_cmd(0x02000000u);
    gp0_cmd((unsigned)back_y << 16);
    gp0_cmd(((unsigned)CONSOLE_H << 16) | (unsigned)CONSOLE_W);

    /* present: the display is the bitmap's own size (256 wide like the
     * SMS), so a 1:1 textured quad centers it vertically */
    w_dst = w_tex;
    h_dst = h_tex;
    draw_bitmap_quad(w_tex, h_tex, w_dst, h_dst, TEX_TPAGE, CLUT_ATTR);
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
        printf("No ROM image found (place game.sms on the CD root)\n");
        for (;;) ;                              /* stop here, visibly */
    }

    st.size = 0;
    {
        int fstat_rc = sys_fstat(fd, &st);
        printf("fstat rc=%d size=%d\n", fstat_rc, st.size);
    }
    size = st.size;

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

        if (got == size && unconfirmed == 0)
            printf("ROM verified, %d chunk repairs\n", repairs);
        else
            printf("ROM UNREADABLE: %d bytes short, %d chunks unconfirmed\n",
                   size - got, unconfirmed);
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
    /* Minimal GPU bring-up first: reset, display on, 256x240 (the
     * SMS's native width), display area (0,0), no offset. */
    GP1 = 0x00000000;
    GP1 = 0x03000000;
    GP1 = 0x08000000;
    GP1 = 0x05000000;
    GP0 = 0xE3000000;
    GP0 = 0xE4000000 | ((unsigned)CONSOLE_H << 10) | CONSOLE_W;
    GP0 = 0xE5000000;

    printf("SMS Plus %s PSXLinux port\n", VERSION);
    gpu_init();
    load_rom();

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

    /* Reset the console AFTER the cart is loaded: cpu_reset() maps the
     * ROM pages (and vdp/sms/render state) for the loaded image.  The
     * original frontend does system_init -> load_rom -> system_reset;
     * skipping this left the Z80 executing an unmapped memory map, so
     * the game never produced any video. */
    system_reset();

    spu_init();
    pace_init();                        /* frame pacing via Timer 1 */

    /* Main emulation loop */
    {
        unsigned frame = 0;
        unsigned t_video = 0, t_sms = 0, t_loop = 0;
        unsigned t_wait = 0, t_in = 0, t_spu = 0;
        unsigned t_sr = 0, t_ss = 0;         /* rendered / skipped frame cost */
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

            /* Path decision: SMS frames are drawn by the GPU tile
             * renderer; Game Gear, and SMS frames following a
             * mid-frame scroll change (raster effect, flagged from
             * vdp_ctrl_w), use the software bitmap path. */
            gpu_tiles = (cart.type == TYPE_SMS) && !raster_pending;
            raster_pending = vdp_raster;
            vdp_raster = 0;
            if (raster_pending && !raster_reported) {
                raster_reported = 1;
                printf("raster effect: software renderer fallback\n");
            }

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
             * as soon as an iteration misses a 60 Hz field.  The old
             * two-field threshold kept rendering at 22-30 fps even
             * though it was exactly the work preventing the emulator
             * from catching up.  Never skip more than three frames in
             * a row: game logic stays at full rate and the display
             * cannot freeze indefinitely. */
            {
                unsigned t0 = T1_COUNT;
                int skip = (iter_ticks > HBLANKS_FRAME) && (skip_run < 3);

                if (gpu_tiles)
                    skip = 0;             /* no software render to skip */
                skip_run = skip ? skip_run + 1 : 0;
                skip_count += skip;
                sms_frame(gpu_tiles ? 1 : skip);
                if (!gpu_tiles && !skip)
                    texture_dirty = 1;
                if (skip)
                    t_ss += (unsigned short)(T1_COUNT - t0);
                else
                    t_sr += (unsigned short)(T1_COUNT - t0);
                t_sms += (unsigned short)(T1_COUNT - t0);
            }

            /* Hand the frame's samples to the SPU streamer */
            {
                unsigned t0 = T1_COUNT;
                if (snd.enabled)
                    spu_frame(snd.buffer[0], snd.buffer[1], snd.bufsize);
                t_spu += (unsigned short)(T1_COUNT - t0);
            }

            frame++;

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
                printf("F%u nz=%d t1=%u u=%u s=%u sr=%u ss=%u gq=%d w=%u i=%u a=%u sk=%d act=%d st=%d p0=%02x%02x%02x\n",
                       frame, nz, t_loop, t_video, t_sms, t_sr, t_ss,
                       gpu_quads,
                       t_wait, t_in, t_spu, skip_count,
                       game_activity, pace_stalled,
                       bitmap.pal.color[0][0], bitmap.pal.color[0][1],
                       bitmap.pal.color[0][2]);
                t_video = 0;
                t_sms = 0;
                t_loop = 0;
                t_wait = 0;
                t_in = 0;
                t_spu = 0;
                t_sr = 0;
                t_ss = 0;
                gpu_quads = 0;
                skip_count = 0;
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
