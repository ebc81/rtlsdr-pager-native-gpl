/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pager_dsp.h -- IQ to pager audio.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * The DSP is derived from rtl_fm (Kyle Keen, GPL-2.0) by way of rtl_ais, as vendored in
 * RTL_SDR_AIS_Driver. See pager_dsp.c for what changed and why.
 */

#ifndef PAGER_DSP_H
#define PAGER_DSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Prepare the DSP for a session. Call before the first pager_dsp_push().
 *
 * Resets all filter history, the DC estimate and the ring buffer, so a second session cannot
 * inherit the first one's state. Returns 0 on success, negative on allocation failure.
 */
int pager_dsp_init(void);

/** Release buffers. Safe to call when not initialised. */
void pager_dsp_deinit(void);

/**
 * Hand one block of raw CU8 IQ from the USB callback to the DSP.
 *
 * Returns immediately: the block is copied into a lock-free ring buffer and processed on the
 * demodulator thread. Never blocks the USB callback, because stalling there stops the transfer
 * queue being resubmitted and the dongle starts dropping samples.
 *
 * Returns the number of bytes accepted; less than len means the ring overflowed.
 */
uint32_t pager_dsp_push(const unsigned char *buf, uint32_t len);

/**
 * Drain and process whatever is buffered. Called in a loop by the demodulator thread.
 *
 * Returns the number of audio samples produced, or 0 if there was not yet a full block to
 * work on.
 */
int pager_dsp_pump(void);

/** Number of IQ byte pairs dropped through ring overflow this session. */
uint64_t pager_dsp_overflow_count(void);

/**
 * Audio sink, implemented by multimon_bridge.c.
 *
 * Receives [len] float samples at PAGER_AUDIO_RATE. The default implementation in
 * pager_dsp.c is a weak no-op, so the DSP still builds and runs with no decoder linked in.
 */
void pager_audio_sink(const float *samples, int len);

#ifdef __cplusplus
}
#endif

#endif /* PAGER_DSP_H */
