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

using aidl::android::hardware::biometrics::fingerprint::ISession;
using aidl::android::hardware::biometrics::fingerprint::ISessionCallback;

// 定义相关常量
static const char* kDescSession = "android.hardware.biometrics.fingerprint.ISession";
static const char* kDefaultNode = "/sys/kernel/oplus_display/notify_fppress";

// 全局变量
static AIBinder_Class_onTransact gSessionOnTransactOrig = nullptr;
static bool gPressed = false;

// 线程局部存储，用于捕获参数
struct WriteCapture {
    AParcel* inParcel;
    int32_t vals[32];
    int count;
};
static thread_local WriteCapture gTL = {nullptr, {0}, 0};

// 配置参数 (默认值)
struct FpConfig {
    int vendorDown  = 22;
    int vendorUp    = 23;

    int sessionDown = 14;
    int sessionUp   = 22;

    int cbEnable    = 1;     // 0 关闭；1 开启
    int cbCode      = 1001;  // 自定义回调 code
    int cbDownVal   = 1201;  // 1001 的第一个 int 值表示 down
    int cbUpVal     = 1202;  // 1001 的第一个 int 值表示 up
    int cbUseDown   = 1;     // 0 不用 1001 判定 down，1 使用
    char nodePath[PROP_VALUE_MAX] = {0};
} gConfig;

// 加载系统属性配置 (只运行一次)
static void loadConfigOnce() {
    static bool inited = false;
    if (inited) return;
    inited = true;

    char buf[PROP_VALUE_MAX] = {0};

    // 加载各个 int 类型的配置
    auto loadInt = [&](const char* key, int& target) {
        if (__system_property_get(key, buf) > 0) target = atoi(buf);
        memset(buf, 0, sizeof(buf));
    };

    loadInt("persist.vendor.fod.down_code", gConfig.vendorDown);
    loadInt("persist.vendor.fod.up_code",   gConfig.vendorUp);
    loadInt("persist.vendor.fod.session_down_code", gConfig.sessionDown);
    loadInt("persist.vendor.fod.session_up_code",   gConfig.sessionUp);
    loadInt("persist.vendor.fod.cb_enable", gConfig.cbEnable);
    loadInt("persist.vendor.fod.cb_code",   gConfig.cbCode);
    loadInt("persist.vendor.fod.cb_down_val", gConfig.cbDownVal);
    loadInt("persist.vendor.fod.cb_up_val",   gConfig.cbUpVal);
    loadInt("persist.vendor.fod.cb_use_down", gConfig.cbUseDown);

    // 加载节点路径
    if (__system_property_get("persist.vendor.fod.notify_node", buf) > 0) {
        strncpy(gConfig.nodePath, buf, sizeof(gConfig.nodePath) - 1);
    } else {
        strncpy(gConfig.nodePath, kDefaultNode, sizeof(gConfig.nodePath) - 1);
    }
}

// 写入节点
static void writeNode(const char* val) {
    // 确保配置已加载
    if (gConfig.nodePath[0] == '\0') loadConfigOnce();

    int fd = open(gConfig.nodePath, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        // 只保留错误日志
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to open %s: %s", gConfig.nodePath, strerror(errno));
        return;
    }
    
    ssize_t want = strlen(val);
    ssize_t r = write(fd, val, want);
    if (r != want) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "Failed to write %s: %s", gConfig.nodePath, strerror(errno));
    } else {
        // [保留] 仅当状态改变成功写入时，打印一条简短日志，方便确认工作状态
        // 如果想要完全静默，可以注释掉下面这行
        __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "Set Fod: %s (Pressed: %d)", val, gPressed);
    }
    close(fd);
}

// 状态管理辅助函数
static void setPressedState(bool pressed) {
    if (gPressed != pressed) {
        gPressed = pressed;
        writeNode(pressed ? "1" : "0");
    }
}

// Hook: ISession onTransact (Client -> HAL)
static binder_status_t Session_onTransact_hook(AIBinder* binder, transaction_code_t code,
                                               const AParcel* in, AParcel* out) {
    loadConfigOnce(); // 确保配置加载

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
    return gSessionOnTransactOrig ? gSessionOnTransactOrig(binder, code, in, out) : STATUS_OK;
}

