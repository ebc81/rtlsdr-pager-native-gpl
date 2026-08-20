/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pocsag_sdr.h -- RTL-SDR device lifecycle for the POCSAG receiver.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * The Kotlin side never talks to this directly; pocsagjni.cpp is the only caller.
 */

#ifndef POCSAG_SDR_H
#define POCSAG_SDR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Device state, mirrored to Kotlin as LiveMessageStore.DeviceState ---------------- */
enum pocsag_dev_state {
    POCSAG_DEV_STOPPED  = 0,
    POCSAG_DEV_STARTING = 1,
    POCSAG_DEV_GRACE    = 2,   /* streaming started but no samples seen yet */
    POCSAG_DEV_STARTED  = 3,
};

/* ---- Error codes returned by pocsag_sdr_run() ---------------------------------------- */
/* Kept numerically stable: Kotlin's PocsagException maps each to a user-facing message and
 * a suggested fix, so renumbering silently changes what the user is told. */
#define POCSAG_OK                    0
#define POCSAG_ERR_BAD_FD         (-1)
#define POCSAG_ERR_OPEN           (-2)
#define POCSAG_ERR_SET_SAMPLERATE (-3)
#define POCSAG_ERR_SET_FREQ       (-4)
#define POCSAG_ERR_SET_GAIN       (-5)
#define POCSAG_ERR_RESET_BUFFER   (-6)
#define POCSAG_ERR_READ_ASYNC     (-7)
#define POCSAG_ERR_ALREADY        (-8)
#define POCSAG_ERR_NO_SAMPLES     (-9)
#define POCSAG_ERR_DSP_INIT       (-10)

/**
 * Everything the Kotlin layer can configure about a session.
 *
 * There is deliberately no sample rate here: it is pinned to POCSAG_RTL_SAMPLE_RATE so the
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
} pocsag_sdr_config_t;

/** 22050 x 64. See AGENTS.md before touching either number. */
#define POCSAG_AUDIO_RATE        22050
#define POCSAG_DECIMATION        64
#define POCSAG_RTL_SAMPLE_RATE   (POCSAG_AUDIO_RATE * POCSAG_DECIMATION)  /* 1411200 */

/**
 * Open the device, configure it and stream until stopped. BLOCKS for the whole session.
 *
 * Returns POCSAG_OK on a clean stop, or a negative POCSAG_ERR_* code.
 */
int pocsag_sdr_run(const pocsag_sdr_config_t *cfg);

/** Ask a running session to stop. Safe to call from any thread, and when not running. */
void pocsag_sdr_stop(int fast);

/** Non-zero while pocsag_sdr_run() has not yet returned. */
int pocsag_sdr_is_running(void);

/* ---- Implemented in pocsagjni.cpp -------------------------------------------------- */
/* Declared here rather than in a JNI header so the C sources need no JNI types. */

/** One decoded page, already serialised to JSON. */
void announce_pocsag_message(const char *json);

/** Device state changed; argument is an enum pocsag_dev_state. */
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

#endif /* POCSAG_SDR_H */
