/*
 * Zoom FFT — per-zoom sub-band spectrum, exactly like the reference websdr64.
 *
 * The full-band FFT (FFT_SIZE=32768, waterfall.c) gives bin = 11.7 Hz for
 * 384k and is used for zoom 0..2. For zoom >= ZOOM_FFT_MIN_ZOOM (3) the
 * reference server does NOT magnify the full-band spectrum ("magnifying
 * glass": the same bins get stretched, so signals smear into wide feathered
 * bands). Instead it decimates the raw IQ to a bandwidth that equals the
 * zoomed window and runs a fresh FFT whose bin width equals one display
 * pixel, so a narrow signal is 1-3 px at any zoom.
 *
 * Geometry: zoom z window = 1024 px * (K>>z) full-band bins
 * (K = FFT_SIZE/1024 = 32). Decimate the IQ by D = 1<<z, then a 1024-point
 * FFT of the decimated stream has bin = (sr/D)/1024 = sr/(1024<<z) =
 * full-band bin * (K >> z) = one pixel. The client's `start` (in
 * maxzoom-pixel units) selects the window's left edge.
 *
 * Only zoom levels actually in use are computed (band->zoom_fft[z].active).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>

#include "websdr.h"

#define ZFFT_PTS 1024

static float zfft_window[ZFFT_PTS];

void zoom_fft_global_init(void) {
    for (int i = 0; i < ZFFT_PTS; i++) {
        double x = (double)i / (ZFFT_PTS - 1);
        zfft_window[i] = 0.35875f
            - 0.48829f * cosf(2.0 * M_PI * x)
            + 0.14128f * cosf(4.0 * M_PI * x)
            - 0.01168f * cosf(6.0 * M_PI * x);
    }
}

/* (Re)create the per-zoom state for zoom z (decim = 1<<z). */
static void zoom_fft_init(struct band *band, int z) {
    struct zoom_fft_state *s = &band->zoom_fft[z];
    if (s->active) {
        if (s->decim != (1 << z)) {
            /* zoom changed: reset */
            if (s->plan) fftwf_destroy_plan(s->plan);
            free(s->fir_delay_i); free(s->fir_delay_q);
            free(s->accum_buf); free(s->fft_in); free(s->fft_out); free(s->power);
            memset(s, 0, sizeof(*s));
        } else {
            return; /* already running at this zoom */
        }
    }
    s->zoom = z;
    s->decim = 1 << z;
    s->active = 1;
    /* simple box-car FIR of length decim (enough: decim is a power of two and
     * the decimated bandwidth is well below the FIR foldover for HF bands) */
    s->fir_len = s->decim;
    s->fir_delay_i = calloc((size_t)s->fir_len, sizeof(float));
    s->fir_delay_q = calloc((size_t)s->fir_len, sizeof(float));
    s->fir_pos = 0;
    s->accum_buf = calloc((size_t)ZFFT_PTS * 2, sizeof(float));
    s->accum_count = 0;
    s->accum_target = ZFFT_PTS;
    s->fft_in = fftwf_alloc_complex(ZFFT_PTS);
    s->fft_out = fftwf_alloc_complex(ZFFT_PTS);
    s->power = malloc((size_t)ZFFT_PTS * sizeof(float));
    s->plan = fftwf_plan_dft_1d(ZFFT_PTS, s->fft_in, s->fft_out,
                                FFTW_FORWARD, FFTW_MEASURE);
}

/* Feed raw int16 IQ (ns16 samples) to every active zoom state.
 * Called under the band lock from band_thread (fifo_reader.c). */
void zoom_fft_feed(struct band *band, const int16_t *iq, int ns16) {
    for (int z = ZOOM_FFT_MIN_ZOOM; z <= band->maxzoom; z++) {
        struct zoom_fft_state *s = &band->zoom_fft[z];
        if (!s->active) continue;
        const int D = s->decim;
        /* take every D-th IQ pair (decimate). decim_counter keeps phase
         * across FIFO chunks so the sub-band window stays contiguous.
         * The accumulated window covers full-band bins [left_bin, left_bin+1024),
         * i.e. decimated samples left_bin/D .. left_bin/D + 1024 - 1 of the
         * decimated stream (a fresh window starts at each left_bin; for a fixed
         * start the window advances as new decimated samples arrive). */
        /* window left edge in full-band bins from the client start */
        int maxzoom = band->maxzoom;
        const int Kz = (FFT_SIZE / WATERFALL_WIDTH);
        long left_bin = ((long)s->start * Kz) >> maxzoom;
        long target_start = left_bin / D;
        for (int i = 0; i + 1 < ns16; i += 2) {
            if (s->decim_counter > 0) { s->decim_counter--; continue; }
            s->decim_counter = D - 1;
            if (s->dstream_pos < target_start) { s->dstream_pos++; continue; }
            s->dstream_pos++;
            s->accum_buf[2*s->accum_count]     = (float)iq[i];
            s->accum_buf[2*s->accum_count + 1] = (float)iq[i + 1];
            s->accum_count++;
            if (s->accum_count >= s->accum_target) {
                for (int k = 0; k < ZFFT_PTS; k++) {
                    s->fft_in[2*k]   = s->accum_buf[2*k]   * zfft_window[k];
                    s->fft_in[2*k+1] = s->accum_buf[2*k+1] * zfft_window[k];
                }
                fftwf_execute(s->plan);
                for (int k = 0; k < ZFFT_PTS; k++) {
                    float re = s->fft_out[2*k], im = s->fft_out[2*k+1];
                    s->power[k] = (re * re + im * im) / (float)ZFFT_PTS + 1e-12f;
                }
                s->accum_count = 0;
            }
        }
    }
}

/* Power spectrum for zoom z (array of 1024 floats, bin0 = low edge of the
 * accumulated window). z must be >= ZOOM_FFT_MIN_ZOOM; NULL if not active. */
float *zoom_fft_get_power(struct band *band, int z) {
    if (z < ZOOM_FFT_MIN_ZOOM || z > band->maxzoom) return NULL;
    struct zoom_fft_state *s = &band->zoom_fft[z];
    return s && s->active ? s->power : NULL;
}

/* Enable zoom state z (client joined zoom z). On a NEW start position the
 * sub-band window must be rebuilt, so reset the accumulation. */
void zoom_fft_activate(struct band *band, int z, int start) {
    if (z < ZOOM_FFT_MIN_ZOOM || z > band->maxzoom) return;
    struct zoom_fft_state *s = &band->zoom_fft[z];
    int need = 0;
    if (!s->active) { zoom_fft_init(band, z); need = 1; }
    if (s->start != start) { s->start = start; need = 1; }
    if (need) { s->accum_count = 0; s->decim_counter = 0; }
}