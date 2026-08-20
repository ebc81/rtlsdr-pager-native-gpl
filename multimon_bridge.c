/* SPDX-License-Identifier: GPL-2.0-only
 *
 * multimon_bridge.c -- multimon-ng host glue for the Android build.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * ============================================================================
 * What this file is for
 * ============================================================================
 *
 * multimon-ng splits into decoders (pocsag.c, demod_poc*.c, bch.c) and a host program that
 * owns everything around them (upstream: unixinput.c -- argument parsing, audio input,
 * logging, stdout). Only the decoders are vendored here, so this file provides the host side:
 *
 *   json_mode        pinned to 1; the JNI sink carries JSON, never formatted text
 *   g_pocsag_baud    the bit rate of the demodulator currently running, read by pocsag.c
 *   _verbprintf()    the target of the verbprintf() macro, routed to logcat
 *   addJsonTimestamp() called by pocsag.c for every message it emits
 *
 * The decoder's own configuration globals (pocsag_mode and friends) are defined by pocsag.c
 * itself -- unixinput.c only declares them extern and assigns them, and so does this file.
 *
 * It also owns the three demodulator states and implements pocsag_audio_sink(), overriding the
 * weak no-op default in pocsag_dsp.c.
 *
 * ============================================================================
 * Three bit rates, always
 * ============================================================================
 *
 * 512, 1200 and 2400 bit/s coexist on real paging channels, and nothing in the signal says
 * which is in use until it decodes. So the user never picks one: every audio block goes to all
 * three demodulators, each with its own independent state, and whichever one locks reports the
 * rate on its message. That costs three sign tests per audio sample -- irrelevant next to the
 * decimation upstream of it.
 *
 * Upstream's input path re-feeds `dem_par->overlap` samples of every block to the next call.
 * That is for FIR-based demodulators; the POCSAG ones declare FILTLEN 1 and keep no filter
 * history, so here the blocks are fed exactly once, contiguously. Replicating the overlap
 * would hand each demodulator one duplicated sample per block, which is a slow bit-clock error
 * for no benefit.
 */

#include "multimon_bridge.h"

#include "pocsag_dsp.h"
#include "multimon/bch.h"
#include "multimon/multimon.h"

#include <android/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "POCSAG_DEC"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

/* ---- Host-side globals the vendored decoder expects -------------------------------- */

/*
 * Always JSON. The human-readable path in pocsag.c writes partial lines to stdout and is
 * designed to be read by a person in a terminal; the app needs one self-describing record per
 * message that Kotlin can parse, and stdout does not exist here anyway.
 */
int json_mode = 1;

/*
 * Bit rate of the demodulator currently being run, in bit/s. Set immediately before each
 * demod call below and read by pocsag.c when it emits a message -- the call chain from
 * demod -> pocsag_rxbit -> pocsag_printmessage is entirely synchronous, and only the
 * demodulator thread ever runs it, so no locking is involved.
 */
int g_pocsag_baud = 0;

/* Owned and defined by pocsag.c; assigned here from the session config. */
extern int pocsag_mode;
extern int pocsag_invert_input;
extern int pocsag_error_correction;
extern int pocsag_show_partial_decodes;
extern int pocsag_heuristic_pruning;
extern int pocsag_prune_empty;
extern int pocsag_polarity;
extern bool pocsag_init_charset(char *charset);

/*
 * Runtime verbosity for the verbprintf() macro. The macro itself is compiled with
 * MAX_VERBOSE_LEVEL=3 (see CMakeLists.txt), so levels 4 and above -- the per-bit and per-word
 * traces -- are not even in the binary.
 *
 * Level 1 keeps the per-session BCH statistics pocsag_deinit() prints. Level 2 adds the
 * "message too long" warning that marks the decoder chewing on noise, which is worth having
 * while developing and not worth the log volume in a shipped build.
 */
