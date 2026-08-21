/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pager_dsp.c -- IQ to pager audio.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 * Derived from rtl_fm, Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>, by way of
 * rtl_ais as vendored in RTL_SDR_AIS_Driver.
 *
 * ============================================================================
 * The chain
 * ============================================================================
 *
 *   RTL-SDR @ 1_411_200 S/s, tuned straight to the channel, CU8 (unsigned 8-bit) IQ
 *     -> uint8 -> int16, centred by subtracting 127
 *     -> CIC decimate /64: 6 passes of fifth_order() on I and on Q
 *     -> generic_fir(cic_9_tables[6]) to undo the CIC passband droop
 *          => 22_050 S/s complex, roughly +/-11 kHz of IF left
 *     -> polar_disc_fast(): FM discriminator, z[n] * conj(z[n-1]) then fast_atan2
 *          => 22_050 S/s real, pi scaled to 1<<14
 *     -> dc_block_filter()
 *     -> int16 -> float, handed to pager_audio_sink()
 *
 * Why 1_411_200: multimon-ng's poc5/poc12/poc24 demodulators hard-code FREQ_SAMP 22050, and
 * 22050 * 64 == 1411200. 64 is a power of two, so the whole rate change is CIC decimation with
 * no resampler and no rate error at all. Every one of those numbers is load-bearing; see
 * AGENTS.md guardrail 6 before changing any of them.
 *
 * Two things deliberately NOT ported from rtl_ais:
 *   - arbitrary_upsample(): its gnuais decoder wanted 48 kHz, so it had to interpolate from
 *     25 kHz. The rate plan above removes the need, and a linear interpolator is the worst
 *     part of that chain.
 *   - rotate_90()/rotate_m90(): AIS needs two channels either side of centre. POCSAG is one
 *     channel and we tune straight to it.
 *
 * ============================================================================
 * The DC blocker is not optional
 * ============================================================================
 *
 * multimon-ng slices bits with `(*buffer.fbuffer) > 0` -- a bare sign test against zero. The
 * FM discriminator's output has a DC component proportional to the frequency error between the
 * transmitter and where we actually tuned, and a few kHz of crystal error is entirely normal on
 * an RTL-SDR. Let that DC through and the sign test answers the same way for every bit, so
 * decoding does not degrade, it stops dead -- with a signal that looks perfectly strong on the
 * level meter.
 *
 * rtl_ais defaults its equivalent filter to OFF. Here it is unconditional. The regression test
 * is to mistune deliberately by 3-4 kHz and confirm messages still decode.
 */

#include "pager_dsp.h"
#include "pager_sdr.h"

#include <android/log.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define TAG "PAGER_DSP"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/*
 * How much IQ we chew per pump() call, in bytes. Must be a multiple of 2 * 2^passes so that
 * every decimation stage divides evenly and no stage is left with a half sample.
 *
 * 65536 bytes = 32768 IQ pairs -> 512 audio samples per block, about 23 ms of audio. Small
 * enough that a decoded message appears promptly, large enough that the per-block overhead is
 * irrelevant.
 */
#define DSP_BLOCK_BYTES     65536
#define DSP_BLOCK_PAIRS     (DSP_BLOCK_BYTES / 2)
#define DSP_AUDIO_PER_BLOCK (DSP_BLOCK_PAIRS / PAGER_DECIMATION)

/*
 * 4 MB ring, about 1.4 seconds at 2.8 MB/s. Generous on purpose: the demodulator thread can be
 * descheduled for a long time on a loaded phone, and dropping IQ costs a whole message.
 */
#define RING_BYTES (4 * 1024 * 1024)

/* Number of /2 CIC passes. 2^6 == 64 == PAGER_DECIMATION. */
#define DECIM_PASSES 6

/*
 * Droop-compensating FIR taps, indexed by the number of CIC passes. Q15, symmetric, 9 taps.
 * Straight from rtl_fm; element [0] is the length. Entry 6 is the one this project uses.
 */
