/*
 * spu.c - SPU-ADPCM (BRR) streaming audio for the PSXLinux port of SMS Plus.
 *
 * The PlayStation SPU cannot play raw 16-bit PCM sample streams; its
 * sample RAM holds ADPCM blocks in the CD-XA/BRR format.  (The bare-metal
 * SDK's raw-PCM "psx_snd_play_tone" path only works through the broken
 * kernel psxspu.c shim - never on real hardware.)
 *
 * So this module is a small streaming ADPCM encoder: every emulated frame
 * the frontend hands in PSG samples, which are encoded into SPU-ADPCM
 * blocks and uploaded into two ring buffers in SPU RAM (voice 0 = left,
 * voice 1 = right).  Each ring is 8 slots, one slot = 14 blocks = 392
 * samples = 224 bytes.  The voice loops seamlessly around its ring via
 * the End+Repeat flag (0x03) on every slot's last block plus per-voice
 * LSAX (loop start address) register writes, so playback never leaves
 * the ring; the CPU tracks the playback position with the SPU IRQ
 * (IRQA register + polling I_STAT bit 9, unmasked in I_MASK just like
 * miniglut unmasks the controller IRQ bit 7).
 *
 * Timing: 22050 Hz output, 367 samples per 59.94 Hz field.  The master
 * clock is paced by VSync, so the pitch register is trimmed a few counts
 * to keep production and consumption rates matched instead of tracking
 * position exactly.
 */
#include "port.h"
#include "hw.h"
#include "spu.h"

/* ------------------------------------------------------------------ */
/* SPU hardware                                                       */
/* ------------------------------------------------------------------ */

#define SPU_VREG(v, o)  (*(volatile unsigned short *)(0x1F801C00u + (v)*16u + (o)))
#define SPU_MVOL_L      (*(volatile unsigned short *)0x1F801D80)
#define SPU_MVOL_R      (*(volatile unsigned short *)0x1F801D82)
#define SPU_KON         (*(volatile unsigned short *)0x1F801D88)
#define SPU_KOF         (*(volatile unsigned short *)0x1F801D8C)
#define SPU_IRQA        (*(volatile unsigned short *)0x1F801DA4)
#define SPU_TSA         (*(volatile unsigned short *)0x1F801DA6)
#define SPU_DATA        (*(volatile unsigned short *)0x1F801DA8)
#define SPU_CTRL        (*(volatile unsigned short *)0x1F801DAA)
#define SPU_STATX       (*(volatile unsigned short *)0x1F801DAE)

#define SPUCNT_ENABLE   0xC000u     /* enable + unmute */
#define SPUCNT_MANUALW  0x0005u     /* manual write mode (kernel demo protocol) */

#define STATX_BUSY      0x0400u     /* transfer busy */

/* ------------------------------------------------------------------ */
/* ring layout                                                        */
/* ------------------------------------------------------------------ */

/* SPU RAM addresses are in 8-byte units for all address registers. */
#define SLOT_BYTES      224             /* 14 blocks * 16 bytes */
#define SLOT_UNITS      28              /* in 8-byte units      */
#define RING_SLOTS      8
#define BLOCK_SAMPLES   28
#define SLOT_SAMPLES    (14 * BLOCK_SAMPLES)    /* 392 */

#define L_RING_UNITS    0x200u          /* byte addr 0x1000 */
#define R_RING_UNITS    0x2E0u          /* byte addr 0x1700 */

static int slot_start_units(int ring, int slot)
{
    return (ring ? R_RING_UNITS : L_RING_UNITS) + (unsigned)slot * SLOT_UNITS;
}

/* ------------------------------------------------------------------ */
/* ADPCM encoder (mirrors the SPU decoder exactly)                     */
/* ------------------------------------------------------------------ */

static const short FIR[5][2] = {
    {   0,   0 },
    {  60,   0 },
    { 115, -52 },
    {  98, -55 },
    { 122, -60 },
};

static int clamp16(int x)
{
    if (x < -32768) return -32768;
    if (x >  32767) return  32767;
    return x;
}

