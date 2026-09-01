/*
 * WebSDR Server — WebSocket/HTTP server using libwebsockets
 *
 * Serves the original WebSDR frontend (pub/) and provides the
 * client-facing endpoints used by the unmodified JS:
 *   HTTP  : static files under pub/, with SSI include expansion
 *   WS    : /~~waterstream<band> (waterfall), /~~stream (audio)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libwebsockets.h>

#include "websdr.h"

extern struct websdr_config *g_config;

/* Set in server_start so other threads can wake the lws service loop.
 * lws_cancel_service(ctx) is thread-safe and unblocks the poll() that the
 * service thread sits in; without it, rows queued from the band thread are
 * never flushed (the loop only re-polls on a new client message). */
struct lws_context *g_lws_ctx = NULL;

#define BANDINFO_CAP (1 << 17)   /* 128 KB of generated bandinfo.js */
static char g_bandinfo[BANDINFO_CAP];
static int g_bandinfo_len = 0;

/* ------------------------------------------------------------------ */
/* Client tracking                                                     */
/* ------------------------------------------------------------------ */

void client_add_to_band(struct client *cli, struct band *band) {
    pthread_mutex_lock(&band->lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (band->clients[i] == NULL) {
            band->clients[i] = cli;
            band->nclients++;
            fprintf(stderr, "[ws] client added to band %s (%d total)\n",
                    band->name, band->nclients);
            break;
        }
    }
    pthread_mutex_unlock(&band->lock);
}

void client_remove_from_band(struct client *cli) {
    if (!cli->band) return;
    struct band *band = cli->band;
    pthread_mutex_lock(&band->lock);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (band->clients[i] == cli) {
            band->clients[i] = NULL;
            band->nclients--;
            fprintf(stderr, "[ws] client removed from band %s (%d total)\n",
                    band->name, band->nclients);
            break;
        }
    }
    pthread_mutex_unlock(&band->lock);
    cli->band = NULL;
}

/* Append bytes to a client's output ring queue. Returns 0 on success. */
static void client_enqueue(struct client *cli, const uint8_t *data, size_t len) {
    pthread_mutex_lock(&cli->out_mutex);
    int next = (cli->outq_tail + 1) % CLIENT_OUT_MAX;
    if (next == cli->outq_head) {
        /* queue full: drop the oldest message */
        if (cli->outq[cli->outq_head]) {
            free(cli->outq[cli->outq_head]);
            cli->outq[cli->outq_head] = NULL;
        }
        cli->outq_head = (cli->outq_head + 1) % CLIENT_OUT_MAX;
    }
    cli->outq[cli->outq_tail] = malloc(len ? len : 1);
    if (cli->outq[cli->outq_tail]) {
        if (len) memcpy(cli->outq[cli->outq_tail], data, len);
        cli->outq_len[cli->outq_tail] = len;
    } else {
        cli->outq_len[cli->outq_tail] = 0;
    }
    cli->outq_tail = next;
    pthread_mutex_unlock(&cli->out_mutex);
}

static void client_free_outq(struct client *cli) {
    pthread_mutex_lock(&cli->out_mutex);
    while (cli->outq_head != cli->outq_tail) {
        if (cli->outq[cli->outq_head]) free(cli->outq[cli->outq_head]);
        cli->outq[cli->outq_head] = NULL;
        cli->outq_head = (cli->outq_head + 1) % CLIENT_OUT_MAX;
    }
    pthread_mutex_unlock(&cli->out_mutex);
}

/* Send all queued messages from the kers writeable callback. */
static void client_flush(struct lws *wsi, struct client *cli) {
    for (;;) {
        pthread_mutex_lock(&cli->out_mutex);
        if (cli->outq_head == cli->outq_tail) {
            pthread_mutex_unlock(&cli->out_mutex);
            break;
        }
        uint8_t *buf = cli->outq[cli->outq_head];
        size_t n = cli->outq_len[cli->outq_head];
        cli->outq[cli->outq_head] = NULL;
        cli->outq_head = (cli->outq_head + 1) % CLIENT_OUT_MAX;
        pthread_mutex_unlock(&cli->out_mutex);

        unsigned char sendbuf[LWS_PRE + 4096];
        if (n > 4096) n = 4096;
        if (buf) {
            memcpy(sendbuf + LWS_PRE, buf, n);
            free(buf);
        }
        if (lws_write(wsi, sendbuf + LWS_PRE, n, LWS_WRITE_BINARY) < 0)
            break;
    }
}

