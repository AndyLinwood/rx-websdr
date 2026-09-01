/*
 * WebSDR Server — scale-tile PNG generation
 *
 * Generates the frequency-ruler tiles that bandinfo.js references (one
 * 1024x14 PNG per (band, zoom, tile)). Tile (band b, zoom z, index k) covers
 * the absolute frequency range
 *     [centerfreq - samplerate/2 + k*(samplerate/2^z),
 *      + samplerate/2^z)
 * at 1024px, so each pixel = (samplerate/2^z)/1024 Hz.
 *
 * A tile is drawn as a transparent 1024x14 palette-PNG with vertical tick
 * marks at "nice" frequency steps and their kHz labels (5x7 pixel font).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <zlib.h>

#include "websdr.h"

#define SCALE_W 1024
#define SCALE_H 14

/* ------------------------------------------------------------------ */
/* Minimal indexed-colour PNG writer (zlib for IDAT).                  */
/* index 0 is fully transparent (tRNS); the rest come from `pal`.      */
/* ------------------------------------------------------------------ */
static int png_write_indexed(const char *path, const uint8_t *idx,
                             size_t w, size_t h, const uint8_t (*pal)[3]) {
    unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    fwrite(sig, 1, 8, fp);

    unsigned char ihdr[13];
    uint32_t dw = (uint32_t)w, dh = (uint32_t)h;
    ihdr[0] = (unsigned char)(dw >> 24); ihdr[1] = (unsigned char)(dw >> 16);
    ihdr[2] = (unsigned char)(dw >> 8);  ihdr[3] = (unsigned char)dw;
    ihdr[4] = (unsigned char)(dh >> 24); ihdr[5] = (unsigned char)(dh >> 16);
    ihdr[6] = (unsigned char)(dh >> 8);  ihdr[7] = (unsigned char)dh;
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 3;   /* colour type 3 = indexed */
    ihdr[10] = 0;  /* compression */
    ihdr[11] = 0;  /* filter */
    ihdr[12] = 0;  /* interlace */

    /* chunk writer: type + crc(data) */
    /* build into memory to compute CRC */
    unsigned char crcbuf[1] = {'x'};
#define WCHUNK(type, data, len) do { \
        uint32_t bl = (uint32_t)(len); \
        fwrite(&bl, 4, 1, fp); \
        fwrite((type), 1, 4, fp); \
        fwrite((data), 1, (len), fp); \
        uLong crc = crc32(0L, Z_NULL, 0); \
        crc = crc32(crc, (type), 4); \
        if (len) crc = crc32(crc, (data), (uInt)(len)); \
        fwrite(&crc, 4, 1, fp); \
    } while (0)
    WCHUNK((const unsigned char *)"IHDR", ihdr, 13);

    unsigned char trns[1] = {0};          /* palette index 0 = transparent */
    WCHUNK((const unsigned char *)"tRNS", trns, 1);

    /* palette: index 0 = (0,0,0) transparent, index 1 = draw colour */
    unsigned char plte[2][3] = {{0, 0, 0}, {255, 255, 255}};
    memcpy(plte, pal, 6);
    WCHUNK((const unsigned char *)"PLTE", plte, 6);

    /* IDAT: raw scanlines (filter byte 0 + w index bytes), zlib-compressed.
     * Indexed pixels are a byproduct of reading `pal`: copy idx bytes to raw. */
    size_t stride = w + 1;
    uLongf rawlen = (uLongf)(stride * h);
    unsigned char *raw = (unsigned char *)malloc(rawlen);
    if (!raw) { fclose(fp); return -1; }
    for (size_t r = 0; r < h; r++) {
        raw[r * stride] = 0;   /* filter: none */
        memcpy(raw + r * stride + 1, idx + r * w, w);
    }
    uLongf zlen = compressBound(rawlen);
    unsigned char *zbuf = (unsigned char *)malloc(zlen);
    if (!zbuf) { free(raw); fclose(fp); return -1; }
    if (compress2(zbuf, &zlen, raw, rawlen, 6) != Z_OK) {
        free(raw); free(zbuf); fclose(fp); return -1;
    }
    WCHUNK((const unsigned char *)"IDAT", zbuf, (uInt)zlen);
    free(zbuf);
    free(raw);

    WCHUNK((const unsigned char *)"IEND", NULL, 0);
    fclose(fp);
    return 0;
#undef WCHUNK
}

