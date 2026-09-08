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
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libwebsockets.h>

#include "websdr.h"

extern struct websdr_config *g_config;

extern volatile int g_running;

/* Stable per-connection slot for the /~~othersjj users list, and the
 * per-poll sequence counter (client starts from undefined -> first request
 * compares against the cfg chseq). Both are used only by the lws service
 * thread, except stats_update() (stats thread) reading g_chseq. */
static struct client *g_uu_slots[MAX_CLIENTS];
static unsigned g_chseq = 3;

/* ------------------------------------------------------------------ */
/* Server statistics (the client's "Statistics:" box)                 */
/* ------------------------------------------------------------------ */
/* The unmodified client polls /~~othersjj every second and eval()s the
 * response; the original websdr64 piggybacks one extra statement onto it:
 *     statsobj.innerHTML="Past 10 seconds: CPUload=%.1f%%, %.2f users;
 *     audio %.1f kb/s, waterfall %.1f kb/s, http %.1f kb/s";
 * which fills <div id="stats"> under "Statistics:". Stats are refreshed
 * every 10 s by the stats thread and sent only to clients whose chseq
 * predates the last refresh (they catch up on the next poll). */
static struct {
    long long audio_bytes, wf_bytes, http_bytes;  /* window byte counters */
    double    user_integral;                      /* users * us in window */
    int       last_nusers;
    long long last_wall_us;
    long long last_ru_utime_us, last_ru_stime_us;
    float     cpu_pct, avg_users, audio_kbps, wf_kbps, http_kbps;
    unsigned  stats_chseq;
} g_stats;

static long long stats_now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static int stats_nusers(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
	struct client *cl = g_uu_slots[i];
        if (cl && cl->wsi) n++;

    }
    return n;
}

static void stats_update(void) {
    long long now = stats_now_us();

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    long long utime = (long long)ru.ru_utime.tv_sec * 1000000LL + ru.ru_utime.tv_usec;
    long long stime = (long long)ru.ru_stime.tv_sec * 1000000LL + ru.ru_stime.tv_usec;

    if (g_stats.last_wall_us == 0) {  /* first tick: baseline only */
        g_stats.last_wall_us = now;
        g_stats.last_ru_utime_us = utime;
        g_stats.last_ru_stime_us = stime;
        g_stats.last_nusers = stats_nusers();
        g_stats.stats_chseq = g_chseq;
        return;
    }

    long long wall = now - g_stats.last_wall_us;
    if (wall < 1) wall = 1;

    g_stats.user_integral += (double)g_stats.last_nusers * (double)wall;

    g_stats.cpu_pct = (float)(((double)(utime - g_stats.last_ru_utime_us) +
                               (double)(stime - g_stats.last_ru_stime_us)) /
                              (double)wall * 100.0);

    double secs = (double)wall / 1e6;
    if (secs < 0.1) secs = 0.1;
    g_stats.audio_kbps = (float)((double)g_stats.audio_bytes * 8.0 / 1000.0 / secs);
    g_stats.wf_kbps    = (float)((double)g_stats.wf_bytes    * 8.0 / 1000.0 / secs);
    g_stats.http_kbps  = (float)((double)g_stats.http_bytes  * 8.0 / 1000.0 / secs);

    g_stats.avg_users = (float)(g_stats.user_integral / (double)wall);

    g_stats.audio_bytes = g_stats.wf_bytes = g_stats.http_bytes = 0;
    g_stats.user_integral = 0.0;
    g_stats.last_wall_us = now;
    g_stats.last_ru_utime_us = utime;
    g_stats.last_ru_stime_us = stime;
    g_stats.last_nusers = stats_nusers();
    g_stats.stats_chseq = g_chseq;
}

static void *stats_thread(void *arg) {
    while (g_running) {
        struct timespec ts = { .tv_sec = 10, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        stats_update();
    }
    return NULL;
}

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
            break;
        }
    }
    pthread_mutex_unlock(&band->lock);
    cli->band = NULL;
}

