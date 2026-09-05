#!/usr/bin/env python3
"""Long capture of ONE waterfall stream; track the peak x over time to detect
frequency drift of the clock (RX888 hypothesis).

Usage: wf_cap_drift.py <zoom> <start> <band_idx> <minutes> [threshold]
Client-exact format-9 decoder (wf_tables).
Peak tracking uses only rows whose max is above a brightness threshold
(CW carrier on an empty window = single bright pixel; noise rows are skipped).
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wf_tables import Z, W
import websocket

ZOOM = int(sys.argv[1]); START = int(sys.argv[2])
BAND = int(sys.argv[3]) if len(sys.argv) > 3 else 6
MINUTES = float(sys.argv[4]) if len(sys.argv) > 4 else 2.0
THRESH = int(sys.argv[5]) if len(sys.argv) > 5 else 190
HOST = "178.215.147.20"; PORT = 8095
# definition: at zoom z each row = FFT_SIZE bins mapped onto 1024 px; step = FFT_SIZE/1024 >> z.
# (natural freq resolution follows from the band's actual samplerate; caller may override)
SR = 384000.0
if len(sys.argv) > 6:
    SR = float(sys.argv[6])

url = f"ws://{HOST}:{PORT}/~~waterstream{BAND}?format=9&width=1024&zoom={ZOOM}&start={START}"
ws = websocket.create_connection(url, origin=f"http://{HOST}", timeout=15)
ws.send(f"GET /~~waterparam?band={BAND}&zoom={ZOOM}&start={START}&slow=0")

def decode_row(data, prev):
    out = prev[:]
    f = h = m = s = k = 0; n = len(data)
    while k < 1024:
        if h + 1 >= n: break
        l = (data[h] << (8 + f)) | (data[h + 1] << f)
        l &= 0xFFFF
        r = 0; wb = 1
        if l & 32768:
            idx = 128 * m + ((l & 32512) >> 8)
            r = Z[idx] if idx < len(Z) else 0
            wb = W[idx] if idx < len(W) else 1
        f += wb
        if f >= 8: f -= 8; h += 1
        if r == 1 or r == -1: m = 1
        elif r > 1 or r < -1: m = 2
        elif r == 0: m = 0
        s += r << 4
        v = out[k] + s
        out[k] = 8 if v < 0 else (248 if v > 255 else v)
        k += 1
    return out

prev = [0] * 1024
t0 = time.time(); t_end = t0 + MINUTES * 60
xs = []; vals = []; rows = 0; above = 0
while time.time() < t_end:
    try:
        op, pay = ws.recv_data()
    except Exception as e:
        print("recv", e); break
    if op != 2: continue
    if pay[0] == 0xFF:
        if len(pay) > 1 and pay[1] == 0x02: prev = [0] * 1024
        continue
    r = decode_row(pay, prev)
    prev = r
    rows += 1
    pm = max(range(1024), key=lambda x: r[x])
    v = r[pm]
    if v >= THRESH:
        above += 1
        xs.append(pm); vals.append(v)
        if above % 30 == 0:
            print(f"{time.time()-t0:7.1f},{pm},{v}", flush=True)
ws.close()

import numpy as np
print(f"rows={rows} over {time.time()-t0:.0f}s, above-thr={above} ({100.0*above/max(rows,1):.0f}%)")
if not xs:
    print("NO peak above threshold — check zoom/start/frequency or lower threshold")
    sys.exit(1)
xs = np.array(xs)
print(f"peak x: mean={xs.mean():.1f} min={xs.min()} max={xs.max()} span={xs.max()-xs.min()} "
      f"std={xs.std():.1f} p90={np.percentile(xs,90):.0f}")
step = (32768 // 1024) >> ZOOM
hz_per_px = SR / 32768.0 * step
print(f"hz_per_px={hz_per_px:.2f} -> drift span {xs.max()-xs.min()} px = {(xs.max()-xs.min())*hz_per_px:.0f} Hz")
print("percentiles of x:", np.percentile(xs, [1, 25, 50, 75, 99]))