/* ------------------------------------------------------------------ */
/* 5x7 pixel font, rows are 5-bit patterns (bit 4 = leftmost).         */
/* ------------------------------------------------------------------ */
static const uint8_t FONT[10][7] = {
    {0x0e,0x11,0x13,0x15,0x19,0x11,0x0e}, /* 0 */
    {0x04,0x0c,0x04,0x04,0x04,0x04,0x0e}, /* 1 */
    {0x1e,0x01,0x01,0x02,0x04,0x08,0x1f}, /* 2 */
    {0x1e,0x01,0x01,0x1e,0x01,0x01,0x1e}, /* 3 */
    {0x02,0x06,0x0a,0x12,0x1f,0x02,0x02}, /* 4 */
    {0x1f,0x10,0x1e,0x01,0x01,0x01,0x1e}, /* 5 */
    {0x0e,0x10,0x10,0x1e,0x11,0x11,0x0e}, /* 6 */
    {0x1f,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x1e,0x11,0x11,0x1e,0x11,0x11,0x1e}, /* 8 */
    {0x1e,0x11,0x11,0x0f,0x01,0x01,0x0e}  /* 9 */
};

/* draw one 5x7 digit with top-left at (x,y); set idx=1 for set pixels. */
static void draw_glyph(uint8_t *px, int x, int y, int digit) {
    for (int r = 0; r < 7; r++) {
        uint8_t bits = FONT[digit][r];
        for (int c = 0; c < 5; c++) {
            if (bits & (0x10 >> c)) {
                int X = x + c, Y = y + r;
                if (X >= 0 && X < SCALE_W && Y >= 0 && Y < SCALE_H)
                    px[Y * SCALE_W + X] = 1;
            }
        }
    }
}

/* draw a decimal integer (e.g. kHz) as 5x7 digits, right-not-required. */
static void draw_num(uint8_t *px, int x, int y, long v) {
    char buf[16];
    int n = snprintf(buf, sizeof(buf), "%ld", v);
    for (int i = 0; i < n; i++) {
        if (buf[i] >= '0' && buf[i] <= '9')
            draw_glyph(px, x + i * 6, y, buf[i] - '0');
    }
}

/* ------------------------------------------------------------------ */
/* Scale tile generator                                                */
/* ------------------------------------------------------------------ */

/* choose a "nice" tick step (1,2,5 x10^n Hz) so ticks land every ~90px */
static double nice_step(double hzperpix) {
    double target = hzperpix * 90.0;
    double m = pow(10.0, floor(log10(target > 0 ? target : 1)));
    double b = target / m;
    double n = (b <= 1.0 ? 1.0 : b <= 2.0 ? 2.0 : b <= 5.0 ? 5.0 : 10.0) * m;
    return n;
}

/* Generate every scale tile for a band into pubdir/tmp.
 * Returns 0 on success. */
static int generate_band_tiles(const char *pubdir, const char *ts,
                               int bandidx, struct band *band) {
    int mz = band->maxzoom;
    double centerHz = band->centerfreq * 1000.0;
    int sr = band->samplerate;
    uint8_t pal[2][3] = {{0, 0, 0}, {255, 255, 255}};
    uint8_t *px = (uint8_t *)malloc(SCALE_W * SCALE_H);
    if (!px) return -1;

    char path[512];
    char fname[64];

    for (int z = 0; z <= mz; z++) {
        int ntiles = 1 << z;
        double span = sr / (double)(1 << z);       /* Hz per tile */
        double hzperpix = span / (double)SCALE_W;
        for (int k = 0; k < ntiles; k++) {
            memset(px, 0, SCALE_W * SCALE_H);
            double tileLower = centerHz - sr / 2.0 + k * span;

            double step = nice_step(hzperpix);
            double f0 = ceil(tileLower / step) * step;

            for (double f = f0; f < tileLower + span; f += step) {
                int x = (int)round((f - tileLower) / hzperpix);
                if (x < 0 || x >= SCALE_W) continue;
                /* major tick: 7px tall; minor ticks at half-step 3px */
                int major = (int)round(f / step) % 2 == 0; /* alternate */
                int th = major ? 7 : 3;
                for (int yy = 0; yy < th && yy < SCALE_H; yy++)
                    px[yy * SCALE_W + x] = 1;
                if (major) {
                    /* label: frequency in kHz */
                    double fkhz = f / 1000.0;
                    long label = (long)llround(fkhz);
                    int lx = x - 8;
                    if (lx < 0) lx = 0;
                    draw_num(px, lx, 3, label);
                }
            }

            snprintf(fname, sizeof(fname), "%s-b%dz%di%d.png", ts, bandidx, z, k);
            snprintf(path, sizeof(path), "%s/tmp/%s", pubdir, fname);
            if (png_write_indexed(path, px, SCALE_W, SCALE_H, pal) != 0) {
                fprintf(stderr, "scale: failed to write %s\n", path);
                continue;
            }
        }
    }

    free(px);
    return 0;
}

/* Generate all scale tiles for all configured bands into pubdir/tmp. */
int scale_generate_all(const char *pubdir, const char *ts,
                       struct websdr_config *cfg) {
    for (int b = 0; b < cfg->nbands; b++)
        generate_band_tiles(pubdir, ts, b, &cfg->bands[b]);
    return 0;
}
