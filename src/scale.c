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

    /* chunk writer: 4-byte length (BE) + type + data + CRC32(BE of type+data).
     * `data` must be non-NULL even when len==0 (pass "" for IEND). */
    static const unsigned char empty[1] = {0};
#define WCHUNK(type, data, len) do { \
        uint32_t bl = (uint32_t)(len); \
        unsigned char lh[4] = {(unsigned char)(bl>>24),(unsigned char)(bl>>16), \
                               (unsigned char)(bl>>8),(unsigned char)bl}; \
        fwrite(lh, 1, 4, fp); \
        fwrite((type), 1, 4, fp); \
        if (len) fwrite((data), 1, (len), fp); \
        uLong crc = crc32(0L, 0, 0); \
        crc = crc32(crc, (const Bytef *)(type), 4); \
        if (len) crc = crc32(crc, (const Bytef *)(data), (uInt)(len)); \
        unsigned char ch[4] = {(unsigned char)(crc>>24),(unsigned char)(crc>>16), \
                               (unsigned char)(crc>>8),(unsigned char)crc}; \
        fwrite(ch, 1, 4, fp); \
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

    WCHUNK((const unsigned char *)"IEND", empty, 0);
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

/* Generate every scale tile for a band into pubdir/tmp.
 * Returns 0 on success. */
static int generate_band_tiles(const char *pubdir, const char *ts,
                               int bandidx, struct band *band) {
    int mz = band->maxzoom;
    double centerHz = band_eff_center(band) * 1000.0;
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

            /* Major (labeled) ticks ~120px apart, with finer minor ticks
             * in between (major/5). Both adapt to the Hz-per-pixel, so the
             * step scales with band width and zoom: on 40m at max zoom-out
             * (375 Hz/px) this yields ~10 kHz minor marks. */
            /* Frequency-step ladder, proportional to band width via a base
             * unit (1 kHz for a 384 kHz band): full band -> 10x base (10 kHz
             * on 40m), mid zooms -> base (1 kHz), deepest zoom -> base/10
             * (100 Hz on 40m; 10 Hz would be <1px, unreadable at z4).
             * Labelled (major) ticks land every `division` minors, where the
             * division cycles 10,5,5 counting from the deepest zoom (deepest
             * is always 10): 40m z4=10 (labels 7098/7099, 10 ticks), z3=5,
             * z2=5, z1=10 (labels 7090/7100, 10 ticks), z0=5. */
            double base = band->samplerate / 384.0;   /* ~1000 Hz for 384k */
            double minor = base;
            if (z == 0) minor = base * 10.0;   /* max out: 10 kHz on 40m */
            else if (z == mz) minor = base / 10.0;  /* deepest: 100 Hz */
            static const double div_cycle[3] = {10.0, 5.0, 5.0};
            double division = div_cycle[(mz - z) % 3];
            double major = minor * division;

            /* minor ticks (short) */
            for (double f = ceil(tileLower / minor) * minor;
                 f < tileLower + span; f += minor) {
                int x = (int)round((f - tileLower) / hzperpix);
                if (x < 0 || x >= SCALE_W) continue;
                for (int yy = 0; yy < 3 && yy < SCALE_H; yy++)
                    px[yy * SCALE_W + x] = 1;
            }

            /* semi-major tick (medium, the 5th of 10) halfway to a labelled
             * tick — only when there are an EVEN number of ticks between the
             * labelled ones (division 10), so the centre lands on a tick. With
             * 5 ticks between (division 5) there is no centre tick, so none
             * is drawn here. */
            if ((int)division % 2 == 0)
            for (double f = ceil((tileLower - major / 2.0) / major) * major;
                 f < tileLower + span; f += major) {
                int x = (int)round((f + major / 2.0 - tileLower) / hzperpix);
                if (x < 0 || x >= SCALE_W) continue;
                for (int yy = 0; yy < 5 && yy < SCALE_H; yy++)
                    px[yy * SCALE_W + x] = 1;
            }

            /* major ticks (tall) + kHz label. Label sits below the tick's
             * bottom edge (row 7, one past the 7px tick) so digits don't
             * collide with the tick marks. */
            for (double f = ceil(tileLower / major) * major;
                 f < tileLower + span; f += major) {
                int x = (int)round((f - tileLower) / hzperpix);
                if (x < 0 || x >= SCALE_W) continue;
                for (int yy = 0; yy < 7 && yy < SCALE_H; yy++)
                    px[yy * SCALE_W + x] = 1;
                long label = (long)llround(f / 1000.0);
                /* center the label on the tick, below the tick bottom */
                int nd = snprintf(NULL, 0, "%ld", label);
                int lx = x - (int)(nd * 6) / 2;
                if (lx < 0) lx = 0;
                draw_num(px, lx, 7, label);
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
