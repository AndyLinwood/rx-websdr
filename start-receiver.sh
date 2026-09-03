#!/bin/bash
# start-receiver.sh — feed every WebSDR band's fifo with pcmrecord from radiod.
# One pcmrecord per fifo; writer opens (blocks) until the websdr reader appears.
# Order matches websdr.cfg band list; ssrc = centerfreq.
cd /home/radio/

start_band() {
    local ssrc="$1" data="$2" fifo="$3"
    pcmrecord -c -r -S "$ssrc" "$data" > "$fifo" &
    sleep 3
}

start_band 29100 "10m-high-pcm.local" /home/radio/fifo/fifo10mHH
start_band 28350 "10m-low-pcm.local"  /home/radio/fifo/fifo10mLH
start_band 27400 "11m-pcm.local"      /home/radio/fifo/fifo11mH
start_band 24940 "12m-pcm.local"      /home/radio/fifo/fifo12mH
start_band 21225 "15m-pcm.local"      /home/radio/fifo/fifo15mL
start_band 18118 "17m-pcm.local"      /home/radio/fifo/fifo17mL
start_band 14175 "20m-pcm.local"      /home/radio/fifo/fifo20mL
start_band 10150 "30m-pcm.local"      /home/radio/fifo/fifo30mL
start_band 7100  "40m-pcm.local"      /home/radio/fifo/fifo40mL
start_band 4625  "uvb-pcm.local"      /home/radio/fifo/fifoUVB
start_band 3660  "80m-pcm.local"      /home/radio/fifo/fifo80mL
start_band 1895  "160m-pcm.local"     /home/radio/fifo/fifo160mL

wait -n