/* encode 28 samples into one 16-byte ADPCM block, minimising the
 * squared error against what the SPU decoder will reconstruct */
static void encode_block(const short *in, unsigned char *out, int flags,
                         int *h1p, int *h2p)
{
    int h1 = *h1p, h2 = *h2p;
    int f, s, i;
    int best_f = 0, best_s = 0;
    long best_err = 0x7FFFFFFFL;
    int win_h1 = h1, win_h2 = h2;
    signed char best_n[BLOCK_SAMPLES];

    for (f = 0; f < 5; f++) {
        const short f1 = FIR[f][0], f2 = FIR[f][1];
        int q1 = h1, q2 = h2;
        long dmax = 0;
        int s0, s_lo, s_hi;

        /* quick pass with the true samples to bracket the shift */
        for (i = 0; i < BLOCK_SAMPLES; i++) {
            long pred = ((long)f1 * q1 + (long)f2 * q2) >> 6;
            long d = (long)in[i] - pred;
            long ad = (d >= 0) ? d : -d;
            if (ad > dmax)
                dmax = ad;
            q2 = q1;
            q1 = in[i];
        }
        s0 = 0;
        while (s0 < 12 && (8L << s0) < dmax)
            s0++;
        s_lo = (s0 > 0) ? s0 - 1 : 0;
        s_hi = (s0 < 12) ? s0 + 1 : 12;

        /* exact simulation per candidate shift */
        for (s = s_lo; s <= s_hi; s++) {
            int p1 = h1, p2 = h2;
            long err = 0;
            signed char ns[BLOCK_SAMPLES];

            for (i = 0; i < BLOCK_SAMPLES; i++) {
                long pred = ((long)f1 * p1 + (long)f2 * p2) >> 6;
                long d = (long)in[i] - pred;
                int n, dec;

                /* round to nearest at this shift, clamp to 4 bits */
                if (s > 0)
                    n = (int)((d + (1L << (s - 1))) >> s);
                else
                    n = (int)d;
                if (n < -8)  n = -8;
                if (n >  7)  n =  7;
                dec = clamp16((int)((long)n << s) + (int)pred);
                err += (long)(dec - in[i]) * (dec - in[i]);
                p2 = p1;
                p1 = dec;
                ns[i] = (signed char)n;
            }
            if (err < best_err) {
                best_err = err;
                best_f = f;
                best_s = s;
                win_h1 = p1;
                win_h2 = p2;
                for (i = 0; i < BLOCK_SAMPLES; i++)
                    best_n[i] = ns[i];
                if (err == 0)
                    f = 5;              /* exact: stop searching */
                break;
            }
        }
    }

    /* serialise the block */
    out[0] = (unsigned char)(best_s | (best_f << 4));
    out[1] = (unsigned char)flags;
    for (i = 0; i < BLOCK_SAMPLES; i += 2)
        out[2 + (i >> 1)] = (unsigned char)((best_n[i] & 0xF) |
                                            ((best_n[i + 1] & 0xF) << 4));

    *h1p = win_h1;
    *h2p = win_h2;
}

/* ------------------------------------------------------------------ */
/* streaming state                                                    */
/* ------------------------------------------------------------------ */

static short stage[2][SLOT_SAMPLES * 2];
static int   stage_n[2];
static int   enc_hist[2][2];            /* per-channel encoder history */

static unsigned char slot_hw[2][SLOT_BYTES];    /* staging for one slot */

static int reader;                      /* slot the voices are playing  */
static int writer;                      /* next slot to produce into    */
static int pitch_reg = 0x7FC;

/* ------------------------------------------------------------------ */
/* low level                                                          */
/* ------------------------------------------------------------------ */

static void spu_delay(int n)
{
    volatile int i = n;
    while (--i >= 0)
        ;
}

