/*
 * rsym_rlog.h: remote debug log. Log lines are sent over TCP to a log
 * server on the network (env/rlog-server.py, port 7865 by default), so a
 * phone's log can be followed live on a PC.
 *
 * Off by default. The settings are kept in <dir>/remote-debug.cfg:
 *     remote-debug=1
 *     remote-debug-host=192.168.1.10
 *     remote-debug-port=7865
 *
 * Lines are queued (from any thread) and sent by a background thread, which
 * reconnects every few seconds while the server cannot be reached. Lines
 * logged while it is unreachable wait in a 32 KB queue; when that is full,
 * newer lines are dropped and the server is told how many.
 *
 * Wire format: plain text, one line per log line ('\n'). The first line of
 * each connection is "#rlog1 app=<name>".
 */
#ifndef RSYM_RLOG_H
#define RSYM_RLOG_H
#ifdef __cplusplus
extern "C" {
#endif

#define RSYM_RLOG_DEFAULT_PORT 7865

/* dir: the app's private directory ("C:/Private/<SID>"); name: e.g. "rssh".
 * Reads the settings and, if remote logging is on, starts sending. Call once,
 * from the main thread, before other threads log. */
void rsym_rlog_init(const char *dir, const char *name);

/* Queue one line (no '\n' needed). A no-op while remote logging is off. */
void rsym_rlog_line(const char *line);
int rsym_rlog_enabled(void);

/* Change the settings: saved, and applied at once (reconnects). */
void rsym_rlog_configure(int on, const char *host, int port);
const char *rsym_rlog_host(void);       /* "" if not set */
int rsym_rlog_port(void);

/* For the Settings screen: "off", "connected to 1.2.3.4:7865", ... */
void rsym_rlog_status(char *buf, int buflen);

/* Wait (up to timeout_ms) for queued lines to go out, e.g. before exiting. */
void rsym_rlog_flush(int timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
