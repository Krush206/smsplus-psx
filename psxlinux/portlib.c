/*
 * portlib.c - tiny libc for the PSXLinux port of SMS Plus.
 *
 * The mipsel-linux (egcs) cross compiler has no target libc, and the
 * port is linked -nostdlib, so this file provides everything the
 * emulator core needs: memory (a bump allocator over a static pool),
 * string helpers, a printf that writes to the kernel console (which
 * shows up on screen via the kernel's GPU text console, with a serial
 * fallback), and the double-precision math functions fmopl.c uses to
 * build its YM3812 tables at startup (all software float - the R3000A
 * has no FPU; basic double ops come from libsoftfp.a).
 */
#include <stdarg.h>
#include "port.h"
#include "psxsys.h"

/* ------------------------------------------------------------------ */
/* memory                                                             */
/* ------------------------------------------------------------------ */

/* Bump allocator with LIFO reclamation (same scheme as the psxdev
 * minilibc).  The emulator only allocates at init time (sound buffers,
 * FM chip context, Z80 tables), a few KB in total. */
#define POOL_SIZE   (16 * 1024)
#define POOL_HDR    8

static unsigned char mem_pool[POOL_SIZE];
static size_t mem_used;

void *malloc(size_t size)
{
    unsigned char *base;

    size = (size + 7) & ~(size_t)7;
    if (mem_used + size + POOL_HDR > POOL_SIZE)
        return NULL;
    base = (unsigned char *)mem_pool + mem_used;
    *(size_t *)base = size;
    mem_used += size + POOL_HDR;
    return base + POOL_HDR;
}

void free(void *p)
{
    unsigned char *base;

    if (p == NULL)
        return;
    base = (unsigned char *)p - POOL_HDR;
    /* only the most recent allocation can be reclaimed */
    if (base + POOL_HDR + *(size_t *)base ==
        (unsigned char *)mem_pool + mem_used)
        mem_used = (size_t)(base - (unsigned char *)mem_pool);
}

void *calloc(size_t n, size_t size)
{
    size_t total = n * size;
    void *p = malloc(total);
    if (p != NULL)
        memset(p, 0, total);
    return p;
}