#ifdef NDEBUG
static int g_verbose_level = 1;
#else
static int g_verbose_level = 2;
#endif

void _verbprintf(int verb_level, const char *fmt, ...)
{
    if (verb_level > g_verbose_level)
        return;

    va_list args;
    va_start(args, fmt);
    /* Upstream writes progressively to stdout, so a single logical line can arrive in several
     * calls and will show up as several logcat lines. Only level 0 does that, and level 0 is
     * inside `if (!json_mode)` throughout pocsag.c, so it never fires here. */
    __android_log_vprint(verb_level <= 1 ? ANDROID_LOG_INFO : ANDROID_LOG_DEBUG, TAG,
                         fmt, args);
    va_end(args);
}

/*
 * Timestamp every emitted message, in milliseconds since the Unix epoch, UTC.
 *
 * Upstream writes a formatted local-time string and only when --timestamp was passed. A number
 * is the better contract across a JNI boundary: no locale, no time zone, no parsing, and
 * Kotlin formats it for display with the device's own settings. The value is the decode time,
 * which for a live receiver is the reception time to within one 23 ms audio block.
 *
 * cJSON prints doubles with %1.17g, and epoch milliseconds are far below 2^53, so this comes
 * out as exact integer digits with no exponent.
 */
void addJsonTimestamp(cJSON *json_output)
{
    if (!json_output)
        return;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;

    double millis = (double)ts.tv_sec * 1000.0 + (double)(ts.tv_nsec / 1000000L);
    cJSON_AddNumberToObject(json_output, "timestamp", millis);
}

/* ---- Demodulator state --------------------------------------------------------------- */

#define POCSAG_RATES 3

/*
 * Bit 6 of struct l2_state_pocsag.state. pocsag.c's `enum states` sets it on every synced
 * state (SYNC, LOSING_SYNC, LOST_SYNC, ADDRESS, MESSAGE, END_OF_MESSAGE) and clears it only
 * for NO_SYNC, which is exactly how pocsag.c's own switch dispatches. The enum is private to
 * that file, so the mask is repeated here rather than exported -- one constant with a comment
 * beats a patch to a vendored header.
 */
#define POCSAG_STATE_SYNC_BIT 64

static const struct demod_param *g_par[POCSAG_RATES];
static struct demod_state g_state[POCSAG_RATES];
static const int g_rate_baud[POCSAG_RATES] = { 512, 1200, 2400 };

static int g_active = 0;

/* ---- Reception statistics ------------------------------------------------------------ */
/*
 * Written by the demodulator thread, read by the USB callback thread once a second. Atomics
 * rather than a mutex: the reader must never be able to block the thread that keeps the USB
 * transfer queue fed, and two ints do not need more than that.
 */
static _Atomic int g_sync_count = 0;
static _Atomic int g_err_ppm = 0;

/*
 * Codewords BCH-checked while in sync, and how many of those could not be repaired. Written by
 * the patched SYNC path in pocsag.c (see multimon/PROVENANCE.md, patch 8) and read here; they
 * are plain uint32_t because only the demodulator thread ever touches them.
 *
 * The counters inside struct l2_state_pocsag cannot answer this question: pocsag_brute_repair()
 * runs in the NO_SYNC search as well, twice per bit per demodulator, and bumps the same
 * uncorrected-error counter. Searching for a sync word in noise fails almost every time, so a
 * rate built from those counters reads ~100 % on an idle channel and never recovers. The
 * decoder self-test showed exactly that: err_ppm = 1000000 while decoding every sample file
 * perfectly. Sampling the sync flag once per audio block is not a fix either -- a demodulator
 * can lose and regain sync inside one 23 ms block, and the noise search in between lands in
 * the interval.
 */
uint32_t g_pocsag_sync_words = 0;
uint32_t g_pocsag_sync_bad_words = 0;