/* Append bytes to a client's output ring queue. Returns 0 on success. */
void client_enqueue(struct client *cli, const uint8_t *data, size_t len) {
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

void client_free_outq(struct client *cli) {
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
        if (cli->audio_stream) g_stats.audio_bytes += n;
        else                   g_stats.wf_bytes    += n;
    }
}

/* Send waterfall data to all connected clients of a band. */
void band_send_waterfall(struct band *band) {
    uint8_t compressed[WATERFALL_WIDTH * 2 + 16];

    pthread_mutex_lock(&band->lock);

    /* Per-client genuine zoom: each client is on its own (zoom,start), so we
     * slice that client's sub-range out of the full-band FFT (power_hi, K bins
     * per min-zoom pixel) and encode it against that client's own prev_line.
     *
     * Geometry (matches websdr-base.js): the client sets
     *   start = (f − center + sr/2 − effsr/2) * 1024 / (sr/2^maxzoom)
     * i.e. start is the LEFT EDGE of the window, in MAXZOOM-pixels. Window
     * lower frequency = center − sr/2 + start*(sr/2^maxzoom)/1024 (Hz); in FFT
     * bins from center (bin = sr/FFT_SIZE):
     *     left_bin = start * (sr/2^maxzoom)/1024 * FFT_SIZE/sr
     *              = start * K / 2^maxzoom = (start * K) >> maxzoom
     * Pixel x (zoom z) spans bins [left_bin + x*(K>>z), +K>>z). */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *cli = band->clients[i];
        if (!cli || !cli->waterfall_active || !cli->wsi) continue;
        if (cli->audio_stream) continue;   /* this socket is audio only */

        int z = cli->zoom;
        if (z < 0) z = 0;
        if (z > band->maxzoom) z = band->maxzoom;
        const int K = FFT_SIZE / WATERFALL_WIDTH;   /* bins per min-zoom px */
        int step = K >> z;                          /* bins per zoom-z px */
        if (step < 1) step = 1;
        int left = (cli->start * K) >> band->maxzoom;

        /* Deep zooms (>=3, window <= ~48 kHz) use the sub-band FFT: true
         * bin-per-pixel resolution (websdr64 style) instead of magnifying
         * full-band bins ("magnifying glass" smears signals wide). */
        float *zoom_power = NULL;
        int zoom_pts = 0;
        if (z >= ZOOM_FFT_MIN_ZOOM) {
            if (band->zoom_fft) zoom_fft_activate(band, z, cli->start);
            zoom_power = zoom_fft_get_power(band, z);
            if (zoom_power) zoom_pts = band->zoom_fft ? 1024 : 0;
        }

        uint8_t row[WATERFALL_WIDTH];
        for (int x = 0; x < WATERFALL_WIDTH; x++) {
            float best = 0.0f;
            if (zoom_power) {
                /* sub-band spectrum: bin = 1 px, window = 1024 points.
                 * (start-offset handling to come; currently fixed window.) */
                if (x < zoom_pts)
                    best = zoom_power[x];
            } else {
                /* MAX downsampling of the full-band power_hi on low zooms. */
                int base = left + x * step;
                for (int j = 0; j < step; j++) {
                    int idx = base + j;
                    if (idx < 0) idx = 0;
                    if (idx >= FFT_SIZE) idx = FFT_SIZE - 1;
                    if (band->power_hi[idx] > best) best = band->power_hi[idx];
                }
            }
            row[x] = (uint8_t)wf_brightness(best, band->noise_dB, band->gain);
        }

        int len = compress_waterfall_format9(row, cli->prev_line,
                                             WATERFALL_WIDTH, compressed);
        if (len <= 0) continue;

        /* compress_waterfall_format9 updates cli->prev_line in place to the
         * row the client decoder will hold (decoder-true baseline), so the
         * next row's deltas compensate quantisation instead of drifting. */

        /* escape a data row that would begin with 0xFF */
        uint8_t rowbuf[WATERFALL_WIDTH * 2 + 16 + 1];
        int rowlen = len;
        if (compressed[0] == 0xFF) {
            rowbuf[0] = 0xFF;                  /* doubles as the escape */
            memcpy(rowbuf + 1, compressed, (size_t)len);
            rowlen = len + 1;
        } else {
            memcpy(rowbuf, compressed, (size_t)len);
        }
        client_enqueue(cli, rowbuf, (size_t)rowlen);
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
    g_stats.http_bytes += (long long)body_len;
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
    g_stats.http_bytes += (long long)len;
    free(buf);
    return lws_http_transaction_completed(wsi) ? -1 : 0;
fail:
    free(buf);
    return -1;
}

