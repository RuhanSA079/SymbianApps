/*
 * fetch_rsym.c: NetSurf fetcher for http: and https: on Symbian, replacing
 * the curl fetcher. Each fetch runs rsym_https_request (native RSocket +
 * mbedTLS) on its own worker thread; the worker queues what it receives and
 * fetch_rsym_poll, on NetSurf's thread, hands it to the core. NetSurf
 * objects (nsurl, the fetch, callbacks) are only touched on NetSurf's thread.
 *
 * A fetch is shared by NetSurf and the worker (refs = 2 while the worker
 * runs); NetSurf may abort and free its side at any time, and the worker
 * then stops at its next network read and drops the last reference.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libwapcaplet/libwapcaplet.h>
#include <nsutils/base64.h>

#include "utils/config.h"
#include "utils/corestrings.h"
#include "utils/log.h"
#include "utils/messages.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"
#include "utils/useragent.h"
#include "content/fetch.h"
#include "content/fetchers.h"
#include "content/urldb.h"

#include "rsym_https.h"
#include "ns_thread.h"
#include "rsym_log.h"

#define WORKER_STACK (128 * 1024)
#define TIMEOUT_MS 60000

enum ev_type { EV_HEADERS, EV_DATA, EV_DONE, EV_ERROR };

struct ev {
	struct ev *next;
	enum ev_type type;
	int status;		/* EV_HEADERS */
	int cert_error;		/* EV_ERROR */
	size_t len;
	char data[1];		/* headers / body bytes / error text */
};

struct rsym_fetch {
	struct rsym_fetch *next;	/* all live fetches (NetSurf side) */

	/* NetSurf side */
	struct fetch *parent;
	nsurl *url;
	bool only_2xx;
	bool finished;		/* a terminal message was sent */
	bool started;

	/* shared with the worker (under ns_mutex) */
	int refs;
	struct ev *head, *tail;
	volatile int cancel;

	/* the request; owned by the fetch, read by the worker */
	char *host, *path, *headers;
	char *body;
	int body_len;
	int port, https;
	const char *method;
};

static struct rsym_fetch *fetch_list;
static char *ca_bundle;

/* ------------------------------------------------------------------ */
/* shared state                                                         */
/* ------------------------------------------------------------------ */

static void fetch_release(struct rsym_fetch *f)
{
	int left;
	struct ev *e, *n;

	ns_mutex_lock();
	left = --f->refs;
	ns_mutex_unlock();
	if (left > 0)
		return;
	for (e = f->head; e != NULL; e = n) {
		n = e->next;
		free(e);
	}
	free(f->host);
	free(f->path);
	free(f->headers);
	free(f->body);
	free(f);
}

/* worker: queue an event; returns -1 if out of memory */
static int push(struct rsym_fetch *f, enum ev_type type, int status,
		const void *data, size_t len)
{
	struct ev *e = malloc(sizeof(*e) + len);
	if (e == NULL)
		return -1;
	e->next = NULL;
	e->type = type;
	e->status = status;
	e->cert_error = 0;
	e->len = len;
	if (len > 0)
		memcpy(e->data, data, len);
	e->data[len] = 0;
	ns_mutex_lock();
	if (f->tail != NULL)
		f->tail->next = e;
	else
		f->head = e;
	f->tail = e;
	ns_mutex_unlock();
	return 0;
}

/* ------------------------------------------------------------------ */
/* worker thread                                                        */
/* ------------------------------------------------------------------ */

static int on_headers(void *ctx, int status, const char *headers)
{
	struct rsym_fetch *f = ctx;
	if (f->cancel)
		return 1;
	return push(f, EV_HEADERS, status, headers, strlen(headers)) != 0;
}

static int on_body(void *ctx, const unsigned char *data, int len)
{
	struct rsym_fetch *f = ctx;
	if (f->cancel)
		return 1;
	return push(f, EV_DATA, 0, data, (size_t)len) != 0;
}

