
#ifndef _DOS_H_
#define _DOS_H_

/* Function prototypes */
void tick_handler(void);
void init_machine(void);
void trash_machine(void);
int check_key(int code);
void update_input(void);
void check_ui_keys(void);
void update_audio(void);
void update_video(void);
void update_palette(void);
int load_rom(char *filename);
void system_load_sram(void);
void load_sram(void);
void save_sram(void);
int load_state(void);
int save_state(void);
void msg_print(int x, int y, char *fmt, ...);
void add_msg(char *fmt, ...);
void log_handler(int data);

#endif /* _DOS_H_ */