/* forward declarations (chat handlers defined below serve_othersjj) */
static int chat_emit(struct lws *wsi, unsigned client_chseq,
                     char *body, int n, int cap);
static void chat_append(struct lws *wsi, const char *name, const char *msg);

/* ------------------------------------------------------------------ */
/* /~~othersjj — "who is listening" list                              */
/* ------------------------------------------------------------------ */
/* The client polls this every second (ajaxFunction3) and eval()s the
 * response. Format (verified against the original websdr64 binary strings:
 * `chseq=%i;`, `uu(%i,'%s',%i,%f);`, `numusersobj.innerHTML="%i";`):
 *     chseq=<seq>;
 *     uu(<slot>,'<name>',<band_idx>,<freq_frac>);   ... one per audio user
 *     numusersobj.innerHTML="<count>";
 * freq_frac is the position within the band in 0..1 (client renders it on the
 * band scale: uu_freqs[i]*1024, and derives the kHz for the jump button).
 * Slots are stable per connection (allocated on ESTABLISHED, freed on CLOSED)
 * so a user's label keeps its colour/position; all callbacks run in the lws
 * service thread, so no locking is needed. */

static int serve_othersjj(struct lws *wsi) {
    static char body[65536];
    int n = 0;

    /* Gate the piggybacked statistics on the client's chseq: send them only
     * when the client has not yet seen the latest 10 s stats window. */
    unsigned client_chseq = 0;
    char arg[32];
    int al = lws_get_urlarg_by_name_safe(wsi, "chseq", arg, (int)sizeof(arg));
    if (al > 0) client_chseq = (unsigned)atoi(arg);
    if (client_chseq < g_stats.stats_chseq)
        n += snprintf(body + n, sizeof(body) - n,
            "statsobj.innerHTML=\"Past 10 seconds: CPUload=%.1f%%, %.2f users; "
            "audio %.1f kb/s, waterfall %.1f kb/s, http %.1f kb/s\";\n",
            g_stats.cpu_pct, g_stats.avg_users,
            g_stats.audio_kbps, g_stats.wf_kbps, g_stats.http_kbps);

    n += snprintf(body + n, sizeof(body) - n, "chseq=%u;\n", g_chseq++);

    int nusers = 0;
    for (int i = 0; i < MAX_CLIENTS && n < (int)sizeof(body) - 128; i++) {
        struct client *cl = g_uu_slots[i];
        if (!cl || !cl->audio_active || !cl->wsi) continue;

        int band_idx = 0;
        double freq = 0.5;
        struct band *b = cl->band;
        if (b && g_config) {
            for (int k = 0; k < g_config->nbands; k++) {
                if (&g_config->bands[k] == b) { band_idx = k; break; }
            }
            double bw_khz = (b->samplerate > 0) ? (double)b->samplerate / 1000.0 : 0.0;
            if (bw_khz > 0.0) {
                freq = (cl->freq - band_eff_center(b) + bw_khz * 0.5) / bw_khz;
                if (freq < 0.0) freq = 0.0;
                if (freq > 1.0) freq = 1.0;
            }
        }

        /* escape single quotes / backslashes so the eval'd JS stays valid */
        char esc[128];
        int e = 0;
        for (int c = 0; cl->username[c] && e < (int)sizeof(esc) - 2; c++) {
            if (cl->username[c] == '\'' || cl->username[c] == '\\') esc[e++] = '\\';
            esc[e++] = cl->username[c];
        }
        esc[e] = 0;

        n += snprintf(body + n, sizeof(body) - n, "uu(%d,'%s',%d,%f);\n",
                      i, esc, band_idx, freq);
        nusers++;
    }

    if (n < (int)sizeof(body) - 64)
        n += snprintf(body + n, sizeof(body) - n, "numusersobj.innerHTML=\"%d\";\n", nusers);

    /* Append new chat lines (if enabled) — see chat_emit() below. */
    if (g_config && g_config->chat)
        n = chat_emit(wsi, client_chseq, body, n, (int)sizeof(body));

    return serve_mem(wsi, body, (size_t)n, "text/javascript");
}

