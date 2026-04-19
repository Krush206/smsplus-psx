
#include "osd.h"
#include "uifont.h"

/* Display timing data */
volatile int frame_skip      = 1;
volatile int frame_count     = 0;
volatile int frames_rendered = 0;
volatile int frame_rate      = 0;
volatile int tick_count      = 0;
volatile int old_tick_count  = 0;
volatile int skip            = 0;

/* UI and feature data */
int exit_flag                = 0;
int fg                       = 0;
int bg                       = 0;
volatile int msg_enable      = 0;
FILE *music_log              = NULL;
int music_log_count          = 0;
int state_slot               = 0;
int snap_count               = 0;
char msg[0x100];

/* Blitter data */
BITMAP *sms_bmp = NULL;
PALETTE sms_pal;

/* Game file name */
char game_name[0x100];

/* CRTC parameters for 256x384 and 160x576 displays */
word twktbl[2][0x10] =
{
    { 0x4F00, 0x3F01, 0x4002, 0x9203, 0x4204, 0x1005, 0xAF06, 0x1F07,
      0x4109, 0x8C10, 0x7E11, 0x7F12, 0x8615, 0xA916, 0x0063, 0x0101 },
    { 0x2D00, 0x2701, 0x2802, 0x9003, 0x2B04, 0x8005, 0x5306, 0xF007,
      0x6309, 0x4010, 0xAC11, 0x3F12, 0x4015, 0x4A16, 0x0067, 0x0901 },
};


int main (int argc, char **argv)
{
    /* Show version information if no arguments are specified */
    if(argc < 2)
    {
        printf("\nSMS Plus\n");
        printf("(C) Charles Mac Donald in 1998, 1999, 2000\n");
        printf("Version %s, build date: %s, %s\n", VERSION, __DATE__, __TIME__);
        printf("Usage: sp <filename.ext> [-options]\n");
        printf("Type 'sp -help' for a summary of the available options\n");
        exit(1);
    }

    /* Show option list */
    if(stricmp(argv[1], "-help") == 0)
    {
        printf("Options:\n");
        printf(" -vdriver <s> \t specify video driver.\n");
        printf(" -res <x> <y> \t set the display resolution.\n");
        printf(" -scale       \t scale display to full resolution. (slow)\n");
        printf(" -expand      \t force 512x384 or 400x300 zoomed display.\n");
        printf(" -nommx       \t disable use of MMX instructions.\n");
        printf(" -novga       \t disable use of VGA vertical scaling with '-expand'.\n");
        printf(" -depth <n>   \t specify color depth. (8, 16)\n");
        printf(" -blur        \t blur display. (16-bit color only)\n");
        printf(" -scanlines   \t use scanlines effect.\n");
        printf(" -tweak       \t force 256x192 or 160x144 tweaked display.\n");
        printf(" -vsync       \t wait for vertical sync before blitting.\n");
        printf(" -throttle    \t limit updates to 60 frames per second.\n");
        printf(" -sound       \t enable sound. (auto-throttle)\n");
        printf(" -sndrate <n> \t specify sound rate. (8000, 11025, 22050, 44100)\n");
        printf(" -sndcard <n> \t specify sound card. (0-7)\n");
        printf(" -joy <s>     \t specify joystick type.\n");
        printf(" -jp          \t use Japanese console type.\n");
        printf(" -fm          \t required to enable YM2413 sound.\n");
        exit(1);
    }

    /* Do configuration */
    do_config("sp.cfg");

    /* Make copy of game filename */
    strcpy(game_name, argv[1]);

    /* Attempt to load game off commandline */
    if(load_rom(game_name) == 0)
    {
        printf("Error loading `%s'.\n", game_name);
        exit(1);
    }

    /* Set up video, audio, input, etc. */
    init_machine();

    /* Initialize the virtual console emulation */
    system_init(option.sound ? option.sndrate : 0);
    sms.country = option.country;
    sms.use_fm = option.fm_enable;
    snd.callback = log_handler;

    /* Load SRAM from disk if possible */
    load_sram();

    pick_blitter_proc();

    if(blitter_proc == NULL)
    {
        set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
        printf("Illegal combination of parameters - not all options can be combined.\n");
        exit(1);
    }

    /* Main emulation loop */
    for(;;)
    {
        if(exit_flag) break;

        /* Bump frame count */
        frame_count += 1;
        frames_rendered += 1;
        skip = (frame_count % frame_skip == 0) ? 0 : 1;

        /* Get current input */
        update_input();
        check_ui_keys();

        /* Run the system emulation for a frame */
        sms_frame(skip);

        /* Update the display */
        if(skip == 0)
        {
            update_audio();
            update_video();
        }

        /* Speed throttling */
        if(option.throttle)
        {
            while(tick_count == old_tick_count);
            old_tick_count = tick_count;
        }
    }

    save_sram();
    system_shutdown();
    trash_machine();
    return (0);
}


