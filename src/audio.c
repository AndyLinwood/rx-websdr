/*
 * WebSDR Server — Audio stream (/~~stream?v=11)
 *
 * Per-client audio demodulator: NCO → FIR bandpass → decimation →
 * envelope detection → peak-hold AGC → codebook-0x80 encode.
 *
 * Original working approach — restored from pre-PhantomSDR code.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libwebsockets.h>

#include "websdr.h"
#include "audio_codebook.h"

#define AUDIO_RATE 8000.0
#define CB_BLOCK 128

#if !AUDIO_USE_FFT   /* legacy time-domain FIR demod helpers */
static double sinc(double x) {
    if (x == 0.0) return 1.0;
    return sin(M_PI * x) / (M_PI * x);
}

static double *design_bandpass(double low, double high, int N) {
    double *h = malloc((size_t)N * sizeof(double));
    int M = (N - 1) / 2;
    for (int i = 0; i < N; i++) {
        int n = i - M;
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * i / (N - 1));
        h[i] = (2.0 * high * sinc(2.0 * high * n)
               - 2.0 * low * sinc(2.0 * low * n)) * w;
    }
    return h;
}

static double fir_dot(const double *delay, const double *h, int len, int pos) {
    int k = (pos - 1 + len) % len;
    double acc = 0.0;
    for (int i = 0; i < len; i++) {
        acc += delay[k] * h[i];
        k = (k - 1 + len) % len;
    }
    return acc;
}
#endif /* !AUDIO_USE_FFT */

void audio_init(struct client *cli, int samplerate) {
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;
#if AUDIO_USE_FFT
    /* FFT path (audio_fft.c): never memset the whole state here — protocol.c
     * calls audio_init on every param message (its guard is !audio.fir, which
     * never gets set on this path), and a memset would leak/drop the per-client
     * IFFT plan. CRITICAL: do NOT reset pace_budget/pace_init here — with a
     * fast retune (VFO sends a param per mouse move) the pacer budget would be
     * zeroed dozens of times per second, so it can never earn the 128 samples
     * to emit a block; the FFT keeps producing, af_obuf fills up, and the
     * overflow guard starts sending 0x84 resets -> audio cuts out / goes
     * silent while retuning. Pace state is only reset on a real band switch
     * (audio_reconfigure). */
    a->npcm = 0;
    a->s_ema = 0;
    pthread_mutex_unlock(&cli->audio_mutex);
    return;
#else
    memset(a, 0, sizeof(*a));
    a->decim = (int)round(samplerate / AUDIO_RATE);
    if (a->decim < 1) a->decim = 1;
    a->lo_hz = 300.0;
    a->hi_hz = 2700.0;
    pthread_mutex_unlock(&cli->audio_mutex);
#endif
}

void audio_free(struct client *cli) {
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;
#if AUDIO_USE_FFT
    if (a->af_dplan) { fftwf_destroy_plan(a->af_dplan); }
    if (a->af_din)   fftwf_free(a->af_din);
    if (a->af_dout)  fftwf_free(a->af_dout);
    if (a->af_dout_r) fftwf_free(a->af_dout_r);
#else
    free(a->fir); free(a->fir_delay_i); free(a->fir_delay_q);
    free(a->fir2); free(a->fir2_delay_i); free(a->fir2_delay_q);
#endif
    memset(a, 0, sizeof(*a));
    pthread_mutex_unlock(&cli->audio_mutex);
}

