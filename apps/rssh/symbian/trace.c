/*
 * trace.c: experimental-build tracing to C:\data\rssh-trace.txt. Every line
 * is opened, written and closed immediately, so the file survives a crash
 * and shows the last step reached.
 */
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include "rssh_trace.h"

void rssh_trace(const char *fmt, ...)
{
    static int started = 0;
    FILE *fp;
    va_list ap;

    if (!started) {
        mkdir("C:/data", 0777);
        fp = fopen("C:/data/rssh-trace.txt", "w");    /* new file per run */
        started = 1;
    } else {
        fp = fopen("C:/data/rssh-trace.txt", "a");
    }
    if (!fp)
        return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

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
