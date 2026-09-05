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
 * It also owns the three demodulator states and implements pager_audio_sink(), overriding the
 * weak no-op default in pager_dsp.c.
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

#include "pager_dsp.h"
#include "multimon/bch.h"
#include "multimon/multimon.h"

#include <android/log.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "PAGER_DEC"
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

/* Every rate selected. cfg->pocsag_rate_mask is masked with this, so an unknown high bit from a
 * corrupted preference can never enable a demodulator that does not exist. */
#define POCSAG_RATE_MASK_ALL ((1 << POCSAG_RATES) - 1)

static const struct demod_param *g_par[POCSAG_RATES];
static struct demod_state g_state[POCSAG_RATES];
static const int g_rate_baud[POCSAG_RATES] = { 512, 1200, 2400 };

/*
 * Which of the three the user left switched on, derived from cfg->pocsag_rate_mask once in
 * ebc_multimon_init(). Bit i of that mask is index i here, which is also g_rate_baud[i] and
 * g_par[i] -- and POCSAG_BITRATES[i] on the Kotlin side. One ordering, four places.
 *
 * Plain int rather than atomic: written by whoever calls ebc_multimon_init(), which pager_sdr.c
 * does before it creates the demodulator thread, and read-only afterwards. pthread_create() is
 * the happens-before edge. Making the set changeable mid-session would need more than a type
 * change here -- see releases/v1.0.8.md for why it is a restart instead.
 *
 * The other half of that argument, added at v1.1.0 when the rest of the native layer moved to
 * atomics and this deliberately did not: ebc_multimon_deinit() reads g_enabled[] again, and it
 * is safe for the mirror-image reason -- pager_sdr_run() pthread_join()s the demodulator thread
 * before calling it, and the join is the happens-before edge on the way out. Both edges are in
 * pager_sdr_run(). Move either call across the thread lifetime and these must become atomic.
 */
static int g_enabled[POCSAG_RATES];

/*
 * FLEX, beside the POCSAG array rather than inside it.
 *
 * One demodulator, not three: demod_flex_next detects 1600/3200/6400 and 2- or 4-level FSK
 * from the sync word itself, so there is no index set for a rate mask to select over. That is
 * why cfg->flex_enabled is a flag, and why none of the POCSAG constants above grew by one --
 * POCSAG_RATES counts POCSAG demodulators and must keep counting only those.
 *
 * g_flex_enabled follows exactly the g_enabled[] contract: written in ebc_multimon_init()
 * before pager_sdr.c creates the demodulator thread, read again in ebc_multimon_deinit() after
 * it joins, and only read by pager_audio_sink() in between. Both happens-before edges are in
 * pager_sdr_run(). Move either call across the thread's lifetime and this must become atomic.
 *
 * Zero is a legitimate value here, unlike an all-off POCSAG mask: FLEX is the paid feature, so
 * a free session arrives with it off on purpose. Nothing below may "repair" that.
 */
static struct demod_state g_flex_state;
static int g_flex_enabled = 0;

/* Same contract as g_enabled[]: set in init before the thread exists, cleared in deinit after
 * the join, and only read by pager_audio_sink() in between. */
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

void ebc_multimon_init(const pager_sdr_config_t *cfg)
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

    /*
     * Kotlin validates this too (SdrConfig.validateAndClamp), but repair it here as well and
     * loudly, the way decode_mode and error_correction above are. A mask that enables nothing is
     * the nastiest failure this config has: the USB device opens, the level meter moves, every
     * log line looks healthy, and not one page is ever decoded.
     */
    int rate_mask = cfg->pocsag_rate_mask & POCSAG_RATE_MASK_ALL;
    if (rate_mask == 0) {
        LOGW("rate mask 0x%x enables no demodulator, running all %d rates",
             cfg->pocsag_rate_mask, POCSAG_RATES);
        rate_mask = POCSAG_RATE_MASK_ALL;
    }

    char rate_names[96] = "";
    int name_len = 0;

    for (int i = 0; i < POCSAG_RATES; i++) {
        /* memset and dem_par unconditionally, even for a disabled slot: nothing should be able
         * to read a half-initialised struct demod_state, whatever a later edit starts doing in
         * the loops below. */
        memset(&g_state[i], 0, sizeof(g_state[i]));
        g_state[i].dem_par = g_par[i];
        g_was_synced[i] = 0;

        g_enabled[i] = (rate_mask >> i) & 1;
        if (!g_enabled[i])
            continue;

        if (g_par[i]->init)
            g_par[i]->init(&g_state[i]);

        if (name_len < (int)sizeof(rate_names) - 1) {
            int n = snprintf(rate_names + name_len, sizeof(rate_names) - (size_t)name_len,
                             "%s%s", name_len ? " + " : "", g_par[i]->name);
            if (n > 0) {
                name_len += n;
                if (name_len > (int)sizeof(rate_names) - 1)
                    name_len = (int)sizeof(rate_names) - 1;
            }
        }
    }

    /*
     * FLEX after the POCSAG loop, and outside it: it has no slot in g_par[]/g_state[], and it
     * carries its own heap state -- flex_next_init() calls Flex_New(), so this init and the
     * deinit below are a malloc/free pair rather than a memset.
     */
    g_flex_enabled = cfg->flex_enabled ? 1 : 0;
    memset(&g_flex_state, 0, sizeof(g_flex_state));
    g_flex_state.dem_par = &demod_flex_next;
    if (g_flex_enabled && demod_flex_next.init)
        demod_flex_next.init(&g_flex_state);

    atomic_store_explicit(&g_sync_count, 0, memory_order_relaxed);
    atomic_store_explicit(&g_err_ppm, 0, memory_order_relaxed);
    g_pocsag_sync_words = 0;
    g_pocsag_sync_bad_words = 0;
    g_base_sync_words = 0;
    g_base_bad_words = 0;
    g_window_samples = 0;

    g_active = 1;
    /* Names the enabled demodulators rather than all three: this line is the first thing to
     * check when a user reports that nothing decodes. */
    LOGI("decoder ready: %s%s, mode=%d ec=%d charset=%s partial=%d pruneEmpty=%d rateMask=0x%x",
         rate_names, g_flex_enabled ? " + FLEX_NEXT" : "", pocsag_mode, pocsag_error_correction,
         charset, pocsag_show_partial_decodes, pocsag_prune_empty, rate_mask);
}