void audio_reconfigure(struct client *cli, int samplerate,
                       double centerfreq_khz) {
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;
#if AUDIO_USE_FFT
    /* Band-wide FFT path (audio_fft.c): no per-client FIR chain, the client
     * demodulates from the shared band spectrum on each FFT block. */
    a->mode = cli->mode;
    a->npcm = 0;                 /* drop PCM from the previous tuning */
    /* Reset pacing only on a real BAND SWITCH (cli->band changed); on a plain
     * retune the FFT stream is continuous, and zeroing pace_budget on every
     * param (dozens per second while dragging the VFO) starves the pacer ->
     * af_obuf fills -> overflow guard resets -> audio cuts out. (See also the
     * audio_init note.) */
    if (cli->band != a->af_buf_band) {
        a->pace_budget = 0;
        a->pace_init = 0;
    }
    audio_fft_client_setup(cli);
    fprintf(stderr, "[reconf] FFT path sr=%d rate=%d\n", samplerate, a->af_rate);
    pthread_mutex_unlock(&cli->audio_mutex);
    return;
#else
    double foff_hz = (cli->freq - centerfreq_khz) * 1000.0;
    a->nco_inc = 2.0 * M_PI * foff_hz / samplerate;
    a->mode = cli->mode;

    /* Passband edges (already Hz, protocol did *1000).
     * SSB/CW (mode 0): narrow audio band, |lo|..|hi| sorted.
     * AM/FM (mode 1/4): wide single-sided band 50..max(|lo|,|hi|) so the whole
     *   sideband fits (client sends lo=-4.96,hi=4.95 kHz for AM). */
    double lo, hi;
    double alo = fabs((double)cli->lo_filter);
    double ahi = fabs((double)cli->hi_filter);
    if (a->mode == 0) {
        lo = alo; hi = ahi;
        if (lo > hi) { double t = lo; lo = hi; hi = t; }
        if (lo < 50.0) lo = 50.0;
        if (hi > 4000.0) hi = 4000.0;
        if (hi - lo < 100.0) hi = lo + 100.0;
    } else {
        hi = (alo > ahi) ? alo : ahi;
        if (hi > 4000.0) hi = 4000.0;
        if (hi < 100.0) hi = 4000.0;
        lo = 50.0;
    }
    a->lo_hz = lo;
    a->hi_hz = hi;

    /* Anti-alias lowpass at samplerate, cutoff at 4kHz */
    double f_aa = (AUDIO_RATE / 2.0) / samplerate;
    free(a->fir);
    int n1 = 8 * a->decim + 1;
    if (n1 < 63) n1 = 63;
    if (n1 > 2047) n1 = 2047;
    if (n1 % 2 == 0) n1++;
    a->fir = design_bandpass(0.0, f_aa, n1);
    a->fir_len = n1;
    a->fir_pos = 0;
    free(a->fir_delay_i); free(a->fir_delay_q);
    a->fir_delay_i = calloc((size_t)n1, sizeof(double));
    a->fir_delay_q = calloc((size_t)n1, sizeof(double));

    /* Bandpass at 8kHz */
    double fbp_lo = lo / AUDIO_RATE;
    double fbp_hi = hi / AUDIO_RATE;
    int n2 = 511;
    if (n2 % 2 == 0) n2++;
    free(a->fir2);
    a->fir2 = design_bandpass(fbp_lo, fbp_hi, n2);
    a->fir2_len = n2;
    a->fir2_pos = 0;
    free(a->fir2_delay_i); free(a->fir2_delay_q);
    a->fir2_delay_i = calloc((size_t)n2, sizeof(double));
    a->fir2_delay_q = calloc((size_t)n2, sizeof(double));

    a->decim_count = 0;
    a->dec2_count = 0;
    a->peak = 0.0;
    a->npcm = 0;
    a->dc_sum = 0; a->dc_count = 0; a->dc_avg = 0;
    a->rate_sent = 0;
    fprintf(stderr, "[reconf] sr=%d decim=%d bp=[%.0f..%.0f]\n",
            samplerate, a->decim, lo, hi);
    pthread_mutex_unlock(&cli->audio_mutex);
#endif
}

