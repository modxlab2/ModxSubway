// ================================================================
// NativeCrashHandler.cpp
//   Signal-based crash handler for SIGSEGV/SIGABRT/SIGBUS/SIGFPE/SIGILL
//
//   On crash:
//     - Writes a native crash report to the log dir
//     - Calls back into Java (CrashHandler.onNativeCrash)
//     - Then re-raises the signal with default handler
//
//   ★ 32bit (armeabi-v7a) + 64bit (arm64-v8a) — দুটোতেই কাজ করে
//   ★ JNI_OnLoad এখানে নেই (Setup.cpp এ already আছে)
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
#include <sys/syscall.h>

#define LOG_TAG "ModXCrashNative"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// ================================================================
//  Cross-architecture gettid() — API 21+ (armeabi-v7a + arm64-v8a)
// ================================================================
static inline pid_t gettid_safe() {
#if defined(__ANDROID__)
    return (pid_t) syscall(__NR_gettid);
#else
    return (pid_t) getpid();
#endif
}

// ================================================================
//  Signals আমরা handle করব
// ================================================================
static const int SIGNALS[]    = { SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL };
static const int SIGNAL_COUNT = sizeof(SIGNALS) / sizeof(SIGNALS[0]);

// ================================================================
//  State
// ================================================================
static struct sigaction  g_oldActions[SIGNAL_COUNT];
static char              g_logDir[512]    = {0};
static JavaVM*           g_jvm            = nullptr;
static jclass            g_crashClass     = nullptr;
static jmethodID         g_onNativeCrash  = nullptr;
static volatile sig_atomic_t g_inHandler  = 0;

// ================================================================
//  Helpers
// ================================================================
static void writeToFd(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = strlen(s);
    ssize_t written = 0;
    while (written < (ssize_t) len) {
        ssize_t r = write(fd, s + written, len - written);
        if (r <= 0) break;
        written += r;
    }
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
    else        LOGI("Native crash log: %s", path);
    return fd;
}

static void writeBacktrace(int fd) {
    writeToFd(fd, "\n=== BACKTRACE (frame pointers) ===\n");

    void* frames[64];
    int n = 0;

    typedef int  (*backtrace_fn)(void**, int);
    typedef void (*backtrace_symbols_fd_fn)(void* const*, int, int);

    backtrace_fn bt = (backtrace_fn) dlsym(RTLD_DEFAULT, "backtrace");
    backtrace_symbols_fd_fn bts =
        (backtrace_symbols_fd_fn) dlsym(RTLD_DEFAULT, "backtrace_symbols_fd");

    if (bt && bts) {
        n = bt(frames, 64);
        if (n > 0) bts(frames, n, fd);
        else       writeToFd(fd, "(no frames)\n");
    } else {
        writeToFd(fd, "(backtrace symbols not available)\n");
    }
}

static void callJavaCallback(const char* info) {
    if (!g_jvm || !g_crashClass || !g_onNativeCrash) return;
    if (g_inHandler) return;

    JNIEnv* env = nullptr;
    bool attached = false;

    jint r = g_jvm->GetEnv((void**) &env, JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    } else if (r != JNI_OK) {
        return;
    }

    if (env) {
        jstring jinfo = env->NewStringUTF(info ? info : "");
        env->CallStaticVoidMethod(g_crashClass, g_onNativeCrash, jinfo);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(jinfo);
    }

    if (attached) g_jvm->DetachCurrentThread();
}

