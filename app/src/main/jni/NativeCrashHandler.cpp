// ================================================================
// NativeCrashHandler.cpp
//   Signal-based crash handler for SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL
//
//   On crash:
//     - Writes a native crash report to the log dir
//     - Calls back into Java (CrashHandler.onNativeCrash)
//     - Then re-raises the signal with default handler
// ================================================================

#include <jni.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <android/log.h>
#include <ucontext.h>
#include <dlfcn.h>

#define LOG_TAG "ModXCrashNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

static const int    SIGNALS[]     = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
static const int    SIGNAL_COUNT  = sizeof(SIGNALS) / sizeof(SIGNALS[0]);

static struct sigaction g_oldActions[SIGNAL_COUNT];
static char             g_logDir[512] = {0};
static JavaVM*          g_jvm         = nullptr;
static jclass           g_crashClass  = nullptr;
static jmethodID        g_onNativeCrash = nullptr;
static volatile sig_atomic_t g_inHandler = 0;

// ================================================================
//  Helpers
// ================================================================
static void writeToFd(int fd, const char* s) {
    if (fd < 0 || !s) return;
    write(fd, s, strlen(s));
}

static int openCrashFile() {
    if (g_logDir[0] == 0) return -1;

    char path[768];
    time_t t = time(nullptr);
    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    snprintf(path, sizeof(path),
             "%s/native_signal_%04d%02d%02d_%02d%02d%02d.txt",
             g_logDir,
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) LOGE("Failed to open crash file: %s", path);
    else LOGI("Native crash log: %s", path);
    return fd;
}

static void writeBacktrace(int fd) {
    writeToFd(fd, "\n=== BACKTRACE (frame pointers) ===\n");

    void* frames[64];
    int n = 0;

    // Try Android's backtrace if available
    typedef int (*backtrace_fn)(void**, int);
    backtrace_fn bt = (backtrace_fn)dlsym(RTLD_DEFAULT, "backtrace");
    typedef void (*backtrace_symbols_fd_fn)(void* const*, int, int);
    backtrace_symbols_fd_fn bts = (backtrace_symbols_fd_fn)
            dlsym(RTLD_DEFAULT, "backtrace_symbols_fd");

    if (bt && bts) {
        n = bt(frames, 64);
        if (n > 0) {
            bts(frames, n, fd);
        }
    } else {
        writeToFd(fd, "(backtrace symbols not available)\n");
    }
}

static void callJavaCallback(const char* info) {
    if (!g_jvm || !g_crashClass || !g_onNativeCrash) return;
    if (g_inHandler) return;

    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_jvm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    if (env) {
        jstring jinfo = env->NewStringUTF(info ? info : "");
        env->CallStaticVoidMethod(g_crashClass, g_onNativeCrash, jinfo);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(jinfo);
    }
    if (attached) g_jvm->DetachCurrentThread();
}

static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
    // re-entrancy guard
    if (g_inHandler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_inHandler = 1;

    int fd = openCrashFile();

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "=== NATIVE SIGNAL %d (%s) ===\n"
                     "fault addr: %p\n"
                     "si_code: %d\n"
                     "pid: %d, tid: %d, uid: %d\n",
                     sig,
                     (sig == SIGSEGV) ? "SIGSEGV" :
                     (sig == SIGABRT) ? "SIGABRT" :
                     (sig == SIGBUS)  ? "SIGBUS"  :
                     (sig == SIGFPE)  ? "SIGFPE"  :
                     (sig == SIGILL)  ? "SIGILL"  : "UNKNOWN",
                     info ? info->si_addr : nullptr,
                     info ? info->si_code : 0,
                     getpid(), gettid(), getuid());

    LOGI("%s", buf);
    writeToFd(fd, buf);
    writeToFd(fd, "\n");

    // Print registers on arm64
#if defined(__aarch64__)
    if (ucontext) {
        ucontext_t* uc = (ucontext_t*)ucontext;
        const mcontext_t* mc = &uc->uc_mcontext;
        char regbuf[512];
        snprintf(regbuf, sizeof(regbuf),
                 "x0=%016lx x1=%016lx x2=%016lx x3=%016lx\n"
                 "x4=%016lx x5=%016lx x6=%016lx x7=%016lx\n"
                 "x8=%016lx x9=%016lx x10=%016lx x11=%016lx\n"
                 "x12=%016lx x13=%016lx x14=%016lx x15=%016lx\n"
                 "pc=%016lx sp=%016lx lr=%016lx fp=%016lx\n",
                 mc->regs[0], mc->regs[1], mc->regs[2], mc->regs[3],
                 mc->regs[4], mc->regs[5], mc->regs[6], mc->regs[7],
                 mc->regs[8], mc->regs[9], mc->regs[10], mc->regs[11],
                 mc->regs[12], mc->regs[13], mc->regs[14], mc->regs[15],
                 mc->pc, mc->sp, mc->regs[30], mc->regs[29]);
        writeToFd(fd, regbuf);
        writeToFd(fd, "\n");
    }
#endif

    writeBacktrace(fd);

    // Java callback (writes logcat dump from Java side)
    char infoBuf[512];
    snprintf(infoBuf, sizeof(infoBuf),
             "signal=%d fault=%p code=%d pid=%d tid=%d",
             sig, info ? info->si_addr : nullptr,
             info ? info->si_code : 0, getpid(), gettid());
    callJavaCallback(infoBuf);

    if (fd >= 0) close(fd);

    // Restore old handler and re-raise
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (SIGNALS[i] == sig) {
            sigaction(sig, &g_oldActions[i], nullptr);
            break;
        }
    }
    raise(sig);

    // If we get here, kill process
    _exit(139);
}

// ================================================================
//  JNI export — install handler
// ================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_android_support_CrashHandler_nativeInstallCrashHandler(
        JNIEnv* env, jclass clazz, jstring logDir) {

    // Save JVM
    env->GetJavaVM(&g_jvm);

    // Cache class + method
    if (!g_crashClass) {
        jclass local = env->FindClass("com/android/support/CrashHandler");
        if (local) g_crashClass = (jclass)env->NewGlobalRef(local);
    }
    if (!g_onNativeCrash && g_crashClass) {
        g_onNativeCrash = env->GetStaticMethodID(
                g_crashClass, "onNativeCrash", "(Ljava/lang/String;)V");
    }

    // Save log dir
    if (logDir) {
        const char* c = env->GetStringUTFChars(logDir, nullptr);
        if (c) {
            strncpy(g_logDir, c, sizeof(g_logDir) - 1);
            g_logDir[sizeof(g_logDir) - 1] = 0;
            env->ReleaseStringUTFChars(logDir, c);
        }
    }

    // Install handlers
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signalHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < SIGNAL_COUNT; i++) {
        sigaction(SIGNALS[i], &sa, &g_oldActions[i]);
    }

    LOGI("Native crash handlers installed, dir=%s", g_logDir);
}

// ================================================================
//  JVM onLoad hook — capture JVM pointer even before install
// ================================================================
JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_jvm = vm;
    return JNI_VERSION_1_6;
}