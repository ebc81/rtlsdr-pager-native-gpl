/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pager_sdr.c -- RTL-SDR device lifecycle for the pager receiver.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * Opens the dongle from an Android UsbManager file descriptor, configures it for a paging
 * channel, streams IQ until asked to stop, and tears down in an order that does not trip
 * libusb's unplug paths.
 *
 * The device is tuned DIRECTLY to the channel -- there is no NCO or channel offset. POCSAG
 * needs about 12.5 kHz of bandwidth out of the 1.4112 MS/s we capture, so the decimator in
 * pager_dsp.c does all the filtering.
 */

#include "pager_sdr.h"
#include "pager_dsp.h"
#include "multimon_bridge.h"
#include "librtlsdr_andro.h"

#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define TAG "PAGER_SDR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/*
 * USB transfer geometry, inherited from rtl_fm / rtl_ais: 16 x 16384 bytes per transfer,
 * 12 transfers in flight. At 1.4112 MS/s (2.8224 MB/s) that is one callback about every
 * 93 ms, which keeps the per-callback DSP burst small enough to never starve the USB queue.
 */
#define PAGER_BUF_LENGTH       (16 * 16384)
#define PAGER_ASYNC_BUF_NUMBER 12

/* ---- Session state ------------------------------------------------------------------ */
/*
 * Single-session by design: one dongle, one decode loop.
 *
 * TWO locks, and the split is load-bearing rather than tidy.
 *
 * g_dev_mutex guards g_dev, and is held across the WHOLE of rtlsdr_cancel_async_save() in
 * pager_sdr_stop() as well as across rtlsdr_close() in pager_sdr_run(). It has to be: that
 * cancel polls dev->async_status once a millisecond for up to a second, so snapshotting the
 * handle and releasing the lock -- which is what this did until v1.1.0 -- let the session
 * thread free the struct out from under the polling loop. See the comment at close_dev.
 *
 * g_running is an atomic instead of something g_dev_mutex protects, because
 * pager_sdr_is_running() is reached from Kotlin on the main thread and must never wait
 * behind a cancel. Folding it into the lock would put a one-second stall on the UI.
 *
 * Everything else that crosses a thread boundary is atomic rather than volatile. volatile
 * is not a memory barrier in C: it stops the compiler caching the value in a register and
 * says nothing about the store ever becoming visible to another core.
 */
static pthread_mutex_t g_dev_mutex = PTHREAD_MUTEX_INITIALIZER;
static rtlsdr_dev_t *g_dev = NULL;
static _Atomic int g_running = 0;
static _Atomic int g_stop_requested = 0;

/* Written by the USB callback thread, read by the same thread. */
static uint64_t g_total_samples = 0;
static double g_next_stat_time = 0.0;
static int g_peak_mag = 0;
static uint64_t g_ring_drops = 0;

/* Written by the session thread (STARTING/GRACE/STOPPED), by the USB callback thread
 * (STARTED) and by isNativeRunning() on whatever thread Kotlin polls from. */
static _Atomic int g_dev_state = PAGER_DEV_STOPPED;

/* Demodulator thread: drains the ring the USB callback fills. */
static pthread_t g_demod_thread;
static _Atomic int g_demod_thread_valid = 0;
static _Atomic int g_demod_exit = 0;

static double monotonic_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void set_dev_state(int state)
{
    /*
     * One atomic exchange, not a read followed by a write. Three different threads reach
     * this (see g_dev_state above), and with a plain int the de-duplication below is a
     * read-modify-write race in which the losing thread's edge simply vanishes -- which for
     * PAGER_DEV_STARTED means a running receiver the UI never shows as started.
     */
    int previous = atomic_exchange_explicit(&g_dev_state, state, memory_order_relaxed);
    if (previous == state)
        return;   /* de-duplicated: the UI only needs edges, not a 10 Hz repeat */
    static const char *names[] = { "STOPPED", "STARTING", "GRACE", "STARTED" };
    if (state >= 0 && state <= 3)
        LOGI("device state -> %s", names[state]);
    announce_device_stat(state);
}

/**
 * Peak IQ magnitude -> dBFS.
 *
 * Full scale for a CU8 dongle is 127 counts from centre. Reported as a peak rather than an
 * RMS because the user-facing question is "am I clipping or am I in the noise", and a peak
 * answers that with no windowing choices to explain.
 */
static int peak_to_dbfs(int peak)
{
    if (peak <= 0)
        return -99;
    double dbfs = 20.0 * log10((double)peak / 127.0);
    if (dbfs > 0.0) dbfs = 0.0;
    if (dbfs < -99.0) dbfs = -99.0;
    return (int)lround(dbfs);
}