static const int cic_9_tables[][10] = {
    {0,},
    {9, -156,  -97, 2798, -15489, 61019, -15489, 2798,  -97, -156},
    {9, -128, -568, 5593, -24125, 74126, -24125, 5593, -568, -128},
    {9, -129, -639, 6187, -26281, 77511, -26281, 6187, -639, -129},
    {9, -122, -612, 6082, -26353, 77818, -26353, 6082, -612, -122},
    {9, -120, -602, 6015, -26269, 77757, -26269, 6015, -602, -120},
    {9, -120, -582, 5951, -26128, 77542, -26128, 5951, -582, -120},
    {9, -119, -580, 5931, -26094, 77505, -26094, 5931, -580, -119},
    {9, -119, -578, 5921, -26077, 77484, -26077, 5921, -578, -119},
    {9, -119, -577, 5917, -26067, 77473, -26067, 5917, -577, -119},
    {9, -199, -362, 5303, -25505, 77489, -25505, 5303, -362, -199},
};

/* ---- State -------------------------------------------------------------------------- */

/*
 * Single-producer / single-consumer ring: the USB callback thread writes head, the demodulator
 * thread writes tail, and neither ever writes the other's index. That is what makes it safe
 * with nothing but acquire/release ordering and no mutex -- and a mutex here would be actively
 * harmful, because blocking the USB callback stalls transfer resubmission.
 */
static unsigned char *g_ring = NULL;
static _Atomic uint32_t g_ring_head = 0;   /* written by producer */
static _Atomic uint32_t g_ring_tail = 0;   /* written by consumer */
static _Atomic uint64_t g_overflow_pairs = 0;

/* Interleaved I/Q working buffer, decimated in place. */
static int16_t *g_iq = NULL;
/* Real audio, one sample per surviving IQ pair. */
static int16_t *g_audio = NULL;
static float *g_audio_f = NULL;

/* Filter history. Persisting these across blocks is what stops a click every 23 ms. */
static int16_t g_lp_i_hist[DECIM_PASSES][6];
static int16_t g_lp_q_hist[DECIM_PASSES][6];
static int16_t g_droop_i_hist[9];
static int16_t g_droop_q_hist[9];

/* Last IQ sample of the previous block, so the discriminator has no seam. */
static int g_pre_r = 0;
static int g_pre_j = 0;

/* Leaky DC estimate of the discriminator output. */
static int g_dc_avg = 0;

static int g_initialised = 0;

/* ---- Filters (ported verbatim; see the header comment for provenance) ---------------- */

/**
 * Fifth-order CIC decimate-by-two over one interleaved half.
 *
 * Binomial coefficients [1,5,10,10,5,1]/16, i.e. a boxcar^5. Stride 4 with output at i/2
 * because I and Q are interleaved and this runs over one of them at a time.
 *
 * Note `>> 4` and not `>> 5`: the sum of the taps is 32, so this leaves 6 dB of gain per pass.
 * rtl_fm's comment is "a downsample should improve resolution, so don't fully shift" -- the
 * extra bit of headroom is deliberate, and it does not matter downstream anyway because the
 * discriminator output is amplitude-independent.
 */
static void fifth_order(int16_t *data, int length, int16_t *hist)
{
    int i;
    int16_t a, b, c, d, e, f;
    a = hist[2];
    b = hist[3];
    c = hist[4];
    d = hist[5];
    e = data[0];
    f = data[2];
    data[0] = (int16_t)((a + (b + e) * 5 + (c + d) * 10 + f) >> 4);
    for (i = 4; i < length; i += 4) {
        a = c;
        b = d;
        c = e;
        d = f;
        e = data[i];
        f = data[i + 2];
        data[i / 2] = (int16_t)((a + (b + e) * 5 + (c + d) * 10 + f) >> 4);
    }
    hist[0] = a;
    hist[1] = b;
    hist[2] = c;
    hist[3] = d;
    hist[4] = e;
    hist[5] = f;
}