/* Send waterfall data to all connected clients of a band. */
void band_send_waterfall(struct band *band) {
    uint8_t compressed[WATERFALL_WIDTH * 2 + 16];

    pthread_mutex_lock(&band->lock);

    /* Periodic temporal re-sync: format-9 is a delta codec whose baseline can
     * drift under codebook quantisation, so roughly every second send a
     * width-reset (clears the client row buffer to 0) and zero our prev_line,
     * re-anchoring the delta baseline as the real websdr64 does. Without it,
     * sparse vertical stripes accumulate and flood the display over time. */
    if (++band->wf_resync >= 12 && band->nclients > 0) {
        band->wf_resync = 0;
        memset(band->prev_line, 0, WATERFALL_WIDTH);
        uint8_t wf[4];
        wf[0] = 0xFF; wf[1] = 0x02;
        wf[2] = (uint8_t)(WATERFALL_WIDTH & 0xFF);
        wf[3] = (uint8_t)((WATERFALL_WIDTH >> 8) & 0xFF);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *cli = band->clients[i];
            if (cli && cli->waterfall_active && cli->wsi && !cli->audio_stream) {
                client_enqueue(cli, wf, 4);
                lws_callback_on_writable(cli->wsi);
            }
        }
        if (g_lws_ctx) lws_cancel_service(g_lws_ctx);
    }

    int len = compress_waterfall_format9(band->waterfall_line, band->prev_line,
                                         WATERFALL_WIDTH, compressed);
    if (len <= 0) { pthread_mutex_unlock(&band->lock); return; }

    /* prev_line must hold exactly what the client decoded this row (the
     * format-9 temporal baseline is 16*round(v/16), and the decoder clamps to
     * 248). Copying the raw 0..255 line here desynchronises the delta baseline
     * from the client, so columns random-walk into vertical stripes. */
    for (int i = 0; i < WATERFALL_WIDTH; i++) {
        int v = band->waterfall_line[i];
        int q = ((v + 8) / 16) * 16;
        if (q > 248) q = 248;
        band->prev_line[i] = (uint8_t)q;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *cli = band->clients[i];
        if (!cli || !cli->waterfall_active || !cli->wsi) continue;
        if (cli->audio_stream) continue;   /* this socket is audio only */

        /* escape a data row that would begin with 0xFF */
        uint8_t row[WATERFALL_WIDTH * 2 + 16 + 1];
        int rowlen = len;
        if (compressed[0] == 0xFF) {
            row[0] = 0xFF;                  /* doubles as the escape */
            memcpy(row + 1, compressed, (size_t)len);
            rowlen = len + 1;
        } else {
            memcpy(row, compressed, (size_t)len);
        }
        client_enqueue(cli, row, (size_t)rowlen);
        if (cli->wsi)
            lws_callback_on_writable(cli->wsi);
    }
    pthread_mutex_unlock(&band->lock);

    /* Wake the lws service loop so it re-polls and flushes the queued rows. */
    if (g_lws_ctx)
        lws_cancel_service(g_lws_ctx);
}

/* ------------------------------------------------------------------ */
/* SSI include expansion                                               */
/* ------------------------------------------------------------------ */

#define SSI_MARKER "<!--#include file=\""

static char *read_whole_file(const char *path, long *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }

    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (sz > 0 && fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return NULL;
    }
    buf[sz] = '\0';
    if (out_len) *out_len = sz;
    fclose(fp);
    return buf;
}