/* ------------------------------------------------------------------ */
/* /~~chat — simple chat box                                           */
/* ------------------------------------------------------------------ */
/* Client sends:  GET /~~chat?name=<callsign>&msg=<message>
 * (message is encodeURIComponent'ed). Each accepted message is appended,
 * one line per message, to the chat file (format: seq<TAB>seq_number is
 * implicit by line number) as:
 *     <epoch>\t<name>\t<message>\n
 * The line number (1-based, starting at 1) is the "sequence" used by
 * chat_emit() to hand each client only lines it has not seen yet, via the
 * same client_chseq mechanics as the statistics.
 *
 * The file itself is both the persistent store and the server-side order;
 * rotation is not needed for a hobby server (messages are tiny).
 * Escaping: name/message must not contain \n or \t (we strip them) so a line
 * stays one record; the JS-side rendering HTML-escapes on display. */

static unsigned g_chat_rows = 0;   /* how many lines are in the chat file */
static int      g_chat_rows_init = 0;

/* Count the lines in the chat file (once) so a server restart still hands
 * the whole history to clients that were already connected. */
static void chat_init_rows(void) {
    if (g_chat_rows_init || !g_config) return;
    g_chat_rows_init = 1;
    FILE *fp = fopen(g_config->chatfile, "r");
    if (!fp) return;
    int c;
    unsigned rows = 0;
    while ((c = fgetc(fp)) != EOF)
        if (c == '\n') rows++;
    fclose(fp);
    g_chat_rows = rows;
}

static void chat_append(struct lws *wsi, const char *name, const char *msg) {
    if (!g_config || !g_config->chat) return;
    chat_init_rows();

    char line[512];
    char tbuf[24];
    snprintf(tbuf, sizeof(tbuf), "%ld", (long)time(NULL));

    /* sanitize: no tabs or newlines in the record */
    char nbuf[96], mbuf[300];
    int i, j;
    for (i = 0, j = 0; name[i] && j < (int)sizeof(nbuf) - 2; i++) {
        char c = name[i];
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        nbuf[j++] = c;
    }
    nbuf[j] = 0;
    for (i = 0, j = 0; msg[i] && j < (int)sizeof(mbuf) - 2; i++) {
        char c = msg[i];
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        mbuf[j++] = c;
    }
    mbuf[j] = 0;

    snprintf(line, sizeof(line), "%s\t%s\t%s\n", tbuf, nbuf, mbuf);

    FILE *fp = fopen(g_config->chatfile, "a");
    if (fp) {
        fputs(line, fp);
        fclose(fp);
        g_chat_rows++;   /* this is what drives "have you seen it?" */
        (void)wsi;       /* response body is empty; 200 below */
    }
}

/* Render chatnewline('...'); statements for lines this client hasn't seen.
 * Returns the new total body length. */
