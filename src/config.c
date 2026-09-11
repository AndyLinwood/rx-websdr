/*
 * WebSDR Server — Configuration parser
 * Reads websdr.cfg format
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "websdr.h"

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

int config_load(const char *filename, struct websdr_config *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) {
        perror(filename);
        return -1;
    }
    
    char line[1024];
    struct band *current_band = NULL;
    
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;
        
        char *key = strsep(&p, " \t");
        if (!key) continue;
        
        if (strcmp(key, "band") == 0 && p) {
            if (config->nbands >= MAX_BANDS) {
                fprintf(stderr, "Too many bands\n");
                fclose(fp);
                return -1;
            }
            current_band = &config->bands[config->nbands];
            strncpy(current_band->name, trim(p), sizeof(current_band->name)-1);
            config->nbands++;
        } else if (current_band) {
            if (strcmp(key, "device") == 0 && p)
                strncpy(current_band->device, trim(p), sizeof(current_band->device)-1);
            else if (strcmp(key, "samplerate") == 0 && p)
                current_band->samplerate = atoi(p);
            else if (strcmp(key, "centerfreq") == 0 && p)
                current_band->centerfreq = atof(p);
            else if (strcmp(key, "freqoffset") == 0 && p)
                current_band->freqoffset = atof(p);


            else if (strcmp(key, "gain") == 0 && p)
                current_band->gain = atof(p);
            else if (strcmp(key, "swapiq") == 0)
                current_band->swapiq = true;
            else if (strcmp(key, "hpf") == 0 && p)
                current_band->hpf = atoi(p);
            else if (strcmp(key, "noiseblanker") == 0 && p)
                current_band->noiseblanker = atoi(p);
            else if (strcmp(key, "extrazoom") == 0 && p) {
                int ez = atoi(p);
                current_band->extrazoom = ez > 0 ? (ez > 8 ? 8 : ez) : 0;
            }
        } else {
            /* Global settings */
            if (strcmp(key, "tcpport") == 0 && p)
                config->tcpport = atoi(p);
            else if (strcmp(key, "maxusers") == 0 && p)
                config->maxusers = atoi(p);
            else if (strcmp(key, "idletimeout") == 0 && p)
                config->idletimeout = atoi(p) * 1000;
            else if (strcmp(key, "waterfallformat") == 0 && p)
                config->waterfallformat = atoi(p);
            else if (strcmp(key, "audioformat") == 0 && p)
                config->audioformat = atoi(p);
            else if (strcmp(key, "fftplaneffort") == 0 && p) {
                int e = atoi(p);
                config->fftplaneffort = (e < 0 ? 0 : (e > 3 ? 3 : e));
            }
            else if (strcmp(key, "initial") == 0 && p) {
                char *freq = strsep(&p, " \t");
                if (freq) config->ini_freq = atof(freq);
                if (p) { char *m = trim(p); strncpy(config->ini_mode, m, sizeof(config->ini_mode)-1); }
            } else if (strcmp(key, "chseq") == 0 && p)
                config->chseq = atoi(p);
            else if (strcmp(key, "chat") == 0 && p)
                config->chat = atoi(p) ? 1 : 0;
            else if (strcmp(key, "chatfile") == 0 && p)
                strncpy(config->chatfile, trim(p), sizeof(config->chatfile)-1);
            else if (strcmp(key, "visitors") == 0 && p)
                config->visitors = atoi(p) ? 1 : 0;
            else if (strcmp(key, "visitorsfile") == 0 && p)
                strncpy(config->visitorsfile, trim(p), sizeof(config->visitorsfile)-1);
            else if (strcmp(key, "visitorsmax") == 0 && p)
                config->visitorsmax = atol(p) ? atol(p) : 2*1024*1024;
            else if (strcmp(key, "buttonlink") == 0 && p) {
                /* Диапазонная кнопка-ссылка: "buttonlink <метка>|<URL>".
                 * Разделитель '|' — URL может содержать пробелы/спецсимволы. */
                if (config->nbuttonlinks >= MAX_BUTTONLINKS) {
                    fprintf(stderr, "Too many buttonlinks (max %d)\n", MAX_BUTTONLINKS);
                    continue;
                }
                char *arg = strdup(trim(p));
                char *sep = strchr(arg, '|');
                if (!sep || sep == arg) {
                    fprintf(stderr, "buttonlink: expected '<label>|<url>', got: %s\n", arg);
                    free(arg);
                    continue;
                }
                struct buttonlink *bl = &config->buttonlinks[config->nbuttonlinks];
                /* label — часть ДО '|' (не весь arg) */
                int llen = (int)(sep - arg);
                if (llen > (int)sizeof(bl->label)-1) llen = sizeof(bl->label)-1;
                memcpy(bl->label, arg, llen);
                bl->label[llen] = 0;
                strncpy(bl->url, sep + 1, sizeof(bl->url)-1);
                free(arg);
                config->nbuttonlinks++;
            }
        }
    }
    
