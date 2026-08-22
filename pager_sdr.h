/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pager_sdr.h -- RTL-SDR device lifecycle for the pager receiver.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * The Kotlin side never talks to this directly; pagerjni.cpp is the only caller.
 */

#ifndef PAGER_SDR_H
#define PAGER_SDR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Device state, mirrored to Kotlin as LiveMessageStore.DeviceState ---------------- */
enum pager_dev_state {
    PAGER_DEV_STOPPED  = 0,
    PAGER_DEV_STARTING = 1,
    PAGER_DEV_GRACE    = 2,   /* streaming started but no samples seen yet */
    PAGER_DEV_STARTED  = 3,
};

/* ---- Error codes returned by pager_sdr_run() ---------------------------------------- */
/* Kept numerically stable: Kotlin's PagerError maps each to a user-facing message and
 * a suggested fix, so renumbering silently changes what the user is told. */
#define PAGER_OK                    0
#define PAGER_ERR_BAD_FD         (-1)
#define PAGER_ERR_OPEN           (-2)
#define PAGER_ERR_SET_SAMPLERATE (-3)
#define PAGER_ERR_SET_FREQ       (-4)
#define PAGER_ERR_SET_GAIN       (-5)
#define PAGER_ERR_RESET_BUFFER   (-6)
#define PAGER_ERR_READ_ASYNC     (-7)
#define PAGER_ERR_ALREADY        (-8)
#define PAGER_ERR_NO_SAMPLES     (-9)
#define PAGER_ERR_DSP_INIT       (-10)

/**
 * Everything the Kotlin layer can configure about a session.
 *
 * There is deliberately no sample rate here: it is pinned to PAGER_RTL_SAMPLE_RATE so the
 * decimation lands exactly on multimon-ng's FREQ_SAMP. See AGENTS.md.
 */
typedef struct {
    int fd;                 /* file descriptor from Android UsbManager */
    uint32_t frequency_hz;
    int ppm;                /* crystal error correction, -999..999 */
    int gain_tenth_db;      /* tuner gain in tenths of a dB; <= 0 means automatic */
    int digital_agc;        /* RTL2832U digital AGC: 0 off, 1 on */
    int bias_tee;           /* 4.5 V on the SMA connector: 0 off, 1 on */
    int error_correction;   /* BCH bits to repair, 0..2 */
    const char *charset;    /* multimon charset name, e.g. "US" or "DE" */
    int decode_mode;        /* multimon pocsag_mode */
    int show_partial;
    int prune_empty;
    /*
     * Which POCSAG demodulators to run, as a bitmask: bit 0 = 512, bit 1 = 1200, bit 2 = 2400.
     * The bit positions are the g_rate_baud[] / g_par[] indices in multimon_bridge.c and the
     * POCSAG_BITRATES indices on the Kotlin side; all three orderings are one contract.
     *
     * Keeps the pocsag_ prefix per the AGENTS.md native naming rule -- it counts POCSAG
     * demodulators. FLEX (stage 10) gets a flex_rate_mask beside it rather than reusing this.
     */
    int pocsag_rate_mask;
} pager_sdr_config_t;

/** 22050 x 64. See AGENTS.md before touching either number. */
#define PAGER_AUDIO_RATE        22050
#define PAGER_DECIMATION        64
#define PAGER_RTL_SAMPLE_RATE   (PAGER_AUDIO_RATE * PAGER_DECIMATION)  /* 1411200 */

/**
 * Open the device, configure it and stream until stopped. BLOCKS for the whole session.
 *
 * Returns PAGER_OK on a clean stop, or a negative PAGER_ERR_* code.
 */
int pager_sdr_run(const pager_sdr_config_t *cfg);

/** Ask a running session to stop. Safe to call from any thread, and when not running. */
void pager_sdr_stop(int fast);

/** Non-zero while pager_sdr_run() has not yet returned. */
int pager_sdr_is_running(void);

/* ---- Implemented in pagerjni.cpp -------------------------------------------------- */
/* Declared here rather than in a JNI header so the C sources need no JNI types. */

/** One decoded page, already serialised to JSON. */
void announce_pocsag_message(const char *json);

/** Device state changed; argument is an enum pager_dev_state. */
void announce_device_stat(int dev_state);

/**
 * Reception quality, emitted about once a second.
 *
 * rssi_dbfs   RF level of the raw IQ, in dBFS (negative; 0 would be a clipping input)
 * sync_count  POCSAG sync words seen since the session started
 * err_ppm     codeword error rate over the last window, in parts per million
 */
void announce_signal_stat(int rssi_dbfs, int sync_count, int err_ppm);

#ifdef __cplusplus
}
#endif

#endif /* PAGER_SDR_H */
