/*
 * WebSDR Server — WebSocket protocol handler
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "websdr.h"

extern struct websdr_config *g_config;

static void handle_waterparam(struct client *cli, const char *params) {
    char *band_name = NULL;
    int zoom = 0, start = 0, width = 0, slow = 0;

    char *tmp = strdup(params);
    char *token = strtok(tmp, "&");
    while (token) {
        if (strncmp(token, "band=", 5) == 0)
            band_name = strdup(token + 5);
        else if (strncmp(token, "zoom=", 5) == 0)
            zoom = atoi(token + 5);
        else if (strncmp(token, "start=", 6) == 0)
            start = atoi(token + 6);
        else if (strncmp(token, "width=", 6) == 0)
            width = atoi(token + 6);
        else if (strncmp(token, "slow=", 5) == 0)
            slow = atoi(token + 5);
        token = strtok(NULL, "&");
    }
    (void)width;
    (void)slow;

    if (band_name && g_config) {
        client_remove_from_band(cli);

        /* The client (websdr-base.js) uses the band INDEX, not the name:
         *   var band=0;  ...  ws://host/~~waterstream0
         *   GET /~~waterparam?band=0&zoom=...
         * so parse "band=" as a numeric index first, falling back to a
         * name match for any hand-written config that still sends names. */
        int band_idx = -1;
        char *end = NULL;
        long idx = strtol(band_name, &end, 10);
        if (end && *end == '\0' && idx >= 0 && idx < g_config->nbands) {
            band_idx = (int)idx;
        } else {
            for (int i = 0; i < g_config->nbands; i++)
                if (strcmp(g_config->bands[i].name, band_name) == 0) { band_idx = i; break; }
        }

        if (band_idx >= 0) {
            struct band *b = &g_config->bands[band_idx];
            cli->band = b;
            cli->zoom = zoom;
            cli->start = start;
            cli->waterfall_active = true;
            client_add_to_band(cli, b);
            /* each client's format-9 delta baseline is per-client (they can be
             * on different zoom/start), so reset it on band/zoom/start change */
            memset(cli->prev_line, 0, WATERFALL_WIDTH);
            client_send_waterfall_control(cli);
            fprintf(stderr, "[proto] client -> band[%d]=%s, zoom %d, start %d\n",
                    band_idx, b->name, zoom, start);
        }
    }

    free(band_name);
    free(tmp);
}

static void handle_soundparam(struct client *cli, const char *params) {
    char *freq_s = NULL, *band_s = NULL, *lo_s = NULL, *hi_s = NULL,
         *mode_s = NULL, *mute_s = NULL, *name_s = NULL;
    char *tmp = strdup(params);
    char *token = strtok(tmp, "&");
    while (token) {
        if (strncmp(token, "f=", 2) == 0) {
            free(freq_s); freq_s = strdup(token + 2);
        } else if (strncmp(token, "band=", 5) == 0) {
            free(band_s); band_s = strdup(token + 5);
        } else if (strncmp(token, "lo=", 3) == 0) {
            free(lo_s); lo_s = strdup(token + 3);
        } else if (strncmp(token, "hi=", 3) == 0) {
            free(hi_s); hi_s = strdup(token + 3);
        } else if (strncmp(token, "mode=", 5) == 0) {
            free(mode_s); mode_s = strdup(token + 5);
        } else if (strncmp(token, "mute=", 5) == 0) {
            free(mute_s); mute_s = strdup(token + 5);
        } else if (strncmp(token, "name=", 5) == 0) {
            free(name_s); name_s = strdup(token + 5);
        }
        token = strtok(NULL, "&");
    }

    /* The client sends name= with every sound param (the users list renders
     * it; anonymous stays empty). It is URL-encoded by the client, so decode
     * %XX escapes and '+' back to space before storing. */
    if (name_s) {
        char *d = cli->username;
        int di = 0;
        for (char *s = name_s; *s && di < (int)sizeof(cli->username) - 1; s++) {
            if (*s == '%' && s[1] && s[2]) {
                int hexv = 0;
                for (int k = 1; k <= 2; k++) {
                    char c = s[k];
                    int v = (c >= '0' && c <= '9') ? c - '0'
                          : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                          : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                    if (v < 0) { hexv = -1; break; }
                    hexv = hexv * 16 + v;
                }
                if (hexv >= 0) { d[di++] = (char)hexv; s += 2; continue; }
            }
            d[di++] = (*s == '+') ? ' ' : *s;
        }
        d[di] = 0;
    }

    /* mute may come alone (toggle) — apply it even without a band change */
    if (mute_s) {
        cli->muted = (atoi(mute_s) != 0);
        fprintf(stderr, "[audio] client mute=%d\n", cli->muted ? 1 : 0);
    }

    if (band_s && g_config) {
        /* band addressed by numeric index (like waterfall), name fallback */
        int band_idx = -1;
        char *end = NULL;
        long idx = strtol(band_s, &end, 10);
        if (end && *end == '\0' && idx >= 0 && idx < g_config->nbands) {
            band_idx = (int)idx;
        } else {
            for (int i = 0; i < g_config->nbands; i++)
                if (strcmp(g_config->bands[i].name, band_s) == 0) { band_idx = i; break; }
        }

        if (band_idx >= 0) {
            struct band *b = &g_config->bands[band_idx];
            if (cli->band != b) {
                client_remove_from_band(cli);
                /* Drop any audio frames already queued for the OLD band: the
                 * pacer holds the old band's lock during flush, so after
                 * client_remove_from_band it can no longer enqueue for this
                 * client on the old band. Without this, already-encoded old
                 * band audio keeps playing after the switch. */
                client_free_outq(cli);
                cli->band = b;
                client_add_to_band(cli, b);
            }
            cli->freq = freq_s ? atof(freq_s) : b->centerfreq;
            cli->lo_filter = lo_s ? (int)(atof(lo_s) * 1000.0) : 300;
            cli->hi_filter = hi_s ? (int)(atof(hi_s) * 1000.0) : 2700;
            cli->mode = mode_s ? atoi(mode_s) : 0;
            cli->audio_active = true;
            if (!cli->audio.fir)
                audio_init(cli, b->samplerate);
            audio_reconfigure(cli, b->samplerate, band_eff_center(b));
            fprintf(stderr, "[audio] client band=%s f=%.1f mode=%d lo=%d hi=%d\n",
                    b->name, cli->freq, cli->mode, cli->lo_filter, cli->hi_filter);
        }
    }

    free(freq_s); free(band_s); free(lo_s); free(hi_s); free(mode_s);
    free(mute_s);
    free(name_s);
    free(tmp);
}

void protocol_handle_message(struct client *cli, const uint8_t *data, size_t len) {
    char msg[4096];
    if (len >= sizeof(msg)) return;
    memcpy(msg, data, len);
    msg[len] = '\0';

    if (strncmp(msg, "GET /~~waterparam", 17) == 0) {
        char *q = strchr(msg, '?');
        if (q) handle_waterparam(cli, q + 1);
    } else if (strncmp(msg, "GET /~~param", 12) == 0) {
        char *q = strchr(msg, '?');
        if (q) handle_soundparam(cli, q + 1);
    }
}
