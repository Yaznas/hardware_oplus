#define LOG_TAG "FpPressShim"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <log/log.h>
#include <fcntl.h>
#include <unistd.h>
#include <dlfcn.h>
#include <string.h>
#include <errno.h>
#include <sys/system_properties.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>

using aidl::android::hardware::biometrics::fingerprint::ISession;
using aidl::android::hardware::biometrics::fingerprint::ISessionCallback;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char* kDescSession = "android.hardware.biometrics.fingerprint.ISession";
static const char* kDefaultNode = "/sys/kernel/oplus_display/notify_fppress";

// ---------------------------------------------------------------------------
// Global state
// ---------------------------------------------------------------------------
static AIBinder_Class_onTransact gSessionOnTransactOrig = nullptr;
static bool gPressed = false;

// Watchdog
static pthread_mutex_t gWatchdogMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gWatchdogCond  = PTHREAD_COND_INITIALIZER;
static bool            gWatchdogArmed = false;

// ---------------------------------------------------------------------------
// Thread-local parcel capture — IDENTICAL to the original, no extra fields
// ---------------------------------------------------------------------------
struct WriteCapture {
    AParcel* inParcel;
    int32_t  vals[32];
    int      count;
};
static thread_local WriteCapture gTL = {nullptr, {0}, 0};

// ---------------------------------------------------------------------------
// Config — IDENTICAL to the original, plus watchdogSec
// ---------------------------------------------------------------------------
struct FpConfig {
    int  vendorDown  = 22;
    int  vendorUp    = 23;
    int  sessionDown = 14;
    int  sessionUp   = 22;
    int  cbEnable    = 1;
    int  cbCode      = 1001;
    int  cbDownVal   = 1201;
    int  cbUpVal     = 1202;
    int  cbUseDown   = 1;
    int  watchdogSec = 1;   // set persist.vendor.fod.watchdog_sec=0 to disable
    char nodePath[PROP_VALUE_MAX] = {0};
} gConfig;

// ---------------------------------------------------------------------------
// Config loader — IDENTICAL to the original, plus watchdogSec
// ---------------------------------------------------------------------------
static void loadConfigOnce() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    char buf[PROP_VALUE_MAX] = {0};

    auto loadInt = [&](const char* key, int& target) {
        if (__system_property_get(key, buf) > 0) target = atoi(buf);
        memset(buf, 0, sizeof(buf));
    };

    loadInt("persist.vendor.fod.down_code",         gConfig.vendorDown);
    loadInt("persist.vendor.fod.up_code",           gConfig.vendorUp);
    loadInt("persist.vendor.fod.session_down_code", gConfig.sessionDown);
    loadInt("persist.vendor.fod.session_up_code",   gConfig.sessionUp);
    loadInt("persist.vendor.fod.cb_enable",         gConfig.cbEnable);
    loadInt("persist.vendor.fod.cb_code",           gConfig.cbCode);
    loadInt("persist.vendor.fod.cb_down_val",       gConfig.cbDownVal);
    loadInt("persist.vendor.fod.cb_up_val",         gConfig.cbUpVal);
    loadInt("persist.vendor.fod.cb_use_down",       gConfig.cbUseDown);
    loadInt("persist.vendor.fod.watchdog_sec",      gConfig.watchdogSec);

    if (__system_property_get("persist.vendor.fod.notify_node", buf) > 0) {
        strncpy(gConfig.nodePath, buf, sizeof(gConfig.nodePath) - 1);
    } else {
        strncpy(gConfig.nodePath, kDefaultNode, sizeof(gConfig.nodePath) - 1);
    }
}

// ---------------------------------------------------------------------------
// Node writer — IDENTICAL to the original
// ---------------------------------------------------------------------------
static void writeNode(const char* val) {
    if (gConfig.nodePath[0] == '\0') loadConfigOnce();

    int fd = open(gConfig.nodePath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Failed to open %s: %s", gConfig.nodePath, strerror(errno));
        return;
    }

    ssize_t want = (ssize_t)strlen(val);
    ssize_t r    = write(fd, val, want);
    if (r != want) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
            "Failed to write %s: %s", gConfig.nodePath, strerror(errno));
    } else {
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
            "Set Fod: %s (Pressed: %d)", val, gPressed);
    }
    close(fd);
}

// ---------------------------------------------------------------------------
// Watchdog thread
// One permanent thread, armed on finger-down, disarmed on finger-up.
// ---------------------------------------------------------------------------
static void* watchdogThread(void*) {
    while (true) {
        pthread_mutex_lock(&gWatchdogMutex);

        while (!gWatchdogArmed)
            pthread_cond_wait(&gWatchdogCond, &gWatchdogMutex);

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += gConfig.watchdogSec;

        int rc = pthread_cond_timedwait(&gWatchdogCond, &gWatchdogMutex, &ts);

        if (rc == ETIMEDOUT && gPressed) {
            __android_log_print(ANDROID_LOG_WARN, LOG_TAG,
                "Watchdog: forcing FOD off after %ds", gConfig.watchdogSec);
            gPressed = false;
            writeNode("0");
        }

        gWatchdogArmed = false;
        pthread_mutex_unlock(&gWatchdogMutex);
    }
    return nullptr;
}

static void startWatchdogOnce() {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, []() {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&t, &attr, watchdogThread, nullptr);
        pthread_attr_destroy(&attr);
    });
}

static void armWatchdog() {
    if (gConfig.watchdogSec <= 0) return;
    startWatchdogOnce();
    pthread_mutex_lock(&gWatchdogMutex);
    gWatchdogArmed = true;
    pthread_cond_signal(&gWatchdogCond);
    pthread_mutex_unlock(&gWatchdogMutex);
}

