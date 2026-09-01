#ifndef WEBSDR_H
#define WEBSDR_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <time.h>
#include <fftw3.h>

#define MAX_BANDS 32
#define MAX_CLIENTS 256
#define WATERFALL_WIDTH 1024
#define FFT_SIZE 8192

/* Audio demodulation path selector.
 * 1 = band-wide FFT + per-client frequency-domain demod (src/audio_fft.c, port
 *     of VertexSDR dsp.c/band.c/client.c, LGPL-3.0). DEPLOYED and confirmed on
 *     the live 8095 server [18 авг]: fixes 15m signal stretching, SSB/AM good.
 * 0 = legacy per-client time-domain NCO+FIR chain (src/audio.c), kept for
 *     instant rollback (server.c pacer must gate on audio.fir when 0). */
#define AUDIO_USE_FFT 1

/* Adaptive compressed-audio encoder (port of VertexSDR send_audio_compressed3,
 * LGPL-3.0). 1 = FFT path encodes via the adaptive predictor + variable-length
 * quantised residuals (0x90-0xDF frames) with mu-law 0x80 fallback. 0 = FFT
 * path keeps emitting plain codebook-0x80 frames (audio_flush_pcm untouched).
 * Requires AUDIO_USE_FFT=1. */
#define AUDIO_USE_CODEC 1

struct client;

struct band {
    char name[64];
    char device[256];
    int samplerate;
    double centerfreq;
    double freqoffset;       /* Hz: calibration offset applied to centerfreq */;
    double gain;
    bool swapiq;
    int hpf;
    int noiseblanker;
    int nstations;
    struct { double freq; char mode[8]; char name[128]; } stations[256];
    int maxzoom;
    int fifo_fd;
    fftwf_plan fft_plan;
    float fft_input[FFT_SIZE * 2];
    float fft_output[FFT_SIZE * 2];
    uint8_t waterfall_line[WATERFALL_WIDTH];
    uint8_t prev_line[WATERFALL_WIDTH];
    float power_spectrum[WATERFALL_WIDTH];
    float avg_power[WATERFALL_WIDTH];
    float power_hi[FFT_SIZE];
    float wf_hi[FFT_SIZE];
    float wf_floor;
    int   wf_init;
    int   wf_resync;
    int   wf_dbgcal;
    double noise_dB;
    int   noise_init;
    pthread_mutex_t lock;
    pthread_t thread;
    bool running;
    struct client *clients[MAX_CLIENTS];
    int nclients;
#if AUDIO_USE_FFT
    /* --- Band-wide audio FFT backbone (shared by all audio clients) --- */
    int af_fftlen;          /* audio FFT size N (sr*32/1000 -> 8k audio) */
    int af_half_fftlen;     /* next fire threshold; cycles N/4 -> 3N/4 -> N */
    int af_sample_count;    /* input sample cursor, cycles 0..N-1 */
    fftwf_complex *af_in;   /* N complex slots: sliding window of last N IQ */
    fftwf_complex *af_out;  /* forward FFT result (N complex) */
    float *af_spec;         /* 2N floats: quadrant-swapped r2c layout */
    fftwf_plan af_plan;     /* complex forward plan */
    int af_ready;
    /* --- Measured input sample clock (port of Vertex stdin_get_measured_sps).
     * The FIFO feed is what it is (radiod may output a rate that differs from
     * the configured one; the config number is only a hint). The audio rate we
     * must present to the client is derived from the MEASURED rate, otherwise
     * the client's drift corrector (±0.2%) saturates and the buffer drains ->
     * periodic dropouts = voice trembling. Window ~2s like the reference. */
    long   af_in_samples;   /* int16 IQ pairs consumed since window start */
    struct timespec af_clock_t0;  /* window start (wall clock) */
    int    af_measured_sr;  /* measured input complex samples/sec */
#endif
};


struct websdr_config {
    int tcpport;
    int maxusers;
    int idletimeout;
    int waterfallformat;
    int audioformat;
    int nbands;
    double ini_freq;
    char  ini_mode[16];
    int   chseq;
    struct band bands[MAX_BANDS];
};


