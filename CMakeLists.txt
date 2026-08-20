cmake_minimum_required(VERSION 3.22.1)

project("pocsag")

# =============================================================================
# Native module layout
# =============================================================================
# One flat shared library, libpocsag.so, composed of:
#
#   libusb-andro/       libusb 1.0.23, Android port. Device enumeration is compiled out;
#                       the only way in is libusb_wrap_sys_device() with a file descriptor
#                       handed over from Kotlin. Carries the Android 14/15/16 unplug
#                       use-after-free fixes -- see AGENTS.md for provenance.
#   rtl-sdr/            RTL-SDR Blog fork of librtlsdr (R82XX/E4K/FC0012/13/2580 tuners,
#                       V4/V4L support, resubmit-failure guard for unplug).
#   librtlsdr_andro.c   rtlsdr_open2(dev**, fd): the fd -> rtlsdr_dev_t bridge.
#   pocsag_sdr.c        Device lifecycle: open, configure, rtlsdr_read_async loop, teardown.
#   pocsag_dsp.c        IQ -> FM audio -> POCSAG bits (added in stage 3).
#   multimon/           multimon-ng's POCSAG decoder: pocsag.c, bch.c, the three
#                       demod_poc*.c, cJSON. Upstream EliasOenal/multimon-ng, see
#                       multimon/PROVENANCE.md for the commit and the local patches.
#   multimon_bridge.c   multimon-ng host glue: the globals, logging and output sink that
#                       upstream's unixinput.c would provide.
#   pocsagjni.cpp       JNI entry points and the announce_* callbacks into Kotlin.
#
# Output pipeline (no stdout redirection anywhere):
#   multimon pocsag.c -> announce_pocsag_message() -> NativeBridge.nativeMessageLine()
#                     -> MyService -> LiveMessageStore -> MessageRepository -> Compose
# =============================================================================

set(RTL_SDR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/rtl-sdr)
set(LIBUSB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libusb-andro/libusb)
set(MULTIMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/multimon)

include_directories(
    ${RTL_SDR_DIR}/include
    ${LIBUSB_DIR}
    ${MULTIMON_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# __EBCANDROID__ rather than the NDK's __ANDROID__: every patch this project makes to a
# vendored source is guarded by it, so project changes stay distinguishable from ordinary
# platform-conditional upstream code.
add_definitions(-DRTLSDR=1 -DLIBUSB1=1 -DTHREADS=1 -D__EBCANDROID__=1)

# multimon-ng build switches.
#   CHARSET_UTF8       pocsag.c's national charsets emit UTF-8 rather than Latin-1, which is
#                      what Java strings and Compose want.
#   NO_X11 / NO_SDL3   compile out the scope demodulators; there is no display server here.
#   MAX_VERBOSE_LEVEL  ceiling for the verbprintf() macro. 3 keeps the sync and statistics
#                      messages available at runtime while compiling out levels 4+, which are
#                      per-bit and per-word traces -- at 22050 samples/s x 3 demodulators
#                      those are not merely noisy, they are a real CPU cost.
add_definitions(-DCHARSET_UTF8 -DNO_X11 -DNO_SDL3 -DMAX_VERBOSE_LEVEL=3)

add_library(pocsag SHARED
    pocsagjni.cpp
    pocsag_sdr.c
    pocsag_dsp.c
    multimon_bridge.c
    librtlsdr_andro.c

    # multimon-ng, POCSAG path only. bch.c and not bch_stub.c/BCHCode.c: the stub does no
    # error correction at all. There is deliberately no demod_flex*.c here -- FLEX ships in
    # the same upstream tree and is out of scope (AGENTS.md guardrail 5) -- and no
    # unixinput.c, whose host-program duties multimon_bridge.c takes over.
    ${MULTIMON_DIR}/pocsag.c
    ${MULTIMON_DIR}/bch.c
    ${MULTIMON_DIR}/demod_poc5.c
    ${MULTIMON_DIR}/demod_poc12.c
    ${MULTIMON_DIR}/demod_poc24.c
    ${MULTIMON_DIR}/cJSON.c

    # libusb sources. Only the Linux/usbfs backend is built; there is no udev and no
    # netlink hotplug on Android, and linux_scan_devices() is compiled to a no-op.
    ${LIBUSB_DIR}/core.c
    ${LIBUSB_DIR}/descriptor.c
    ${LIBUSB_DIR}/io.c
    ${LIBUSB_DIR}/sync.c
    ${LIBUSB_DIR}/strerror.c
    ${LIBUSB_DIR}/hotplug.c
    ${LIBUSB_DIR}/os/linux_usbfs.c
    ${LIBUSB_DIR}/os/threads_posix.c
    ${LIBUSB_DIR}/os/linux_netlink.c
    ${LIBUSB_DIR}/os/poll_posix.c

    # rtl-sdr tuner sources. librtlsdr.c is deliberately absent: librtlsdr_andro.c
    # #includes it inline so the wrapper can reach the private struct rtlsdr_dev.
    # Listing it here as well would produce duplicate symbols.
    ${RTL_SDR_DIR}/src/tuner_r82xx.c
    ${RTL_SDR_DIR}/src/tuner_e4k.c
    ${RTL_SDR_DIR}/src/tuner_fc0012.c
    ${RTL_SDR_DIR}/src/tuner_fc0013.c
    ${RTL_SDR_DIR}/src/tuner_fc2580.c
)

find_library(log-lib log)

# Release optimisation is deliberately held at -O1. RTL_SDR_AIS_Driver records that higher
# levels produced random libusb crashes; the DSP is nowhere near the bottleneck at 1.4 MS/s,
# so there is nothing to gain by raising it.
set(CMAKE_C_FLAGS_RELEASE "-O1 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG")

# Strict warnings for OUR files only. The vendored trees are third-party and would drown
# the build in warnings we must not "fix".
set_source_files_properties(
    pocsagjni.cpp
    pocsag_sdr.c
    pocsag_dsp.c
    multimon_bridge.c
    PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Wno-unused-parameter"
)

target_link_libraries(pocsag ${log-lib})