/* ---- USB async callback ------------------------------------------------------------- */

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
    (void)ctx;

    if (atomic_load_explicit(&g_stop_requested, memory_order_acquire) || !buf || len == 0)
        return;

    if (g_total_samples == 0) {
        LOGI("first IQ block: %u bytes", len);
        set_dev_state(PAGER_DEV_STARTED);
    }
    g_total_samples += len / 2;

    /*
     * Hand the block to the DSP. This copies into a lock-free ring and returns at once --
     * doing the filtering here would stall libusb's transfer resubmission and the dongle
     * would start dropping samples.
     */
    if (pager_dsp_push(buf, len) == 0)
        g_ring_drops++;

    /*
     * Level measurement, strided by 32 bytes (8 IQ pairs): a peak detector does not need
     * every sample, and at 2.8 MB/s a full scan would burn measurable CPU for no extra
     * information.
     */
    for (uint32_t i = 0; i + 1 < len; i += 32) {
        int q = (int)buf[i] - 127;
        int j = (int)buf[i + 1] - 127;
        if (q < 0) q = -q;
        if (j < 0) j = -j;
        int mag = (q > j) ? q : j;
        if (mag > g_peak_mag)
            g_peak_mag = mag;
    }

    double now = monotonic_seconds();
    if (now >= g_next_stat_time) {
        g_next_stat_time = now + 1.0;
        /* The decoder publishes these through atomics, so reading them here costs a
         * relaxed load and cannot block the USB queue. */
        int sync_count = 0;
        int err_ppm = 0;
        ebc_multimon_stats(&sync_count, &err_ppm);
        announce_signal_stat(peak_to_dbfs(g_peak_mag), sync_count, err_ppm);
        g_peak_mag = 0;
        if (g_ring_drops > 0) {
            /* Not silent: dropped IQ means lost messages, and the user is entitled to know
             * the receiver is running faster than this phone can process. */
            LOGW("%llu USB blocks dropped -- the DSP is not keeping up",
                 (unsigned long long)g_ring_drops);
            g_ring_drops = 0;
        }
    }
}

/* ---- Demodulator thread ------------------------------------------------------------- */

/*
 * Drains the ring buffer and runs the DSP.
 *
 * A separate thread, not the USB callback, for one reason: the callback runs inside libusb's
 * event loop, and every microsecond spent there is a microsecond the completed transfer is not
 * resubmitted. The DSP is cheap (a few percent of one core at 1.4 MS/s) but it is not free, and
 * "cheap" is not the same as "safe to do in an interrupt-shaped context".
 *
 * When the ring is empty it sleeps 5 ms rather than spinning. One block is about 23 ms of
 * audio, so a 5 ms poll cannot become the bottleneck, and a busy-wait would cost real battery.
 */
static void *demod_thread_fn(void *arg)
{
    (void)arg;
    LOGI("demodulator thread started");
    while (!atomic_load_explicit(&g_demod_exit, memory_order_acquire)) {
        if (pager_dsp_pump() == 0) {
            /* usleep and not a condition variable: signalling a condvar from the USB
             * callback would put a lock in the one place that must never block. */
            usleep(5000);
        }
    }
    /* Drain whatever is left so a message that finished arriving just before Stop is not
     * thrown away mid-batch. Bounded so a stuck producer cannot keep us here. */
    int drained = 0;
    while (pager_dsp_pump() > 0 && ++drained < 256) {
        /* keep going */
    }
    LOGI("demodulator thread finished (%d trailing blocks drained)", drained);
    return NULL;
}

/* ---- Configuration ------------------------------------------------------------------ */

