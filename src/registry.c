#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <curl/curl.h>

#include "registry.h"
#include "websdr.h"

extern struct websdr_config *g_config;

static struct {
    int    enabled;
    char   endpoint[512];
    char   server_id[128];
    char   name[256];
    char   owner_call[64];
    char   qth[256];
    double lat, lon;
    char   website[512];
    char   description[512];
    char   antenna[256];
    char   sdr_model[64];
    int    max_listeners;
    int    interval;
} Cfg = { .enabled = 0, .interval = 60, .max_listeners = 100 };

static time_t Start_time = 0;
static pthread_t Hb_thread;
static volatile int Running = 0;

static void json_escape(char *dst, size_t dstsz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src && src[i] && j + 2 < dstsz; i++) {
        if (src[i] == '"' || src[i] == '\\') dst[j++] = '\\';
        dst[j++] = src[i];
    }
    dst[j] = 0;
}

static int send_heartbeat(void) {
    CURL *curl = curl_easy_init();
    if (!curl) return -1;

    char buf[4096];
    char e_name[512], e_call[128], e_qth[512], e_desc[1024],
         e_ant[512], e_web[1024], e_model[128], e_id[256];

    json_escape(e_name, sizeof(e_name), Cfg.name);
    json_escape(e_call, sizeof(e_call), Cfg.owner_call);
    json_escape(e_qth,  sizeof(e_qth),  Cfg.qth);
    json_escape(e_desc, sizeof(e_desc), Cfg.description);
    json_escape(e_ant,  sizeof(e_ant),  Cfg.antenna);
    json_escape(e_web,  sizeof(e_web),  Cfg.website);
    json_escape(e_model,sizeof(e_model),Cfg.sdr_model);
    json_escape(e_id,   sizeof(e_id),   Cfg.server_id);

    int listeners = server_get_total_clients();

    long uptime = Start_time ? (long)(time(NULL) - Start_time) : 0;

    int samprate = 0;
    long long freq_min = 0, freq_max = 0;
    if (g_config && g_config->nbands > 0) {
        samprate = g_config->bands[0].samplerate;
        double cf = g_config->bands[0].centerfreq * 1000.0;
        freq_min = (long long)(cf - samprate / 2.0);
        freq_max = (long long)(cf + samprate / 2.0);
    }

    snprintf(buf, sizeof(buf),
        "{"
        "\"server_id\":\"%s\","
        "\"rx_websdr_version\":\"1.0.0\","
        "\"name\":\"%s\","
        "\"owner_call\":\"%s\","
        "\"qth\":\"%s\","
        "\"lat\":%.4f,"
        "\"lon\":%.4f,"
        "\"website\":\"%s\","
        "\"description\":\"%s\","
        "\"antenna\":\"%s\","
        "\"sdr_model\":\"%s\","
        "\"samprate\":%d,"
        "\"reference\":27000000,"
        "\"freq_min\":%lld,"
        "\"freq_max\":%lld,"
        "\"bands\":[\"HF\"],"
        "\"max_listeners\":%d,"
        "\"listeners_current\":%d,"
        "\"listeners_peak_today\":0,"
        "\"listeners_peak_alltime\":0,"
        "\"uptime_seconds\":%ld,"
        "\"timestamp\":%ld"
        "}",
        e_id,
        e_name, e_call, e_qth,
        Cfg.lat, Cfg.lon,
        e_web, e_desc, e_ant, e_model,
        samprate, freq_min, freq_max,
        Cfg.max_listeners, listeners,
        uptime, (long)time(NULL)
    );

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, Cfg.endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        fprintf(stderr, "registry: heartbeat failed: %s\n", curl_easy_strerror(res));
        return -1;
    }
    if (http_code != 200) {
        fprintf(stderr, "registry: heartbeat HTTP %ld\n", http_code);
        return -1;
    }
    return 0;
}

static void *heartbeat_loop(void *arg) {
    (void)arg;
    send_heartbeat();
    while (Running) {
        for (int i = 0; i < Cfg.interval && Running; i++) sleep(1);
        if (Running) send_heartbeat();
    }
    return NULL;
}