/** Symmetric 9-tap Q15 FIR over one interleaved half, undoing the CIC droop. */
static void generic_fir(int16_t *data, int length, const int *fir, int16_t *hist)
{
    int d, temp, sum;
    for (d = 0; d < length; d += 2) {
        temp = data[d];
        sum = 0;
        sum += (hist[0] + hist[8]) * fir[1];
        sum += (hist[1] + hist[7]) * fir[2];
        sum += (hist[2] + hist[6]) * fir[3];
        sum += (hist[3] + hist[5]) * fir[4];
        sum += hist[4] * fir[5];
        data[d] = (int16_t)(sum >> 15);
        hist[0] = hist[1];
        hist[1] = hist[2];
        hist[2] = hist[3];
        hist[3] = hist[4];
        hist[4] = hist[5];
        hist[5] = hist[6];
        hist[6] = hist[7];
        hist[7] = hist[8];
        hist[8] = (int16_t)temp;
    }
}

/** Run all DECIM_PASSES stages plus droop compensation over one interleaved IQ block. */
static void decimate_iq(int16_t *iq, int len_in)
{
    for (int i = 0; i < DECIM_PASSES; i++) {
        fifth_order(iq, len_in >> i, g_lp_i_hist[i]);
        /* len-1 on the Q half: the last I sample has no Q partner within the block. */
        fifth_order(iq + 1, (len_in >> i) - 1, g_lp_q_hist[i]);
    }
    generic_fir(iq, len_in >> DECIM_PASSES, cic_9_tables[DECIM_PASSES], g_droop_i_hist);
    generic_fir(iq + 1, (len_in >> DECIM_PASSES) - 1, cic_9_tables[DECIM_PASSES],
                g_droop_q_hist);
}

static void complex_multiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
    *cr = ar * br - aj * bj;
    *cj = aj * br + ar * bj;
}

/**
 * Integer four-quadrant arctangent, pre-scaled so pi == 1<<14.
 *
 * An approximation, not atan2(): it is a couple of divisions with no libm call and no
 * floating point. The residual error is a fraction of a percent, which is irrelevant when the
 * only thing downstream is a sign test.
 */
static int fast_atan2(int y, int x)
{
    int yabs, angle;
    const int pi4 = (1 << 12);
    const int pi34 = 3 * (1 << 12);

    if (x == 0 && y == 0)
        return 0;

    yabs = y < 0 ? -y : y;
    if (x >= 0)
        angle = pi4 - pi4 * (x - yabs) / (x + yabs);
    else
        angle = pi34 - pi4 * (x + yabs) / (yabs - x);

    return y < 0 ? -angle : angle;
}

/** arg(a * conj(b)) -- the phase advance between consecutive samples, i.e. FM. */
static int polar_disc_fast(int ar, int aj, int br, int bj)
{
    int cr, cj;
    complex_multiply(ar, aj, br, -bj, &cr, &cj);
    return fast_atan2(cj, cr);
}

/**
 * FM discriminate an interleaved complex block into a real one.
 *
 * [g_pre_r]/[g_pre_j] carry the previous block's last sample so there is no phase
 * discontinuity at the block boundary -- without them, every 23 ms boundary would inject a
 * spike and cost a bit.
 */
static int demodulate(const int16_t *iq, int pairs, int16_t *out)
{
    if (pairs <= 0)
        return 0;

    out[0] = (int16_t)polar_disc_fast(iq[0], iq[1], g_pre_r, g_pre_j);
    int n = 1;
    for (int i = 2; i < pairs * 2; i += 2) {
        out[n++] = (int16_t)polar_disc_fast(iq[i], iq[i + 1], iq[i - 2], iq[i - 1]);
    }

    g_pre_r = iq[(pairs - 1) * 2];
    g_pre_j = iq[(pairs - 1) * 2 + 1];
    return n;
}

/**
 * Remove the frequency-offset DC from the discriminator output.
 *
 * A single-pole leaky average of the per-block mean, alpha = 0.1. Slow on purpose: it must
 * track crystal drift (which moves over seconds) without following the message itself, and a
 * POCSAG burst is a long run of same-sign bits that a fast filter would happily flatten into
 * nothing.
 */