static int configure_device(rtlsdr_dev_t *dev, const pager_sdr_config_t *cfg)
{
    int r;

    /* ppm first: it rescales the crystal, so setting it after the frequency would leave the
     * tuner on a slightly different channel than requested. */
    if (cfg->ppm != 0) {
        r = rtlsdr_set_freq_correction(dev, cfg->ppm);
        /* -2 means "already at this value", which is not a failure. */
        if (r < 0 && r != -2)
            LOGW("set_freq_correction(%d) failed: %d (continuing)", cfg->ppm, r);
        else
            LOGI("ppm correction = %d", cfg->ppm);
    }

    r = rtlsdr_set_sample_rate(dev, PAGER_RTL_SAMPLE_RATE);
    if (r < 0) {
        LOGE("set_sample_rate(%d) failed: %d", PAGER_RTL_SAMPLE_RATE, r);
        return PAGER_ERR_SET_SAMPLERATE;
    }
    LOGI("sample rate = %d S/s (audio %d x %d)",
         PAGER_RTL_SAMPLE_RATE, PAGER_AUDIO_RATE, PAGER_DECIMATION);

    /*
     * Narrow the tuner's own IF filter where the tuner supports it (R820T/R828D do). This is
     * belt-and-braces ahead of the decimator: it keeps strong adjacent-channel signals out
     * of the ADC rather than relying on digital filtering to remove them after the fact.
     * A failure here is not fatal -- other tuners simply ignore it.
     */
    r = rtlsdr_set_tuner_bandwidth(dev, 200000);
    if (r < 0)
        LOGW("set_tuner_bandwidth failed: %d (tuner may not support it)", r);

    r = rtlsdr_set_center_freq(dev, cfg->frequency_hz);
    if (r < 0) {
        LOGE("set_center_freq(%u) failed: %d", cfg->frequency_hz, r);
        return PAGER_ERR_SET_FREQ;
    }
    LOGI("centre frequency = %u Hz", cfg->frequency_hz);

    if (cfg->gain_tenth_db <= 0) {
        r = rtlsdr_set_tuner_gain_mode(dev, 0);   /* 0 = automatic */
        if (r < 0) {
            LOGE("set_tuner_gain_mode(auto) failed: %d", r);
            return PAGER_ERR_SET_GAIN;
        }
        LOGI("tuner gain = auto");
    } else {
        r = rtlsdr_set_tuner_gain_mode(dev, 1);   /* 1 = manual */
        if (r < 0) {
            LOGE("set_tuner_gain_mode(manual) failed: %d", r);
            return PAGER_ERR_SET_GAIN;
        }
        /* Snap to the nearest gain the tuner actually implements. Passing an unsupported
         * value leaves librtlsdr picking for us, which makes the UI a lie. */
        int count = rtlsdr_get_tuner_gains(dev, NULL);
        if (count > 0) {
            int *gains = malloc((size_t)count * sizeof(int));
            if (gains) {
                if (rtlsdr_get_tuner_gains(dev, gains) == count) {
                    int best = gains[0];
                    int best_delta = abs(gains[0] - cfg->gain_tenth_db);
                    for (int i = 1; i < count; i++) {
                        int delta = abs(gains[i] - cfg->gain_tenth_db);
                        if (delta < best_delta) {
                            best_delta = delta;
                            best = gains[i];
                        }
                    }
                    if (best != cfg->gain_tenth_db)
                        LOGI("requested gain %.1f dB snapped to supported %.1f dB",
                             cfg->gain_tenth_db / 10.0, best / 10.0);
                    r = rtlsdr_set_tuner_gain(dev, best);
                } else {
                    r = rtlsdr_set_tuner_gain(dev, cfg->gain_tenth_db);
                }
                free(gains);
            } else {
                r = rtlsdr_set_tuner_gain(dev, cfg->gain_tenth_db);
            }
        } else {
            r = rtlsdr_set_tuner_gain(dev, cfg->gain_tenth_db);
        }
        if (r < 0) {
            LOGE("set_tuner_gain failed: %d", r);
            return PAGER_ERR_SET_GAIN;
        }
        LOGI("tuner gain = %.1f dB (manual)", cfg->gain_tenth_db / 10.0);
    }

    r = rtlsdr_set_agc_mode(dev, cfg->digital_agc ? 1 : 0);
    if (r < 0)
        LOGW("set_agc_mode(%d) failed: %d (continuing)", cfg->digital_agc, r);
    else
        LOGI("RTL2832U digital AGC = %s", cfg->digital_agc ? "on" : "off");

    /* Bias-T only exists on RTL-SDR Blog V3/V4 hardware; other dongles ignore it. */
    r = rtlsdr_set_bias_tee(dev, cfg->bias_tee ? 1 : 0);
    if (r < 0)
        LOGW("set_bias_tee(%d) failed: %d (dongle may not have one)", cfg->bias_tee, r);
    else
        LOGI("bias-T = %s", cfg->bias_tee ? "ON (4.5 V on SMA)" : "off");

    r = rtlsdr_reset_buffer(dev);
    if (r < 0) {
        LOGE("reset_buffer failed: %d", r);
        return PAGER_ERR_RESET_BUFFER;
    }

    return PAGER_OK;
}

/* ---- Public API --------------------------------------------------------------------- */