static int chat_emit(struct lws *wsi, unsigned client_chseq,
                     char *body, int n, int cap) {
    if (!g_config || !g_config->chat) return n;
    chat_init_rows();
    if (client_chseq >= g_chat_rows) return n;   /* nothing new for this client */

    FILE *fp = fopen(g_config->chatfile, "r");
    if (!fp) return n;

    char line[600];
    unsigned lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (lineno <= client_chseq) continue;   /* already seen */

        /* line: <epoch>\t<name>\t<message>\n  (message may not contain \t) */
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        char *t2 = strchr(t1 + 1, '\t');
        if (!t2) continue;
        char *name = t1 + 1;
        char *msg  = t2 + 1;
        *t2 = 0;                 /* terminate name at the second tab */
        /* trim trailing newline/CR from msg */
        char *e = msg + strlen(msg);
        while (e > msg && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;

        if (n + 96 >= cap) break;   /* body full — give what we have */

        /* HTML-escape for the JS string + single quotes for the JS literal */
        char esc[560];
        int j = 0;
        for (int k = 0; name[k] && j < (int)sizeof(esc) - 4; k++) {
            char c = name[k];
            if (c == '\'') { esc[j++]='\\'; esc[j++]='\''; continue; }
            if (c == '\\') { esc[j++]='\\'; esc[j++]='\\'; continue; }
            if (c == '<') { esc[j++]='&'; esc[j++]='l'; esc[j++]='t'; esc[j++]=';'; continue; }
            if (c == '>') { esc[j++]='&'; esc[j++]='g'; esc[j++]='t'; esc[j++]=';'; continue; }
            if (c == '&') { esc[j++]='&'; esc[j++]='a'; esc[j++]='m'; esc[j++]='p'; esc[j++]=';'; continue; }
            esc[j++] = c;
        }
        esc[j++] = ':'; esc[j++] = ' ';
        for (int k = 0; msg[k] && j < (int)sizeof(esc) - 4; k++) {
            char c = msg[k];
            if (c == '\'') { esc[j++]='\\'; esc[j++]='\''; continue; }
            if (c == '\\') { esc[j++]='\\'; esc[j++]='\\'; continue; }
            if (c == '<') { esc[j++]='&'; esc[j++]='l'; esc[j++]='t'; esc[j++]=';'; continue; }
            if (c == '>') { esc[j++]='&'; esc[j++]='g'; esc[j++]='t'; esc[j++]=';'; continue; }
            if (c == '&') { esc[j++]='&'; esc[j++]='a'; esc[j++]='m'; esc[j++]='p'; esc[j++]=';'; continue; }
            esc[j++] = c;
        }
        esc[j] = 0;

        n += snprintf(body + n, (size_t)(cap - n),
                      "chatnewline('%s');\n", esc);
    }
    fclose(fp);
    return n;
}

