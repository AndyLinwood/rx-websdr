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

# 1) radio front-end (RX888 / radiod) must be up
if ! systemctl is-active --quiet radiod@rx888.service; then
    echo ">> starting radiod@rx888.service ..."
    sudo systemctl start radiod@rx888.service
fi

# 2) receiver writer: pcmrecord -> fifos. Restart unconditionally so fifos are
#    always written in the right order (its unit Restart=always revives on EPIPE).
echo ">> (re)starting receiver.service ..."
sudo systemctl restart receiver.service

# 3) websdr reader. The unit's ExecStartPre kills any leftover websdr-server
#    (e.g. a previous manual launch) before binding the port.
echo ">> (re)starting websdr.service ..."
sudo systemctl restart websdr.service

# 4) verify
sleep 3
if systemctl is-active --quiet websdr.service && \
   systemctl is-active --quiet receiver.service; then
    echo ">> WebSDR stack is UP: websdr.service + receiver.service active"
    systemctl show websdr.service -p MainPID --value | sed 's/^/>>   websdr pid /'
else
    echo ">> FAILED — check:"
    echo "     systemctl status websdr.service receiver.service radiod@rx888.service"
    exit 1
fi