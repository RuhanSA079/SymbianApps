/*
 * frontend.c: rSSH's PuTTY front end. Owns the one session (Conf,
 * Terminal, Ldisc, SSH backend) and implements PuTTY's Seat, TermWin and
 * LogPolicy on top of the small C API in rssh_ui.h, which the C++ UI
 * provides. Kept in C so it can use putty.h directly.
 */

#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "putty.h"
#include "terminal.h"
#include "ssh.h"
#include "storage.h"
#include "rssh_ui.h"
#include "rssh_session.h"
#include "rssh_trace.h"

typedef struct RsshSession {
    Conf *conf;
    Terminal *term;
    Backend *backend;
    Ldisc *ldisc;
    LogContext *logctx;
    struct unicode_data ucsdata;
    rgb palette[OSC4_NCOLOURS];
    bool exited;
    struct PendingQuestion *question;   /* host key / weak crypto prompt */

    Seat seat;
    TermWin termwin;
    LogPolicy logpolicy;
} RsshSession;

static RsshSession *sess;

/* ------------------------------------------------------------------ */
/* drawing (TermWin)                                                    */
/* ------------------------------------------------------------------ */

static unsigned int rgb_to_uint(rgb c)
{
    return ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b;
}

static unsigned int optrgb_to_uint(optionalrgb c)
{
    return ((unsigned)c.r << 16) | ((unsigned)c.g << 8) | c.b;
}

enum { CURSOR_NONE, CURSOR_ACTIVE, CURSOR_PASSIVE };

static void draw_cells(RsshSession *s, int x, int y, wchar_t *text, int len,
                       unsigned long attr, truecolour tc, int cursor)
{
    int nfg = (attr & ATTR_FGMASK) >> ATTR_FGSHIFT;
    int nbg = (attr & ATTR_BGMASK) >> ATTR_BGSHIFT;
    unsigned int fg, bg;
    int flags = 0, i;
    unsigned short buf[256];

    if (attr & ATTR_REVERSE) {
        int t = nfg; nfg = nbg; nbg = t;
        optionalrgb trgb = tc.fg; tc.fg = tc.bg; tc.bg = trgb;
    }
    if ((attr & ATTR_BOLD) && conf_get_int(s->conf, CONF_bold_style) & 2) {
        if (nfg < 16) nfg |= 8;
        else if (nfg >= 256) nfg |= 1;
    }
    if (attr & ATTR_BLINK) {
        if (nbg < 16) nbg |= 8;
        else if (nbg >= 256) nbg |= 1;
    }
    fg = tc.fg.enabled ? optrgb_to_uint(tc.fg) : rgb_to_uint(s->palette[nfg]);
    bg = tc.bg.enabled ? optrgb_to_uint(tc.bg) : rgb_to_uint(s->palette[nbg]);

    if (cursor == CURSOR_ACTIVE) {
        fg = rgb_to_uint(s->palette[OSC4_COLOUR_cursor_fg]);
        bg = rgb_to_uint(s->palette[OSC4_COLOUR_cursor_bg]);
    } else if (cursor == CURSOR_PASSIVE) {
        flags |= RSSH_DRAW_PASSIVE_CURSOR;
    }
    if ((attr & ATTR_BOLD) && conf_get_int(s->conf, CONF_bold_style) & 1)
        flags |= RSSH_DRAW_BOLD;
    if (attr & ATTR_UNDER)
        flags |= RSSH_DRAW_UNDERLINE;

    if (len > (int)lenof(buf))
        len = lenof(buf);
    for (i = 0; i < len; i++) {
        unsigned long c = (unsigned short)text[i];
        if (DIRECT_CHAR(c) || DIRECT_FONT(c))
            c &= 0xFF;                 /* font-codepage chars: best effort */
        buf[i] = (unsigned short)c;
    }
    rssh_ui_draw(x, y, buf, len, fg, bg, flags);
}

static bool tw_setup_draw_ctx(TermWin *tw) { return true; }

static void tw_draw_text(TermWin *tw, int x, int y, wchar_t *text, int len,
                         unsigned long attrs, int lattrs, truecolour tc)
{
    RsshSession *s = container_of(tw, RsshSession, termwin);
    draw_cells(s, x, y, text, len, attrs, tc, CURSOR_NONE);
}

