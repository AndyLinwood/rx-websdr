# ПАМЯТКА — rx-websdr (сводка сессии 29.09)

> Вставляй первой репликой в новую сессию или просто скажи: «Открой ПАМЯТКА_RX_WEBSDR.md и продолжай работу с rx-websdr».
> Быстрая шпаргалка по модулю websdr.org: **ORGWEBSDR_NOTES.md** (рядом с памяткой).

---

## ⚠️ ТВЁРДОЕ ПРАВИЛО (установлено пользователем 14.10 — после серии потерь правок!)
**РЕАЛЬНЫЙ КОД ТОЛЬКО НА uhf (сервер). ЛЮБЫЕ ЛОКАЛЬНЫЕ КОПИИ (rx-websdr-work, rx-websdr-server, GitHub) — ЧИСТО РАБОЧИЕ ЧЕРНОВИКИ, НЕ ИСТОЧНИК ИСТИНЫ.**
1. Работаю ТОЛЬКО на сервере (`/home/radio/rx-websdr/`): правки вношу прямо там, файл за файлом.
2. Локальные папки использую только для чтения/анализа — но НИКОГДА не «восстанавливаю» и не деплою из них без сверки с сервером.
3. **КОММИТ/PUSH — ТОЛЬКО С UHF** (`sudo git ...` в `/home/radio/rx-websdr`). Никогда не коммитить из локальных копий, никогда не пушить локальный git.
4. После ЛЮБОЙ правки кода: закоммитить на сервере сразу (по просьбе пользователя или в конце сессии), чтобы правка не потерялась при откате/восстановлении из git.
5. Причина: неоднократные потери (Magic Eye 08.10, шрифт 8 с 05.10 — правильная правка месяцами жила только в локальной копии и «возвращалась» ущербной из git).

## СЕРВЕР
- SSH: `ssh andy@178.215.147.20 -p 4422`
- Проект: `/home/radio/rx-websdr/` — git-репо (ветка `main`, `origin → git@github.com:AndyLinwood/rx-websdr.git`), это же рабочая папка запущенного сервиса `websdr.service`
- Git только через sudo (`.git` принадлежит root):
  `sudo git -c user.name='Andy' -c user.email='alinwood@gmail.com' ...`
- Сайт: https://websdr.srr-76.ru/ (обновлять страницу: Ctrl+F5 / Cmd+Shift+R)
- Бэкап с прежним дизайном (источник истины по Magic Eye): `/home/radio/rx-websdr.bak-20260925-194439/pub/websdr-controls.html`

## ПРАВИЛА (обязательно)
1. Коммитить/push ТОЛЬКО по явной просьбе. Не своевольничать.
2. НИКОГДА не коммитить: `cfg/websdr.cfg` (там `adminpass` чата), `*.log`, `banlist.txt`. Коммить точечно: `git add <файл>` (не `git add .`, не `-a`).
3. Если сомневаешься в дизайне/коде — спроси пользователя. У него есть глаза, у агента — нет.
4. **НЕ ЗАТИРАТЬ ПРАВКИ ПОЛЬЗОВАТЕЛЯ НА СЕРВЕРЕ!** Правило после инцидента 08.10 (пользователь потерял правку html, сделанную на сервере, из-за моего деплоя):
   - ПЕРЕД заливкой ЛЮБОГО файла на сервер: `sudo git status` + `sudo git diff <файл>` на сервере. Если есть некоммиченные правки, которых НЕТ в моей локальной копии — СТОП, спрашивать пользователя.
   - Свои правки вносить ТОЛЬКО поверх актуальной серверной версии, а не из локальной копии, если серверная версия не совпадает с локальной (md5/diff).
   - Бэкапы `*.bak-<дата>`, снятые перед деплоем, не удалять до тех пор, пока изменения НЕ закоммичены в git.
   - Если пользователь говорит «правка потеряна» — не заниматься долгим поиском, а дать восстановить самому и ЗАФИКСИРОВАТЬ его версию (md5 + git diff), чтобы не затереть повторно.
   - Хеши/состояние после восстановления пользователем (08.10 12:44): `pub/rx-websdr.html` = `9fce4717…`, `pub/rx-base.js` = `d0944954…` (совпадает с моей копией). НЕ менять без явной команды.

## СЕРВИСЫ (systemd)
- `websdr.service` — прод (active, enabled), ExecStart=`rx-websdr -c cfg/websdr.cfg`
- `receiver.service` — pcmrecord → FIFO (active)
- `websdr-test.service` — тестовый клон :8090, **disabled** (поднимается вручную; автозапуск не нужен)

## СДЕЛАНО (закоммичено, push в main 29.09)
- **reload stationinfo** (коммит `eba0e62`): `./reload.sh` (SIGHUP) перечитывает `cfg/stationinfo.txt` (DX-метки) без рестарта/отключения клиентов. Код: `stationinfo_reload()` в `src/stationinfo.c`, вызов в `src/server.c` + перегенерация `bandinfo.js`.
- **Magic Eye** (коммит `9d40cd3`): вернул вид из бекапа 24.09 (см. ниже).
- В истории есть лишний мой коммит `52d090d` (ошибочная правка градиентов) — при желании чистить только по запросу (force-push).

## MAGIC EYE — ФИНАЛЬНОЕ РЕШЕНИЕ (НЕ ЛОМАТЬ!)
Точная копия прежнего дизайна из бэкапа `rx-websdr.bak-20260925-194439/pub/websdr-controls.html`:
- Обёртка: `div.progress-ring` (flex, `margin-right:-22px; margin-left:-9px; opacity:.9`)
- SVG: **56×56**, `background:green`, `border:2px solid rgb(43,43,43)`, `box-shadow:0 0 8px 0px rgb(13,224,12)`, `position:relative; bottom:50%; top:50%`, атрибут холста `width="120" height="120"`
- Дуги: 2 × `circle.progress-ring__circle`, `stroke-width="17px" r="20" cx/cy="28"`, `stroke-dasharray:125.664`, offset `125.664` / `-125.664`, `mix-blend-mode:lighten`, градиенты `#gradient`/`#gradient2`
- Тёмный центр: **отдельный div** `border-radius:50%; width/height:23px; background:#2d2d2d; position:relative; left:-41.5px; box-shadow:0 0 15px 1px rgba(0,0,0)`
- Расположение: **в потоке flex** (вписан в окно частоты, НЕ absolute)
- CSS: `.progress-ring__circle` — transition + rotate(-90deg) + `filter: drop-shadow(...)`
- Цифры частоты (`#freq-digits`): `text-align:center; padding-left:0`

### ГРАБЛИ (что ломало Magic Eye — никогда не повторять)
84px, absolute-позиционирование, радиальные градиенты, cx=60, `position:absolute` на flex-обёртке → овал, смещение, плоская заливка, разъезд. Правильный ответ всегда один: **скопировать блок из бекапа, не изобретать.**

## DSH / ПРОФИЛЬ (окружение агента)
- Конфиг: `~/.dsh/profiles/web/cordis.patch.yml`
- Провайдер `polza`, модель `deepseek/deepseek-v4-flash-0731`
- **БЕЗ поля `reasoningEffort`** — оно ломает старт новой сессии (polza не принимает ни `high-0731`, ни `high`). Рабочий вариант — просто без него.

## ОТКАТ (если что-то сломается на сервере)
Бэкапы на сервере:
- `pub/websdr.html.bak-magiceye-prev8` (и `-prev2..-prev7`) — версии Magic Eye
- `pub/websdr.css.bak-center-prev` — CSS до центрирования
- Восстановление: `sudo cp pub/websdr.html.bak-magiceye-prev8 pub/websdr.html && sudo chown radio:radio pub/websdr.html`
---

# СЕССИЯ 30.09 — КОСМЕТИКА (закоммичено `0141ab9`, push в main)

## UI-ПРАВКИ (коммит 0141ab9, 3 файла pub/)
- **S-метр**: калибровка −10 дБ через `SMETER_TRIM_DB = 10` в `pub/rx-base.js` (сервер завышал: станции «9 баллов» показывали 9+20 дБ). Вычитаем из `s` в `updatesmeter()`, `getnoise()`, `smetercallback` (squelch). SNR/MagicEye не трогаются (считаются как разница).
- **Метка шумового поля `#smetermin`** (бирюзовая ползающая точка) — УДАЛЕНА из `websdr.html`/`websdr.css` (display:none). Расчёт `smetermin` в rx-base.js ОСТАВЛЕН (питает SNR/MagicEye), убраны только обращения к визуалу.
- **Magic Eye**: сдвинут вправо в окне частоты: `margin-left:-9px → +23px` (итого +32px). МЕНЯТЬ ТОЛЬКО margin-left, НЕ margin-right (он задаёт границу зоны цифр — от него зависит «дыхание» цифр).
- **Радиус окна частоты** (`.digital-display`): `border-radius: 8px` (было 3px).
- **CSS версия в URL**: `websdr.css?v=rec4`.

# СЕССИЯ 30.09 — RSP1a / VHF (НЕ ДОКОНЧЕНО!)

## АРХИТЕКТУРА ПРИЁМА (важно!)
- **HF (0–30 МГц)**: `radiod@rx888` (RX888mk2) → RTP multicast → `pcmrecord -r -S <центр> <имя>-pcm.local > /home/radio/fifo/fifoXXX` → `websdr.service` читает FIFO.
- **УКВ 2m (145 МГц)**: `rtl_tcp_vhf.service` (RTL-SDR) → `websdr` читает `band 2m device !rtlsdr 127.0.0.1:1234` (rtl_tcp API).
- **RSP1a → УКВ**: radiod НЕ может работать с двумя устройствами (один процесс = один frontend, `hardware =` в [global]). Правильный путь: ВТОРОЙ radiod.

## ЧТО СДЕЛАНО НА СЕРВЕРЕ (30.09)
1. **`sdrplay-api.service`** (systemd, enabled): `/opt/sdrplay_api/sdrplay_apiService` — демон API SDRplay. БЕЗ НЕГО RSP1a не открывается (ошибка `sdrplay_api_Open(): sdrplay_api_Fail`).
2. **`/etc/radio/radiod@rsp1a.conf`** — второй radiod через **SoapySDR** (плагин `soapy.so`, НЕ `sdrplay.so` — его в билде НЕТ!). Секции:
   - `[global] hardware = soapy`
   - `[soapy] driver = sdrplay`, `samprate = 4M`, `frequency = 146000000`
   - `[VHF] mode = iq`, `data = vhf-pcm.local`, `freq = "146M"`
