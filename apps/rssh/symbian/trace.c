/*
 * trace.c: rSSH's debug log (Settings -> Debug logging), off by default.
 * The switch is the presence of <private dir>/debug-logging.on; the log is
 * <private dir>/rssh-debug.log, appended across runs and trimmed when it
 * grows past KMaxLog. Each line is opened, written and closed, so the log
 * survives a crash and shows the last step reached.
 *
 * With the remote debug log on (Settings; apps/common/net/rsym_rlog.h),
 * every line also goes to the log server, whether or not the file log is on.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "rssh_trace.h"
#include "rsym_rlog.h"

#define KMaxLog (512 * 1024)

static char log_path[160];
static char flag_path[160];
static int ready = 0, enabled = 0;

void rssh_trace_init(const char *dir)
{
    struct stat st;
    snprintf(log_path, sizeof(log_path), "%s/rssh-debug.log", dir);
    snprintf(flag_path, sizeof(flag_path), "%s/debug-logging.on", dir);
    enabled = stat(flag_path, &st) == 0;
    ready = 1;
    if (enabled && stat(log_path, &st) == 0 && st.st_size > KMaxLog)
        remove(log_path);
    rsym_rlog_init(dir, "rssh");
    rssh_trace("---- rSSH started");
}

void rssh_trace(const char *fmt, ...)
{
    char line[512];
    va_list ap;

    if (!ready || (!enabled && !rsym_rlog_enabled()))
        return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (enabled) {
        FILE *fp = fopen(log_path, "a");
        if (fp) {
            fputs(line, fp);
            fputc('\n', fp);
            fclose(fp);
        }
    }
    rsym_rlog_line(line);
}

int rssh_debug_enabled(void) { return enabled; }

void rssh_debug_set(int on)
{
    if (on) {
        FILE *fp = fopen(flag_path, "w");
        if (fp)
            fclose(fp);
        enabled = 1;
        rssh_trace("---- debug logging enabled");
    } else {
        rssh_trace("---- debug logging disabled");
        remove(flag_path);
        enabled = 0;
    }
}

const char *rssh_debug_log_path(void) { return log_path; }

void rssh_debug_clear(void) { remove(log_path); }

/*
 * PuTTY relies on assert() (it refuses to build with NDEBUG). P.I.P.S.'s
 * __assert() just aborts, which takes the whole EKA2L1 emulator down with no
 * clue why; this one (linked in ahead of libc's) records the failure first.
 * Not including <assert.h> here avoids its IMPORT_C declaration.
 */
void rssh_ui_fatal(const char *msg) __attribute__((noreturn));

void __assert(const char *func, const char *file, int line, const char *expr)
{
    char msg[256];
    rssh_trace("ASSERTION FAILED: %s in %s() at %s:%d", expr, func, file, line);
    snprintf(msg, sizeof(msg), "Internal error (assertion failed):\n%s\n%s:%d",
             expr, file, line);
    rssh_ui_fatal(msg);
}
