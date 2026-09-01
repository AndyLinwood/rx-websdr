/*
 * WebSDR Server — Audio FFT path (band-wide FFT + per-client frequency-domain
 * demodulation). Port of VertexSDR src/dsp.c, src/band.c and src/client.c
 * (client_dispatch_audio audio part), LGPL-3.0.
 *
 * Architecture (faithful to VertexSDR / original WebSDR):
 *   - one complex forward FFT per band on a sliding window of the last
 *     `fftlen` IQ samples; the window advances by fftlen/2 per block
 *     (4-phase cadence: fire at sample_count == fftlen/4 and 3*fftlen/4,
 *     reset at fftlen) and each block emits `audiolen`=128 audio samples
 *     per client -> 2*128*sr/fftlen = 8000 Hz.
 *   - per client: select the 2*half bins around the tuned frequency, apply
 *     the WebSDR passband table product (af_fbuf, built from filter_table.h),
 *     inverse-FFT back to time, demodulate (SSB = single-sideband pick,
 *     AM = envelope minus adaptive DC, FM = phase discriminator), AGC and
 *     push int16 into the shared audio.pcm buffer, which the existing
 *     pacer/codebook flush (audio_flush_pcm) keeps sending unchanged.
 *
 * Compiled only when AUDIO_USE_FFT is 1 in websdr.h; otherwise this file
 * compiles to nothing and the legacy per-client FIR path stays in use.
 */

#include "websdr.h"   /* must precede the #if below: it defines AUDIO_USE_FFT */

#if AUDIO_USE_FFT

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fftw3.h>

#include "filter_table.h"

#define AF_HALF 128          /* demod IFFT half width -> 8k audio */
#define AF_AUDIOLEN 128

/* fftlen such that 2*AF_AUDIOLEN*sr/fftlen == 8000  =>  fftlen = sr*32/1000 */
static int af_fftlen_for_sr(int sr)
{
    int n = (int)(((long long)sr * 32) + 500) / 1000;
    if (n < 4) n = 4;
    return n & ~1;
}

/* ---------- per-band backbone ---------- */

int audio_fft_band_init(struct band *b)
{
    int n = af_fftlen_for_sr(b->samplerate);

    b->af_fftlen = n;
    b->af_in  = fftwf_malloc(sizeof(fftwf_complex) * (size_t)n);
    b->af_out = fftwf_malloc(sizeof(fftwf_complex) * (size_t)n);
    b->af_spec = fftwf_malloc(sizeof(float) * (size_t)(2 * n));
    if (!b->af_in || !b->af_out || !b->af_spec) {
        audio_fft_band_free(b);
        return -1;
    }
    memset(b->af_in, 0, sizeof(fftwf_complex) * (size_t)n);
    memset(b->af_out, 0, sizeof(fftwf_complex) * (size_t)n);

    b->af_plan = fftwf_plan_dft_1d(n, b->af_in, b->af_out,
                                   FFTW_FORWARD, FFTW_ESTIMATE);
    if (!b->af_plan) {
        audio_fft_band_free(b);
        return -1;
    }

    b->af_sample_count = 0;
    b->af_half_fftlen = n / 4;
    b->af_ready = 1;
    b->af_in_samples = 0;
    b->af_measured_sr = 0;
    clock_gettime(CLOCK_MONOTONIC, &b->af_clock_t0);
    fprintf(stderr, "Band %s: audio FFT ready (fftlen=%d sr=%d rate=%d)\n",
            b->name, n, b->samplerate, 2 * AF_AUDIOLEN * b->samplerate / n);
    return 0;
}

void audio_fft_band_free(struct band *b)
{
    if (b->af_plan) { fftwf_destroy_plan(b->af_plan); b->af_plan = NULL; }
    if (b->af_in)  { fftwf_free(b->af_in);  b->af_in  = NULL; }
    if (b->af_out) { fftwf_free(b->af_out); b->af_out = NULL; }
    if (b->af_spec){ fftwf_free(b->af_spec); b->af_spec = NULL; }
    b->af_ready = 0;
}

static void audio_fft_fire(struct band *b);

