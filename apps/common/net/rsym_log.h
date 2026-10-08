/*
 * rsym_log.h: optional debug log shared by the apps (off by default). When
 * on, lines go to <dir>/<name>-debug.log; the switch is <dir>/debug-logging.on.
 * Safe to call from any thread (each line is a separate open/append/close).
 */
#ifndef RSYM_LOG_H
#define RSYM_LOG_H
#ifdef __cplusplus
extern "C" {
#endif
/* dir: the app's private directory ("C:/Private/<SID>"); name: e.g. "rdrive". */
void rsym_log_init(const char *dir, const char *name);
void rsym_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int rsym_log_enabled(void);
void rsym_log_set(int on);
const char *rsym_log_path(void);
void rsym_log_clear(void);
#ifdef __cplusplus
}
#endif
#endif