static void worker(void *arg)
{
	struct rsym_fetch *f = arg;
	rsym_log("worker %p: start", (void *)f);
	rsym_http_request req;
	rsym_http_response resp;
	int ret;

	memset(&req, 0, sizeof(req));
	req.method = f->method;
	req.host = f->host;
	req.port = f->port;
	req.path = f->path;
	req.headers = f->headers;
	req.body = f->body;
	req.body_len = f->body_len;
	req.timeout_ms = TIMEOUT_MS;
	req.on_body = on_body;
	req.on_headers = on_headers;
	req.body_ctx = f;
	req.plain = !f->https;
	req.user_agent = user_agent_string();
	req.cancel = &f->cancel;

	ret = rsym_https_request(&req, &resp);
	if (!f->cancel) {
		if (ret == 0) {
			push(f, EV_DONE, resp.status, NULL, 0);
		} else {
			if (push(f, EV_ERROR, 0, resp.error, strlen(resp.error)) == 0 &&
			    resp.cert_error) {
				ns_mutex_lock();
				f->tail->cert_error = 1;
				ns_mutex_unlock();
			}
		}
	}
	rsym_log("worker %p: done (%d)", (void *)f, ret);
	rsym_http_response_free(&resp);
	fetch_release(f);
}

/* ------------------------------------------------------------------ */
/* building the request (NetSurf thread)                                */
/* ------------------------------------------------------------------ */

struct sbuf {
	char *p;
	size_t len, size;
	bool failed;
};

static void sb_add(struct sbuf *b, const void *data, size_t len)
{
	if (b->failed)
		return;
	if (b->len + len + 1 > b->size) {
		size_t n = b->size ? b->size * 2 : 512;
		char *np;
		while (n < b->len + len + 1)
			n *= 2;
		np = realloc(b->p, n);
		if (np == NULL) {
			b->failed = true;
			return;
		}
		b->p = np;
		b->size = n;
	}
	memcpy(b->p + b->len, data, len);
	b->len += len;
	b->p[b->len] = 0;
}

static void sb_str(struct sbuf *b, const char *s)
{
	sb_add(b, s, strlen(s));
}

static void sb_header(struct sbuf *b, const char *name, const char *value)
{
	sb_str(b, name);
	sb_str(b, ": ");
	sb_str(b, value);
	sb_str(b, "\r\n");
}

/* multipart/form-data body; returns the boundary used */
static void build_multipart(struct sbuf *body, const char *boundary,
			    const struct fetch_multipart_data *part)
{
	for (; part != NULL; part = part->next) {
		sb_str(body, "--");
		sb_str(body, boundary);
		sb_str(body, "\r\nContent-Disposition: form-data; name=\"");
		sb_str(body, part->name);
		sb_str(body, "\"");
		if (part->file) {
			FILE *fp = part->rawfile ? fopen(part->rawfile, "rb") : NULL;
			char buf[1024];
			size_t n;
			sb_str(body, "; filename=\"");
			sb_str(body, part->value);
			sb_str(body, "\"\r\nContent-Type: application/octet-stream\r\n\r\n");
			if (fp != NULL) {
				while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
					sb_add(body, buf, n);
				fclose(fp);
			}
		} else {
			sb_str(body, "\r\n\r\n");
			sb_str(body, part->value);
		}
		sb_str(body, "\r\n");
	}
	sb_str(body, "--");
	sb_str(body, boundary);
	sb_str(body, "--\r\n");
}

