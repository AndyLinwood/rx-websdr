/*
 * WebSDR Server — Waterfall generator
 * FFT-based spectrum analysis for waterfall display
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>

#include "websdr.h"

static float window[FFT_SIZE];

/* Waterfall brightness map. Faithful to PhantomSDR fft_impl.cpp power_and_quantize
 * (brightness ~ 20*log10(linear_power)), anchored to the band's tracked noise
 * floor so every band's BACKGROUND lands at a fixed, stable brightness, plus the
 * per-band cfg `gain` as a dB colour-scale shift (original websdr semantics:
 * config.txt "gain x — Insert x dB of gain... it only shifts the waterfall colour
 * scale and the S meter").
 *
 *   brightness = WF_SLOPE * (20*log10(avg_power) - noise_dB) + WF_BG + gain
 *
 * WF_SLOPE > 1 gives the contrast the original websdr64 shows (a signal Δ dB
 * above the floor renders ~1.5*Δ brightness units): signals read brighter and
 * floor dips darker than the old flat 1:1 dB map, which made signals look dim
 * next to the production server. WF_BG is lowered so the noise floor lands at
 * ~64 (the production background level) instead of 80 on the busy HF bands.
 * When avg_power equals the noise floor, brightness = WF_BG + gain. */
#define WF_BG 38.0f   /* target background brightness (noise floor -> this, before gain) */
#define WF_SLOPE 1.5f /* contrast: brightness units per dB above the noise floor */
float wf_brightness(float avg_power, double noise_dB, double gain_db) {
    float d = 20.0f * log10f(avg_power) - (float)noise_dB;
    float v = WF_SLOPE * d + WF_BG + (float)gain_db;
    if (v < 0.0f)  v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return v;
}

static int cmp_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

void waterfall_init(void) {
    for (int i = 0; i < FFT_SIZE; i++) {
        double x = (double)i / (FFT_SIZE - 1);
        window[i] = 0.35875f
            - 0.48829f * cosf(2.0f * M_PI * x)
            + 0.14128f * cosf(4.0f * M_PI * x)
            - 0.01168f * cosf(6.0f * M_PI * x);
    }
}

void waterfall_process(struct band *band, int16_t *iq_data, int samples) {
    int fft_samples = samples;
    if (fft_samples > FFT_SIZE * 2) fft_samples = FFT_SIZE * 2;

    /* Convert IQ int16 to float, apply window. NOTE: do NOT apply band->swapiq
     * here. The waterfall shows the FULL band spectrum (a +50 kHz probe tone is
     * verified to land right of centre, pixel~645), independent of the IQ
     * polarity the audio path compensates for; swapping I/Q would mirror the
     * whole spectrum around the band centre (stations of the lower half shown
     * in the upper half and vice versa). */
    for (int i = 0; i < fft_samples / 2; i++) {
        band->fft_input[2*i]     = (float)iq_data[2*i] * window[i];
        band->fft_input[2*i + 1] = (float)iq_data[2*i + 1] * window[i];
    }
    /* zero-fill the FFT input if a partial block was read */
    if (fft_samples / 2 < FFT_SIZE) {
        int k = (fft_samples / 2) * 2;
        memset(band->fft_input + k, 0,
               (size_t)(FFT_SIZE * 2 - k) * sizeof(float));
    }

    if (band->fft_plan)
        fftwf_execute(band->fft_plan);

    /* Fixed-dB waterfall map, faithful to PhantomSDR fft_impl.cpp: brightness =
     * 20*log10(linear_power) + V0 (1 unit ~= 1 dB). NO temporal persistence and
     * NO spatial smoothing — spatial smoothing smears narrowband digital modes
     * (FT8 ~50 Hz / PSK31 ~31 Hz occupy 1-2 of the 46.9 Hz bins at FFT 8192; a
     * 5-bin box stretches them to ~234 Hz "размазня", user-verified [21 авг]).
     * The background smoothness comes from SUM-decimating many fine FFT bins
     * into each display pixel (server.c band_send_waterfall), and signal
     * crispness from those fine bins. power_hi holds the raw per-bin linear
     * power, normalized by FFT_SIZE (PhantomSDR power_and_quantize divides
     * complexbuf by `size`) so the 20*log10 map is well-scaled. */
    const int K = FFT_SIZE / WATERFALL_WIDTH;        /* bins per display pixel */
    for (int i = 0; i < FFT_SIZE; i++) {
        int bin = (i + FFT_SIZE / 2) % FFT_SIZE;
        float re = band->fft_output[2 * bin];
        float im = band->fft_output[2 * bin + 1];
        float power = (re * re + im * im) / (float)FFT_SIZE + 1e-12f;
        band->power_hi[i] = power;
    }

    /* Track the full-band noise floor: median of the 1024 min-zoom pixel powers,
     * low-passed so it moves slowly with the receiver noise but ignores
     * transients/signals. Use the median of SHIFTED (discard the hot small
     * fraction) so strong signals don't bias it upward. */
    {
        float ma[WATERFALL_WIDTH];
        for (int px = 0; px < WATERFALL_WIDTH; px++) {
            float s = 0.0f;
            for (int j = 0; j < K; j++) s += band->power_hi[px * K + j];
            ma[px] = s / (float)K;
        }
        qsort(ma, WATERFALL_WIDTH, sizeof(float), cmp_float);
        float med = ma[WATERFALL_WIDTH / 2];          /* robust median */
        double med_dB = 20.0 * log10((double)med);
        if (!band->noise_init) {
            band->noise_dB = med_dB;
            band->noise_init = 1;
        } else {
            band->noise_dB += 0.03 * (med_dB - band->noise_dB); /* slow EMA */
        }
    }
}