/* Recursively expand <!--#include file="X"--> within an HTML body.
 * `base` is the directory the includes resolve against (the pub dir).
 * Returns a malloc'd, growable string buffer (not necessarily NUL-safe). */
static char *expand_includes_body(const char *base, const char *body, size_t len,
                                  size_t *out_len, int depth) {
    if (depth <= 0) {
        char *r = malloc(len ? len : 1);
        if (r) { memcpy(r, body, len); }
        if (out_len) *out_len = len;
        return r;
    }

    size_t cap = len + 256;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;

    const char *cur = body;
    const char *end = body + len;
    size_t marker_len = strlen(SSI_MARKER);

    while (cur < end) {
        const char *m = strstr((char *)cur, SSI_MARKER);
        if (!m || m >= end) {
            /* copy remainder */
            size_t rem = (size_t)(end - cur);
            if (o + rem + 1 > cap) { cap = o + rem + 64; out = realloc(out, cap); }
            memcpy(out + o, cur, rem); o += rem;
            break;
        }
        /* copy up to marker */
        size_t pre = (size_t)(m - cur);
        if (o + pre + 1 > cap) { cap = o + pre + 64; out = realloc(out, cap); }
        memcpy(out + o, cur, pre); o += pre;

        /* filename between quotes */
        const char *q = m + marker_len;
        const char *qend = strchr(q, '"');
        if (!qend || qend >= end || qend == q) {
            /* malformed: emit marker literally and advance */
            if (o + marker_len + 1 > cap) { cap = o + marker_len + 64; out = realloc(out, cap); }
            memcpy(out + o, m, marker_len); o += marker_len;
            cur = m + marker_len;
            continue;
        }
        char name[512];
        size_t nlen = (size_t)(qend - q);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        memcpy(name, q, nlen); name[nlen] = '\0';

        /* skip past --> */
        const char *tail = qend + 1;
        if (tail + 3 <= end && strncmp(tail, "-->", 3) == 0) tail += 3;

        /* read the include file relative to base */
        char incpath[1024];
        snprintf(incpath, sizeof(incpath), "%s/%s", base, name);
        long ilen = 0;
        char *inc = read_whole_file(incpath, &ilen);
        if (inc) {
            size_t olen = (size_t)ilen;
            char *expanded = expand_includes_body(base, inc, (size_t)ilen, &olen, depth - 1);
            free(inc);
            if (expanded) {
                if (o + olen + 1 > cap) { cap = o + olen + 64; out = realloc(out, cap); }
                memcpy(out + o, expanded, olen); o += olen;
                free(expanded);
            }
        }

        cur = tail;
    }

    if (o + 1 > cap) { cap = o + 64; out = realloc(out, cap); }
    /* add NUL terminator space */
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

static int serve_file(struct lws *wsi, const char *pubdir, const char *uri) {
    /* Resolve pubdir + uri -> path */
    char path[1024];
    if (uri[0] == '/') uri++;
    snprintf(path, sizeof(path), "%s/%s", pubdir, uri);

    const char *mime = "text/html";
    if (strstr(path, ".js"))        mime = "application/javascript";
    else if (strstr(path, ".css"))  mime = "text/css";
    else if (strstr(path, ".png"))  mime = "image/png";
    else if (strstr(path, ".jpg"))  mime = "image/jpeg";
    else if (strstr(path, ".jpeg")) mime = "image/jpeg";
    else if (strstr(path, ".svg"))  mime = "image/svg+xml";
    else if (strstr(path, ".ttf"))  mime = "font/ttf";
    else if (strstr(path, ".jar"))  mime = "application/java-archive";
    else if (strstr(path, ".ico"))  mime = "image/x-icon";
    else if (strstr(path, ".txt"))  mime = "text/plain";

    char *body = NULL;
    size_t body_len = 0;
    int is_html = (strstr(path, ".html") != NULL);

    if (is_html) {
        long bl = 0;
        char *raw = read_whole_file(path, &bl);
        if (!raw) { lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Not found"); return -1; }
        char *expanded = expand_includes_body(pubdir, raw, (size_t)bl, &body_len, 8);
        free(raw);
        if (!expanded) { lws_return_http_status(wsi, HTTP_STATUS_INTERNAL_SERVER_ERROR, "SSI error"); return -1; }
        body = expanded;
    } else {
        long bl = 0;
        char *raw = read_whole_file(path, &bl);
        if (!raw) { lws_return_http_status(wsi, HTTP_STATUS_NOT_FOUND, "Not found"); return -1; }
        body = raw;
        body_len = (size_t)bl;
    }

    unsigned char *buf = malloc(LWS_PRE + 1024 + body_len);
    if (!buf) { free(body); return -1; }

    unsigned char *p = buf + LWS_PRE;
    unsigned char *end = buf + LWS_PRE + 1024;

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end))
        goto fail;
    if (lws_add_http_header_by_name(wsi,
            (const unsigned char *)"content-type:",
            (const unsigned char *)mime, strlen(mime), &p, end))
        goto fail;
    if (lws_add_http_header_content_length(wsi, body_len, &p, end))
        goto fail;
    if (lws_finalize_http_header(wsi, &p, end))
        goto fail;

    size_t hdr_len = (size_t)(p - (buf + LWS_PRE));
    if (lws_write(wsi, buf + LWS_PRE, hdr_len, LWS_WRITE_HTTP_HEADERS) < 0)
        goto fail;

    memcpy(buf + LWS_PRE, body, body_len);

    if (lws_write(wsi, buf + LWS_PRE, body_len, LWS_WRITE_HTTP) < 0) {
        free(buf); free(body);
        return -1;
    }
    free(buf);
    free(body);

    if (lws_http_transaction_completed(wsi))
        return -1;
    return 0;

fail:
    free(buf);
    free(body);
    return -1;
}