3. **`start.sh` обновлён** (бэкап `.bak-20260930-vhf`): добавлены `sdrplay-api` и `radiod@rsp1a` в стоп и старт (после rx888, до receiver); `N_BANDS` теперь считается по `^start_band `-строкам в start-receiver.sh (НЕ по band-строкам! 2m через rtl_tcp, не fifo).
4. **`start-receiver.sh`** (бэкап `.bak-20260930`): добавлена `start_band 146000 "vhf-pcm.local" /home/radio/fifo/fifoVHF`.
5. **`cfg/websdr.cfg`**: добавлен `band VHF` (device fifoVHF, samplerate 4000000, centerfreq 146000, gain 18). RTL `band 2m` ОСТАВЛЕН (два источника). НЕ КОММИТИТЬ config! (см. правила).

## СОСТОЯНИЕ ПРОВЕРКИ (30.09, до паузы)
- radiod@rsp1a работает: RSP1A виден через Soapy (`hardware RSP1A, RX channel 0`), sample rate 4 МГц, CS16, tuner 146 МГц, поток `vhf-pcm.local` опубликован (multicast, lo).
- pcmrecord VHF жив, fifoVHF наполняется (данные идут, средний |int16|≈600 — шум есть).
- websdr читает fifoVHF непрерывно (`FIFO VHF read blocked` — норм), клиент подключается: `[fft]/[audio] client band=VHF`.
- bandinfo: `VHF centerfreq=146000 samplerate=4000 maxzoom=5`, тайлы шкалы `tmp/<ts>-b10z*.png` ЕСТЬ (генерились раньше, 7 сен).
- **ПРОБЛЕМА (НЕ РЕШЕНА)**: у пользователя **водопад VHF не рисуется/не плывёт и звук полный ноль**, хотя шкала 144–148 верная. Гипотезы «слабый сигнал» ОТКЛОНЕНЫ пользователем (шум эфира есть всегда). Следующей сессии копать сюда.

## КУДА КОПАТЬ ДАЛЬШЕ (незавершённое)
- Водопад VHF стоит + звук = 0 при живом FIFO. Проверить по порядку:
  1. **Смапплированный сигнал до websdr**: реальная скорость данных через fifo (4 МГц CS16 = ~16 МБ/с; если реально КБ/с — pcmrecord не получает multicast!).
  2. **RTP поток radiod@rsp1a**: скорость/пакеты (tcpdump lo, порт 5006 — ctl, 5004 — rtp). Сравнить с рабочим rx888-потоком.
  3. **Порядок байт/формат**: rx888 vs rsp1a CS16 — вдруг radiod отдаёт другой порядок/размер сэмпла для soapy, и websdr читает мусор (тогда и водопад плоский, и звук нулевой). Формат: `pcmrecord -r` пишет сырое. Сравнить дампы fifoVHF и рабочего fifo40m (`od -t d2`).
  4. **`swapiq`/инверсия спектра**: в config.c swapiq ставится, но (по comment в waterfall.c) на водопад НЕ применяется. Для VHF можно попробовать добавить `swapiq` (как у 2m) — но сначала пункты 1–3.
- Потом (по желанию): второй бэнд 70см на том же radiod@rsp1a (радиод держит несколько демодуляторов).

## ЗАМЕЧАНИЕ ПОЛЬЗОВАТЕЛЯ (как следовать!)
- НЕ БРЕДИТЬ про кэш браузера / слабый сигнал — пользователь опытный инженер.
- Использовать `start.sh` для перезапуска стека (последовательность важна), не дёргать сервисы по одному.
## RSP1a/VHF — СЕССИЯ 30.09 ВЕЧЕР (что выяснили)

### ЧТО РАБОТАЕТ (проверено)
- RSP1a виден: `lsusb` = 1df7:3000, serial **1712023B93**
- `sdrplay-api.service` (демон SDRplay API) — активен, но НЕСТАБИЛЕН: **2 segfault'а за сессию** (16:42 и 19:15, см. `dmesg -T | grep segfault`), краш-репорт `/var/crash/_opt_sdrplay_api_sdrplay_apiService.0.crash`
- **Нативный плагин `sdrplay.so` СОБРАН** (из `/home/andy/ka9q-radio/src/sdrplay.c`, `make SOFLAGS=-shared sdrplay.so`) и установлен в `/usr/local/lib/ka9q-radio/`. Конфиг `radiod@rsp1a` переключен на `hardware = sdrplay` (НЕ soapy!). Лог: `Discovered SDRplay RSP device serial 1712023B93`, `Set sample rate 2,000,000 Hz`, `Locked tuner frequency 145,000,000`, `SDRplay RSP running`, `[vhf] 1 channel started` — **НО поток I/Q не отдаётся** (fifoVHF = 0 байт, радиод не молотит, callback не приходит).
- Пробовали ДО этого:
  - Soapy-путь (`hardware = soapy`, `driver = sdrplay`): тоже 0 поток. `SoapySDRUtil --probe` ВИДИТ устройство, но readStream всегда 0.
  - `rx_sdr -d sdrplay -f 145M -s 2M`: **0 байт** (даже простая утилита через Soapy).
- **Проблема глубже радиод/конфига**: RSP1a через sdrplay_apiService не отдаёт I/Q ни одному потребителю. Подозрение: **несовместимость sdrplay_apiService (3.15, май 2025) с ядром 6.8/Ubuntu 24.04/libusb** — сервис крашится при работе.

### ЧТО СДЕЛАНО НА ПРОДЕ (не коммить без запроса!)
- `/etc/radio/radiod@rsp1a.conf` — нативный sdrplay, `samprate=2000000` (не 2M — плагин ждёт ЧИСЛО В ГЦ!), `frequency=145000000`, `[VHF] freq="145M" low=-1M high=1M` (центр 145, полоса 144–146), `encoding = s16be` (сомнительно — КВ-секции без него работают; если VHF оживёт и будут проблемы — УБРАТЬ)
- `cfg/websdr.cfg` — `band VHF` добавлен после 2m: device fifoVHF, samplerate 2000000, centerfreq 145000, gain 18, `swapiq` (НЕ влияет — в коде не используется, но оставлен как у всех)
- `start-receiver.sh` + `start_band 146000 "vhf-pcm.local" ... fifoVHF` — **ЗАТЫЧКА! ssrc=146000 — после смены центра на 145 НЕЛЬЗЯ!** должен быть **145000**, иначе pcmrecord может не найти поток. (Хотя поток всё равно пуст.)
- `start.sh` — обновлён под sdrplay-api + radiod@rsp1a (стоп и старт), N_BANDS по строкам start_band

### КУДА КОПАТЬ ДАЛЬШЕ (когда вернёмся)
1. **[ОБЯЗАТЕЛЬНО]** поправить ssrc в start-receiver.sh: `-S 146000` → `-S 145000`
2. Обновить sdrplay_apiService: скачать свежий с sdrplay.com (3.15 устарел для ядра 6.8), заменить в /opt/sdrplay_api/ (бэкап!), перезапустить. Проверить `rx_sdr -d sdrplay -f 145M -s 2M` — если даёт байты, всё оживёт.
3. Если новый API тоже молчит: попробовать нативный sdrplay.so БЕЗ sdrplay_apiService (он умеет сам?) или другой транспорт.
4. Проверить USB-конфликт: RSP1a на USB2 (Bus 001) вместе с RTL — можно временно вынуть RTL и посмотреть.
5. Вопросы фильтров/усиления RSP (из [sdrplay] ключи: lna-state, rf-gr, biasT, notch, if-agc и т.д.) — смотреть sdrplay.c `Sdrplay_keys[]` (строки 153-186) и README ka9q.

### ССТОП-МОМЕНТ
- Прод ЖИВ: HF + RTL-2m работают, https отдаёт 200. VHF-бэнд в вебе ЕСТЬ, но без данных (водопад/звук пусты).
- Ничего не коммитить без явной просьбы.

### ИТОГ 30.09 ПОЗДНО ВЕЧЕР (дополнение к RSP1a)
- **Подтверждено из официального README GitHub ka9q**: «SDRplay ... now optionally supported» — плагин официальный, конфиг наш верный.
- **Подтверждено из исходника sdrplay.c (главный вывод)**: `sdrplay_monitor()` **сам выходит, если сэмплы не растут** (`if(!(curr_samples > prev_samples)) break;` => "Device is no longer streaming, exiting"). Это объясняет, почему радиод rsp1a «жив, но молчит» и периодически рестартует.
- **ПРОБЛЕМА НЕ В КОНФИГЕ**: ни Soapy, ни нативный sdrplay.so, ни даже `rx_sdr -d sdrplay` не получают НИ ОДНОГО байта. Дело в sdrplay_apiService/драйвере/железе.
- **Сегфолты в libusb-1.0.so.0.4.0 у ДВУХ клиентов**: `radiod[3150/3161/3171]` (16:28, это было ДО моих работ — при Soapy) и `sdrplay_apiService` (16:42, 19:15). Паттерн: RSP1a + Ubuntu 24.04 + ядро 6.8 + libusb = падение клиента. Это **системная несовместимость**, НЕ конфиг radiod.
- **Открытый mcedit**: на сервере висят `11203/11209/11210 mcedit /etc/radio/radiod@rsp1a.conf` (открыт конфиг в редакторе с 11:40!). Перед любыми правками КОНФИГА радиода — закрыть: `pkill -f mcedit` (или Ctrl+C в той сессии). ВАЖНО: он ЕСТЬ и может мешать.
- **СЛЕДУЮЩИЙ ШАГ (когда решим продолжать)**:
  1. Закрыть mcedit.
  2. Либо: попробовать починить libusb/ядро (переустановка libusb-1.0, ядро другое) — долго.
  3. Либо: признать RSP1a нерабочим на этом сервере и вернуть VHF на RTL-2m (работает). УКВ-70см тогда не добавим на RSP1a.
  4. Конфиг radiod@rsp1a НЕ КОММИТИТЬ (рабочий черновик), в git он и не должен попадать (не в pub/).

# СЕССИЯ 30.09 ВЕЧЕР — RSP1a VHF РАБОТАЕТ НА ПРОДЕ, ТЕСТ-ПЛОЩАДКА СЛОМАНА МОИМИ ДЕЙСТВИЯМИ

## ЧТО РАБОТАЕТ (ПОДТВЕРЖДЕНО, НЕ ТРОГАТЬ!)
- **ПРОД VHF (RSP1a, Soapy)**: центр 145 МГц, полоса 6 МГц (144-148), поток полный.
  - Конфиг `/etc/radio/radiod@rsp1a.conf`: `hardware = soapy`, `[soapy] driver=sdrplay samprate=6M frequency=145000000`, `[VHF] samprate=6M low=-3M high=3M freq="145M" agc=0 gain=22`, секция [soapy] также `gain = 30` (текущее, подбиралось).
  - `cfg/websdr.cfg` band VHF: device fifoVHF, samplerate 6000000, centerfreq 145000, gain 18, swapiq.
  - `start-receiver.sh` (прод): `start_band 145000 "vhf-pcm.local" ... fifoVHF`.
  - websdr читает: `Band VHF: running (rate=6000000 Hz, center=145000)`, feed стабилен.
