
#include "shared.h"
#include "psx.h"

int frame_count = 0;
int frame_skip  = 32;
int skip_flag   = 0;
int exit_flag   = 0;
byte base_rom[ROM_SIZE];
byte bitmap_data[256 * 192];
static unsigned char pad1[34],pad2[34];


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
    fb_rect(0, 0, 0x3FF, 0x1FF, 0x1F, 0x1F, 0x1F);

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
    int x, y, c, r, g, b;

    /* Width and height of viewport */
    int w = (cart.type ? 160 : 256);
    int h = (cart.type ? 144 : 192);

    /* Offset in framebuffer */
    int dx = (cart.type ? 48 : 0);
    int dy = (cart.type ? 48 : 24);

    /* Offset in display bitmap */
    int sx = (cart.type ? 48 : 0);
    int sy = (cart.type ? 24 : 0);

    for(y = 0; y < h; y += 1)
    {
        /* Point to current line of bitmap */
        char *p = &bitmap.data[((sy + y) * bitmap.width) + (sx)];

        for(x = 0; x < w; x += 1)
        {
            /* Strip priority and sprite marker bits */
            c = (p[x] & PIXEL_MASK);

            /* Look-up color components for given pixel */
            r = bitmap.pal.color[c][0];
            g = bitmap.pal.color[c][1];
            b = bitmap.pal.color[c][2];

            /* Write to framebuffer */
            putpixel(dx + x, dy + y, r, g, b);
        }
    }
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