/****************************************************************************/

/* Handles music log writing */
void log_handler(int data)
{
    if(music_log) fputc(data, music_log);
}

/* Timer handler, runs at 60Hz */
void tick_handler(void)
{
    tick_count += 1;
    if(msg_enable) --msg_enable;
    if(tick_count % FRAMES_PER_SECOND == 0)
    {
        frame_rate = frames_rendered;
        frames_rendered = 0;
    }
}
END_OF_FUNCTION(tick_handler);


/* Initialize machine dependant stuff */
void init_machine(void)
{
    /* Set up sound hardware */
    if(option.sound)
    {
        /* Initialize sound and update sample rate */
        int ret = msdos_init_sound(&option.sndrate, option.sndcard);

        /* Error - disable sound */
        if(ret != 0) option.sound = 0;
    }

    /* Set up the Allegro library */
    allegro_init();

    /* Disable MMX usage if requested */
    if(option.no_mmx == 1) cpu_mmx = 0;

    /* Install input devices */
    install_keyboard();
    install_joystick(option.joy_driver);

    /* Install tick handler */
    install_timer();
    LOCK_FUNCTION(tick_handler);
    LOCK_VARIABLE(tick_count);
    LOCK_VARIABLE(frame_rate);
    LOCK_VARIABLE(frames_rendered);
    LOCK_VARIABLE(msg_enable);
    install_int_ex(tick_handler, BPS_TO_TIMER(FRAMES_PER_SECOND));

    /* Change video settings if screen expansion is enabled */
    if(option.expand)
    {
        option.video_driver = GFX_AUTODETECT;
        option.video_width  = (IS_GG) ? 400 : 512;
        option.video_height = (IS_GG) ? 300 : 384;
    }

    /* Double requested height if scanlines are being used
       and screen expansion is disabled */
    if((option.scanlines == 1) && (option.expand == 0))
    {
        option.video_height *= 2;            
    }

    /* Change video settings if tweaked display is enabled */
    if(option.tweak)
    {
        option.video_driver = GFX_VGA;
        option.video_depth  = 8;
        option.video_width  = 320;
        option.video_height = 200;
    }

    /* Attempt to set graphics mode */
    set_color_depth(option.video_depth);
    if(set_gfx_mode(option.video_driver, option.video_width, option.video_height, 0, 0) != 0)
    {
        printf("Error: %s\n", allegro_error);
        exit(1);
    }

    /* Clear palette and display */
    memcpy(sms_pal, black_palette, sizeof(PALETTE));
    sms_pal[0xFE].r = sms_pal[0xFE].g = sms_pal[0xFE].b = 0xFF;
    set_palette(sms_pal);
    clear(screen);

    /* Use vertical stretching if screen expansion is enabled,
       VGA expansion is disabled, and scanlines are disabled */
    if((option.expand == 1) && (option.no_vga == 0) && (option.scanlines == 0))
    {
        outp(0x3D4, 0x09);
        outp(0x3D5, (inp(0x3D5) & 0xE0) | 0x03);
    }

    /* Force overscan color to be black */
    if(option.video_depth == 8)
    {
        inp(0x3DA);
        outp(0x3C0, 0x31);
        outp(0x3C0, 0xFF);
        clear_to_color(screen, 0xFF);
    }

    /* Tweak display accordingly */
    if(option.tweak)
    {
        int i, j = IS_GG ? 1 : 0;

        /* Disable CRTC write protect */
        outp(0x3D4, 0x11);
        outp(0x3D5, inp(0x3D5) & 0x7F);

        /* Load CRTC parameters */
        for(i = 0; i <= 0x0D; i += 1)
            outpw(0x3D4, twktbl[j][i]);

        /* Enable CRTC write protect */
        outp(0x3D4, 0x11);
        outp(0x3D5, inp(0x3D5) | 0x80);

        /* Set clocking mode register */
        outp(0x3C2, twktbl[j][0x0E]);

        /* Set miscellaneous output register */
        outpw(0x3C4, twktbl[j][0x0F]);
    }

    /* Allocate memory bitmap for blitting */
    sms_bmp = create_bitmap_ex(option.video_depth, 256, 192);
    clear(sms_bmp);

    /* Set up bitmap structure */
    memset(&bitmap, 0, sizeof(t_bitmap));
    bitmap.width  = sms_bmp->w;
    bitmap.height = sms_bmp->h;
    bitmap.depth  = option.video_depth;
    bitmap.pitch  = (bitmap.width * (bitmap.depth >> 3));
    bitmap.data   = (unsigned char *)&sms_bmp->line[0][0];

    /* Set up text printing */
    text_mode(-1);
    font = &zyrinx_font;
    fg = (option.video_depth == 8) ? 0xFE : makecol(0xFF, 0xFF, 0xFF);
    bg = (option.video_depth == 8) ? 0xFF : makecol(0x00, 0x00, 0x00);
}