/* ------------------------------------------------------------------ */
/* lws callbacks                                                       */
/* ------------------------------------------------------------------ */

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
        if (strncmp(uri, "/~~othersjj", 11) == 0)
            return serve_othersjj(wsi);
        if (strncmp(uri, "/~~chat", 6) == 0) {
            /* GET /~~chat?name=<callsign>&msg=<message> — append to chat file.
             * The real websdr also returns a 200 with empty JS body. */
            char an[96], am[320];
            int anl = lws_get_urlarg_by_name_safe(wsi, "name", an, (int)sizeof(an));
            int aml = lws_get_urlarg_by_name_safe(wsi, "msg",  am, (int)sizeof(am));
            if (anl > 0 && aml > 0)
                chat_append(wsi, an, am);
            return serve_mem(wsi, ";", 1, "text/javascript"); /* empty JS 200 */
        }
        return serve_file(wsi, "pub", uri);
    }

    /* WebSocket upgrade / messages */
    case LWS_CALLBACK_ESTABLISHED:
        memset(cli, 0, sizeof(*cli));
        pthread_mutex_init(&cli->out_mutex, NULL);
        pthread_mutex_init(&cli->audio_mutex, NULL);
        cli->wsi = wsi;
        cli->fd = lws_get_socket_fd(wsi);
        cli->band = NULL;
        cli->waterfall_active = false;
        /* stable slot for the /~~othersjj users list */
        cli->uu_index = -1;
        for (int si = 0; si < MAX_CLIENTS; si++) {
            if (!g_uu_slots[si]) { g_uu_slots[si] = cli; cli->uu_index = si; break; }
        }
        /* distinguish audio (/~~stream) from waterfall (/~~waterstreamN) */
        {
            char uri[256];
            lws_hdr_copy(wsi, uri, sizeof(uri), WSI_TOKEN_GET_URI);
            cli->audio_stream = (strstr(uri, "/~~stream") == uri);
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
        if (cli->uu_index >= 0 && g_uu_slots[cli->uu_index] == cli)
            g_uu_slots[cli->uu_index] = NULL;
        client_remove_from_band(cli);
        client_free_outq(cli);
        /* audio_free locks audio_mutex itself; it must run after
         * client_remove_from_band so the DSP thread no longer iterates this
         * client (otherwise audio_free would tear down the FIR arrays / FFT
         * plan under a concurrently-running demod). */
#if AUDIO_USE_FFT
        if (cli->audio.af_dplan) audio_free(cli);
#else
        if (cli->audio.fir) audio_free(cli);
#endif
        pthread_mutex_destroy(&cli->audio_mutex);
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

/* Steady audio delivery clock. PCM is produced in bursts by the band threads
 * (one large FIFO read per iter — up to ~170 ms on 192 kHz bands), so flushing
 * inside that loop made clients receive clumps then gaps; the client's drift
 * corrector then wobbles the pitch ("trembling" voices, worst on slow bands).
 * This thread flushes every ~16 ms on wall-clock, decoupled from FIFO reads;
 * audio_flush_pcm additionally paces emission to exactly AUDIO_RATE. */
void *audio_pacer_thread(void *arg) {
    while (g_running) {
        struct timespec ts;
        ts.tv_sec = 0;
        ts.tv_nsec = 16000000;              /* 16 ms -> 62.5 frames/s */
        nanosleep(&ts, NULL);

        if (!g_config) continue;
        for (int bi = 0; bi < g_config->nbands; bi++) {
            struct band *b = &g_config->bands[bi];
            pthread_mutex_lock(&b->lock);
            for (int ai = 0; ai < MAX_CLIENTS; ai++) {
                struct client *ac = b->clients[ai];
#if AUDIO_USE_FFT
                if (ac && ac->audio_stream && ac->audio_active && ac->audio.af_dplan) {
#else
                if (ac && ac->audio_stream && ac->audio_active && ac->audio.fir) {
#endif
                    audio_flush_pcm(ac);
                    if (ac->wsi) lws_callback_on_writable(ac->wsi);
                }
            }
            pthread_mutex_unlock(&b->lock);
        }
        if (g_lws_ctx) lws_cancel_service(g_lws_ctx);
    }
    return NULL;
}

int server_start(struct websdr_config *config) {
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));

    info.port = config->tcpport;
    info.iface = NULL;
    info.protocols = protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = 0;
    info.timeout_secs = 20;      /* kick idle/half-open connections promptly */
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

    /* Decouple audio delivery from the bursty FIFO read loop. */
    pthread_t pacer, stats;
    pthread_create(&pacer, NULL, audio_pacer_thread, NULL);
    pthread_create(&stats, NULL, stats_thread, NULL);

    /* Small poll timeout keeps audio/waterfall queued by the band thread
     * delivered promptly with minimal latency/jitter. */
    while (g_running)
        lws_service(ctx, 20);

    pthread_cancel(pacer);
    pthread_join(pacer, NULL);
    pthread_cancel(stats);
    pthread_join(stats, NULL);

    lws_context_destroy(ctx);
    return 0;
}