static void tw_draw_cursor(TermWin *tw, int x, int y, wchar_t *text, int len,
                           unsigned long attrs, int lattrs, truecolour tc)
{
    RsshSession *s = container_of(tw, RsshSession, termwin);
    int cursor = CURSOR_NONE;
    if (attrs & ATTR_ACTCURS)
        cursor = CURSOR_ACTIVE;
    else if (attrs & ATTR_PASCURS)
        cursor = CURSOR_PASSIVE;
    draw_cells(s, x, y, text, len, attrs, tc, cursor);
}

static void tw_draw_trust_sigil(TermWin *tw, int x, int y)
{
    static const unsigned short sigil[2] = { '*', ' ' };
    RsshSession *s = container_of(tw, RsshSession, termwin);
    rssh_ui_draw(x, y, sigil, 2, rgb_to_uint(s->palette[OSC4_COLOUR_fg]),
                 rgb_to_uint(s->palette[OSC4_COLOUR_bg]), RSSH_DRAW_BOLD);
}

static int tw_char_width(TermWin *tw, int uc) { return 1; }
static void tw_free_draw_ctx(TermWin *tw) { rssh_ui_flush(); }
static void tw_set_cursor_pos(TermWin *tw, int x, int y) {}
static void tw_set_raw_mouse_mode(TermWin *tw, bool enable) {}
static void tw_set_raw_mouse_mode_pointer(TermWin *tw, bool enable) {}
static void tw_set_scrollbar(TermWin *tw, int total, int start, int page) {}
static void tw_bell(TermWin *tw, int mode) {}
static void tw_clip_write(TermWin *tw, int clipboard, wchar_t *text,
                          int *attrs, truecolour *colours, int len,
                          bool must_deselect) {}
static void tw_clip_request_paste(TermWin *tw, int clipboard) {}
static void tw_refresh(TermWin *tw) { rssh_session_redraw(); }
static void tw_request_resize(TermWin *tw, int w, int h) {}

static void tw_set_title(TermWin *tw, const char *title, int codepage)
{
    rssh_ui_set_title(title);
}

static void tw_set_icon_title(TermWin *tw, const char *t, int codepage) {}
static void tw_set_minimised(TermWin *tw, bool minimised) {}
static void tw_set_maximised(TermWin *tw, bool maximised) {}
static void tw_move(TermWin *tw, int x, int y) {}
static void tw_set_zorder(TermWin *tw, bool top) {}

static void tw_palette_set(TermWin *tw, unsigned start, unsigned ncolours,
                           const rgb *colours)
{
    RsshSession *s = container_of(tw, RsshSession, termwin);
    unsigned i;
    for (i = 0; i < ncolours && start + i < OSC4_NCOLOURS; i++)
        s->palette[start + i] = colours[i];
}

static void tw_palette_get_overrides(TermWin *tw, Terminal *term) {}

static void tw_unthrottle(TermWin *tw, size_t bufsize)
{
    RsshSession *s = container_of(tw, RsshSession, termwin);
    if (s->backend)
        backend_unthrottle(s->backend, bufsize);
}

static const TermWinVtable rssh_termwin_vt = {
    .setup_draw_ctx = tw_setup_draw_ctx,
    .draw_text = tw_draw_text,
    .draw_cursor = tw_draw_cursor,
    .draw_trust_sigil = tw_draw_trust_sigil,
    .char_width = tw_char_width,
    .free_draw_ctx = tw_free_draw_ctx,
    .set_cursor_pos = tw_set_cursor_pos,
    .set_raw_mouse_mode = tw_set_raw_mouse_mode,
    .set_raw_mouse_mode_pointer = tw_set_raw_mouse_mode_pointer,
    .set_scrollbar = tw_set_scrollbar,
    .bell = tw_bell,
    .clip_write = tw_clip_write,
    .clip_request_paste = tw_clip_request_paste,
    .refresh = tw_refresh,
    .request_resize = tw_request_resize,
    .set_title = tw_set_title,
    .set_icon_title = tw_set_icon_title,
    .set_minimised = tw_set_minimised,
    .set_maximised = tw_set_maximised,
    .move = tw_move,
    .set_zorder = tw_set_zorder,
    .palette_set = tw_palette_set,
    .palette_get_overrides = tw_palette_get_overrides,
    .unthrottle = tw_unthrottle,
};

/* ------------------------------------------------------------------ */
/* Seat                                                                 */
/* ------------------------------------------------------------------ */

static size_t rseat_output(Seat *seat, SeatOutputType type,
                          const void *data, size_t len)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    return term_data(s->term, data, len);
}