static void *
fetch_rsym_setup(struct fetch *parent_fetch, nsurl *url, bool only_2xx,
		 bool downgrade_tls, const char *post_urlenc,
		 const struct fetch_multipart_data *post_multipart,
		 const char **headers)
{
	struct rsym_fetch *f;
	struct sbuf h = { 0 };
	lwc_string *scheme, *host, *port, *path, *query;
	char *s;
	int i;

	(void)downgrade_tls;

	f = calloc(1, sizeof(*f));
	if (f == NULL)
		return NULL;
	f->parent = parent_fetch;
	f->url = nsurl_ref(url);
	f->only_2xx = only_2xx;
	f->refs = 1;
	f->method = "GET";

	scheme = nsurl_get_component(url, NSURL_SCHEME);
	if (scheme != NULL) {
		bool match = false;
		if (lwc_string_caseless_isequal(scheme, corestring_lwc_https,
						&match) == lwc_error_ok)
			f->https = match;
		lwc_string_unref(scheme);
	}

	host = nsurl_get_component(url, NSURL_HOST);
	if (host == NULL)
		goto failed;
	f->host = strdup(lwc_string_data(host));
	lwc_string_unref(host);
	/* IPv6 literals come as [addr]; rsym_tcp wants the bare address */
	if (f->host != NULL && f->host[0] == '[') {
		size_t n = strlen(f->host);
		memmove(f->host, f->host + 1, n);
		if (n >= 2 && f->host[n - 2] == ']')
			f->host[n - 2] = 0;
	}

	port = nsurl_get_component(url, NSURL_PORT);
	if (port != NULL) {
		f->port = atoi(lwc_string_data(port));
		lwc_string_unref(port);
	}

	path = nsurl_get_component(url, NSURL_PATH);
	query = nsurl_get_component(url, NSURL_QUERY);
	{
		struct sbuf p = { 0 };
		sb_str(&p, path != NULL ? lwc_string_data(path) : "/");
		if (p.len == 0)
			sb_str(&p, "/");
		if (query != NULL) {
			sb_str(&p, "?");
			sb_str(&p, lwc_string_data(query));
		}
		f->path = p.p;
		if (p.failed)
			goto failed;
	}
	if (path != NULL)
		lwc_string_unref(path);
	if (query != NULL)
		lwc_string_unref(query);
	if (f->host == NULL || f->path == NULL)
		goto failed;

	/* headers, as the curl fetcher sends them */
	sb_header(&h, "Accept", "*/*");
	if (nsoption_charp(accept_language) != NULL &&
	    nsoption_charp(accept_language)[0] != '\0') {
		char v[80];
		snprintf(v, sizeof v, "%s, *;q=0.1", nsoption_charp(accept_language));
		sb_header(&h, "Accept-Language", v);
	}
	if (nsoption_charp(accept_charset) != NULL &&
	    nsoption_charp(accept_charset)[0] != '\0') {
		char v[80];
		snprintf(v, sizeof v, "%s, *;q=0.1", nsoption_charp(accept_charset));
		sb_header(&h, "Accept-Charset", v);
	}
	if (nsoption_bool(do_not_track) == true)
		sb_header(&h, "DNT", "1");

	s = urldb_get_cookie(url, true);
	if (s != NULL) {
		sb_header(&h, "Cookie", s);
		free(s);
	}

	{
		const char *auth = urldb_get_auth_details(url, NULL);
		if (auth != NULL) {
			uint8_t *enc = NULL;
			size_t enclen = 0;
			if (nsu_base64_encode_alloc((const uint8_t *)auth, strlen(auth),
					      &enc, &enclen) == NSUERROR_OK) {
				sb_str(&h, "Authorization: Basic ");
				sb_add(&h, enc, enclen);
				sb_str(&h, "\r\n");
				free(enc);
			}
		}
	}

	if (post_urlenc != NULL) {
		f->method = "POST";
		f->body = strdup(post_urlenc);
		if (f->body == NULL)
			goto failed;
		f->body_len = (int)strlen(post_urlenc);
		sb_header(&h, "Content-Type", "application/x-www-form-urlencoded");
	} else if (post_multipart != NULL) {
		struct sbuf body = { 0 };
		char boundary[48];
		unsigned char rnd[12];
		int k;
		arc4random_buf(rnd, sizeof rnd);
		k = snprintf(boundary, sizeof boundary, "----NetSurfFormBoundary");
		for (i = 0; i < (int)sizeof rnd && k < (int)sizeof boundary - 3; i++)
			k += snprintf(boundary + k, sizeof boundary - k, "%02x", rnd[i]);
		build_multipart(&body, boundary, post_multipart);
		if (body.failed) {
			free(body.p);
			goto failed;
		}
		f->method = "POST";
		f->body = body.p;
		f->body_len = (int)body.len;
		sb_str(&h, "Content-Type: multipart/form-data; boundary=");
		sb_str(&h, boundary);
		sb_str(&h, "\r\n");
	}

	/* and the ones from the caller (Referer, If-None-Match, ...) */
	for (i = 0; headers[i] != NULL; i++) {
		sb_str(&h, headers[i]);
		sb_str(&h, "\r\n");
	}
	if (h.failed)
		goto failed;
	f->headers = h.p;
	h.p = NULL;

	NSLOG(netsurf, INFO, "rsym fetch %p: %s %s", f, f->method, nsurl_access(url));
	f->next = fetch_list;
	fetch_list = f;
	return f;

failed:
	free(h.p);
	nsurl_unref(f->url);
	fetch_release(f);
	return NULL;
}