/* Per-client audio backlog (codec queue af_obuf / codebook pcm[]). 2048 =
 * 256 ms at 8 kHz: bounds the worst-case server-side audio delay while still
 * leaving headroom for the largest FIFO read clump (~170 ms on 192 kHz bands)
 * before the overflow guard resets the queue. */
#define AUDIO_BUFSZ 2048

struct audio_state {
    double nco_phase;
    double nco_inc;
    int decim;
    int decim_count;
    /* Stage 1: anti-alias lowpass at samplerate */
    double *fir;
    int fir_len;
    int fir_pos;
    double *fir_delay_i;
    double *fir_delay_q;
    /* Stage 2: bandpass at 8kHz */
    double *fir2;
    int fir2_len;
    int fir2_pos;
    double *fir2_delay_i;
    double *fir2_delay_q;
    int dec2_count;
    /* Mode / passband */
    int mode;
    double lo_hz;
    double hi_hz;
    /* DC removal */
    double dc_sum;
    int dc_count;
    double dc_avg;
    /* Steady-rate output pacing. PCM is produced in bursts (one big FIFO read
     * per band thread iter), so dumping everything at each flush makes the
     * wire deliver in clumps (instant rate -54%..+22%) and the client's
     * drift-corrector wobbles the pitch ("trembling" voices). `pace_budget`
     * accumulates samples scheduled by wall-clock at AUDIO_RATE: each flush
     * emits at most what time has earned, keeping the rest buffered. */
    double pace_budget;
    struct timespec pace_last;
    int pace_init;
    /* AGC */
    double peak;
    double last_phase;       /* FM discriminator state */
    int rate_sent;
    /* Output */
    short pcm[AUDIO_BUFSZ];
    int npcm;
    double s_ema;
#if AUDIO_USE_FFT
    /* --- FFT-domain per-client demod state (audio_fft.c) --- */
    int af_half_size;        /* demod IFFT half width (128 in 8k mode) */
    int af_audiolen;         /* audio samples per block == af_half_size */
    int af_rate;             /* 2*af_audiolen*sr/fftlen (8000 in 8k mode) */
    int af_tune_bin;
    int af_filter_dirty;     /* recompute af_fbuf on next block */
    int af_filter_lo_bin;
    int af_filter_hi_bin;
    /* Band whose audio is currently buffered in af_obuf. Used to tell a BAND
     * SWITCH (drop the old band's buffer, reset codec, soft handoff) from a
     * plain retune within the same band (stream is continuous — no reset, or
     * the sound cuts on every frequency drag). */
    struct band *af_buf_band;
    /* Snapshot of the tuning taken under audio_mutex at reconfigure time, so
     * the band thread never reads cli->freq/lo/hi/mode mid-write from the lws
     * thread. */
    double af_freq;
    int af_lo;
    int af_hi;
    float af_fbuf[1024];     /* passband table product (filter_table.h) */
    float af_agc_gain;
    float af_agc_peak;       /* per-block RMS/env level for AGC 1/√power */
    float af_am_dc;
    float af_fm_prev_re, af_fm_prev_im;
    float af_fm_prev2_re, af_fm_prev2_im;
    fftwf_complex *af_din;   /* 2*half complex (IFFT input) */
    fftwf_complex *af_dout;  /* 2*half complex (IFFT output, AM/FM) */
    float *af_dout_r;        /* 2*half floats (IFFT output, SSB c2r) */
    fftwf_plan af_dplan;
    int af_dplan_len;        /* IFFT size (2*half) */
    int af_am_plan;          /* 1 = complex IFFT (AM/FM), 0 = c2r (SSB); kind used when plan was created */
    /* --- adaptive codec state (port of VertexSDR send_audio_compressed3) ---
     * The demod writes scaled FLOAT samples into af_obuf[] (not int16 pcm);
     * the pacer drains it in 128-blocks through audio_codec_encode(). */
    float af_obuf[AUDIO_BUFSZ];
    int af_on;               /* valid samples in af_obuf */
    int af_conv;             /* conv_type for this filter (adpcm_shift = &0x10 ? 12 : 14) */
    int c_smeter;              /* latest smeter value handed to the codec */
    int c_block_size;        /* Ot (0x82); 256 matches the init frame for audioformat 2 */
    int c_last_rate;         /* last audio rate told to the client via 0x81 */
    int c_quant_mode;
    int c_header_counter;    /* cadence for 0xF0 smeter headers */
    int c_conv_sent;         /* last conv_type we told the client via 0x83; -1 = none yet */
    int pred_h[20];
    int pred_x[20];
    int pred_accum;
#endif
};