static void dc_block(int16_t *samples, int len)
{
    if (len <= 0)
        return;

    int64_t sum = 0;
    for (int i = 0; i < len; i++)
        sum += samples[i];

    int avg = (int)(sum / len);
    avg = (avg + g_dc_avg * 9) / 10;
    for (int i = 0; i < len; i++)
        samples[i] = (int16_t)(samples[i] - avg);
    g_dc_avg = avg;
}

/* ---- Lifecycle ---------------------------------------------------------------------- */

int pager_dsp_init(void)
{
    pager_dsp_deinit();

    g_ring = malloc(RING_BYTES);
    g_iq = malloc(sizeof(int16_t) * DSP_BLOCK_PAIRS * 2);
    g_audio = malloc(sizeof(int16_t) * (DSP_AUDIO_PER_BLOCK + 8));
    g_audio_f = malloc(sizeof(float) * (DSP_AUDIO_PER_BLOCK + 8));

    if (!g_ring || !g_iq || !g_audio || !g_audio_f) {
        LOGE("out of memory setting up the DSP");
        pager_dsp_deinit();
        return -1;
    }

    atomic_store_explicit(&g_ring_head, 0, memory_order_relaxed);
    atomic_store_explicit(&g_ring_tail, 0, memory_order_relaxed);
    atomic_store_explicit(&g_overflow_pairs, 0, memory_order_relaxed);

    /* Zeroed, not left over: stale history from a previous session would corrupt the first
     * few hundred samples of this one. */
    memset(g_lp_i_hist, 0, sizeof(g_lp_i_hist));
    memset(g_lp_q_hist, 0, sizeof(g_lp_q_hist));
    memset(g_droop_i_hist, 0, sizeof(g_droop_i_hist));
    memset(g_droop_q_hist, 0, sizeof(g_droop_q_hist));
    g_pre_r = 0;
    g_pre_j = 0;
    g_dc_avg = 0;

    g_initialised = 1;
    LOGI("DSP ready: %d S/s IQ -> /%d -> %d S/s audio, %d bytes per block (%d audio samples)",
         PAGER_RTL_SAMPLE_RATE, PAGER_DECIMATION, PAGER_AUDIO_RATE,
         DSP_BLOCK_BYTES, DSP_AUDIO_PER_BLOCK);
    return 0;
}

void pager_dsp_deinit(void)
{
    g_initialised = 0;
    free(g_ring);
    g_ring = NULL;
    free(g_iq);
    g_iq = NULL;
    free(g_audio);
    g_audio = NULL;
    free(g_audio_f);
    g_audio_f = NULL;
}

/* ---- Producer: called on the USB callback thread ------------------------------------ */

uint32_t pager_dsp_push(const unsigned char *buf, uint32_t len)
{
    if (!g_initialised || !buf || len == 0)
        return 0;

    uint32_t tail = atomic_load_explicit(&g_ring_tail, memory_order_acquire);
    /* Only this thread writes head, so a relaxed read of our own index is fine. */
    uint32_t head = atomic_load_explicit(&g_ring_head, memory_order_relaxed);

    uint32_t used = (head >= tail) ? (head - tail) : (head - tail + RING_BYTES);
    uint32_t free_bytes = RING_BYTES - used - 1;   /* -1 keeps full distinguishable from empty */

    if (len > free_bytes) {
        /*
         * Drop the whole block rather than a partial one. Splitting a block would leave an odd
         * number of bytes in the ring and swap I with Q for the rest of the session -- a far
         * worse failure than losing 23 ms of audio.
         */
        atomic_fetch_add_explicit(&g_overflow_pairs, len / 2, memory_order_relaxed);
        return 0;
    }

    if (head + len <= RING_BYTES) {
        memcpy(g_ring + head, buf, len);
    } else {
        uint32_t first = RING_BYTES - head;
        memcpy(g_ring + head, buf, first);
        memcpy(g_ring, buf + first, len - first);
    }

    atomic_store_explicit(&g_ring_head, (head + len) % RING_BYTES, memory_order_release);
    return len;
}

uint64_t pager_dsp_overflow_count(void)
{
    return atomic_load_explicit(&g_overflow_pairs, memory_order_relaxed);
}