/* Called from band_thread with each FIFO chunk (no band lock held). */
void audio_fft_push_iq(struct band *b, const int16_t *iq, int nsamples)
{
    b->af_in_samples += nsamples;
    for (int i = 0; i < nsamples; i++) {
        int sc = b->af_sample_count;
        b->af_in[sc][0] = (float)iq[2 * i];
        b->af_in[sc][1] = (float)iq[2 * i + 1];
        b->af_sample_count = sc + 1;
        if (b->af_sample_count == b->af_half_fftlen)
            audio_fft_fire(b);
    }

    /* Port of VertexSDR stdin_get_measured_sps: the FIFO feed rate is measured
     * from wall-clock, not trusted from config. The audio rate the client is
     * told (0x81) must reflect the REAL rate or its drift corrector (±0.2%)
     * saturates and the 768 ms buffer drains -> periodic dropouts = trembling.
     * Window ~2 s (the reference updates every >2s of wall-clock). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_us = (now.tv_sec - b->af_clock_t0.tv_sec) * 1000000L
                    + (now.tv_nsec - b->af_clock_t0.tv_nsec) / 1000L;
    if (elapsed_us > 2000000L) {
        long long sps = (b->af_in_samples * 1000000LL) / elapsed_us;
        if (sps > 100000 && sps < 10000000) {   /* sane bound (Vertex: 1M..50M) */
            b->af_measured_sr = (int)sps;
            fprintf(stderr, "Band %s: measured feed sr=%d (cfg %d)\n",
                    b->name, b->af_measured_sr, b->samplerate);
        }
        b->af_in_samples = 0;
        b->af_clock_t0 = now;
    }
}

static void audio_fft_spectrum(struct band *b, int phase);

/* VertexSDR dsp_process_fft_block cadence, verbatim. The input window is a
 * sliding last-N buffer (write cursor cycles 0..N-1), so consecutive blocks
 * overlap by 3N/4 and phase_flag alternates to keep the IFFT sub-blocks
 * aligned. */
static void audio_fft_fire(struct band *b)
{
    int cur = b->af_half_fftlen;
    int n = b->af_fftlen;

    if (cur == n) {
        b->af_sample_count = 0;
        b->af_half_fftlen = n / 4;
        return;
    }
    if (cur == n / 4)
        b->af_half_fftlen = 3 * n / 4;
    else
        b->af_half_fftlen = n;

    int phase = (b->af_sample_count != n / 4) ? 1 : 0;
    audio_fft_spectrum(b, phase);
}

static void audio_fft_client_block(struct band *b, struct client *cli, int phase);

static void audio_fft_spectrum(struct band *b, int phase)
{
    int n = b->af_fftlen;
    int half = n / 2;

    fftwf_execute(b->af_plan);            /* af_in -> af_out */

    /* Quadrant half-swap so bin k of af_spec == frequency (k - half) bins:
     * index k of the demod code maps directly to signed offset from centre. */
    memcpy(b->af_spec,
           (float *)b->af_out + (size_t)half * 2, (size_t)half * 2 * sizeof(float));
    memcpy(b->af_spec + (size_t)half * 2,
           (float *)b->af_out, (size_t)half * 2 * sizeof(float));

    pthread_mutex_lock(&b->lock);
    for (int ai = 0; ai < MAX_CLIENTS; ai++) {
        struct client *ac = b->clients[ai];
        if (ac && ac->audio_stream && ac->audio_active && ac->audio.af_dplan)
            audio_fft_client_block(b, ac, phase);
    }
    pthread_mutex_unlock(&b->lock);
}

/* ---------- per-client passband filter (VertexSDR compute_passband_filter) ---------- */