struct client {
    int fd;
    struct band *band;
    struct lws *wsi;
    int zoom;
    int start;
    bool waterfall_active;
    bool audio_active;
    bool muted;
    int mode;
    double freq;
    int lo_filter;
    int hi_filter;
    char username[64];        /* callsign/name from ~~param (for the users list) */
    int uu_index;             /* stable slot index for the /~~othersjj users list */
    struct audio_state audio;
    bool audio_stream;
    /* Serializes access to `audio` (and the audio FIR state) between the
     * per-band DSP thread (audio_process_iq / audio_flush_pcm, which run under
     * the band lock) and the lws service thread (audio_init/reconfigure on
     * retune, audio_free on disconnect). Without it a client retune frees and
     * reallocs the FIR arrays while the DSP thread is mid-loop reading/writing
     * them -> use-after-free / SEGV core dump on frequency retune. */
    pthread_mutex_t audio_mutex;
    uint8_t prev_line[WATERFALL_WIDTH];
    pthread_mutex_t out_mutex;
#define CLIENT_OUT_MAX 128
    uint8_t *outq[CLIENT_OUT_MAX];
    size_t outq_len[CLIENT_OUT_MAX];
    int outq_head;
    int outq_tail;
};


int config_load(const char *filename, struct websdr_config *config);
int bandinfo_build(char *buf, size_t cap, struct websdr_config *config, const char *ts);
int scale_generate_all(const char *pubdir, const char *ts, struct websdr_config *config);
void *band_thread(void *arg);
void waterfall_init(void);
void waterfall_process(struct band *band, int16_t *iq_data, int samples);
float wf_brightness(float avg_power, double noise_dB, double gain_db);
int server_start(struct websdr_config *config);
void protocol_handle_message(struct client *client, const uint8_t *data, size_t len);
int compress_waterfall_format9(const uint8_t *newrow, uint8_t *prevrow, int width, uint8_t *out);
void client_add_to_band(struct client *cli, struct band *band);
void client_remove_from_band(struct client *cli);
void client_send_waterfall_control(struct client *cli);
void band_send_waterfall(struct band *band);
void client_enqueue(struct client *cli, const uint8_t *data, size_t len);
void client_free_outq(struct client *cli);

void audio_init(struct client *cli, int samplerate);
void audio_free(struct client *cli);
void audio_reconfigure(struct client *cli, int samplerate,
                       double centerfreq_khz);
void audio_process_iq(struct client *cli, const int16_t *iq, int nsamples);
void audio_flush_pcm(struct client *cli);
void *audio_pacer_thread(void *arg);
int audio_compute_smeter(struct client *cli);

#if AUDIO_USE_FFT
int  audio_fft_band_init(struct band *b);
void audio_fft_band_free(struct band *b);
void audio_fft_push_iq(struct band *b, const int16_t *iq, int nsamples);
void audio_fft_client_setup(struct client *cli);
#if AUDIO_USE_CODEC
/* Adaptive compressed-audio encoder. Consumes exactly af_audiolen float samples
 * already present in cli->audio.af_obuf and emits a self-contained frame (its
 * own 0x81/0x82/0x83 / 0xF0 smeter / 0x84 mute / 0x80 mu-law fallback headers)
 * via client_enqueue. Called per demod block from the band thread. */
void audio_codec_send(struct client *cli, float scale, int smeter_raw);
#endif /* AUDIO_USE_CODEC */
#endif /* AUDIO_USE_FFT */

/* Effective centre frequency in kHz, accounting for per-band calibration. */
static inline double band_eff_center(const struct band *b) {
    return b->centerfreq + b->freqoffset / 1000.0;
}

int stationinfo_load(const char *filename, struct websdr_config *config);
#endif
