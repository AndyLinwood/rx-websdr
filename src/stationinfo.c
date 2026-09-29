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

static void parse_mode(const char *s, char *out) {
    if (strcmp(s, "am")==0) { strcpy(out, "AM"); return; }
    if (strcmp(s, "fm")==0) { strcpy(out, "FM"); return; }
    if (strcmp(s, "usb")==0 || strcmp(s, "usbn")==0) { strcpy(out, "USB"); return; }
    if (strcmp(s, "lsb")==0 || strcmp(s, "lsbn")==0) { strcpy(out, "LSB"); return; }
    if (strcmp(s, "cw")==0 || strcmp(s, "cwn")==0) { strcpy(out, "CW"); return; }
    strcpy(out, "AM");
}

int stationinfo_load(const char *filename, struct websdr_config *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { fprintf(stderr, "stationinfo: cannot open %s\n", filename); return -1; }
    char line[512];
    int total = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;
        char *sp = strchr(p, ' ');
        char *tab = strchr(p, '\t');
        char *sep = sp ? (tab && tab < sp ? tab : sp) : tab;
        if (!sep) continue;
        *sep = '\0';
        char *name = trim(sep + 1);
        if (!name || !*name) continue;
        int flen = strlen(p);
        int mi = flen - 1;
        while (mi > 0 && isalpha((unsigned char)p[mi])) mi--;
        if (mi <= 0) continue;
        char mode_str[16];
        strncpy(mode_str, p + mi + 1, sizeof(mode_str)-1);
        mode_str[sizeof(mode_str)-1] = '\0';
        p[mi + 1] = '\0';
        double freq = atof(p);
        if (freq <= 0) continue;
        char mode[8];
        parse_mode(mode_str, mode);
        fprintf(stderr, "[STDBG] parsed freq=%.3f mode=%s name=%s\n", freq, mode, name);
        for (int b = 0; b < config->nbands; b++) {
            struct band *band = &config->bands[b];
            double half = band->samplerate / 2000.0;
            double eff_center = band->centerfreq + band->freqoffset / 1000.0;
            fprintf(stderr, "[STDBG]   check band %s center=%.3f half=%.3f range=[%.3f,%.3f]\n",
                    band->name, eff_center, half, eff_center-half, eff_center+half);
            if (freq >= eff_center - half && freq <= eff_center + half) {
                fprintf(stderr, "[STDBG]   -> MATCH band %s\n", band->name);
                if (band->nstations < 256) {
                    int n = band->nstations++;
                    band->stations[n].freq = freq;
                    strncpy(band->stations[n].mode, mode, 7);
                    band->stations[n].mode[7] = '\0';
                    strncpy(band->stations[n].name, name, 127);
                    band->stations[n].name[127] = '\0';
                }
                total++;
                break;
            }
        }
    }
    fclose(fp);
    fprintf(stderr, "stationinfo: loaded %d stations total\n", total);
    for (int b = 0; b < config->nbands; b++) {
        if (config->bands[b].nstations > 0)
            fprintf(stderr, "stationinfo: band %s has %d stations\n",
                    config->bands[b].name, config->bands[b].nstations);
    }
    return 0;
}

/* stationinfo_reload: перечитывает stationinfo.txt на лету (SIGHUP reload),
 * не трогая живые данные и не отключая клиентов.
 *
 * Читает файл во временные буферы станций (по образцу band.stations[256]),
 * распределяя по бэндам тем же алгоритмом, что stationinfo_load(). Если файл
 * прочитан корректно — атомарно подменяет nstations/stations каждого бэнда.
 * При ошибке чтения/парсинга живые данные НЕ трогаются (бэкап не нужен).
 *
 * Вызывается из главного lws-потока (server_start, обработка SIGHUP), поэтому
 * гонок с HTTP-отдачей bandinfo.js нет. Возвращает 0 при успехе, -1 при ошибке. */
int stationinfo_reload(const char *filename, struct websdr_config *config) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { fprintf(stderr, "stationinfo: reload: cannot open %s\n", filename); return -1; }
    if (!config) { fclose(fp); return -1; }

    /* Временные массивы: MAX_BANDS — сколько бэндов может быть в конфиге.
     * Берём по образцу struct band: станции хранятся отдельно per band. */
    int tmp_n[MAX_BANDS];
    double tmp_freq[MAX_BANDS][256];
    char   tmp_mode[MAX_BANDS][256][8];
    char   tmp_name[MAX_BANDS][256][128];
    for (int b = 0; b < config->nbands; b++) tmp_n[b] = 0;

    char line[512];
    int total = 0;
    while (fgets(line, sizeof(line), fp)) {
        char *p = trim(line);
        if (*p == '#' || *p == '\0') continue;
        char *sp = strchr(p, ' ');
        char *tab = strchr(p, '\t');
        char *sep = sp ? (tab && tab < sp ? tab : sp) : tab;
        if (!sep) continue;
        *sep = '\0';
        char *name = trim(sep + 1);
        if (!name || !*name) continue;
        int flen = strlen(p);
        int mi = flen - 1;
        while (mi > 0 && isalpha((unsigned char)p[mi])) mi--;
        if (mi <= 0) continue;
        char mode_str[16];
        strncpy(mode_str, p + mi + 1, sizeof(mode_str)-1);
        mode_str[sizeof(mode_str)-1] = '\0';
        p[mi + 1] = '\0';
        double freq = atof(p);
        if (freq <= 0) continue;
        char mode[8];
        parse_mode(mode_str, mode);
        for (int b = 0; b < config->nbands; b++) {
            struct band *band = &config->bands[b];
            double half = band->samplerate / 2000.0;
            double eff_center = band->centerfreq + band->freqoffset / 1000.0;
            if (freq >= eff_center - half && freq <= eff_center + half) {
                if (tmp_n[b] < 256) {
                    int n = tmp_n[b]++;
                    tmp_freq[b][n] = freq;
                    strncpy(tmp_mode[b][n], mode, 7);
                    tmp_mode[b][n][7] = '\0';
                    strncpy(tmp_name[b][n], name, 127);
                    tmp_name[b][n][127] = '\0';
                }
                total++;
                break;
            }
        }
    }
    fclose(fp);

    /* Атомарная подмена: после успешного чтения обновляем живые данные. */
    for (int b = 0; b < config->nbands; b++) {
        struct band *band = &config->bands[b];
        band->nstations = tmp_n[b];
        for (int n = 0; n < tmp_n[b]; n++) {
            band->stations[n].freq = tmp_freq[b][n];
            strncpy(band->stations[n].mode, tmp_mode[b][n], 7);
            band->stations[n].mode[7] = '\0';
            strncpy(band->stations[n].name, tmp_name[b][n], 127);
            band->stations[n].name[127] = '\0';
        }
    }

    fprintf(stderr, "stationinfo: reloaded %d stations total\n", total);
    for (int b = 0; b < config->nbands; b++) {
        if (config->bands[b].nstations > 0)
            fprintf(stderr, "stationinfo: band %s now has %d stations\n",
                    config->bands[b].name, config->bands[b].nstations);
    }
    return 0;
}
