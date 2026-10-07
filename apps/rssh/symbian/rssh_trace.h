/* rssh_trace.h: printf-style trace lines to C:\data\rssh-trace.txt. */
#ifndef RSSH_TRACE_H
#define RSSH_TRACE_H
#ifdef __cplusplus
extern "C" {
#endif
void rssh_trace(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#ifdef __cplusplus
}
#endif
#endif
