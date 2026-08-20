/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pocsag_dsp.h -- IQ to POCSAG audio.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * The DSP is derived from rtl_fm (Kyle Keen, GPL-2.0) by way of rtl_ais, as vendored in
 * RTL_SDR_AIS_Driver. See pocsag_dsp.c for what changed and why.
 */

#ifndef POCSAG_DSP_H
#define POCSAG_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Prepare the DSP for a session. Call before the first pocsag_dsp_push().
 *
 * Resets all filter history, the DC estimate and the ring buffer, so a second session cannot
 * inherit the first one's state. Returns 0 on success, negative on allocation failure.
 */
int pocsag_dsp_init(void);

/** Release buffers. Safe to call when not initialised. */
void pocsag_dsp_deinit(void);

/**
 * Hand one block of raw CU8 IQ from the USB callback to the DSP.
 *
 * Returns immediately: the block is copied into a lock-free ring buffer and processed on the
 * demodulator thread. Never blocks the USB callback, because stalling there stops the transfer
 * queue being resubmitted and the dongle starts dropping samples.
 *
 * Returns the number of bytes accepted; less than len means the ring overflowed.
 */
uint32_t pocsag_dsp_push(const unsigned char *buf, uint32_t len);

/**
 * Drain and process whatever is buffered. Called in a loop by the demodulator thread.
 *
 * Returns the number of audio samples produced, or 0 if there was not yet a full block to
 * work on.
 */
int pocsag_dsp_pump(void);

/** Number of IQ byte pairs dropped through ring overflow this session. */
uint64_t pocsag_dsp_overflow_count(void);

/**
 * Audio sink, implemented by multimon_bridge.c.
 *
 * Receives [len] float samples at POCSAG_AUDIO_RATE. The default implementation in
 * pocsag_dsp.c is a weak no-op, so the DSP still builds and runs with no decoder linked in.
 */
void pocsag_audio_sink(const float *samples, int len);

#ifdef __cplusplus
}
#endif

#endif /* POCSAG_DSP_H */
