/*
 * WebSDR Server — FIFO reader
 *
 * Reads raw LE int16 IQ from a (named-)pipe, accumulates a full FFT_SIZE
 * complex frame (a short zero-padded FFT would smear the spectrum into
 * spurious vertical stripes), runs a real FFT on each complete frame and
 * emits one waterfall row per FFT frame (~12 rows/s at FFT_SIZE 32768).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>

#include <libwebsockets.h>

#include "websdr.h"

#define BLOCK_SIZE (FFT_SIZE * 2 * sizeof(int16_t))

extern void band_send_waterfall(struct band *band);

/* True if any client on this band has an active audio stream. The band-wide
 * audio FFT (audio_fft_push_iq) only feeds per-client demodulators, so it must
 * not run on bands with no listeners: with 11 configured bands that was ~400
 * idle 12k-point FFTs/s of constant CPU (measured 46% with 2 users vs the
 * reference server's 37% with 55). Reading the client pointers without the
 * band lock is benign: worst case a stale value makes us compute one extra
 * FFT frame; the demod itself runs under the lock. */
static int band_has_audio_client(struct band *b) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = b->clients[i];
        if (c && c->audio_stream && c->audio_active)
            return 1;
    }
    return 0;
}

void *band_thread(void *arg) {
    struct band *band = (struct band *)arg;

    fprintf(stderr, "Band %s: opening %s\n", band->name, band->device);

    band->fifo_fd = open(band->device, O_RDONLY);
    if (band->fifo_fd < 0) {
        fprintf(stderr, "Band %s: cannot open %s: %s\n",
                band->name, band->device, strerror(errno));
        band->running = 0;
        return NULL;
    }

    int16_t *buffer = malloc(BLOCK_SIZE);          /* FIFO read chunk    */
    int16_t *frame  = malloc((size_t)FFT_SIZE * 2 * sizeof(int16_t)); /* full IQ frame */
    if (!buffer || !frame) {
        free(buffer); free(frame);
        close(band->fifo_fd);
        band->running = 0;
        return NULL;
    }

    fprintf(stderr, "Band %s: running (rate=%d Hz, center=%.1f kHz)\n",
            band->name, band->samplerate, band->centerfreq);

#if AUDIO_USE_FFT
    /* Band-wide audio FFT backbone (shared by all audio clients). */
    if (audio_fft_band_init(band) != 0)
        fprintf(stderr, "Band %s: audio FFT init FAILED, audio offline\n", band->name);
#endif

    /* Waterfall row cadence: average a few short FFT frames per emitted row,
     * like the original websdr64 (short FFT + temporal averaging at ~12 rows/s).
     * Emitting ONE 85 ms FFT frame per row lets a single-frame transient from a
     * strong signal (CW keying, FT8 tone change, squelch click) smear across
     * the whole band in that row -> a horizontal spur with ragged signal edges.
     * Averaging dilutes such transients and smooths the edges.
     *
     * The FFT frames OVERLAP 50% (sliding window): with non-overlapping frames
     * each frame starts at an arbitrary signal phase, so a tone's peak jitters
     * ±1 bin between rows and strong signals get ragged edges that read as
     * "a wide band of densely packed spurs". Overlap makes the window slide
     * smoothly (the reference VertexSDR cycles N/4 -> 3N/4 -> N), keeping the
     * peak's bin stable across rows. */
    /* Row cadence: average 4 short FFT frames (4 x 21 ms = 85 ms) per emitted
     * row, like the reference websdr64: short FFT (bin = 46.9 Hz) + temporal
     * power averaging at ~12 rows/s. Averaging LINEAR POWER keeps a stable tone
     * a single-bin peak (no frequency smear) while steadying the noise — the
     * one-bin-per-pixel geometry at zoom 3 is what makes signals thin lines.
     * Averaging post-contrast brightness (uint8) instead dims one-frame peaks
     * and blurs edges (the earlier failed attempt). */
    int emit_every = (int)((double)band->samplerate / (FFT_SIZE * 12.0) + 0.5);
    if (emit_every < 1) emit_every = 1;

    int acc_blocks = 0;
    int frame_pos = 0;                             /* int16 samples in current frame */
    uint8_t wf_m_prev[WATERFALL_WIDTH] = { 0 };    /* emitted row t-2 (median-3 window) */
    uint8_t wf_m_cur[WATERFALL_WIDTH] = { 0 };     /* emitted row t-1 */
    int wf_m_seen = 0;

    struct timespec _t0; clock_gettime(CLOCK_MONOTONIC, &_t0);
    while (band->running) {
        ssize_t n = read(band->fifo_fd, buffer, BLOCK_SIZE);
        {
            struct timespec _t1; clock_gettime(CLOCK_MONOTONIC, &_t1);
            long _ms = (_t1.tv_sec - _t0.tv_sec) * 1000 + (_t1.tv_nsec - _t0.tv_nsec) / 1000000;
            _t0 = _t1;
            /* Rate-limit this diagnostic: with many configured-but-idle bands
             * it flooded journald at ~20 lines/s and caused periodic CPU/I/O
             * jitter (synchronous waterfall lags + audio stutter). */
            static long _last_note = 0;
            if (_ms > 30 && _t1.tv_sec - _last_note >= 1) {
                _last_note = _t1.tv_sec;
                fprintf(stderr, "[dbg] FIFO %s read blocked %.0f ms\n", band->name, (double)_ms);
            }
        }
        if (n <= 0) {
            if (n < 0 && errno != EINTR)
                perror("read fifo");
            continue;
        }

        int ns = (int)(n / sizeof(int16_t));
        int off = 0;

#if AUDIO_USE_FFT
        /* Band-wide audio FFT feed: one shared ring window + FFT, all audio
         * clients demodulated from its spectrum (audio_fft.c). Only when a
         * client is actually listening — otherwise it is idle CPU. */
        if (band->af_ready && band_has_audio_client(band))
            audio_fft_push_iq(band, buffer, ns / 2);
#else
        /* Feed continuous IQ to any audio clients on this band (they need
         * every sample for tuning/decimation, unlike FFT frames). audio_process_iq
         * counts IQ PAIRS (it indexes iq[2*s], iq[2*s+1]), but `ns` above is the
         * int16 count — pass ns/2 or it processes the chunk twice and reads past
         * the buffer, injecting a periodic buzz at the FFT-frame rate. */
        pthread_mutex_lock(&band->lock);
        for (int ai = 0; ai < MAX_CLIENTS; ai++) {
            struct client *ac = band->clients[ai];
            if (ac && ac->audio_stream && ac->audio_active && ac->audio.fir)
                audio_process_iq(ac, buffer, ns / 2);
        }
        pthread_mutex_unlock(&band->lock);
#endif

        while (off < ns) {
            /* top up the current frame from this FIFO chunk */
            size_t space = (size_t)FFT_SIZE * 2 - (size_t)frame_pos;
            size_t take = (size_t)(ns - off);
            if (take > space) take = space;
            memcpy((char *)frame + (size_t)frame_pos * sizeof(int16_t),
                   (char *)buffer + (size_t)off * sizeof(int16_t), take * sizeof(int16_t));
            frame_pos += (int)take;
            off += (int)take;

            if (frame_pos < FFT_SIZE * 2)
                break;                 /* not enough IQ for a complete frame yet */

            pthread_mutex_lock(&band->lock);
            int ready = 0;
            if (band->nclients > 0) {
                waterfall_process(band, frame, FFT_SIZE * 2);
                /* Average LINEAR POWER over the last emit_every frames, then put
                 * the average back into power_hi so band_send_waterfall (server.c)
                 * serves the AVERAGED spectrum, exactly like the original websdr64
                 * (short FFT frames + power accumulation per ~85 ms row). A tone
                 * transition (FT8 symbol change, CW keying) lands in only one of
                 * the averaged frames instead of smearing across a single long
                 * window — crisp signal edges, no "wide band of spurs". Averaging
                 * post-contrast brightness (uint8) instead would dim one-frame
                 * peaks and blur edges (the yesterday attempt). */
                for (int i = 0; i < FFT_SIZE; i++)
                    band->wf_hi[i] += band->power_hi[i];
                acc_blocks++;
                if (acc_blocks >= emit_every) {
                    for (int i = 0; i < FFT_SIZE; i++)
                        band->power_hi[i] = band->wf_hi[i] / (float)acc_blocks;
                    memset(band->wf_hi, 0, (size_t)FFT_SIZE * sizeof(float));
                    acc_blocks = 0;
                    ready = 1;
                }
            }
            pthread_mutex_unlock(&band->lock);

            /* Non-overlapping frames: each new frame is a fresh full window
             * (FFT_SIZE pairs), giving the reference websdr64 cadence
             * 384000/32768 = 11.7 rows/s. Overlapping half the window doubled
             * the scroll speed (23 rows/s). */
            frame_pos = 0;

            if (ready) {
                /* Temporal median-of-3 across consecutive emitted rows: a
                 * single-frame transient (strong-signal keying/modulation click)
                 * is an isolated row spike and is fully suppressed; continuous
                 * signals (present in every row) pass through unchanged. Adds
                 * one row (~85 ms) of latency. */
                if (wf_m_seen < 2) {
                    for (int i = 0; i < WATERFALL_WIDTH; i++) {
                        wf_m_prev[i] = wf_m_cur[i];
                        wf_m_cur[i] = band->waterfall_line[i];
                    }
                    wf_m_seen++;
                } else {
                    for (int i = 0; i < WATERFALL_WIDTH; i++) {
                        uint8_t a = wf_m_prev[i], b = wf_m_cur[i], c = band->waterfall_line[i];
                        uint8_t mx = a > b ? (a > c ? a : c) : (b > c ? b : c);
                        uint8_t mn = a < b ? (a < c ? a : c) : (b < c ? b : c);
                        band->waterfall_line[i] = (uint8_t)(a + b + c - mx - mn);
                        wf_m_prev[i] = b;
                        wf_m_cur[i] = c;
                    }
                }
                band_send_waterfall(band);
            }
        }

        /* Audio delivery is decoupled from this read loop: the audio pacer
         * thread (audio_pacer_thread, server.c) flushes PCM to clients on a
         * steady wall-clock cadence. Here we only feed IQ into the per-client
         * demodulators; a FIFO read block on a 192 kHz band is ~170 ms of
         * audio, and flushing on that rhythm made delivery clump (voice jitter
         * / stutter on slow bands). */
    }

    free(buffer);
    free(frame);
#if AUDIO_USE_FFT
    audio_fft_band_free(band);
#endif
    close(band->fifo_fd);
    fprintf(stderr, "Band %s: stopped (running=%d)\n", band->name, band->running);
    return NULL;
}