static void audio_fft_client_filter(struct client *cli)
{
    struct band *b = cli->band;
    struct audio_state *a = &cli->audio;
    if (!b) return;

    double fftlen_d = (double)b->af_fftlen;
    double sr_d = (double)b->samplerate;

    double lo_bin_f = fftlen_d * (double)a->af_lo / sr_d;
    int lo_bin = (lo_bin_f <= 0.0) ? (int)(lo_bin_f - 0.5) : (int)(lo_bin_f + 0.5);
    double hi_bin_f = fftlen_d * (double)a->af_hi / sr_d;
    int hi_bin = (hi_bin_f <= 0.0) ? (int)(hi_bin_f - 0.5) : (int)(hi_bin_f + 0.5);

    if (lo_bin == a->af_filter_lo_bin && hi_bin == a->af_filter_hi_bin)
        return;

    int hi_table_base = 5122 - lo_bin;
    int lo_table_limit = 1024 - hi_bin;
    if (lo_bin > hi_bin || hi_table_base > 6143 || lo_table_limit < 0) {
        for (int i = 0; i < 1024; i++)
            a->af_fbuf[i] = 1.0f;
        a->af_filter_lo_bin = lo_bin;
        a->af_filter_hi_bin = hi_bin;
        a->af_conv = 2;   /* neutral raw filter */
        return;
    }

    int filter_half = a->af_half_size;
    if (hi_bin >= filter_half - 1)
        hi_bin = filter_half - 2;
    int lo_edge = 2 - filter_half;
    if (lo_bin >= lo_edge)
        lo_edge = lo_bin;

    int hi_idx_base = 4098 - lo_edge;
    for (int i = 0; i < 1024; i++) {
        int hi_idx = hi_idx_base + i;
        int lo_idx = lo_edge - 3076 - hi_bin + hi_idx_base + i;
        float fh = (hi_idx >= 0 && hi_idx < 6144) ? filter_table[hi_idx] : 0.0f;
        float fl = (lo_idx >= 0 && lo_idx < 6144) ? filter_table[lo_idx] : 0.0f;
        a->af_fbuf[i] = fh * fl;
    }
    a->af_filter_lo_bin = lo_edge;
    a->af_filter_hi_bin = hi_bin;

    /* conv_type for the adaptive codec (VertexSDR compute_passband_filter).
     * bit 0x10 = SSB sideband selection -> decoder adpcm_shift 12, else 14. */
    {
        double max_abs_khz = fmax(fabs((double)a->af_lo), fabs((double)a->af_hi)) / 1000.0;
        int conv_mode = 2;
        if (max_abs_khz >= 1.0) conv_mode = (max_abs_khz < 2.7) ? 1 : 0;
        if (a->af_rate > 9000) conv_mode = 3;
        if (hi_bin * lo_edge > 0 && a->mode != 1 && a->mode != 4)
            conv_mode |= 0x10;
        a->af_conv = conv_mode;
    }
}

/* ---------- per-client setup (called from audio_reconfigure, audio_mutex held) ---------- */