/* ---- Consumer: called on the demodulator thread ------------------------------------- */

/**
 * Log what the discriminator is actually producing, once every 5 seconds.
 *
 * This is the hardware test for the whole chain, and it belongs here rather than in the sink
 * because it describes the DSP's output and must stay true whoever consumes it:
 *
 *   rms   rises clearly while a burst passes and sits low on an empty channel
 *   mean  must converge towards zero within a few seconds. A mean that settles on a large
 *         value means the DC blocker is not working, and then the decoder finds nothing at all
 *         while the level meter looks perfectly healthy -- see the header comment.
 */
static void level_stats(const float *samples, int len)
{
    /* One reporting window, reset each time it is emitted. */
    static uint64_t window_samples = 0;
    static double window_sum = 0.0;
    static double window_sum_sq = 0.0;

    for (int i = 0; i < len; i++) {
        window_sum += samples[i];
        window_sum_sq += (double)samples[i] * (double)samples[i];
    }
    window_samples += (uint64_t)len;

    if (window_samples >= (uint64_t)PAGER_AUDIO_RATE * 5) {
        double mean = window_sum / (double)window_samples;
        double rms = sqrt(window_sum_sq / (double)window_samples);
        LOGI("audio: rms=%.4f mean=%.5f over %llu samples, ring overflow pairs=%llu "
             "(mean should sit near zero once the DC blocker has settled)",
             rms, mean, (unsigned long long)window_samples,
             (unsigned long long)pager_dsp_overflow_count());
        window_samples = 0;
        window_sum = 0.0;
        window_sum_sq = 0.0;
    }
}

int pager_dsp_pump(void)
{
    if (!g_initialised)
        return 0;

    uint32_t head = atomic_load_explicit(&g_ring_head, memory_order_acquire);
    uint32_t tail = atomic_load_explicit(&g_ring_tail, memory_order_relaxed);

    uint32_t available = (head >= tail) ? (head - tail) : (head - tail + RING_BYTES);
    if (available < DSP_BLOCK_BYTES)
        return 0;

    /* CU8 -> centred int16. 127 rather than 127.5: the DC blocker downstream removes the
     * residual half-LSB offset anyway, and an integer subtraction is free. */
    for (uint32_t i = 0; i < DSP_BLOCK_BYTES; i++) {
        uint32_t idx = (tail + i) % RING_BYTES;
        g_iq[i] = (int16_t)((int)g_ring[idx] - 127);
    }
    atomic_store_explicit(&g_ring_tail, (tail + DSP_BLOCK_BYTES) % RING_BYTES,
                          memory_order_release);

    decimate_iq(g_iq, DSP_BLOCK_PAIRS * 2);

    int audio_len = demodulate(g_iq, DSP_AUDIO_PER_BLOCK, g_audio);
    dc_block(g_audio, audio_len);

    /*
     * multimon's poc*_demod read buffer.fbuffer, so float it is. Scale by 1/32768 to land in
     * roughly +/-1: the demodulators only test the sign, but keeping a conventional range
     * means anything added later (a scope, an AGC) sees sane numbers.
     */
    for (int i = 0; i < audio_len; i++)
        g_audio_f[i] = (float)g_audio[i] / 32768.0f;

    level_stats(g_audio_f, audio_len);
    pager_audio_sink(g_audio_f, audio_len);
    return audio_len;
}

/* ---- Default audio sink ------------------------------------------------------------- */

/*
 * Weak so multimon_bridge.c overrides it by simply existing, without this file having to know
 * whether the decoder is linked in. With the decoder present this is dead code -- deliberately
 * kept, because it is what makes the DSP testable on its own if the bridge is ever taken out
 * of the build.
 *
 * The signal measurement that used to live here moved into level_stats() above: it is a
 * property of the DSP output, and losing it the moment a real sink appeared would have thrown
 * away the only hardware check for the DC blocker.
 */
__attribute__((weak)) void pager_audio_sink(const float *samples, int len)
{
    (void)samples;
    (void)len;
}
