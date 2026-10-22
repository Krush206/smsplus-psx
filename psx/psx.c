
#include "shared.h"
#include "psx.h"

int frame_count = 0;
/*
 * The original frontend drew a frame only once every 32 emulated
 * frames.  That was necessary because update_video() submitted two GPU
 * commands per SMS pixel.  The renderer below uploads an 8bpp texture
 * and draws one sprite instead, so rendering every frame is practical.
 */
int frame_skip  = 1;
int skip_flag   = 0;
int exit_flag   = 0;
byte base_rom[ROM_SIZE];
byte bitmap_data[256 * 192];
static unsigned char pad1[34],pad2[34];

/* Keep the source image in 8bpp VRAM and use a 128 entry CLUT.  Render
 * pixels carry palette, priority and sprite bits; duplicating each of the
 * 32 palette entries four times makes those extra bits free to display. */
#define TEX_X       512
#define TEX_Y       0
#define CLUT_X      512
#define CLUT_Y      208
#define TEX_TPAGE   (((TEX_X >> 6) & 0x0F) | (((TEX_Y >> 8) & 1) << 4) | (1 << 7))
#define CLUT_ATTR   ((CLUT_Y << 6) | ((CLUT_X >> 4) & 0x3F))

static word clut_data[128];

/*
 * Upload a 16-bit VRAM image.  For an 8bpp texture, w is expressed in
 * halfwords (two source bytes per halfword).  GP0 accepts two halfwords
 * per 32-bit write, which is exactly the native layout of bitmap.data on
 * this little-endian target.
 */
static void upload_image(int x, int y, int w, int h, const void *data)
{
    const dword *src = (const dword *)data;
    int words = ((w * h) + 1) >> 1;

    gp0 = 0xA0000000;
    gp0 = PKT_XY(x, y);
    gp0 = PKT_WH(w, h);

    while(words--)
        gp0 = *src++;
}

static void update_palette(void)
{
    int i;

    for(i = 0; i < 128; i += 1)
    {
        int c = i & PIXEL_MASK;
        int r = bitmap.pal.color[c][0] >> 3;
        int g = bitmap.pal.color[c][1] >> 3;
        int b = bitmap.pal.color[c][2] >> 3;

        /* PSX VRAM colours are BGR555, unlike the core's RGB888 palette. */
        clut_data[i] = (b << 10) | (g << 5) | r;
    }

    upload_image(CLUT_X, CLUT_Y, 128, 1, clut_data);
}


int main (void)
{
    init_machine();
    load_rom();
    system_init(0);

    frame_count = 0;

    /* Main emulation loop */
    for(;;)
    {
        if(exit_flag) break;

        /* Bump frame count */
        frame_count += 1;

        /* Set skip flag based on frameskip settings */
        if(frame_count % frame_skip == 0)
            skip_flag = 0;
        else
            skip_flag = 1;

        /* Get current input */
        update_input();

        /* Run the SMS emulation for a frame */
        sms_frame(skip_flag);

        /* Update the display if desired */
        if(skip_flag == 0) update_video();
    }

    system_shutdown();
    trash_machine();
    return (0);
}


void init_machine(void)
{
    /* Set up video hardware */
    gp1 = 0x00000000; /* GPU reset */
    gp1 = 0x03000000;
    gp1 = 0x06C60260;
    gp1 = 0x07040010;
    gp0 = 0xE1000400; /* 256x240 NTSC, depth=16, dfe=1 */
    gp0 = 0xE3000000;
    gp0 = 0xE407ffff;
    gp0 = 0xE5000000;
    gp1 = 0x08000000;

    /* Clear framebuffer */
    /* Palette entry zero is transparent in the GPU's textured path. */
    fb_rect(0, 0, 0x3FF, 0x1FF, 0, 0, 0);

    /* Initialize display bitmap */
    memset(&bitmap, 0, sizeof(t_bitmap));
    bitmap.width  = 256;
    bitmap.height = 192;
    bitmap.depth  = 8;
    bitmap.pitch  = (bitmap.width * (bitmap.depth >> 3));
    bitmap.data   = (unsigned char *)&bitmap_data[0];

    /* Set up BIOS control pad polling */
    InitPAD(pad1, sizeof(pad1), pad2, sizeof(pad2));
	StartPAD();
}

void trash_machine(void)
{
    StopPAD();
}