void audio_process_iq(struct client *cli, const int16_t *iq, int nsamples) {
#if AUDIO_USE_FFT
    /* The FFT path demodulates from the shared band spectrum in audio_fft.c;
     * per-sample IQ feed is not used. */
    (void)cli; (void)iq; (void)nsamples;
    return;
#else
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;
    if (!a->fir || !a->fir2) return;

    for (int s = 0; s < nsamples; s++) {
        double I = iq[2*s], Q = iq[2*s+1];
        double c = cos(a->nco_phase), sn = sin(a->nco_phase);
        double mi = I*c + Q*sn, mq = -I*sn + Q*c;
        a->nco_phase += a->nco_inc;
        if (a->nco_phase >  2*M_PI) a->nco_phase -= 2*M_PI;
        if (a->nco_phase < -2*M_PI) a->nco_phase += 2*M_PI;

        a->fir_delay_i[a->fir_pos] = mi;
        a->fir_delay_q[a->fir_pos] = mq;
        a->fir_pos = (a->fir_pos + 1) % a->fir_len;
        if (++a->decim_count < a->decim) continue;
        a->decim_count = 0;

        double fi = fir_dot(a->fir_delay_i, a->fir, a->fir_len, a->fir_pos);
        double fq = fir_dot(a->fir_delay_q, a->fir, a->fir_len, a->fir_pos);

        a->fir2_delay_i[a->fir2_pos] = fi;
        a->fir2_delay_q[a->fir2_pos] = fq;
        a->fir2_pos = (a->fir2_pos + 1) % a->fir2_len;

        double bi = fir_dot(a->fir2_delay_i, a->fir2, a->fir2_len, a->fir2_pos);
        double bq = fir_dot(a->fir2_delay_q, a->fir2, a->fir2_len, a->fir2_pos);

        static long dbg_ctr = 0;
        if ((dbg_ctr++ % 40000) == 0)
            fprintf(stderr, "[dsp] I=%d Q=%d nco_ph=%.2f fi=%.0f fq=%.0f bi=%.0f bq=%.0f\n",
                    (int)I, (int)Q, a->nco_phase, fi, fq, bi, bq);

        /* Demodulate according to mode (proven model, [11 авг]):
         *   SSB/CW (mode 0) -> I channel (real audio)
         *   AM (mode 1)     -> envelope sqrt(I^2+Q^2)
         *   FM (mode 4)     -> phase derivative */
        double audio;
        if (a->mode == 0) {
            audio = bi;                      /* I channel */
        } else if (a->mode == 1) {
            audio = sqrt(bi*bi + bq*bq);     /* AM envelope */
        } else {
            double ph = atan2(bq, bi);
            double dph = ph - a->last_phase;
            if (dph >  M_PI) dph -= 2*M_PI;
            if (dph < -M_PI) dph += 2*M_PI;
            a->last_phase = ph;
            audio = dph;                     /* FM discriminator */
        }

        double av = fabs(audio);
        a->peak = (av > a->peak) ? av : a->peak * 0.9993;
        if (a->peak < 10.0) a->peak = 10.0;
        double gain = 14000.0 / a->peak;
        if (gain > 1400.0) gain = 1400.0;
        double sample = audio * gain;

        int32_t out = (int32_t)(sample + 32768.5) - 32768;
        if (out < -32768) out = -32768;
        if (out >  32767) out =  32767;
        if (a->npcm < AUDIO_BUFSZ)
            a->pcm[a->npcm++] = (short)out;
    }
    pthread_mutex_unlock(&cli->audio_mutex);
#endif
}

/* Noise-anchored S-meter, consistent with the waterfall (wf_brightness):
 * the band's tracked noise floor (noise_dB, the median of power_hi in dB)
 * is subtracted, so the receiver's band-dependent input gain/attenuation
 * cancels and the noise floor reads the same S level on every band.
 * SMETER_NOISE_DBM pins the noise floor at ~S2-S3 (raw≈180, -109 dBm on the
 * client's S scale S9=-73 dBm, 6 dB/S); signals sit above it. The config
 * per-band `gain` stays a waterfall colour-scale control and is deliberately
 * NOT applied here (with 30m/160m negative gains it would push the meter off
 * the bottom). A fixed absolute offset like the old -150 only matched 40m —
 * on 80m (strong input) the noise floor read S9+. */
