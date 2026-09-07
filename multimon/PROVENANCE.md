# multimon-ng — provenance and local patches

Upstream: <https://github.com/EliasOenal/multimon-ng>
Commit: `de0585926542687155852db502a9d2861e9acf95` ("Bump version to 1.6.0", 2026-07-26)
License: GPL-2.0-or-later (`COPYING`), except `bch.c` / `bch.h`, which upstream released into
the public domain (Unlicense), `cJSON.c` / `cJSON.h` (MIT), and **`demod_flex_next.c`, which is
GPL-3.0-or-later** — see the section below. It is the reason the **native layer** is
GPL-3.0-or-later. It is *not* a reason the application as a whole is: the Kotlin layer is
proprietary and is not GPL covered. See AGENTS.md, "Legal posture".

The POCSAG and FLEX paths are vendored. AIS, ADS-B, ACARS and the other demodulators in the
upstream tree are deliberately absent — see AGENTS.md guardrail 5.

## `demod_flex_next.c` and the licence of the native layer

This one file descends from GNU Radio and carries a different header from everything else here:

> Copyright 2004,2006,2010 Free Software Foundation, Inc.
> Copyright (C) 2015 Craig Shelley (craig@microtron.org.uk)
> FLEX Radio Paging Decoder - Adapted from GNURadio for use with Multimon
> … either version 3, or (at your option) any later version.

`demod_flex.c`, the older FLEX decoder that is not vendored, carries the same header, so there
is no GPL-2 route to FLEX in this tree. Combining it is lawful — every other component here is
"or later" and so upgrades to v3 — but the combined **native layer** is then
**GPL-3.0-or-later**, and that is what `NOTICE`, `app/config/libraries/*.json` and the public
mirror's `LICENSE` now say. Removing this file is the only thing that would put the native layer
back on GPL-2.

Nothing in the plan for stage 10 predicted this; it was found by reading the file's header
before vendoring it. Read a licence header before a rebase, not a project's own summary of one.

### The v3 header is the correction, not a copy-paste error — do not "fix" it

The obvious suspicion is that this header is a mistake: multimon-ng's `COPYING` says GPLv2, so
why does one file say v3? It was investigated in full (2026-09-06) against upstream git history.
**The GPLv2 header was the error, and upstream already fixed it.** The chain, all primary:

1. **2015-05-25** (`81e6429e`) Craig Shelley adds FLEX to multimon-ng, stamps it with
   multimon-ng's own boilerplate — "either version **2** of the License, or (at your option) any
   later version" — and omits the FSF copyright entirely. That is the actual copy-paste error.
2. **2018** Göran Weinholt, preparing multimon-ng **for Debian**, files upstream issue **#108**,
   "License issues with FLEX decoder": the FSF notice is missing, the original is GNU Radio's
   `gr-pager/lib/flex_sync_impl.cc`, and "if the FSF copyright is restored it should say GPL
   version 3 … there are still large identical comments".
3. **2018-07-27** upstream **PR #110** lands as `dc3fc918`, "Restore FSF notice on the FLEX
   decoder derived from GNU Radio". The diff removes the v2 paragraph, adds "Copyright
   2004,2006,2010 Free Software Foundation, Inc." and adds "either version **3**". Maintainer
   review: "Looks good to me."
4. **GNU Radio relicensed GPL-2.0-or-later → GPL-3.0-or-later in July 2007.** The FSF copyright
   line runs to **2010**, after that switch.
5. **`demod_flex_next.c` did not exist until 2022-06-06** (`Rename FLEX to FLEX_NEXT` /
   `Brought back FLEX from 1.1.9`) — four years after the correction. It has therefore never
   carried anything but the v3 header, and every contribution to it since (2022, 2024, and the
   2026 ARIB STD-43A overhaul) was made under it.

A GPLv2 route is therefore theoretical only: the 2004/2006 GNU Radio flex code predates the
relicense, but building on it means discarding Craig Shelley's C port and every improvement from
2015 to 2026 and rewriting from a module GNU Radio deprecated in 3.7 and deleted in 3.8. Do not
reopen this. Reverting the header would mean distributing GPLv3 code under GPLv2 terms.

It also buys nothing. Once the scope is stated correctly — native layer GPL, Kotlin layer
proprietary — v2 versus v3 changes nothing operationally here: the native source is published
either way, there is no GPL-2.0-**only** code anywhere in the tree to conflict with, and Play
distributes GPLv3 without issue.

## What the FLEX path needed, and what it did not

The groundwork was already here, byte-identical to upstream:

