#!/bin/bash
# start.sh — (re)start the WebSDR stack in the ONLY correct way:
#   1) stop EVERYTHING (including radiod and rtl_tcp) for a clean slate
#   2) start, strictly in order: radiod@rx888 -> receiver (writers) ->
#      rtl_tcp_vhf (RTL-SDR dongle) -> websdr (reader)
# Each step must fully finish before the next one begins.
#
# No state counting, no timers: the ordering alone guarantees the stack comes
# up. A writer (pcmrecord, pre-exec bash subshell) blocks in open(fifo,
# O_WRONLY) until the reader appears; the reader (websdr) blocks in open(fifo,
# O_RDONLY) until a writer appears. The only safe sequence is: get ALL writers
# parked in open() first, THEN start the reader, which unblocks every writer
# at once.

set -o errexit

N_BANDS=$(grep -c "^band " /home/radio/rx-websdr/cfg/websdr.cfg)
[ -z "$N_BANDS" ] && N_BANDS=9

# --- 1) Kill everything ------------------------------------------------------
for unit in websdr.service receiver.service radiod@rx888.service rtl_tcp_vhf.service radiod@vhf.service; do
    echo ">> stopping $unit"
    # receiver/websdr ignore SIGTERM while blocked in FIFO I/O; systemd would
    # wait TimeoutStopSec then SIGKILL. Force-kill the cgroup right away.
    sudo systemctl stop "$unit" 2>/dev/null || true
    if systemctl is-active --quiet "$unit" 2>/dev/null; then
        sudo systemctl kill --kill-who=all -s KILL "$unit" 2>/dev/null || true
        sleep 2
    fi
done
sudo pkill -9 -x pcmrecord 2>/dev/null || true
sudo pkill -9 -x rx-websdr  2>/dev/null || true
# Let the RTL-SDR dongle fully release (radiod@vhf may have held it).
sleep 3
echo ">> all services stopped"

# --- 2) Start radiod ---------------------------------------------------------
echo ">> starting radiod@rx888.service"
sudo systemctl start radiod@rx888.service
until systemctl is-active --quiet radiod@rx888.service; do sleep 1; done
echo "   radiod up (waiting a moment for streams to publish)"
sleep 2

echo ">> starting rtl_tcp_vhf.service (RTL-SDR 2m via rtl_tcp, manual gain)"
sudo systemctl start rtl_tcp_vhf.service
for i in $(seq 1 15); do
    if ! ss -tln 2>/dev/null | grep -q '127.0.0.1:1234'; then
        sleep 1
    fi
done
systemctl is-active --quiet rtl_tcp_vhf.service || echo "   WARNING: rtl_tcp_vhf not active"
echo "   rtl_tcp vhf up (listening 127.0.0.1:1234)"

# --- 3) Start receiver (writers), WAIT for every band's writer to be parked
#        in open(fifo) before the reader is allowed to start.
echo ">> starting receiver.service (writers)"
sudo systemctl start receiver.service
# Each `start_band ... > fifo &` forks a bash subshell that blocks in
# open(fifo, O_WRONLY) until the reader appears; pcmrecord execs only AFTER
# the open succeeds. So "all bash subshells present" == "all writers parked
# on their fifo". Count only actual blocked subshells, not the manager.
for i in $(seq 1 30); do
    parked=$(pgrep -f "start-receiver.sh" | wc -l)
    [ "$parked" -gt "$N_BANDS" ] && break   # manager + N_BANDS subshells
    sleep 1
done
echo "   writers parked: $((parked - 1)) / $N_BANDS"
if [ "$parked" -le "$N_BANDS" ]; then
    echo "   WARNING: not all writers parked; continuing anyway"
fi

# --- 4) Start websdr (reader) — unblocks every parked writer ----------------
echo ">> starting websdr.service (reader)"
sudo systemctl start websdr.service

# --- 5) Report (informational only) -----------------------------------------
for i in $(seq 1 20); do
    code=$(curl -s -m 2 -o /dev/null -w '%{http_code}' http://127.0.0.1:80/ 2>/dev/null || true)
    [ "$code" = "200" ] && break
    sleep 1
done
echo
echo ">> stack state:"
systemctl is-active radiod@rx888.service receiver.service rtl_tcp_vhf.service websdr.service
echo "   writers:   $(pgrep -x pcmrecord | wc -l)"
echo "   http:      ${code:-FAIL}"