static bool rseat_eof(Seat *seat) { return true; }   /* close our side too */

static SeatPromptResult rseat_get_userpass_input(Seat *seat, prompts_t *p)
{
    /* Prompt inside the terminal window, as GUI PuTTY does. */
    RsshSession *s = container_of(seat, RsshSession, seat);
    SeatPromptResult spr = term_get_userpass_input(s->term, p);
    rssh_trace("userpass: %d prompt(s), first echo=%d -> kind %d",
               (int)p->n_prompts, p->n_prompts ? (int)p->prompts[0]->echo : -1,
               (int)spr.kind);
    return spr;
}

static void free_connection(void *vctx);

static void rseat_notify_remote_exit(Seat *seat)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    if (s->backend && !s->exited && backend_exitcode(s->backend) >= 0) {
        s->exited = true;
        queue_toplevel_callback(free_connection, s);
        rssh_ui_session_ended("Connection closed.");
    }
}

static void rseat_connection_fatal(Seat *seat, const char *message)
{
    rssh_trace("connection_fatal: %s", message);
    RsshSession *s = container_of(seat, RsshSession, seat);
    if (s->exited)
        return;
    s->exited = true;
    queue_toplevel_callback(free_connection, s);
    rssh_ui_session_ended(message);
}

static void rseat_nonfatal(Seat *seat, const char *message)
{
    rssh_ui_message(message);
}

static char *rseat_get_ttymode(Seat *seat, const char *mode)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    return term_get_ttymode(s->term, mode);
}

/*
 * Host-key and weak-crypto questions are answered asynchronously: we return
 * SPR_INCOMPLETE, the UI asks from the top level of the event loop (not
 * from deep inside PuTTY's SSH code), and the answer goes to PuTTY's
 * callback. If the session is closed meanwhile, the answer is dropped.
 */
typedef struct PendingQuestion {
    RsshSession *s;                 /* NULL once the session has gone */
    void (*callback)(void *ctx, SeatPromptResult result);
    void *ctx;
    bool store_key;
    char *host, *keytype, *keystr;
    int port;
} PendingQuestion;

static void question_answered(void *vq, int yes)
{
    PendingQuestion *q = (PendingQuestion *)vq;
    rssh_trace("question answered: %d", yes);
    if (q->s) {
        q->s->question = NULL;
        if (yes && q->store_key)
            store_host_key(&q->s->seat, q->host, q->port, q->keytype, q->keystr);
        q->callback(q->ctx, yes ? SPR_OK : SPR_USER_ABORT);
    }
    sfree(q->host);
    sfree(q->keytype);
    sfree(q->keystr);
    sfree(q);
}

/*
 * A short prompt for a small confirmation query: the scary heading if there
 * is one, the DISPLAY items (host:port, fingerprint) and a question.
 */
static char *short_question(SeatDialogText *text, const char *question)
{
    strbuf *sb = strbuf_new();
    SeatDialogTextItem *item, *end = text->items + text->nitems;
    const char *first_para = NULL;
    bool any_display = false;
    for (item = text->items; item < end; item++) {
        if (item->type == SDT_SCARY_HEADING)
            put_fmt(sb, "%s\n", item->text);
        else if (item->type == SDT_PARA && !first_para)
            first_para = item->text;
    }
    for (item = text->items; item < end; item++) {
        if (item->type == SDT_DISPLAY) {
            put_fmt(sb, "%s\n", item->text);
            any_display = true;
        }
    }
    if (!any_display && first_para)
        put_fmt(sb, "%s\n", first_para);
    put_dataz(sb, question);
    return strbuf_to_str(sb);
}

static SeatPromptResult ask_question(RsshSession *s, SeatDialogText *text,
                                     void (*callback)(void *, SeatPromptResult),
                                     void *ctx, PendingQuestion **out)
{
    const char *title = "rSSH";
    char *msg = short_question(text, out ? "Trust this host?" : "Continue?");
    SeatDialogTextItem *item;
    for (item = text->items; item < text->items + text->nitems; item++)
        if (item->type == SDT_TITLE)
            title = item->text;
    PendingQuestion *q = snew(PendingQuestion);
    memset(q, 0, sizeof(*q));
    q->s = s;
    q->callback = callback;
    q->ctx = ctx;
    s->question = q;
    if (out)
        *out = q;
    rssh_ui_confirm_async(title, msg, question_answered, q);
    sfree(msg);
    return SPR_INCOMPLETE;
}

