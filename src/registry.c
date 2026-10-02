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

/* Категория диапазона по centerfreq (кГц), чтобы агрегатор мог показать
 * VHF/UHF и фильтр "VHF+" на карте работал. Пороги: ITU bands. */
static const char *band_category_khz(double center_khz) {
    double mhz = center_khz / 1000.0;
    if (mhz < 30.0)          return "HF";
    if (mhz < 300.0)         return "VHF";
    if (mhz < 3000.0)        return "UHF";
    return "SHF";
}

/* Построить JSON-массив категорий диапазонов по реальным бэндам конфига.
 * Пример: ["HF","VHF"]. Категории уникальны, порядок = порядок в конфиге. */
static int build_bands_json(char *dst, size_t dstsz) {
    if (!g_config) { snprintf(dst, dstsz, "[\"HF\"]"); return 0; }

    char cats[8][8];
    int nc = 0;
    for (int i = 0; i < g_config->nbands; i++) {
        const char *c = band_category_khz(g_config->bands[i].centerfreq);
        int seen = 0;
        for (int j = 0; j < nc; j++)
            if (strcmp(cats[j], c) == 0) { seen = 1; break; }
        if (!seen && nc < 8) strncpy(cats[nc++], c, 8);
    }
    if (nc == 0) { snprintf(dst, dstsz, "[\"HF\"]"); return 0; }

    size_t n = 0;
    n += (size_t)snprintf(dst + n, dstsz - n, "[");
    for (int i = 0; i < nc; i++) {
        n += (size_t)snprintf(dst + n, dstsz - n, "%s\"%s\"", i ? "," : "", cats[i]);
        if (n + 4 >= dstsz) break;
    }
    snprintf(dst + n, dstsz - n, "]");
    return 0;
}

/* buffer для тела ответа агрегатора (ищем next_heartbeat_in / verify_token) */
static char Resp_body[1024];
static size_t Resp_len = 0;
static size_t resp_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata) {
    (void)userdata;
    size_t avail = sizeof(Resp_body) - Resp_len;
    size_t take = size * nmemb;
    if (take > avail) take = avail;
    memcpy(Resp_body + Resp_len, ptr, take);
    Resp_len += take;
    return size * nmemb;
}

/* Токен верификации, выданный агрегатором (ждёт отправки в след. heartbeat). */
static char Pending_token[128] = "";