static bool fetch_rsym_start(void *vf)
{
	struct rsym_fetch *f = vf;
	ns_mutex_lock();
	f->refs++;			/* the worker's reference */
	ns_mutex_unlock();
	int err = ns_thread_start(worker, f, WORKER_STACK);
	if (err != 0) {
		NSLOG(netsurf, WARNING, "rsym fetch %p: no worker thread (%d)", f, err);
		ns_mutex_lock();
		f->refs--;
		ns_mutex_unlock();
		return false;
	}
	f->started = true;
	return true;
}

static void fetch_rsym_abort(void *vf)
{
	struct rsym_fetch *f = vf;
	f->cancel = 1;
	fetch_remove_from_queues(f->parent);
	fetch_free(f->parent);		/* calls fetch_rsym_free */
}

static void fetch_rsym_free(void *vf)
{
	struct rsym_fetch *f = vf;
	struct rsym_fetch **pp;

	f->cancel = 1;
	for (pp = &fetch_list; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == f) {
			*pp = f->next;
			break;
		}
	}
	nsurl_unref(f->url);
	f->url = NULL;
	fetch_release(f);
}

/* ------------------------------------------------------------------ */
/* delivering results (NetSurf thread)                                  */
/* ------------------------------------------------------------------ */

/* Send a terminal message and let the core free the fetch. */
static void finish(struct rsym_fetch *f, fetch_msg *msg)
{
	f->finished = true;
	f->cancel = 1;
	if (msg != NULL)
		fetch_send_callback(msg, f->parent);
	fetch_remove_from_queues(f->parent);
	fetch_free(f->parent);
}

static void send_error(struct rsym_fetch *f, fetch_msg_type type, const char *text)
{
	fetch_msg msg;
	msg.type = type;
	msg.data.error = text;
	finish(f, &msg);
}

/* Returns true if the fetch was finished (and freed). */
static bool process_headers(struct rsym_fetch *f, int status, char *headers)
{
	fetch_msg msg;
	char *line, *next, *location = NULL, *realm = NULL;
	bool stop = false;

	fetch_set_http_code(f->parent, status);
	NSLOG(netsurf, INFO, "rsym fetch %p: HTTP %d", f, status);

	for (line = headers; line != NULL && *line; line = next) {
		char *eol = strstr(line, "\r\n");
		size_t len = eol ? (size_t)(eol - line) : strlen(line);
		next = eol ? eol + 2 : NULL;
		{
			/* the core wants each header with its CRLF */
			char *h = malloc(len + 3);
			if (h == NULL)
				continue;
			memcpy(h, line, len);
			memcpy(h + len, "\r\n", 3);
			msg.type = FETCH_HEADER;
			msg.data.header_or_data.buf = (const uint8_t *)h;
			msg.data.header_or_data.len = len + 2;
			fetch_send_callback(&msg, f->parent);
			free(h);
		}
		if (len > 9 && strncasecmp(line, "Location:", 9) == 0) {
			char *v = line + 9;
			while (*v == ' ' || *v == '\t')
				v++;
			free(location);
			location = strndup(v, len - (size_t)(v - line));
		} else if (len > 11 && strncasecmp(line, "Set-Cookie:", 11) == 0) {
			char *v = line + 11, *c;
			while (*v == ' ' || *v == '\t')
				v++;
			c = strndup(v, len - (size_t)(v - line));
			if (c != NULL) {
				fetch_set_cookie(f->parent, c);
				free(c);
			}
		} else if (len > 17 && strncasecmp(line, "WWW-Authenticate:", 17) == 0) {
			char *r = strcasestr(line, "realm=\"");
			if (r != NULL && r < line + len) {
				char *e = memchr(r + 7, '"', len - (size_t)(r + 7 - line));
				if (e != NULL) {
					free(realm);
					realm = strndup(r + 7, (size_t)(e - (r + 7)));
				}
			}
		}
	}

	if (status == 304) {
		msg.type = FETCH_NOTMODIFIED;
		finish(f, &msg);
		stop = true;
	} else if (status >= 300 && status < 400 && location != NULL) {
		NSLOG(netsurf, INFO, "rsym fetch %p: redirect to %s", f, location);
		msg.type = FETCH_REDIRECT;
		msg.data.redirect = location;
		finish(f, &msg);
		stop = true;
	} else if (status == 401) {
		msg.type = FETCH_AUTH;
		msg.data.auth.realm = realm != NULL ? realm : "";
		finish(f, &msg);
		stop = true;
	} else if (f->only_2xx && (status < 200 || status > 299)) {
		send_error(f, FETCH_ERROR, messages_get("Not2xx"));
		stop = true;
	}
	free(location);
	free(realm);
	return stop;
}

