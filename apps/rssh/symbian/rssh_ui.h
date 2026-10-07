/*
 * rssh_ui.h: what the C side (PuTTY core, symbian/frontend.c, stubs.c)
 * needs from the C++ UI. Implemented in the src/ C++ files. Strings are
 * UTF-8; terminal text is UTF-16 (wchar_t is 16-bit on this target).
 */
#ifndef RSSH_UI_H
#define RSSH_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Show msg in an error note, then exit the application. */
void rssh_ui_fatal(const char *msg) __attribute__((noreturn));
/* Show msg in an information note; returns when dismissed. */
void rssh_ui_message(const char *msg);
/* Yes/No question, asked later from the top level (never nested inside
 * PuTTY): calls answer(ctx, yes) once the user has replied. */
typedef void (*rssh_ui_answer_fn)(void *ctx, int yes);
void rssh_ui_confirm_async(const char *title, const char *text,
                           rssh_ui_answer_fn answer, void *ctx);

/* Terminal drawing, called between PuTTY's setup/free_draw_ctx. */
enum {
    RSSH_DRAW_BOLD = 1,
    RSSH_DRAW_UNDERLINE = 2,
    RSSH_DRAW_PASSIVE_CURSOR = 4   /* outline box around the cell */
};
/* Draw len cells starting at column x, row y; colours are 0xRRGGBB. */
void rssh_ui_draw(int x, int y, const unsigned short *text, int len,
                  unsigned int fg, unsigned int bg, int flags);
/* End of a batch of rssh_ui_draw calls: schedule a repaint. */
void rssh_ui_flush(void);
void rssh_ui_set_title(const char *title);
/* The connection has ended (msg says why); the UI may offer reconnect. */
void rssh_ui_session_ended(const char *msg);

#ifdef __cplusplus
}
#endif

#endif
