/*
 * WebSDR Server — Waterfall "format 9" compression
 *
 * Bit-compatible encoder reconstructed from the official client decoder
 * (websdr-waterfall.js). Round-trip verified against that decoder.
 *
 * Model: decoded[k] = prev[k] + 16*R_k, R_k = sum of r_i for i<=k.
 * So R_k = round((new[k]-prev[k])/16), r_k = R_k - R_{k-1}.
 * Greedy per column: pick r from the current state m's codebook that
 * minimises |(targetR - R) - r|.
 *
 * prevrow is BOTH the encoder's delta baseline AND (after the call) the
 * row the client decoder will actually hold: we update prevrow[k] in place
 * to decoded[k] = clamp(prevrow[k] + 16*R_k, 8, 248) — exactly what the
 * served client computes. Keeping the baseline equal to the DECODED value
 * (not a re-quantisation of newrow) prevents the greedy ±16 quantisation
 * errors from freezing into a permanent offset: without it the encoder
 * thinks the client sits on q(newrow) while the client really sits on
 * q(newrow) ± 16, so the error is never compensated on the next row and the
 * row-mean slowly drifts away from the true level until the periodic
 * width-reset snaps it back (the "spectrum slowly sinks, then jumps back"
 * artefact). With a decoder-true baseline the next row's deltas compensate.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "websdr.h"
#include "wf_tables.h"

typedef struct { int r, w, P; } Code;

static Code CB[3][32];
static int CBn[3];
static int cb_ready = 0;

static int iabs(int x) { return x < 0 ? -x : x; }

/* round(x/16) to nearest integer (C truncates toward zero) */
static int iround16(int x) { return (x + (x < 0 ? -8 : 8)) / 16; }

/* Build the codebook: a (m,r) codeword of width w is valid only if the
 * address block [P, P+2^(8-w)) it owns is base-aligned and has uniform
 * (Z,W) — the low (8-w) payload bits belong to the next symbol. */
static void build_codebook(void) {
    if (cb_ready) return;
    for (int m = 0; m < 3; m++) {
        CBn[m] = 0;
        for (int P = 0; P < 128; P++) {
            int w = W[m * 128 + P];
            if (!w) continue;
            int size = 1 << (8 - w);
            if (P % size) continue;               /* base-aligned block */
            int r = Z[m * 128 + P], u = 1;
            for (int q = 0; q < size; q++)
                if (Z[m * 128 + P + q] != r || W[m * 128 + P + q] != w) { u = 0; break; }
            if (!u) continue;
            int j;
            for (j = 0; j < CBn[m]; j++) if (CB[m][j].r == r) break;
            if (j == CBn[m]) CB[m][CBn[m]++] = (Code){ r, w, P };
            else if (w < CB[m][j].w) { CB[m][j].w = w; CB[m][j].P = P; }
        }
        CB[m][CBn[m]++] = (Code){ 0, 1, 0 };      /* escape r=0 */
    }
    cb_ready = 1;
}

/* MSB-first bit writer. emit(v,n) writes the top n bits of v. */
typedef struct {
    unsigned char *out;
    size_t len, cap;
    int cur, nb;
} BW;

static void bw_init(BW *b, unsigned char *out, size_t cap) {
    b->out = out; b->cap = cap; b->len = 0; b->cur = 0; b->nb = 0;
}

static void bw_emit(BW *b, int v, int n) {
    for (int i = 0; i < n; i++) {
        int bit = (v >> (n - 1 - i)) & 1;
        b->cur = (b->cur << 1) | bit;
        b->nb++;
        if (b->nb == 8) {
            b->out[b->len++] = (unsigned char)b->cur;
            b->cur = 0; b->nb = 0;
        }
    }
}

/* Flush remaining bits, left-aligned to the MSB so the byte is read
 * MSB-first by the decoder. */
static size_t bw_finish(BW *b) {
    if (b->nb) {
        b->out[b->len++] = (unsigned char)(b->cur << (8 - b->nb));
        b->cur = 0; b->nb = 0;
    }
    return b->len;
}

/* Encode one waterfall row (newrow) as deltas from prevrow. On return
 * prevrow holds the row the client decoder will have (decoder-true baseline).
 * Returns the number of bytes written to out (>=1 pad byte included). */
int compress_waterfall_format9(const uint8_t *newrow, uint8_t *prevrow,
                               int width, uint8_t *out) {
    build_codebook();

    BW bw;
    bw_init(&bw, out, (size_t)width * 2 + 16);

    int R = 0, m = 0;
    for (int k = 0; k < width; k++) {
        int delta = iround16((int)newrow[k] - (int)prevrow[k]) - R;
        int br = 0, be = 1 << 30, bwid = 0, bp = 0;
        for (int j = 0; j < CBn[m]; j++) {
            int e = iabs(delta - CB[m][j].r);
            if (e < be) { be = e; br = CB[m][j].r; bwid = CB[m][j].w; bp = CB[m][j].P; }
        }
        if (br == 0)
            bw_emit(&bw, 0, 1);
        else
            bw_emit(&bw, (0x80 | bp) >> (8 - bwid), bwid);
        R += br;
        /* m MUST mirror the served client decoder (websdr-waterfall.js)
         * byte-for-byte, or the encoder picks from a different codebook than
         * the decoder reads and the stream desynchronises (decoded rows sit
         * at a wrong baseline and drift to saturation). The served decoder is:
         *     1==r || -1==r  ->  m = 1
         *     1<r  || -1>r   ->  m = 2     (any |r| > 1)
         *     0==r           ->  m = 0
         */
        if (br == 1 || br == -1) m = 1;
        else if (br == 0) m = 0;
        else m = 2;   /* |br| > 1 */

        /* Decoder-true baseline: the client holds prevrow[k] + 16*R_k,
         * clamped 8..248 (websdr-waterfall.js: `0>r&&(r=8); 255<r&&(r=248)`).
         * Mirror it exactly so the next row's deltas compensate any greedy
         * quantisation error instead of freezing it (see header comment). */
        int decoded = (int)prevrow[k] + 16 * R;
        if (decoded < 0) decoded = 8;
        if (decoded > 255) decoded = 248;
        prevrow[k] = (uint8_t)decoded;
    }

    size_t n = bw_finish(&bw);
    /* decoder reads n[h+1] even on the last byte: guarantee a pad byte */
    if (n >= (size_t)width * 2 + 16) n = (size_t)width * 2 + 16;
    out[n++] = 0x00;
    return (int)n;
}
