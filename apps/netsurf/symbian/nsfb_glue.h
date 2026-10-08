/*
 * nsfb_glue.h: the C interface between the Symbian app (nsapp.cpp) and
 * NetSurf's framebuffer frontend.
 *
 * NetSurf runs on the app's main thread, one step at a time from an active
 * object: nsfb_sym_step() handles queued input, runs NetSurf's scheduler and
 * redraws, then says how long until it next needs to run.
 */
#ifndef NSFB_GLUE_H
#define NSFB_GLUE_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- implemented by NetSurf's side (C) ---- */

/* Register the "symbian" libnsfb surface; call before nsfb_sym_start. */
void nsfb_symbian_register(void);

/* frontends/framebuffer/gui.c (patched): main() split in three. */
int nsfb_sym_start(int argc, char **argv);  /* 0 ok, -1 no window */
/* >= 0: run again within that many ms; -1: only after new input;
 * -2: NetSurf has quit (call nsfb_sym_finish). */
int nsfb_sym_step(void);
void nsfb_sym_finish(void);
void nsfb_sym_quit(void);
void nsfb_sym_scroll(int dx, int dy);

/* Input, queued for the next step. Coordinates are surface pixels. */
#define NSFB_SYM_UCS4 0x100000      /* = NSFB_SYMBIAN_UCS4_KEY */
void nsfb_sym_key(int down, int nsfb_keycode);
void nsfb_sym_pointer_move(int x, int y);
void nsfb_sym_pointer_button(int down);
void nsfb_sym_resize(int width, int height);
int nsfb_symbian_input_pending(void);

/* Browser actions on the (only) window, for the app's menu (nsfb_glue.c). */
int nsfb_sym_in_content(int x, int y);  /* (x,y) inside the page area? */
void nsfb_sym_open_url(const char *url);    /* UTF-8; adds http:// if needed */
void nsfb_sym_back(void);
void nsfb_sym_forward(void);
int nsfb_sym_can_back(void);
void nsfb_sym_reload(void);
void nsfb_sym_stop(void);
void nsfb_sym_home(void);
/* Current URL (UTF-8) into buf; returns buf ("" if none). */
char *nsfb_sym_current_url(char *buf, int buflen);

/* Settings (nsfb_glue.c). The user's choices live in their own file, read
 * over res/Choices at start: set its path before nsfb_sym_start. */
void nsfb_sym_set_user_choices(const char *path);
const char *nsfb_sym_user_choices(void);
enum {
	NSFB_SYM_OPT_SCALE,         /* page zoom, percent (applies at once) */
	NSFB_SYM_OPT_FONT_SIZE,     /* default text size, 0.1 pt (on reload) */
	NSFB_SYM_OPT_IMAGES,        /* load images: 0/1 (on reload) */
	NSFB_SYM_OPT_BLOCK_ADS,     /* 0/1 */
	NSFB_SYM_OPT_DNT            /* send Do Not Track: 0/1 */
};
int nsfb_sym_option(int opt);
void nsfb_sym_set_option(int opt, int value);
char *nsfb_sym_homepage(char *buf, int buflen);     /* "" if none */
void nsfb_sym_set_homepage(const char *url);        /* "" = NetSurf's own */
int nsfb_sym_save_options(void);                    /* 0 ok */

/* ---- implemented by the app (C++) ---- */

/* Show the given area of the surface (32 bpp 0x00RRGGBB pixels). */
void nsfb_symbian_update(const unsigned int *pixels, int stride_bytes,
                         int width, int height,
                         int x0, int y0, int x1, int y1);

#ifdef __cplusplus
}
#endif

#endif
