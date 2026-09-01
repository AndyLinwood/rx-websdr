#!/bin/bash
# start.sh — launch the new WebSDR C server (uses the existing receiver feed).
#
# Correct order (per sysop): radiod -> receiver (writer) -> websdr (reader).
# CRITICAL: killing the websdr reader FIRST cascades and stops receiver
# (pcmrecord gets EPIPE), so receiver must be restarted AFTER that kill.
#
# Usage:  ./start.sh
#   - requires sudo (prompts for password) to start radiod/receiver services
#   - server logs to /tmp/wf_real.log
#   - stop the server with:  pkill -x websdr-server
set -e
cd "$(dirname "$0")"

# 1) radio front-end (RX888 / radiod) must be up
if ! systemctl is-active --quiet radiod@rx888.service; then
    echo ">> starting radiod@rx888.service ..."
    sudo systemctl start radiod@rx888.service
fi

# 2) kill any stale server reader FIRST. Killing it cascades and stops the
#    receiver, so we then restart receiver fresh (guaranteed correct order).
pkill -9 -x websdr-server 2>/dev/null || true
sleep 1

# 3) receiver writer: pcmrecord -> fifo. Restart unconditionally so it is
#    always writing in the right order, regardless of prior state.
echo ">> (re)starting receiver.service ..."
sudo systemctl restart receiver.service

# 4) reader: start this server. Opening the FIFO as reader unblocks the
#    receiver's pcmrecord and makes data flow.
sleep 1
( setsid ./websdr-server -c cfg/websdr.cfg > /tmp/wf_real.log 2>&1 </dev/null & disown )

sleep 1
pid="$(pgrep -x websdr-server || true)"
if [ -n "$pid" ]; then
    echo ">> websdr-server started (pid $pid) on port 8095"
    echo ">>  logs: /tmp/wf_real.log    stop: pkill -x websdr-server"
else
    echo ">> FAILED: websdr-server did not start (see /tmp/wf_real.log)"
fi
