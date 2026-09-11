#!/bin/bash
# reload.sh — горячая перезагрузка конфигурации (SIGHUP) без остановки сервера.
#
# Перечитывает cfg/websdr.cfg на лету и применяет БЕЗОПАСНЫЕ параметры:
#   registry_*, chat/chatfile, visitors/visitorsfile/visitorsmax,
#   gain (per-band), buttonlink.
# Клиенты при этом НЕ отключаются (вебсокеты не рвутся).
#
# Структурные изменения (количество/имена бэндов, samplerate, centerfreq,
# device, freqoffset, maxzoom) требуют полного рестарта — ./start.sh;
# reload.sh о них предупредит (запись в журнале websdr.service) и НЕ применит.
#
# Использование:
#   ./reload.sh
#   # требует sudo (как start.sh)

set -e

# PID процесса rx-websdr из systemd (источник истины — MainPID).
PID=$(systemctl show websdr.service -p MainPID --value 2>/dev/null)

if [ -z "$PID" ] || [ "$PID" -le 1 ]; then
    echo ">> websdr.service не запущен (MainPID пуст) — используйте ./start.sh"
    exit 1
fi

if ! systemctl is-active --quiet websdr.service; then
    echo ">> websdr.service не active — используйте ./start.sh"
    exit 1
fi

echo ">> SIGHUP -> rx-websdr (pid $PID): горячая перезагрузка cfg/websdr.cfg..."

sudo kill -HUP "$PID"

# Дать серверу время обработать сигнал и посмотреть результат в журнале.
sleep 1

if systemctl is-active --quiet websdr.service; then
    echo ">> websdr.service active: конфигурация перезагружена на лету."
    echo ">> Детали (применённые gain и пр.) — в журнале:"
    echo "     journalctl -u websdr.service -n 20 --no-pager | grep 'reload'"
    # Покажем свежие строки reload из журнала, если есть.
    sudo journalctl -u websdr.service -n 20 --no-pager 2>/dev/null \
        | grep "reload" | tail -5 || true
else
    echo ">> ОШИБКА: websdr.service перестал быть active после SIGHUP."
    echo ">> Проверьте: systemctl status websdr.service"
    exit 1
fi