fprintf(stderr, "[CONFIG] %s freqoffset=%.2f center=%.1f eff=%.3f\n",
        current_band->name, current_band->freqoffset,
        current_band->centerfreq,
        current_band->centerfreq + current_band->freqoffset/1000.0);

    fclose(fp);

    /* Derive the deepest usable waterfall zoom per band from its samplerate:
     * at maxzoom the 1024-pixel window shows ~24 kHz (moderately-resolved
     * passband); flickout any larger. cfg `extrazoom` adds extra zoom levels
     * on top (original websdr semantics), but never beyond one FFT bin per
     * display pixel: step = (FFT_SIZE/1024) >> zoom must stay >= 1, i.e.
     * zoom <= log2(FFT_SIZE/1024). */
    int zoom_cap = 0;
    while ((FFT_SIZE / WATERFALL_WIDTH) >> (zoom_cap + 1) > 0) zoom_cap++;
    for (int i = 0; i < config->nbands; i++) {
        int mz = 0;
        while ((config->bands[i].samplerate >> (mz + 1)) >= 24000)
            mz++;
        mz += config->bands[i].extrazoom;
        if (mz > zoom_cap) mz = zoom_cap;
        config->bands[i].maxzoom = mz;
    }
    
    /* Defaults */
    if (config->tcpport == 0) config->tcpport = 8095;
    if (config->maxusers == 0) config->maxusers = 200;
    if (config->idletimeout == 0) config->idletimeout = 900 * 1000;
    if (config->waterfallformat == 0) config->waterfallformat = 9;
    if (config->chatfile[0] == 0) strcpy(config->chatfile, "chat.log");
    config->visitors = 1;
    if (config->visitorsfile[0] == 0) strcpy(config->visitorsfile, "visitors.log");
    if (config->visitorsmax <= 0) config->visitorsmax = 2*1024*1024;
    if (config->fftplaneffort == 0) config->fftplaneffort = 0; /* 0 = FFTW_ESTIMATE */

    return 0;
}

/* Горячий перезапуск конфигурации (SIGHUP): перечитывает cfg/websdr.cfg и
 * применяет на лету БЕЗ опасных для клиентов параметров.
 *
 * Безопасно обновляются (не требуют рестарта):
 *   chat, chatfile, visitors, visitorsfile, visitorsmax, gain (per-band),
 *   tcpport/прочее — НЕ трогаем (клиенты уже подключены).
 *
 * Структурные изменения (кол-во бэндов, имена, samplerate, centerfreq,
 * device, freqoffset, maxzoom) требуют полного ./start.sh — при их
 * обнаружении печатаем предупреждение и НЕ применяем.
 *
 * Вызывается ТОЛЬКО из главного lws-потока (server_start), не из обработчика
 * сигнала. Возвращает 0 при успехе, -1 при ошибке чтения конфига. */
int config_reload_hot(struct websdr_config *live) {
    if (!live) return -1;

    /* НЕ на стеке: struct websdr_config огромна (32 band * fft_input/output
     * 32768*2 float * 2 ≈ десятки МБ) — main держит её как static, здесь
     * используем heap, иначе stack overflow -> SEGV. */
    struct websdr_config *tmp = calloc(1, sizeof(struct websdr_config));
    if (!tmp) { fprintf(stderr, "[reload] OOM\n"); return -1; }

    if (config_load(g_config_file != NULL ? g_config_file : "cfg/websdr.cfg", tmp) != 0) {
        fprintf(stderr, "[reload] FAILED to re-read %s — keeping live config\n",
                g_config_file ? g_config_file : "cfg/websdr.cfg");
        free(tmp);
        return -1;
    }
    fprintf(stderr, "[reload] config_load done, nbands=%d\n", tmp->nbands);

    /* Структурная сверка: любые изменения тут требуют рестарта, а не релоада. */
    if (tmp->nbands != live->nbands) {
        fprintf(stderr, "[reload] band count changed (%d -> %d): full restart (./start.sh) required, skipping\n",
                live->nbands, tmp->nbands);
        free(tmp);
        return -1;
    }
    for (int i = 0; i < live->nbands; i++) {
        struct band *a = &live->bands[i];
        struct band *b = &tmp->bands[i];
        if (strcmp(a->name, b->name) != 0 ||
            a->samplerate != b->samplerate ||
            a->centerfreq != b->centerfreq ||
            a->maxzoom != b->maxzoom ||
            strcmp(a->device, b->device) != 0) {
            fprintf(stderr, "[reload] band %d '%s' changed structurally: full restart (./start.sh) required, skipping\n",
                    i, a->name);
            free(tmp);
            return -1;
        }
    }

    /* Безопасные поля — применять на лету. */
    live->chat      = tmp->chat;
    strncpy(live->chatfile, tmp->chatfile, sizeof(live->chatfile)-1);
    live->visitors  = tmp->visitors;
    strncpy(live->visitorsfile, tmp->visitorsfile, sizeof(live->visitorsfile)-1);
    live->visitorsmax = tmp->visitorsmax;

    /* gain применяется при рендере строки водопада — достаточно обновить поле. */
    for (int i = 0; i < live->nbands; i++) {
        if (live->bands[i].gain != tmp->bands[i].gain) {
            fprintf(stderr, "[reload] band %s gain %.1f -> %.1f\n",
                    live->bands[i].name, live->bands[i].gain, tmp->bands[i].gain);
            live->bands[i].gain = tmp->bands[i].gain;
        }
    }

    /* buttonlinks можно обновлять целиком (простой массив, рендерится на лету). */
    live->nbuttonlinks = tmp->nbuttonlinks;
    memcpy(live->buttonlinks, tmp->buttonlinks, sizeof(tmp->buttonlinks));

    free(tmp);
    fprintf(stderr, "[reload] config re-read OK (chat=%d visitors=%d, bands unchanged)\n",
            live->chat, live->visitors);
    return 0;
}
