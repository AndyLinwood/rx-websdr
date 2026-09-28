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
#include <strings.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libwebsockets.h>

#include "websdr.h"
#include "registry.h"

extern struct websdr_config *g_config;

extern volatile int g_running;
extern volatile int g_reload;

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
        if (cl && cl->audio_active) n++;

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
static unsigned g_chat_seq = 0;    /* highest chat sequence number issued */

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
    /* Chat cursor: the client echoes back the chatseq value we last sent it
     * (the number of lines in the chat file). It must NOT share the stats
     * chseq, which increments on every poll and quickly overtakes the chat
     * line count, starving chat_emit() below. */
    unsigned client_chatseq = 0;
    al = lws_get_urlarg_by_name_safe(wsi, "chatseq", arg, (int)sizeof(arg));
    if (al > 0) client_chatseq = (unsigned)atoi(arg);
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
        n = chat_emit(wsi, client_chatseq, body, n, (int)sizeof(body));
    if (n < (int)sizeof(body) - 32)
        n += snprintf(body + n, sizeof(body) - n, "chatseq=%u;\n", g_chat_seq);

    return serve_mem(wsi, body, (size_t)n, "text/javascript");
}

/* ------------------------------------------------------------------ */
/* /~~ft8 — FT8 decoder output ("who decoded what on FT8")            */
/* ------------------------------------------------------------------ */
/* The FT8 decode daemon (ka9q-radio ft8-decode@1) appends one line per
 * decoded message to /var/log/ft8.log, e.g.:
 *     2026/09/18 09:07:15  14 +1.93 7,074,721.8 ~ HA1BF LA1RQ JP51
 * This handler hands the LAST MAX_FT8_LINES lines to the client as a JS
 * snippet (eval'd by the client, same pattern as /~~othersjj):
 *     ft8chseq=<seq>;
 *     ft8str("<escaped line>");   ... newest last
 * The client keeps its own window (ring) of decoded lines and re-renders
 * the "FT8" panel whenever the chseq moves on. */

#define MAX_FT8_LINES 60

static unsigned g_ft8_chseq = 0;

static int serve_ft8(struct lws *wsi) {
    static char body[16384];
    static char path[128];
    int n = 0;

    snprintf(path, sizeof(path), "/var/log/ft8.log");

    /* Read the last MAX_FT8_LINES non-empty lines of the log. */
    FILE *fp = fopen(path, "r");
    if (!fp) {
        n += snprintf(body + n, sizeof(body) - n,
                      "ft8chseq=%u;\nft8str(\"[ft8.log not readable]\");\n", ++g_ft8_chseq);
        return serve_mem(wsi, body, (size_t)n, "text/javascript");
    }

    /* ring buffer of lines */
    static char lines[MAX_FT8_LINES][256];
    int nlines = 0;
    char tmp[300];
    while (fgets(tmp, (int)sizeof(tmp), fp)) {
        if (tmp[strlen(tmp)-1] == '\n') tmp[strlen(tmp)-1] = 0;
        if (tmp[strlen(tmp)-1] == '\r') tmp[strlen(tmp)-1] = 0;
        if (strlen(tmp) == 0) continue;
        if (nlines < MAX_FT8_LINES) {
            strcpy(lines[nlines], tmp);
            nlines++;
        } else {
            memmove(lines, lines + 1, (MAX_FT8_LINES - 1) * sizeof(lines[0]));
            strcpy(lines[nlines - 1], tmp);
        }
    }
    fclose(fp);

    n += snprintf(body + n, sizeof(body) - n, "ft8chseq=%u;\n", ++g_ft8_chseq);
    for (int i = 0; i < nlines && n < (int)sizeof(body) - 128; i++) {
        /* escape for JS single-quoted string */
        char esc[512];
        int e = 0;
        for (int c = 0; lines[i][c] && e < (int)sizeof(esc) - 2; c++) {
            if (lines[i][c] == '"' || lines[i][c] == '\\') esc[e++] = '\\';
            esc[e++] = lines[i][c];
        }
        esc[e] = 0;
        n += snprintf(body + n, sizeof(body) - n, "ft8str(\"%s\");\n", esc);
    }

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

static int g_chat_seq_init = 0;

/* Highest chat sequence number seen so far (read from the file once). The
 * seq is a monotonic id written as the first tab-field of each chat record
 * (seq\tepoch\tname\tmsg). Manual deletion of rows (spam cleanup) shifts
 * line numbers but not seqs, so a client cursor stays valid across edits.
 * We only write seqs that are higher than anything previously seen. */
static void chat_init_rows(void) {
    if (g_chat_seq_init || !g_config) return;
    g_chat_seq_init = 1;
    FILE *fp = fopen(g_config->chatfile, "r");
    if (!fp) return;
    char line[600];
    unsigned maxseq = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned seq = (unsigned)strtoul(line, NULL, 10);
        if (seq > maxseq) maxseq = seq;
    }
    fclose(fp);
    g_chat_seq = maxseq;
}


