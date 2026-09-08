# rx-websdr — Многодиапазонный WebSDR-сервер (RX888 / ka9q-radio)

WebSDR-сервер на основе оригинального WebSDR (PA3FWM) для работы с приёмником
RX888MKII и программным стеком [ka9q-radio](https://github.com/ka9q/ka9q-radio)
(Phil Karn, KA9Q). Отдаёт в браузер водопад и звук по WebSocket, декодируя
потоки IQ, которые публикует `radiod` в multicast-RTP.

Проект развивался для личного сервера в Ярославле (srr-76.ru) и заточен под
серверы с разнообразным железом: минимальные зависимости, конфиг через один
файл, запуск через systemd.

> **Установка и настройка `radiod` (приёмник, multicast, калибровка) — по
> описаниям автора KA9Q:** https://github.com/ka9q/ka9q-radio . Этот репозиторий
> содержит только WebSDR-часть (сервер + HTML5-клиент) и предполагает, что
> `radiod` уже настроен и публикует нужные потоки.

---

## Содержание

- [Архитектура](#архитектура)
- [Возможности](#возможности)
- [Структура репозитория](#структура-репозитория)
- [Требования](#требования)
- [Установка](#установка)
- [Конфигурация](#конфигурация)
- [Запуск и сервисы](#запуск-и-сервисы)
- [Тестовый сервер (клон)](#тестовый-сервер-клон)
- [Клиент rx-* (HTML5)](#клиент-rx--html5)
- [Часто задаваемые вопросы](#часто-задаваемые-вопросы)
- [Лицензии и авторство](#лицензии-и-авторство)

---

## Архитектура

Данные идут по цепочке:

```
 антенна → RX888MKII (USB) → radiod (ka9q-radio) → multicast RTP (lo)
        → pcmrecord → FIFO (/home/radio/fifo/fifo<band>) → rx-websdr
                                                        → браузер (HTML5)
```

1. **radiod** публикует каждый бэнд как отдельный RTP-multicast поток
   (`<band>-pcm.local`, avahi резолвит в `239.x`). Подробности — в документации
   KA9Q.
2. **receiver.service** запускает скрипт `start-receiver.sh`, который для
   каждого бэнда поднимает **pcmrecord** (`pcmrecord -c -r -S <ssrc> <group>`),
   пишущий сырые IQ-сэмплы в именованный канал FIFO.
3. **websdr.service** запускает `rx-websdr -c cfg/websdr.cfg` — сервер читает
   FIFO, считает FFT/спектр/аудио и обслуживает клиентов по HTTP/WebSocket.
4. Браузер грузит HTML5-клиента (`pub/rx-base.js`, `rx-waterfall.js`,
   `websdr-sound.js`) и получает водопад (format-9) и звук (format-11).

Схема позволяет запускать **параллельно несколько серверов** на одном
приёмнике: multicast доставляет полную копию потока каждому слушателю, поэтому
тестовый сервер (см. ниже) ничего не ломает в основе.

### Порты по умолчанию
Могут назначаться различные порты, в зависимости ситуации. Как пример:
| Назначение | Порт |
|---|---|
| Основной WebSDR | 8095 |
| Тестовый WebSDR (клон) | 8090 |

---

## Возможности

- 12 диапазонов одновременно (и более), каждый со своей полосой, шагом и гибкими настройками.
- HTML5-клиент: водопад (формат-9), зум/пан с пересчётом спектра (бина = 1 px),
  аудио (формат-11), S-метр, график уровня, DX-метки из `stationinfo.txt`,
  DX-кластер (`/~~fetchdx` — см. FAQ), чат, журнал, запись в WAV.
- Per-band `freqoffset` для компенсации сдвига частот (см. Конфигурация).
- Надёжный рестарт через systemd (таймауты, kill-группы, проверка порта curl).
- Лёгкая инфраструктура: только `libwebsockets`, `fftw3`, `libbsd`, `iniparser`.

---

## Структура репозитория

```
rx-websdr/
├── Makefile               # сборка (нужны pkg-config зависимости)
├── start.sh               # ручной (re)старт стека через systemd
├── start-receiver.sh      # запуск pcmrecord-писателей (для receiver.service)
├── cfg/
│   └── websdr.cfg         # главный конфиг сервера (порт, бэнды, freqoffset)
├── src/                   # исходники сервера (C)
├── pub/                   # статические файлы + HTML5-клиент
└── rx-websdr              # собранный бинарник
```

---

## Требования

- Linux (проверено на Debian/Ubuntu), `gcc`, `make`, `pkg-config`.
- `radiod` от ka9q-radio (настроен и публикует потоки IQ) — см. ссылку KA9Q.
- Библиотеки сборки (Debian/Ubuntu):
  ```bash
  sudo apt install build-essential pkg-config \
    libwebsockets-dev libfftw3-single3 libfftw3-dev libbsd-dev libiniparser-dev
  ```
  (`libwebsockets` названия пакета могут отличаться — см. сборку).
- `pcmrecord` (входит в состав ka9q-radio), `avahi-daemon` (резолвит
  `<band>-pcm.local`).

---

## Установка

1. Получите исходники:
   ```bash
   git clone https://github.com/AndyLinwood/rx-websdr.git
   cd rx-websdr
   ```
2. Соберите:
   ```bash
   make
   # появится бинарник ./rx-websdr
   ```
3. Скопируйте FIFO-каталог (нужен root):
   ```bash
   sudo mkdir -p /home/radio/fifo
   # создайте 12 именованных каналов для ваших бэндов, напр.:
   sudo mkfifo /home/radio/fifo/fifo40m
   ```
   Имена бэндов и пути FIFO задаются в `cfg/websdr.cfg` и `start-receiver.sh`
   — они должны совпадать (см. ниже).
4. Установите юниты systemd (примеры — в `start.sh`/`start-receiver.sh`,
   актуальные unit-файлы создаются по образцу из комментариев в файлах):
   - `receiver.service` → `ExecStart=/path/to/start-receiver.sh`
   - `websdr.service` → `ExecStart=/path/to/rx-websdr -c cfg/websdr.cfg`
5. Запустите стек: `./start.sh`.

> Полная пошаговая наладка (порядок radiod→receiver→websdr, тайминги,
> диагностика) — в разделе Запуск и сервисы и в комментариях к `start.sh`.

---

## Конфигурация

Все настройки сервера — в `cfg/websdr.cfg`. Кратко ключевые директивы:

| Директива | Описание |
|---|---|
| `tcpport 8095` | порт HTTP/WebSocket |
| `maxusers 200` | лимит одновременных клиентов |
| `idletimeout 5400` | таймаут бездействия клиента, мс |
| `band <имя>` | начало блока бэнда (10mH, 40m, UVB…) |
| `device /home/radio/fifo/fifo40m` | FIFO для этого бэнда |
| `samplerate 384000` | частота дискретизации IQ в отводе |
| `centerfreq 7100` | центральная частота бэнда, кГц |
| `gain 24` | усиление спектра |
| `freqoffset -452` | сдвиг частоты (Гц) относительно номинала (per-band) |

### freqoffset

Компенсирует систематический сдвиг частоты приёмника (аналоговые/гетеродинные
эффекты). Указывается per-band в Гц; знак — по направлению сдвига («-» если
станция видна выше номинала). Применяется согласованно к центру/меткам/шкале
и тюнеру. Настройка подбирается по известной станции (напр., UVB-зуммер 4625 кГц).

### Бэнды и FIFO

Имена FIFO единообразны: `fifo<имя-бэнда>` (напр., `fifo40m`, `fifo80m`,
`fifoUVB`). В `cfg/websdr.cfg` (`device`) и в `start-receiver.sh`
(`/home/radio/fifo/fifo<имя>`) они должны совпадать по именам.

---

## Запуск и сервисы

Стек управляется systemd. Правильный порядок:

```
radiod@rx888.service  (приёмник, публикует multicast)
   → receiver.service (pcmrecord → FIFO, писатели)
      → websdr.service (rx-websdr, читатели FIFO)
```

### start.sh

Ручной (re)старт: `./start.sh`

Скрипт сам:
1. поднимает `radiod@rx888` если он неактивен;
2. рестартует `receiver.service` и ждёт ≥12 писателей `pcmrecord`;
3. рестартует `websdr.service`;
4. ждёт, пока `:8095` ответит HTTP (curl), и сообщает статус.

### Юниты (образцы)

`/etc/systemd/system/receiver.service`:
```ini
[Unit]
Description=WebSDR receiver (pcmrecord -> fifo)
Requires=network-online.target
After=radiod@rx888.service
Before=websdr.service

[Service]
Type=exec
ExecStart=/home/radio/rx-websdr/start-receiver.sh
Restart=always
RestartSec=3
KillMode=control-group
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
```

`/etc/systemd/system/websdr.service`:
```ini
[Unit]
Description=WebSDR (our project)
Requires=network-online.target
After=receiver.service

[Service]
Type=exec
WorkingDirectory=/home/radio/rx-websdr
ExecStartPre=-/bin/pkill -9 -x rx-websdr
ExecStart=/home/radio/rx-websdr/rx-websdr -c cfg/websdr.cfg
Restart=on-failure
RestartSec=3
KillMode=control-group
TimeoutStopSec=10

[Install]
WantedBy=multi-user.target
```

Пояснения:
- `KillMode=control-group` + `TimeoutStopSec=10` — pcmrecord/rx-websdr могут
  игнорировать SIGTERM (заблокированы в FIFO-I/O); так остановка быстрая и не
  виснет 90 с.
- `ExecStartPre=-/bin/pkill -9 -x rx-websdr` — освобождает порт на старте
  (только свой процесс по имени бинарника). **Не дублируйте** pkill в
  `ExecStop` — он убьёт новый инстанс при `systemctl restart` (гонка по имени).

### Диагностика

```bash
systemctl status websdr.service receiver.service radiod@rx888.service
journalctl -u websdr -n 50 --no-pager
curl -sf http://127.0.0.1:8095/ && echo ok     # HTTP готов
```

Если водопад «пустой» сразу после старта — серверу нужно несколько секунд на
прогрев FFT- истории; обновите страницу.

---

## Тестовый сервер (клон)

Для разработки используется **локальный тестовый инструментарий** (второй
экземпляр сервера на порту **8090** с собственными FIFO и unit-файлами
`receiver-test.service`/`websdr-test.service`), позволяющий проверять клиент и
сервер, не влияя на основной :8095: оба сервера читают те же multicast-потоки
radiod, но каждый пишет в свои FIFO. Это внутренний инструмент проекта и **не
входит в состав поставки**; в документации упоминается только для пояснения,
почему возможен параллельный запуск.

---

## Клиент rx-* (HTML5)

Клиент — в `pub/`:

| Файл | Назначение |
|---|---|
| `websdr-head.html`, `websdr-controls.html` | разметка (граница фронтенда) |
| `classic.css`, `style.css`, `styles.less` | стили |
| `rx-base.js` | главный клиент (переработанный websdr-base.js: разделы, комментарии, фиксы) |
| `rx-waterfall.js` | HTML5-водопад (prep_html5waterfalls) — наш |
| `websdr-sound.js` | HTML5-звук (лицензия PA3FWM, см. раздел Лицензии) |
| `bandinfo.js` / `tmp/*.png` | генерируются сервером (сеть шкал) |

В `websdr-head.html` подключается `rx-base.js`; он динамически грузит
`rx-waterfall.js` (водопад) и `websdr-sound.js` (звук). Особенности:

- **reset/зум:** сервер пересчитывает спектр на каждый уровень зума (бина = 1px),
  а не растягивает готовую картину — это устраняет «ёлочки» при зуме.
- **fetchdx:** сервер пока не реализует `/~~fetchdx`, клиент после первого
  404 перестаёт его опрашивать и показывает только локальные метки станций
  из `stationinfo` (см. FAQ).

---

## Часто задаваемые вопросы

**Почему в консоли браузера видны 404 на `/~~fetchdx`?**
Сервер не реализует набор сетевых DX-сообщений. Клиент после первого 404
отмечает это флагом (`dxserverdead`) и больше не опрашивает. Локальные метки
станций (stationinfo) отображаются.

**Как добавить бэнд?**
1. Добавьте секцию `band <имя>` в `cfg/websdr.cfg` (centerfreq, samplerate,
   device…);
2. в `start-receiver.sh` добавьте строку `start_band <ssrc> "<band>-pcm.local"
   /home/radio/fifo/fifo<имя>`;
3. пересоздайте FIFO: `sudo mkfifo /home/radio/fifo/fifo<имя>`;
4. `./start.sh`.

**Как подобрать freqoffset?**
Наведитесь на известную станцию (напр., UVB 4625 кГц), посмотрите частотомер
клиента; разница в Гц (с учётом знака) — значение `freqoffset` для этого бэнда.

**Можно ли несколько серверов на одном приёмнике?**
Да: multicast-потоки radiod допускают любое число слушателей; каждый сервер
использует свои FIFO и свой `pcmrecord`. См. «Тестовый сервер (клон)».

---

## Лицензии и авторство

- Серверная часть и клиент `rx-base.js`/`rx-waterfall.js` — проект rx-websdr;
  в основе клиента — `websdr-base.js`/`websdr-waterfall.js` PA3FWM (WebSDR.org),
  используемые в рамках их лицензионных условий (неизменённое распространение
  через оригинальное ПО WebSDR; наш клиент — переработка для нашего сервера).
- `websdr-sound.js` — **WebSDR HTML5 client © PA3FWM**, используется в
  неизменном виде под его лицензией (см. шапку файла), с разрешёнными правками
  KA7OEI (de-emphasis, фильтры).
- `radiod`, `pcmrecord` — **ка9q-radio, Phil Karn KA9Q**:
  https://github.com/ka9q/ka9q-radio . Установка/настройка — по документации
  автора.
- `stationinfo.txt` — список станций; редактируется на сервере.