- **RTL-2m, HF (RX888) — работают.**

## ГЛАВНЫЙ УРОК СЕГОДНЯ (НЕ ПОВТОРЯТЬ!)
- **НЕ запускать руками процессы теста параллельно с systemd-юнитами!** Я насоздавал ручных `start-receiver-test.sh`, они конфликтовали с юнитами и давали неполные/дублирующие писатели. Чистый рестарт юнита даёт все 13.
- **НЕ читать fifo через cat/dd во время работы websdr** (cvaraет данные у ридера!). Проверять только журналы (`journalctl -u websdr-test`), lsof, ss.
- Симптом «ничего не работает» на тесте часто = **нет клиента-браузера**: `measured feed` печатается ТОЛЬКО когда открыта страница `http://IP:8090/`. Данные могут идти, а «running/measured» не показаны.

## ТЕСТОВАЯ ПЛОЩАДКА (состояние на конец сессии — НЕ ДОВЕРЕНА)
- Юниты: `receiver-test.service` (start-receiver-test.sh, fifo-test), `websdr-test.service` (:8090).
- `start-receiver-test.sh` — VHF вставлен на строку 15 (корректно).
- НО: за сессию я насоздавал ручных процессов (23345, 23638...) — **есть риск, что они мешают**. Если тест не оживает: `sudo pkill -f start-receiver-test; sudo systemctl restart receiver-test; sudo systemctl restart websdr-test; затем открыть в браузере http://178.215.147.20:8090/`.
- Прошлый успех: после `systemctl restart receiver-test.service` дети = все 13 писателей (проверено: 25032 VHF и др.)

## RSP1a УПРАВЛЕНИЕ (для будущего)
- Нативно (sdrplay.so) — полный набор (bandwidth/lna-state/if-*). НО нативный на этом API пока НЕ даёт поток (sdrplay_apiService segfault). Оставлен soapy-путь.
- Soapy-путь: gain/atten/agc работают (gain == attenuation, setGain(gain-atten)).
- Разработка официально поддерживает sdrplay (README ka9q).

## ОБРАТИТЬСЯ К ПОЛЬЗОВАТЕЛЮ
- Извиниться за хаос; предложить: завтра с чистого листа — протестировать ПРОД-VHF (работает!), потом аккуратно тестовую площадку через юниты.

# СЕССИЯ 01.10 — ГЛУБОКИЕ CCЫЛКИ (?tune=...)
- Ссылка формата ua3mrs: `https://websdr.srr-76.ru/?tune=145725000,mod=nfm` — **РАБОТАЕТ** (проверено пользователем).
- Правка в `pub/rx-base.js` (НЕ КОММИТИЛ, ждать отмашку):
  1. `setfreqtune()` — парсит `<Гц>,mod=<режим>`; частота переводится Гц→кГц (`freq_hz/1000`).
  2. `set_mode()` — добавлены `case "NFM"/"NBFM"` → FM (раньше только FM/AM/USB/LSB/CW).
- `sql=` в ссылке ИГНОРИРУЕТСЯ (пользователь выбрал «tune+mod без sql», но формат ссылки это не ломает).
- Формат совместим с ua3mrs.ru: упомянут в websdr.html на кнопку круглого стола (но та ссылка ведёт на ua3mrs — внешний).
- Деплой: scp rx-base.js → /tmp → sudo cp + chown radio:radio (md5 сверил, https отдаёт новую версию, кэш caddy не держит).

# СЕССИЯ 01.10 — ПЕРЕИМЕНОВАНИЕ websdr.html/css → rx-websdr.html/css (ЗАКОММИЧЕНО `e3f0025`, PUSH ✅)
- Сделано на сервере:
  - `sudo git mv pub/websdr.html pub/rx-websdr.html` (+`websdr.css` → `rx-websdr.css`), chown radio:radio.
  - `pub/index.html` (SSI: `<!--#include file="html">`) → `rx-websdr.html`.
  - В `rx-websdr.html` строка 13: `href="rx-websdr.css?v=rec5"` (бамп v=rec4→rec5 для сброса кэша).
- Проверено по https: `/` 200, `rx-websdr.html` 200, `rx-websdr.css` 200, старые имена → 404. Caddy не держит кэш.
- В коде сервера жёстких ссылок на имена не было (serve_file отдаёт по URI) — бэкенд не трогали.
- **ВАЖНО**: локальная копия `rx-websdr-work/pub/` синхронизирована с сервером (rename + index.html + rx-base.js от 10:16). `websdr.html/websdr.css` в pub/ больше НЕТ (удалены локально).
- Коммиты 01.10 (push в main, origin/main == 727f43e):
  - `e3f0025` refactor(pub): переименование websdr.html/css → rx-websdr.html/css (index.html SSI + css?v=rec5)
  - `b571733` feat(client): глубокие ссылки ?tune=<Гц>,mod=<режим> (NFM/NBFM→FM); isVHF по имени бэнда 2m; чистка «№ разделов» в комментариях rx-base.js
  - `727f43e` feat(station): метка 145725fm RR3M; VHF start_band ssrc 145000; start.sh: N_BANDS по start_band, sdrplay-api+radiod@rsp1a в стек
- ВАЖНО (из commit 2): в `setband()` VHF теперь определяется по `bi[band].name == '2m'` (а не 'VHF', как было в локальной копии). Если прод-VHF-бэнд в `cfg/websdr.cfg` называется иначе — squelch/FM-авторежим не включится. Проверить на проде!

## ПРАВИЛО ОТ ПОЛЬЗОВАТЕЛЯ (01.10, после коммитов)
- Пользователь сам вносит правки в файлы проекта (не спрашивать, не комментировать, не «чинить»).
- Коммит/push — ТОЛЬКО по явной команде «коммить/закоммить/commit». До команды ничего не делать.
- Задача на сессию приходит отдельным сообщением пользователя (не в файлах).

## УБОРКА ПРИ ОКОНЧАНИИ РАБОТЫ (ПРАВИЛО С 01.10)
- При объявлении «на сегодня заканчиваем» — прибрать рабочую папку на сервере `/home/radio/rx-websdr/`.
- Что делать: удалять временные бэкапы (`.bak-*`, кроме свежего на каждый скрипт), временные файлы, хвосты от экспериментов.
- НЕ удалять: `cfg/websdr.cfg`, `*.log`, `banlist.txt`, `visitors.log`, рабочие pub/src.
- Удаление файлов из git (`.gitignore` не покрывает) оставляет `D` в статусе — не коммитить без команды.
- Пример уборки 01.10: удалены `start-receiver.sh.bak-20260930`, `start.sh.bak-20260930-vhf`; оставлены свежие `start-receiver.sh.bak-20260930-146m`, `start.sh.bak-rtlclean`.

## RTL-SDR — ПОЛНОСТЬЮ УБРАН (01.10)
- `rtl_tcp_vhf.service` — остановлен, disabled, ФАЙЛ ЮНИТА УДАЛЁН (`/etc/systemd/system/rtl_tcp_vhf.service`), daemon-reload.
- `rtl_tcp_vhf.sh` — удалён, **ЗАКОММИЧЕНО `21c8330` (chore(rtl)) + push**, origin/main = 21c8330.
- `start.sh` — почищен от всех упоминаний rtl_tcp (проверка `systemctl is-active rtl_tcp_vhf` и echo удалены).
- VHF теперь ТОЛЬКО на RSP1a (band 2m → fifoVHF). B cfg/websdr.cfg осталась закомм. строка `#device !rtlsdr 127.0.0.1:1234` (инертный комментарий).
- Прод после чистки: 5 сервисов active, VHF-писатель жив, HTTP 200.
- РАБОЧЕЕ ДЕРЕВО СЕРВЕРА ЧИСТОЕ (git status пуст). Локальная копия синхронизирована (start.sh, start-receiver.sh).

## HTTPS / Caddy — РЕШЕНО ОКОНЧАТЕЛЬНО (01.10)
- Оставляем Caddy как reverse proxy (НЕ убираем). Пользователь интересовался, но решил не менять.
- Почему: у OpenWebRX+/WebSDR-classic встроенного TLS нет (архитектурно через proxy); системный libwebsockets собран БЕЗ SSL («SSL disabled»); встроенный TLS = пересборка + крон продления — нет смысла.
- Факты для справки:
  - Caddy: `/etc/caddy/Caddyfile` = `websdr.srr-76.ru { reverse_proxy 127.0.0.1:8081 }`, v2.11.4.
  - Сертификаты LE у Caddy: `/var/lib/caddy/.local/share/caddy/certificates/acme-v02.api.letsencrypt.org-directory/websdr.srr-76.ru/` (валиден до 27.12.2026, автопродление).
  - `websdr.service` слушает `:8081` (cfg `tcpport 8081`), от root.
  - Если Caddy убрать: порт 80 можно биндить напрямую, но HTTPS/обновление cert потеряем.

## УПРАВЛЕНИЕ УСИЛЕНИЕМ RSP1a БЕЗ РЕСТАРТА — НАЙДЕНО (01.10, проверено!)
- **`tune` (ka9q-radio)**: консольная команда, шлёт ControlRPC по multicast — НЕ TUI, не рестарт.
  - Статус (read-only): `tune -v -r rsp1a -s 145000`
  - RF усиление: `tune -r rsp1a -s 145000 --rfgain <дБ>` / `--rfatten <дБ>`
  - Частота/полоса/AGC/канальный gain: `-f <Гц>`, `-L/-H`, `-a`, `-g <дБ>`
- SSRC VHF = **145000** (= centerfreq из `start_band 145000 "vhf-pcm.local"`), `-r rsp1a` = `status rsp1a.local`.
- **Soapy-путь** (текущий `hardware = soapy`) поддерживает только `gain/atten/agc`. `lna-state/rf-gr/if-gr/bias-t/notch` есть только в нативном плагине sdrplay (не давал поток).
- Изменения применяются в RAM, до рестарта radiod; после рестарта вернётся `gain = 36` из `/etc/radio/radiod@rsp1a.conf` ([soapy] gain) / `22` ([VHF] gain).
- Пользователь сам подбирает усиление — НЕ трогать, только если попросит.