void audio_fft_client_setup(struct client *cli)
{
    struct band *b = cli->band;
    struct audio_state *a = &cli->audio;
    if (!b || b->af_fftlen <= 0) return;   /* band FFT not initialized yet */

    int am = (a->mode == 1 || a->mode == 4) ? 1 : 0;
    a->af_half_size = AF_HALF;
    a->af_audiolen = AF_AUDIOLEN;
    /* NOTE: af_rate is NOT recomputed here on a plain retune — see below. The
     * measured feed clock "breathes" by up to +-1% between 2 s windows, so
     * recomputing af_rate on every param (every mouse move while retuning)
     * would re-emit 0x81 with a jumping rate (e.g. 7920..8071) on each
     * retune, making the client's resampler/drift corrector chase a noisy
     * target -> dropouts / silence during fast retuning. af_rate is fixed at
     * band switch / mode switch time and left alone while retuning. */
    a->af_freq = cli->freq;
    a->af_lo = cli->lo_filter;
    a->af_hi = cli->hi_filter;
    a->af_filter_dirty = 1;

    int len = 2 * a->af_half_size;
    int had_plan = (a->af_dplan != NULL);
    if (a->af_dplan_len == len && a->af_dplan && a->af_am_plan == am) {
        /* Plan is reusable (same size/mode). Distinguish a BAND SWITCH from a
         * plain RETUNE within the same band:
         *  - retune (af_buf_band == b): the demod only moves tune_bin and the
         *    passband filter; the FFT input stream is CONTINUOUS, so nothing
         *    needs resetting. Reset + 0x84 here would cut the audio on EVERY
         *    frequency drag (VFO sends a param per mouse move) = stutter /
         *    fast dropouts while retuning.
         *  - band switch (af_buf_band != b): drop the old band's buffered
         *    blocks (otherwise it keeps playing in parallel), reset the codec
         *    predictor, recompute af_rate for the new band and hand the client
         *    a silent soft handoff (0x84).
         * NOTE: do NOT reset the AGC/AM-DC/FM state even on a band switch —
         * the gain jump from its converged value to zero knocks. AGC adapts on
         * its own (fast attack). (audio_reconfigure cleared npcm/pace.) */
        if (a->af_buf_band != b) {
            a->af_on = 0;
            a->af_conv = 0;
            a->c_header_counter = 0;
            a->c_conv_sent = -1;
            a->c_last_rate = 0;
            a->pred_accum = 0;
            memset(a->pred_h, 0, sizeof(a->pred_h));
            memset(a->pred_x, 0, sizeof(a->pred_x));
            /* Soft handoff: 128 silent samples + predictor reset on the CLIENT
             * side (0x84), so the stream break is silent instead of a click.
             * Only when the client was already playing. */
            if (had_plan) {
                uint8_t sil = 0x84;
                client_enqueue(cli, &sil, 1);
            }
            /* New band: recompute the client audio rate from the measured feed
             * clock (see the NOTE at the top of this function). */
            {
                int eff_sr = (b->af_measured_sr > 0) ? b->af_measured_sr : b->samplerate;
                a->af_rate = 2 * AF_AUDIOLEN * eff_sr / b->af_fftlen;
            }
            a->af_buf_band = b;
        }
        return;
    }

    /* Plan kind depends on the demod mode (SSB = c2r, AM/FM = complex
     * backward), so a mode switch rebuilds the plan even at equal size. */
    if (a->af_dplan) fftwf_destroy_plan(a->af_dplan);
    if (a->af_din)   fftwf_free(a->af_din);
    if (a->af_dout)  fftwf_free(a->af_dout);
    if (a->af_dout_r) fftwf_free(a->af_dout_r);

    a->af_din  = fftwf_malloc(sizeof(fftwf_complex) * (size_t)len);
    a->af_dout = fftwf_malloc(sizeof(fftwf_complex) * (size_t)len);
    a->af_dout_r = fftwf_malloc(sizeof(float) * (size_t)len);
    a->af_dplan = NULL;
    a->af_dplan_len = 0;
    a->af_am_plan = 0;
    if (!a->af_din || !a->af_dout || !a->af_dout_r)
        return;
    memset(a->af_din, 0, sizeof(fftwf_complex) * (size_t)len);
    memset(a->af_dout, 0, sizeof(fftwf_complex) * (size_t)len);

    if (am)
        a->af_dplan = fftwf_plan_dft_1d(len, a->af_din, a->af_dout,
                                        FFTW_BACKWARD, FFTW_ESTIMATE);
    else
        a->af_dplan = fftwf_plan_dft_c2r_1d(len, a->af_din, a->af_dout_r,
                                            FFTW_ESTIMATE);
    if (!a->af_dplan)
        return;
    a->af_dplan_len = len;
    a->af_am_plan = am;

    /* Fresh demod state with a new plan. */
    a->af_agc_gain = 0.0f;
    a->af_am_dc = 0.0f;
    a->af_fm_prev_re = a->af_fm_prev_im = 0.0f;
    a->af_fm_prev2_re = a->af_fm_prev2_im = 0.0f;

    /* Fresh adaptive-codec state (port of VertexSDR client state). */
    a->af_on = 0;
    a->af_conv = 0;
    a->c_block_size = 256;          /* matches the 0x82 init frame / audioformat 2 */
    a->c_quant_mode = 0;
    a->c_header_counter = 0;
    a->c_conv_sent = -1;
    a->c_last_rate = 0;             /* force 0x81 on the first encoded frame */
    a->pred_accum = 0;
    memset(a->pred_h, 0, sizeof(a->pred_h));
    memset(a->pred_x, 0, sizeof(a->pred_x));

    /* Soft handoff on mode switch too (see the reuse branch above). */
    if (had_plan) {
        uint8_t sil = 0x84;
        client_enqueue(cli, &sil, 1);
    }
    /* New plan (mode switch): recompute the client audio rate too. */
    {
        int eff_sr = (b->af_measured_sr > 0) ? b->af_measured_sr : b->samplerate;
        a->af_rate = 2 * AF_AUDIOLEN * eff_sr / b->af_fftlen;
    }
    a->af_buf_band = b;

    fprintf(stderr, "[fft] client setup band=%s f=%.1f mode=%d lo=%d hi=%d "
            "half=%d rate=%d\n", b->name, cli->freq, a->mode,
            cli->lo_filter, cli->hi_filter, a->af_half_size, a->af_rate);
}