/* push halfwords into SPU RAM through the manual transfer port.
 * Protocol matches the kernel's proven spxgame demo: set the transfer
 * address, enable manual write (CTRL bit0 = manual, bit2 kept on),
 * burst the data in 32-halfword chunks with short settles, then
 * restore normal playback mode.  Uses the kernel demo's proven manual
 * write protocol (CTRL bit0 = manual transfer, bit2 on); when a voice
 * stream is live the SPU is re-enabled immediately afterwards, which
 * is why every write is short (<= 112 halfwords). */
static void spu_ram_write(unsigned addr_units, const unsigned short *hw, int n)
{
    SPU_TSA  = (unsigned short)addr_units;
    spu_delay(8);
    SPU_CTRL = SPUCNT_MANUALW;          /* bits: manual write mode */
    spu_delay(32);
    while (n > 0) {
        int chunk = (n > 32) ? 32 : n;
        n -= chunk;
        while (chunk-- > 0)
            SPU_DATA = *hw++;
        spu_delay(32);
    }
    spu_delay(64);
    SPU_CTRL = SPUCNT_ENABLE;           /* back to playback mode */
    spu_delay(32);
}

static void set_loop_targets(void)
{
    unsigned next;
    int v;

    for (v = 0; v < 2; v++) {
        next = (unsigned)slot_start_units(v, (reader + 1) & (RING_SLOTS - 1));
        SPU_VREG(v, 14) = (unsigned short)next;     /* LSAX */
    }
    SPU_IRQA = (unsigned short)slot_start_units(0, (reader + 1) & (RING_SLOTS - 1));
}

/* encode one slot of staged samples for a channel */
static void encode_slot(int ch, int flags_last)
{
    unsigned char *base = slot_hw[ch];
    int b;

    for (b = 0; b < 14; b++)
        encode_block(&stage[ch][b * BLOCK_SAMPLES],
                     base + b * 16,
                     (b == 13) ? flags_last : 0x00,
                     &enc_hist[ch][0], &enc_hist[ch][1]);
}

/* ------------------------------------------------------------------ */
/* public interface                                                   */
/* ------------------------------------------------------------------ */

void spu_init(void)
{
    int v, s;

    /* make sure kernel/BIOS SPU state can't interfere */
    SPU_CTRL = 0x0000;
    SPU_KOF  = 0xFFFF;
    spu_delay(256);

    /* enable the SPU position IRQ (bit 9) and the controller IRQ (bit 7
     * is handled by the pad code; unmask both) */
    I_MASK &= ~(0x200u | 0x080u);
    I_STAT  = ~0x200u;

    /* build an encoded-silence slot template; every slot's last block
     * carries the End+Repeat flag so the voices always chain via LSAX */
    {
        int b, i;
        for (i = 0; i < SLOT_BYTES; i++)
            slot_hw[0][i] = 0;
        for (b = 0; b < 14; b++)
            slot_hw[0][b * 16 + 1] = (b == 13) ? 0x03 : 0;
        memcpy(slot_hw[1], slot_hw[0], SLOT_BYTES);
    }

    /* fill both rings with encoded silence */
    for (s = 0; s < RING_SLOTS; s++) {
        for (v = 0; v < 2; v++) {
            spu_ram_write(slot_start_units(v, s),
                          (const unsigned short *)slot_hw[v], 112);
        }
    }

    /* master + voice volumes */
    SPU_MVOL_L = 0x3FFF;
    SPU_MVOL_R = 0x3FFF;
    for (v = 0; v < 2; v++) {
        /* sustain-forever envelope, full volume on this voice's own side */
        SPU_VREG(v, 0)  = (v == 0) ? 0x3FFF : 0x0000;   /* voice 0 -> left  */
        SPU_VREG(v, 2)  = (v == 0) ? 0x0000 : 0x3FFF;   /* voice 1 -> right */
        SPU_VREG(v, 4)  = (unsigned short)pitch_reg;
        SPU_VREG(v, 6)  = (unsigned short)slot_start_units(v, 0);   /* SSA */
        /* sustain-forever envelope, exactly as the kernel's spxgame demo
         * uses it: instant attack, decay rate 0 (frozen) = full volume
         * for as long as the voice is running */
        SPU_VREG(v, 8)  = 0x000F;                       /* ADSR1 */
        SPU_VREG(v, 10) = 0x0000;                       /* ADSR2 */
    }

    reader = 0;
    writer = 2;                        /* slots 0/1 play silence while      */
    stage_n[0] = stage_n[1] = 0;       /* production fills the pipeline     */
    enc_hist[0][0] = enc_hist[0][1] = 0;
    enc_hist[1][0] = enc_hist[1][1] = 0;

    set_loop_targets();
    I_STAT = ~0x200u;

    /* unmute and key on both voices */
    SPU_CTRL = SPUCNT_ENABLE;
    spu_delay(64);
    SPU_KON = 0x0003;
}