static void disarmWatchdog() {
    if (gConfig.watchdogSec <= 0) return;
    pthread_mutex_lock(&gWatchdogMutex);
    gWatchdogArmed = false;
    pthread_cond_signal(&gWatchdogCond);
    pthread_mutex_unlock(&gWatchdogMutex);
}

// ---------------------------------------------------------------------------
// State manager — same guard as original, plus watchdog arm/disarm
// ---------------------------------------------------------------------------
static void setPressedState(bool pressed) {
    if (gPressed != pressed) {
        gPressed = pressed;
        writeNode(pressed ? "1" : "0");
        if (pressed) armWatchdog();
        else         disarmWatchdog();
    }
}

// ---------------------------------------------------------------------------
// Hook: ISession::onTransact — IDENTICAL to the original
// ---------------------------------------------------------------------------
static binder_status_t Session_onTransact_hook(AIBinder* binder,
                                               transaction_code_t code,
                                               const AParcel* in,
                                               AParcel* out) {
    loadConfigOnce();

    if (gConfig.sessionDown >= 0 && code == (transaction_code_t)gConfig.sessionDown) {
        setPressedState(true);
    } else if (gConfig.sessionUp >= 0 && code == (transaction_code_t)gConfig.sessionUp) {
        setPressedState(false);
    } else {
        if (code == ISession::TRANSACTION_onPointerDown) {
            setPressedState(true);
        } else if (code == ISession::TRANSACTION_onPointerUp) {
            setPressedState(false);
        }
    }

    return gSessionOnTransactOrig
        ? gSessionOnTransactOrig(binder, code, in, out)
        : STATUS_OK;
}

// ---------------------------------------------------------------------------
// Hook: AIBinder_prepareTransaction — IDENTICAL to the original
// ---------------------------------------------------------------------------
extern "C" binder_status_t AIBinder_prepareTransaction(AIBinder* binder, AParcel** in) {
    using PrepareFn = binder_status_t (*)(AIBinder*, AParcel**);
    static auto orig = (PrepareFn)dlsym(RTLD_NEXT, "AIBinder_prepareTransaction");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    binder_status_t st = orig(binder, in);
    gTL.inParcel = (in ? *in : nullptr);
    gTL.count    = 0;
    return st;
}

// ---------------------------------------------------------------------------
// Hook: AParcel_writeInt32 — IDENTICAL to the original
// ---------------------------------------------------------------------------
extern "C" binder_status_t AParcel_writeInt32(AParcel* parcel, int32_t value) {
    using WriteIntFn = binder_status_t (*)(AParcel*, int32_t);
    static auto orig = (WriteIntFn)dlsym(RTLD_NEXT, "AParcel_writeInt32");

    if (parcel && parcel == gTL.inParcel && gTL.count < 32) {
        gTL.vals[gTL.count++] = value;
    }

    return orig ? orig(parcel, value) : STATUS_UNKNOWN_ERROR;
}

// ---------------------------------------------------------------------------
// Hook: AIBinder_transact
//
// Only change from original: the onAcquired path now uses a single symmetric
// loop for both down and up (Fix 2). The fallback block that only checked
// vals[1] for the up case has been removed — it was redundant and missed
// up events at other indices.
// ---------------------------------------------------------------------------
extern "C" binder_status_t AIBinder_transact(AIBinder* binder,
                                             transaction_code_t code,
                                             AParcel** in,
                                             AParcel** out,
                                             binder_flags_t flags) {
    using TransactFn = binder_status_t (*)(AIBinder*, transaction_code_t,
                                           AParcel**, AParcel**, binder_flags_t);
    static auto orig = (TransactFn)dlsym(RTLD_NEXT, "AIBinder_transact");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    loadConfigOnce();

    // onAcquired: scan all captured ints for vendor down/up codes
    if (code == ISessionCallback::TRANSACTION_onAcquired
            && gTL.inParcel && in && *in == gTL.inParcel) {
        for (int i = 0; i < gTL.count; ++i) {
            if (gTL.vals[i] == gConfig.vendorDown) {
                setPressedState(true);
                break;
            } else if (gTL.vals[i] == gConfig.vendorUp) {
                setPressedState(false);
                break;
            }
        }
    }

    // Custom callback code
    if (gConfig.cbEnable && code == (transaction_code_t)gConfig.cbCode) {
        int event = (gTL.count > 0) ? gTL.vals[0] : -1;
        if (gConfig.cbUseDown && event == gConfig.cbDownVal) {
            setPressedState(true);
        } else if (event == gConfig.cbUpVal) {
            setPressedState(false);
        }
    }

    binder_status_t ret = orig(binder, code, in, out, flags);

    gTL.inParcel = nullptr;
    gTL.count    = 0;
    return ret;
}

// ---------------------------------------------------------------------------
// Hook: AIBinder_Class_define — IDENTICAL to the original
// ---------------------------------------------------------------------------
extern "C" AIBinder_Class* AIBinder_Class_define(const char*               descriptor,
                                                 AIBinder_Class_onCreate   onCreate,
                                                 AIBinder_Class_onDestroy  onDestroy,
                                                 AIBinder_Class_onTransact onTransact) {
    using DefineFn = AIBinder_Class* (*)(const char*,
                                         AIBinder_Class_onCreate,
                                         AIBinder_Class_onDestroy,
                                         AIBinder_Class_onTransact);
    static auto orig_define = (DefineFn)dlsym(RTLD_NEXT, "AIBinder_Class_define");
    if (!orig_define) return nullptr;

    if (descriptor && strcmp(descriptor, kDescSession) == 0) {
        gSessionOnTransactOrig = onTransact;
        return orig_define(descriptor, onCreate, onDestroy, &Session_onTransact_hook);
    }

    return orig_define(descriptor, onCreate, onDestroy, onTransact);
}