## DC-ПОЛОСА НА ЦЕНТРЕ VHF (145000) — РЕШЕНО, НЕ ЧИНИМ (01.10)
- На водопаде 144–146 МГц: «шумовая полоса» на **145000** = DC/LO-природа прямого преобразования RSP1a (zero-IF); «полоса» на **144000** = нижний край окна (center−samplerate/2). Это НЕ неисправность.
- Исследовано: SoapySDRplay 0.5.2 (`libsdrPlaySupport.so 0.5.2`) поддерживает `iqcorr_ctrl` (IQ+DC коррекция) через args → writeSetting, НО по умолчанию **уже включена** («DC correction: on, IQ balance: on» в Settings.cpp). args не помогут — эффекта не будет.
- Вывод: оставляем как есть. Пользователь знал про DC при использовании RSP. НЕ предлагать снова чинить/двигать центр.
- Ссылка: исходник pothosware/SoapySDRPlay3, Settings.cpp (writeSetting iqcorr_ctrl: IQenable=1;DCenable=1).

## VHF/2м «НЕТ ПРИЁМА» — РЕШЕНО 01.10 ВЕЧЕР (нативный sdrplay.so ЗАРАБОТАЛ!)
### ГЛАВНЫЙ УРОК: симптом «2м молчит» = умер pcmrecord VHF, а start-receiver.sh НЕ перезапускает мёртвых!
- start-receiver.sh (рестарт) перезапускает ВСЕ pcmrecord (write «wait» ждёт всех, но мёртвого НЕ возрождает; при рестарте radiod@rsp1a поток пропадает → pcmrecord VHF умирает EPIPE).
- Лечение: `sudo systemctl restart receiver.service` — все писатели (вкл. VHF) оживают. ПРОВЕРЯТЬ `pgrep -a pcmrecord | grep vhf` при жалобах «2м нет».
- 11 писателей: 10 бэндов + ft8 (32403 декодер).

### НАТИВНЫЙ ПЛАГИН sdrplay.so — РАБОТАЕТ (01.10 20:04, переключен на проде!)
- `hardware = sdrplay` (НЕ soapy), секция `[sdrplay]`. **ФИНАЛЬНАЯ РАБОЧАЯ НАСТРОЙКА 01.10 21:30**:
  - `samprate = 6000000` — НЕ «6M» (плагин ждёт ЧИСЛО В ГЦ, иначе "sample_rate=6.000000 is invalid")
  - **`lna-state = 3`** (18 dB) + **`if-gr = 40`** + `if-agc = false` + `dc-offset-corr = true` + `iq-imbalance-corr = true`
  - Только ОДИН из lna-state/rf-att/rf-gr (иначе "only one of ... is allowed").
  - **ПОДБОР УСИЛЕНИЯ (результат экспериментов 01.10)**:
    - RSP1a таблица LNA для 145М: state 0..9 = 0,6,12,18,20,26,32,38,57,62 dB.
    - lna 0/if59: чисто, но слабые станции тихо. lna 4/if40: перегруз (прямой сигнал на репитер 145350 пропадает, «чувствительность хуже»). lna 3/if20: гармоники репитера. **lna 3/if40: гармоник нет + прямой сигнал виден — ЭТО ФИНАЛ.** lna 3/if30: слабые гармоники.
    - Симптомы перегруза RSP1a: кратные гармоники репитера по водопаду + «темнеет» шумовая дорожка + пропадает сильный прямой сигнал.
    - Решение пользователя: «лучший усилитель это антенна» — дальше НЕ крутить, если не попросит.
- Логи: «RF LNA state 3, IF att 40, IF AGC 0, DC offset corr 1, IQ imbalance corr 1», «SDRplay RSP running», каналы [vhf]+[ft8].
- **Управление усилением — ТОЛЬКО через нативные ключи конфига** (`lna-state`, `rf-att`, `if-gr`) + рестарт radiod@rsp1a. `tune --rfgain` нативный ИГНОРИРУЕТ (RF Gain всегда 0). (Soapy-путь `--rfgain` крутил только IFGR-ослабление, инвертирован.)
- **⚠️ ПОСЛЕ КАЖДОГО `systemctl restart radiod@rsp1a` ОБЯЗАТЕЛЬНО `sudo systemctl restart receiver.service`** — иначе pcmrecord VHF умирает (EPIPE при пропаже потока) и start-receiver.sh НЕ возрождает мёртвых → «2м без приёма». Проверка: `pgrep -a pcmrecord | grep vhf`.
- БЭКАПы: `/etc/radio/radiod@rsp1a.conf.bak-soapy` (Soapy-вариант), `.bak-good-lnastate4` (промежуточный).
- sdrplay_apiService периодически крашится (сегфолты), но radiod переживает — поток восстанавливается после рестарта API/radiod.

# СЕССИЯ 02.10 — ГЛОБАЛЬНЫЙ МОНИТОРИНГ (registry) ПЕРЕРАБОТАН (коммиты 83d4d5b, c6af19b, e5cdbb6 — push в main)

## АРХИТЕКТУРА (как стало)
- **endpoint ЗАШИТ В КОД**: `registry.c` → `https://srr-76.ru/api.php` (константа протокола, из cfg НЕ читается). Белых списков/ручного ввода id НЕТ.
- **server_id авто-генерируется** из `registry_website` (hostname URL), фоллбэк — hostname машины. Явный `registry_server_id` в cfg — опциональное переопределение (у нас в cfg он есть: `yaroslavl-rx888`).
- **Токен-рукопожатие (защита от мусора вместо allowlist)**: новый сервер → агрегатор отвечает `need_token` + `verify_token` → клиент возвращает токен след. heartbeat'ом → запись создаётся. Спам без токена не проходит, всё автоматом.
- **Интервал heartbeat** берётся из ответа агрегатора (`next_heartbeat_in`, 60 → шлём ~48с), НЕ из cfg.
- **`registry_interval`/`registry_max_users`/`registry_endpoint` в cfg УДАЛЕНЫ** (не читаются). Лимит слушателей — из `maxusers` конфига.
- **Заголовок `x-rx-websdr: <server_id>`** добавлен во все HTTP-ответы (server.c `serve_file`+`serve_mem`) — маркер настоящего rx-websdr для верификации агрегатором.

## ЧТО НА ПРОДЕ (проверено)
- registry.c с токен-логикой + интервал по ответу; `websdr.service` перезапущен; сервер **online, bands=['VHF','HF']** на https://srr-76.ru/listsdr.html.
- api.php агрегатора (хост srr-76.ru, НЕТ доступа у меня) пользователь обновил сам — токен-механика, `tokens.json` рядом с `servers.json`.
- Связь сервер→srr-76.ru НЕСТАБИЛЬНА (~25% TCP-timeout/медленные ответы, «чёрные дыры» по 2-15с) — НЕ код, а маршрут провайдера. Клиент переживает за счёт short-timeout 40с + ретраи.

## ДОКУМЕНТАЦИЯ (в git, push ✅)
- `cfg/websdr.cfg.example` — секция Registry приведена к коду (endpoint/interval/max_users убраны, server_id закомментирован с пояснением).
- `README.md` — «12 диапазонов» убраны (в 4 местах), FIFO-установка → ссылка на `sfifo.sh`, раздел «Лицензии и авторство» переписан (PA3FWM all-rights-reserved / ka9q GPL v3 / jQuery MIT), раздел «Глобальный мониторинг» синхронизирован с кодом.
- `sfifo.sh` — НОВЫЙ скрипт-пример создания FIFO-каналов (в корне репо, executable, тестирован на /tmp).
- `install.sh` — список FIFO без «12 бэндов».

## ГРАБЛИ/ЗАМЕТКИ
- **НЕ ЛОМАТЬ**: токен одноразовый, клиент очищает его после успешной отправки; при `need_token` ответе интервал от агрегатора 10с (ускоряется до 8с).
- Для нового сервера в сети: достаточно `registry_enabled yes` + `registry_website` — появится в мониторинге сам через ~1-2 мин (токен-рукопожатие). НЕ надо ничего вписывать в api.php.
- `nohup ... &` в командной строке ssh убивает ssh (exit 255) — запускать фоновые демоны через `ssh -f` или с `& disown` + отдельную команду.
- Правки rx-websdr.html на проде (пользовательские) — не коммитить без команды; сегодняшние коммиты сделаны по явной просьбе.

# СЕССИЯ 08.10 — SEO / «Страница с переадресацией» + 404 (НЕ ЗАКОММИЧЕНО, ждать команду!)

## ДИАГНОЗ «Страница с переадресацией» (Google)
- **Главный источник**: Caddy редиректит `http://websdr.srr-76.ru/*` → 308 на https. Googlebot ходит по http → видит «переадресацию» → помечает страницы.
- `www.websdr.srr-76.ru` ПРОПИСАН в DNS (тот же IP), но сайта на Caddy для www НЕТ → https://www… тупик, у Google «redirect + nothing».
- НЕ было canonical (дубли `/`, `/index.html`, `/rx-websdr.html`, `/?tune=...`), НЕ было robots.txt/sitemap.xml.
- 404 отдавался «голым» (стандартный lws) — в т.ч. для старых имён websdr.html/css (после переименования 01.10).

## ЧТО СДЕЛАНО НА ПРОДЕ (деплой ✅, НЕ коммичено)
- **`pub/404.html`** (новый): стилизованная 404 в теме сайта, `meta robots=noindex`.
- **`src/server.c`**: `serve_file()` для отсутствующего `.html` отдаёт `pub/404.html` со **статусом 404** (раньше — голый ответ). MIME: добавлен `.xml → application/xml` (для sitemap).
- **`pub/robots.txt`** (новый): Allow /, Disallow служебных `/~~admin /~~chat /~~othersjj /~~ft8 /tmp/ /?admin /?action=* /*.bak*`.
- **`pub/sitemap.xml`** (новый): `/`, `/index.html`, `/rx-websdr.html`.
- **`pub/rx-websdr.html`**: добавлены `<link rel=canonical href="https://websdr.srr-76.ru/">` + `<meta name=description>`.
- Бинырь пересобран на сервере (`sudo make` в /home/radio/rx-websdr), стек перезапущен **только через start.sh**. Бэкапы: `pub/*.bak-20261008`, `src/server.c.bak-20261008`, `rx-websdr.bin.bak-404` (в /home/radio/).

## ПРОВЕРЕНО (после деплоя)
- `/` 200; `/robots.txt` 200 text/plain; `/sitemap.xml` 200 **application/xml**; `/websdr.html` 404 (стилизованная 404.html); `/foo.html` 404; `/несущ.xyz` (без .html) 404 (голый — так и задумано для не-HTML).
- Homepage: canonical + description присутствуют. X-RX-WEBSDR header на месте.