void spu_shutdown(void)
{
    SPU_KOF  = 0x0003;
    spu_delay(256);
    SPU_CTRL = 0x0000;
}

/* follow the hardware position through the ring via the SPU IRQ */
static void spu_poll(void)
{
    if ((I_STAT & 0x200u) != 0) {
        I_STAT = ~0x200u;
        reader = (reader + 1) & (RING_SLOTS - 1);
        set_loop_targets();
    }
}

/* produced-slot counter, exported for the frontend's GPU breadcrumbs
 * (audio has no other observable channel in the emulator) */
int spu_dbg_slots;

/* produce slots while samples are staged and the ring has room; the
 * pitch register is trimmed so that consumption follows production */
static void spu_produce(void)
{
    int fullness = (writer - reader) & (RING_SLOTS - 1);
    int ch;

    /* pitch trim: fullness too small -> voice is eating too fast */
    if (fullness < 3 && pitch_reg != 0x7FA)
        pitch_reg = 0x7FA;
    else if (fullness > 4 && pitch_reg != 0x7FD)
        pitch_reg = 0x7FD;
    else if (fullness >= 3 && fullness <= 4 && pitch_reg != 0x7FC)
        pitch_reg = 0x7FC;
    SPU_VREG(0, 4) = (unsigned short)pitch_reg;
    SPU_VREG(1, 4) = (unsigned short)pitch_reg;

    while ((stage_n[0] >= SLOT_SAMPLES) && (stage_n[1] >= SLOT_SAMPLES) &&
           (((writer - reader) & (RING_SLOTS - 1)) >= 2)) {
        for (ch = 0; ch < 2; ch++) {
            encode_slot(ch, 0x03);
            spu_ram_write(slot_start_units(ch, writer),
                          (const unsigned short *)slot_hw[ch], 112);
        }
        writer = (writer + 1) & (RING_SLOTS - 1);
        spu_dbg_slots++;
        stage_n[0] -= SLOT_SAMPLES;
        stage_n[1] -= SLOT_SAMPLES;
        if (stage_n[0] > 0) {
            memmove(stage[0], stage[0] + SLOT_SAMPLES,
                    stage_n[0] * sizeof(short));
            memmove(stage[1], stage[1] + SLOT_SAMPLES,
                    stage_n[1] * sizeof(short));
        }
    }

    /* safety: never let the staging buffer grow unbounded */
    if (stage_n[0] > SLOT_SAMPLES * 2 - 392) {
        stage_n[0] -= SLOT_SAMPLES;
        stage_n[1] -= SLOT_SAMPLES;
        memmove(stage[0], stage[0] + SLOT_SAMPLES,
                stage_n[0] * sizeof(short));
        memmove(stage[1], stage[1] + SLOT_SAMPLES,
                stage_n[1] * sizeof(short));
    }
}

void spu_frame(const short *left, const short *right, int count)
{
    spu_poll();

    /* append this frame's samples to the staging buffers */
    if (count > 0) {
        memcpy(stage[0] + stage_n[0], left,  count * sizeof(short));
        memcpy(stage[1] + stage_n[1], right, count * sizeof(short));
        stage_n[0] += count;
        stage_n[1] += count;
    }

    spu_produce();
}
