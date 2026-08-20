/* SPDX-License-Identifier: GPL-2.0-only
 *
 * pocsagjni.cpp -- the single JNI boundary between Kotlin and the native POCSAG receiver.
 *
 * Copyright (C) 2026 Christian Ebner / ebcTech
 *
 * Nothing else in the native layer touches JNI, and nothing in Kotlin touches native except
 * through eu.ebctech.pocsag.rtlsdr.NativeBridge. See AGENTS.md, guardrail 2.
 *
 * The thread-safety pattern here is not incidental. Callbacks arrive on the libusb transfer
 * thread, which Java knows nothing about, while initNative/releaseNative run on a Kotlin
 * worker thread. Every callback therefore:
 *   1. attaches the calling thread to the JVM if needed,
 *   2. copies the cached class global-ref under g_jni_mutex and uses the copy outside it,
 *   3. checks for a pending exception after every lookup and every call.
 * Skipping step 2 leaves a window where releaseNative() deletes the global ref between the
 * NULL check and the call.
 */

#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include "pocsag_sdr.h"
}

#define TAG "POCSAG_JNI"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static const char *kBridgeClass = "eu/ebctech/pocsag/rtlsdr/NativeBridge";

static pthread_mutex_t g_jni_mutex = PTHREAD_MUTEX_INITIALIZER;
static JavaVM *g_javaVm = nullptr;
static jint g_javaVersion = JNI_VERSION_1_6;
static jclass g_cls = nullptr;   /* global ref to NativeBridge.class */

/* ---- Thread attach helper ------------------------------------------------------------ */

/**
 * Get a JNIEnv for the calling thread, attaching it if it is native-only.
 *
 * Sets *needDetach when the caller must detach afterwards. Detaching a thread that was
 * already attached would tear down a JNIEnv somebody else still holds.
 */
static bool attachThread(JNIEnv **env, bool *needDetach)
{
    *needDetach = false;
    if (!g_javaVm)
        return false;

    jint res = g_javaVm->GetEnv(reinterpret_cast<void **>(env), g_javaVersion);
    if (res == JNI_EDETACHED) {
        if (g_javaVm->AttachCurrentThread(env, nullptr) != JNI_OK) {
            LOGE("AttachCurrentThread failed");
            return false;
        }
        *needDetach = true;
    } else if (res != JNI_OK) {
        LOGE("GetEnv failed: %d", res);
        return false;
    }
    return *env != nullptr;
}

/** Snapshot the cached class ref. Never dereference g_cls outside the lock. */
static jclass bridgeClassRef()
{
    pthread_mutex_lock(&g_jni_mutex);
    jclass cls = g_cls;
    pthread_mutex_unlock(&g_jni_mutex);
    return cls;
}

static bool clearPendingException(JNIEnv *env, const char *what)
{
    if (!env->ExceptionCheck())
        return false;
    LOGE("pending Java exception after %s", what);
    env->ExceptionDescribe();
    env->ExceptionClear();
    return true;
}

/* ---- Native -> Kotlin callbacks ----------------------------------------------------- */

