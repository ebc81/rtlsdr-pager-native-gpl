cmake_minimum_required(VERSION 3.22.1)

project("pager")

# =============================================================================
# Native module layout
# =============================================================================
# One flat shared library, libpager.so, composed of:
#
#   ebc-sdr-native/     git submodule, pinned to a tag. The shared SDR base of all three EBC
#                       SDR apps: libusb 1.0.23 (Android port, unplug fixes), rtl-sdr
#                       (osmocom v2.0.3 + the RTL-SDR-Blog features worth keeping) and
#                       rtlsdr_open2(dev**, fd), the fd -> rtlsdr_dev_t bridge. It builds
#                       itself as the static library `ebc_sdr` and brings its own include
#                       paths, compile options and linker flags. Provenance of every local
#                       patch: ebc-sdr-native/PROVENANCE.md.
#   pager_sdr.c        Device lifecycle: open, configure, rtlsdr_read_async loop, teardown.
#   pager_dsp.c        IQ -> FM audio -> demodulator input.
#   multimon/           multimon-ng's POCSAG decoder: pocsag.c, bch.c, the three
#                       demod_poc*.c, cJSON. Upstream EliasOenal/multimon-ng, see
#                       multimon/PROVENANCE.md for the commit and the local patches.
#   multimon_bridge.c   multimon-ng host glue: the globals, logging and output sink that
#                       upstream's unixinput.c would provide.
#   pagerjni.cpp       JNI entry points and the announce_* callbacks into Kotlin.
#
# Output pipeline (no stdout redirection anywhere):
#   multimon pocsag.c -> announce_pocsag_message() -> NativeBridge.nativeMessageLine()
#                     -> MyService -> LiveMessageStore -> MessageRepository -> Compose
#
# Until v1.1.1 this directory carried its own copies of rtl-sdr/ and libusb-andro/ plus
# librtlsdr_andro.c, and listed every one of their sources below. They are gone: three apps
# each maintaining a copy is what the submodule exists to end. A clone needs
# `git clone --recursive`, or `git submodule update --init` afterwards.
# =============================================================================

set(MULTIMON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/multimon)

# The shared base, first: everything below inherits this directory's compile definitions, and
# the library must not. Its own log tag keeps rtl-sdr's and libusb's diagnostics apart from
# ours in logcat.
set(EBC_SDR_LOG_TAG "pager-sdr" CACHE STRING "" FORCE)
add_subdirectory(ebc-sdr-native)