static size_t find_json_string(const char *key, char *out, size_t outsz, int *found) {
    *found = 0;
    /* key: "..." — допускаем пробелы после ':' */
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    char *p = strstr(Resp_body, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (p < Resp_body + Resp_len && (*p == ' ' || *p == '\t')) p++;
    if (p >= Resp_body + Resp_len || *p != '"') return 0;
    p++;
    size_t j = 0;
    while (p < Resp_body + Resp_len && *p != '"' && j < outsz - 1) {
        if (*p == '\\' && p + 1 < Resp_body + Resp_len) p++;  /* экранирование */
        out[j++] = *p++;
    }
    out[j] = 0;
    *found = 1;
    return j;
}

/* Разобрать ответ агрегатора: next_heartbeat_in (интервал) и verify_token. */
static void parse_server_response(void) {
    /* verify_token: если агрегатор выдал токен — сохраняем для след. heartbeat */
    int tk = 0;
    char tok[128];
    find_json_string("verify_token", tok, sizeof(tok), &tk);
    if (tk && tok[0]) {
        snprintf(Pending_token, sizeof(Pending_token), "%s", tok);
        fprintf(stderr, "registry: got verify_token, will send with next heartbeat\n");
    }

    const char *needle = "next_heartbeat_in";
    char *p = strstr(Resp_body, needle);
    if (!p) return;
    p += strlen(needle);
    while (p < Resp_body + Resp_len && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    long v = 0;
    while (p < Resp_body + Resp_len && *p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    if (v >= 8 && v <= 3600) {
        /* Шлём чуть чаще, чем просит агрегатор: 0.8 * запрошенного,
         * но не меньше 8с (агрегатор на need_token просит 10с — идём
         * быстро, чтобы верификация прошла до протухания токена). */
        long want = v * 8 / 10;
        if (want < 8) want = 8;
        if (want != Cfg.interval) {
            Cfg.interval = (int)want;
            fprintf(stderr, "registry: interval adjusted to %ds (server asked %lds)\n",
                    Cfg.interval, v);
        }
    }
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

    /* Охват частот по всем бэндам (не только первому) + макс. samprate. */
    int samprate = 0;
    long long freq_min = 0, freq_max = 0;
    for (int i = 0; g_config && i < g_config->nbands; i++) {
        double cf = g_config->bands[i].centerfreq * 1000.0;
        long long lo = (long long)(cf - g_config->bands[i].samplerate / 2.0);
        long long hi = (long long)(cf + g_config->bands[i].samplerate / 2.0);
        if (i == 0) { freq_min = lo; freq_max = hi; }
        else {
            if (lo < freq_min) freq_min = lo;
            if (hi > freq_max) freq_max = hi;
        }
        if (g_config->bands[i].samplerate > samprate)
            samprate = g_config->bands[i].samplerate;
    }

    char bands[128];
    build_bands_json(bands, sizeof(bands));

    /* verify_token: добавляем в первый heartbeat после того, как агрегатор
     * выдал токен (регистрация нового сервера). После успешной отправки
     * токен очищается. */
    char e_token[160];
    char tok_field[200];
    if (Pending_token[0] != 0) {
        json_escape(e_token, sizeof(e_token), Pending_token);
        snprintf(tok_field, sizeof(tok_field), ",\"verify_token\":\"%s\"", e_token);
    } else {
        tok_field[0] = 0;
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
        "\"bands\":%s,"
        "\"max_listeners\":%d,"
        "\"listeners_current\":%d,"
        "\"listeners_peak_today\":0,"
        "\"listeners_peak_alltime\":0,"
        "\"uptime_seconds\":%ld,"
        "\"timestamp\":%ld"
        "%s"
        "}",
        e_id,
        e_name, e_call, e_qth,
        Cfg.lat, Cfg.lon,
        e_web, e_desc, e_ant, e_model,
        samprate, freq_min, freq_max,
        bands,
        Cfg.max_listeners, listeners,
        uptime, (long)time(NULL),
        tok_field
    );

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, Cfg.endpoint);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, buf);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    /* Связь до агрегатора нестабильна (наблюдали ответы 0.1с..12с+ и
     * отдельные TCP-timeout). Длинный общий таймаут + ретраи с паузами
     * надёжнее короткого: единичный медленный ответ не должен ронять
     * статус в slow/offline. Интервал при этом управляется ответом
     * агрегатора (next_heartbeat_in), так что перегрузки не будет. */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, resp_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, NULL);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   /* потокобезопасность для многопоточного процесса */

    Resp_len = 0;
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
    parse_server_response();
    /* Если токен отправлен успешно (сервер принял heartbeat) — очищаем.
     * При 'need_token' ответе код 200, но токен мы ещё не отдали:
     * очистку делаем ТОЛЬКО после того, как отправили его. */
    if (Pending_token[0] != 0 && strstr(Resp_body, "need_token") == NULL) {
        Pending_token[0] = 0;
    }
    return 0;
}