static SeatPromptResult rseat_confirm_ssh_host_key(
    Seat *seat, const char *host, int port, const char *keytype,
    char *keystr, SeatDialogText *text, HelpCtx helpctx,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    PendingQuestion *q;
    SeatPromptResult spr;
    rssh_trace("confirm host key %s:%d %s (async)", host, port, keytype);
    spr = ask_question(s, text, callback, ctx, &q);
    q->store_key = true;
    q->host = dupstr(host);
    q->port = port;
    q->keytype = dupstr(keytype);
    q->keystr = dupstr(keystr);
    return spr;
}

static SeatPromptResult rseat_confirm_weak(
    Seat *seat, SeatDialogText *text,
    void (*callback)(void *ctx, SeatPromptResult result), void *ctx)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    return ask_question(s, text, callback, ctx, NULL);
}

static bool rseat_is_utf8(Seat *seat)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    return s->ucsdata.line_codepage == CS_UTF8;
}

static StripCtrlChars *rseat_stripctrl_new(
    Seat *seat, BinarySink *bs_out, SeatInteractionContext sic)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    return stripctrl_new_term(bs_out, false, 0, s->term);
}

static void rseat_set_trust_status(Seat *seat, bool trusted)
{
    RsshSession *s = container_of(seat, RsshSession, seat);
    term_set_trust_status(s->term, trusted);
}

static const SeatVtable rssh_seat_vt = {
    .output = rseat_output,
    .eof = rseat_eof,
    .sent = nullseat_sent,
    .banner = nullseat_banner_to_stderr,
    .get_userpass_input = rseat_get_userpass_input,
    .notify_session_started = nullseat_notify_session_started,
    .notify_remote_exit = rseat_notify_remote_exit,
    .notify_remote_disconnect = nullseat_notify_remote_disconnect,
    .connection_fatal = rseat_connection_fatal,
    .nonfatal = rseat_nonfatal,
    .update_specials_menu = nullseat_update_specials_menu,
    .get_ttymode = rseat_get_ttymode,
    .set_busy_status = nullseat_set_busy_status,
    .confirm_ssh_host_key = rseat_confirm_ssh_host_key,
    .confirm_weak_crypto_primitive = rseat_confirm_weak,
    .confirm_weak_cached_hostkey = rseat_confirm_weak,
    .prompt_descriptions = nullseat_prompt_descriptions,
    .is_utf8 = rseat_is_utf8,
    .echoedit_update = nullseat_echoedit_update,
    .get_display = nullseat_get_display,
    .get_windowid = nullseat_get_windowid,
    .get_window_pixel_size = nullseat_get_window_pixel_size,
    .stripctrl_new = rseat_stripctrl_new,
    .set_trust_status = rseat_set_trust_status,
    .can_set_trust_status = nullseat_can_set_trust_status_yes,
    .has_mixed_input_stream = nullseat_has_mixed_input_stream_yes,
    .verbose = nullseat_verbose_no,
    .interactive = nullseat_interactive_yes,
    .get_cursor_position = nullseat_get_cursor_position,
};

/* ------------------------------------------------------------------ */
/* LogPolicy: no event log window, no session logging                   */
/* ------------------------------------------------------------------ */

static void rlp_eventlog(LogPolicy *lp, const char *event) {}
static int rlp_askappend(LogPolicy *lp, Filename *filename,
                        void (*callback)(void *ctx, int result), void *ctx)
{
    return 2;                         /* overwrite */
}
static void rlp_logging_error(LogPolicy *lp, const char *event) {}

static const LogPolicyVtable rssh_logpolicy_vt = {
    .eventlog = rlp_eventlog,
    .askappend = rlp_askappend,
    .logging_error = rlp_logging_error,
    .verbose = null_lp_verbose_no,
};

/* ------------------------------------------------------------------ */
/* session lifetime                                                     */
/* ------------------------------------------------------------------ */

static void free_connection(void *vctx)
{
    RsshSession *s = (RsshSession *)vctx;
    if (s->ldisc) {
        ldisc_free(s->ldisc);
        s->ldisc = NULL;
    }
    if (s->backend) {
        backend_free(s->backend);
        s->backend = NULL;
        term_provide_backend(s->term, NULL);
    }
}

static int session_start_conf(Conf *conf, int cols, int rows);