int pager_sdr_run(const pager_sdr_config_t *cfg)
{
    if (!cfg || cfg->fd <= 0) {
        LOGE("no USB file descriptor (fd=%d)", cfg ? cfg->fd : -1);
        return PAGER_ERR_BAD_FD;
    }

    /* Claim the single session slot in one step: two concurrent starts must not both get in. */
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&g_running, &expected, 1,
                                                 memory_order_acq_rel, memory_order_acquire)) {
        LOGE("a session is already running");
        return PAGER_ERR_ALREADY;
    }
    atomic_store_explicit(&g_stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&g_dev_state, PAGER_DEV_STOPPED, memory_order_relaxed);
    atomic_store_explicit(&g_demod_exit, 0, memory_order_relaxed);
    atomic_store_explicit(&g_demod_thread_valid, 0, memory_order_relaxed);
    /* Plain ints: no other thread exists yet, and all four are reset before one is created. */
    g_total_samples = 0;
    g_peak_mag = 0;
    g_next_stat_time = 0.0;
    g_ring_drops = 0;

    set_dev_state(PAGER_DEV_STARTING);

    int result = PAGER_OK;
    rtlsdr_dev_t *dev = NULL;

    int r = rtlsdr_open2(&dev, cfg->fd);
    if (r < 0 || dev == NULL) {
        LOGE("rtlsdr_open2(fd=%d) failed: %d", cfg->fd, r);
        /*
         * "Someone still holds the interface" deserves different advice from "this dongle is
         * broken". rtlsdr_open2() collapses every claim failure into EBC_SDR_ERR_CLAIM, so the
         * underlying libusb error is read back separately. The common cause is an immediate
         * re-plug: the kernel has not finished releasing the interface from the previous
         * session.
         */
        result = rtlsdr_last_open_was_busy() ? PAGER_ERR_BUSY : PAGER_ERR_OPEN;
        goto done;
    }

    result = configure_device(dev, cfg);
    if (result != PAGER_OK)
        goto close_dev;

    /* Publish the handle only once the device is fully configured: pager_sdr_stop() uses it
     * to cancel, and cancelling a half-configured device is how you get a wedged dongle. */
    pthread_mutex_lock(&g_dev_mutex);
    g_dev = dev;
    pthread_mutex_unlock(&g_dev_mutex);

    /* A Stop that arrived while we were configuring must be honoured now, before we block
     * for the whole session in read_async. */
    if (atomic_load_explicit(&g_stop_requested, memory_order_acquire)) {
        LOGI("stop requested during startup, not entering the read loop");
        goto unpublish;
    }

    if (pager_dsp_init() < 0) {
        result = PAGER_ERR_DSP_INIT;
        goto unpublish;
    }
    /* Before the thread that feeds it exists: pager_audio_sink() walks the demodulator state
     * this sets up, so the decoder has to be ready before the first block can arrive. */
    ebc_multimon_init(cfg);
    if (pthread_create(&g_demod_thread, NULL, demod_thread_fn, NULL) != 0) {
        LOGE("could not create the demodulator thread");
        ebc_multimon_deinit();
        pager_dsp_deinit();
        result = PAGER_ERR_DSP_INIT;
        goto unpublish;
    }
    atomic_store_explicit(&g_demod_thread_valid, 1, memory_order_release);

    set_dev_state(PAGER_DEV_GRACE);
    LOGI("entering rtlsdr_read_async (%d buffers x %d bytes)",
         PAGER_ASYNC_BUF_NUMBER, PAGER_BUF_LENGTH);

    /* BLOCKS until cancelled, or until libusb gives up on an unplugged device. */
    r = rtlsdr_read_async(dev, rtlsdr_callback, NULL,
                          PAGER_ASYNC_BUF_NUMBER, PAGER_BUF_LENGTH);
    if (r < 0)
        LOGW("rtlsdr_read_async returned %d", r);

    LOGI("read loop finished after %llu samples", (unsigned long long)g_total_samples);

    /*
     * Distinguish "the user stopped us" from "we streamed nothing". The second case is what
     * a wrong or dead dongle looks like, and it deserves its own message rather than a
     * silent return to idle.
     */
    int stopped_by_user = atomic_load_explicit(&g_stop_requested, memory_order_acquire);
    /*
     * The unplug test comes first and beats a pending stop, because an unplug SETS that flag:
     * UsbDetachReceiver calls stopFast() the moment the broadcast lands. Testing the flag
     * first would report every unplug as a clean user stop, which is how an unplug used to
     * reach the user as the generic PAGER_ERR_READ_ASYNC text.
     */
    if (rtlsdr_is_dev_lost(dev)) {
        LOGW("the device was lost -- the dongle was unplugged");
        result = PAGER_ERR_DETACHED;
    } else if (!stopped_by_user && g_total_samples == 0)
        result = PAGER_ERR_NO_SAMPLES;
    else if (!stopped_by_user && r < 0)
        result = PAGER_ERR_READ_ASYNC;