static void *heartbeat_loop(void *arg) {
    (void)arg;
    send_heartbeat();
    while (Running) {
        /* Интервал может меняться из ответа агрегатора (next_heartbeat_in),
         * поэтому читаем Cfg.interval на каждой итерации. */
        int iv = Cfg.interval;
        for (int i = 0; i < iv && Running; i++) sleep(1);
        if (Running) {
            /* Одна потеря не роняет статус: до 2 ретраев с паузами. */
            int rc = send_heartbeat();
            if (rc != 0 && Running) {
                for (int r = 0; r < 2 && Running; r++) {
                    sleep(3 + 2 * r);
                    if (Running && send_heartbeat() == 0) break;
                }
            }
        }
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

/* Вытащить hostname из URL вида "https://websdr.srr-76.ru" → "websdr.srr-76.ru".
 * Служит основой для уникального server_id (адрес сайта уникален и стабилен). */
static void host_from_url(const char *url, char *dst, size_t dstsz) {
    size_t i = 0, j = 0;
    /* пропускаем схему http(s):// */
    if (strncmp(url, "http://", 7) == 0) i = 7;
    else if (strncmp(url, "https://", 8) == 0) i = 8;
    /* берём до первого '/', ':', '?' или конца */
    for (; url[i] && url[i] != '/' && url[i] != ':' && url[i] != '?' && j < dstsz - 1; i++) {
        dst[j++] = url[i];
    }
    dst[j] = 0;
    if (j == 0) snprintf(dst, dstsz, "%s", "unset");
}

int registry_init(const struct websdr_config *config, const char *config_file) {
    (void)config;
    if (!config_file) config_file = "cfg/websdr.cfg";
    
    Cfg.enabled = read_cfg_bool(config_file, "registry_enabled", 0);
    if (!Cfg.enabled) {
        fprintf(stderr, "registry: disabled (registry_enabled != yes)\n");
        return 0;
    }

    /* registry_endpoint намеренно НЕ читается из конфига: это константа
     * протокола (адрес глобального агрегатора сети RX-WebSDR). Она вшита
     * в код, чтобы в реестр могли попасть только серверы из нашего
     * дистрибутива, а не левые подделки. */
    snprintf(Cfg.endpoint, sizeof(Cfg.endpoint), "%s", "https://srr-76.ru/api.php");

    read_cfg_string(config_file, "registry_name", Cfg.name, sizeof(Cfg.name), "RX-WebSDR");
    read_cfg_string(config_file, "registry_owner_call", Cfg.owner_call, sizeof(Cfg.owner_call), "");
    read_cfg_string(config_file, "registry_qth", Cfg.qth, sizeof(Cfg.qth), "");
    Cfg.lat = read_cfg_double(config_file, "registry_lat", 0.0);
    Cfg.lon = read_cfg_double(config_file, "registry_lon", 0.0);
    read_cfg_string(config_file, "registry_website", Cfg.website, sizeof(Cfg.website), "");
    read_cfg_string(config_file, "registry_desc", Cfg.description, sizeof(Cfg.description), "");
    read_cfg_string(config_file, "registry_antenna", Cfg.antenna, sizeof(Cfg.antenna), "");
    read_cfg_string(config_file, "registry_sdr_model", Cfg.sdr_model, sizeof(Cfg.sdr_model), "RX888 MkII");

    /* server_id: 
     * 1) явный registry_server_id в cfg — переопределение (продвинутый режим);
     * 2) иначе генерируем из registry_website (hostname из URL — стабильный
     *    и уникальный признак станции);
     * 3) если website не задан — hostname машины. */
    char explicit[128];
    read_cfg_string(config_file, "registry_server_id", explicit, sizeof(explicit), "");
    bool have_explicit = (explicit[0] != 0 && strcmp(explicit, "default-server") != 0);

    char host[160];
    if (Cfg.website[0] != 0) {
        host_from_url(Cfg.website, host, sizeof(host));
    } else {
        if (gethostname(host, sizeof(host)) != 0) snprintf(host, sizeof(host), "%s", "rx-websdr");
    }

    if (have_explicit) {
        snprintf(Cfg.server_id, sizeof(Cfg.server_id), "%s", explicit);
        fprintf(stderr, "registry: server_id = '%s' (from registry_server_id)\n", Cfg.server_id);
    } else {
        snprintf(Cfg.server_id, sizeof(Cfg.server_id), "%s", host);
        fprintf(stderr, "registry: server_id auto = '%s' (from %s)\n",
                Cfg.server_id,
                Cfg.website[0] ? "registry_website" : "hostname");
    }

    /* Лимит слушателей: предпочитаем реальный maxusers из конфига
     * (registry_max_users больше не нужен). Если maxusers не задан — 200. */
    Cfg.max_listeners = 200;
    if (config && config->maxusers > 0) Cfg.max_listeners = config->maxusers;
    else {
        int mu = read_cfg_int(config_file, "maxusers", 0);
        if (mu > 0) Cfg.max_listeners = mu;
    }

    /* Интервал heartbeat: управляется ответом агрегатора next_heartbeat_in
     * (см. parse_next_interval). Стартовый дефолт — 60с. */
    Cfg.interval = 60;
    
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

const char *registry_get_server_id(void) {
    if (!Cfg.enabled || Cfg.server_id[0] == 0) return "";
    return Cfg.server_id;
}