void update_video(void)
{
    int w = cart.type ? 160 : 256;
    int h = cart.type ? 144 : 192;
    int dx = cart.type ? 48 : 0;
    int dy = cart.type ? 48 : 24;

    update_palette();

    if(cart.type)
    {
        /* Game Gear's viewport is inset in the core's 256-pixel bitmap.
         * Its rows are not contiguous, so transfer each 160-byte row. */
        int y;
        for(y = 0; y < h; y += 1)
            upload_image(TEX_X, TEX_Y + y, w >> 1, 1,
                         bitmap.data + ((y + 24) * bitmap.width) + 48);
    }
    else
    {
        /* 256 pixels at 8bpp is 128 VRAM halfwords per scanline. */
        upload_image(TEX_X, TEX_Y, w >> 1, h, bitmap.data);
    }

    /* Draw one unmodulated 8bpp textured sprite.  This replaces 49,152
     * individual pixel primitives (98,304 GP0 writes) with four words. */
    gp0 = 0xE1000000 | TEX_TPAGE;
    gp0 = 0x64808080;
    gp0 = PKT_XY(dx, dy);
    gp0 = (CLUT_ATTR << 16);
    gp0 = PKT_WH(w, h);
}


void update_input(void)
{
    /* Pad #1 */
    int pad1_mask = (~(pad1[3] | pad1[2] << 8));

    /* Pad #2 */
    int pad2_mask = (~(pad2[3] | pad2[2] << 8));

    memset(&input, 0, sizeof(t_input));

    if(pad1_mask & (1 <<  8))   exit_flag = 1;

    if(pad1_mask & (1 << 11))   input.system |= (cart.type) ? INPUT_START : INPUT_PAUSE;
    if(pad1_mask & (1 << 12))   input.pad[0] |= INPUT_UP;
    else
    if(pad1_mask & (1 << 14))   input.pad[0] |= INPUT_DOWN;
    if(pad1_mask & (1 << 15))   input.pad[0] |= INPUT_LEFT;
    else
    if(pad1_mask & (1 << 13))   input.pad[0] |= INPUT_RIGHT;
    if(pad1_mask & (1 <<  7))   input.pad[0] |= INPUT_BUTTON2;
    if(pad1_mask & (1 <<  6))   input.pad[0] |= INPUT_BUTTON1;
    if(pad2_mask & (1 << 12))   input.pad[1] |= INPUT_UP;
    else
    if(pad2_mask & (1 << 14))   input.pad[1] |= INPUT_DOWN;
    if(pad2_mask & (1 << 15))   input.pad[1] |= INPUT_LEFT;
    else
    if(pad2_mask & (1 << 13))   input.pad[1] |= INPUT_RIGHT;
    if(pad2_mask & (1 <<  7))   input.pad[1] |= INPUT_BUTTON2;
    if(pad2_mask & (1 <<  6))   input.pad[1] |= INPUT_BUTTON1;
}

void system_load_sram(void)
{
}

/* Load a game image to run */
void load_rom(void)
{
    int x, fd, size;
    char path[0x100];
    char *argv = (char *)0x1F800380;

    /* Try to fix filename */
    for(x=0;x<0x80;x++)
    {
        if((argv[x] >= 'A' || argv[x] >= 'a') && (argv[x] <= 'Z' || argv[x] <= 'z'))
        {

        }
        else
        if((argv[x] >= '0') && (argv[x] <= '9'))
        {
        }
        else
        {
            if( (argv[x]!=':') && (argv[x]!='/') && (argv[x]!='\\') && (argv[x]!='.') )
            {
                argv[x] = '\0';
            }
        }
    }

    memset(path, 0, 0x100);
    strcpy(path, "pcdrv:");
    strcat(path, argv);

    fd = open(path, 1);
    if(!fd) exit(1);
    cart.rom = &base_rom[0];
    size = read(fd, cart.rom, ROM_SIZE);
    close(fd);

    printf("Loaded file `%s', %d bytes.\n", path, size);
    cart.pages = (size / 0x4000);

    cart.type = 0;
    if(strcmp(strrchr(path, '.'), ".gg") == 0)
    {
        cart.type = 1;
    }
}

/****************************************************************************/
/** Helper functions                                                       **/
/****************************************************************************/

/* Draw a pixel to the framebuffer */
void putpixel(int x, int y, int r, int g, int b)
{
    gp0 = PKT_HDR(0x68, r, g, b);
    gp0 = PKT_XY(x, y);
}


/* Clear a rectangluar region of the framebuffer */
void fb_rect(int x, int y, int w, int h, int r, int g, int b)
{
    gp0 = PKT_HDR(0x02, r, g, b);
    gp0 = PKT_XY(x, y);
    gp0 = PKT_WH(w, h);
}