/* Serve a memory buffer (e.g. generated bandinfo.js) as an HTTP response. */
static int serve_mem(struct lws *wsi, const void *data, size_t len, const char *mime) {
    unsigned char *buf = malloc(LWS_PRE + 1024 + len);
    if (!buf) return -1;
    unsigned char *p = buf + LWS_PRE;
    unsigned char *end = buf + LWS_PRE + 1024;

    if (lws_add_http_header_status(wsi, HTTP_STATUS_OK, &p, end)) goto fail;
    if (lws_add_http_header_by_name(wsi, (const unsigned char *)"content-type:",
                                    (const unsigned char *)mime, strlen(mime), &p, end)) goto fail;
    if (lws_add_http_header_content_length(wsi, len, &p, end)) goto fail;
    if (lws_finalize_http_header(wsi, &p, end)) goto fail;

    size_t hdr_len = (size_t)(p - (buf + LWS_PRE));
    if (lws_write(wsi, buf + LWS_PRE, hdr_len, LWS_WRITE_HTTP_HEADERS) < 0) goto fail;
    memcpy(buf + LWS_PRE, data, len);
    if (lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_HTTP) < 0) { free(buf); return -1; }
    free(buf);
    return lws_http_transaction_completed(wsi) ? -1 : 0;
fail:
    free(buf);
    return -1;
}

/* ------------------------------------------------------------------ */
/* lws callbacks                                                       */
/* ------------------------------------------------------------------ */

extern volatile int g_running;

/* Send the format-9 control frames to a client on subscribe:
 *  - a width reset (0xFF 0x02 width[2LE]) which clears the client's row
 *    buffer (so its previous-row state becomes all zeros), and
 *  - a position frame (0xFF 0x01 zoom start[4LE]). */