/* Free system resources */
void trash_machine(void)
{
    if(option.sound) msdos_shutdown_sound();
    set_gfx_mode(GFX_TEXT, 0, 0, 0, 0);
}


/* Debounce keyboard buttons */
int check_key(int code)
{
    static int lastpressed = 0;

    if((!key[code]) && (lastpressed == code))
        lastpressed = 0;

    if((key[code]) && (lastpressed != code))
    {
        lastpressed = code;
        return (1);
    }

    return (0);
}


/* Update virtual console input */
void update_input(void)
{
    /* Clear all button states */
    memset(&input, 0, sizeof(t_input));

    /* System buttons */
    if(key[KEY_ENTER] || key[KEY_SPACE]) input.system |= (IS_GG) ? INPUT_START : INPUT_PAUSE;
    if(check_key(KEY_DEL))  input.system |= INPUT_HARD_RESET;
    if(check_key(KEY_TAB))  input.system |= (IS_GG) ? INPUT_HARD_RESET : INPUT_SOFT_RESET;

    /* Player 1 buttons */
    if(key[KEY_UP])         input.pad[0] |= INPUT_UP;
    else
    if(key[KEY_DOWN])       input.pad[0] |= INPUT_DOWN;
    if(key[KEY_LEFT])       input.pad[0] |= INPUT_LEFT;
    else
    if(key[KEY_RIGHT])      input.pad[0] |= INPUT_RIGHT;
    if(key[KEY_LCONTROL])   input.pad[0] |= INPUT_BUTTON2;
    if(key[KEY_ALT])        input.pad[0] |= INPUT_BUTTON1;

    /* Is the joystick being used ? */
    if(option.joy_driver != JOY_TYPE_NONE)
    {
        poll_joystick();

        /* Check player 1 joystick */
        if(joy[0].stick[0].axis[1].d1) input.pad[0] |= INPUT_UP;
        else
        if(joy[0].stick[0].axis[1].d2) input.pad[0] |= INPUT_DOWN;

        if(joy[0].stick[0].axis[0].d1) input.pad[0] |= INPUT_LEFT;
        else
        if(joy[0].stick[0].axis[0].d2) input.pad[0] |= INPUT_RIGHT;

        if(joy[0].button[0].b)  input.pad[0] |= INPUT_BUTTON2;
        if(joy[0].button[1].b)  input.pad[0] |= INPUT_BUTTON1;
        if(joy[0].button[2].b)  input.system |= (IS_GG) ? INPUT_START : INPUT_PAUSE;

        /* More than one joystick supported ? */
        if(num_joysticks > 2)
        {
            /* Check player 2 joystick */
            if(joy[1].stick[0].axis[1].d1) input.pad[1] |= INPUT_UP;
            else
            if(joy[1].stick[0].axis[1].d2) input.pad[1] |= INPUT_DOWN;

            if(joy[1].stick[0].axis[0].d1) input.pad[1] |= INPUT_LEFT;
            else
            if(joy[1].stick[0].axis[0].d1) input.pad[1] |= INPUT_RIGHT;

            if(joy[1].button[0].b)  input.pad[1] |= INPUT_BUTTON2;
            if(joy[1].button[1].b)  input.pad[1] |= INPUT_BUTTON1;
        }
    }
}


