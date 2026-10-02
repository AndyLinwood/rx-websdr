#ifndef REGISTRY_H
#define REGISTRY_H

struct websdr_config;

// Инициализация. Читает ключи registry_* из файла конфига.
// Если registry_enabled != yes — ничего не делает.
int registry_init(const struct websdr_config *config, const char *config_file);

// Запуск фонового потока heartbeat.
int registry_start(void);

// Остановка потока.
void registry_stop(void);

// Вернуть активный server_id (пустая строка, если registry выключен).
// Используется сервером как HTTP-заголовок x-rx-websdr для верификации
// агрегатором: настоящий rx-websdr отвечает на запрос к своему сайту
// этим заголовком с тем же id, что шлёт в heartbeat.
const char *registry_get_server_id(void);

#endif