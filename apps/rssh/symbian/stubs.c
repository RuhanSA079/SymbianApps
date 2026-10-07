/*
 * stubs.c: the parts of PuTTY's platform interface that rSSH doesn't
 * support (X11 display, local proxy commands, connection sharing, Unix
 * accounts) plus default settings and fatal-error reporting.
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "putty.h"
#include "ssh.h"
#include "network.h"
#include "rssh_ui.h"
#include "rssh_trace.h"

/* ---- defaults ---- */

char *platform_default_s(const char *name)
{
    if (!strcmp(name, "TermType"))
        return dupstr("xterm-256color");
    return NULL;
}

bool platform_default_b(const char *name, bool def) { return def; }
int platform_default_i(const char *name, int def) { return def; }

FontSpec *platform_default_fontspec(const char *name)
{
    return fontspec_new_default();
}

Filename *platform_default_filename(const char *name)
{
    if (!strcmp(name, "LogFileName"))
        return filename_from_str("rssh.log");
    return filename_from_str("");
}

char *x_get_default(const char *key) { return NULL; }

/* ---- identity ---- */

char *get_username(void) { return NULL; }      /* always prompt */

/* ---- errors ---- */

void modalfatalbox(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    rssh_trace("modalfatalbox: %s", msg);
    rssh_ui_fatal(msg);
}

void nonfatal(const char *fmt, ...)
{
    va_list ap;
    char *msg;
    va_start(ap, fmt);
    msg = dupvprintf(fmt, ap);
    va_end(ap);
    rssh_trace("nonfatal: %s", msg);
    rssh_ui_message(msg);
    sfree(msg);
}

void old_keyfile_warning(void)
{
    rssh_ui_message("This private key is in an old format; "
                    "consider converting it with PuTTYgen.");
}

/* ---- local proxy commands ---- */

Socket *platform_start_subprocess(const char *cmd, Plug *plug,
                                  const char *pfx, SubprocessWaiter **waiter)
{
    if (waiter)
        *waiter = NULL;
    return new_error_socket_fmt(plug, "local proxy commands are not "
                                "supported on Symbian");
}

void subproc_waiter_set_callback(SubprocessWaiter *waiter,
                                 SubprocessWaiterCallback cb, void *cbctx) {}
void subproc_waiter_free(SubprocessWaiter *waiter) {}

/* ---- X11 forwarding: no local X server ---- */

const bool platform_uses_x11_unix_by_default = false;
char *platform_get_x_display(void) { return NULL; }
void platform_get_x11_auth(struct X11Display *display, Conf *conf) {}
SockAddr *platform_get_x11_unix_address(const char *path, int displaynum)
{
    return NULL;
}

/* ---- connection sharing: off ---- */

const bool share_can_be_downstream = false;
const bool share_can_be_upstream = false;