/* Check user interface keys */
void check_ui_keys(void)
{
    /* Frame skip keys */
    if(check_key(KEY_F1)) { frame_skip = 1; add_msg("Frame skip disabled"); };
    if(check_key(KEY_F2)) { frame_skip = 2; add_msg("Frame skip set to 1"); };
    if(check_key(KEY_F3)) { frame_skip = 3; add_msg("Frame skip set to 2"); };
    if(check_key(KEY_F4)) { frame_skip = 4; add_msg("Frame skip set to 3"); };

    /* State save/load keys */
    if(check_key(KEY_F5)) { if(save_state()) add_msg("State saved to slot %d", state_slot); };
    if(check_key(KEY_F6)) { state_slot = (state_slot + 1) % 10; add_msg("Slot %d", state_slot); };
    if(check_key(KEY_F7)) { if(load_state()) add_msg("State loaded from slot %d", state_slot); };

    /* Screen snapshot */
    if(check_key(KEY_F8))
    {
        char name[0x100];
        PALETTE snap_pal;
        BITMAP *snap_bmp = create_bitmap_ex(option.video_depth, BMP_WIDTH, BMP_HEIGHT);
        if(snap_bmp)
        {
            int i;

            /* Get current palette */
            get_palette(snap_pal);

            /* Remove unused palette entries */
            for(i = 0x20; i < 0xFE; i += 1)
                snap_pal[i].r = snap_pal[i].g = snap_pal[i].b = 0x00;
            
            /* Clip image */
            blit(sms_bmp, snap_bmp, BMP_X_OFFSET, BMP_Y_OFFSET, 0, 0, BMP_WIDTH, BMP_HEIGHT);

            /* Generate file name */
            sprintf(name, "snap%04d.pcx", snap_count);

            /* Remove unused bits */
            if(option.video_depth == 8)
            {
                int x, y;
                for(y = 0; y < BMP_HEIGHT; y += 1)
                for(x = 0; x < BMP_WIDTH; x += 1)
                    putpixel(snap_bmp, x, y, getpixel(snap_bmp, x, y) & PIXEL_MASK);
            }

            /* Save snapshot in PCX format */
            save_pcx(name, snap_bmp, snap_pal);

            /* Deallocate temporary bitmap */
            destroy_bitmap(snap_bmp);

            /* Bump snapshot counter */
            snap_count = (snap_count + 1) & 0xFFFF;

            /* Display results */
            add_msg("Saved screen to %s", name);
        }
    }


    /* FPS meter toggle */
    if(check_key(KEY_F9))
    {
        option.fps ^= 1;
        add_msg("FPS meter %s", vdp.limit ? "on" : "off");
    }

    /* Speed throttling */
    if(check_key(KEY_F10))
    {
        if(!option.sound)
        {
            option.throttle ^= 1;
            add_msg("Speed throttling %s", option.throttle ? "on" : "off");
        }
    }

    /* Speed throttling */
    if(check_key(KEY_F11))
    {
        option.vsync ^= 1;
        add_msg("VSync %s", option.vsync ? "on" : "off");
    }

    /* Sprite limit toggle */
    if(check_key(KEY_F12))
    {
        vdp.limit ^= 1;
        add_msg("Sprite limit %s", vdp.limit ? "on" : "off");
    }

    if(check_key(KEY_BACKSPACE))
    {
        if(music_log)
        {
            snd.log = 0;
            fclose(music_log);
            music_log = NULL;
            add_msg("Stopped logging sound.");
        }
        else
        {
            char name[0x100];
            char ext[0x100];
            sprintf(ext, "%02d.ssl", music_log_count);
            strcpy(name, game_name);
            strcpy(strrchr(name, '.'), ext);
            snd.log = 1;
            music_log_count += 1;
            music_log = fopen(name, "wb");
            add_msg("Logging `%s'", name);
        }
    }

    /* Exit keys */
    if(key[KEY_ESC] || key[KEY_END]) exit_flag = 1;
}


/* Update audio */
void update_audio(void)
{
    if(!option.sound) return;
    osd_play_streamed_sample_16(0, &snd.buffer[0][0], snd.bufsize * 2, option.sndrate, 60, -100);
    osd_play_streamed_sample_16(1, &snd.buffer[1][0], snd.bufsize * 2, option.sndrate, 60,  100);
}


/* Update video */
void update_video(void)
{
    /* Wait for VSync */
    if(option.vsync) vsync();

    /* Update the palette */
    if(option.video_depth == 8) update_palette();

    /* Use the blur effect */
    if(option.blur)
    {
        if(IS_GG)
            blur(&sms_bmp->line[24][96], 160, 144, 192);
        else
            blur(&sms_bmp->line[0][0], 256, 192, 0);
    }

    if(option.fps) msg_print(2, 2, "%d", frame_rate);
    if(msg_enable)  msg_print(4, BMP_HEIGHT - 12, "%s", msg);

    blitter_proc(sms_bmp, screen);
}


