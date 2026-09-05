# RTL-SDR Pager — native (GPL) components

This repository carries the pager-specific half of the **complete corresponding source code**
for the GPL-licensed native components of the Android application **RTL-SDR Pager** by ebcTech
(Christian Ebner). It exists to satisfy the written offer under the GNU General Public License:
anyone who receives the app binary is entitled to the source of these components.

> **Since v1.3.0 this is GPL-3.0-or-later, not GPL-2.0.** multimon-ng's FLEX decoder,
> `multimon/demod_flex_next.c`, descends from GNU Radio and is licensed GPL-3.0-**or-later**;
> so is the older `demod_flex.c`, so there is no GPL-2 route to FLEX at all. Every other
> component here is GPL-2.0-or-later or looser, so combining them is lawful and the combined
> work is GPL-3.0-or-later. Nothing was relicensed and no component's own terms changed — only
> the terms of the combination. `LICENSE` is the GPL-3 text; releases up to and including
> v1.2.0 were GPL-2.0.

> **Since v1.1.2 the source lives in two repositories, and this one is not complete on its own.**
> The shared SDR base — librtlsdr, the libusb Android port and the file-descriptor bridge — moved
> to its own project, shared with the other EBC radio apps:
>
> **<https://github.com/ebc81/ebc-sdr-native>**, pinned by the app at tag **`v0.3.0`**.
>
> In the app's tree that project sits at `app/src/main/cpp/ebc-sdr-native` as a git submodule. It
> is deliberately **not** copied in here: mirroring it would republish the same GPL code at a
> second place and a second version, which is the drift the shared base exists to end. Together,
> the two repositories are the complete corresponding source; `CMakeLists.txt` here expects the
> other one beside it (see **Building** below).

The app itself decodes POCSAG pager messages at 512, 1200 and 2400 bit/s, and FLEX at 1600,
3200 and 6400 bit/s, from an RTL-SDR USB dongle connected to an Android device over USB-OTG. It
is receive-only, and all decoding happens on the device — there is no server component.

This tree contains **only** the C/C++ layer. The application's Kotlin sources, resources and
build system are not GPL-obligated and are not published here.

## Layout