- `bch.c` / `bch.h` already define `bch_flex_correct()` and `bch_flex_next_correct()` (and the
  GSC Golay tables), so nothing had to change there.
- `multimon.h` already declares `demod_flex` and `demod_flex_next`, and already carries the
  `struct Flex *flex` / `struct Flex_Next *flex_next` slots in the `l1` union.
- The demod param is `{"FLEX_NEXT", true, FREQ_SAMP, FILTLEN, …}` — 22050 Hz, float samples,
  FILTLEN 1 — the same audio contract `demod_poc12` uses, so **the DSP needed no change at all**.

Two things this file claimed before the work were wrong, and both were wrong in the direction
of making the job sound smaller than it is:

- **There are eight `fprintf(stdout, …)` sites, not four**, and they are not all the same kind
  of thing. One emits a decoded page; seven emit network housekeeping. See the patch table.
- **`flex_disable_timestamp` does not appear in this file at all.** It belongs to
  `demod_flex.c`, which defines it itself. Nothing had to be added to `../multimon_bridge.c`
  for it — and the linker would have said so either way.

`unixinput.c` is absent too: its host-program duties (configuration globals, logging, output)
are taken over by `../multimon_bridge.c`.

Files are stored with LF line endings, as upstream has them, so `diff` against a fresh clone
shows only the patches below. Everything vendored here is byte-identical to upstream except
`pocsag.c` and `demod_flex_next.c`.

## Patches to `pocsag.c`

Every one is inside `#ifdef __EBCANDROID__`, with the upstream code kept in the `#else`, so a
diff against upstream reads as pure addition and the original behaviour is never lost.

| # | What | Why |
|---|---|---|
| 1 | `ebc_emit_json()` helper, plus `announce_pocsag_message()` and `g_pocsag_baud` declarations | There is no stdout in an Android service. Adds the `bitrate` field while it is there. |
| 2–3 | The four `json_mode` output sites call `ebc_emit_json()` instead of `fprintf(stdout, …)` | The JNI callback is the only route out of the native layer. |
| 4 | `cJSON_Delete()` before the `pocsag_heuristic_pruning` early return | Upstream leaks the object on that path. |
| 5 | One `cJSON_Delete()` at the end of `pocsag_printmessage()` | Upstream shares one object between the numeric/alpha/skyper branches and deletes it in each; in `POCSAG_MODE_AUTO`, or whenever `unsure` is set, two or three of those branches run for the same message and the second delete works on freed memory. Renewing in `ebc_emit_json()` and deleting once here makes ownership single-valued. |
| 6 | "Message too long" goes to `verbprintf` rather than a stdout JSON line | The upstream JSON form is malformed (a raw newline inside a string), and this is a decoder-ate-noise indicator, not a page the user should see. |
| 7 | `pocsag_init_charset()` restores the ISO 646 IRV defaults before applying a national variant | Upstream only ever overwrites `trtab` entries, which is safe for a program that parses argv once. This app can start a second session with a different charset in the same process, and without the reset the previous session's umlauts would survive into it. |
| 8 | Two counters, `g_pocsag_sync_words` / `g_pocsag_sync_bad_words`, incremented on the SYNC path of `do_one_bit()` | The UI needs a codeword error rate. `struct l2_state_pocsag`'s own counters cannot give one: `pocsag_brute_repair()` also runs in the NO_SYNC search — twice per bit, per demodulator — and bumps the same uncorrected-error counter there, so any rate derived from it reads ~100 % on an idle channel. Sampling the sync flag per audio block does not fix it either; a demodulator can lose and regain sync inside one 23 ms block. Counting at the one place that knows it is synced is exact and costs two increments per codeword. |

## Patches to `demod_flex_next.c`

Same convention: each one inside `#ifdef __EBCANDROID__`, upstream kept in the `#else`.