/* Update VGA palette hardware */
void update_palette(void)
{
    RGB color;
    int index;

    if(bitmap.pal.update == 0) return;

    for(index = 0; index < PALETTE_SIZE; index += 1)
    {
        if(bitmap.pal.dirty[index])
        {
            color.r = (bitmap.pal.color[index][0] >> 2);
            color.g = (bitmap.pal.color[index][1] >> 2);
            color.b = (bitmap.pal.color[index][2] >> 2);

            set_color(0x00 | index, &color);
            set_color(0x20 | index, &color);
            set_color(0x40 | index, &color);
        }
    }

    bitmap.pal.update = 0;
    memset(bitmap.pal.dirty, 0, PALETTE_SIZE);
}


/* Load a game file */
int load_rom(char *filename)
{
    int size;

    if(stricmp(strrchr(filename, '.'), ".zip") == 0)
    {
        char name[0x100];

        cart.rom = loadzip(filename, name, &size);
        if(!cart.rom) return (0);
        printf("Loaded `%s', (%d bytes) from archive `%s'\n", name, size, filename);
        strcpy(game_name, name);

        /* Take care of image header, if present */
        if((size / 512) & 1)
        {
            size -= 512;
            memmove(cart.rom, cart.rom + 512, size);
        }

        cart.pages = (size / 0x4000);
    }
    else
    {
        FILE *fd = NULL;

        fd = fopen(filename, "rb");
        if(!fd) return (0);

        /* Seek to end of file, and get size */
        fseek(fd, 0, SEEK_END);
        size = ftell(fd);
        fseek(fd, 0, SEEK_SET);

        /* Don't load games smaller than 32k */
        if(size < 0x8000) return (0);

        /* Take care of image header, if present */
        if((size / 512) & 1)
        {
            size -= 512;
            fseek(fd, 512, SEEK_SET);
        }

        cart.pages = (size / 0x4000);
        cart.rom = malloc(size);
        if(!cart.rom) return (0);
        fread(cart.rom, size, 1, fd);

        fclose(fd);
    }

    /* Figure out game image type */
    if(stricmp(strrchr(game_name, '.'), ".gg") == 0)
        cart.type = TYPE_GG;
    else
        cart.type = TYPE_SMS;

    return (1);
}

void system_load_sram(void)
{
    load_sram();
}


/* Load SRAM data */
void load_sram(void)
{
    char name[0x100];
    FILE *fd;
    strcpy(name, game_name);
    strcpy(strrchr(name, '.'), ".sav");
    fd = fopen(name, "rb");
    if(fd)
    {
        sms.save = 1;
        fread(sms.sram, 0x8000, 1, fd);
        fclose(fd);
    }
}


/* Save SRAM data */
void save_sram(void)
{
    if(sms.save)
    {
        char name[0x100];
        FILE *fd = NULL;
        strcpy(name, game_name);
        strcpy(strrchr(name, '.'), ".sav");
        fd = fopen(name, "wb");
        if(fd)
        {
            fwrite(sms.sram, 0x8000, 1, fd);
            fclose(fd);
        }
    }
}

/* Load system state */
int load_state(void)
{
    char name[0x100];
    FILE *fd = NULL;
    strcpy(name, game_name);
    sprintf(strrchr(name, '.'), ".st%d", state_slot);
    fd = fopen(name, "rb");
    if(!fd) return (0);
    system_load_state(fd);
    fclose(fd);
    return (1);
}

/* Save system state */
int save_state(void)
{
    char name[0x100];
    FILE *fd = NULL;
    strcpy(name, game_name);
    sprintf(strrchr(name, '.'), ".st%d", state_slot);
    fd = fopen(name, "wb");
    if(!fd) return (0);
    system_save_state(fd);
    fclose(fd);
    return(1);
}

void msg_print(int x, int y, char *fmt, ...)
{
    int i = BMP_X_OFFSET;
    int j = BMP_Y_OFFSET;
    va_list ap;
    char token[0x100];
    char str[0x100];

    strcpy(str, "\0");
    va_start(ap, fmt);
    vsprintf(token, fmt, ap);
    strcat(str, token);
    va_end(ap);

    textprintf(sms_bmp, font, i+x+1, j+y  , bg, "%s", str);
    textprintf(sms_bmp, font, i+x-1, j+y  , bg, "%s", str);
    textprintf(sms_bmp, font, i+x  , j+y+1, bg, "%s", str);
    textprintf(sms_bmp, font, i+x  , j+y-1, bg, "%s", str);
    textprintf(sms_bmp, font, i+x  , j+y  , fg, "%s", str);
}


void add_msg(char *fmt, ...)
{
    va_list ap;
    char token[0x100];

    msg_enable = FRAMES_PER_SECOND;

    strcpy(msg, "\0");
    va_start(ap, fmt);
    vsprintf(token, fmt, ap);
    strcat(msg, token);
    va_end(ap);
}









