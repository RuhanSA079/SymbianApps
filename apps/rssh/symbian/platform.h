/*
 * symbian/platform.h: rSSH's Symbian^3 platform definitions for PuTTY's
 * portable code (stands in for unix/platform.h / windows/platform.h).
 * C-only declarations; the Symbian side lives in the .cpp files beside it.
 */

#ifndef PUTTY_SYMBIAN_PLATFORM_H
#define PUTTY_SYMBIAN_PLATFORM_H

#include <stdio.h>                     /* for FILENAME_MAX */
#include <stdint.h>
#include <errno.h>
#include <strings.h>
#include <sys/types.h>
#include "charset.h"

#define BUILDINFO_PLATFORM "Symbian^3 (rSSH)"

struct Filename {
    char *path;
};
FILE *f_open(const struct Filename *, char const *, bool);

struct FontSpec {
    char *name;
};
struct FontSpec *fontspec_new(const char *name);

#define BROKEN_PIPE_ERROR_CODE EPIPE   /* used in ssh/sharing.c */

#define MULTICLICK_ONLY_EVENT 0

typedef void *HelpCtx;
#define NULL_HELPCTX ((HelpCtx)NULL)
#define HELPCTX(x) NULL

#define SELECTION_NUL_TERMINATED 0
#define SEL_NL { 13, 10 }

/* Millisecond wraparound timer (User::NTickCount based). */
unsigned long getticks(void);
#define GETTICKCOUNT getticks
#define TICKSPERSEC    1000
#define CURSORBLINK     500

#define WCHAR wchar_t
#define BYTE unsigned char

/* Only the local clipboard for now. */
#define PLATFORM_CLIPBOARDS(X)                  \
    X(CLIP_SYSTEM, "system clipboard")          \
    /* end of list */
#define MOUSE_SELECT_CLIPBOARD CLIP_SYSTEM
#define MOUSE_PASTE_CLIPBOARD CLIP_SYSTEM
#define CLIPNAME_IMPLICIT "Last selected text"
#define CLIPNAME_EXPLICIT "System clipboard"
#define CLIPNAME_EXPLICIT_OBJECT "system clipboard"
#define CLIPUI_DEFAULT_AUTOCOPY true
#define CLIPUI_DEFAULT_MOUSE CLIPUI_EXPLICIT
#define CLIPUI_DEFAULT_INS CLIPUI_EXPLICIT
#define MENU_CLIPBOARD CLIP_SYSTEM
#define COPYALL_CLIPBOARDS CLIP_SYSTEM

#define DEFAULT_CODEPAGE CS_UTF8
#define CP_UTF8 CS_UTF8
#define CP_437 CS_CP437
#define CP_ISO8859_1 CS_ISO8859_1

#define strnicmp strncasecmp
#define stricmp strcasecmp

bool init_ucs(struct unicode_data *ucsdata, char *line_codepage,
              bool utf8_override, int font_charset, int vtmode);

/* No connection sharing, local proxy commands or Unix-domain sockets. */
static inline bool sk_peer_trusted(Socket *sock) { return false; }

/* symbian/network.c: X11 XDM auth data is never available. */
void *sk_getxdmdata(Socket *sock, int *lenp);

/* unix/storage.c keeps sessions and host keys under $HOME/.putty; the app
 * points HOME at its private directory at startup. It also asks X11
 * resources for defaults, which we never have. */
char *x_get_default(const char *key);                     /* always NULL */

/* unix/utils */
char *make_dir_and_check_ours(const char *dirname);
char *make_dir_path(const char *path, mode_t mode);

void plug_closing_errno(Plug *plug, int error);
SeatPromptResult make_spr_sw_abort_errno(const char *prefix, int errno_value);

#define DEFAULT_GTK_FONT "terminal"

/* Directory for saved sessions and host keys (app private dir). */
const char *rssh_data_dir(void);

#endif /* PUTTY_SYMBIAN_PLATFORM_H */
