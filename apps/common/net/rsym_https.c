/*
 * rsym_https.c: blocking HTTPS/1.1 client on mbedTLS 4 and rsym_tcp.
 * See rsym_https.h.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <psa/crypto.h>
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

#include "rsym_https.h"
#include "rsym_tcp.h"
#include "rsym_log.h"

/* net_sockets.h's codes (MBEDTLS_NET_C is off in our build). */
#define BIO_ERR_SEND (-0x004E)
#define BIO_ERR_RECV (-0x004C)

#define MAX_HEADER_BYTES (32 * 1024)
#define DEFAULT_TIMEOUT_MS 30000

static mbedtls_x509_crt g_ca;
static int g_inited = 0;

int rsym_https_init(void)
{
    psa_status_t st;
    int ret;
    if (g_inited)
        return 0;
    st = psa_crypto_init();
    if (st != PSA_SUCCESS) {
        rsym_log("https: psa_crypto_init -> %d", (int)st);
        return (int)st;
    }
    mbedtls_x509_crt_init(&g_ca);
    ret = mbedtls_x509_crt_parse(&g_ca, (const unsigned char *)rsym_ca_google_pem,
                                 strlen(rsym_ca_google_pem) + 1);
    if (ret < 0) {
        rsym_log("https: CA parse -> -0x%04x", (unsigned)-ret);
        mbedtls_x509_crt_free(&g_ca);
        return ret;
    }
    g_inited = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* mbedTLS <-> rsym_tcp                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    rsym_tcp *tcp;
    int timeout_ms;
    int last_err;                       /* Symbian error of the last failure */
} bio_ctx;

static int bio_send(void *vctx, const unsigned char *buf, size_t len)
{
    bio_ctx *b = (bio_ctx *)vctx;
    int n = rsym_tcp_send(b->tcp, buf, (int)len, b->timeout_ms);
    if (n < 0) {
        b->last_err = n;
        return BIO_ERR_SEND;
    }
    return n;
}

