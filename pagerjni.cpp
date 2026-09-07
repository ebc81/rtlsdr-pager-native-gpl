/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pagerjni.cpp -- the single JNI boundary between Kotlin and the native pager receiver.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * Nothing else in the native layer touches JNI, and nothing in Kotlin touches native except
 * through eu.ebctech.pagerdecoder.rtlsdr.NativeBridge. See AGENTS.md, guardrail 2.
 *
 * The thread-safety pattern here is not incidental. Callbacks arrive on the libusb transfer
 * thread, which Java knows nothing about, while initNative/releaseNative run on a Kotlin
 * worker thread. Every callback therefore:
 *   1. attaches the calling thread to the JVM ONCE and leaves it attached (see attachThread),
 *   2. copies the cached class global-ref under g_jni_mutex and uses the copy outside it,
 *   3. checks for a pending exception after every lookup and every call.
 * Skipping step 2 leaves a window where releaseNative() deletes the global ref between the
 * NULL check and the call.
 *
 * Step 1 changed at v1.1.0. It used to attach and detach around every single callback, which
 * on a busy channel is a JVM round-trip per decoded page plus one a second for the signal
 * stats, all on the thread that must keep the USB transfer queue fed. A thread now attaches
 * on first use and a pthread_key destructor detaches it when it dies -- the standard JNI
 * idiom, and the only correct one here, because a native thread that exits while attached
 * leaks its JNIEnv and, on some Android versions, aborts the process.
 */

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "pager_sdr.h"
}

#define TAG "PAGER_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const char *kBridgeClass = "eu/ebctech/pagerdecoder/rtlsdr/NativeBridge";

static pthread_mutex_t g_jni_mutex = PTHREAD_MUTEX_INITIALIZER;
static JavaVM *g_javaVm = nullptr;
static jint g_javaVersion = JNI_VERSION_1_6;
static jclass g_cls = nullptr;   /* global ref to NativeBridge.class */

/* ---- Thread attach helper ------------------------------------------------------------ */