// Вспомогательные функции для чтения конфига
static void read_cfg_string(const char *filename, const char *key, char *dst, size_t dstsz, const char *def) {
    FILE *fp = fopen(filename, "r");
    if (!fp) { strncpy(dst, def, dstsz - 1); dst[dstsz - 1] = 0; return; }
    
    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0' || *p == '\n') continue;
        
        char *k = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t klen = p - k;
        
        if (klen == strlen(key) && strncmp(k, key, klen) == 0) {
            while (*p == ' ' || *p == '\t') p++;
            char *e = p + strlen(p) - 1;
            while (e > p && (*e == '\n' || *e == '\r' || *e == ' ')) *e-- = '\0';
            strncpy(dst, p, dstsz - 1);
            dst[dstsz - 1] = 0;
            fclose(fp);
            return;
        }
    }
    fclose(fp);
    strncpy(dst, def, dstsz - 1);
    dst[dstsz - 1] = 0;
}

static int read_cfg_int(const char *filename, const char *key, int def) {
    char buf[64];
    read_cfg_string(filename, key, buf, sizeof(buf), "");
    if (buf[0] == 0) return def;
    return atoi(buf);
}

static double read_cfg_double(const char *filename, const char *key, double def) {
    char buf[64];
    read_cfg_string(filename, key, buf, sizeof(buf), "");
    if (buf[0] == 0) return def;
    return atof(buf);
}

static int read_cfg_bool(const char *filename, const char *key, int def) {
    char buf[16];
    read_cfg_string(filename, key, buf, sizeof(buf), "");
    if (buf[0] == 0) return def;
    if (strcasecmp(buf, "yes") == 0 || strcasecmp(buf, "true") == 0 || strcmp(buf, "1") == 0) return 1;
    if (strcasecmp(buf, "no") == 0 || strcasecmp(buf, "false") == 0 || strcmp(buf, "0") == 0) return 0;
    return def;
}

int registry_init(const struct websdr_config *config, const char *config_file) {
    (void)config;
    if (!config_file) config_file = "cfg/websdr.cfg";
    
    Cfg.enabled = read_cfg_bool(config_file, "registry_enabled", 0);
    if (!Cfg.enabled) {
        fprintf(stderr, "registry: disabled (registry_enabled != yes)\n");
        return 0;
    }

    read_cfg_string(config_file, "registry_endpoint", Cfg.endpoint, sizeof(Cfg.endpoint), "https://srr-76.ru/api.php");
    read_cfg_string(config_file, "registry_server_id", Cfg.server_id, sizeof(Cfg.server_id), "default-server");
    read_cfg_string(config_file, "registry_name", Cfg.name, sizeof(Cfg.name), "RX-WebSDR");
    read_cfg_string(config_file, "registry_owner_call", Cfg.owner_call, sizeof(Cfg.owner_call), "");
    read_cfg_string(config_file, "registry_qth", Cfg.qth, sizeof(Cfg.qth), "");
    Cfg.lat = read_cfg_double(config_file, "registry_lat", 0.0);
    Cfg.lon = read_cfg_double(config_file, "registry_lon", 0.0);
    read_cfg_string(config_file, "registry_website", Cfg.website, sizeof(Cfg.website), "");
    read_cfg_string(config_file, "registry_desc", Cfg.description, sizeof(Cfg.description), "");
    read_cfg_string(config_file, "registry_antenna", Cfg.antenna, sizeof(Cfg.antenna), "");
    read_cfg_string(config_file, "registry_sdr_model", Cfg.sdr_model, sizeof(Cfg.sdr_model), "RX888 MkII");
    Cfg.max_listeners = read_cfg_int(config_file, "registry_max_users", 100);
    Cfg.interval = read_cfg_int(config_file, "registry_interval", 60);
    
    if (Cfg.interval < 10) Cfg.interval = 10;
    if (Cfg.interval > 600) Cfg.interval = 600;

    Start_time = time(NULL);
    curl_global_init(CURL_GLOBAL_DEFAULT);

    fprintf(stderr, "registry: enabled, endpoint=%s, server_id='%s', name='%s', interval=%ds\n",
            Cfg.endpoint, Cfg.server_id, Cfg.name, Cfg.interval);
    return 0;
}

int registry_start(void) {
    if (!Cfg.enabled) return 0;
    Running = 1;
    if (pthread_create(&Hb_thread, NULL, heartbeat_loop, NULL) != 0) {
        fprintf(stderr, "registry: failed to start thread\n");
        Running = 0;
        return -1;
    }
    return 0;
}

void registry_stop(void) {
    if (!Cfg.enabled || !Running) return;
    Running = 0;
    pthread_join(Hb_thread, NULL);
    curl_global_cleanup();
    fprintf(stderr, "registry: stopped\n");
}