void client_send_waterfall_control(struct client *cli) {
    uint8_t wf[4];
    wf[0] = 0xFF; wf[1] = 0x02;
    wf[2] = (uint8_t)(WATERFALL_WIDTH & 0xFF);
    wf[3] = (uint8_t)((WATERFALL_WIDTH >> 8) & 0xFF);
    client_enqueue(cli, wf, 4);
    if (cli->wsi) lws_callback_on_writable(cli->wsi);

    uint8_t ctrl[8];
    ctrl[0] = 0xFF;
    ctrl[1] = 0x01;
    ctrl[2] = (uint8_t)(cli->zoom & 0x7F);
    int32_t s = cli->start;
    ctrl[3] = (uint8_t)(s & 0xFF);
    ctrl[4] = (uint8_t)((s >> 8) & 0xFF);
    ctrl[5] = (uint8_t)((s >> 16) & 0xFF);
    ctrl[6] = (uint8_t)((s >> 24) & 0xFF);
    ctrl[7] = 0x00;
    client_enqueue(cli, ctrl, 8);
    if (cli->wsi) lws_callback_on_writable(cli->wsi);
}

static int ws_handler(struct lws *wsi, enum lws_callback_reasons reason,
                      void *user, void *in, size_t len) {
    struct client *cli = (struct client *)user;

    switch (reason) {
    /* HTTP serving */
    case LWS_CALLBACK_HTTP: {
        char uri[512];
        lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI);
        if (strlen(uri) == 0 || strcmp(uri, "/") == 0)
            strcpy(uri, "/index.html");
        if (strstr(uri, "/tmp/bandinfo.js"))
            return serve_mem(wsi, g_bandinfo, (size_t)g_bandinfo_len,
                             "application/javascript");
        return serve_file(wsi, "pub", uri);
    }

    /* WebSocket upgrade / messages */
    case LWS_CALLBACK_ESTABLISHED:
        fprintf(stderr, "[ws] client connected\n");
        memset(cli, 0, sizeof(*cli));
        pthread_mutex_init(&cli->out_mutex, NULL);
        cli->wsi = wsi;
        cli->fd = lws_get_socket_fd(wsi);
        cli->band = NULL;
        cli->waterfall_active = false;
        /* distinguish audio (/~~stream) from waterfall (/~~waterstreamN) */
        {
            char uri[256];
            lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI);
            cli->audio_stream = (strstr(uri, "/~~stream") == uri);
            fprintf(stderr, "[ws] path=%s audio=%d\n", uri, cli->audio_stream ? 1 : 0);
        }
        break;

    case LWS_CALLBACK_SERVER_WRITEABLE:
        client_flush(wsi, cli);
        break;

    case LWS_CALLBACK_RECEIVE:
        if (in && len > 0)
            protocol_handle_message(cli, in, len);
        break;

    case LWS_CALLBACK_CLOSED:
        fprintf(stderr, "[ws] client disconnected\n");
        client_remove_from_band(cli);
        client_free_outq(cli);
        pthread_mutex_destroy(&cli->out_mutex);
        cli->wsi = NULL;
        break;

    default:
        break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    /* The WebSDR client connects to ws://host/~~... paths but requests no
     * subprotocol, so a single default protocol must serve both plain HTTP
     * (LWS_CALLBACK_HTTP) and websocket upgrade/messages. */
    { "websdr", ws_handler, sizeof(struct client), 8192, 0, NULL, 0 },
    { NULL, NULL, 0, 0 }
};

int server_start(struct websdr_config *config) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = config->tcpport;
    info.iface = NULL;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = 0;
    info.extensions = NULL;

    struct lws_context *ctx = lws_create_context(&info);
    if (!ctx) {
        fprintf(stderr, "Failed to create WebSocket context\n");
        return -1;
    }
    g_lws_ctx = ctx;

    /* Generate bandinfo.js + scale tiles from the loaded bands. One shared
     * timestamp ties bandinfo.js scale paths to the written PNG files. */
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", (long)time(NULL));
    scale_generate_all("pub", ts, config);
    g_bandinfo_len = bandinfo_build(g_bandinfo, BANDINFO_CAP, config, ts);
    if (g_bandinfo_len < 0) {
        fprintf(stderr, "bandinfo_build failed\n");
        g_bandinfo_len = 0;
    }

    fprintf(stderr, "HTTP/WebSocket server on port %d\n", config->tcpport);

    while (g_running)
        lws_service(ctx, 50);

    lws_context_destroy(ctx);
    return 0;
}
