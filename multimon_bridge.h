/* SPDX-License-Identifier: GPL-2.0-only
 *
 * multimon_bridge.h -- multimon-ng host glue for the Android build.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * multimon-ng expects a host program (upstream: unixinput.c) to own the decoder's
 * configuration globals, its logging and its output. That file is not compiled here, so this
 * one takes its place. See multimon_bridge.c for the full list of what it must provide.
 */

#ifndef MULTIMON_BRIDGE_H
#define MULTIMON_BRIDGE_H

#include "pager_sdr.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configure the decoder for a session and reset all three demodulators.
 *
 * Call once, on the session thread, before the demodulator thread starts. Never fails: a
 * charset the decoder does not know falls back to US with a warning, because refusing to
 * receive over a bad preference would be a worse outcome than the wrong umlauts.
 */
void ebc_multimon_init(const pager_sdr_config_t *cfg);

/**
 * Tear the decoder down and log the per-bit-rate BCH statistics.
 *
 * Must be called only after the demodulator thread has been joined -- it walks the same
 * demodulator state the audio sink writes.
 */
void ebc_multimon_deinit(void);

/**
 * Reception quality for announce_signal_stat(), safe to call from any thread.
 *
 * [sync_count] counts POCSAG sync-word acquisitions since ebc_multimon_init(); [err_ppm] is
 * the share of codewords BCH could not repair over the last second, in parts per million.
 * Both are published by the demodulator thread through atomics, so a reader never has to
 * touch the decoder's state.
 */
void ebc_multimon_stats(int *sync_count, int *err_ppm);

#ifdef __cplusplus
}
#endif

#endif /* MULTIMON_BRIDGE_H */