/* Demodulator-thread-private window state. */
static int g_was_synced[POCSAG_RATES];
static uint32_t g_base_sync_words = 0;
static uint32_t g_base_bad_words = 0;
static int g_window_samples = 0;

/**
 * Close the error-rate window and publish it.
 *
 * Deliberately per window rather than cumulative: the user-facing question is "is reception
 * good right now", and a session average would keep reporting the bad minute long after the
 * antenna was moved. A window in which nothing was in sync publishes 0 -- there is no error
 * rate to report when no codewords arrived, and the sync counter beside it already says so.
 */
static void publish_error_rate(void)
{
    uint32_t words = g_pocsag_sync_words;
    uint32_t bad = g_pocsag_sync_bad_words;

    /* A counter going backwards would mean a reset under us; clamp rather than wrap a bogus
     * delta into the window. */
    uint32_t d_words = (words > g_base_sync_words) ? (words - g_base_sync_words) : 0;
    uint32_t d_bad = (bad > g_base_bad_words) ? (bad - g_base_bad_words) : 0;
    g_base_sync_words = words;
    g_base_bad_words = bad;

    int ppm = 0;
    if (d_words > 0) {
        uint64_t scaled = (uint64_t)d_bad * 1000000ull / (uint64_t)d_words;
        ppm = (scaled > 1000000ull) ? 1000000 : (int)scaled;
    }
    atomic_store_explicit(&g_err_ppm, ppm, memory_order_relaxed);
}

void ebc_multimon_stats(int *sync_count, int *err_ppm)
{
    if (sync_count)
        *sync_count = atomic_load_explicit(&g_sync_count, memory_order_relaxed);
    if (err_ppm)
        *err_ppm = atomic_load_explicit(&g_err_ppm, memory_order_relaxed);
}

/* ---- Lifecycle ----------------------------------------------------------------------- */

void ebc_multimon_init(const pocsag_sdr_config_t *cfg)
{
    ebc_multimon_deinit();

    /* POCSAG_MODE_STANDARD..POCSAG_MODE_AUTO. Kotlin validates this too, but a decoder that
     * silently ran with an out-of-range mode would be a nasty thing to debug. */
    if (cfg->decode_mode >= POCSAG_MODE_STANDARD && cfg->decode_mode <= POCSAG_MODE_AUTO) {
        pocsag_mode = cfg->decode_mode;
    } else {
        LOGW("decode mode %d out of range, using standard", cfg->decode_mode);
        pocsag_mode = POCSAG_MODE_STANDARD;
    }

    /* Above 2 bits BCH(31,21) stops correcting and starts producing plausible-looking wrong
     * text, which is worse for the user than a dropped message. */
    pocsag_error_correction = cfg->error_correction;
    if (pocsag_error_correction < 0 || pocsag_error_correction > 2) {
        LOGW("error correction %d out of range, using 1", pocsag_error_correction);
        pocsag_error_correction = 1;
    }

    pocsag_show_partial_decodes = cfg->show_partial ? 1 : 0;
    pocsag_prune_empty = cfg->prune_empty ? 1 : 0;

    /*
     * Not user-visible, and deliberately so:
     *
     * pocsag_invert_input inverts the bit stream before framing. The DSP does not need it --
     * the discriminator sign is fixed by construction -- and pocsag_polarity 0 lets pocsag.c
     * resolve POCSAG's "1 = -4.5 kHz" convention by trying both polarities against the sync
     * word, which is more reliable than any setting a user could pick (AGENTS.md).
     *
     * pocsag_heuristic_pruning drops every message the content heuristic is unsure about. On
     * a real channel that silently throws away good pages, so it stays off; prune_empty
     * already removes the beeper-only traffic that motivates it.
     */
    pocsag_invert_input = 0;
    pocsag_polarity = 0;
    pocsag_heuristic_pruning = 0;

    /* pocsag_init_charset() takes a mutable char* (it only compares, but the signature is
     * upstream's) and the config string belongs to the JNI layer, so copy it. */
    char charset[16];
    const char *requested = (cfg->charset && cfg->charset[0]) ? cfg->charset : "US";
    snprintf(charset, sizeof(charset), "%s", requested);
    if (!pocsag_init_charset(charset))
        LOGW("charset \"%s\" is not one of US/DE/FR/DK/SE/SI, falling back to US", charset);

    /* Explicit rather than lazy on the first codeword: bch_init() builds its tables under the
     * demodulator thread otherwise, and a one-off table build in the middle of the first
     * message is exactly the kind of thing that makes a first-burst-is-always-lost bug. */
    bch_init();

    g_par[0] = &demod_poc5;
    g_par[1] = &demod_poc12;
    g_par[2] = &demod_poc24;

    for (int i = 0; i < POCSAG_RATES; i++) {
        memset(&g_state[i], 0, sizeof(g_state[i]));
        g_state[i].dem_par = g_par[i];
        if (g_par[i]->init)
            g_par[i]->init(&g_state[i]);
        g_was_synced[i] = 0;
    }

    atomic_store_explicit(&g_sync_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_err_ppm, 0, memory_order_relaxed);
    g_pocsag_sync_words = 0;
    g_pocsag_sync_bad_words = 0;
    g_base_sync_words = 0;
    g_base_bad_words = 0;
    g_window_samples = 0;

    g_active = 1;
    LOGI("decoder ready: %s + %s + %s, mode=%d ec=%d charset=%s partial=%d pruneEmpty=%d",
         g_par[0]->name, g_par[1]->name, g_par[2]->name,
         pocsag_mode, pocsag_error_correction, charset,
         pocsag_show_partial_decodes, pocsag_prune_empty);
}

