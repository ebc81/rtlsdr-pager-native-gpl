# multimon-ng — provenance and local patches

Upstream: <https://github.com/EliasOenal/multimon-ng>
Commit: `de0585926542687155852db502a9d2861e9acf95` ("Bump version to 1.6.0", 2026-07-26)
License: GPL-2.0-or-later (`COPYING`), except `bch.c` / `bch.h`, which upstream released into
the public domain (Unlicense), and `cJSON.c` / `cJSON.h` (MIT).

Only the POCSAG path is vendored **so far**. AIS, ADS-B, ACARS and the other demodulators in
the upstream tree are deliberately absent — see AGENTS.md guardrail 5.

FLEX is the one exception to that list: it is **in scope**, as stage 10, and
`demod_flex_next.c` is merely not vendored yet. Vendoring it needs no change to any file in
this directory, because the groundwork is already here, byte-identical to upstream:

- `bch.c` / `bch.h` already define `bch_flex_correct()` and `bch_flex_next_correct()` (and the
  GSC Golay tables). They are dead code in the current binary.
- `multimon.h` already declares `demod_flex` and `demod_flex_next`, and already carries the
  `struct Flex *flex` / `struct Flex_Next *flex_next` slots in the `l1` union.
- Upstream's demod param is `{"FLEX", true, FREQ_SAMP, FILTLEN, …}` — 22050 Hz, float samples,
  FILTLEN 1 — which is the same audio contract `demod_poc12` uses, so the DSP needs no change.

What it does need: the four `fprintf(stdout, cJSON_PrintUnformatted(…))` sites patched the way
patches 2–3 patched `pocsag.c`, and `flex_disable_timestamp` defined in `../multimon_bridge.c`.

`unixinput.c` is absent too: its host-program duties (configuration globals, logging, output)
are taken over by `../multimon_bridge.c`.

Files are stored with LF line endings, as upstream has them, so `diff` against a fresh clone
shows only the patches below. Everything vendored here is byte-identical to upstream except
`pocsag.c`.

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

## Rebasing onto a newer upstream

1. Clone upstream and copy the ten files listed in AGENTS.md over this directory.
2. Normalise to LF if your Git checkout produced CRLF.
3. Re-apply the eight patches above; the anchors are the `json_mode` blocks in
   `pocsag_printmessage()`, the top of `pocsag_init_charset()` and the `pocsag_brute_repair()`
   call in `do_one_bit()`'s `case SYNC`.
4. Check whether `struct l2_state_pocsag.state` still carries the sync flag in bit 6 —
   `../multimon_bridge.c` reads it through `POCSAG_STATE_SYNC_BIT` to count sync acquisitions.
5. Check whether `_verbprintf`, `addJsonTimestamp` or `json_mode` gained siblings that
   `unixinput.c` defines; the link will fail loudly if so, which is the intended outcome.