unpublish:
    /*
     * Stop the consumer before the producer's buffers go away. pthread_join and not
     * pthread_cancel: Bionic has no pthread_cancel, so the only way to end a thread is to
     * ask it to leave. The loop polls g_demod_exit every 5 ms, so this returns promptly.
     */
    if (atomic_load_explicit(&g_demod_thread_valid, memory_order_acquire)) {
        atomic_store_explicit(&g_demod_exit, 1, memory_order_release);
        pthread_join(g_demod_thread, NULL);
        atomic_store_explicit(&g_demod_thread_valid, 0, memory_order_relaxed);
    }
    /* Strictly after the join: this walks the same demodulator state the audio sink writes. */
    ebc_multimon_deinit();
    pager_dsp_deinit();

close_dev:
    /*
     * Unpublishing the handle and closing it are ONE critical section, and that is the whole
     * fix for the use-after-free this release exists for.
     *
     * pager_sdr_stop() holds g_dev_mutex for the whole of rtlsdr_cancel_async_save(), which
     * dereferences dev once a millisecond for up to a second. Until v1.1.0 the stop merely
     * snapshotted g_dev under the lock and let go, so the sequence below -- free the struct,
     * and with it libusb_exit()'s mutexes -- could run while that loop was still reading it.
     * It presented as "SIGABRT: pthread_mutex_lock called on a destroyed mutex", which
     * AGENTS.md attributed only to closing the UsbDeviceConnection too early. There was a
     * second, purely native cause, and it fired on every ordinary Stop.
     *
     * No deadlock: the cancel loop only needs the libusb event loop to make progress, and by
     * the time we are here rtlsdr_read_async() has already returned, so async_status is
     * INACTIVE and the loop breaks on its next poll -- a millisecond, not a second.
     *
     * A stop arriving after this point finds g_dev NULL and does nothing, which is correct.
     *
     * Order inside the section matters too: bias-T off before close, or the dongle keeps
     * feeding 4.5 V into the antenna after the session ends. Ignore that result -- if the
     * device is already gone there is nothing to turn off.
     */
    pthread_mutex_lock(&g_dev_mutex);
    g_dev = NULL;
    if (cfg->bias_tee)
        (void)rtlsdr_set_bias_tee(dev, 0);
    rtlsdr_close(dev);
    pthread_mutex_unlock(&g_dev_mutex);
    LOGI("device closed");

done:
    set_dev_state(PAGER_DEV_STOPPED);
    atomic_store_explicit(&g_running, 0, memory_order_release);
    return result;
}

void pager_sdr_stop(int fast)
{
    atomic_store_explicit(&g_stop_requested, 1, memory_order_release);

    /*
     * The lock is held across the cancel, not merely around the read of g_dev.
     *
     * rtlsdr_cancel_async_save() polls dev->async_status every millisecond for up to a
     * second, so a snapshot-then-unlock leaves the session thread free to run rtlsdr_close()
     * -- freeing the struct and destroying libusb's mutexes -- while that loop is still
     * reading it. The matching half of this contract is the close_dev block in
     * pager_sdr_run(); read both before changing either.
     *
     * cancel_async_save[_fast] guard against cancelling a device that is not streaming or has
     * already been lost; the plain rtlsdr_cancel_async does not, and calling it on a lost
     * device is one of the routes into the unplug use-after-free.
     *
     * The non-fast variant additionally polls until libusb reports the transfers idle. Even
     * so, expect roughly 1.8 s before read_async returns: libusb must drain the bulk transfer
     * already in flight. The UI shows "Stopping..." for exactly this reason.
     */
    pthread_mutex_lock(&g_dev_mutex);
    rtlsdr_dev_t *dev = g_dev;
    if (!dev) {
        pthread_mutex_unlock(&g_dev_mutex);
        LOGI("stop: no device open");
        return;
    }

    if (fast) {
        LOGI("stop (fast)");
        rtlsdr_cancel_async_save_fast(dev);
    } else {
        LOGI("stop (waiting for transfers to drain)");
        rtlsdr_cancel_async_save(dev);
    }
    pthread_mutex_unlock(&g_dev_mutex);
}

int pager_sdr_is_running(void)
{
    /* Deliberately lock-free: reached from Kotlin on the main thread, and g_dev_mutex can be
     * held for up to a second by a stop in progress. */
    return atomic_load_explicit(&g_running, memory_order_acquire);
}