static bool clearPendingException(JNIEnv *env, const char *what)
{
    if (!env->ExceptionCheck())
        return false;
    LOGE("pending Java exception after %s", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

/*
 * Threads this file attaches stay attached until they die, and this key is how they get
 * detached when they do.
 *
 * A native thread that exits while still attached to the JVM leaks its JNIEnv, and Android
 * has historically aborted the process for it ("native thread exited without detaching").
 * A pthread key destructor runs on thread exit, which is the only hook a thread created by
 * libusb -- not by us -- gives us.
 */
static pthread_key_t g_detach_key;
static pthread_once_t g_detach_key_once = PTHREAD_ONCE_INIT;

static void detachOnThreadExit(void *value)
{
    (void)value;
    if (g_javaVm)
        g_javaVm->DetachCurrentThread();
}

static void makeDetachKey()
{
    if (pthread_key_create(&g_detach_key, detachOnThreadExit) != 0)
        LOGE("pthread_key_create failed; attached threads will not be detached on exit");
}

/**
 * Get a JNIEnv for the calling thread, attaching it on first use.
 *
 * A thread attached here is NOT detached when the call returns: it is registered with
 * g_detach_key so it is detached when the thread itself exits. Attaching per callback was
 * measurably wasteful on the one thread that must never stall (see the file header), and
 * detaching a thread the JVM owns -- the Kotlin worker calling start() -- would tear down a
 * JNIEnv somebody else still holds, which is why only the attach path registers the key.
 */
static bool attachThread(JNIEnv **env)
{
    if (!g_javaVm)
        return false;

    jint res = g_javaVm->GetEnv(reinterpret_cast<void **>(env), g_javaVersion);
    if (res == JNI_EDETACHED) {
        if (g_javaVm->AttachCurrentThread(env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return false;
        }
        pthread_once(&g_detach_key_once, makeDetachKey);
        /* Any non-null value will do; only the destructor matters. */
        pthread_setspecific(g_detach_key, (void *)1);
    } else if (res != JNI_OK) {
        LOGE("GetEnv failed: %d", res);
        return false;
    }
    return *env != nullptr;
}

/**
 * Look up one of the static callbacks, leaving no exception pending on failure.
 *
 * The unconditional clearPendingException() is the point. This used to read
 * `if (mid && !clearPendingException(...))`, and && short-circuits: when GetStaticMethodID
 * returned null the NoSuchMethodError it had thrown was never described and never cleared,
 * and the caller went on to make further JNI calls with an exception pending.
 */
static jmethodID staticMethod(JNIEnv *env, jclass cls, const char *name, const char *sig)
{
    jmethodID mid = env->GetStaticMethodID(cls, name, sig);
    if (clearPendingException(env, name) || !mid) {
        LOGE("could not resolve NativeBridge.%s%s", name, sig);
        return nullptr;
    }
    return mid;
}

/** Snapshot the cached class ref. Never dereference g_cls outside the lock. */
static jclass bridgeClassRef()
{
    pthread_mutex_lock(&g_jni_mutex);
    jclass cls = g_cls;
    pthread_mutex_unlock(&g_jni_mutex);
    return cls;
}

/* ---- Native -> Kotlin callbacks ----------------------------------------------------- */

extern "C" void announce_pager_message(const char *json)
{
    if (!json)
        return;
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    if (!attachThread(&env))
        return;

    jmethodID mid = staticMethod(env, cls, "nativeMessageLine", "(Ljava/lang/String;)V");
    if (!mid)
        return;

    jstring s = env->NewStringUTF(json);
    if (s) {
        env->CallStaticVoidMethod(cls, mid, s);
        clearPendingException(env, "nativeMessageLine");
        env->DeleteLocalRef(s);
    } else {
        /* NewStringUTF returns null on OOM or on invalid modified-UTF8. The decoder can
         * emit odd bytes from a corrupted page, so this is reachable, not theoretical. */
        clearPendingException(env, "NewStringUTF");
        LOGW("could not create Java string for a decoded message, dropping it");
    }
}

/*
 * The POCSAG name is the one patched multimon/pocsag.c calls, and AGENTS.md's native naming
 * rule freezes it: renaming it would force a ninth patch to a file PROVENANCE.md promises is
 * byte-identical to upstream apart from the eight recorded ones. So it stays, as a forwarder
 * to the protocol-neutral name the FLEX patch uses. One implementation, two callers.
 */
extern "C" void announce_pocsag_message(const char *json)
{
    announce_pager_message(json);
}

extern "C" void announce_device_stat(int dev_state)
{
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    if (!attachThread(&env))
        return;

    jmethodID mid = staticMethod(env, cls, "nativeDeviceStat", "(I)V");
    if (!mid)
        return;

    env->CallStaticVoidMethod(cls, mid, (jint)dev_state);
    clearPendingException(env, "nativeDeviceStat");
}

extern "C" void announce_signal_stat(int rssi_dbfs, int sync_count, int err_ppm,
                                     int flex_sync_count, int flex_err_ppm)
{
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    if (!attachThread(&env))
        return;

    jmethodID mid = staticMethod(env, cls, "nativeSignalStat", "(IIIII)V");
    if (!mid)
        return;

    env->CallStaticVoidMethod(cls, mid, (jint)rssi_dbfs, (jint)sync_count, (jint)err_ppm,
                              (jint)flex_sync_count, (jint)flex_err_ppm);
    clearPendingException(env, "nativeSignalStat");
}

/* ---- Kotlin -> native entry points -------------------------------------------------- */

extern "C" JNIEXPORT jboolean JNICALL
Java_eu_ebctech_pagerdecoder_rtlsdr_NativeBridge_initNative(JNIEnv *env, jobject /*thiz*/)
{
    if (env->GetJavaVM(&g_javaVm) != JNI_OK || !g_javaVm) {
        LOGE("GetJavaVM failed");
        return JNI_FALSE;
    }
    g_javaVersion = env->GetVersion();

    /*
     * Already initialised? Then do nothing at all -- and that early return is a fix, not an
     * optimisation.
     *
     * kBridgeClass never changes, so a second global ref to the same class buys nothing. What
     * the old code did buy was a use-after-delete: NativeBridge.closeNativeChecked() calls
     * initNative() on EVERY stop, off `lock`, from the service handler thread and from the
     * PagerDetachStop thread. It then deleted the previous ref below -- while a demodulator
     * thread could be sitting between its bridgeClassRef() snapshot and its
     * CallStaticVoidMethod, holding a jclass that had just been freed. CheckJNI aborts the
     * process on that; without CheckJNI the table slot may already have been reused.
     *
     * Returning early leaves releaseNative() as the only deleter, and that runs from
     * SdrWorkerThread's finally -- after pager_sdr_run() has joined the demodulator thread, so
     * no callback can be in flight. Note that snapshotting under g_jni_mutex, which is what
     * bridgeClassRef() does, never closed this: it copies the pointer, it does not keep the
     * referent alive.
     */
    pthread_mutex_lock(&g_jni_mutex);
    bool already = (g_cls != nullptr);
    pthread_mutex_unlock(&g_jni_mutex);
    if (already)
        return JNI_TRUE;

    jclass local = env->FindClass(kBridgeClass);
    if (!local || clearPendingException(env, "FindClass(NativeBridge)")) {
        LOGE("could not find %s", kBridgeClass);
        return JNI_FALSE;
    }

    jclass fresh = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    if (!fresh) {
        LOGE("NewGlobalRef(NativeBridge) failed");
        return JNI_FALSE;
    }

    /*
     * Only reached when g_cls was NULL, so `old` is almost always NULL too -- the swap and the
     * delete stay only to cover two initNative() calls racing each other past the early return
     * above, where the loser's ref would otherwise leak.
     *
     * This comment used to claim that a bad interleaving costs "one leaked jclass global ref,
     * which costs a slot and nothing else". That was wrong, and the reasoning is worth keeping
     * so it is not reintroduced: it accounted only for double deletion. The real hazard was a
     * concurrent DELETE of a ref another thread was still using -- see the early return above.
     * Do not "restore symmetry" by dropping that return and unconditionally re-creating the
     * ref here. */
    pthread_mutex_lock(&g_jni_mutex);
    jclass old = g_cls;
    g_cls = fresh;
    pthread_mutex_unlock(&g_jni_mutex);

    if (old)
        env->DeleteGlobalRef(old);

    LOGI("initNative OK (JNI version 0x%x)", g_javaVersion);
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_eu_ebctech_pagerdecoder_rtlsdr_NativeBridge_releaseNative(JNIEnv *env, jobject /*thiz*/)
{
    pthread_mutex_lock(&g_jni_mutex);
    jclass old = g_cls;
    g_cls = nullptr;
    pthread_mutex_unlock(&g_jni_mutex);

    if (old)
        env->DeleteGlobalRef(old);

    LOGI("releaseNative OK");
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_eu_ebctech_pagerdecoder_rtlsdr_NativeBridge_isNativeRunning(JNIEnv * /*env*/, jobject /*thiz*/)
{
    int running = pager_sdr_is_running();
    /* Re-emit the device state so a status poll re-syncs the UI even if a previous
     * announce_device_stat was dropped (for example while g_cls was null). */
    announce_device_stat(running ? PAGER_DEV_STARTED : PAGER_DEV_STOPPED);
    return running ? JNI_TRUE : JNI_FALSE;
}

/**
 * Start a session. BLOCKS for the whole session; the caller must be a dedicated thread.
 *
 * Returns 0 on a clean stop, or a negative PAGER_ERR_* code.
 *
 * Note the gain is an int in tenths of a dB, not the formatted string rtlsdr433 passes: that
 * project needed a string because rtl_433 parses one, and it then had to force Locale.ROOT on
 * the Kotlin side to stop a comma decimal separator reaching atof(). There is no such
 * constraint here, so the locale hazard is designed out.
 */
extern "C" JNIEXPORT jint JNICALL
Java_eu_ebctech_pagerdecoder_rtlsdr_NativeBridge_start(
        JNIEnv *env, jobject /*thiz*/,
        jint fd, jint frequencyHz, jint ppm, jint gainTenthDb, jint digitalAgc, jint biasTee,
        jint errorCorrection, jstring charset, jint decodeMode, jint showPartial,
        jint pruneEmpty, jint pocsagRateMask, jint flexEnabled)
{
    if (fd <= 0) {
        LOGE("start: USB file descriptor missing (fd=%d)", fd);
        return PAGER_ERR_BAD_FD;
    }

    const char *cs = nullptr;
    if (charset)
        cs = env->GetStringUTFChars(charset, nullptr);

    pager_sdr_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = fd;
    cfg.frequency_hz = (uint32_t)frequencyHz;
    cfg.ppm = ppm;
    cfg.gain_tenth_db = gainTenthDb;
    cfg.digital_agc = digitalAgc;
    cfg.bias_tee = biasTee;
    cfg.error_correction = errorCorrection;
    cfg.charset = cs ? cs : "US";
    cfg.decode_mode = decodeMode;
    cfg.show_partial = showPartial;
    cfg.prune_empty = pruneEmpty;
    cfg.pocsag_rate_mask = pocsagRateMask;
    /* memset above already made this 0; assigning it anyway so a reader of the fill block
     * sees every field the Kotlin side sends, in the order start() takes them. */
    cfg.flex_enabled = flexEnabled ? 1 : 0;

    LOGI("PAGER_CONFIG: fd=%d freq=%dHz ppm=%d gain=%.1fdB digitalAgc=%d biasT=%d "
         "ec=%d charset=%s mode=%d partial=%d pruneEmpty=%d rateMask=0x%x flex=%s",
         fd, frequencyHz, ppm, gainTenthDb / 10.0, digitalAgc, biasTee,
         errorCorrection, cfg.charset, decodeMode, showPartial, pruneEmpty, pocsagRateMask,
         cfg.flex_enabled ? "on" : "off");

    jint result = (jint)pager_sdr_run(&cfg);

    /* Released only after run() returns: cfg.charset aliases this buffer for the whole
     * session, and multimon's charset table is initialised from it. */
    if (cs)
        env->ReleaseStringUTFChars(charset, cs);

    LOGI("start returning %d", result);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_eu_ebctech_pagerdecoder_rtlsdr_NativeBridge_closeNative(JNIEnv * /*env*/, jobject /*thiz*/,
                                                       jboolean fast)
{
    pager_sdr_stop(fast == JNI_TRUE ? 1 : 0);
}