int rssh_session_start(const char *host, int port, const char *user,
                       int cols, int rows)
{
    Conf *conf = conf_new();
    rssh_trace("session_start host=%s port=%d user=%s cols=%d rows=%d", host, port, user ? user : "", cols, rows);
    do_defaults(NULL, conf);
    rssh_trace("do_defaults ok");
    conf_set_str(conf, CONF_host, host);
    conf_set_int(conf, CONF_port, port > 0 ? port : 22);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    if (user && *user)
        conf_set_str(conf, CONF_username, user);
    return session_start_conf(conf, cols, rows);
}

int rssh_session_start_profile(const char *name, int cols, int rows)
{
    Conf *conf = conf_new();
    rssh_trace("session_start_profile");
    do_defaults(name, conf);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    return session_start_conf(conf, cols, rows);
}

/* Takes ownership of conf. */
static int session_start_conf(Conf *conf, int cols, int rows)
{
    RsshSession *s;
    const struct BackendVtable *vt;
    char *error, *realhost = NULL;
    const char *host;

    if (sess)
        rssh_session_close();

    s = snew(RsshSession);
    memset(s, 0, sizeof(*s));
    s->seat.vt = &rssh_seat_vt;
    s->termwin.vt = &rssh_termwin_vt;
    s->logpolicy.vt = &rssh_logpolicy_vt;
    s->conf = conf;
    host = conf_get_str(conf, CONF_host);
    conf_set_str(s->conf, CONF_line_codepage, "UTF-8");

    init_ucs(&s->ucsdata, conf_get_str(s->conf, CONF_line_codepage),
             false, CS_UTF8, conf_get_int(s->conf, CONF_vtmode));

    rssh_trace("init_ucs ok");
    s->term = term_init(s->conf, &s->ucsdata, &s->termwin);
    rssh_trace("term_init ok");
    s->logctx = log_init(&s->logpolicy, s->conf);
    term_provide_logctx(s->term, s->logctx);
    term_size(s->term, rows, cols, conf_get_int(s->conf, CONF_savelines));
    rssh_trace("term_size ok");
    sess = s;

    vt = backend_vt_from_proto(PROT_SSH);
    rssh_trace("backend_init...");
    error = backend_init(vt, &s->seat, &s->backend, s->logctx, s->conf,
                         conf_get_str(s->conf, CONF_host),
                         conf_get_int(s->conf, CONF_port), &realhost,
                         conf_get_bool(s->conf, CONF_tcp_nodelay),
                         conf_get_bool(s->conf, CONF_tcp_keepalives));
    rssh_trace("backend_init returned %s", error ? error : "ok");
    if (error) {
        char *msg = dupprintf("Unable to open connection to %s:\n%s",
                              host, error);
        sfree(error);
        s->exited = true;
        rssh_ui_session_ended(msg);
        sfree(msg);
        return -1;
    }
    term_setup_window_titles(s->term, realhost);
    sfree(realhost);
    term_provide_backend(s->term, s->backend);
    s->ldisc = ldisc_create(s->conf, s->term, s->backend, &s->seat);
    rssh_trace("ldisc_create ok, session started");
    return 0;
}

int rssh_session_active(void)
{
    return sess && sess->backend && !sess->exited;
}

void rssh_session_resize(int cols, int rows)
{
    if (sess && sess->term)
        term_size(sess->term, rows, cols,
                  conf_get_int(sess->conf, CONF_savelines));
}

void rssh_session_send_text(const char *utf8, int len)
{
    if (!sess || !sess->ldisc)
        return;
    term_keyinput(sess->term, CS_UTF8, utf8, len);
    term_seen_key_event(sess->term);
    noise_ultralight(NOISE_SOURCE_KEY, len);
}

