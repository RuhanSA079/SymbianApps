/*
 * rssh_trace.h: the optional debug log (Settings -> Debug logging).
 * Off by default; when on, lines go to <private dir>/rssh-debug.log.
 */
#ifndef RSSH_TRACE_H
#define RSSH_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif
/* Call once the private directory is known (before that, nothing is logged). */
void rssh_trace_init(const char *dir);
/* printf-style log line; a no-op while debug logging is off. */
void rssh_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int rssh_debug_enabled(void);
void rssh_debug_set(int on);                 /* remembered across runs */
const char *rssh_debug_log_path(void);       /* "C:/Private/<SID>/rssh-debug.log" */
void rssh_debug_clear(void);
#ifdef __cplusplus
}
#endif
#endif
