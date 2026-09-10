#!/bin/bash
# start.sh — (re)start the WebSDR stack using systemd units only.
#
# Correct order (per sysop): radiod@rx888 -> receiver (writer) -> websdr (reader).
# All three are enabled units, so a reboot brings them up automatically.
# Use this script when you want to (re)start / fix the stack by hand without
# waiting for a reboot. It never launches websdr-server directly: the service
# owns the process, so `systemctl status websdr.service` stays the source of
# truth and ExecStartPre (pkill) keeps the port free.
#
# FIX (8 sep): two things made repeated `./start.sh` cycles leave no waterfall:
#   1) counting writers with `pgrep -f "pcmrecord -c -r"` also matched the
#      *bash subshells* of start-receiver.sh blocked in open(fifo, O_WRONLY),
#      so "12 writers" was reported while none had exec'ed into pcmrecord yet.
#   2) restarting websdr EPIPEs the pcmrecord writers (they die), and receiver
#      does NOT respawn them on its own (its script sits in `wait`). So the new
#      websdr finds no writer for each fifo and stays with 0 fifos open.
#   Fix: count REAL pcmrecord (`pgrep -x pcmrecord`), and AFTER restarting
#   websdr ALSO restart receiver so the writers reconnect to the new reader,
#   then wait for 12 writers AND 12 fifo fds before declaring success.
#
# Usage:  ./start.sh
#   - requires passwordless sudo (or will prompt)
#   - exit code 0 on success, non-zero on failure
set -e

N_BANDS=12            # number of bands in cfg/websdr.cfg (fifo pairs expected)
WAIT_WRITERS_MAX=20   # seconds to wait for real pcmrecord writers
WAIT_FIFO_MAX=25      # seconds to wait for websdr to open all fifos

count_fifo_fds() {   # websdr fifo fds as a number (works even if PID empty)
    local pid opened
    pid=$(systemctl show websdr.service -p MainPID --value 2>/dev/null)
    [ -n "$pid" ] && [ "$pid" -gt 1 ] 2>/dev/null || { echo 0; return; }
    opened=$(sudo ls /proc/$pid/fd 2>/dev/null | grep -c fifo)
    [ -n "$opened" ] || opened=0
    echo "$opened"
}

echo "== WebSDR stack (re)start =="

# 1) radio front-end (RX888 / radiod) must be up
if ! systemctl is-active --quiet radiod@rx888.service; then
    echo ">> radiod@rx888.service is DOWN, starting..."
    sudo systemctl start radiod@rx888.service
    echo "   ... waiting for radiod to publish streams"
    for i in $(seq 1 30); do
        systemctl is-active --quiet radiod@rx888.service && break
        sleep 1
    done
    systemctl is-active --quiet radiod@rx888.service \
        && echo "   radiod@rx888.service up" \
        || { echo "   ERROR: radiod@rx888.service did not start"; exit 1; }
else
    echo ">> radiod@rx888.service already up"
fi

# 2) (re)start the writers first
echo
echo ">> (re)starting receiver.service (writers pcmrecord -> fifos)..."
sudo systemctl restart receiver.service

wait_writers() {     # wait until N real pcmrecord are up
    local i writers
    writers=0
    for i in $(seq 1 $WAIT_WRITERS_MAX); do
        writers=$(pgrep -x pcmrecord | wc -l)
        [ "$writers" -ge "$N_BANDS" ] && break
        sleep 1
    done
    echo "$writers"
}

echo "   waiting for $N_BANDS real pcmrecord writers..."
writers=$(wait_writers)
echo "   (pcmrecord writers running: $writers)"
if [ "$writers" -lt "$N_BANDS" ]; then
    echo "   ERROR: expected $N_BANDS writers, only $writers up"
    echo "     -> systemctl status receiver.service"
    exit 1
fi

# 3) (re)start the reader
echo
echo ">> (re)starting websdr.service (reader)..."
sudo systemctl restart websdr.service

# Restarting websdr closes the fifos of the *old* reader; every pcmrecord
# writer attached to them dies of EPIPE. receiver.service does not notice
# (its start-receiver.sh is blocked in `wait`), so the new websdr would find
# no writers. Restart receiver again so writers reconnect to the new reader.
echo "   restarting receiver.service again (writers died on reader restart)..."
sudo systemctl restart receiver.service
echo "   waiting for $N_BANDS real pcmrecord writers..."
writers=$(wait_writers)
echo "   (pcmrecord writers running: $writers)"
if [ "$writers" -lt "$N_BANDS" ]; then
    echo "   ERROR: after reconnect only $writers writers up"
    echo "     -> systemctl status receiver.service"
    exit 1
fi

# 4) wait for websdr to be reachable AND to actually open all fifos.
echo ">> waiting for websdr.service to be reachable on :80 and open fifos..."
ok=0
for i in $(seq 1 $WAIT_FIFO_MAX); do
    if systemctl is-active --quiet websdr.service && \
       curl -sf -o /dev/null "http://127.0.0.1:8095/" 2>/dev/null; then
        # count fifos by the CURRENT websdr process (fresh pgrep), not MainPID
        # cached at loop start — the fd set grows a couple of seconds after
        # the HTTP listener is up.
        opened=$(pgrep -x rx-websdr | head -1 | sed 's/.*/&/' | \
                 xargs -r -I{} sudo ls /proc/{}/fd 2>/dev/null | grep -c fifo)
        [ -n "$opened" ] || opened=0
        if [ "$opened" -ge "$N_BANDS" ]; then
            ok=1
            break
        fi
    fi
    sleep 1
done

if [ "$ok" -eq 1 ] && systemctl is-active --quiet receiver.service; then
    echo
    echo ">> WebSDR stack is UP:"
    echo "   websdr.service   active (pid $(systemctl show websdr.service -p MainPID --value))"
    echo "   receiver.service active ($(pgrep -x pcmrecord | wc -l) writers)"
    echo "   websdr has opened $(count_fifo_fds) fifos"
    echo "   waterfall will be live within a couple of seconds - refresh the page if needed"
else
    echo ">> FAILED — check:"
    echo "     systemctl status websdr.service receiver.service radiod@rx888.service"
    exit 1
fi