void ebc_multimon_deinit(void)
{
    if (!g_active)
        return;

    g_active = 0;
    for (int i = 0; i < POCSAG_RATES; i++) {
        /*
         * pocsag_deinit() logs the per-rate BCH statistics through verbprintf(1).
         *
         * Gated on g_enabled[] so init and deinit pair exactly. Calling it on a slot whose
         * pocsag_init() never ran happens to be harmless today -- pocsag_deinit() does nothing
         * unless pocsag_total_error_count is non-zero, and a memset slot's is zero -- but that
         * is a guard inside a vendored file we do not control, not an invariant we hold. Pair
         * them properly rather than depending on it surviving the next upstream rebase.
         */
        if (g_enabled[i] && g_par[i] && g_par[i]->deinit)
            g_par[i]->deinit(&g_state[i]);
    }

    /* Paired with the init above for a stronger reason than the POCSAG slots: flex_next_deinit()
     * frees what Flex_New() allocated, so skipping it leaks ~100 kB per session. It is null-safe
     * either way, but the pairing is what makes that a belt rather than the only strap. */
    if (g_flex_enabled && demod_flex_next.deinit)
        demod_flex_next.deinit(&g_flex_state);
    g_flex_enabled = 0;
    LOGI("decoder stopped after %d sync acquisitions",
         atomic_load_explicit(&g_sync_count, memory_order_relaxed));
}

/* ---- Audio sink ---------------------------------------------------------------------- */

/**
 * Receive one block of PAGER_AUDIO_RATE float samples from the DSP.
 *
 * Overrides the weak no-op in pager_dsp.c; the level statistics stayed behind there, in
 * pager_dsp_pump(). Runs on the demodulator thread, and a decoded message reaches Kotlin from
 * inside this call, by way of pocsag_printmessage() -> announce_pocsag_message().
 */
void pager_audio_sink(const float *samples, int len)
{
    if (!g_active || !samples || len <= 0)
        return;

    buffer_t buffer;
    buffer.sbuffer = NULL;   /* every POCSAG demodulator declares float_samples = true */
    buffer.fbuffer = samples;

    for (int i = 0; i < POCSAG_RATES; i++) {
        /*
         * Skipping here takes out the demod call, the baud label and the sync-edge scan
         * together, which is the point: a disabled rate must not reach g_sync_count, and the
         * error-rate counters it would otherwise feed are shared across all three demodulators.
         * Sharpening that readout is most of why this switch exists.
         */
        if (!g_enabled[i])
            continue;

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

    /*
     * FLEX last, on the same block. buffer_t is passed by value, so this sees the block from
     * the start exactly as each POCSAG demodulator did.
     *
     * It contributes nothing to g_sync_count or to the error rate, and that is a decision
     * rather than an omission: FLEX keeps all of its state behind the opaque l1.flex_next
     * pointer -- there is no l2 slot and no equivalent of POCSAG_STATE_SYNC_BIT to probe --
     * so counting its sync acquisitions would mean a ninth patch to a vendored file, and
     * PROVENANCE.md promises everything but pocsag.c is byte-identical to upstream. The
     * readout stays fed either way: the POCSAG mask can never be empty, so at least one
     * demodulator is always contributing to it.
     */
    if (g_flex_enabled)
        demod_flex_next.demod(&g_flex_state, buffer, len);

    g_window_samples += len;
    if (g_window_samples >= PAGER_AUDIO_RATE) {
        publish_error_rate();
        g_window_samples = 0;
    }
}
