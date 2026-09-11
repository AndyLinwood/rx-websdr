#!/bin/bash
# ============================================================================
# install.sh — установка/наладка серверного стека rx-websdr
# ============================================================================
# Ставит зависимости сборки, собирает бинарник, создаёт FIFO-каталог и
# устанавливает systemd-юниты (receiver.service, websdr.service) по месту.
# radiod/pcmrecord (ka9q-radio) НЕ ставит — см. README ссылку на KA9Q.
#
# Использование:
#   sudo ./install.sh [--prefix /home/radio/rx-websdr] [--fifo-dir /home/radio/fifo]
#
# Ключи:
#   --prefix     каталог, куда установлено дерево (по умолчанию PWD)
#   --fifo-dir   каталог FIFO (по умолчанию $PREFIX/fifo)
#
# После установки: правьте cfg/websdr.cfg, затем ./start.sh

set -e

PREFIX="${PREFIX:-$(pwd)}"
FIFO_DIR="${FIFO_DIR:-$PREFIX/fifo}"
PORT="${PORT:-80}"
USER_RADIO="radio"          # владелец FIFO/каталогов (создаётся при необходимости)

echo "== rx-websdr: установка в $PREFIX (fifo: $FIFO_DIR, порт: $PORT) =="

# ---------- 1) системные зависимости (Debian/Ubuntu) ----------
echo
echo ">> [1/5] зависимости сборки..."
apt-get update -y
apt-get install -y git build-essential pkg-config \
    libwebsockets-dev libfftw3-single3 libfftw3-dev \
    libbsd-dev libiniparser-dev libcurl4-openssl-dev \
    avahi-daemon

# ---------- 2) сборка ----------
echo
echo ">> [2/5] сборка бинарника..."
cd "$PREFIX"
make

echo "   готово: $PREFIX/rx-websdr"

# ---------- 3) FIFO-каталог ----------
echo
echo ">> [3/5] FIFO-каталог $FIFO_DIR ..."
if ! id "$USER_RADIO" >/dev/null 2>&1; then
    useradd -r -m -s /bin/false "$USER_RADIO" 2>/dev/null || true
fi
mkdir -p "$FIFO_DIR"
chown "$USER_RADIO":"$USER_RADIO" "$FIFO_DIR" 2>/dev/null || true

# Имена FIFO по умолчанию (12 бэндов, единый формат fifo<band>):
for b in 10mH 10mL 11m 12m 15m 17m 20m 30m 40m 80m 160m UVB; do
    [ -p "$FIFO_DIR/fifo$b" ] || mkfifo "$FIFO_DIR/fifo$b"
done
chown "$USER_RADIO":"$USER_RADIO" "$FIFO_DIR"/fifo* 2>/dev/null || true
echo "   создано FIFO: $(ls "$FIFO_DIR" | wc -l)"

# ---------- 4) systemd-юниты ----------
echo
echo ">> [4/5] установка systemd-юнитов..."
cat > /etc/systemd/system/receiver.service <<EOF
[Unit]
Description=WebSDR receiver (pcmrecord -> fifo)
Requires=network-online.target
After=radiod@rx888.service
Before=websdr.service

[Service]
Type=exec
ExecStart=$PREFIX/start-receiver.sh
Restart=always
RestartSec=3
KillMode=control-group
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF

cat > /etc/systemd/system/websdr.service <<EOF
[Unit]
Description=WebSDR (our project)
Requires=network-online.target
After=receiver.service

[Service]
Type=exec
WorkingDirectory=$PREFIX
ExecStartPre=-/bin/pkill -9 -x rx-websdr
ExecStart=$PREFIX/rx-websdr -c cfg/websdr.cfg
Restart=on-failure
RestartSec=3
KillMode=control-group
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable receiver.service websdr.service
echo "   юниты установлены: receiver.service, websdr.service"

# ---------- 5) проверка ----------
echo
echo ">> [5/5] проверка (для запуска используйте ./start.sh)..."
systemctl is-enabled receiver.service websdr.service 2>/dev/null | tr '\n' ' '; echo

cat <<EOF

Установка завершена.

Дальнейшие шаги:
  1. Настройте radiod (см. https://github.com/ka9q/ka9q-radio) так, чтобы он
     публиковал потоки <band>-pcm.local.
  2. Правьте $PREFIX/cfg/websdr.cfg: tcpport $PORT, device-пути
     ($FIFO_DIR/fifo<band>), freqoffset при необходимости.
  3. (Опционально) Глобальный мониторинг: заполните секцию
     "=== Registry" в cfg/websdr.cfg (registry_enabled yes, эндпоинт
     srr-76.ru, координаты, позывной) — сервер начнёт публиковаться
     в списке на https://srr-76.ru/listsdr.html.
  3a. (Опционально) Кнопки-ссылки на внешние серверы: добавьте в
     cfg/websdr.cfg строки "buttonlink <метка>|<URL>" (напр. 2m-кнопку).
  4. Правьте $PREFIX/start-receiver.sh под ваши бэнды.
  5. Запустите:  $PREFIX/start.sh
     Проверка:    curl -sf http://127.0.0.1:$PORT/
EOF