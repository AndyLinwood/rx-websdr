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

#endif