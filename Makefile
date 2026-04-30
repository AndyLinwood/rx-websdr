# Makefile для WebSDR сервера
# Компилятор и флаги
CXX = g++
CXXFLAGS = -std=c++17 -O3 -Wall -Wextra -pthread
LDFLAGS = -lfftw3f -lfftw3f_threads
LDFLAGS += -lssl -lcrypto

# Директории
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

# Исходные файлы
SOURCES = $(SRC_DIR)/websdr_server.cpp
OBJECTS = $(BUILD_DIR)/websdr_server.o
TARGET = $(BIN_DIR)/websdr_server

# Цели по умолчанию
all: directories $(TARGET)

# Создание директорий
directories:
	@mkdir -p $(BUILD_DIR)
	@mkdir -p $(BIN_DIR)

# Компиляция
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Линковка
$(TARGET): $(OBJECTS)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@ $(LDFLAGS)

# Очистка
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Установка (требует прав root)
install: $(TARGET)
	cp $(TARGET) /usr/local/bin/websdr_server
	chmod +x /usr/local/bin/websdr_server

# Запуск сервера
run: $(TARGET)
	cd .. && ./$(TARGET) cfg/websdr.cfg

# Отладочная сборка
debug: CXXFLAGS += -g -DDEBUG
debug: clean all

# Помощь
help:
	@echo "WebSDR Server Makefile"
	@echo ""
	@echo "Цели:"
	@echo "  all     - Сборка проекта (по умолчанию)"
	@echo "  clean   - Очистка скомпилированных файлов"
	@echo "  install - Установка в систему (/usr/local/bin)"
	@echo "  run     - Запуск сервера"
	@echo "  debug   - Отладочная сборка"
	@echo "  help    - Показать эту справку"
	@echo ""
	@echo "Требования:"
	@echo "  - FFTW3 library (libfftw3f-dev)"
	@echo "  - C++17 совместимый компилятор"
	@echo "  - pthread"

.PHONY: all clean install run debug help directories
LDFLAGS += -lssl -lcrypto -lgd