| Path | What it is |
|---|---|
| `CMakeLists.txt` | builds everything below into a single `libpager.so` |
| `pagerjni.cpp` | the one JNI boundary: entry points from Kotlin, callbacks back into it |
| `pager_sdr.c/.h` | device lifecycle — open from a USB file descriptor, configure, stream, tear down |
| `pager_dsp.c/.h` | IQ → audio: lock-free ring buffer, /64 CIC decimation with droop compensation, FM discriminator, DC blocker |
| `multimon_bridge.c/.h` | the host glue multimon-ng expects from its main program (upstream's `unixinput.c`), plus the demodulator states — three for POCSAG, one for FLEX — and the audio sink |
| `multimon/` | multimon-ng's POCSAG and FLEX decoders — see `multimon/PROVENANCE.md` |
| *(not here)* | librtlsdr, the libusb Android port and `rtlsdr_open2(dev, fd)` live in [`ebc-sdr-native`](https://github.com/ebc81/ebc-sdr-native) @ `v0.3.0`, which `CMakeLists.txt` pulls in with `add_subdirectory(ebc-sdr-native)` |

## Signal path

```
RTL-SDR @ 1 411 200 S/s (= 22050 × 64), CU8 IQ, tuned straight to the channel
  → uint8 → int16
  → 6 × fifth-order CIC decimate-by-two, then a 9-tap droop-compensating FIR   → 22 050 S/s
  → polar_disc_fast() FM discriminator
  → DC blocker                       ← not optional: the demodulators slice at > 0
  → int16 → float
  → POCSAG512 + POCSAG1200 + POCSAG2400 demodulators, and FLEX_NEXT when enabled
  → pocsag.c / demod_flex_next.c → one JSON object per message → JNI → Kotlin
```

1 411 200 S/s is 22050 × 64, and multimon-ng's POCSAG demodulators hard-code `FREQ_SAMP 22050`.
Because 64 is a power of two, the whole rate change is CIC decimation with no resampler and no
rate error at all. All three POCSAG bit rates run in parallel because they coexist on real paging
channels and nothing in the signal says which is in use until it decodes.

`demod_flex_next` declares the same `FREQ_SAMP 22050` and the same `FILTLEN 1`, which is why FLEX
needed no DSP change whatsoever — it is fed the same audio block after the POCSAG loop. It is one
demodulator, not three: it recovers 1600/3200/6400 bit/s and 2- or 4-level FSK from the sync word
itself, so the configuration carries a `flex_enabled` flag rather than a rate mask.

## Licensing

| Component | License |
|---|---|
| multimon-ng (`multimon/`, except as noted) | GPL-2.0-or-later — © 1996 Thomas Sailer, © 2012-2014 Elias Oenal, and contributors |
| **`multimon/demod_flex_next.c`** | **GPL-3.0-or-later** — © 2004, 2006, 2010 Free Software Foundation, Inc.; © 2015 Craig Shelley. Adapted from GNU Radio. This one file is why the combined work is GPL-3.0-or-later |
| `multimon/bch.c`, `multimon/bch.h` | public domain (Unlicense), as released by their author |
| `multimon/cJSON.c`, `multimon/cJSON.h` | MIT — © 2009-2017 Dave Gamble and cJSON contributors |
| librtlsdr and libusb | not in this repository since v1.1.2 — see [`ebc-sdr-native`](https://github.com/ebc81/ebc-sdr-native), which carries their licences and its own provenance notes |
| EBC integration layer (`pagerjni.cpp`, `pager_sdr.c`, `pager_dsp.c`, `multimon_bridge.c`, `CMakeLists.txt`, and the Android patches to the above) | GPL-3.0-or-later — © 2026 Christian Ebner |

`LICENSE` is the GPL-3 text; it was the GPL-2 text up to v1.2.0, and the integration layer was
declared GPL-2.0-only there. Each vendored tree keeps its own `COPYING` where upstream shipped
one — `multimon/COPYING` is still GPL-2, because that is what upstream ships and this mirror does
not rewrite vendored files. The IQ-to-audio DSP in `pager_dsp.c` derives from `rtl_fm` (Kyle
Keen, GPL-2.0) by way of `rtl_ais`.

## Local modifications

Every Android-specific change to a vendored source is wrapped in `#ifdef __EBCANDROID__`, with
the upstream code kept in the `#else`. The define is set by CMake and is deliberately **not**
the NDK's `__ANDROID__`, so this project's patches stay distinguishable from ordinary
platform-conditional upstream code — `grep -rn __EBCANDROID__` lists all of them.

`multimon/PROVENANCE.md` records the exact upstream commit, all eight patches to `pocsag.c` and
the four to `demod_flex_next.c` with the reasoning for each, and how to rebase onto a newer
upstream. Two of the `pocsag.c` patches fix upstream memory bugs that only matter to a
long-running process: a `cJSON` object shared between output branches and deleted in each (a
double free reachable in `POCSAG_MODE_AUTO`), and a leaked print buffer.

The FLEX patches are output routing, and the shape of them is worth knowing before a rebase:
upstream has **eight** `fprintf(stdout, …)` sites in `demod_flex_next.c`, and only one of them —
at the end of `flex_next_json_emit()` — is a decoded page. The other seven are network
housekeeping (BIW system identity, date, time, timezone, country, an INS instruction word,
per-phase BCH statistics) and are routed to the Android log instead. Count them again after a
rebase rather than trusting this paragraph.

## Building

This tree is not standalone — it is compiled as part of the app's Gradle project, by the Android
Gradle Plugin's CMake integration, for `arm64-v8a`, `armeabi-v7a`, `x86` and `x86_64`:

- NDK 29.0.14206865, CMake 4.1.2, `minSdk 29`
- `-D__EBCANDROID__=1`, plus the definitions `ebc-sdr-native` sets for itself
- multimon-ng switches: `-DCHARSET_UTF8 -DNO_X11 -DNO_SDL3 -DMAX_VERBOSE_LEVEL=3`
- Release optimisation is whatever AGP configures. It builds the release variant as
  `RelWithDebInfo`, so the compiler gets `-O2 -g -DNDEBUG`; `CMAKE_C_FLAGS_RELEASE` is never
  consulted. An earlier version of this file claimed `-O1` was held deliberately against "random
  libusb crashes in sibling projects" — that line never took effect in any project that carried
  it, and no measurement was ever made behind the claim. `CMAKE_C_FLAGS_RELWITHDEBINFO` is the
  knob that would actually work, if anyone ever has a reason to turn it.

**To build this tree you need `ebc-sdr-native` beside it.** `CMakeLists.txt` calls
`add_subdirectory(ebc-sdr-native)` and links the static library `ebc_sdr`, so clone
<https://github.com/ebc81/ebc-sdr-native> at tag `v0.3.0` into a directory of that name here
first. Then point a CMake toolchain file at the NDK and pass the definitions above;
`CMakeLists.txt` needs no Gradle-provided variables beyond the standard Android toolchain ones.
Include paths, compile options and linker flags for the SDR base come from that subproject.

## About this mirror

This is a one-way export of `app/src/main/cpp/` from the application's own repository — minus the
`ebc-sdr-native` submodule, which has its own repository — published on every change to that
directory. Pull requests here cannot be merged into the app; if you have a fix for one of the
vendored projects, it belongs upstream:

- multimon-ng — <https://github.com/EliasOenal/multimon-ng>
- rtl-sdr — <https://github.com/osmocom/rtl-sdr> (via [`ebc-sdr-native`](https://github.com/ebc81/ebc-sdr-native))
- libusb — <https://libusb.info> (via [`ebc-sdr-native`](https://github.com/ebc81/ebc-sdr-native))

Issues about the app itself, or about the integration layer in this repository, are welcome here.