extern "C" void announce_pocsag_message(const char *json)
{
    if (!json)
        return;
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    bool needDetach = false;
    if (!attachThread(&env, &needDetach))
        return;

    jmethodID mid = env->GetStaticMethodID(cls, "nativeMessageLine", "(Ljava/lang/String;)V");
    if (mid && !clearPendingException(env, "GetStaticMethodID(nativeMessageLine)")) {
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

    if (needDetach)
        g_javaVm->DetachCurrentThread();
}

extern "C" void announce_device_stat(int dev_state)
{
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    bool needDetach = false;
    if (!attachThread(&env, &needDetach))
        return;

    jmethodID mid = env->GetStaticMethodID(cls, "nativeDeviceStat", "(I)V");
    if (mid && !clearPendingException(env, "GetStaticMethodID(nativeDeviceStat)")) {
        env->CallStaticVoidMethod(cls, mid, (jint)dev_state);
        clearPendingException(env, "nativeDeviceStat");
    }

    if (needDetach)
        g_javaVm->DetachCurrentThread();
}

extern "C" void announce_signal_stat(int rssi_dbfs, int sync_count, int err_ppm)
{
    jclass cls = bridgeClassRef();
    if (!cls)
        return;

    JNIEnv *env = nullptr;
    bool needDetach = false;
    if (!attachThread(&env, &needDetach))
        return;

    jmethodID mid = env->GetStaticMethodID(cls, "nativeSignalStat", "(III)V");
    if (mid && !clearPendingException(env, "GetStaticMethodID(nativeSignalStat)")) {
        env->CallStaticVoidMethod(cls, mid, (jint)rssi_dbfs, (jint)sync_count, (jint)err_ppm);
        clearPendingException(env, "nativeSignalStat");
    }

    if (needDetach)
        g_javaVm->DetachCurrentThread();
}

/* ---- Kotlin -> native entry points -------------------------------------------------- */

extern "C" JNIEXPORT jboolean JNICALL
Java_eu_ebctech_pocsag_rtlsdr_NativeBridge_initNative(JNIEnv *env, jobject /*thiz*/)
{
    if (env->GetJavaVM(&g_javaVm) != JNI_OK || !g_javaVm) {
        LOGE("GetJavaVM failed");
        return JNI_FALSE;
    }
    g_javaVersion = env->GetVersion();

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

    /* Install the new ref before deleting the old one. Deleting first would leave a window
     * in which a callback on the USB thread sees a NULL class and drops a message. */
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
Java_eu_ebctech_pocsag_rtlsdr_NativeBridge_releaseNative(JNIEnv *env, jobject /*thiz*/)
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
Java_eu_ebctech_pocsag_rtlsdr_NativeBridge_isNativeRunning(JNIEnv * /*env*/, jobject /*thiz*/)
{
    int running = pocsag_sdr_is_running();
    /* Re-emit the device state so a status poll re-syncs the UI even if a previous
     * announce_device_stat was dropped (for example while g_cls was null). */
    announce_device_stat(running ? POCSAG_DEV_STARTED : POCSAG_DEV_STOPPED);
    return running ? JNI_TRUE : JNI_FALSE;
}

/**
 * Start a session. BLOCKS for the whole session; the caller must be a dedicated thread.
 *
 * Returns 0 on a clean stop, or a negative POCSAG_ERR_* code.
 *
 * Note the gain is an int in tenths of a dB, not the formatted string rtlsdr433 passes: that
 * project needed a string because rtl_433 parses one, and it then had to force Locale.ROOT on
 * the Kotlin side to stop a comma decimal separator reaching atof(). There is no such
 * constraint here, so the locale hazard is designed out.
 */
extern "C" JNIEXPORT jint JNICALL
Java_eu_ebctech_pocsag_rtlsdr_NativeBridge_start(
        JNIEnv *env, jobject /*thiz*/,
        jint fd, jint frequencyHz, jint ppm, jint gainTenthDb, jint digitalAgc, jint biasTee,
        jint errorCorrection, jstring charset, jint decodeMode, jint showPartial,
        jint pruneEmpty)
{
    if (fd <= 0) {
        LOGE("start: USB file descriptor missing (fd=%d)", fd);
        return POCSAG_ERR_BAD_FD;
    }

    const char *cs = nullptr;
    if (charset)
        cs = env->GetStringUTFChars(charset, nullptr);

    pocsag_sdr_config_t cfg;
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

    LOGI("POCSAG_CONFIG: fd=%d freq=%dHz ppm=%d gain=%.1fdB digitalAgc=%d biasT=%d "
         "ec=%d charset=%s mode=%d partial=%d pruneEmpty=%d",
         fd, frequencyHz, ppm, gainTenthDb / 10.0, digitalAgc, biasTee,
         errorCorrection, cfg.charset, decodeMode, showPartial, pruneEmpty);

    jint result = (jint)pocsag_sdr_run(&cfg);

    /* Released only after run() returns: cfg.charset aliases this buffer for the whole
     * session, and multimon's charset table is initialised from it. */
    if (cs)
        env->ReleaseStringUTFChars(charset, cs);

    LOGI("start returning %d", result);
    return result;
}

extern "C" JNIEXPORT void JNICALL
Java_eu_ebctech_pocsag_rtlsdr_NativeBridge_closeNative(JNIEnv * /*env*/, jobject /*thiz*/,
                                                       jboolean fast)
{
    pocsag_sdr_stop(fast == JNI_TRUE ? 1 : 0);
}