#define SMETER_NOISE_DBM -109.0
int audio_compute_smeter(struct client *cli) {
    struct band *b = cli->band;
    if (!b) return 0;
    int lo = cli->lo_filter, hi = cli->hi_filter;
    if (lo > hi) { int t = lo; lo = hi; hi = t; }
    double fs = b->samplerate, f0 = cli->freq*1000.0, c0 = band_eff_center(b)*1000.0;
    int lo_bin = (int)lround(FFT_SIZE/2.0 + (f0+lo-c0)*FFT_SIZE/fs);
    int hi_bin = (int)lround(FFT_SIZE/2.0 + (f0+hi-c0)*FFT_SIZE/fs);
    if (lo_bin < 0) lo_bin = 0;
    if (hi_bin >= FFT_SIZE) hi_bin = FFT_SIZE-1;
    if (hi_bin <= lo_bin) hi_bin = lo_bin+1;
    if (hi_bin >= FFT_SIZE) hi_bin = FFT_SIZE-1;
    double sum = 0;
    for (int i = lo_bin; i <= hi_bin; i++) sum += b->power_hi[i];
    double dbm = 20*log10(sum/(hi_bin-lo_bin+1)) - b->noise_dB + SMETER_NOISE_DBM;
    double raw = (dbm+127)*10;
    if (raw < 0) raw = 0;
    if (raw > 4095) raw = 4095;
    return (int)lround(raw);
}