/* ------------------------------------------------------------------ */
/* Admin: /~~admin — moderate the chat (delete msgs, ban by callsign
 * and by IP). Protected by a password from cfg "adminpass"; a successful
 * login sets a session cookie (adminses) whose value is a simple hash of
 * the password — good enough for a hobby server, no external deps.      */
/* ------------------------------------------------------------------ */

/* Ban list: one entry per line — either a callsign (exact match, case
 * insensitive) or an IP/CIDR. Loaded once at startup and re-read on each
 * ban/unban; checked in chat_append before a message is stored. */
#define BANLIST_MAX 256
static char g_banlist[BANLIST_MAX][128];
static int  g_bann = 0;

static void banlist_load(void) {
    if (!g_config || g_config->banfile[0] == 0) return;
    g_bann = 0;
    FILE *fp = fopen(g_config->banfile, "r");
    if (!fp) return;
    char line[160];
    while (g_bann < BANLIST_MAX && fgets(line, sizeof(line), fp)) {
        char *e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        strncpy(g_banlist[g_bann], line, sizeof(g_banlist[g_bann]) - 1);
        g_banlist[g_bann][sizeof(g_banlist[g_bann]) - 1] = 0;
        g_bann++;
    }
    fclose(fp);
}

/* Real client IP from a raw wsi (used by admin/chat paths outside a
 * struct client): X-Forwarded-For first entry (caddy) or socket peer. */
static void client_ip_from_wsi(struct lws *wsi, char *out, size_t outsz) {
    out[0] = 0;
    if (!wsi) return;
    char xff[256] = "";
    if (lws_hdr_copy(wsi, xff, sizeof(xff), WSI_TOKEN_X_FORWARDED_FOR) > 0) {
        char *comma = strchr(xff, ',');
        if (comma) *comma = 0;
        char *sp = xff;
        while (*sp == ' ') sp++;
        char *e = sp + strlen(sp);
        while (e > sp && e[-1] == ' ') *--e = 0;
        if (sp[0]) { strncpy(out, sp, outsz - 1); out[outsz - 1] = 0; return; }
    }
    lws_get_peer_simple(wsi, out, outsz);
}