## ЧТО ОСТАЛОСЬ (спросить пользователя / незакрыто)
- **СДЕЛАНО 08.10 (после основного деплоя)**: 301-редирект старых имён в `ws_handler` (server.c):
  `/websdr.html` → `/rx-websdr.html`, `/websdr.css` → `/rx-websdr.css` (HTTP 301 MOVED_PERMANENTLY, Location, content-length 0).
  Проверено: https://websdr.srr-76.ru/websdr.html → 301 → rx-websdr.html 200 (в т.ч. с `?v=...`).
  Бэкап: `src/server.c.bak-20261008b` на сервере.
- **www-домен — РЕШЕНО НЕ ТРОГАТЬ** (пользователь: «устарело и очень давно»). В DNS www.websdr.srr-76.ru остаётся, сайта для www нет — не вопрос.
- **ПОЧТА r3mav@srr-76.ru — НЕ СУЩЕСТВОВАЛА в коде/бэкапах** (пользователь решил, что «приснилось»). Поиск: git -S 'r3mav'/'@srr-76' пуст во всей истории; нет ни в pub/*.bak-*, ни в бэкап-папке 25.09, ни в локальных копиях. R3MAV встречается только как `registry_owner_call` в README.md:345.
- Коммит НЕ делался — ждать явной команды (в рабочем дереве сервера: server.c, rx-websdr.html, 404/robots/sitemap; локально rx-websdr-server отражает всё это).

# СЕССИЯ 08.10 (вечер) — websdr.org РЕЙТИНГ: модуль orgserver (РАБОТАЕТ, НЕ ЗАКОММИЧЕНО)

## ПРОТОКОЛ websdr.org (раскрыт перехватом tcpdump у оригинального websdr64)
- Дистрибутив оригинала: `/home/andy/Дистр_websdr/websdr-dist11/dist11/` (websdr64).
- **Heartbeat (раз в 60 сек)**: `GET /~~websdrorg?host=<hostname>&port=<port>` на `websdr.ewi.utwente.nl:80` → ответ `ok`. Без hostname — `?port=<port>` (рейтинг покажет IP).
- **Deregister**: `GET /~~websdrNOorg?port=<port>` (при остановке).
- **Опрос рейтингом**: рейтинг сам заходит на `http://<host>:<port>/~~orgstatus?config=N&token=0` и получает:
  ```
  Config: <число — СТАБИЛЬНОЕ на время жизни процесса!>
  Qth: LO07AQ98
  Description: <ASCII, не кириллица!>
  Logo: favicon.png
  Email: <XOR-обфускация: каждый байт ^1, НЕ +1/-1!>
  Bands: N
  Band: <idx> <centerkHz.6f> <widthkHz.6f> <antenna>   (6 знаков после запятой!)
  Users: <реальное число слушателей>
  ```
- **Критично для попадания в список** (выяснено экспериментально):
  1. `Config` должен быть **стабилен** между запросами (иначе рейтинг считает сервер нестабильным и НЕ включает).
  2. Ответ должен содержать заголовки **`Server: WebSDR/20140718.1716-64`** и **`Set-Cookie: ID=...`** (рейтинг проверяет, что это настоящий websdr).
  3. `Description` — только ASCII (UTF-8 кириллица ломает).
  4. `Band:` — 6 знаков после запятой (%.6f).
- Рейтинг ТРЕБУЕТ, чтобы host:port был **доступен снаружи**.
- **Рейтинг перезагружается 1-2 раза в сутки** — список КЭШИРУЕТСЯ, новый сервер может появиться не сразу (до следующей перезагрузки). Проверять: `curl http://websdr.ewi.utwente.nl/~~websdrlistk | grep srr-76`.

## АРХИТЕКТУРА РЕШЕНИЯ (попытка №2, после аварии попытки №1)
- **Попытка №1 (провалилась)**: встраивали orgserver в исходники rx-websdr (orgserver.c/h, правки main.c/server.c/config.c/websdr.h). Из-за **рассинхрона локальной копии websdr.h** (в ней не было ctcss_freq из коммита 1599217) бинарь собрался с несогласованными структурами → websdr зависал, не отвечал на HTTP. **Откачено** (бинарник 139824, исходники к 033bf60).
- **Попытка №2 (РАБОТАЕТ)**: **автономный отдельный модуль** `/home/radio/orgserver/orgserver` + `orgserver.cfg` — свой процесс, свой порт, никак не трогает основной rx-websdr. Основной НЕ пересобирался!
  - Слушает :8091 (проброс на роутере: внешний 8091 → uhf 8091).
  - Heartbeat `~~websdrorg?host=websdr.srr-76.ru&port=8091` раз в 60с.
  - `~~orgstatus` отдаёт данные из orgserver.cfg + **реальный Users** (опрашивает бэкенд `~~othersjj` на 127.0.0.1:8081, парсит numusersobj.innerHTML).
  - **Проксирует ВСЁ остальное** (/, favicon, ws) на основной rx-websdr (127.0.0.1:8081) — посетитель websdr.org попадает на живой приёмник.
  - Заголовки Server/Set-Cookie — как у websdr64.

## СОСТОЯНИЕ НА 08.10 КОНЕЦ (для продолжения дома)
- Модуль работает на uhf: процесс `orgserver`, порт 8091, heartbeat → 200.
- `orgserver.cfg`: hostname websdr.srr-76.ru, port 8091, qth LO07AQ98, desc ASCII «RX-WebSDR Yaroslavl (R3MAV) - HF + 2m VHF online SDR receiver», logo favicon.png, email r3mav@srr-76.ru, backend 127.0.0.1:8081.
- Проверено снаружи: orgstatus отдаёт данные, `/` → 200, заголовки Server/Set-Cookie верные.
- **В списке websdr.org пока НЕТ (есть старый тест 8090)** — ждать перезагрузки рейтинга (1-2 раза/сутки).
- **ПОСЛЕДНЯЯ ПРОВЕРКА (вечер 08.10)**: даже после исправления Config/Users/Server-заголовков наш 8091 в списке НЕ появился (только старый тест 8090). Рейтинг кэширует ~сутки — проверить в НОВОЙ сессии: `curl -s http://websdr.ewi.utwente.nl/~~websdrlistk | grep srr-76`. Если так и нет — копать дальше (см. ORGWEBSDR_NOTES.md).

## ЧТО ОСТАЛОСЬ СДЕЛАТЬ (следующая сессия)
1. **Убрать старый тест 8090 из рейтинга**: остановить websdr64 на 8090 (он отправит дерегистрацию) — `/home/andy/wsdr-test/dist11/websdr64` (запущен sudo nohup).
2. **Проверить появление 8091 в списке**: `curl -s http://websdr.ewi.utwente.nl/~~websdrlistk | grep srr-76` (после перезагрузки рейтинга).
3. **Автозапуск модуля**: сделать systemd unit `/etc/systemd/system/orgserver.service` (ExecStart=/home/radio/orgserver/orgserver /home/radio/orgserver/orgserver.cfg, Restart=always, User=radio) + enable.
4. **Почистить временное**: `/tmp/og.log`, старый тестовый websdr64 (после дерегистрации можно удалить /home/andy/wsdr-test и дистрибутив — спросить пользователя).
5. **Занести в git** (по команде): `orgserver.c`, `orgserver.cfg`, systemd unit — в `rx-websdr-server` (или отдельный каталог проекта).
6. Проверить, что **основной rx-websdr не затронут** (8081, сайт, писатели 11).

## ВАЖНЫЕ УРОКИ (НЕ ПОВТОРЯТЬ)
- **Не собирать по несогласованной локальной копии**: взять исходники из git (или с сервера), а не из rx-websdr-work, где websdr.h мог быть старым. Сверять `git status`/`git diff` перед сборкой.
- Рейтинг проверяет: стабильный Config, Server-заголовок, ASCII-description, %.6f, доступность порта снаружи.
- Обфускация email = XOR с 1 (не +1/−1).

## СЕССИЯ 09.10 — websdr.org: ПРОТОКОЛ РАЗОБРАН ОКОНЧАТЕЛЬНО (ЭКСПЕРИМЕНТАМИ)

### КАК РЕАЛЬНО РАБОТАЕТ РЕГИСТРАЦИЯ (проверено tcpdump)
- websdr.org (192.87.173.88) **сам ходит к серверу** после heartbeat: `GET /~~orgstatus?config=<реальный Config сервера>&token=0`.
  - К ПРИНЯТОМУ websdr64 ходит с **реальным Config** (например `config=1791531154`), НЕ с 0.
  - К нашему orgserver ходил с `config=0` — признак, что сервер **не в базе/не принят**.
- Ответ на orgstatus (эталон websdr64, принят в список):
  ```
  Config: 1791531154        ← = time(NULL) в момент старта, СТАБИЛЕН
  Qth: LO07AQ98
  Description: ...
  Logo: logo_rd3mk.png
  Email: <XOR ^1>           ← НЕ +1/−1, а XOR с 1
  Bands: 1
  Band: 0 14444.000000 48.000000 funny dipole    ← центр/ширина %.6f
  Users: 0
  ```
  Заголовки: `Server: WebSDR/20140718.1716-64`, `Set-Cookie: ID=...`, `Content-Type: text/plain`, `Cache-control: no-cache`.
- **`Mobile: m.html` в orgstatus НЕ обязателен** (у локального websdr64 его нет; у Maasbree есть — рейтинг принимает оба). Не добавлять.
- Bands могут быть любыми (в списке есть серверы с >8; у websdr64 почему-то лимит ≤8 при ЗАПУСКЕ, но рейтингу всё равно). Антенна `?` — некритично.

### ВАЖНО: ЗАЩИТА websdr.org ОТ ЧАСТЫХ РЕГИСТРАЦИЙ
- После остановки сервера/смены порта/перезапуска **регистрация приостанавливается на ~20-30 мин** (не точно). Список помнит старый сервер, но новый (или тот же после рестарта с новым Config) НЕ пускает сразу.
- **Процедура чистого запуска**: остановить ВСЕ имитаторы (websdr64 и orgserver) на ≥30 мин, НЕ трогать, потом запустить ТОЛЬКО orgserver → появится в списке через минуты.
- Смерть процесса → запись исчезает из списка (рейтинг сам снимает мёртвых).

### ПРОВЕРЕННЫЙ ПУТЬ (что работает)
- Настоящий websdr64 на 8090 (`/home/andy/wsdr-test/dist11/websdr64`, конфиг `../cfg/websdr.cfg`) — **регистрируется сразу** (Config = его время старта). В списке появлялся на 3-м месте, desc «RX-WebSDR Yaroslavl (R3MAV) - HF + 2m VHF online SDR receiver».
- Роутер: внешний 8090 → внутренний 80 uhf — клик из списка ведёт на rx-websdr (работает!).
- Наш orgserver (8091) — отвечает идентично websdr64 + наши 10 бэндов + реальный Users (24) — но НЕ был принят (пока! гипотеза: окно защиты от частых регистраций, т.к. много перезапусков/портов).

### СОСТОЯНИЕ НА 09.10 11:30 (после чистки)
- websdr64 УБИТ, orgserver ОСТАНОВЛЕН (systemctl stop), 8090/8091 свободны → серверы остывают ~30 мин.
- Локальный код: `rx-websdr-work/orgserver/orgserver.c` — без Mobile, парсер `org`-ключей, пересобран/задеплоен (бинарник /home/radio/orgserver/orgserver 19:49... теперь 11:29).
- systemd-юнит `orgserver.service` (User=radio, Restart=always) — создан, но сейчас inactive.
- **СЛЕДУЮЩИЙ ШАГ**: после 12:00 MSK `sudo systemctl start orgserver.service` → проверить `curl http://websdr.ewi.utwente.nl/~~websdrlistk | grep srr-76` → должен появиться `websdr.srr-76.ru:8091` с нашими диапазонами и Users.

## СЕССИЯ 09.10 — ПРОТОКОЛ websdr.org РАЗОБРАН ПОЛНОСТЬЮ (перехватом websdr64 с нуля)

### ТОЧНЫЙ ПРОТОКОЛ РЕГИСТРАЦИИ (записан tcpdump, 09.10)
websdr.org (192.87.173.88) при регистрации НОВОГО сервера делает:
1. `GET /~~orgstatus?config=0&token=0` → сервер отвечает своим `Config: <time(NULL)>`
2. `GET /~~orgstatus?config=<реальный Config>&token=0` → websdr.org ЗАПОМНИЛ Config из шага 1 и проверяет стабильность
3. `GET /logo_rd3mk.png` → скачивает логотип (НЕ критично для регистрации — проверено: без лого проходит)
4. далее регулярно `GET /~~orgstatus?config=<Config>&token=0` (стабильность) — пока сервер в списке

- **Config = time(NULL) при старте, стабилен на время жизни процесса** (у websdr64 и у нас одинаково).
- websdr64 регистрируется МГНОВЕННО (~20с) на 8090, рейтинг сразу опрашивает с его реальным Config.
- **Наш orgserver на том же 8090 НЕ принимается**: рейтинг ходит к нему с `config=0` и НЕ переходит на реальный Config (значит, не считает ответ валидным), хотя ответ структурно идентичен websdr64 (Config/Qth/Description/Logo/Email-XOR/Bands/Band/Users, заголовки Server/Set-Cookie, лого 200 PNG).
- **Mobile: m.html в orgstatus ОТСУТСТВУЕТ у принятого локального websdr64** — не критично. (Во фронтенде websdr.org `j.mobile` влияет только на фильтр mobile-устройств.)

### ЗАЩИТА websdr.org ОТ ЧАСТЫХ РЕГИСТРАЦИЙ (подтверждено 09.10)
- Частые переключения одного host:port между websdr64 и orgserver → websdr.org перестаёт включать сервер на ~30+ мин (блок по частоте регистраций).
- **Правило: НЕ переключать серверы туда-сюда. Один запущенный сервер = один стабильный Config. Менять только после паузы ≥30 мин.**
- websdr64 (настоящий) регистрируется МГНОВЕННО в любой момент (проверено: 20с), если нет активного блока.

### ЧТО ЕЩЁ ВЫЯСНЕНО
- Роутер: внешний 8090 → внутренний 80 (для клика из списка на rx-websdr). Когда надо, чтобы websdr.org достучался до оргсервера — проброс должен вести на 8090 (оргсервер проксирует сайт сам).
- У websdr64 в `wsdr-test/cfg/websdr.cfg`: `org qth/description/logo/email` + hostname; деталь — websdr64 НЕ запускается с >8 бэндов или «непонятными» строками (у него 1 бэнд 20m, `funny dipole`, device $hw:0,0,0).
- Фронтенд websdr.org (index.html): список = JSON `{url,desc,qth,lon,lat,users,logourl,mobile,bands:[{c,l,h,a}]}`; `users` обновляются отдельным запросом (`update_users`); координаты из QTH; фильтр mobile скрывает серверы без m.html на мобильных.

### СОСТОЯНИЕ 09.10 (~12:30, ПОСЛЕ СРАБАТЫВАНИЯ ЗАЩИТЫ)
- websdr64 на 8090 — в списке (рабочее состояние, клик → rx-websdr через роутер). orgserver — stop.
- Локальный код `rx-websdr-work/orgserver/orgserver.c` — без Mobile, `org`-ключи, прокси на 8081.
- **НЕЗАКРЫТО**: добиться, чтобы websdr.org принимал именно наш orgserver (с нашими 10 бэндами и реальным Users) вместо websdr64-заглушки. Пауза ≥30 мин от переключений, потом новый тест. Гипотеза для следующей сессии: сравнить наш ответ на `config=0` с websdr64 ПОСИМВОЛЬНО (в т.ч. перевод строк, порядок полей, заголовки), и проверить, что websdr.org переходит на наш реальный Config.

### СОСТОЯНИЕ 09.10 ВЕЧЕР: ЗАЩИТА websdr.org АКТИВНА (ПОЛНАЯ ОСТАНОВКА!)
- **СИМПТОМ**: НИ ОДНОГО входящего запроса от websdr.org к 8090 за 120с+ перехвата при живом websdr64 (0 GET!). websdr64 всегда регистрировался за ~20с, а тут НЕТ. В списке srr-76 = 0.
- **ДИАГНОЗ**: защита websdr.org от частых регистраций активна для `websdr.srr-76.ru:8090` (сработала из-за множества переключений websdr64↔orgserver на одном порту за сессию). Блокирует ЛЮБОЙ сервер на этом host:port — даже настоящий websdr64. Это НЕ ошибка orgserver!
- **НЕ ПОВТОРЯТЬ**: не переключать websdr64↔orgserver на 8090 туда-сюда. Одна замена = одна попытка, потом пауза.
- **СЕЙЧАС**: 8090 свободен, orgserver stop, websdr64 убит. Порт в покое.
- **СЛЕДУЮЩИЙ ШАГ (после паузы несколько часов, лучше до след. сессии)**: один чистый запуск `sudo systemctl start orgserver.service` (бинарник уже НА СЕРВЕРЕ: /home/radio/orgserver/orgserver, с Mobile: m.html + 10 бэндов + реальный Users). Heartbeat `~~websdrorg?host=websdr.srr-76.ru&port=8090`. Проверка списка — С uhf (мой доступ к websdr.org заблокирован; только websdr.ewi.utwente.nl/~~websdrlistk отвечает).

### ЧТО ТОЧНО ПРОВЕРЕНО (база для нового теста)
- websdr.org при регистрации: `config=0` → Config → `config=<реальный>` → логотип (не критично) → стабильные опросы. Переход на реальный Config = признак принятия.
- К нашему orgserver websdr.org ходил ТОЛЬКО с config=0 и НЕ переходил — но сейчас причина может быть именно в блоке, а не в формате (websdr64 тоже не принялся!).
- Ответ orgserver побайтово сверен с websdr64: заголовки (Server/Set-Cookie/Content-Type/Cache), Config=time(NULL), Email-XOR, Mobile (есть), Bands %.6f, Users реальный. Различия несущественны.
- «mobile»-поле: серверы без Mobile НЕ видны на телефоне (фильтр по умолчанию на iOS/Android), на ПК видны все 139. Наш orgserver с Mobile → будет виден и там, и там.

### СОСТОЯНИЕ 09.10 (ОКОНЧАНИЕ СЕССИИ, ~21:00) — ПРОКСИ usersproxy2 РАБОТАЕТ, НО В СПИСКЕ НЕТ
- **СХЕМА СЕЙЧАС (работает локально)**:
  - websdr64 на **8092** (внутренний, `tcpport 8092` в /home/andy/wsdr-test/cfg/websdr.cfg), PID был 50081.
  - **usersproxy2 на 8090** (слушает порт websdr.org): проксирует на websdr64:8092, в ответе orgstatus **подменяет `Users: N` на реальное число из rx-websdr** (парсит `statsobj.innerHTML="...31.00 users..."` в `~~othersjj`).
  - Юнит: `sudo systemctl status usersproxy2` (ExecStart=/home/radio/orgserver/usersproxy2 8090 8092 8081).
  - Исходник: **ЛОКАЛЬНО `rx-websdr-work/orgserver/usersproxy2.c`** (важно! на сервере только бинарь).
  - **ПОДМЕНА ПРОВЕРЕНА**: `curl 127.0.0.1:8090/~~orgstatus` → `Users: 31/32/33` (живое, совпадает с rx-websdr). Раньше websdr64 отдавал Users: 0-1.
- **НО В СПИСКЕ websdr.org СЕРВЕРА НЕТ**: websdr.org ходит к прокси только с `config=0` (1-2 раза) и НЕ переходит на реальный Config → не включает. Т.е. **прокси-версия ответа websdr64 тоже не проходит** (как и orgserver).
- **ГЛАВНЫЙ ВЫВОД СЕССИИ**: websdr.org принимает ТОЛЬКО настоящий websdr64, отвечающий НАПРЯМУЮ на порту (без прокси/имитатора). Любой посредник (orgserver, usersproxy2, spoof_responder, подмена процесса с тем же Config) → websdr.org ходит с config=0 и не включает, либо сбрасывает запись мгновенно при kill.
- **ПРОВЕРЕНО ЖЕЛЕЗНО**:
  - websdr64 на 8091 (второй экземпляр, RTL) → в списке за ~10с ✅
  - наш оргсервер/клон на том же порту → не проходит ❌
  - kill websdr64 → запись пропадает мгновенно (даже с тем же Config) ❌
- **КОД**: usersproxy2.c (локально), orgserver.c (локально) — оба готовы, лежат в rx-websdr-work/orgserver/.

### ЧТО ДЕЛАТЬ ЗАВТРА (идеи от пользователя)
1. Пользователь предлагал: **«заменить websdr64 без ненужных функций»** — т.е. написать заглушку, которая работает **в структуре оригинального websdr** (тот же бинарник, но без звука → RTL-SDR как в тесте 8091!), получает users от rx-websdr и публикует ссылку/инфо. Ключ: **настоящий websdr64 с RTL-SDR на 8090** (доказано — он всегда регистрируется), но надо придумать как он покажет реальных users rx-websdr.
2. **Стартовать websdr64 на 8090 напрямую с RTL** (как второй экземпляр 8091 из теста: device !rtlsdr 127.0.0.1:1234) — он зарегистрируется; users он покажет своих, но это уже настоящий сервер в списке.
3. НЕ метаться между процессами — каждая смена = пауза 30+ мин (защита).

### ТЕКУЩЕЕ СОСТОЯНИЕ (проверить при старте)
- 8090 = usersproxy2; 8092 = websdr64; rtl_tcp на 1234 НЕ запущен (остановлен при чистке).
- В списке srr-76 НЕТ (выпал при переводах портов). Роутер: 8090 → 8090.
- Если надо вернуть «рабочую заглушку в списке»: остановить usersproxy2, вернуть websdr64 на 8090 (tcpport 8090), подождать.

## ✅ РЕШЕНО ПОЗДНИМ ВЕЧЕРОМ 09.10 — РАЗГАДКА: KEEP-ALIVE!
**Наш orgserver (rx-websdr) зарегистрирован в websdr.org ПОЛНОЦЕННО! Было это всё время из-за keep-alive!**

- **СИМПТОМ весь день**: websdr.org ходил к нашему серверу только с `config=0` и никогда не переходил на реальный Config → не включал. websdr64 — включал мгновенно.
- **ПРИЧИНА (найдена сравнением)**: настоящий websdr64 **не отдаёт заголовок `Connection: close`** и **держит TCP-соединение открытым** (keep-alive), отвечая на НЕСКОЛЬКО запросов подряд по одному сокету (проверено: 3/3 запроса). websdr.org делает последовательность: `config=0` → `config=<реальный>` → логотип → опросы **на одном соединении**. Наш orgserver закрывал сокет после первого ответа (`Connection: close`) → websdr.org не мог сделать второй запрос → сервер считался «не настоящим».
- **ФИКС**: orgserver.c — обработчик клиента переведён на keep-alive: цикл «прочитать запрос → ответить → ждать следующий» (без close), заголовки БЕЗ `Connection: close`, таймаут чтения 30с (SO_RCVTIMEO), чтобы потоки не висели.
- **ПРОВЕРЕНО**: после фикса websdr.org ходит к нашему 8091 с РЕАЛЬНЫМ Config (7 запросов) и сервер появился в списке:
  - `http://websdr.srr-76.ru:8091/` — users **23** (живое, обновляется от rx-websdr), bands `m2 143.976-146.024 MHz` (наш VHF!) + все 10 бэндов.
- **ТЕКУЩЕЕ (конец сессии)**: websdr64:8090 (заглушка, users 0) И orgserver:8091 (наш, реальные users) — ОБА в списке. Роутер 8090→80 ведёт клик заглушки на rx-websdr.
- **ДАЛЬШЕ (по желанию)**: можно убрать заглушку 8090 и оставить только orgserver:8091 (чище: одна запись, реальные users); или перевести orgserver на 8090 (единственная запись). Решение за пользователем.

## ИСТОРИЯ ЭКСПЕРИМЕНТОВ (09.10, что НЕ работает)
- orgserver (имитатор) без keep-alive → websdr.org: только config=0, не включает.
- Смена процесса с тем же Config (spoof_responder) → запись вылетает мгновенно (kill виден).
- usersproxy2 (прокси websdr64 + подмена Users) → websdr.org ходил с config=0 и не включал (тоже закрывал соединение? и прокси ≠ настоящий).
- Второй экземпляр websdr64 на 8091 (RTL-SDR) → в списке за 10с ✅ (он настоящий, keep-alive).
- **ВЫВОД**: websdr.org принимает только серверы с keep-alive-соединением, отвечающие на несколько запросов подряд по одному сокету. Это и есть «проверка на настоящесть».

#### ✅ 12.10: РАБОЧАЯ ВЕРСИЯ НАЙДЕНА — ИСХОДНИК В ВОРКСПЕЙСЕ (НЕ НА СЕРВЕРЕ!)
- **ПРОБЛЕМА БЫЛА**: на сервере стоял/пересобирался orgserver из СТАРОГО/БИТОГО исходника (`orgserver.c` 12 997 байт, без org-парсера) → orgstatus без Qth/Description → websdr.org не включал 8091, хотя днём включал.
- **РАБОЧИЙ ИСХОДНИК — ЛОКАЛЬНО**: `rx-websdr-work/orgserver/orgserver.c` (18 496 байт, правки 12.10 08:11) — с keep-alive (цикл запросов, БЕЗ Connection: close, SO_RCVTIMEO 30с) + org-префикс парсер + antenna `kl==7`.
- **ДЕПЛОЙ**: scp локального orgserver.c → /home/andy/ → gcc на сервере → install в /home/radio/orgserver/orgserver → restart orgserver.service. Проверка: orgstatus полный (Qth/Description/Logo/Email/Mobile) + keep-alive 2/2.
- **РЕЗУЛЬТАТ (проверено 12.10)**: websdr.org СРАЗУ переходит на реальный Config (`config=<наш>`) и включает `websdr.srr-76.ru:8091` в список: users 33 (живые), бэнд m2 143.976-146.024 (наш VHF) + все 10 бэндов. Мгновенная регистрация!
- **УРОК**: НЕ пересобирать orgserver на сервере из серверного orgserver.c (он может быть старым). Всегда брать исходник из воркспейса `rx-websdr-work/orgserver/orgserver.c` — это единственная рабочая версия. После правок локально — деплой через scp+gcc+install.
- **ТЕСТОВЫЙ websdr64:8090** — тоже в списке (users 0, не мешает); можно прибить, если не нужен.

## ТЕКУЩЕЕ СОСТОЯНИЕ (пауза, ~22:00 09.10) — РАБОТАЕТ ✅
- **websdr.srr-76.ru:8091 — наш orgserver В СПИСКЕ websdr.org** с реальными пользователями (users 25-39, живое число = numusersobj из ~~othersjj rx-websdr) и нашими 10 бэндами (VHF 144-146 + HF). Место ~20.
- **websdr64-заглушка (8090) УБРАНА** (убита, дерегистрировалась). В списке теперь только 8091.
- Код: orgserver.c (keep-alive!) на сервере /home/radio/orgserver/orgserver + юнит orgserver.service (active). Конфиг orgserver.cfg: listen_port 8091, backend 127.0.0.1:8081, org-ключи, main_cfg = /home/radio/rx-websdr/cfg/websdr.cfg.
- **Users в списке** — органо: orgserver опрашивает ~~othersjj rx-websdr (numusersobj.innerHTML="N") раз в цикл heartbeat и отдаёт в orgstatus. Число живое (совпадает со счётчиком на странице rx-websdr «пользуются (N)»).
- **Антенны в списке `?`** — в cfg/websdr.cfg основного у бэндов нет `antenna`. ПОЛЬЗОВАТЕЛЬ сам допишет антенны (не трогать без просьбы!).
- Роутер: 8091→8091 проброс есть (сервер виден снаружи); 8090→80 остался (не мешает).
- ССЫЛКА по клику из списка работает: 8091 проксирует на rx-websdr 8081.

### ГЛАВНЫЙ УРОК ВСЕХ ДНЕЙ (для любых будущих имитаций websdr.org)
- **websdr.org принимает сервер ТОЛЬКО если он отвечает на НЕСКОЛЬКО запросов подряд по одному TCP-соединению (keep-alive)**: последовательность `~~orgstatus?config=0` → `config=<реальный>` → логотип → опросы идёт ПО ОДНОМУ сокету. Без этого (Connection: close после первого ответа) — сервер считается «ненастоящим» и не включается.
- Заголовки ответа: `Server: WebSDR/20140718.1716-64`, `Content-Length`, `Content-Type: text/plain`, `Cache-control: no-cache`, `Set-Cookie: ID=...` — БЕЗ `Connection: close`!
- Тело orgstatus: `Config: <time(NULL)>` (стабильный на жизнь процесса), `Qth`, `Description`, `Logo`, `Email` (XOR^1), `Bands: N`, `Band: <idx> <centerkHz.6f> <widthkHz.6f> <antenna>`, `Users: <живое число>`.

## БАГ: счётчик только растёт до перезагрузки страницы
- **Причина — в КЛИЕНТЕ, не в сервере.** Сервер (`/~~othersjj`) присылает ПОЛНЫЙ список активных слушателей по стабильным слотам и корректный `nusers` (считает только `cl->audio_active && cl->wsi`; слот освобождается при CLOSED).
- Клиент: `uu(i,...)` пишет в `uu_names[i]` по индексу слота; `douu()` считает `total` как ВСЕ непустые элементы массива. **Массивы `uu_names/uu_bands/uu_freqs` нигде не очищались** → слоты ушедших пользователей остаются навсегда → `total` растёт до максимума «одновременных с момента загрузки». Перезагрузка = массивы обнуляются → цифра падает. Отсюда 26 → 50 → перезагрузка → меньше.

## ФИКС (rx-base.js, ajaxFunction3, ~строка 1604)
- Перед `eval(xmlHttp.responseText)` — очистка: `uu_names=new Array(); uu_bands=new Array(); uu_freqs=new Array();`
  → после eval остаются только свежие слоты, `total` правильный.
- Кэш: `rx-base.js?v=rec7` → **`?v=rec8`** в rx-websdr.html (бамп обязателен при правке js!).
- Деплой: scp → /tmp → sudo cp + chown radio:radio. Проверено: live / отдаёт v=rec8, rx-base.js содержит 2 вхождения `uu_names=new Array()` (объявление + очистка).
- `node --check` — синтаксис OK.
- НЕ ЗАКОММИЧЕНО. В зеркале rx-websdr-server: M pub/rx-base.js (мой фикс + прежние некоммиченные правки 02.10), M pub/rx-websdr.html (v=rec8).

### 12.10 ВЕЧЕР — ФИНАЛЬНАЯ СХЕМА ПРОБРОСОВ (рабочая)
- **websdr.srr-76.ru:8091** — наш orgserver (rx-websdr) В СПИСКЕ websdr.org (users живые, 10 бэндов, антенны: 2m=Vertical, HF=Inverted Vee -> RX888).
- **Проброс роутера — ДВА РЕЖИМА**:
  1. `8091 → 8091`: websdr.org видит оргserver, **регистрация после перезапуска МГНОВЕННАЯ** (проверено: Config сменился, сервер вернулся в список сразу).
  2. `8091 → 80`: посетитель по ссылке из списка попадает на rx-websdr через Caddy (приём работает).
- **РЕГЛАМЕНТ после рестарта orgserver/сервера**: сначала проброс `8091→8091` (сервер мгновенно в списке), затем переключить на `8091→80` — работает. (Суть: первичный опрос websdr.org идёт на наружный 8091, где должен быть orgserver; после принятия можно вести на 80.)
- **Писькомерство/продвижение** по-минимуму: текущая запись в списке достаточно.
- Роутер 8090→80: больше не используется (заглушка websdr64 убита), можно убрать при желании.

# СЕССИЯ 13.10 — НОВОЕ ОКНО FT8 (дизайн sv1btl) — ЗАДЕПЛОЕНО НА ПРОД ✅ (НЕ закоммичено!)

## ЧТО СДЕЛАНО (по просьбе: «переделать окно декодов FT8 в стиле github.com/sv1btl/WebSDR-emulation-for-UberSDR, тёмная тема»)
- Окно FT8 (кнопка FT8) переделано в духе sv1btl `decoders.js`: заголовок **«FT8 decoder»** + счётчик **Decodes N** + кнопка **Clear**, таблица **UTC | dB | DT | Hz | Message | Locator | Distance** (sticky-шапка, автоскролл, hover).
- **Бэкенд НЕ менялся** (остался серверный `/~~ft8` → ka9q-radio `decode_ft8` → `/var/log/ft8.log`). Браузерный декодер PhantomSDR из sv1btl НЕ переносился (решено с пользователем).
- **Дистанция**: парсится локатор из сообщения (regex `\b([A-R]{2}\d{2}(?:[A-X]{2})?)\b`, RR73 отбрасывается) → ссылка levinecentral + **расстояние от QTH из шапки** (`a[href*="qth="]`, текст ссылки `LO07AQ98LT` → берётся 6-значный). Проверено: JP51 = 1674 км, KO85 = 305 км от LO07AQ98.
- **Клик по строке** — перестройка на частоту декода (исправлен старый баг: передавались Hz в `setfreqb`, которая ждёт кГц — теперь `freqKhz.toFixed(3)`).
- Парсер строки принимает и `~`, и `@`-разделители, и отрицательные SNR (`-7`), и `<...>` в сообщении.

## 13.10 (фикс выравнивания колонок)
- **Симптом**: цифры dB/DT/Hz были прижаты вправо, не под названиями колонок.
- **Фикс** (CSS в `pub/rx-websdr.css`): числовые колонки (2=dB, 3=DT, 4=Hz, 7=Distance) центрированы — и заголовки `.ft8-table th:nth-child(2/3/4/7)`, и значения `.ft8-table td.ft8-n { text-align: center }`. UTC/Message/Locator — слева.
- `rx-websdr.html`: `rx-websdr.css?v=rec6` → **`v=rec7`**.
- Новые хеши: `pub/rx-websdr.css` = `11b3302a68ffc7ab505da9bce2bdc8e9`, `pub/rx-websdr.html` = `e2c101104be13c600103853739a96f0e`.

## ВАЖНЫЙ БАГФИКС (server.c, пересобран!)
- Старый сервер слал `ft8chseq=1;` — это **ПРИСВАИВАНИЕ**: в eval затирало клиентскую функцию `ft8chseq` числом → сброс буфера никогда не работал, `ft8_lines` рос бесконечно.
- Исправлено на **вызов** `ft8chseq(1);` в **двух местах** `serve_ft8` (src/server.c). `sudo make` + `sudo systemctl restart websdr.service` — применено. Проверено: `/~~ft8` отдаёт `ft8chseq(N);`.
- Клиент устойчив и к старому серверу (страховка: `ft8_lines` ограничен 200).

## 13.10 (ПОВТОРНЫЙ ФИКС после жалобы «декоды пропадают мгновенно»)
- **Симптом**: с первым вариантом окна декоды исчезали через ~1с. Причина: `ft8chseq(N)` сбрасывал `ft8_lines` при КАЖДОМ опросе (1с), а сервер шлёт только хвост (60 строк) → окно показывало только последнюю порцию.
- **Исправление** (клиент `pub/rx-base.js`): окно стало **накопительным**, как было раньше:
  - `ft8_lines` НЕ сбрасывается опросом (только при открытии/закрытии/Clear)
  - `ft8str()` копит строки с **дедупликацией по последним 60** (сервер шлёт тот же хвост между слотами — без дедупа всё задвоилось бы; проверено: хвост стабилен между опросами)
  - лимит окна 500 строк (~8 мин FT8), старые уходят естественно
  - `ft8_total` (счётчик Decodes) считает только НОВЫЕ добавленные строки
- `rx-websdr.html`: `rx-base.js?v=rec9` → **`v=rec10`** (сброс кэша обязателен!).
- Новые хеши: `pub/rx-base.js` = `e3bcd9cbaa54e1319ee8b26012db631c`, `pub/rx-websdr.html` = `8aeacb6065f9af10ae927e5adc35dffe`.

## ВАЖНО: РАБОТА ТОЛЬКО НА СЕРВЕРЕ (правило подтверждено 13.10!)
- **Локальная копия рассинхронизирована с сервером**: на сервере `git log` ушёл вперёд (ad9be85, 723ad7c — docs), а `pub/rx-websdr.html` имеет НЕКОММИЧЕННУЮ правку пользователя **port-redirect** (в head: `if (location.port === '8091') location.replace('https://websdr.srr-76.ru/')`).
- В этот раз НЕ задеплоено поверх локальной копии: скачаны серверные файлы (base64-ом), к ним применены правки, залито обратно. **Всегда работать с серверными файлами!**
- Локальная копия `rx-websdr-server/` содержит СВОИ некоммиченные правки (анти-spam username, фикс `uu_names`, orgserver-вставки в server.c) — НА СЕРВЕРЕ ИХ НЕТ. Не переносить без запроса!

## ФАЙЛЫ/ХЕШИ (после деплоя 13.10, md5)
- `pub/rx-base.js` = `e3bcd9cbaa54e1319ee8b26012db631c` (v=rec10 в html; накопительное окно)
- `pub/rx-websdr.css` = `11b3302a68ffc7ab505da9bce2bdc8e9` (v=rec7; числовые колонки по центру)
- `pub/rx-websdr.html` = `e2c101104be13c600103853739a96f0e` (port-redirect внутри!)
- `src/server.c` = `684a1a0655079b309c9ef03d7438a05f` → бинарь пересобран (139824 б, 21:29)
- Бэкапы оригиналов: `/home/radio/rx-websdr/backups-ft8-20261013/` (`*.orig`) — откат = sudo cp обратно + пересборка.
- git status на сервере: M 4 файла + ?? backups-ft8-20261013/. НЕ КОММИТИТЬ без команды.

## ОТКАТ (если дизайн не понравится)
```bash
cd /home/radio/rx-websdr
sudo cp backups-ft8-20261013/rx-base.js.orig pub/rx-base.js
sudo cp backups-ft8-20261013/rx-websdr.html.orig pub/rx-websdr.html
sudo cp backups-ft8-20261013/rx-websdr.css.orig pub/rx-websdr.css
sudo cp backups-ft8-20261013/server.c.orig src/server.c
sudo chown radio:radio pub/* src/server.c
sudo make && sudo systemctl restart websdr.service
```

# СЕССИЯ 14.10 — ШРИФТ ТАЙЛОВ ШКАЛЫ: ЦИФРА 8 (ЗАКОММИЧЕНО dec6b8a, PUSH ✅)

## Дополнение 14.10 (14:3x): серединка 8 тоже отцентрирована
- Пользователь попросил отцентрировать и среднюю линию 8 (была `####.` прижата влево).
- Правка `src/scale.c` строка 121: `{0x0e,0x11,0x11,0x1e,0x11,0x11,0x0e}` → `{0x0e,0x11,0x11,0x0e,0x11,0x11,0x0e}` (средний байт `0x1e` → `0x0e`).
- Бэкап: `src/scale.c.bak-20261014-font8-mid`.
- **ПЕРЕЗАПУСК ТОЛЬКО ЧЕРЕЗ start.sh!** (в этот раз был инцидент: `systemctl restart websdr.service` завис в `deactivating` из-за FIFO-блокировки writers; разрешилось само через ~2 мин, но правило — только `./start.sh`).
- Пересборка `sudo make` + `sudo ./start.sh`. Тайлы перегенерированы: новый timestamp `1791963350` (старый `1791963023`).
- md5 нового тайла `1791963350-b9z4i6.png` = `75e303f1d91c949423a0876924170916` (отличается от старого — пиксели изменились).
- Проверено по пикселям: середина 8 теперь `.###.` (по центру), было `####.` (влево).
- КОММИТ этого дополнения: `ca4dec3` fix(scale): цифра 8 — серединка тоже по центру (полностью симметричная 8), pushed ✅.

## Проблема пользователя
- На шкале частота **1880 кГц выглядела как «1bb0»** (цифра 8 читалась как b). «Тогда решили, но после откатов/восстановлений правка потерялась».

## Что нашли
- Тайлы шкалы генерируются встроенным **5x7 пиксельным шрифтом** в `src/scale.c` (массив `FONT[10][7]`, строки 112-123), НЕ системным шрифтом.
- На сервере/в git глиф '8' был **битый**: `{0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}` — верх/низ прижаты влево (`####.`), из-за чего на тайле читается как 'b'.
- **Правильная 8 сохранилась** в рабочей копии `rx-websdr-work/src/scale.c` (правка от 05.10, не была задеплоена/закоммичена): `{0x0e,0x11,0x11,0x1e,0x11,0x11,0x0e}` — верх/низ **центрированы** (`.###.` как у 0).

## Что сделано (на сервере)
1. `sudo sed -i` в `src/scale.c` строка 121: заменил глиф 8 на правильный (`0x0e...0x0e`). Бэкап: `src/scale.c.bak-20261014-font8`.
2. `sudo make` (пересобрал scale.o + бинарь) + `sudo systemctl restart websdr.service` — **тайлы перегенерированы автоматически** при старте (новый timestamp `1791963023`, старые `1788802775` отдаются 404).
3. Проверено: `bandinfo.js` на проде ссылается только на новые тайлы (200), старые — 404.
4. **Закоммичено `dec6b8a`** + push origin main. Больше НЕ потеряется.

## УРОКИ
- **Тайлы шкалы рендерятся СВОИМ пиксельным шрифтом в scale.c** — не искать шрифты в системе!
- Правки в `rx-websdr-work/src/*` НЕ попадают на сервер сами: сервер — источник истины, локальная папка может содержать несинхронизированные правки (здесь правильная 8 лежала только там).
- После правки scale.c достаточно пересборки + рестарта websdr.service: тайлы и bandinfo.js перегенерируются с новым timestamp.