static void fetch_rsym_poll(lwc_string *scheme)
{
	struct rsym_fetch *f, *next, *g;

	(void)scheme;
	for (f = fetch_list; f != NULL; f = next) {
		struct ev *events, *e, *en;
		bool done = false;

		next = f->next;
		if (!f->started)
			continue;

		/* Our own reference: a callback below may make the core abort
		 * and free this fetch (f->url is then NULL). */
		ns_mutex_lock();
		events = f->head;
		f->head = f->tail = NULL;
		if (events != NULL)
			f->refs++;
		ns_mutex_unlock();
		if (events == NULL)
			continue;
		rsym_log("poll: events for fetch %p", (void *)f);

		for (e = events; e != NULL; e = en) {
			fetch_msg msg;
			en = e->next;
			if (!done && f->url != NULL) {
				switch (e->type) {
				case EV_HEADERS:
					done = process_headers(f, e->status, e->data);
					break;
				case EV_DATA:
					msg.type = FETCH_DATA;
					msg.data.header_or_data.buf = (const uint8_t *)e->data;
					msg.data.header_or_data.len = e->len;
					fetch_send_callback(&msg, f->parent);
					break;
				case EV_DONE:
					msg.type = FETCH_FINISHED;
					finish(f, &msg);
					done = true;
					break;
				case EV_ERROR:
					NSLOG(netsurf, INFO, "rsym fetch %p: %s", f, e->data);
					send_error(f, FETCH_ERROR, e->data);
					done = true;
					break;
				}
			}
			free(e);
		}
		fetch_release(f);

		/* callbacks may also have freed `next`: if so, start over */
		if (next != NULL) {
			for (g = fetch_list; g != NULL && g != next; g = g->next)
				;
			if (g == NULL)
				next = fetch_list;
		}
	}
}

/* ------------------------------------------------------------------ */
/* registration                                                         */
/* ------------------------------------------------------------------ */

static bool fetch_rsym_initialise(lwc_string *scheme)
{
	(void)scheme;
	return true;
}

static void fetch_rsym_finalise(lwc_string *scheme)
{
	(void)scheme;
}

static bool fetch_rsym_can_fetch(const nsurl *url)
{
	(void)url;
	return true;
}

static int fetch_rsym_fdset(lwc_string *scheme, fd_set *read_set,
			    fd_set *write_set, fd_set *error_set)
{
	(void)scheme; (void)read_set; (void)write_set; (void)error_set;
	return -1;
}

/* Load the CA bundle shipped in NetSurf's resources (resources/ca-bundle). */
static void load_ca_bundle(void)
{
	const char *path = nsoption_charp(ca_bundle);
	FILE *fp;
	long n;

	if (path == NULL || path[0] == '\0')
		return;
	fp = fopen(path, "rb");
	if (fp == NULL) {
		NSLOG(netsurf, WARNING, "cannot open CA bundle %s", path);
		return;
	}
	fseek(fp, 0, SEEK_END);
	n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	ca_bundle = n > 0 ? malloc((size_t)n + 1) : NULL;
	if (ca_bundle != NULL && fread(ca_bundle, 1, (size_t)n, fp) == (size_t)n) {
		ca_bundle[n] = 0;
		rsym_https_set_ca(ca_bundle);
		NSLOG(netsurf, INFO, "CA bundle %s (%ld bytes)", path, n);
	}
	fclose(fp);
}

nserror fetch_rsym_register(void)
{
	static const struct fetcher_operation_table ops = {
		.initialise = fetch_rsym_initialise,
		.acceptable = fetch_rsym_can_fetch,
		.setup = fetch_rsym_setup,
		.start = fetch_rsym_start,
		.abort = fetch_rsym_abort,
		.free = fetch_rsym_free,
		.poll = fetch_rsym_poll,
		.fdset = fetch_rsym_fdset,
		.finalise = fetch_rsym_finalise,
	};
	nserror ret;

	if (ns_mutex_init() != 0)
		return NSERROR_INIT_FAILED;
	load_ca_bundle();

	ret = fetcher_add(lwc_string_ref(corestring_lwc_http), &ops);
	if (ret != NSERROR_OK)
		return ret;
	return fetcher_add(lwc_string_ref(corestring_lwc_https), &ops);
}
