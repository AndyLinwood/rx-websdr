/*
 * WebSDR Server — Adaptive compressed-audio encoder.
 *
 * Faithful port of VertexSDR send_audio_compressed3 (src/client.c:664),
 * LGPL-3.0 (see COPYING in /Volumes/40ГБ/VertexSDR), adapted to our own
 * struct client / struct audio_state and outq (client_enqueue).
 *
 * Format is the original WebSDR compressed audio (0x90-0xDF frames): a
 * backward-adaptive 20-tap linear predictor + variable-length quantised
 * residual codes, with a mu-law 0x80 fallback on residual overflow (which also
 * resets the predictor) and a 0x84 silent frame on mute.
 *
 * The demod (audio_fft.c) produces exactly `af_audiolen` float samples per
 * block, so the codec is fed one block at a time and emits a self-contained
 * frame (its own 0x81 rate / 0x82 block-size / 0x83 conv / 0xF0 smeter
 * headers). The predictor requires a contiguous sample stream, so this path
 * must not drop/pacer samples the way the codebook-0x80 path does.
 */

/* websdr.h must be included BEFORE the #if below: it defines AUDIO_USE_FFT /
 * AUDIO_USE_CODEC. (audio_fft.c shares this constraint.) */
#include "websdr.h"

#if AUDIO_USE_FFT && AUDIO_USE_CODEC

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <libwebsockets.h>

extern struct lws_context *g_lws_ctx;

/* Same mulaw exponent table as WebSDR / VertexSDR — bitlayout used by both the
 * 0x80 fallback and (indirectly) the decompressor expectations. */