/* ---------- demod cores (VertexSDR demod_ssb_core / demod_am_core / demod_fm) ---------- */

static float *audio_fft_demod_ssb(struct band *b, struct client *cli, int phase)
{
    struct audio_state *a = &cli->audio;
    int half = a->af_half_size;
    float *spec = b->af_spec;
    fftwf_complex *din = a->af_din;

    float *fwd = spec + 2 * a->af_tune_bin + 2;
    float *bwd = spec + 2 * a->af_tune_bin - 2;
    float *filt = a->af_fbuf + 512;

    for (int k = 1; k < half; k++) {
        float f_pos = filt[k];
        float f_neg = filt[-k];
        float re = fwd[0] * f_pos + bwd[0] * f_neg;
        float im = fwd[1] * f_pos - bwd[1] * f_neg;
        din[k][0] = re;
        din[k][1] = im;
        fwd += 2;
        bwd -= 2;
    }
    din[0][0] = 0.0f; din[0][1] = 0.0f;
    din[half][0] = 0.0f; din[half][1] = 0.0f;

    fftwf_execute(a->af_dplan);              /* c2r, 2*half -> af_dout_r */

    float *out = a->af_dout_r + (phase ? 0 : half);
    double sum_sq = 0.0;
    for (int i = 0; i < half; i++)
        sum_sq += (double)(out[i] * out[i]);
    a->af_agc_peak = (float)sqrt(sum_sq * (128.0 / (double)half));
    return out;
}

static float *audio_fft_demod_am(struct band *b, struct client *cli, int phase)
{
    struct audio_state *a = &cli->audio;
    int half = a->af_half_size;
    int N = b->af_fftlen;
    int mask = 2 * half - 1;
    float *spec = b->af_spec;
    fftwf_complex *din = a->af_din;

    int first_bin = 1 - half;
    for (int k = first_bin; k < half; k++) {
        int dst = k & mask;
        int src_bin = k + a->af_tune_bin;
        if (src_bin >= 0 && src_bin < N) {
            float *sp = spec + 2 * src_bin;
            float fw = a->af_fbuf[512 + k];
            din[dst][0] = sp[0] * fw;
            din[dst][1] = sp[1] * fw;
        } else {
            din[dst][0] = 0.0f;
            din[dst][1] = 0.0f;
        }
    }

    fftwf_execute(a->af_dplan);              /* complex backward -> af_dout */

    fftwf_complex *out_cx = a->af_dout;
    int cx_offset = phase ? 0 : half;

    float dc = a->af_am_dc;
    float max_env = 0.0f;
    for (int i = 0; i < half; i++) {
        float re = out_cx[cx_offset + i][0];
        float im = out_cx[cx_offset + i][1];
        float env = sqrtf(re * re + im * im);
        float diff = env - dc;
        dc += diff * 0.01f;
        float sample = env - dc;
        a->af_dout_r[i] = sample;
        if (sample > max_env) max_env = sample;
    }
    a->af_am_dc = dc;
    a->af_agc_peak = max_env * 2.0f;
    return a->af_dout_r;
}

static float *audio_fft_demod_fm(struct band *b, struct client *cli, int phase)
{
    struct audio_state *a = &cli->audio;
    int half = a->af_half_size;
    int N = b->af_fftlen;
    int mask = 2 * half - 1;
    float *spec = b->af_spec;
    fftwf_complex *din = a->af_din;

    int first_bin = 1 - half;
    for (int k = first_bin; k < half; k++) {
        int dst = k & mask;
        int src_bin = k + a->af_tune_bin;
        if (src_bin >= 0 && src_bin < N) {
            float *sp = spec + 2 * src_bin;
            float fw = a->af_fbuf[512 + k];
            din[dst][0] = sp[0] * fw;
            din[dst][1] = sp[1] * fw;
        } else {
            din[dst][0] = 0.0f;
            din[dst][1] = 0.0f;
        }
    }

    fftwf_execute(a->af_dplan);

    fftwf_complex *out_cx = a->af_dout;
    int cx_offset = phase ? 0 : half;

    float p2_re = a->af_fm_prev2_re, p2_im = a->af_fm_prev2_im;
    float p_re  = a->af_fm_prev_re,  p_im  = a->af_fm_prev_im;

    for (int j = 0; j < half; j++) {
        float re = out_cx[cx_offset + j][0];
        float im = out_cx[cx_offset + j][1];
        float mag = sqrtf(re * re + im * im + 1e-30f);
        float i_n = re / mag;
        float q_n = im / mag;

        float d = (p2_re - i_n) * p_im - (p2_im - q_n) * p_re;
        if (d > 0.99f) d = 0.99f;
        if (d < -0.99f) d = -0.99f;
        a->af_dout_r[j] = d;

        p2_re = p_re; p2_im = p_im;
        p_re = i_n; p_im = q_n;
    }

    a->af_fm_prev2_re = p2_re; a->af_fm_prev2_im = p2_im;
    a->af_fm_prev_re  = p_re;  a->af_fm_prev_im  = p_im;

    a->af_agc_peak = 2.0f;
    return a->af_dout_r;
}