void ebc_multimon_deinit(void)
{
    if (!g_active)
        return;

    g_active = 0;
    for (int i = 0; i < POCSAG_RATES; i++) {
        /* pocsag_deinit() logs the per-rate BCH statistics through verbprintf(1). */
        if (g_par[i] && g_par[i]->deinit)
            g_par[i]->deinit(&g_state[i]);
    }
    LOGI("decoder stopped after %d sync acquisitions",
         atomic_load_explicit(&g_sync_count, memory_order_relaxed));
}

/* ---- Audio sink ---------------------------------------------------------------------- */

/**
 * Receive one block of POCSAG_AUDIO_RATE float samples from the DSP.
 *
 * Overrides the weak no-op in pocsag_dsp.c; the level statistics stayed behind there, in
 * pocsag_dsp_pump(). Runs on the demodulator thread, and a decoded message reaches Kotlin from
 * inside this call, by way of pocsag_printmessage() -> announce_pocsag_message().
 */
void pocsag_audio_sink(const float *samples, int len)
{
    if (!g_active || !samples || len <= 0)
        return;

    buffer_t buffer;
    buffer.sbuffer = NULL;   /* every POCSAG demodulator declares float_samples = true */
    buffer.fbuffer = samples;

    for (int i = 0; i < POCSAG_RATES; i++) {
        /* Read by pocsag.c while this call is on the stack. */
        g_pocsag_baud = g_rate_baud[i];
        /* buffer_t is passed by value, so each demodulator advances its own copy of the
         * pointer and all three see the same block from the start. */
        g_par[i]->demod(&g_state[i], buffer, len);

        int synced = (g_state[i].l2.pocsag.state & POCSAG_STATE_SYNC_BIT) != 0;
        /* Edge-triggered, every block rather than once a second: a burst shorter than the
         * statistics window is exactly the event worth counting. */
        if (synced && !g_was_synced[i])
            atomic_fetch_add_explicit(&g_sync_count, 1, memory_order_relaxed);
        g_was_synced[i] = synced;
    }

    g_window_samples += len;
    if (g_window_samples >= POCSAG_AUDIO_RATE) {
        publish_error_rate();
        g_window_samples = 0;
    }
}
