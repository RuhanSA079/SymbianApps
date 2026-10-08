/*
 * rsym_log.c: see rsym_log.h. The log is appended across runs and trimmed
 * when it grows past MAX_LOG at start-up.
 */
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include "rsym_log.h"

#define MAX_LOG (512 * 1024)

static char log_path[160];
static char flag_path[160];
static int ready = 0, enabled = 0;

void rsym_log_init(const char *dir, const char *name)
{
    struct stat st;
    snprintf(log_path, sizeof(log_path), "%s/%s-debug.log", dir, name);
    snprintf(flag_path, sizeof(flag_path), "%s/debug-logging.on", dir);
    enabled = stat(flag_path, &st) == 0;
    ready = 1;
    if (enabled && stat(log_path, &st) == 0 && st.st_size > MAX_LOG)
        remove(log_path);
    rsym_log("---- %s started", name);
}

void rsym_log(const char *fmt, ...)
{
    FILE *fp;
    va_list ap;
    if (!ready || !enabled)
        return;
    fp = fopen(log_path, "a");
    if (!fp)
        return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fputc('\n', fp);
    fclose(fp);
}

int rsym_log_enabled(void) { return enabled; }

void rsym_log_set(int on)
{
    if (on) {
        FILE *fp = fopen(flag_path, "w");
        if (fp)
            fclose(fp);
        enabled = 1;
        rsym_log("---- debug logging enabled");
    } else {
        rsym_log("---- debug logging disabled");
        remove(flag_path);
        enabled = 0;
    }
}

const char *rsym_log_path(void) { return log_path; }

void rsym_log_clear(void) { remove(log_path); }
