# POCSAG Pager — native (GPL) components

This repository is the **complete corresponding source code** for the GPL-licensed native
components of the Android application **POCSAG Pager** by ebcTech (Christian Ebner). It exists
to satisfy the written offer under the GNU General Public License, version 2: anyone who
receives the app binary is entitled to the source of these components.

The app itself decodes POCSAG pager messages at 512, 1200 and 2400 bit/s from an RTL-SDR USB
dongle connected to an Android device over USB-OTG. It is receive-only, and all decoding happens
on the device — there is no server component.

This tree contains **only** the C/C++ layer. The application's Kotlin sources, resources and
build system are not GPL-obligated and are not published here.

## Layout

| Path | What it is |
|---|---|
| `CMakeLists.txt` | builds everything below into a single `libpocsag.so` |
| `pocsagjni.cpp` | the one JNI boundary: entry points from Kotlin, callbacks back into it |
| `pocsag_sdr.c/.h` | device lifecycle — open from a USB file descriptor, configure, stream, tear down |
| `pocsag_dsp.c/.h` | IQ → audio: lock-free ring buffer, /64 CIC decimation with droop compensation, FM discriminator, DC blocker |
| `multimon_bridge.c/.h` | the host glue multimon-ng expects from its main program (upstream's `unixinput.c`), plus the three demodulator states and the audio sink |
| `librtlsdr_andro.c/.h` | `rtlsdr_open2(dev, fd)`: the Android file-descriptor bridge into librtlsdr |
| `multimon/` | multimon-ng's POCSAG decoder — see `multimon/PROVENANCE.md` |
| `rtl-sdr/` | librtlsdr, RTL-SDR Blog fork, with Android USB and unplug fixes |
| `libusb-andro/` | libusb 1.0.23, Android port: no enumeration, `libusb_wrap_sys_device()` only |

## Signal path

```
RTL-SDR @ 1 411 200 S/s (= 22050 × 64), CU8 IQ, tuned straight to the channel
  → uint8 → int16
  → 6 × fifth-order CIC decimate-by-two, then a 9-tap droop-compensating FIR   → 22 050 S/s
  → polar_disc_fast() FM discriminator
  → DC blocker                       ← not optional: the demodulators slice at > 0
  → int16 → float
  → POCSAG512 + POCSAG1200 + POCSAG2400 demodulators, all three, always
  → pocsag.c → one JSON object per message → JNI → Kotlin
```

1 411 200 S/s is 22050 × 64, and multimon-ng's POCSAG demodulators hard-code `FREQ_SAMP 22050`.
Because 64 is a power of two, the whole rate change is CIC decimation with no resampler and no
rate error at all. All three bit rates run in parallel because they coexist on real paging
channels and nothing in the signal says which is in use until it decodes.

## Licensing

| Component | License |
|---|---|
| multimon-ng (`multimon/`, except as noted) | GPL-2.0-or-later — © 1996 Thomas Sailer, © 2012-2014 Elias Oenal, and contributors |
| `multimon/bch.c`, `multimon/bch.h` | public domain (Unlicense), as released by their author |
| `multimon/cJSON.c`, `multimon/cJSON.h` | MIT — © 2009-2017 Dave Gamble and cJSON contributors |
| librtlsdr (`rtl-sdr/`) | GPL-2.0-only — © 2012-2024 Steve Markgraf, Osmocom, RTL-SDR Blog contributors |
| libusb (`libusb-andro/`) | LGPL-2.1-only — © libusb contributors |
| EBC integration layer (`pocsagjni.cpp`, `pocsag_sdr.c`, `pocsag_dsp.c`, `multimon_bridge.c`, `librtlsdr_andro.c`, `CMakeLists.txt`, and the Android patches to the above) | GPL-2.0-only — © 2026 Christian Ebner |

`LICENSE` is the GPL-2 text. Each vendored tree keeps its own `COPYING` where upstream shipped
one. The IQ-to-audio DSP in `pocsag_dsp.c` derives from `rtl_fm` (Kyle Keen, GPL-2.0) by way of
`rtl_ais`.

## Local modifications

Every Android-specific change to a vendored source is wrapped in `#ifdef __EBCANDROID__`, with
the upstream code kept in the `#else`. The define is set by CMake and is deliberately **not**
the NDK's `__ANDROID__`, so this project's patches stay distinguishable from ordinary
platform-conditional upstream code — `grep -rn __EBCANDROID__` lists all of them.

`multimon/PROVENANCE.md` records the exact upstream commit, all eight patches to `pocsag.c` with
the reasoning for each, and how to rebase onto a newer upstream. Two of those patches fix
upstream memory bugs that only matter to a long-running process: a `cJSON` object shared between
output branches and deleted in each (a double free reachable in `POCSAG_MODE_AUTO`), and a
leaked print buffer.

## Building

This tree is not standalone — it is compiled as part of the app's Gradle project, by the Android
Gradle Plugin's CMake integration, for `arm64-v8a`, `armeabi-v7a`, `x86` and `x86_64`:

- NDK 29.0.14206865, CMake 4.1.2, `minSdk 29`
- `-D__EBCANDROID__=1 -DRTLSDR=1 -DLIBUSB1=1 -DTHREADS=1`
- multimon-ng switches: `-DCHARSET_UTF8 -DNO_X11 -DNO_SDL3 -DMAX_VERBOSE_LEVEL=3`
- Release optimisation is held at `-O1` deliberately; higher levels have produced random libusb
  crashes in sibling projects, and the DSP is nowhere near the bottleneck at 1.4 MS/s.

To build it on its own, point a CMake toolchain file at the NDK and pass those definitions;
`CMakeLists.txt` needs no Gradle-provided variables beyond the standard Android toolchain ones.
Note that `rtl-sdr/src/librtlsdr.c` is intentionally **not** listed as a source:
`librtlsdr_andro.c` `#include`s it inline so the wrapper can reach the private
`struct rtlsdr_dev`.

## About this mirror

This is a one-way export of `app/src/main/cpp/` from the application's own repository, published
on every change to that directory. Pull requests here cannot be merged into the app; if you have
a fix for one of the vendored projects, it belongs upstream:

- multimon-ng — <https://github.com/EliasOenal/multimon-ng>
- rtl-sdr (RTL-SDR Blog fork) — <https://github.com/rtlsdrblog/rtl-sdr-blog>
- libusb — <https://libusb.info>

Issues about the app itself, or about the integration layer in this repository, are welcome here.
