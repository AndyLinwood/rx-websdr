#!/bin/sh
# rtl_tcp_vhf.sh — RTL-SDR (2m VHF) rtl_tcp server for rx-websdr.
#
# НАСТРОЙКИ — правь здесь (как при старте, так и для перезапуска службы):
#   RTL_GAIN        усиление в дБ (ручной режим, БЕЗ AGC).
#                   Может быть переопределено websdr.cfg (gain VHF).
#   RTL_FREQ        частота настройки донгла, Гц (145000000 = 145.000 МГц).
#   RTL_RATE        частота дискретизации, Гц (дефолт rtl_tcp 2048000).
#   RTL_SERIAL      СЕРИЙНЫЙ НОМЕР донгла (у твоего — 00000303).
#                   rtl_tcp сам различает «индекс» и «серийник» (verbose_device_search):
#                   -d 00000303 надёжно связывает именно этот свисток при НЕСКОЛЬКИХ
#                   устройствах (по индексу порядок может меняться).
#   RTL_PORT        TCP-порт, который слушает rtl_tcp (в websdr.cfg device !rtlsdr 127.0.0.1:PORT).
#   RTL_PPM         коррекция кварца, ppm (0).
#   RTL_DIRECT      ПРЯМОЙ (I/Q) режим для приёма КВ на rtl-sdr v3/v4: 0=выкл, 1=I, 2=Q.
#                   Включает опцию rtl_tcp -D (direct sampling). Для КВ на v3/v4 = 1.
#   RTL_BIAS_T      Bias-T на GPIO PIN 0 (rtl-sdr.com v3/v4): 0=выкл, 1=вкл (флаг -T).
#
# Несколько донглов: скопируй этот файл (rtl_tcp_70cm.sh, rtl_tcp_40m.sh ...),
# поставь свой RTL_SERIAL/частоту/частоту/порт, и заведи свою systemd-службу
# по образцу rtl_tcp_vhf.service.

RTL_GAIN=19
RTL_FREQ=145000000
RTL_RATE=2048000
RTL_SERIAL=00000303
RTL_PORT=1234
RTL_PPM=0
RTL_DIRECT=0
RTL_BIAS_T=0

set -- /usr/local/bin/rtl_tcp -a 127.0.0.1 -p "${RTL_PORT}" -f "${RTL_FREQ}" \
    -s "${RTL_RATE}" -g "${RTL_GAIN}" -d "${RTL_SERIAL}" -P "${RTL_PPM}"

if [ "${RTL_DIRECT}" != "0" ]; then
    set -- "$@" -D "${RTL_DIRECT}"
fi
if [ "${RTL_BIAS_T}" != "0" ]; then
    set -- "$@" -T
fi

# Слушаем только на loopback (доступ к свистку только с этой машины).
exec "$@"