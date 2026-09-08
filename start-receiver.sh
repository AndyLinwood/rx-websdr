#!/bin/bash
# start-receiver.sh — feed every WebSDR band's fifo with pcmrecord from radiod.
# One pcmrecord per fifo; writer opens (blocks) until the websdr reader appears.
# Order matches websdr.cfg band list; ssrc = centerfreq.
cd /home/radio/

start_band() {
    local ssrc="$1" data="$2" fifo="$3"
    # No per-band sleep: a writer blocks on opening the fifo until the websdr
    # reader appears, so the FIFO handshake already enforces ordering. The old
    # "sleep 3" per band (x12 = ~36 s) stretched every start for no reason.
    pcmrecord -c -r -S "$ssrc" "$data" > "$fifo" &
}

start_band 29100 "10m-high-pcm.local" /home/radio/fifo/fifo10mH
start_band 28350 "10m-low-pcm.local"  /home/radio/fifo/fifo10mL
start_band 27400 "11m-pcm.local"      /home/radio/fifo/fifo11m
start_band 24940 "12m-pcm.local"      /home/radio/fifo/fifo12m
start_band 21225 "15m-pcm.local"      /home/radio/fifo/fifo15m
start_band 18118 "17m-pcm.local"      /home/radio/fifo/fifo17m
start_band 14175 "20m-pcm.local"      /home/radio/fifo/fifo20m
start_band 10150 "30m-pcm.local"      /home/radio/fifo/fifo30m
start_band 7100  "40m-pcm.local"      /home/radio/fifo/fifo40m
start_band 4625  "uvb-pcm.local"      /home/radio/fifo/fifoUVB
start_band 3660  "80m-pcm.local"      /home/radio/fifo/fifo80m
start_band 1895  "160m-pcm.local"     /home/radio/fifo/fifo160m

# Wait for ALL writers, not just the first one to exit. With `wait -n` a single
# pcmrecord dying (e.g. EPIPE when websdr.service restarts and briefly closes a
# fifo) makes this script exit, and systemd (Restart=always) then SIGKILLs the
# whole cgroup — every band's writer, not just the dead one. Waiting for all of
# them keeps the process alive while at least one writer runs; when every writer
# is gone, wait returns, the script exits and Restart=always rebuilds the set.
wait