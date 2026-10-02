#!/bin/bash
# sfifo.sh — пример создания FIFO-каналов для бэндов WebSDR.
#
# Каждому бэнду из cfg/websdr.cfg нужен свой именованный канал (FIFO),
# в который pcmrecord пишет сырые IQ-данные. Имя канала и его путь должны
# совпадать с device-строкой бэнда в конфиге и со start-receiver.sh.
#
# Это ПРИМЕР: список бэндов ниже — с вашего сервера. Подправьте переменные
# FIFO_DIR и BANDS под свои бэнды (или передайте их аргументами), затем
# запустите от root:
#
#   sudo ./sfifo.sh                 # создать каналы из списка BANDS ниже
#   sudo ./sfifo.sh 80m 20m 2m      # или только нужные бэнды
#
# Скрипт безопасен: уже существующие каналы не пересоздаются.

set -e

# Каталог FIFO (совпадает с start-receiver.sh и cfg/websdr.cfg).
FIFO_DIR=${FIFO_DIR:-/home/radio/fifo}

# Пользователь/группа-владелец (совпадает с install.sh).
FIFO_USER=${FIFO_USER:-radio:radio}

# Список бэндов по умолчанию (пример с этого сервера). Замените под себя —
# количество и набор диапазонов произвольны.
BANDS=${BANDS:-10mH VHF 12m 15m 17m 20m 30m 40m 80m 160m}

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then usage; fi

# Если бэнды переданы аргументами — используем их вместо списка по умолчанию.
if [ $# -gt 0 ]; then
    BANDS="$*"
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "Ошибка: нужны права root (sudo) — каталог $FIFO_DIR создаётся под root." >&2
    exit 1
fi

mkdir -p "$FIFO_DIR"
chown "$FIFO_USER" "$FIFO_DIR" 2>/dev/null || true

for b in $BANDS; do
    fifo="$FIFO_DIR/fifo$b"
    if [ -p "$fifo" ]; then
        echo "* уже есть: $fifo"
    else
        mkfifo "$fifo"
        echo "+ создан:  $fifo"
    fi
done

chown "$FIFO_USER" "$FIFO_DIR"/fifo* 2>/dev/null || true
echo
echo "Готово. Не забудьте указать эти device-пути в cfg/websdr.cfg"
echo "(device $FIFO_DIR/fifo<бэнд>) и строки start_band в start-receiver.sh."
