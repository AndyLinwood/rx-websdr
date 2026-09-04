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
            else if (strcmp(key, "initial") == 0 && p) {
                char *freq = strsep(&p, " \t");
                if (freq) config->ini_freq = atof(freq);
                if (p) { char *m = trim(p); strncpy(config->ini_mode, m, sizeof(config->ini_mode)-1); }
            } else if (strcmp(key, "chseq") == 0 && p)
                config->chseq = atoi(p);
        }
    }
    
fprintf(stderr, "[CONFIG] %s freqoffset=%.2f center=%.1f eff=%.3f\n",
        current_band->name, current_band->freqoffset,
        current_band->centerfreq,
        current_band->centerfreq + current_band->freqoffset/1000.0);

    fclose(fp);

    /* Derive the deepest usable waterfall zoom per band from its samplerate:
     * at maxzoom the 1024-pixel window shows ~24 kHz (moderately-resolved
     * passband); flickout any larger. */
    for (int i = 0; i < config->nbands; i++) {
        int mz = 0;
        while ((config->bands[i].samplerate >> (mz + 1)) >= 24000)
            mz++;
        config->bands[i].maxzoom = mz;
    }
    
    /* Defaults */
    if (config->tcpport == 0) config->tcpport = 8095;
    if (config->maxusers == 0) config->maxusers = 200;
    if (config->idletimeout == 0) config->idletimeout = 900 * 1000;
    if (config->waterfallformat == 0) config->waterfallformat = 9;
    
    return 0;
}
