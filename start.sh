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
# Usage:  ./start.sh
#   - requires passwordless sudo (or will prompt)
#   - exit code 0 on success, non-zero on failure
set -e

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

echo
echo ">> (re)starting receiver.service (writers pcmrecord -> fifos)..."
sudo systemctl restart receiver.service

# Give receiver a moment to spawn the pcmrecord writers, then report how many.
echo "   waiting for pcmrecord writers..."
for i in $(seq 1 15); do
    n=$(pgrep -c -f "pcmrecord -c -r") || n=0
    if [ "$n" -ge 12 ]; then echo "   $n pcmrecord writers up"; break; fi
    sleep 1
done
n=$(pgrep -c -f "pcmrecord -c -r") || n=0
echo "   (pcmrecord writers running: $n)"

echo
echo ">> (re)starting websdr.service (reader)..."
echo "   note: waterfall needs a few seconds to fill its FFT history,"
echo "   the page may look empty briefly right after this step"
sudo systemctl restart websdr.service

# 4) wait for the reader to actually open the fifos, then confirm.
echo ">> waiting for websdr.service to become active..."
for i in $(seq 1 15); do
    systemctl is-active --quiet websdr.service && break
    sleep 1
done
sleep 2   # let the first FFT frames accumulate so the waterfall starts moving

if systemctl is-active --quiet websdr.service && \
   systemctl is-active --quiet receiver.service; then
    echo
    echo ">> WebSDR stack is UP:"
    echo "   websdr.service   active (pid $(systemctl show websdr.service -p MainPID --value))"
    echo "   receiver.service active"
    echo "   waterfall will be live within a couple of seconds - refresh the page if needed"
else
    echo ">> FAILED — check:"
    echo "     systemctl status websdr.service receiver.service radiod@rx888.service"
    exit 1
fi