static int bio_recv_timeout(void *vctx, unsigned char *buf, size_t len,
                            uint32_t timeout)
{
    bio_ctx *b = (bio_ctx *)vctx;
    int n = rsym_tcp_recv(b->tcp, buf, (int)len, timeout ? (int)timeout : b->timeout_ms);
    if (n == RSYM_TCP_TIMEOUT) {
        b->last_err = n;
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (n < 0) {
        b->last_err = n;
        return BIO_ERR_RECV;
    }
    return n;                           /* 0 = EOF */
}

/* ------------------------------------------------------------------ */
/* response body: buffering, Content-Length and chunked decoding        */
/* ------------------------------------------------------------------ */

typedef struct {
    const rsym_http_request *req;
    rsym_http_response *resp;
    int size;                           /* allocated size of resp->body */
    int aborted;
    /* chunked decoding */
    int chunked;
    enum { CH_SIZE, CH_DATA, CH_DATA_CRLF, CH_TRAILER, CH_DONE } ch_state;
    long ch_left;
    char line[64];
    int line_len;
} body_state;

static int deliver(body_state *bs, const unsigned char *data, int len)
{
    rsym_http_response *r = bs->resp;
    if (len <= 0)
        return 0;
    if (bs->req->on_body) {
        if (bs->req->on_body(bs->req->body_ctx, data, len)) {
            bs->aborted = 1;
            return -1;
        }
        r->body_len += len;
        return 0;
    }
    if (r->body_len + len + 1 > bs->size) {
        int nsize = bs->size ? bs->size : 4096;
        unsigned char *nb;
        while (nsize < r->body_len + len + 1)
            nsize *= 2;
        nb = (unsigned char *)realloc(r->body, nsize);
        if (!nb)
            return -1;
        r->body = nb;
        bs->size = nsize;
    }
    memcpy(r->body + r->body_len, data, len);
    r->body_len += len;
    r->body[r->body_len] = 0;
    return 0;
}

/* Feed raw (possibly chunked) body bytes; returns -1 on error/abort. */
static int feed(body_state *bs, const unsigned char *p, int len)
{
    if (!bs->chunked)
        return deliver(bs, p, len);
    while (len > 0 && bs->ch_state != CH_DONE) {
        switch (bs->ch_state) {
        case CH_SIZE:
        case CH_DATA_CRLF:
        case CH_TRAILER: {
            char c = (char)*p++;
            len--;
            if (c == '\r')
                break;
            if (c != '\n') {
                if (bs->line_len < (int)sizeof(bs->line) - 1)
                    bs->line[bs->line_len++] = c;
                break;
            }
            bs->line[bs->line_len] = 0;
            if (bs->ch_state == CH_SIZE) {
                bs->ch_left = strtol(bs->line, NULL, 16);
                bs->ch_state = bs->ch_left > 0 ? CH_DATA : CH_TRAILER;
            } else if (bs->ch_state == CH_DATA_CRLF) {
                bs->ch_state = CH_SIZE;
            } else if (bs->line_len == 0) {
                bs->ch_state = CH_DONE;         /* empty line ends trailers */
            }
            bs->line_len = 0;
            break;
        }
        case CH_DATA: {
            int n = len < bs->ch_left ? len : (int)bs->ch_left;
            if (deliver(bs, p, n) < 0)
                return -1;
            p += n;
            len -= n;
            bs->ch_left -= n;
            if (bs->ch_left == 0)
                bs->ch_state = CH_DATA_CRLF;
            break;
        }
        default:
            len = 0;
            break;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* helpers                                                              */
/* ------------------------------------------------------------------ */

static void set_error(rsym_http_response *r, const char *what, int ret)
{
    char buf[128];
    mbedtls_strerror(ret, buf, sizeof(buf));
    snprintf(r->error, sizeof(r->error), "%s: %s (-0x%04x)", what, buf,
             (unsigned)(ret < 0 ? -ret : ret));
}

static int header_value(const char *headers, const char *name, char *buf, int buflen)
{
    size_t nlen = strlen(name);
    const char *p = headers;
    while (p && *p) {
        const char *eol = strstr(p, "\r\n");
        size_t llen = eol ? (size_t)(eol - p) : strlen(p);
        if (llen > nlen && p[nlen] == ':' && !strncasecmp(p, name, nlen)) {
            const char *v = p + nlen + 1;
            size_t vlen;
            while (*v == ' ' || *v == '\t')
                v++;
            vlen = llen - (size_t)(v - p);
            if ((int)vlen >= buflen)
                vlen = buflen - 1;
            memcpy(buf, v, vlen);
            buf[vlen] = 0;
            return 1;
        }
        p = eol ? eol + 2 : NULL;
    }
    return 0;
}

char *rsym_http_header(const rsym_http_response *resp, const char *name,
                       char *buf, int buflen)
{
    if (!resp->headers || !header_value(resp->headers, name, buf, buflen))
        return NULL;
    return buf;
}

void rsym_http_response_free(rsym_http_response *resp)
{
    free(resp->headers);
    free(resp->body);
    resp->headers = NULL;
    resp->body = NULL;
}

static int ssl_write_all(mbedtls_ssl_context *ssl, const unsigned char *p, size_t len)
{
    while (len > 0) {
        int n = mbedtls_ssl_write(ssl, p, len);
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (n < 0)
            return n;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the request                                                          */
/* ------------------------------------------------------------------ */

int rsym_https_request(const rsym_http_request *req, rsym_http_response *resp)
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    bio_ctx bio;
    body_state bs;
    char *head = NULL, *hdrbuf = NULL;
    int hdr_len = 0, have_headers = 0, ret = -1, err;
    long content_length = -1;
    int port = req->port ? req->port : 443;
    unsigned char rbuf[4096];

    memset(resp, 0, sizeof(*resp));
    memset(&bio, 0, sizeof(bio));
    memset(&bs, 0, sizeof(bs));
    bs.req = req;
    bs.resp = resp;
    bio.timeout_ms = req->timeout_ms > 0 ? req->timeout_ms : DEFAULT_TIMEOUT_MS;

    if ((err = rsym_https_init()) != 0) {
        set_error(resp, "TLS initialisation failed", err);
        return -1;
    }

    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);

    rsym_log("https: %s https://%s%s", req->method, req->host, req->path);
    bio.tcp = rsym_tcp_connect(req->host, port, bio.timeout_ms, &err);
    if (!bio.tcp) {
        snprintf(resp->error, sizeof(resp->error), "Cannot connect to %s: %s (%d)",
                 req->host, rsym_tcp_strerror(err), err);
        goto out;
    }

    if ((err = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                           MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        set_error(resp, "TLS configuration failed", err);
        goto out;
    }
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &g_ca, NULL);
    mbedtls_ssl_conf_read_timeout(&conf, (uint32_t)bio.timeout_ms);
    if ((err = mbedtls_ssl_setup(&ssl, &conf)) != 0 ||
        (err = mbedtls_ssl_set_hostname(&ssl, req->host)) != 0) {
        set_error(resp, "TLS setup failed", err);
        goto out;
    }
    mbedtls_ssl_set_bio(&ssl, &bio, bio_send, NULL, bio_recv_timeout);

    do {
        err = mbedtls_ssl_handshake(&ssl);
    } while (err == MBEDTLS_ERR_SSL_WANT_READ || err == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (err != 0) {
        uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (flags != 0 && flags != (uint32_t)-1) {
            char vbuf[160];
            mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "", flags);
            snprintf(resp->error, sizeof(resp->error),
                     "Certificate check failed for %s: %s", req->host, vbuf);
        } else if (bio.last_err) {
            snprintf(resp->error, sizeof(resp->error), "TLS handshake: %s (%d)",
                     rsym_tcp_strerror(bio.last_err), bio.last_err);
        } else {
            set_error(resp, "TLS handshake failed", err);
        }
        goto out;
    }
    rsym_log("https: handshake ok, %s %s", mbedtls_ssl_get_version(&ssl),
             mbedtls_ssl_get_ciphersuite(&ssl));

    /* request line and headers */
    {
        int has_body = req->body && req->body_len > 0;
        int need_length = has_body || !strcmp(req->method, "POST") ||
                          !strcmp(req->method, "PUT") || !strcmp(req->method, "PATCH");
        size_t hlen = strlen(req->method) + strlen(req->path) + strlen(req->host) +
                      (req->headers ? strlen(req->headers) : 0) + 200;
        head = (char *)malloc(hlen);
        if (!head) {
            snprintf(resp->error, sizeof(resp->error), "Out of memory");
            goto out;
        }
        snprintf(head, hlen,
                 "%s %s HTTP/1.1\r\n"
                 "Host: %s\r\n"
                 "User-Agent: rsym/0.1 (Symbian)\r\n"
                 "Accept-Encoding: identity\r\n"
                 "Connection: close\r\n"
                 "%s",
                 req->method, req->path, req->host, req->headers ? req->headers : "");
        if (need_length)
            snprintf(head + strlen(head), hlen - strlen(head),
                     "Content-Length: %d\r\n", has_body ? req->body_len : 0);
        strncat(head, "\r\n", hlen - strlen(head) - 1);
        if ((err = ssl_write_all(&ssl, (const unsigned char *)head, strlen(head))) != 0 ||
            (has_body && (err = ssl_write_all(&ssl, (const unsigned char *)req->body,
                                              (size_t)req->body_len)) != 0)) {
            set_error(resp, "Sending the request failed", err);
            goto out;
        }
    }

    /* response */
    hdrbuf = (char *)malloc(MAX_HEADER_BYTES + 1);
    if (!hdrbuf) {
        snprintf(resp->error, sizeof(resp->error), "Out of memory");
        goto out;
    }
    for (;;) {
        int n = mbedtls_ssl_read(&ssl, rbuf, sizeof(rbuf));
        if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE ||
            n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;
        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || n == MBEDTLS_ERR_SSL_CONN_EOF)
            break;                                      /* end of stream */
        if (n < 0) {
            set_error(resp, "Reading the response failed", n);
            goto out;
        }
        if (!have_headers) {
            char *end;
            int take = n;
            if (hdr_len + take > MAX_HEADER_BYTES)
                take = MAX_HEADER_BYTES - hdr_len;
            memcpy(hdrbuf + hdr_len, rbuf, take);
            hdr_len += take;
            hdrbuf[hdr_len] = 0;
            end = strstr(hdrbuf, "\r\n\r\n");
            if (!end) {
                if (hdr_len >= MAX_HEADER_BYTES) {
                    snprintf(resp->error, sizeof(resp->error), "Response headers too long");
                    goto out;
                }
                continue;
            }
            {
                int head_bytes = (int)(end - hdrbuf) + 4;
                int extra = hdr_len - head_bytes + (n - take);  /* body bytes already read */
                char val[32];
                *end = 0;
                resp->headers = strdup(hdrbuf);
                if (sscanf(hdrbuf, "HTTP/%*d.%*d %d", &resp->status) != 1) {
                    snprintf(resp->error, sizeof(resp->error), "Not an HTTP response");
                    goto out;
                }
                if (header_value(resp->headers, "Transfer-Encoding", val, sizeof(val)) &&
                    strstr(val, "chunked"))
                    bs.chunked = 1;
                else if (header_value(resp->headers, "Content-Length", val, sizeof(val)))
                    content_length = atol(val);
                have_headers = 1;
                if (extra > 0) {
                    /* the remainder of this read: header copy plus what didn't fit */
                    const unsigned char *p = rbuf + (n - extra);
                    if (feed(&bs, p, extra) < 0)
                        goto body_err;
                }
            }
        } else if (feed(&bs, rbuf, n) < 0) {
            goto body_err;
        }
        if ((bs.chunked && bs.ch_state == CH_DONE) ||
            (!bs.chunked && content_length >= 0 && resp->body_len >= content_length))
            break;
    }
    if (!have_headers) {
        snprintf(resp->error, sizeof(resp->error), "Connection closed before a response");
        goto out;
    }
    rsym_log("https: HTTP %d, %d body bytes", resp->status, resp->body_len);
    ret = 0;
    goto out;

body_err:
    snprintf(resp->error, sizeof(resp->error),
             bs.aborted ? "Transfer cancelled" : "Out of memory reading the response");

out:
    if (ret != 0)
        rsym_log("https: failed: %s", resp->error);
    if (bio.tcp) {
        mbedtls_ssl_close_notify(&ssl);
        rsym_tcp_close(bio.tcp);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    free(head);
    free(hdrbuf);
    return ret;
}
