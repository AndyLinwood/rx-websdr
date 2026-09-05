#!/usr/bin/env python3
"""Capture live server waterfall rows (format-9) at a given zoom/start,
decode exactly like the client (websdr-waterfall.js lines ~316-338) and
analyze the 1024-value brightness row: signal peaks, gaps, background.
Also prints freq window per pixel.

Usage: wf_cap.py <zoom> <start> <band_idx> [seconds]
"""
import sys, time, json, math
import socket, base64, struct
try:
    import websocket
except ImportError:
    sys.exit("pip install websocket-client on this venv")

BAND_IDX = int(sys.argv[3]) if len(sys.argv) > 3 else 6  # 20m = idx 6
ZOOM = int(sys.argv[1])
START = int(sys.argv[2])
SECS = float(sys.argv[4]) if len(sys.argv) > 4 else 4.0
HOST = "178.215.147.20"; PORT = 8095

# ---- open websocket like the client ----
ws_url = f"ws://{HOST}:{PORT}/~~waterstream{''}{BAND_IDX}?format=9&width=1024&zoom={ZOOM}&start={START}"
print("connecting", ws_url)
ws = websocket.create_connection(ws_url, origin=f"http://{HOST}", timeout=10)

# ---- format-9 decoder (from websdr-waterfall.js) ----
R = [0]*256
for i in range(256): R[i]=0
# Huffman tables from client: Z[l], $[l] — but client builds them elsewhere.
# We'll decode directly from the compressor tables (wf_tables.h) signature: 
# The format: velocity predictor R = prevR + r*16, newP = clamp(prevP + R, 8, 248).
# Reconstruct from compress.c logic instead (equivalent decoder-true baseline).

def decode_row(data, prev):
    """prev: list 1024; data: bytes; returns new row. Mirrors client: m=1/2/-1/-2/0 model."""
    out = prev[:]
    f = 0; h = 0; m = 0; s = 0; k = 0
    n = len(data)
    while k < 1024:
        if h + 1 >= n: break
        l = (data[h] << (8 + f) | data[h+1] << f) & 0xFFFF
        # client: l&32768? huffman else raw 2-bit
        r = 0; w = 1
        if l & 32768:
            idx = 128*m + ((l & 32512) >> 8)
            # r = Z[idx], w = $[idx] — need tables; approximate: parse codebook
            # For a robust capture, use structural decode: read packets until 1024
            # -> not possible without tables. So: NOTE — we need exact tables.
            r, w = 0, 1  # placeholder
        f += w
        if f >= 8: f -= 8; h += 1
        if r == 1 or r == -1: m = 1
        elif r > 1 or r < -1: m = 2
        elif r == 0: m = 0
        s += r << 4
        r = out[k] + s
        if r < 0: r = 8
        if r > 255: r = 248
        out[k] = r
        k += 1
    return out

# fallback: also try a simpler approach — read raw and print hex head
ws.send("GET /~~waterparam?band=%d&zoom=%d&start=%d&slow=0 HTTP/1.1\r\nHost: %s\r\n\r\n" % (BAND_IDX, ZOOM, START, HOST))
time.sleep(0.3)
rows = []
t0 = time.time()
while time.time() - t0 < SECS:
    try:
        opcode, payload = ws.recv_data()
    except Exception as e:
        print("recv err", e); break
    if opcode != 2:  # binary
        continue
    if payload[0] == 0xFF:
        # init/control frame 0xFF 0x01 zoom start | 0xFF 0x02 width
        print("CTL", payload[:8].hex())
        prev = [0]*1024
        continue
    # data row
    rows.append(payload)
    if len(rows) >= 8: break

print(f"\ncaptured {len(rows)} rows")
for i, p in enumerate(rows[:5]):
    print(f"row{i}: len={len(p)} head={p[:20].hex()}")

# Decode with real Huffman tables extracted from wf_tables.h
try:
    sys.path.insert(0, "/tmp")
    from wf_tables import Z, DOLLAR  # if available
    print("tables loaded")
except Exception as e:
    print("no tables:", e)

ws.close()