static int cb_index(short v) {
    int best = 0, bd = 1<<30;
    for (int i = 0; i < 256; i++) {
        int d = (int)AUDIO_CODEBOOK[i] - (int)v;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

void audio_flush_pcm(struct client *cli) {
#if AUDIO_USE_FFT && AUDIO_USE_CODEC
    /* Codec mode frames are produced by the FFT cadence into a buffered queue
     * (af_obuf) and encoded by audio_codec_send one af_audiolen block at a
     * time. The band thread fills af_obuf in bursts (one large FIFO read per
     * iter ~64 ms), so dump the queue each tick here — the SAME wall-clock
     * pacing that fixed codebook-0x80 jitter. We never drop a whole block
     * (predictor needs contiguity); we only delay emission to match 8000/s. */
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;

    if (cli->muted) {
        client_free_outq(cli);
        uint8_t sil = 0x84;
        client_enqueue(cli, &sil, 1);
        a->af_on = 0;
        /* 0x84 resets the CLIENT codec decoder predictor, so the server's
         * codec ENCODER predictor must be zeroed too. Leaving it frozen makes
         * encoder/decoder predictor state diverge on unmute: the decoder then
         * reconstructs raw+(pred_dec-pred_enc) = full-scale noise that never
         * converges (the residual never trips the 0x80 overflow fallback) —
         * loud hiss until the client reloads. Mirrors the band/mode-switch
         * soft handoff reset in audio_fft.c. */
        memset(a->pred_h, 0, sizeof(a->pred_h));
        memset(a->pred_x, 0, sizeof(a->pred_x));
        a->pred_accum = 0;
        pthread_mutex_unlock(&cli->audio_mutex);
        return;
    }

    /* Steady-rate pacing: earn budget by wall-clock at AUDIO_RATE. */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!a->pace_init) {
            a->pace_last = now;
            a->pace_init = 1;
        }
        double dt = (double)(now.tv_sec - a->pace_last.tv_sec)
                  + (double)(now.tv_nsec - a->pace_last.tv_nsec) / 1e9;
        a->pace_last = now;
        a->pace_budget += dt * AUDIO_RATE;
        /* Cap the pacing backlog at 50 ms (was 100 ms): bounds the server-side
         * audio delay; 4 blocks of headroom is still enough to spread FIFO
         * clumps evenly. */
        if (a->pace_budget > AUDIO_RATE * 0.05) a->pace_budget = AUDIO_RATE * 0.05;
    }

    /* Overflow guard: if the pacer fell behind and the queue is full, the
     * predictor can no longer be kept contiguous — force a client-side reset via
     * a 0x84 (128 silent samples + predictor clear) and drop the backlog. In
     * steady state production == consumption (~8000/s) so this never fires. */
    if (a->af_on >= AUDIO_BUFSZ) {
        memset(a->pred_h, 0, sizeof(a->pred_h));
        memset(a->pred_x, 0, sizeof(a->pred_x));
        a->pred_accum = 0;
        uint8_t sil = 0x84;
        client_enqueue(cli, &sil, 1);
        a->af_on = 0;
        a->pace_budget = 0;
    }

    /* Emit whole blocks while wall-clock has earned the samples. At most ONE
     * block per 16 ms pacer tick (128 samples == exactly 8000/s): radiod writes
     * the FIFO in 40-60 ms clumps, so the FFT produces blocks in bursts. If we
     * dumped the whole earned backlog at once, the client would receive a clump
     * then a gap, and its drift corrector (±0.2%) would wobble the pitch on
     * every burst. One block per tick stretches a burst evenly over time. */
    int emitted = 0;
    while (emitted < 1 && a->pace_budget >= a->af_audiolen
           && a->af_on >= a->af_audiolen) {
        audio_codec_send(cli, 1.0f, audio_compute_smeter(cli));
        a->pace_budget -= (double)a->af_audiolen;
        emitted++;
    }
    if (a->pace_budget < 0) a->pace_budget = 0;
    pthread_mutex_unlock(&cli->audio_mutex);
#else
    pthread_mutex_lock(&cli->audio_mutex);
    struct audio_state *a = &cli->audio;

    if (cli->muted) {
        client_free_outq(cli);
        uint8_t sil = 0x84;
        client_enqueue(cli, &sil, 1);
        a->npcm = 0;
        if (cli->wsi) lws_callback_on_writable(cli->wsi);
        pthread_mutex_unlock(&cli->audio_mutex);
        return;
    }

    if (!a->rate_sent) {
        uint8_t init[] = {0x81,0x1F,0x40, 0x82,0x01,0x00, 0x83,0x10};
        client_enqueue(cli, init, sizeof(init));
        a->rate_sent = 1;
    }

    if (a->npcm <= 0) {
        pthread_mutex_unlock(&cli->audio_mutex);
        return;
    }

    {
        double s = audio_compute_smeter(cli);
        a->s_ema = (a->s_ema<=0) ? s : 0.25*s+0.75*a->s_ema;
        int v = (int)lround(a->s_ema);
        if (v<0) v=0;
        if (v>4095) v=4095;
        uint8_t sm[2];
        sm[0] = (uint8_t)(0xF0|((v>>8)&0xF));
        sm[1] = (uint8_t)(v&0xFF);
        client_enqueue(cli, sm, 2);
    }

        /* Steady-rate pacing: PCM is produced in bursts by the band thread (one
     * large FIFO read, up to ~170 ms worth on 192 kHz bands). Dumping it all
     * each tick made the client receive clumps + gaps, which its drift
     * corrector rendered as voice jitter. Emit only what wall-clock has
     * earned at AUDIO_RATE, keeping the rest buffered. */
    {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!a->pace_init) {
            a->pace_last = now;
            a->pace_init = 1;
        }
        double dt = (double)(now.tv_sec - a->pace_last.tv_sec)
                  + (double)(now.tv_nsec - a->pace_last.tv_nsec) / 1e9;
        a->pace_last = now;
        a->pace_budget += dt * AUDIO_RATE;
        if (a->pace_budget > AUDIO_RATE * 0.05) a->pace_budget = AUDIO_RATE * 0.05;
    }

    int off = 0;
    int emit = (int)(a->pace_budget / CB_BLOCK) * CB_BLOCK;
    if (emit > a->npcm) emit = (a->npcm / CB_BLOCK) * CB_BLOCK;
    while (off < emit) {
        uint8_t frame[1+CB_BLOCK];
        frame[0] = 0x80;
        for (int i = 0; i < CB_BLOCK; i++)
            frame[1+i] = (uint8_t)cb_index(a->pcm[off+i]);
        client_enqueue(cli, frame, 1+CB_BLOCK);
        off += CB_BLOCK;
    }
    a->pace_budget -= (double)off;
    if (a->pace_budget < 0) a->pace_budget = 0;

    if (emit < a->npcm)
        memmove(a->pcm, a->pcm+emit, (size_t)(a->npcm-emit)*sizeof(short));
    a->npcm -= emit;
    if (a->npcm < 0) a->npcm = 0;
    pthread_mutex_unlock(&cli->audio_mutex);
#endif /* codec-mode: paced queue drain */
}
