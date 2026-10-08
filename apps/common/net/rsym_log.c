/*
 * rsym_log.c: see rsym_log.h. The log is appended across runs and trimmed
 * when it grows past MAX_LOG at start-up. With the remote debug log on
 * (rsym_rlog.h), every line also goes to the log server, whether or not the
 * file log is on.
 */
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include "rsym_log.h"
#include "rsym_rlog.h"

#define MAX_LOG (512 * 1024)

static char log_path[160];
static char flag_path[160];
static int ready = 0, enabled = 0;
static struct timeval t0;

void rsym_log_init(const char *dir, const char *name)
{
    struct stat st;
    snprintf(log_path, sizeof(log_path), "%s/%s-debug.log", dir, name);
    snprintf(flag_path, sizeof(flag_path), "%s/debug-logging.on", dir);
    enabled = stat(flag_path, &st) == 0;
    ready = 1;
    gettimeofday(&t0, NULL);
    if (enabled && stat(log_path, &st) == 0 && st.st_size > MAX_LOG)
        remove(log_path);
    rsym_rlog_init(dir, name);
    rsym_log("---- %s started", name);
}

void rsym_log(const char *fmt, ...)
{
    char line[512];
    int n;
    va_list ap;
    if (!ready || (!enabled && !rsym_rlog_enabled()))
        return;
    {
        /* seconds since rsym_log_init, to the millisecond */
        struct timeval now;
        long ms;
        gettimeofday(&now, NULL);
        ms = (now.tv_sec - t0.tv_sec) * 1000L + (now.tv_usec - t0.tv_usec) / 1000L;
        n = snprintf(line, sizeof(line), "[%ld.%03ld] ", ms / 1000, ms % 1000);
    }
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(line) - n, fmt, ap);
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