include_directories(
    ${MULTIMON_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# multimon-ng build switches.
#   CHARSET_UTF8       pocsag.c's national charsets emit UTF-8 rather than Latin-1, which is
#                      what Java strings and Compose want.
#   NO_X11 / NO_SDL3   compile out the scope demodulators; there is no display server here.
#   MAX_VERBOSE_LEVEL  ceiling for the verbprintf() macro. 3 keeps the sync and statistics
#                      messages available at runtime while compiling out levels 4+, which are
#                      per-bit and per-word traces -- at 22050 samples/s x 3 demodulators
#                      those are not merely noisy, they are a real CPU cost.
#   __EBCANDROID__     every patch this project makes to a vendored source is guarded by it,
#                      so project changes stay distinguishable from ordinary
#                      platform-conditional upstream code. multimon/pocsag.c relies on it.
#
# Scoped to the target rather than add_definitions(): a directory-scope definition would also
# reach ebc-sdr-native, and the shared library must compile the same way in all three apps.
#
# Dropped in the same move: -DRTLSDR=1 -DLIBUSB1=1 -DTHREADS=1. Those are rtl_433 switches,
# copied here from rtlsdr433 when this project was seeded from it. Nothing in this app or in
# ebc-sdr-native tests them -- checked before removing.

add_library(pager SHARED
    pagerjni.cpp
    pager_sdr.c
    pager_dsp.c
    multimon_bridge.c

    # multimon-ng, the two paging paths. bch.c and not bch_stub.c/BCHCode.c: the stub does no
    # error correction at all, and bch.c is also where bch_flex_next_correct() lives. There is
    # no unixinput.c, whose host-program duties multimon_bridge.c takes over.
    #
    # demod_flex_next.c is the ONLY file here under GPL-3.0-or-later (it descends from GNU
    # Radio); everything else is GPL-2.0-or-later or looser. That is what makes the whole
    # application GPL-3.0-or-later -- see NOTICE and multimon/PROVENANCE.md before removing or
    # replacing it.
    ${MULTIMON_DIR}/pocsag.c
    ${MULTIMON_DIR}/bch.c
    ${MULTIMON_DIR}/demod_poc5.c
    ${MULTIMON_DIR}/demod_poc12.c
    ${MULTIMON_DIR}/demod_poc24.c
    ${MULTIMON_DIR}/demod_flex_next.c
    ${MULTIMON_DIR}/cJSON.c
)

target_compile_definitions(pager PRIVATE
    CHARSET_UTF8
    NO_X11
    NO_SDL3
    MAX_VERBOSE_LEVEL=3
    __EBCANDROID__=1
)

find_library(log-lib log)

# These two lines DO NOT AFFECT THE SHIPPED BUILD, and that is deliberate as of the v1.1.0
# audit -- read this before "fixing" them.
#
# AGP configures the release variant as RelWithDebInfo, not Release, so what actually reaches
# the compiler is CMAKE_C_FLAGS_RELWITHDEBINFO = "-O2 -g -DNDEBUG". CMAKE_C_FLAGS_RELEASE is
# empty in the cache and these set() calls never reach that build type. Verify with:
#   grep -E "^CMAKE_BUILD_TYPE|^CMAKE_C_FLAGS_REL" app/.cxx/RelWithDebInfo/*/*/CMakeCache.txt
# Debug carries no -O flag at all, i.e. -O0.
#
# The -O1 was inherited from RTL_SDR_AIS_Driver ("Keep optimization low to reduce random libusb
# crashes"). Checked at v1.1.1, and the result is the reason -O1 is not being forced here:
#
#   RTL_SDR_AIS_Driver  app/CMakeLists.txt:16  SET(CMAKE_C_FLAGS_RELEASE "-O1 -DNDEBUG")
#                       -> dead for the same reason, same empty _RELEASE cache entry
#   rtlsdr433           rtl433/CMakeLists.txt:92  the equivalent line is COMMENTED OUT
#
# So the project that recorded the crash history has itself been shipping -O2 the whole time,
# and the sibling that looked at the question decided not to lower it at all. Nothing anywhere
# demonstrates that -O1 ever prevented a crash; there is no measurement behind the rule, only a
# line that never ran. Forcing -O1 here now would change what has been in the field for ten
# versions on the strength of that.
#
# If a libusb crash ever does look optimisation-dependent, the knob is
# CMAKE_C_FLAGS_RELWITHDEBINFO -- setting CMAKE_C_FLAGS_RELEASE silently does nothing, which is
# exactly how this went unnoticed in two projects at once.
#
# Note these are set AFTER add_subdirectory(ebc-sdr-native) on purpose, so that even if they
# were ever made live they would not reach the shared library behind the other two apps' backs.
#
# The real gap is elsewhere and is worth more than the flag: this app is tested in DEBUG only,
# so the -O2 configuration that ships has never been run on hardware. Debug is -O0. See the
# release-build item in releases/v1.1.1.md.
set(CMAKE_C_FLAGS_RELEASE "-O1 -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O1 -DNDEBUG")

# Strict warnings for OUR files only. multimon/ is third-party and would drown the build in
# warnings we must not "fix". ebc-sdr-native applies its own set to its own files.
set_source_files_properties(
    pagerjni.cpp
    pager_sdr.c
    pager_dsp.c
    multimon_bridge.c
    PROPERTIES COMPILE_FLAGS "-Wall -Wextra -Wno-unused-parameter"
)

# ebc_sdr brings rtl-sdr, libusb, the bridge, their include paths, -fvisibility=hidden and the
# 16 KB page-alignment linker flags. It also links the NDK log library itself, but ${log-lib}
# stays listed here because this app's own files call __android_log_print directly.
target_link_libraries(pager PRIVATE ebc_sdr ${log-lib})