// Hook: AIBinder_prepareTransaction (初始化捕获)
extern "C" binder_status_t AIBinder_prepareTransaction(AIBinder* binder, AParcel** in) {
    using PrepareFn = binder_status_t (*)(AIBinder*, AParcel**);
    static auto orig = (PrepareFn)dlsym(RTLD_NEXT, "AIBinder_prepareTransaction");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    binder_status_t st = orig(binder, in);
    // 重置当前线程的捕获状态
    gTL.inParcel = (in ? *in : nullptr);
    gTL.count = 0;
    return st;
}

// Hook: AParcel_writeInt32 (捕获参数)
extern "C" binder_status_t AParcel_writeInt32(AParcel* parcel, int32_t value) {
    using WriteIntFn = binder_status_t (*)(AParcel*, int32_t);
    static auto orig = (WriteIntFn)dlsym(RTLD_NEXT, "AParcel_writeInt32");
    
    if (parcel && parcel == gTL.inParcel) {
        if (gTL.count < 32) {
            gTL.vals[gTL.count++] = value;
        }
    }
    return orig ? orig(parcel, value) : STATUS_UNKNOWN_ERROR;
}

// Hook: AIBinder_transact (HAL -> Client 回调处理)
extern "C" binder_status_t AIBinder_transact(AIBinder* binder, transaction_code_t code,
                                  AParcel** in, AParcel** out, binder_flags_t flags) {
    using TransactFn = binder_status_t (*)(AIBinder*, transaction_code_t, AParcel**, AParcel**, binder_flags_t);
    static auto orig = (TransactFn)dlsym(RTLD_NEXT, "AIBinder_transact");
    if (!orig) return STATUS_UNKNOWN_ERROR;

    loadConfigOnce();

    // 1. 处理 onAcquired
    if (code == ISessionCallback::TRANSACTION_onAcquired) {
        bool matched = false;
        if (gTL.inParcel && in && *in == gTL.inParcel && gTL.count > 0) {
            // 遍历所有捕获的整数寻找匹配
            for (int i = 0; i < gTL.count; ++i) {
                if (gTL.vals[i] == gConfig.vendorDown) {
                    setPressedState(true);
                    matched = true;
                    break;
                } else if (gTL.vals[i] == gConfig.vendorUp) {
                    setPressedState(false);
                    matched = true;
                    break;
                }
            }
            // 如果没直接匹配到，尝试检查前两个值 (兼容逻辑)
            if (!matched && gTL.count >= 2) {
                int32_t vendor = gTL.vals[1];
                if (vendor == gConfig.vendorDown) {
                    setPressedState(true);
                } else if (vendor == gConfig.vendorUp) {
                    setPressedState(false);
                }
            }
        }
    }

    // 2. 处理自定义回调 (gCbCode)
    if (gConfig.cbEnable && code == (transaction_code_t)gConfig.cbCode) {
        int event = (gTL.count > 0) ? gTL.vals[0] : -1;
        
        if (gConfig.cbUseDown && event == gConfig.cbDownVal) {
            setPressedState(true);
        } else if (event == gConfig.cbUpVal) {
            setPressedState(false);
        }
    }

    binder_status_t ret = orig(binder, code, in, out, flags);
    
    // 清理状态
    gTL.inParcel = nullptr;
    gTL.count = 0;

    return ret;
}

// Hook: AIBinder_Class_define (拦截 ISession 定义)
extern "C" AIBinder_Class* AIBinder_Class_define(const char* descriptor,
                                      AIBinder_Class_onCreate onCreate,
                                      AIBinder_Class_onDestroy onDestroy,
                                      AIBinder_Class_onTransact onTransact) {
    using DefineFn = AIBinder_Class* (*)(const char*, AIBinder_Class_onCreate, AIBinder_Class_onDestroy, AIBinder_Class_onTransact);
    static auto orig_define = (DefineFn)dlsym(RTLD_NEXT, "AIBinder_Class_define");
    if (!orig_define) return nullptr;

    if (descriptor && strcmp(descriptor, kDescSession) == 0) {
        gSessionOnTransactOrig = onTransact;
        // 使用我们的 Hook 函数替换原始函数
        return orig_define(descriptor, onCreate, onDestroy, &Session_onTransact_hook);
    }

    return orig_define(descriptor, onCreate, onDestroy, onTransact);
}