void *memset(void *s, int c, size_t n)
{
    unsigned char *p = (unsigned char *)s;
    while (n--)
        *p++ = (unsigned char)c;
    return s;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--)
        *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;

    if (dp == sp || n == 0)
        return d;
    if (dp < sp) {
        while (n--)
            *dp++ = *sp++;
    } else {
        dp += n;
        sp += n;
        while (n--)
            *--dp = *--sp;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* strings                                                            */
/* ------------------------------------------------------------------ */

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

char *strcpy(char *d, const char *s)
{
    char *r = d;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

char *strncpy(char *d, const char *s, size_t n)
{
    char *r = d;
    while (n && *s) {
        *d++ = *s++;
        n--;
    }
    while (n--)
        *d++ = 0;
    return r;
}

char *strcat(char *d, const char *s)
{
    char *r = d;
    while (*d)
        d++;
    while ((*d++ = *s++) != 0)
        ;
    return r;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    return (c == 0) ? (char *)s : NULL;
}

char *strstr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return (char *)hay;
    while (*hay) {
        if (*hay == *needle && strncmp(hay, needle, n) == 0)
            return (char *)hay;
        hay++;
    }
    return NULL;
}

int atoi(const char *s)
{
    int v = 0;
    int neg = 0;

    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') {
        neg = 1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}

/* ------------------------------------------------------------------ */
/* console output                                                     */
/* ------------------------------------------------------------------ */

/* Console output goes to the raw SIO1 serial port registers.  Each byte
 * written to 0x1F801050 is routed by pcsx's psxHwWrite8() into
 * sioWrite8(), which echoes it to stdout/stderr as `c[c@cycle]` tokens
 * (the kernel's own printk path uses the identical register, so we know
 * the echo works).  /dev/console is not used: devfs never mounts on this
 * system ("unable to mount devfs", "unable to open an initial console"),
 * and sys_open("/dev/console") can return a fd that sys_write() then
 * silently drops, which left the port completely mute. */

#define SIO1_DATA (*(volatile unsigned char *)0x1F801050)
#define SIO1_STAT (*(volatile unsigned short *)0x1F801054)
#define SIO1_CTRL (*(volatile unsigned short *)0x1F80105A)

static void serial_init(void)
{
    static int done;

    if (!done) {
        SIO1_CTRL = 0x0005;     /* RX|TX enable (same as kernel head.S) */
        done = 1;
    }
}

static void serial_putc(int c)
{
    unsigned i;

    if (c == '\n')
        serial_putc('\r');
    /* TX-ready is STAT bit 0 (kernel head.S: andi t3, t3, 0x0001);
     * polling bit 1 was wrong, so nothing was ever transmitted. */
    for (i = 0; i < 100000; i++)
        if (SIO1_STAT & 0x0001)  /* TX ready */
            break;
    SIO1_DATA = (unsigned char)c;
}

void con_putc(int c)
{
    serial_init();
    serial_putc(c);
}

void con_write(const char *s, size_t n)
{
    if (n == 0)
        return;
    serial_init();
    while (n--)
        serial_putc((unsigned char)*s++);
}

/* --- small printf (enough for status lines) --- */

static void fmt_int(char **out, unsigned int v, int base, int width,
                    int zeropad, int neg)
{
    char tmp[12];
    int n = 0;
    int digits;

    do {
        int d = (int)(v % (unsigned int)base);
        v /= (unsigned int)base;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    } while (v);

    digits = n + (neg ? 1 : 0);
    if (neg)
        *(*out)++ = '-';
    while (width-- > digits)
        *(*out)++ = (char)(zeropad ? '0' : ' ');
    while (n > 0)
        *(*out)++ = tmp[--n];
}

static int vsprintf_(char *out, const char *fmt, va_list ap)
{
    char *o = out;

#define ARG_INT()  va_arg(ap, int)
#define ARG_UINT() va_arg(ap, unsigned int)
#define ARG_PTR()  va_arg(ap, char *)

    while (*fmt) {
        int width = 0, zeropad = 0;

        if (*fmt != '%') {
            *o++ = *fmt++;
            continue;
        }
        fmt++;
        if (*fmt == '0') {
            zeropad = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l' || *fmt == 'h')
            fmt++;

        switch (*fmt) {
        case 'd':
        case 'i': {
            int v = ARG_INT();
            fmt_int(&o, (unsigned int)(v < 0 ? -v : v), 10, width,
                    zeropad, v < 0);
            break;
        }
        case 'u':
            fmt_int(&o, ARG_UINT(), 10, width, zeropad, 0);
            break;
        case 'x':
            fmt_int(&o, ARG_UINT(), 16, width, zeropad, 0);
            break;
        case 'c':
            *o++ = (char)ARG_INT();
            break;
        case 's': {
            char *s = ARG_PTR();
            if (s == NULL)
                s = "(null)";
            while (*s)
                *o++ = *s++;
            break;
        }
        case '%':
            *o++ = '%';
            break;
        case 0:
            fmt--;
            break;
        default:
            *o++ = '%';
            *o++ = *fmt;
            break;
        }
        fmt++;
    }
    *o = 0;
    return (int)(o - out);

#undef ARG_INT
#undef ARG_UINT
#undef ARG_PTR
}

static char printf_buf[256];

int printf(const char *fmt, ...)
{
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsprintf_(printf_buf, fmt, ap);
    va_end(ap);
    con_write(printf_buf, strlen(printf_buf));
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    int n;
    va_list ap;

    va_start(ap, fmt);
    n = vsprintf_(buf, fmt, ap);
    va_end(ap);
    return n;
}

void exit(int code)
{
    sys_exit(code);
    for (;;) ;
}

int abs(int x)
{
    return x < 0 ? -x : x;
}

/* fmopl.c uses rand() for the OPL noise (drum) channel only */
static unsigned int rand_seed = 0x1234;

int rand(void)
{
    rand_seed = rand_seed * 1103515245u + 12345u;
    return (int)((rand_seed >> 16) & 0x7FFF);
}

/* pulled in by libgcc's __main; nothing to clean up */
int atexit(void (*func)(void))
{
    (void)func;
    return 0;
}

/* ------------------------------------------------------------------ */
/* math (double precision, soft-float)                                */
/* ------------------------------------------------------------------ */
/*
 * Only fmopl.c uses these, once, while building the YM3812 lookup
 * tables at startup - so straightforward (rather than fast) series
 * evaluations are fine.
 */

static double d_fabs(double x)
{
    return x < 0.0 ? -x : x;
}

double fabs(double x)
{
    return d_fabs(x);
}

double floor(double x)
{
    int i = (int)x;
    if ((double)i > x)
        i--;
    return (double)i;
}

#define LN2   0.69314718055994530942
#define LN10  2.30258509299404568402

/* ln(x) for x > 0: normalise into [1,2), then ln(m) = 2*atanh(z),
 * z = (m-1)/(m+1), |z| <= 1/3 so the series converges fast. */
static double d_ln(double x)
{
    double m, z, z2, sum, zk;
    int k = 0;
    int i;

    if (x <= 0.0)
        return -1.0e30;
    if (x < 1.0) {
        while (x < 1.0) {
            x *= 2.0;
            k--;
        }
    } else {
        while (x >= 2.0) {
            x *= 0.5;
            k++;
        }
    }
    m = x;
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    sum = 0.0;
    zk = z;
    for (i = 0; i < 12; i++) {
        sum += zk / (double)(2 * i + 1);
        zk *= z2;
    }
    return 2.0 * sum + (double)k * LN2;
}

static double d_exp(double x)
{
    double f, e, term;
    int k, i;

    if (x < -1000.0)
        return 0.0;
    if (x > 1000.0)
        return 1.0e300;
    k = (int)(x / LN2 + (x < 0.0 ? -0.5 : 0.5));
    f = x - (double)k * LN2;          /* |f| <= 0.35 */
    e = 1.0;
    term = 1.0;
    for (i = 1; i <= 14; i++) {
        term *= f / (double)i;
        e += term;
    }
    /* scale by 2^k */
    while (k > 0) {
        e *= 2.0;
        k--;
    }
    while (k < 0) {
        e *= 0.5;
        k++;
    }
    return e;
}

double log10(double x)
{
    return d_ln(x) / LN10;
}

double pow(double x, double y)
{
    if (x == 0.0)
        return (y == 0.0) ? 1.0 : 0.0;
    if (x < 0.0) {
        /* not needed by fmopl; avoid the complex case */
        return 0.0;
    }
    return d_exp(y * d_ln(x));
}

#define PI 3.14159265358979323846

double sin(double x)
{
    double x2, term, sum;
    int n, i;

    /* reduce to [-pi/2, pi/2] */
    n = (int)(x * (1.0 / (2.0 * PI)) + (x >= 0.0 ? 0.5 : -0.5));
    x = x - (double)n * 2.0 * PI;
    if (x > PI / 2.0)
        x = PI - x;
    else if (x < -PI / 2.0)
        x = -PI - x;

    x2 = x * x;
    term = x;
    sum = x;
    for (i = 3; i <= 19; i += 2) {
        term *= -x2 / (double)((i - 1) * i);
        sum += term;
    }
    return sum;
}

double cos(double x)
{
    return sin(x + PI / 2.0);
}