void rssh_session_send_key(int key, int shift, int ctrl, int alt)
{
    char buf[32];
    int n = 0;
    bool consumed_alt = false;

    if (!sess || !sess->ldisc)
        return;
    switch (key) {
      case RSSH_KEY_UP:    n = format_arrow_key(buf, sess->term, 'A', shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_DOWN:  n = format_arrow_key(buf, sess->term, 'B', shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_RIGHT: n = format_arrow_key(buf, sess->term, 'C', shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_LEFT:  n = format_arrow_key(buf, sess->term, 'D', shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_HOME:   n = format_small_keypad_key(buf, sess->term, SKK_HOME, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_END:    n = format_small_keypad_key(buf, sess->term, SKK_END, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_PGUP:   n = format_small_keypad_key(buf, sess->term, SKK_PGUP, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_PGDN:   n = format_small_keypad_key(buf, sess->term, SKK_PGDN, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_INSERT: n = format_small_keypad_key(buf, sess->term, SKK_INSERT, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_DELETE: n = format_small_keypad_key(buf, sess->term, SKK_DELETE, shift, ctrl, alt, &consumed_alt); break;
      case RSSH_KEY_ENTER:  buf[0] = '\r'; n = 1; break;
      case RSSH_KEY_TAB:    buf[0] = '\t'; n = 1; break;
      case RSSH_KEY_ESCAPE: buf[0] = '\033'; n = 1; break;
      case RSSH_KEY_BACKSPACE:
        buf[0] = conf_get_bool(sess->conf, CONF_bksp_is_delete) ? 0x7F : 0x08;
        n = 1;
        break;
      default:
        if (key >= RSSH_KEY_F1 && key < RSSH_KEY_F1 + 12)
            n = format_function_key(buf, sess->term, key - RSSH_KEY_F1 + 1,
                                    shift, ctrl, alt, &consumed_alt);
        break;
    }
    if (n > 0) {
        term_keyinput(sess->term, -1, buf, n);
        term_seen_key_event(sess->term);
        noise_ultralight(NOISE_SOURCE_KEY, key);
    }
}

void rssh_session_redraw(void)
{
    if (sess && sess->term)
        term_invalidate(sess->term);
}

void rssh_session_close(void)
{
    RsshSession *s = sess;
    if (!s)
        return;
    sess = NULL;
    if (s->question)
        s->question->s = NULL;       /* its answer will be ignored */
    delete_callbacks_for_context(s);
    free_connection(s);
    if (s->logctx)
        log_free(s->logctx);
    if (s->term)
        term_free(s->term);
    conf_free(s->conf);
    sfree(s);
}

/* ------------------------------------------------------------------ */
/* connection profiles = PuTTY saved sessions                           */
/* ------------------------------------------------------------------ */

int rssh_profiles_list(rssh_profile **out)
{
    settings_e *e = enum_settings_start();
    strbuf *name = strbuf_new();
    rssh_profile *list = NULL;
    size_t n = 0, size = 0;

    *out = NULL;
    if (!e) {
        strbuf_free(name);
        return 0;
    }
    while (strbuf_clear(name), enum_settings_next(e, name)) {
        Conf *conf;
        if (!strcmp(name->s, "Default Settings"))
            continue;
        conf = conf_new();
        do_defaults(name->s, conf);
        sgrowarray(list, size, n);
        list[n].name = dupstr(name->s);
        list[n].host = dupstr(conf_get_str(conf, CONF_host));
        /* username is STR_AMBI: conf_get_str() would assert. */
        list[n].user = dupstr(conf_get_str_ambi(conf, CONF_username, NULL));
        list[n].port = conf_get_int(conf, CONF_port);
        n++;
        conf_free(conf);
    }
    enum_settings_finish(e);
    strbuf_free(name);
    rssh_trace("profiles_list: %d", (int)n);
    *out = list;
    return (int)n;
}

void rssh_profiles_free(rssh_profile *list, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        sfree(list[i].name);
        sfree(list[i].host);
        sfree(list[i].user);
    }
    sfree(list);
}

int rssh_profile_save(const char *name, const char *host, int port,
                      const char *user)
{
    Conf *conf = conf_new();
    char *err;
    do_defaults(name, conf);          /* keep any other saved settings */
    conf_set_str(conf, CONF_host, host);
    conf_set_int(conf, CONF_port, port > 0 ? port : 22);
    conf_set_int(conf, CONF_protocol, PROT_SSH);
    conf_set_str(conf, CONF_username, user ? user : "");
    err = save_settings(name, conf);
    conf_free(conf);
    rssh_trace("profile_save -> %s", err ? err : "ok");
    if (err) {
        rssh_ui_message(err);
        sfree(err);
        return -1;
    }
    return 0;
}

void rssh_profile_delete(const char *name)
{
    del_settings(name);
}

int rssh_forget_host_keys(void)
{
    /* unix/storage.c keeps them in $HOME/.putty/sshhostkeys */
    char *path = dupprintf("%s/.putty/sshhostkeys", rssh_data_dir());
    struct stat st;
    int ret = 0;
    if (stat(path, &st) == 0)
        ret = remove(path);
    rssh_trace("forget host keys -> %d", ret);
    sfree(path);
    return ret;
}