static const uint8_t mulaw_log2[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,
    4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

static uint8_t codec_pcm16_to_mulaw(int16_t sample)
{
    int sign_bit = ((~(int)sample) >> 8) & 0x80;
    int magnitude = (int)sample;
    if (!sign_bit) magnitude = -magnitude;
    if (magnitude > 32635) magnitude = 32635;
    if (magnitude <= 255)
        return (uint8_t)(sign_bit ^ 0x55 ^ ((unsigned)magnitude >> 4));
    int exponent = mulaw_log2[(unsigned)magnitude >> 8] + 1;
    return (uint8_t)(sign_bit ^ 0x55
                     ^ (((magnitude >> (exponent + 3)) & 0xF)
                        | (unsigned)(16 * exponent)));
}

void audio_codec_send(struct client *cli, float scale, int smeter_raw)
{
    struct audio_state *a = &cli->audio;

    uint8_t frame[1024];
    int fpos = 0;

    /* Periodic 0xF0 smeter header, every 8 frames (matches reference).
     * smeter_raw is already the calibrated 12-bit value (audio_compute_smeter
     * returns (dBm+127)*10 clamped 0..4095; the client renders dBm=field/10-127),
     * so it is sent as-is — do NOT divide by 10 here (VertexSDR uses a larger
     * smeter_raw scale and needs the /10; we don't). */
    a->c_header_counter--;
    if (a->c_header_counter <= 0) {
        int v = smeter_raw;
        if (v < 0) v = 0;
        if (v > 4095) v = 4095;
        frame[fpos++] = (uint8_t)(0xF0 | ((v >> 8) & 0x0F));
        frame[fpos++] = (uint8_t)(v & 0xFF);
        a->c_header_counter = 8;
    }

    /* 0x81: audio rate (Hz). Emit once and re-emit whenever the MEASURED feed
     * clock changed af_rate (first frame precedes the ~2s measurement window). */
    if (!a->rate_sent || a->af_rate != a->c_last_rate) {
        int audio_rate = a->af_rate;
        frame[fpos++] = 0x81;
        frame[fpos++] = (uint8_t)((audio_rate >> 8) & 0xFF);
        frame[fpos++] = (uint8_t)(audio_rate & 0xFF);

        /* 0x82: decoder block size (== audioformat blk_sizes[2] = 256). */
        int bs = a->c_block_size;
        frame[fpos++] = 0x82;
        frame[fpos++] = (uint8_t)((bs >> 8) & 0xFF);
        frame[fpos++] = (uint8_t)(bs & 0xFF);
        a->rate_sent = 1;
        a->c_last_rate = a->af_rate;
    }

    /* 0x83: conv type, only when it changes. */
    if (a->c_conv_sent != a->af_conv) {
        frame[fpos++] = 0x83;
        frame[fpos++] = (uint8_t)a->af_conv;
        a->c_conv_sent = a->af_conv;
    }

    if (cli->muted) {
        memset(a->pred_h, 0, sizeof(a->pred_h));
        memset(a->pred_x, 0, sizeof(a->pred_x));
        a->pred_accum = 0;
        frame[fpos++] = 0x84;
        a->af_on = 0;
        client_enqueue(cli, frame, (size_t)fpos);
        return;
    }

    int n = a->af_audiolen;
    if (a->af_on < n)
        return;                              /* not a full block yet (pacer guard) */
    const float *audio_f = a->af_obuf;

    int residuals[128];
    int pred_sum = 0;
    for (int j = 0; j < 20; j++)
        pred_sum += a->pred_h[j] * a->pred_x[j];

    int blk = a->c_block_size;
    int neg_half = -(blk / 2);
    int blk_shift = blk << 16;
    int adpcm_shift = (a->af_conv & 0x10) ? 12 : 14;
    double sum_abs = 0.0, sum_raw = 0.0, sum_pred = 0.0;
    int overflow = 0;

    for (int i = 0; i < n; i++) {
        int old_accum = a->pred_accum;
        int prediction = pred_sum / 4096;

        int raw_delta = (int)(audio_f[i] * scale - (float)(old_accum >> 4));
        int raw_delta_sq = raw_delta * raw_delta;
        int error = raw_delta - prediction;

        sum_raw += (double)raw_delta_sq;
        sum_pred += (double)(error * error);

        int wrapped = (neg_half + blk_shift + error) / blk - 0x10000;
        int aw = wrapped < 0 ? -wrapped : wrapped;
        int mask = -1;
        if (aw > 16) mask = (aw <= 32) ? -2 : -4;
        wrapped = mask & wrapped;

        residuals[i] = wrapped;
        int abs_res = wrapped ^ (wrapped >> 31);
        sum_abs += (double)abs_res;

        if (abs_res > 1000) { overflow = 1; break; }

        int scaled = blk * wrapped + (blk / 2);
        int adapt_sample = scaled >> 4;

        int partial = 0;
        for (int j = 19; j >= 1; j--) {
            int xprev  = a->pred_x[j - 1];
            int hj     = a->pred_h[j];
            int xj     = a->pred_x[j];
            int new_hj = hj + ((adapt_sample * xj) >> adpcm_shift) - (hj >> 7);
            a->pred_h[j] = new_hj;
            a->pred_x[j] = xprev;
            partial += xprev * new_hj;
        }

        int old_x0 = a->pred_x[0];
        int adapt_product = old_x0 * adapt_sample;
        int predicted_sample = prediction + scaled;
        int h0 = a->pred_h[0];
        int new_h0 = h0 + (adapt_product >> adpcm_shift) - (h0 >> 7);

        a->pred_x[0] = predicted_sample;
        a->pred_h[0] = new_h0;
        pred_sum = predicted_sample * new_h0 + partial;

        if (a->af_conv & 0x10)
            a->pred_accum = 0;
        else
            a->pred_accum = old_accum + ((16 * predicted_sample) >> 3);
    }

    /* No squelch on our server (client doesn't send a squelch param), so the
     * squelch gate is skipped — silence-by-input is handled by AGC/0x84. */

    if (overflow) {
        memset(a->pred_h, 0, sizeof(a->pred_h));
        memset(a->pred_x, 0, sizeof(a->pred_x));
        a->pred_accum = 0;
        frame[fpos++] = 0x80;
        for (int i = 0; i < n; i++) {
            int sample = (int)(audio_f[i] * scale);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            frame[fpos++] = codec_pcm16_to_mulaw((int16_t)sample);
        }
        goto consumed;
    }

    float avg_residual = sum_abs * 0.0078125f;
    int quant_step, quant_mode, reduced_mant_bits, quotient_limit;

    if (avg_residual >= 3.81f) {
        if (avg_residual < 8.0f) {
            quotient_limit = 12; quant_step = 4; reduced_mant_bits = 2; quant_mode = 3;
        } else if (avg_residual < 16.3f) {
            quotient_limit = 11; quant_step = 8; reduced_mant_bits = 3; quant_mode = 4;
        } else {
            quotient_limit = 10; quant_step = 16; reduced_mant_bits = 4; quant_mode = 5;
        }
    } else {
        if (avg_residual < 1.65f) {
            quotient_limit = 14; quant_step = 1; reduced_mant_bits = 0; quant_mode = 1;
        } else {
            quotient_limit = 13; quant_step = 2; reduced_mant_bits = 1; quant_mode = 2;
        }
    }

    uint8_t *bstart = frame + fpos;
    int mantissa_shrink_thresholds[8] = {999, 999, 8, 4, 2, 1, 99, 99};
    int bitpos;

    if (a->c_quant_mode == quant_mode) {
        *bstart = 0;
        bitpos = 1;
    } else {
        *bstart = (uint8_t)((16 * (6 - quant_mode)) ^ 0x80);
        bitpos = 4;
    }

    uint8_t *bp = bstart;
    int encode_ok = 1;

    for (int i = 0; i < n && encode_ok; i++) {
        int val = residuals[i];
        int sign = (unsigned int)val >> 31;
        int abs_val = val ^ (val >> 31);
        int quotient = abs_val / quant_step;

        if (quotient > 255) { encode_ok = 0; break; }

        int prefix_len, prefix_val;
        if (quotient >= quotient_limit) {
            prefix_len = 23 - quant_mode;
            prefix_val = quotient;
        } else {
            prefix_len = quotient + 1;
            prefix_val = 1;
        }

        int mantissa = abs_val & (quant_step - 1);
        int mant_bits = quant_mode;

        if (quotient >= mantissa_shrink_thresholds[quant_mode]) {
            mantissa >>= 1;
            mant_bits = reduced_mant_bits;
        }
        if (quotient >= mantissa_shrink_thresholds[reduced_mant_bits]) {
            mantissa >>= 1;
            mant_bits--;
        }
        if (mant_bits <= 0) mant_bits = 1;

        int code = (prefix_val << mant_bits) | (2 * mantissa + sign);
        int code_len = mant_bits + prefix_len;
        int shift_amt = 32 - code_len - bitpos;
        int shifted = code << shift_amt;

        *bp |= (uint8_t)(shifted >> 24);
        bitpos += code_len;
        if (bitpos > 7) {
            unsigned int extra = bitpos - 8;
            uint8_t *end = bp + 1 + (extra >> 3);
            while (bp < end) {
                shifted <<= 8;
                *(++bp) = (uint8_t)(shifted >> 24);
            }
            bitpos = extra - 8 * (extra >> 3);
        }
    }

    if (!encode_ok) {
        fpos = (int)(bstart - frame);
        memset(a->pred_h, 0, sizeof(a->pred_h));
        memset(a->pred_x, 0, sizeof(a->pred_x));
        a->pred_accum = 0;
        frame[fpos++] = 0x80;
        for (int i = 0; i < n; i++) {
            int sample = (int)(audio_f[i] * scale);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            frame[fpos++] = codec_pcm16_to_mulaw((int16_t)sample);
        }
    } else {
        int compressed_len = (int)(bp - bstart) + (bitpos > 0 ? 1 : 0);
        fpos = (int)(bstart - frame) + compressed_len;
    }

    a->c_quant_mode = quant_mode;

consumed:
    /* Consume one af_audiolen block from the front of the buffered queue; the
     * remainder stays for the next paced frame. */
    {
        int rest = a->af_on - n;
        if (rest > 0)
            memmove(a->af_obuf, a->af_obuf + n, (size_t)rest * sizeof(float));
        a->af_on = rest;
    }
    client_enqueue(cli, frame, (size_t)fpos);
}

#endif /* AUDIO_USE_FFT && AUDIO_USE_CODEC */