| # | What | Why |
|---|---|---|
| F1 | An `ebc_flex_emit()` / `ebc_flex_note()` pair and an `announce_pager_message()` declaration, beside `extern int json_mode;` (F5 and F6 later added their two declarations to the same block) | There is no stdout in an Android service. Two helpers rather than one because the eight output sites are two different kinds of thing — see F2 and F3. `pocsag.c`'s `ebc_emit_json()` is `static`, so the pattern is copied rather than shared; promoting it would have meant a ninth patch to `pocsag.c`. |
| F2 | The `fprintf(stdout, …)` at the end of `flex_next_json_emit()` calls `ebc_flex_emit()` | The one site that emits a decoded page, and so the only FLEX caller of the JNI callback. |
| F3 | The other **seven** `fprintf(stdout, …)` sites call `ebc_flex_note()` | BIW system identity, date, time, timezone and country, an INS instruction word, and per-phase BCH statistics. None is a message; handing them to `MessageRepository` would produce one "unusable decoder output" warning each, several per frame. They go to logcat at verbosity 2 instead. |
| F4 | `demod_name`, `address` and `bitrate` added at the top of `flex_next_json_emit()`, and `timestamp` replaced by `addJsonTimestamp()` | So both protocols answer to the same four names on the wire and `PagerMessageParser` needs no second shape. `timestamp` is *replaced* rather than duplicated because upstream writes a local-time string while the POCSAG path writes epoch milliseconds, and two types under one key is a trap. Upstream's own `capcode` and `baud` stay where they are. |
| F5 | `report_state()` calls `ebc_flex_stat_sync()` on the transition into `FLEX_STATE_FIW` | The status card's sync-word readout. SYNC1 → FIW means the outer sync word was found, which is what `multimon_bridge.c` counts as one acquisition on `POCSAG_STATE_SYNC_BIT` for POCSAG — and `report_state()` already fires exactly once per state change, so the edge is free. It has to be *inside* the file: `struct Flex_State` lives behind the opaque `l1.flex_next` pointer, so there is nothing for the bridge to probe from outside. |
| F6 | The "Per-phase BCH summary" block calls `ebc_flex_stat_bch()` with `bch_0err`, `bch_1err`, `bch_2err` and `bch_uncorr` | The FLEX codeword error rate, the counterpart of patch 8 above. Upstream already keeps these four per phase and clears them each frame, so this reads numbers computed anyway; their sum is the codewords BCH checked and everything but `bch_0err` needed repair. Placed *outside* the `json_mode` branch: the bridge pins `json_mode` to 1, so a statistic in the other arm would be dead code, and a statistic must not depend on the output format. Like the POCSAG counters these advance only once the decoder is in DATA, so no search through noise pollutes the ratio. |

F5 and F6 exist because FLEX became able to run **without** POCSAG at v1.5.0. Until then at
least one POCSAG demodulator was always running and feeding those two readouts, so FLEX could
contribute nothing and cost nothing — `multimon_bridge.c` said so in a comment. In a FLEX-only
session the same readout would sit at zero while the decoder worked perfectly: a healthy status
dot, a moving level meter and a counter that never moves, which AGENTS.md calls the worst
failure this app has. The two counters are kept separate from the POCSAG pair rather than
pooled; `ebc_multimon_stats()` explains why.

The piped, non-JSON output needs no patch: every one of those `verbprintf(0, …)` calls sits
behind `if (!json_mode)`, and `../multimon_bridge.c` pins `json_mode` to 1. That is worth more
than tidiness — those lines carry decoded page text, and AGENTS.md's legal posture keeps
received content out of the log. **Check it again after a rebase**: a new unguarded
`verbprintf(0, …)` carrying a message body would be a privacy regression, not a cosmetic one.

## Rebasing onto a newer upstream

1. Clone upstream and copy the eleven files listed in AGENTS.md over this directory.
2. Normalise to LF if your Git checkout produced CRLF.
3. Re-apply the eight `pocsag.c` patches above; the anchors are the `json_mode` blocks in
   `pocsag_printmessage()`, the top of `pocsag_init_charset()` and the `pocsag_brute_repair()`
   call in `do_one_bit()`'s `case SYNC`.
4. Check whether `struct l2_state_pocsag.state` still carries the sync flag in bit 6 —
   `../multimon_bridge.c` reads it through `POCSAG_STATE_SYNC_BIT` to count sync acquisitions.
5. Check whether `_verbprintf`, `addJsonTimestamp` or `json_mode` gained siblings that
   `unixinput.c` defines; the link will fail loudly if so, which is the intended outcome.
6. Re-apply the six `demod_flex_next.c` patches. **Count the `fprintf(stdout` sites rather
   than trusting the table** — this document predicted four and the file has eight — and
   re-check that every `verbprintf(0, …)` carrying message text is still behind
   `if (!json_mode)`. The anchors for F5 and F6 are `report_state()` and the "Per-phase BCH
   summary" block at the end of `decode_phase()`; if either moved, the statistics go silent
   without a compile error, and a FLEX-only session then shows a dead readout for a working
   receiver.
7. Re-read `demod_flex_next.c`'s licence header. It is the only GPL-3.0-or-later file here and
   the reason the native layer is GPL-3.0-or-later; if upstream ever relicenses it, `NOTICE` and
   the two `app/config/libraries/*.json` entries have to follow. If it still says v3, that is
   correct and settled — see "The v3 header is the correction" above before touching anything.
