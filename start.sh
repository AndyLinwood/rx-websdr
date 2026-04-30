#!/bin/bash

# Цвета для вывода
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Пути (адаптируйте под вашу систему)
RADIO_CONF="/etc/radio/radiod.conf"
RECEIVER_SCRIPT="/home/websdr/start-receiver.sh"
SERVER_BIN="/home/websdr/rx-websdr/bin/websdr_server"
CONFIG_FILE="/home/websdr/rx-websdr/cfg/websdr.cfg"
LOG_FILE="./websdr.log"

# PID файлы
RADIO_PID=""
RECEIVER_PID=""
SERVER_PID=""

echo -e "${GREEN}=== WebSDR Multi-Channel Startup Script ===${NC}"

# Функция очистки
cleanup() {
    echo -e "\n${YELLOW}Stopping services...${NC}"
    
    # Останавливаем сервер
    if [ ! -z "$SERVER_PID" ] && kill -0 $SERVER_PID 2>/dev/null; then
        echo "Stopping WebSDR Server (PID: $SERVER_PID)..."
        kill $SERVER_PID 2>/dev/null
        sleep 1
        kill -9 $SERVER_PID 2>/dev/null
    fi

    # Останавливаем скрипт приемников
    if [ ! -z "$RECEIVER_PID" ] && kill -0 $RECEIVER_PID 2>/dev/null; then
        echo "Stopping Receiver Script (PID: $RECEIVER_PID)..."
        kill $RECEIVER_PID 2>/dev/null
        sleep 1
        # Убиваем дочерние процессы pcmrecord
        pkill -f "pcmrecord.*fifo" 2>/dev/null
    fi

    echo -e "${GREEN}All services stopped.${NC}"
    exit 0
}

trap cleanup SIGINT SIGTERM EXIT

# 1. Проверка radiod
echo -n "Checking radiod... "
if pgrep -x "radiod" > /dev/null; then
    echo -e "${GREEN}Already running.${NC}"
else
    echo -e "${YELLOW}Not running. Starting...${NC}"
    sudo radiod "$RADIO_CONF" > "$LOG_FILE.radiod" 2>&1 &
    sleep 2
    if ! pgrep -x "radiod" > /dev/null; then
        echo -e "${RED}Failed to start radiod.${NC}"
        exit 1
    fi
    echo -e "${GREEN}Started.${NC}"
fi

# 2. Запуск приемников
echo -n "Starting receiver channels... "
if [ ! -f "$RECEIVER_SCRIPT" ]; then
    echo -e "${RED}Error: $RECEIVER_SCRIPT not found!${NC}"
    exit 1
fi

bash "$RECEIVER_SCRIPT" > "$LOG_FILE.receiver" 2>&1 &
RECEIVER_PID=$!

# Ожидание FIFO
echo "Waiting for FIFO files..."
for i in {1..30}; do
    if ls /home/websdr/fifo/fifo*L 1> /dev/null 2>&1 || ls /home/websdr/fifo/fifo*H 1> /dev/null 2>&1; then
        echo -e "${GREEN}FIFO files detected.${NC}"
        break
    fi
    sleep 1
    echo -n "."
done
echo ""

# 3. Проверка конфига
if [ ! -f "$CONFIG_FILE" ]; then
    echo -e "${RED}Error: Config file $CONFIG_FILE not found!${NC}"
    exit 1
fi

# 4. Запуск сервера
echo -n "Starting WebSDR Server... "
if [ ! -f "$SERVER_BIN" ]; then
    echo -e "${RED}Error: Binary $SERVER_BIN not found! Run 'make'.${NC}"
    exit 1
fi

# ВАЖНО: Запускаем из текущей директории, чтобы пути к web/pub2 работали
"$SERVER_BIN" "$CONFIG_FILE" > "$LOG_FILE.server"   2>&1 &

SERVER_PID=$!

sleep 2
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo -e "${RED}Server failed to start. Check logs.${NC}"
    cat "$LOG_FILE.server"
    exit 1
fi

echo -e "${GREEN}Server started (PID: $SERVER_PID)${NC}"
echo -e "${GREEN}=========================================${NC}"
echo -e "WebSDR is RUNNING at http://localhost"
echo -e "Logs: tail -f $LOG_FILE.server"
echo -e "${YELLOW}Press Ctrl+C to stop.${NC}"

# Ожидание завершения сервера
wait $SERVER_PID