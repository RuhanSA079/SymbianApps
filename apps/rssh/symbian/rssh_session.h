/*
 * rssh_session.h: the C session API (symbian/frontend.c) used by the UI.
 * One SSH session at a time.
 */
#ifndef RSSH_SESSION_H
#define RSSH_SESSION_H

#ifdef __cplusplus
extern "C" {
#endif

/* Start connecting; user may be NULL or "" to be prompted. 0 = started. */
int rssh_session_start(const char *host, int port, const char *user,
                       int cols, int rows);
/* Connect using saved profile `name` (a PuTTY saved session). */
int rssh_session_start_profile(const char *name, int cols, int rows);
int rssh_session_active(void);

/* Saved connection profiles (PuTTY saved sessions in the private dir). */
typedef struct rssh_profile {
    char *name, *host, *user;          /* UTF-8 */
    int port;
} rssh_profile;
/* All profiles, in directory order; returns the count (0 if none) and sets
 * *out (free with rssh_profiles_free). Plain data, no callbacks into C++. */
int rssh_profiles_list(rssh_profile **out);
void rssh_profiles_free(rssh_profile *list, int count);
/* Create or update a profile; 0 on success (errors are shown to the user). */
int rssh_profile_save(const char *name, const char *host, int port,
                      const char *user);
void rssh_profile_delete(const char *name);
void rssh_session_resize(int cols, int rows);
/* Typed text, UTF-8. */
void rssh_session_send_text(const char *utf8, int len);
/* Special keys. */
enum {
    RSSH_KEY_UP = 1, RSSH_KEY_DOWN, RSSH_KEY_LEFT, RSSH_KEY_RIGHT,
    RSSH_KEY_HOME, RSSH_KEY_END, RSSH_KEY_PGUP, RSSH_KEY_PGDN,
    RSSH_KEY_INSERT, RSSH_KEY_DELETE,
    RSSH_KEY_ENTER, RSSH_KEY_BACKSPACE, RSSH_KEY_TAB, RSSH_KEY_ESCAPE,
    RSSH_KEY_F1 = 100              /* F1..F12 = RSSH_KEY_F1 + 0..11 */
};
void rssh_session_send_key(int key, int shift, int ctrl, int alt);
/* Repaint everything (e.g. after the view was resized or uncovered). */
void rssh_session_redraw(void);
void rssh_session_close(void);

#ifdef __cplusplus
}
#endif

#endif