/* CIDR match: "a.b.c.d/len" against dotted ip. */
static int cidr_match(const char *cidr, const char *ip) {
    unsigned a[4] = {0}, b[4] = {0};
    int plen = 32;
    char buf[64];
    strncpy(buf, cidr, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
    char *slash = strchr(buf, '/');
    if (slash) { *slash = 0; plen = atoi(slash + 1); }
    if (sscanf(buf, "%u.%u.%u.%u", &a[0], &a[1], &a[2], &a[3]) != 4) return 0;
    if (sscanf(ip, "%u.%u.%u.%u", &b[0], &b[1], &b[2], &b[3]) != 4) return 0;
    if (plen < 0) plen = 0; if (plen > 32) plen = 32;
    unsigned mask = plen ? (0xFFFFFFFFu << (32 - plen)) : 0;
    unsigned av = (a[0]<<24)|(a[1]<<16)|(a[2]<<8)|a[3];
    unsigned bv = (b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3];
    return (av & mask) == (bv & mask);
}

static int is_banned(const char *name, const char *ip) {
    if (g_bann <= 0) return 0;
    char nlow[128];
    int k = 0;
    for (int i = 0; name && name[i] && k < (int)sizeof(nlow) - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        nlow[k++] = c;
    }
    nlow[k] = 0;
    for (int i = 0; i < g_bann; i++) {
        if (!g_banlist[i][0]) continue;
        if (strchr(g_banlist[i], '/')) {
            if (ip && cidr_match(g_banlist[i], ip)) return 1;
        } else if (strchr(g_banlist[i], '.')) {
            if (ip && strcmp(g_banlist[i], ip) == 0) return 1;
        } else {
            /* callsign — case-insensitive exact */
            char blow[128];
            int m = 0;
            for (int j = 0; g_banlist[i][j] && m < (int)sizeof(blow) - 1; j++) {
                char c = g_banlist[i][j];
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
                blow[m++] = c;
            }
            blow[m] = 0;
            if (nlow[0] && strcmp(nlow, blow) == 0) return 1;
        }
    }
    return 0;
}

static void banlist_add(const char *entry) {
    if (!entry || !entry[0]) return;
    /* avoid duplicates */
    for (int i = 0; i < g_bann; i++)
        if (strcasecmp(g_banlist[i], entry) == 0) return;
    if (g_bann >= BANLIST_MAX) return;
    strncpy(g_banlist[g_bann], entry, sizeof(g_banlist[g_bann]) - 1);
    g_banlist[g_bann][sizeof(g_banlist[g_bann]) - 1] = 0;
    g_bann++;
    FILE *fp = g_config && g_config->banfile[0] ? fopen(g_config->banfile, "a") : NULL;
    if (fp) { fprintf(fp, "%s\n", entry); fclose(fp); }
}

static void banlist_remove(const char *entry) {
    int removed = 0;
    for (int i = 0; i < g_bann; i++) {
        if (strcasecmp(g_banlist[i], entry) == 0) {
            g_banlist[i][0] = 0; removed++;
        }
    }
    if (!removed || !g_config || g_config->banfile[0] == 0) return;
    FILE *fp = fopen(g_config->banfile, "w");
    if (fp) {
        for (int i = 0; i < g_bann; i++)
            if (g_banlist[i][0]) fprintf(fp, "%s\n", g_banlist[i]);
        fclose(fp);
    }
}

/* Session cookie: adminses = hash(adminpass). */
static uint32_t admin_hash(const char *s) {
    uint32_t h = 5381;
    for (; s && *s; s++) h = h * 33 + (unsigned char)*s;
    return h;
}
static char g_admin_session[16];   /* hex of hash, set at config time */

static void admin_session_init(void) {
    if (g_admin_session[0]) return;
    if (!g_config || !g_config->adminpass[0]) return;
    snprintf(g_admin_session, sizeof(g_admin_session), "%08x", admin_hash(g_config->adminpass));
}

static int admin_ok(struct lws *wsi) {
    if (!g_config || !g_config->adminpass[0]) return 0;
    admin_session_init();
    char cookie[256] = "";
    if (lws_hdr_copy(wsi, cookie, sizeof(cookie), WSI_TOKEN_HTTP_COOKIE) <= 0) return 0;
    char ses[24] = "";
    /* parse adminses=... from cookie */
    char *p = strstr(cookie, "adminses=");
    if (!p) return 0;
    p += 9;
    int i = 0;
    while (p[i] && p[i] != ';' && p[i] != ' ' && i < (int)sizeof(ses) - 1) { ses[i] = p[i]; i++; }
    ses[i] = 0;
    return strcmp(ses, g_admin_session) == 0;
}

static void chat_append(struct lws *wsi, const char *name, const char *msg) {
    if (!g_config || !g_config->chat) return;
    if (g_bann == 0) banlist_load();
    char ip[64] = "";
    client_ip_from_wsi(wsi, ip, sizeof(ip));
    if (is_banned(name, ip)) return;   /* banned: drop silently */
    chat_init_rows();

    char line[600];
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

    unsigned seq = ++g_chat_seq;
    snprintf(line, sizeof(line), "%u\t%s\t%s\t%s\n", seq, tbuf, nbuf, mbuf);

    FILE *fp = fopen(g_config->chatfile, "a");
    if (fp) {
        fputs(line, fp);
        fclose(fp);
        (void)wsi;       /* response body is empty; 200 below */
    }
}

/* Render chatnewline('...'); statements for records whose seq is newer than
 * the cursor the client echoed back (client_chseq). Returns the new total
 * body length. */
static int chat_emit(struct lws *wsi, unsigned client_chseq,
                     char *body, int n, int cap) {
    if (!g_config || !g_config->chat) return n;
    chat_init_rows();
    if (client_chseq >= g_chat_seq) return n;   /* nothing new for this client */

    FILE *fp = fopen(g_config->chatfile, "r");
    if (!fp) return n;

    char line[600];
    while (fgets(line, sizeof(line), fp)) {
        /* line: <seq>\t<epoch>\t<name>\t<message>\n */
        char *f1 = strchr(line, '\t');
        if (!f1) continue;
        unsigned seq = (unsigned)strtoul(line, NULL, 10);
        if (seq <= client_chseq) continue;   /* already seen */

        char *t2 = strchr(f1 + 1, '\t');
        if (!t2) continue;
        char *t3 = strchr(t2 + 1, '\t');
        if (!t3) continue;
        char *name = t2 + 1;
        char *msg  = t3 + 1;
        *t3 = 0;                 /* terminate name at the third tab */
        /* trim trailing newline/CR from msg */
        char *e = msg + strlen(msg);
        while (e > msg && (e[-1] == '\n' || e[-1] == '\r')) *--e = 0;

        /* Chat line time stamp: epoch was the second tab-separated field.
         * Render as [HH:MM] in local server time. */
        char ts[8];
        long epoch = atol(f1 + 1);
        if (epoch > 0) {
            struct tm tmv;
            localtime_r(&epoch, &tmv);
            strftime(ts, sizeof(ts), "%H:%M", &tmv);
        } else {
            strncpy(ts, "??:??", sizeof(ts) - 1);
            ts[sizeof(ts) - 1] = 0;
        }

        if (n + 96 >= cap) break;   /* body full — give what we have */

        /* HTML-escape for the JS string + single quotes for the JS literal */
        char esc[560];
        int j = 0;
        esc[j++] = '['; esc[j++] = ts[0]; esc[j++] = ts[1];
        esc[j++] = ':'; esc[j++] = ts[3]; esc[j++] = ts[4];
        esc[j++] = ']'; esc[j++] = ' ';
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
/* /~~admin — chat moderation page                                     */
/* ------------------------------------------------------------------ */

/* Delete a chat record by its seq: rewrite chatfile without that line.
 * seqs are never reused, so client cursors stay valid. Returns 1 if found. */
static int chat_delete_by_seq(unsigned seq) {
    if (!g_config) return 0;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_config->chatfile);
    FILE *in = fopen(g_config->chatfile, "r");
    if (!in) return 0;
    FILE *out = fopen(tmp, "w");
    if (!out) { fclose(in); return 0; }
    char line[600];
    int found = 0;
    while (fgets(line, sizeof(line), in)) {
        unsigned s = (unsigned)strtoul(line, NULL, 10);
        if (s == seq) { found = 1; continue; }
        fputs(line, out);
    }
    fclose(in); fclose(out);
    if (found) rename(tmp, g_config->chatfile);
    else remove(tmp);
    return found;
}


/* Build the admin page HTML (chat tail + controls). Returns length. */
/* Render just the chat table (used by AJAX refresh and full page). */
static int admin_build_table(char *buf, size_t cap) {
    int n = 0;
    n += snprintf(buf + n, cap - n,
        "<table><tr><th>seq</th><th>время</th><th>имя</th><th>сообщение</th><th></th></tr>");
    FILE *fp = g_config ? fopen(g_config->chatfile, "r") : NULL;
    if (fp) {
        char lines[100][600];
        int cnt = 0;
        char line[600];
        while (fgets(line, sizeof(line), fp)) {
            if (cnt == 100) {
                memmove(lines[0], lines[1], sizeof(lines[0]) * 99);
                cnt = 99;
            }
            strncpy(lines[cnt], line, sizeof(lines[cnt]) - 1);
            lines[cnt][sizeof(lines[cnt]) - 1] = 0;
            cnt++;
        }
        fclose(fp);
        for (int i = 0; i < cnt; i++) {
            char *f1 = strchr(lines[i], '\t');
            if (!f1) continue;
            unsigned seq = (unsigned)strtoul(lines[i], NULL, 10);
            char *t2 = strchr(f1 + 1, '\t');
            if (!t2) continue;
            char *t3 = strchr(t2 + 1, '\t');
            if (!t3) continue;
            *t3 = 0;
            char *name = t2 + 1;
            char *msg = t3 + 1;
            char *e2 = msg + strlen(msg);
            while (e2 > msg && (e2[-1]=='\n'||e2[-1]=='\r')) *--e2 = 0;
            char ts[8] = "??:??";
            long epoch = atol(f1 + 1);
            if (epoch > 0) {
                struct tm tmv; time_t tt = (time_t)epoch;
                localtime_r(&tt, &tmv);
                strftime(ts, sizeof(ts), "%H:%M", &tmv);
            }
            n += snprintf(buf + n, cap - n,
                "<tr><td>%u</td><td>%s</td>"
                "<td><a href='javascript:ban(\"%s\")' title='забанить'>%s</a></td>"
                "<td>%s</td>"
                "<td><a class=del href='/?admin=del&seq=%u'>[удалить]</a></td></tr>",
                seq, ts, name, name, msg, seq);
        }
    }
    n += snprintf(buf + n, cap - n, "</table>");
    return n;
}

static int admin_build_html(char *buf, size_t cap) {
    int n = 0;
    const char *css =
        "<style>body{font:14px system-ui;margin:20px;background:#111;color:#ddd}"
        "table{border-collapse:collapse;width:100%}"
        "td,th{border:1px solid #333;padding:4px 8px;text-align:left}"
        ".del{color:#f66}a{color:#7af}</style>";
    n += snprintf(buf + n, cap - n,
        "<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>"
        "<title>Чат — админ</title>"
        "%s</head><body>"
        "<h2>Админ чата</h2>"
        "<p><a href='/?'>На сайт</a> | <a href='/?admin=logout'>Выйти</a> | "
        "<button onclick='refresh()'>Обновить</button></p>"
        "<script>"
        "function ban(v){var i=document.getElementById('be');i.value=v;i.focus();}"
        "function refresh(){"
        "fetch('?admin=raw').then(function(r){return r.text();}).then(function(t){"
        "document.getElementById('chatfeed').innerHTML=t;});}"
        "setInterval(refresh,5000);"
        "refresh();"
        "</script>",
        css);

    if (g_bann > 0) {
        n += snprintf(buf + n, cap - n, "<h3>В бане:</h3><ul>");
        for (int i = 0; i < g_bann; i++)
            if (g_banlist[i][0])
                n += snprintf(buf + n, cap - n,
                    "<li>%s <a class=del href='/?admin=unban&e=%s'>[разбанить]</a></li>",
                    g_banlist[i], g_banlist[i]);
        n += snprintf(buf + n, cap - n, "</ul>");
    } else {
        n += snprintf(buf + n, cap - n, "<h3>В бане: пусто</h3>");
    }

    n += snprintf(buf + n, cap - n,
        "<h3>Забанить:</h3><form method=get>"
        "<input type=hidden name=admin value=ban><input id=be name=e placeholder='позывной или IP/CIDR'>"
        "<button>OK</button></form>");

    n += snprintf(buf + n, cap - n,
        "<h3>Чат (последние 100):</h3><div id=chatfeed>");
    n += admin_build_table(buf + n, (size_t)(cap - n));
n += snprintf(buf + n, cap - n, "</div></body></html>");
    return n;
}


/* Login prompt — password check happens in serve_admin (admin=login&p=..);
 * this static form just submits to it. */
static int serve_admin_login(struct lws *wsi) {
    const char *html =
        "<!DOCTYPE html><html lang=ru><head><meta charset=utf-8>"
        "<title>Админ — вход</title></head>"
        "<body style='font:14px system-ui;background:#111;color:#ddd;margin:40px'>"
        "<h2>Вход для администратора</h2>"
        "<form method=get>"
        "<input type=hidden name=admin value=login>"
        "<input type=password name=p placeholder='пароль' autofocus>"
        "<button>Войти</button></form>"
        "</body></html>";
    return serve_mem(wsi, html, strlen(html), "text/html");
}

/* Entry: auth, act, render the admin page. Query args are parsed with
 * lws_get_urlarg_by_name_safe (action=p/login/del/ban/unban/clear...). */
static int serve_admin(struct lws *wsi) {
    if (!g_config || !g_config->adminpass[0])
        return serve_mem(wsi, "admin disabled", 14, "text/plain");
    admin_session_init();

    char action[16] = "";
    char val[128] = "";
    char arg[32];
    if (lws_get_urlarg_by_name_safe(wsi, "admin", arg, (int)sizeof(arg)) > 0)
        strncpy(action, arg, sizeof(action) - 1);
    if (lws_get_urlarg_by_name_safe(wsi, "p", val, (int)sizeof(val)) <= 0)
        val[0] = 0;
    char entry[128] = "";
    if (lws_get_urlarg_by_name_safe(wsi, "e", entry, (int)sizeof(entry)) <= 0)
        entry[0] = 0;
    unsigned delseq = 0;
    char seqarg[16];
    if (lws_get_urlarg_by_name_safe(wsi, "seq", seqarg, (int)sizeof(seqarg)) > 0)
        delseq = (unsigned)atoi(seqarg);

    /* login */
    if (strcmp(action, "login") == 0 && val[0]) {
        if (strcmp(val, g_config->adminpass) == 0) {
            char page[256];
            snprintf(page, sizeof(page),
                "<script>document.cookie='adminses=%s; Path=/';location='?admin';</script>",
                g_admin_session);
            return serve_mem(wsi, page, strlen(page), "text/html");
        }
        return serve_admin_login(wsi);
    }

    /* everything else requires a valid session cookie */
    if (!admin_ok(wsi)) return serve_admin_login(wsi);

    if (strcmp(action, "logout") == 0) {
        const char *page =
            "<script>document.cookie='adminses=; Path=/; max-age=0';"
            "location='?admin';</script>";
        return serve_mem(wsi, page, strlen(page), "text/html");
    }

    if (strcmp(action, "del") == 0 && delseq)
        chat_delete_by_seq(delseq);
    else if (strcmp(action, "ban") == 0 && entry[0]) {
        banlist_add(entry);
        banlist_load();
    }
    else if (strcmp(action, "unban") == 0 && entry[0]) {
        banlist_remove(entry);
        banlist_load();
    }
    else if (strcmp(action, "clear") == 0) {
        FILE *fp = g_config->chatfile[0] ? fopen(g_config->chatfile, "w") : NULL;
        if (fp) fclose(fp);
    }

    if (strcmp(action, "raw") == 0) {
        /* AJAX refresh: just the chat table body (no full page). */
        char html[65536];
        int n = admin_build_table(html, sizeof(html));
        return serve_mem(wsi, html, (size_t)n, "text/html");
    }

    char html[65536];
    int n = admin_build_html(html, sizeof(html));
    return serve_mem(wsi, html, (size_t)n, "text/html");
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
        if (strncmp(uri, "/~~ft8", 6) == 0)
            return serve_ft8(wsi);
        if (strncmp(uri, "/~~admin", 7) == 0) {
            /* /~~admin?admin=... — chat moderation (parses args itself). */
            return serve_admin(wsi);
        }
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
        /* Real client IP: behind caddy every peer is 127.0.0.1, so prefer
         * X-Forwarded-For (first entry) added by the proxy; fall back to the
         * socket peer for direct connections. */
        {
            char xff[256] = "";
            char *src = NULL;
            if (lws_hdr_copy(wsi, xff, sizeof(xff), WSI_TOKEN_X_FORWARDED_FOR) > 0)
                src = xff;
            if (src) {
                char *comma = strchr(src, ',');
                if (comma) *comma = 0;
                while (*src == ' ') src++;
                char *e = src + strlen(src);
                while (e > src && e[-1] == ' ') *--e = 0;
            }
            if (src && src[0])
                strncpy(cli->client_ip_str, src, sizeof(cli->client_ip_str) - 1);
            else
                lws_get_peer_simple(wsi, cli->client_ip_str, sizeof(cli->client_ip_str));
            cli->client_ip_str[sizeof(cli->client_ip_str) - 1] = 0;
        }
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

/* Подсчёт клиентов для registry (тот же метод, что и для веб-интерфейса) */
int server_get_total_clients(void) {
    int n = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *cl = g_uu_slots[i];
        if (cl && cl->audio_active) n++;
    }
    return n;
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
    while (g_running) {
        lws_service(ctx, 20);
        if (g_reload) {
            g_reload = 0;
            fprintf(stderr, "[reload] SIGHUP received, hot-reloading cfg...\n");
            if (config_reload_hot(g_config) == 0) {
                /* Registry перечитывается отдельно (registry_init сам читает
                 * cfg/websdr.cfg) — перезапускаем тред heartbeat. */
                registry_stop();
                registry_init(g_config, g_config_file);
                registry_start();
                fprintf(stderr, "[reload] done\n");
            }
        }
    }

    pthread_cancel(pacer);
    pthread_join(pacer, NULL);
    pthread_cancel(stats);
    pthread_join(stats, NULL);

    lws_context_destroy(ctx);
    return 0;
}