// ================================================================
//  Signal handler
// ================================================================
static void signalHandler(int sig, siginfo_t* info, void* ucontext) {
    // re-entrancy guard
    if (g_inHandler) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    g_inHandler = 1;

    int fd = openCrashFile();

    const char* sigName =
        (sig == SIGSEGV) ? "SIGSEGV" :
        (sig == SIGABRT) ? "SIGABRT" :
        (sig == SIGBUS ) ? "SIGBUS"  :
        (sig == SIGFPE ) ? "SIGFPE"  :
        (sig == SIGILL ) ? "SIGILL"  : "UNKNOWN";

    char buf[512];
    snprintf(buf, sizeof(buf),
             "=== NATIVE SIGNAL %d (%s) ===\n"
             "fault addr: %p\n"
             "si_code: %d\n"
             "pid: %d, tid: %d, uid: %d\n",
             sig, sigName,
             info ? info->si_addr : nullptr,
             info ? info->si_code : 0,
             getpid(), gettid_safe(), getuid());

    LOGI("%s", buf);
    writeToFd(fd, buf);
    writeToFd(fd, "\n");

    // ------------------------------------------------------------
    //  Registers — architecture specific
    // ------------------------------------------------------------
#if defined(__aarch64__)
    // -------- 64-bit ARM (arm64-v8a) --------
    if (ucontext) {
        ucontext_t* uc = (ucontext_t*) ucontext;
        const mcontext_t* mc = &uc->uc_mcontext;
        char regbuf[1024];
        snprintf(regbuf, sizeof(regbuf),
                 "x0=%016lx x1=%016lx x2=%016lx x3=%016lx\n"
                 "x4=%016lx x5=%016lx x6=%016lx x7=%016lx\n"
                 "x8=%016lx x9=%016lx x10=%016lx x11=%016lx\n"
                 "x12=%016lx x13=%016lx x14=%016lx x15=%016lx\n"
                 "x16=%016lx x17=%016lx x18=%016lx x19=%016lx\n"
                 "x20=%016lx x21=%016lx x22=%016lx x23=%016lx\n"
                 "x24=%016lx x25=%016lx x26=%016lx x27=%016lx\n"
                 "x28=%016lx x29(fp)=%016lx x30(lr)=%016lx\n"
                 "pc=%016lx sp=%016lx\n",
                 (unsigned long) mc->regs[0],  (unsigned long) mc->regs[1],
                 (unsigned long) mc->regs[2],  (unsigned long) mc->regs[3],
                 (unsigned long) mc->regs[4],  (unsigned long) mc->regs[5],
                 (unsigned long) mc->regs[6],  (unsigned long) mc->regs[7],
                 (unsigned long) mc->regs[8],  (unsigned long) mc->regs[9],
                 (unsigned long) mc->regs[10], (unsigned long) mc->regs[11],
                 (unsigned long) mc->regs[12], (unsigned long) mc->regs[13],
                 (unsigned long) mc->regs[14], (unsigned long) mc->regs[15],
                 (unsigned long) mc->regs[16], (unsigned long) mc->regs[17],
                 (unsigned long) mc->regs[18], (unsigned long) mc->regs[19],
                 (unsigned long) mc->regs[20], (unsigned long) mc->regs[21],
                 (unsigned long) mc->regs[22], (unsigned long) mc->regs[23],
                 (unsigned long) mc->regs[24], (unsigned long) mc->regs[25],
                 (unsigned long) mc->regs[26], (unsigned long) mc->regs[27],
                 (unsigned long) mc->regs[28],
                 (unsigned long) mc->regs[29], (unsigned long) mc->regs[30],
                 (unsigned long) mc->pc,       (unsigned long) mc->sp);
        writeToFd(fd, regbuf);
    }
#elif defined(__arm__)
    // -------- 32-bit ARM (armeabi-v7a) --------
    if (ucontext) {
        ucontext_t* uc = (ucontext_t*) ucontext;
        const mcontext_t* mc = &uc->uc_mcontext;
        char regbuf[1024];
        snprintf(regbuf, sizeof(regbuf),
                 "r0=%08lx r1=%08lx r2=%08lx r3=%08lx\n"
                 "r4=%08lx r5=%08lx r6=%08lx r7=%08lx\n"
                 "r8=%08lx r9=%08lx r10=%08lx r11(fp)=%08lx\n"
                 "r12(ip)=%08lx sp=%08lx lr=%08lx pc=%08lx\n"
                 "cpsr=%08lx\n",
                 (unsigned long) mc->arm_r0,  (unsigned long) mc->arm_r1,
                 (unsigned long) mc->arm_r2,  (unsigned long) mc->arm_r3,
                 (unsigned long) mc->arm_r4,  (unsigned long) mc->arm_r5,
                 (unsigned long) mc->arm_r6,  (unsigned long) mc->arm_r7,
                 (unsigned long) mc->arm_r8,  (unsigned long) mc->arm_r9,
                 (unsigned long) mc->arm_r10, (unsigned long) mc->arm_fp,
                 (unsigned long) mc->arm_ip,  (unsigned long) mc->arm_sp,
                 (unsigned long) mc->arm_lr,  (unsigned long) mc->arm_pc,
                 (unsigned long) mc->arm_cpsr);
        writeToFd(fd, regbuf);
    }
#endif

    writeBacktrace(fd);

    // Java callback (Java side writes logcat dump)
    char infoBuf[512];
    snprintf(infoBuf, sizeof(infoBuf),
             "signal=%d(%s) fault=%p code=%d pid=%d tid=%d",
             sig, sigName,
             info ? info->si_addr : nullptr,
             info ? info->si_code : 0,
             getpid(), gettid_safe());
    callJavaCallback(infoBuf);

    if (fd >= 0) close(fd);

    // Restore old handler and re-raise so Android's tombstone still gets generated
    for (int i = 0; i < SIGNAL_COUNT; i++) {
        if (SIGNALS[i] == sig) {
            sigaction(sig, &g_oldActions[i], nullptr);
            break;
        }
    }
    raise(sig);

    _exit(139);
}

// ================================================================
//  JNI export — install handler
// ================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_android_support_CrashHandler_nativeInstallCrashHandler(
        JNIEnv* env, jclass clazz, jstring logDir) {

    // ---- JVM pointer capture ----
    env->GetJavaVM(&g_jvm);
    if (!g_jvm) {
        LOGE("Failed to get JavaVM");
        return;
    }

    // ---- Cache CrashHandler class + onNativeCrash method ----
    if (!g_crashClass) {
        jclass local = env->FindClass("com/android/support/CrashHandler");
        if (local) {
            g_crashClass = (jclass) env->NewGlobalRef(local);
            env->DeleteLocalRef(local);
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("CrashHandler class not found");
        }
    }
    if (!g_onNativeCrash && g_crashClass) {
        g_onNativeCrash = env->GetStaticMethodID(
                g_crashClass,
                "onNativeCrash",
                "(Ljava/lang/String;)V");
        if (!g_onNativeCrash) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("onNativeCrash method not found");
        }
    }

    // ---- Save log dir ----
    if (logDir) {
        const char* c = env->GetStringUTFChars(logDir, nullptr);
        if (c) {
            strncpy(g_logDir, c, sizeof(g_logDir) - 1);
            g_logDir[sizeof(g_logDir) - 1] = 0;
            env->ReleaseStringUTFChars(logDir, c);
        }
    }

    // ---- Install handlers ----
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = signalHandler;
    sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);

    for (int i = 0; i < SIGNAL_COUNT; i++) {
        sigaction(SIGNALS[i], &sa, &g_oldActions[i]);
    }

    LOGI("Native crash handlers installed, dir=%s", g_logDir);
}

// ================================================================
//  ★ JNI_OnLoad এখানে নেই — Setup.cpp তে already define করা আছে
//  ★ JVM pointer উপরে nativeInstallCrashHandler() থেকেই পাচ্ছি
// ================================================================