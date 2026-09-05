#!/usr/bin/env python3
"""Capture with explicit waterparam request inside the WS socket (protocol.c
requires it, otherwise the server sends no rows). Client-exact format-9 decoder."""
import sys, time
sys.path.insert(0, "/tmp")
from wf_tables import Z, W
import websocket

ZOOM = int(sys.argv[1]); START = int(sys.argv[2])
BAND = int(sys.argv[3]) if len(sys.argv)>3 else 6
SECS = float(sys.argv[4]) if len(sys.argv)>4 else 6.0
HOST="178.215.147.20"; PORT=8095

url = f"ws://{HOST}:{PORT}/~~waterstream{BAND}?format=9&width=1024&zoom={ZOOM}&start={START}"
ws = websocket.create_connection(url, origin=f"http://{HOST}", timeout=10)
ws.send(f"GET /~~waterparam?band={BAND}&zoom={ZOOM}&start={START}&slow=0")

def decode_row(data, prev):
    out = prev[:]
    f=h=m=s=k=0; n=len(data)
    while k < 1024:
        if h+1 >= n: break
        l = (data[h] << (8+f)) | (data[h+1] << f)
        l &= 0xFFFF
        r=0; wb=1
        if l & 32768:
            idx = 128*m + ((l & 32512) >> 8)
            r = Z[idx] if idx < len(Z) else 0
            wb = W[idx] if idx < len(W) else 1
        f += wb
        if f >= 8: f -= 8; h += 1
        if r==1 or r==-1: m=1
        elif r>1 or r<-1: m=2
        elif r==0: m=0
        s += r << 4
        v = out[k] + s
        out[k] = 8 if v<0 else (248 if v>255 else v)
        k += 1
    return out

prev=[0]*1024; rows=[]; t0=time.time()
while time.time()-t0 < SECS:
    try:
        op, pay = ws.recv_data()
    except Exception as e:
        print("recv", e); break
    if op != 2: continue
    if pay[0]==0xFF:
        if len(pay)>1 and pay[1]==0x02: prev=[0]*1024
        continue
    rows.append(decode_row(pay, prev))
    prev = rows[-1]
    if len(rows)>=30: break
ws.close()
print(f"rows={len(rows)}")
if not rows: sys.exit()
import numpy as np
A = np.array(rows)
for thr in (90,120,150):
    widths=[]
    for r in A:
        mask=r>=thr; inr=False; s=0
        for x in range(1024):
            if mask[x] and not inr: inr=True; s=x
            elif not mask[x] and inr: inr=False; widths.append(x-s)
        if inr: widths.append(1024-s)
    if widths:
        print(f"thr={thr}: runs={len(widths)} med={np.median(widths):.0f} p90={np.percentile(widths,90):.0f} max={max(widths)}")
rowm = A.mean(axis=0)
print("ASCII (1ch=4px, around center 512±64):")
chars=" .:-=+*#%@"
for x0 in range(448, 576+1, 4):
    v=rowm[x0:x0+4].mean()
    sys.stdout.write(chars[min(len(chars)-1,int(v/256*len(chars)))])
print()
# brightest pixel position stability
peaks=[]
for r in A:
    pm=max(range(1024), key=lambda x: r[x])
    peaks.append(pm)
print("peak x:", " ".join(str(p) for p in peaks[:20]))