/* ---------- per-client block: tune -> filter -> demod -> AGC -> pcm ---------- */

static void audio_fft_client_block(struct band *b, struct client *cli, int phase)
{
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;
    if (!a->af_dplan) {
        /* A param message may have arrived before the band FFT was up;
         * retry the setup now that the backbone exists. */
        if (b->af_fftlen > 0)
            audio_fft_client_setup(cli);
        if (!a->af_dplan) {
            pthread_mutex_unlock(&cli->audio_mutex);
            return;
        }
    }

    /* VertexSDR dsp_dispatch_clients tune_bin mapping (band centre = N/2). */
    {
        double tune_offset_norm = (a->af_freq - band_eff_center(b))
                                / ((double)b->samplerate / 1000.0);
        double tn = 0.0;
        if (tune_offset_norm >= -0.5)
            tn = (tune_offset_norm <= 0.5) ? (tune_offset_norm + 0.5) : 1.0;
        int tb = (int)((double)b->af_fftlen * tn + 0.5);
        if (tb < a->af_half_size) tb = a->af_half_size;
        if (tb >= b->af_fftlen - a->af_half_size) tb = b->af_fftlen - a->af_half_size - 1;
        a->af_tune_bin = tb;
    }

    if (a->af_filter_dirty) {
        audio_fft_client_filter(cli);
        a->af_filter_dirty = 0;
    }

    float *out;
    if (a->mode == 0)
        out = audio_fft_demod_ssb(b, cli, phase);
    else if (a->mode == 1)
        out = audio_fft_demod_am(b, cli, phase);
    else
        out = audio_fft_demod_fm(b, cli, phase);
    int out_samples = a->af_audiolen;

    /* AGC: 1/sqrt_power, fast attack / slow release (VertexSDR). */
    float new_gain = (float)(1.0 / (a->af_agc_peak + 1e-30));
    float old_gain = a->af_agc_gain;
    a->af_agc_gain = new_gain;
    if (new_gain > old_gain) {
        new_gain = new_gain * 0.01f + old_gain * 0.99f;
        a->af_agc_gain = new_gain;
    }
    float scale = new_gain * 32000.0f;

#if AUDIO_USE_CODEC
    /* Adaptive codec path: buffer PRE-SCALED float samples into af_obuf (a
     * contiguous queue consumed one block at a time by the pacer/acdc path).
     * Scale (AGC) is baked in here so the whole buffered stream is homogeneous;
     * audio_flush_pcm then encodes blocks with scale=1.0. The codec predictor
     * needs the full contiguous 8000/s stream — pacing only delays frame
     * EMISSION, never drops samples, so continuity is preserved. */
    if (a->af_on + out_samples <= AUDIO_BUFSZ) {
        for (int s = 0; s < out_samples; s++)
            a->af_obuf[a->af_on + s] = out[s] * scale;
        a->af_on += out_samples;
    }
#else
    /* int16 PCM into the shared buffer, drained by audio_flush_pcm (codebook
     * 0x80 path). */
    for (int s = 0; s < out_samples && a->npcm < AUDIO_BUFSZ; s++) {
        int32_t outv = (int32_t)(out[s] * scale + 32768.5) - 32768;
        if (outv < -32768) outv = -32768;
        if (outv > 32767) outv = 32767;
        a->pcm[a->npcm++] = (short)outv;
    }
#endif
    pthread_mutex_unlock(&cli->audio_mutex);
}

#endif /* AUDIO_USE_FFT */
