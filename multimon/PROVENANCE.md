# multimon-ng — provenance and local patches

Upstream: <https://github.com/EliasOenal/multimon-ng>
Commit: `de0585926542687155852db502a9d2861e9acf95` ("Bump version to 1.6.0", 2026-07-26)
License: GPL-2.0-or-later (`COPYING`), except `bch.c` / `bch.h`, which upstream released into
the public domain (Unlicense), `cJSON.c` / `cJSON.h` (MIT), and **`demod_flex_next.c`, which is
GPL-3.0-or-later** — see the section below. It is the reason the whole application is
GPL-3.0-or-later.

The POCSAG and FLEX paths are vendored. AIS, ADS-B, ACARS and the other demodulators in the
upstream tree are deliberately absent — see AGENTS.md guardrail 5.

## `demod_flex_next.c` and the licence of the whole app

This one file descends from GNU Radio and carries a different header from everything else here:

> Copyright 2004,2006,2010 Free Software Foundation, Inc.
> Copyright (C) 2015 Craig Shelley (craig@microtron.org.uk)
> FLEX Radio Paging Decoder - Adapted from GNURadio for use with Multimon
> … either version 3, or (at your option) any later version.

`demod_flex.c`, the older FLEX decoder that is not vendored, carries the same header, so there
is no GPL-2 route to FLEX in this tree. Combining it is lawful — every other component here is
"or later" and so upgrades to v3 — but the combined work is then **GPL-3.0-or-later**, and that
is what `NOTICE`, `app/config/libraries/*.json` and the public mirror's `LICENSE` now say.
Removing this file is the only thing that would put the application back on GPL-2.

Nothing in the plan for stage 10 predicted this; it was found by reading the file's header
before vendoring it. Read a licence header before a rebase, not a project's own summary of one.

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
| F1 | An `ebc_flex_emit()` / `ebc_flex_note()` pair and an `announce_pager_message()` declaration, beside `extern int json_mode;` | There is no stdout in an Android service. Two helpers rather than one because the eight output sites are two different kinds of thing — see F2 and F3. `pocsag.c`'s `ebc_emit_json()` is `static`, so the pattern is copied rather than shared; promoting it would have meant a ninth patch to `pocsag.c`. |
| F2 | The `fprintf(stdout, …)` at the end of `flex_next_json_emit()` calls `ebc_flex_emit()` | The one site that emits a decoded page, and so the only FLEX caller of the JNI callback. |
| F3 | The other **seven** `fprintf(stdout, …)` sites call `ebc_flex_note()` | BIW system identity, date, time, timezone and country, an INS instruction word, and per-phase BCH statistics. None is a message; handing them to `MessageRepository` would produce one "unusable decoder output" warning each, several per frame. They go to logcat at verbosity 2 instead. |
| F4 | `demod_name`, `address` and `bitrate` added at the top of `flex_next_json_emit()`, and `timestamp` replaced by `addJsonTimestamp()` | So both protocols answer to the same four names on the wire and `PagerMessageParser` needs no second shape. `timestamp` is *replaced* rather than duplicated because upstream writes a local-time string while the POCSAG path writes epoch milliseconds, and two types under one key is a trap. Upstream's own `capcode` and `baud` stay where they are. |

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
6. Re-apply the four `demod_flex_next.c` patches. **Count the `fprintf(stdout` sites rather
   than trusting the table** — this document predicted four and the file has eight — and
   re-check that every `verbprintf(0, …)` carrying message text is still behind
   `if (!json_mode)`.
7. Re-read `demod_flex_next.c`'s licence header. It is the only GPL-3.0-or-later file here and
   the reason the application is GPL-3.0-or-later; if upstream ever relicenses it, `NOTICE` and
   the two `app/config/libraries/*.json` entries have to follow.
