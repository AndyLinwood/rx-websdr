#!/usr/bin/env python3
"""Capture full rows (client-exact decoder), detect 'rightward tail' artifacts
caused by lost rows (delta-accumulator blowup). Usage:
wf_cap_tail.py <zoom> <start> <band_idx> <seconds> [peak_thr] [host]
Writes rows to stdout as compact (t, max, xmax, tail_len, tail_level) and
prints a summary of tail events at the end.
"""
import sys, time, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wf_tables import Z, W
import websocket

ZOOM = int(sys.argv[1]); START = int(sys.argv[2])
BAND = int(sys.argv[3]) if len(sys.argv) > 3 else 8
SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 120.0
THR = int(sys.argv[5]) if len(sys.argv) > 5 else 180
HOST = sys.argv[6] if len(sys.argv) > 6 else "127.0.0.1"
PORT = 8095

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
t0 = time.time(); t_end = t0 + SECS
rows = 0; resets = 0; tails = []; normals = 0
while time.time() < t_end:
    try:
        op, pay = ws.recv_data()
    except Exception as e:
        print("recv", e); break
    if op != 2: continue
    if pay[0] == 0xFF:
        if len(pay) > 1 and pay[1] == 0x02:
            prev = [0] * 1024
            resets += 1
        continue
    r = decode_row(pay, prev)
    prev = r
    rows += 1
    pm = max(range(1024), key=lambda x: r[x])
    mx = r[pm]
    # tail: how far the row stays >= 60% of max going right from the peak
    if mx >= THR:
        t = 0
        while pm + 1 + t < 1024 and r[pm + 1 + t] >= mx * 0.6:
            t += 1
        if t >= 3:  # a 'smear' of >=3 px to the right of a strong peak
            tails.append((round(time.time() - t0, 1), pm, mx, t, r[pm + t] if pm + t < 1024 else -1))
        else:
            normals += 1
# persist full rows for later analysis? keep summary only
print(f"rows={rows} resets={resets} strong(>=thr)={normals + len(tails)}tails={len(tails)} over {time.time()-t0:.0f}s")
for ev in tails[:40]:
    print(f"TAIL t={ev[0]}s peak_x={ev[1]} max={ev[2]} len={ev[3]} end={ev[4]}")
ws.close()