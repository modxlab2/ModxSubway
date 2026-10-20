#include "Includes.h"
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "Includes/Strings.h"
#include "Includes/Macros.h"
#include "KittyMemory/MemoryPatch.h"
#include "AutoHook/AutoHook.h"

#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <map>
#include <android/log.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <unwind.h>

#include "Chams.h"

#define LIB OBFUSCATE("libil2cpp.so")
#define LOG_TAG "MODXLAB"

// ================================================================
//  BOOT STEP TRACKER
// ================================================================
static std::atomic<int> g_BootStep{0};
static const char* bootStepName(int s) {
    static const char* names[] = {
        "0-constructor", "1-logging", "2-crash-handler", "3-hack-thread",
        "4-lib-loaded", "5-module-base", "6-phase1", "7-phase2a",
        "8-phase2b", "9-phase2c", "10-phase3", "11-render",
        "12-heartbeat", "13-complete"
    };
    return (s >= 0 && s < 14) ? names[s] : "?";
}

// ================================================================
//  FILE HANDLES & PATHS
// ================================================================
static int              g_LogFd      = -1;
static int              g_CrashFd    = -1;
static std::atomic<bool> g_LogReady  {false};
static std::string      g_LogDir;
static std::string      g_PackageName;
static std::string      g_StartupFlagPath;
static std::string      g_SafeModePath;

// ================================================================
//  ASYNC-SAFE STRING HELPERS
// ================================================================
static inline void safeWriteStr(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        if (n <= 0) break;
        s += n; len -= (size_t)n;
    }
}

static int safeItoa(long val, char* buf, int bufsz) {
    if (bufsz < 2) return 0;
    char tmp[24]; int n = 0; bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (val % 10)); val /= 10;
    }
    int k = 0;
    if (neg && k < bufsz - 1) buf[k++] = '-';
    while (n > 0 && k < bufsz - 1) buf[k++] = tmp[--n];
    buf[k] = '\0';
    return k;
}

// ================================================================
//  LOGGER
// ================================================================
static void logWrite(const char* level, const char* fmt, ...) {
    char buf[1024];
    int pos = 0;
    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[%02d:%02d:%02d][%s] ",
                    tmv.tm_hour, tmv.tm_min, tmv.tm_sec, level);
    va_list ap; va_start(ap, fmt);
    pos += vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
    va_end(ap);
    if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
    else { buf[sizeof(buf) - 2] = '\n'; buf[sizeof(buf) - 1] = '\0'; }

    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
    if (g_LogReady.load() && g_LogFd >= 0) {
        safeWriteStr(g_LogFd, buf);
        // NOTE: no fsync — periodic fsync would be added but not on every call
    }
}
#define LOGI(...) logWrite("I", __VA_ARGS__)
#define LOGW(...) logWrite("W", __VA_ARGS__)
#define LOGE(...) logWrite("E", __VA_ARGS__)

// ================================================================
//  PACKAGE / PATH HELPERS
// ================================================================
static std::string getPackageName() {
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) { ssize_t n = read(fd, buf, sizeof(buf) - 1); (void)n; close(fd); }
    std::string s(buf);
    size_t p = s.find('\0'); if (p != std::string::npos) s = s.substr(0, p);
    p = s.find(':'); if (p != std::string::npos) s = s.substr(0, p);
    return s;
}

static bool ensureDir(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(p.c_str(), 0777) == 0;
}

static std::string findLogDir() {
    g_PackageName = getPackageName();
    std::vector<std::string> cands;
    if (!g_PackageName.empty()) {
        cands.push_back("/sdcard/Android/data/" + g_PackageName + "/files/ModXLab");
        cands.push_back("/storage/emulated/0/Android/data/" + g_PackageName + "/files/ModXLab");
    }
    cands.push_back("/sdcard/Documents/ModXLab");
    cands.push_back("/storage/emulated/0/Documents/ModXLab");
    for (const auto& c : cands) {
        if (!ensureDir(c)) continue;
        std::string test = c + "/.w";
        int fd = open(test.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { close(fd); unlink(test.c_str()); return c; }
    }
    return "";
}

// ================================================================
//  MODULE BASE PARSING (via /proc/self/maps)
// ================================================================
static void* findModuleBase(const char* libName) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return nullptr;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, libName)) {
            // Format: "start-end perms offset ... path"
            char* dash = strchr(line, '-');
            if (dash) {
                char* endptr = nullptr;
                unsigned long start = strtoul(line, &endptr, 16);
                fclose(f);
                return (void*)start;
            }
        }
    }
    fclose(f);
    return nullptr;
}

// ================================================================
//  SIGNAL HANDLERS — with chaining
// ================================================================
struct SavedHandler {
    struct sigaction prev;
    bool has_prev;
};
static SavedHandler g_Saved[5];
static const int g_SigList[5] = { SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE };

static const char* sigName(int s) {
    switch (s) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGBUS:  return "SIGBUS";
        case SIGILL:  return "SIGILL";
        case SIGFPE:  return "SIGFPE";
        default:      return "SIG-?-";
    }
}

// Async-signal-safe hex formatter
static void safeHexToStr(unsigned long val, char* buf, int bufsz) {
    if (bufsz < 4) { buf[0] = 0; return; }
    char tmp[20]; int n = 0;
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < (int)sizeof(tmp)) {
        int d = val & 0xF;
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        val >>= 4;
    }
    int k = 0;
    if (k < bufsz - 3) { buf[k++] = '0'; buf[k++] = 'x'; }
    while (n > 0 && k < bufsz - 1) buf[k++] = tmp[--n];
    buf[k] = 0;
}

static void crashHandler(int sig, siginfo_t* info, void* ctx) {
    // Restore default immediately to prevent recursion
    for (int i = 0; i < 5; i++) signal(g_SigList[i], SIG_DFL);

    // ---- async-safe write to both fds ----
    char buf[512]; int n = 0;

    safeWriteStr(g_CrashFd, "\n\n========================================\n");
    safeWriteStr(g_CrashFd, "NATIVE CRASH DETECTED\n");
    safeWriteStr(g_CrashFd, "========================================\n");

    n += snprintf(buf + n, sizeof(buf) - n, "Signal   : %s (%d)\n", sigName(sig), sig);
    safeWriteStr(g_CrashFd, buf); n = 0;

    n += snprintf(buf + n, sizeof(buf) - n, "Boot step: %s\n", bootStepName(g_BootStep.load()));
    safeWriteStr(g_CrashFd, buf); n = 0;

    if (info) {
        char hex[24]; safeHexToStr((unsigned long)info->si_addr, hex, sizeof(hex));
        n += snprintf(buf + n, sizeof(buf) - n, "Fault addr: %s\n", hex);
        safeWriteStr(g_CrashFd, buf); n = 0;
    }

    char pidBuf[16]; safeItoa((long)getpid(), pidBuf, sizeof(pidBuf));
    n += snprintf(buf + n, sizeof(buf) - n, "PID: %s\n", pidBuf);
    safeWriteStr(g_CrashFd, buf); n = 0;

    if (g_CrashFd >= 0) fsync(g_CrashFd);

    // ---- try backtrace (may crash — signals already SIG_DFL, so process dies cleanly) ----
    {
        void* bt[64];
        struct State { void** cur; void** end; } st = { bt, bt + 64 };
        _Unwind_Backtrace([](struct _Unwind_Context* c, void* a) -> _Unwind_Reason_Code {
            auto* s = static_cast<State*>(a);
            uintptr_t pc = _Unwind_GetIP(c);
            if (pc) { if (s->cur == s->end) return _URC_END_OF_STACK; *(s->cur++) = (void*)pc; }
            return _URC_NO_REASON;
        }, &st);

        int cnt = (int)(st.cur - bt);
        safeWriteStr(g_CrashFd, "--- Backtrace ---\n");
        for (int i = 0; i < cnt; i++) {
            Dl_info dl; n = 0;
            if (dladdr(bt[i], &dl) && dl.dli_fname) {
                unsigned long off = (unsigned long)bt[i] - (unsigned long)dl.dli_fbase;
                char offHex[24]; safeHexToStr(off, offHex, sizeof(offHex));
                n += snprintf(buf + n, sizeof(buf) - n,
                              "  #%02d %s (%s + %s)\n",
                              i, dl.dli_sname ? dl.dli_sname : "?",
                              dl.dli_fname, offHex);
            } else {
                n += snprintf(buf + n, sizeof(buf) - n, "  #%02d %p\n", i, bt[i]);
            }
            safeWriteStr(g_CrashFd, buf);
        }
        if (g_CrashFd >= 0) fsync(g_CrashFd);
    }

    // ---- chain to previous handler ----
    for (int i = 0; i < 5; i++) {
        if (g_SigList[i] == sig && g_Saved[i].has_prev) {
            struct sigaction& prev = g_Saved[i].prev;
            if (prev.sa_sigaction && prev.sa_sigaction != crashHandler) {
                prev.sa_sigaction(sig, info, ctx);
                return;
            } else if (prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN && prev.sa_handler) {
                prev.sa_handler(sig);
                return;
            }
        }
    }
    // no previous handler → reraise
    raise(sig);
}

static void installCrashHandler() {
    static char s_altStack[64 * 1024];
    stack_t ss; ss.ss_sp = s_altStack; ss.ss_size = sizeof(s_altStack); ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    for (int i = 0; i < 5; i++) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
        sa.sa_sigaction = crashHandler;
        sigemptyset(&sa.sa_mask);

        // Save previous
        struct sigaction prev;
        if (sigaction(g_SigList[i], nullptr, &prev) == 0) {
            g_Saved[i].prev = prev;
            g_Saved[i].has_prev = (prev.sa_sigaction || prev.sa_handler);
        } else {
            g_Saved[i].has_prev = false;
        }

        sigaction(g_SigList[i], &sa, nullptr);
    }
}

// ================================================================
//  INIT LOGGING
// ================================================================
static void initLogging() {
    g_LogDir = findLogDir();
    if (g_LogDir.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "No writable log dir");
        return;
    }
    std::string mainPath  = g_LogDir + "/modxlab.log";
    std::string crashPath = g_LogDir + "/modxlab_crash.log";
    g_LogFd   = open(mainPath.c_str(),  O_WRONLY | O_CREAT | O_APPEND, 0666);
    g_CrashFd = open(crashPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (g_LogFd >= 0) {
        g_LogReady.store(true);
        safeWriteStr(g_LogFd,
            "\n\n========================================\n"
            "ModXLab Session Start\n"
            "========================================\n");
        fsync(g_LogFd);
    }
    g_StartupFlagPath = g_LogDir + "/startup_pending";
    g_SafeModePath    = g_LogDir + "/SAFE_MODE";

    LOGI("Package : %s", g_PackageName.c_str());
    LOGI("Log dir : %s", g_LogDir.c_str());
    LOGI("PID     : %d", (int)getpid());
}

// ================================================================
//  CRASH-LOOP / SAFE MODE / SKIP FILES
// ================================================================
static bool fileExists(const std::string& p) { return !p.empty() && access(p.c_str(), F_OK) == 0; }
static bool previousStartupCrashed() { return fileExists(g_StartupFlagPath); }
static bool safeModeFileExists()     { return fileExists(g_SafeModePath); }

static void markStartupPending() {
    if (g_StartupFlagPath.empty()) return;
    int fd = open(g_StartupFlagPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { (void)!write(fd, "1", 1); close(fd); }
}
static void markStartupComplete() { if (!g_StartupFlagPath.empty()) unlink(g_StartupFlagPath.c_str()); }

// Per-hook skip file
static bool skipHook(const std::string& hookName) {
    if (g_LogDir.empty()) return false;
    std::string p = g_LogDir + "/SKIP_" + hookName;
    return fileExists(p);
}

// ================================================================
//  TIME HELPER
// ================================================================
static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//  FEATURE FLAGS (all default off)
// ================================================================
static std::atomic<bool> g_FrontalImpact    {false};
static std::atomic<bool> g_SideImpact       {false};
static std::atomic<int>  g_JumpLimit        {0};
static std::atomic<int>  g_JumpHeight       {0};
static std::atomic<bool> g_AutoRevive       {false};
static std::atomic<int>  g_PowerDuration    {0};
static std::atomic<bool> g_FastLaneChange   {false};
static std::atomic<int>  g_ScoreMultiplier  {0};
static std::atomic<bool> g_NoCollision      {false};
static std::atomic<bool> g_UnlimitedCoins   {false};
static std::atomic<long long> g_NoCollisionStartMs {0};

static std::atomic<bool> g_UnlimitedShop        {false};
static std::atomic<bool> g_NoGravity            {false};
static std::atomic<bool> g_FreezeCharacter      {false};
static std::atomic<int>  g_CharacterSpeed       {0};
static std::atomic<int>  g_AirJumpHeight        {0};
static std::atomic<int>  g_CameraMode           {0};
static std::atomic<bool> g_NoBoundaryImpact     {false};
static std::atomic<bool> g_NoStumble            {false};
static std::atomic<bool> g_HideJetpack          {false};
static std::atomic<bool> g_HidePogostick        {false};
static std::atomic<bool> g_HideMagnet           {false};
static std::atomic<bool> g_HideHoverboard       {false};
static std::atomic<bool> g_StopTrains           {false};
static std::atomic<int>  g_AddCoinsBonus        {0};
static std::atomic<int>  g_AddKeysBonus         {0};
static std::atomic<int>  g_AddScoreBonus        {0};
static std::atomic<bool> g_UnlimitedPowerTime   {false};
static std::atomic<bool> g_UnlimitedJetpackTime {false};
static std::atomic<bool> g_ReviveReward         {false};
static std::atomic<bool> g_FreezeCoins          {false};
static std::atomic<bool> g_FreezeScore          {false};

// ================================================================
//  HOOK LOGGING MACROS
// ================================================================
// First 3 calls log ENTER + EXIT — perfect for pinpointing crash
#define TRACE_CALL(tag) \
    static std::atomic<int> _traceCounter{0}; \
    int _myCall = _traceCounter.fetch_add(1, std::memory_order_relaxed) + 1; \
    if (_myCall <= 3) LOGI("[ENTER] %s #%d", tag, _myCall)

#define TRACE_EXIT(tag) \
    if (_myCall <= 3) LOGI("[EXIT ] %s #%d", tag, _myCall)

// Hook installation macro with skip-file + prologue logging
#define HOOK_CHECK(className, methodName, addr, replace, orig) do { \
    if (skipHook(methodName)) { \
        LOGW("[HOOK SKIP-FILE] %s.%s", className, methodName); \
        break; \
    } \
    /* Log prologue bytes for debugging */ \
    { \
        uint32_t* code = (uint32_t*)(addr); \
        LOGI("[HOOK TRY]  %s.%s @ %p  [%08x %08x]", \
             className, methodName, (void*)(addr), code[0], code[1]); \
    } \
    HOOK_AU((void*)(addr), (void*)(replace), (orig)); \
    if ((orig) != nullptr) { \
        LOGI("[HOOK OK]   %s.%s", className, methodName); \
    } else { \
        LOGE("[HOOK FAIL] %s.%s — original pointer is null", className, methodName); \
    } \
} while(0)

#define LOG_SKIP(className, methodName) \
    LOGE("[HOOK MISS] %s.%s — offset not found", className, methodName)

// ================================================================
//  ORIGINAL FUNCTION POINTERS
// ================================================================
static bool  (*old_CheckFrontalImpact)(...)     = nullptr;
static bool  (*old_CheckSideImpact)(...)        = nullptr;
static int   (*old_get_JumpLimit)(...)          = nullptr;
static float (*old_get_JumpHeight)(...)         = nullptr;
static bool  (*old_IsAutoReviveEnabled)(...)    = nullptr;
static float (*old_GetDuration)(...)            = nullptr;
static float (*old_get_LaneChangeDuration)(...) = nullptr;
static int   (*old_get_BaseMultiplierSum)(...)  = nullptr;
static bool  (*old_get_detectCollisions)(...)   = nullptr;
static int   (*old_GetCurrency)(...)            = nullptr;

static bool   (*old_get_IsIAP)(void*)                         = nullptr;
static void   (*old_ApplyGravity)(void*)                      = nullptr;
static float  (*old_get_MinSpeed)(void*)                      = nullptr;
static float  (*old_get_MaxSpeed)(void*)                      = nullptr;
static void   (*old_UpdateMovement)(void*)                    = nullptr;
static float  (*old_get_AirJumpHeight)(void*)                 = nullptr;
static bool   (*old_CameraApply)(void*, void*)                = nullptr;
static void   (*old_NotifySideImpact)(void*, int, int, void*) = nullptr;
static void   (*old_OnStumble)(void*, int)                    = nullptr;
static void   (*old_ShowFlight)(void*, bool, int)             = nullptr;
static void   (*old_ShowPogostick)(void*, bool)               = nullptr;
static void   (*old_ShowMagnet)(void*, bool)                  = nullptr;
static void   (*old_ShowHoverboard)(void*, bool)              = nullptr;
static void   (*old_Displace)(void*)                          = nullptr;
static void   (*old_Addcoins)(void*, int)                     = nullptr;
static void   (*old_AddKeys)(void*, int)                      = nullptr;
static void   (*old_AddPoints)(void*, double)                 = nullptr;
static void   (*old_UpdatePowers)(void*)                      = nullptr;
static void   (*old_MainUpdatePowers)(void*)                  = nullptr;
static void   (*old_FinishFlight)(void*)                      = nullptr;
static void   (*old_OnRevivedWithCurrency)(void*)             = nullptr;
static void   (*old_MockRevivedWithCurrency)(void*)           = nullptr;
static void   (*old_HandleCoinsPickedUp)(void*)               = nullptr;
static void   (*old_HandlePointsEarned)(void*)                = nullptr;

// ================================================================
//  HOOK IMPLEMENTATIONS — ALL PASS-THROUGH WHEN DISABLED
// ================================================================

static bool new_CheckFrontalImpact(void* self, void* other) {
    TRACE_CALL("CharacterMotor.CheckFrontalImpact");
    bool r;
    if (!self) r = false;
    else if (g_FrontalImpact.load()) r = false;
    else r = old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
    TRACE_EXIT("CharacterMotor.CheckFrontalImpact");
    return r;
}

static bool new_CheckSideImpact(void* self, void* other) {
    TRACE_CALL("CharacterMotor.CheckSideImpact");
    bool r;
    if (!self) r = false;
    else if (g_SideImpact.load()) r = false;
    else r = old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
    TRACE_EXIT("CharacterMotor.CheckSideImpact");
    return r;
}

static int new_get_JumpLimit(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_JumpLimit");
    int v = g_JumpLimit.load();
    int r = (v > 0) ? v : (old_get_JumpLimit ? old_get_JumpLimit(self) : 0);
    TRACE_EXIT("CharacterMotorAbilities.get_JumpLimit");
    return r;
}

static float new_get_JumpHeight(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_JumpHeight");
    int v = g_JumpHeight.load();
    float r = (v > 0) ? (float)v : (old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f);
    TRACE_EXIT("CharacterMotorAbilities.get_JumpHeight");
    return r;
}

static float new_get_LaneChangeDuration(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_LaneChangeDuration");
    float r = g_FastLaneChange.load() ? 0.0f
            : (old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f);
    TRACE_EXIT("CharacterMotorAbilities.get_LaneChangeDuration");
    return r;
}

static bool new_IsAutoReviveEnabled(void* self) {
    TRACE_CALL("StumbleBehaviour.IsAutoReviveEnabled");
    bool r = g_AutoRevive.load() ? true
           : (old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false);
    TRACE_EXIT("StumbleBehaviour.IsAutoReviveEnabled");
    return r;
}

static float new_GetDuration(void* self, void* cfg) {
    TRACE_CALL("PowerConfig.GetDuration");
    int v = g_PowerDuration.load();
    float r = (v > 0) ? (float)v : (old_GetDuration ? old_GetDuration(self, cfg) : 0.0f);
    TRACE_EXIT("PowerConfig.GetDuration");
    return r;
}

static int new_get_BaseMultiplierSum(void* self) {
    TRACE_CALL("ScoreMultiplierManager.get_BaseMultiplierSum");
    int v = g_ScoreMultiplier.load();
    int r = (v > 0) ? v : (old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0);
    TRACE_EXIT("ScoreMultiplierManager.get_BaseMultiplierSum");
    return r;
}

static bool new_get_detectCollisions(void* self) {
    TRACE_CALL("CharacterController.get_detectCollisions");
    bool r;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        r = (start > 0 && (nowMs() - start) < 1000) ? false
          : (old_get_detectCollisions ? old_get_detectCollisions(self) : true);
    } else {
        r = old_get_detectCollisions ? old_get_detectCollisions(self) : true;
    }
    TRACE_EXIT("CharacterController.get_detectCollisions");
    return r;
}

static int new_GetCurrency(void* self, int currencyType) {
    TRACE_CALL("WalletModel.GetCurrency");
    int r = g_UnlimitedCoins.load() ? 99999999
          : (old_GetCurrency ? old_GetCurrency(self, currencyType) : 0);
    TRACE_EXIT("WalletModel.GetCurrency");
    return r;
}

static bool new_get_IsIAP(void* self) {
    TRACE_CALL("Currency.get_IsIAP");
    bool r = g_UnlimitedShop.load() ? false
           : (old_get_IsIAP ? old_get_IsIAP(self) : false);
    TRACE_EXIT("Currency.get_IsIAP");
    return r;
}

static void new_ApplyGravity(void* self) {
    TRACE_CALL("CharacterMotor.ApplyGravity");
    if (!g_NoGravity.load() && old_ApplyGravity) old_ApplyGravity(self);
    TRACE_EXIT("CharacterMotor.ApplyGravity");
}

static float new_get_MinSpeed(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_MinSpeed");
    int v = g_CharacterSpeed.load();
    float r = (v > 0) ? (float)v : (old_get_MinSpeed ? old_get_MinSpeed(self) : 0.0f);
    TRACE_EXIT("CharacterMotorAbilities.get_MinSpeed");
    return r;
}

static float new_get_MaxSpeed(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_MaxSpeed");
    int v = g_CharacterSpeed.load();
    float r = (v > 0) ? (float)v : (old_get_MaxSpeed ? old_get_MaxSpeed(self) : 0.0f);
    TRACE_EXIT("CharacterMotorAbilities.get_MaxSpeed");
    return r;
}

static void new_UpdateMovement(void* self) {
    TRACE_CALL("CharacterMotor.UpdateMovement");
    if (!g_FreezeCharacter.load() && old_UpdateMovement) old_UpdateMovement(self);
    TRACE_EXIT("CharacterMotor.UpdateMovement");
}

static float new_get_AirJumpHeight(void* self) {
    TRACE_CALL("CharacterMotorAbilities.get_AirJumpHeight");
    int v = g_AirJumpHeight.load();
    float r = (v > 0) ? (float)v : (old_get_AirJumpHeight ? old_get_AirJumpHeight(self) : 0.0f);
    TRACE_EXIT("CharacterMotorAbilities.get_AirJumpHeight");
    return r;
}

static bool new_CameraApply(void* self, void* context) {
    TRACE_CALL("CameraGroundedModifier.Apply");
    int mode = g_CameraMode.load();
    bool r;
    if (mode == 1) r = true;
    else if (mode == 2) r = false;
    else r = old_CameraApply ? old_CameraApply(self, context) : false;
    TRACE_EXIT("CameraGroundedModifier.Apply");
    return r;
}

static void new_NotifySideImpact(void* self, int h, int e, void* ent) {
    TRACE_CALL("CharacterMotor.NotifySideImpact");
    if (!g_NoBoundaryImpact.load() && old_NotifySideImpact)
        old_NotifySideImpact(self, h, e, ent);
    TRACE_EXIT("CharacterMotor.NotifySideImpact");
}

static void new_OnStumble(void* self, int dc) {
    TRACE_CALL("RunProgressOrderIndicator.OnStumble");
    if (!g_NoStumble.load() && old_OnStumble) old_OnStumble(self, dc);
    TRACE_EXIT("RunProgressOrderIndicator.OnStumble");
}

static void new_ShowFlight(void* self, bool show, int mode) {
    TRACE_CALL("CharacterPropsController.ShowFlight");
    if (!g_HideJetpack.load() && old_ShowFlight) old_ShowFlight(self, show, mode);
    TRACE_EXIT("CharacterPropsController.ShowFlight");
}

static void new_ShowPogostick(void* self, bool show) {
    TRACE_CALL("CharacterPropsController.ShowPogostick");
    if (!g_HidePogostick.load() && old_ShowPogostick) old_ShowPogostick(self, show);
    TRACE_EXIT("CharacterPropsController.ShowPogostick");
}

static void new_ShowMagnet(void* self, bool show) {
    TRACE_CALL("CharacterPropsController.ShowMagnet");
    if (!g_HideMagnet.load() && old_ShowMagnet) old_ShowMagnet(self, show);
    TRACE_EXIT("CharacterPropsController.ShowMagnet");
}

static void new_ShowHoverboard(void* self, bool rb) {
    TRACE_CALL("CharacterPropsController.ShowHoverboard");
    if (!g_HideHoverboard.load() && old_ShowHoverboard) old_ShowHoverboard(self, rb);
    TRACE_EXIT("CharacterPropsController.ShowHoverboard");
}

static void new_Displace(void* self) {
    TRACE_CALL("MovingTrain.Displace");
    if (!g_StopTrains.load() && old_Displace) old_Displace(self);
    TRACE_EXIT("MovingTrain.Displace");
}

static void new_Addcoins(void* self, int coins) {
    TRACE_CALL("RunSessionData.Addcoins");
    int b = g_AddCoinsBonus.load();
    if (old_Addcoins) old_Addcoins(self, b > 0 ? coins + b : coins);
    TRACE_EXIT("RunSessionData.Addcoins");
}

static void new_AddKeys(void* self, int keys) {
    TRACE_CALL("RunSessionData.AddKeys");
    int b = g_AddKeysBonus.load();
    if (old_AddKeys) old_AddKeys(self, b > 0 ? keys + b : keys);
    TRACE_EXIT("RunSessionData.AddKeys");
}

static void new_AddPoints(void* self, double points) {
    TRACE_CALL("RunSessionData.AddPoints");
    int b = g_AddScoreBonus.load();
    if (old_AddPoints) old_AddPoints(self, b > 0 ? points + (double)b : points);
    TRACE_EXIT("RunSessionData.AddPoints");
}

static void new_UpdatePowers(void* self) {
    TRACE_CALL("CharacterPower.UpdatePowers");
    if (!g_UnlimitedPowerTime.load() && old_UpdatePowers) old_UpdatePowers(self);
    TRACE_EXIT("CharacterPower.UpdatePowers");
}

static void new_MainUpdatePowers(void* self) {
    TRACE_CALL("MainPowerSystem.UpdatePowers");
    if (!g_UnlimitedPowerTime.load() && old_MainUpdatePowers) old_MainUpdatePowers(self);
    TRACE_EXIT("MainPowerSystem.UpdatePowers");
}

static void new_FinishFlight(void* self) {
    TRACE_CALL("FlyingState.FinishFlight");
    if (!g_UnlimitedJetpackTime.load() && old_FinishFlight) old_FinishFlight(self);
    TRACE_EXIT("FlyingState.FinishFlight");
}

static void new_OnRevivedWithCurrency(void* self) {
    TRACE_CALL("MainReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) if (old_OnRevivedWithCurrency) old_OnRevivedWithCurrency(self);
    TRACE_EXIT("MainReviveSystem.OnRevivedWithCurrency");
}

static void new_MockRevivedWithCurrency(void* self) {
    TRACE_CALL("MockReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) if (old_MockRevivedWithCurrency) old_MockRevivedWithCurrency(self);
    TRACE_EXIT("MockReviveSystem.OnRevivedWithCurrency");
}

static void new_HandleCoinsPickedUp(void* self) {
    TRACE_CALL("GameScreenCoinCounter.HandleOnCoinsPickedUp");
    if (!g_FreezeCoins.load() && old_HandleCoinsPickedUp) old_HandleCoinsPickedUp(self);
    TRACE_EXIT("GameScreenCoinCounter.HandleOnCoinsPickedUp");
}

static void new_HandlePointsEarned(void* self) {
    TRACE_CALL("GameScreenScoreCounter.HandleOnPointsEarned");
    if (!g_FreezeScore.load() && old_HandlePointsEarned) old_HandlePointsEarned(self);
    TRACE_EXIT("GameScreenScoreCounter.HandleOnPointsEarned");
}

// ================================================================
//  HEARTBEAT
// ================================================================
static void* heartbeat_thread(void*) {
    int n = 0;
    while (true) {
        sleep(60); n++;
        LOGI("[HEARTBEAT %02d] alive | step=%s | chams=%d",
             n, bootStepName(g_BootStep.load()), (int)g_ChamsEnabled);
        if (n == 2) {
            markStartupComplete();
            LOGI("[BOOT] Startup marked stable — crash-loop protection cleared");
        }
    }
    return nullptr;
}

// ================================================================
//  PHASE 1 — PROVEN SAFE HOOKS
// ================================================================
static void installPhase1() {
    LOGI("=== PHASE 1: safe hooks ===");
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (C && C->thisclass) {
            DWORD oF = C->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oS = C->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oF) HOOK_CHECK("CharacterMotor", "CheckFrontalImpact", oF, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            else    LOG_SKIP("CharacterMotor", "CheckFrontalImpact");
            if (oS) HOOK_CHECK("CharacterMotor", "CheckSideImpact", oS, (void*)new_CheckSideImpact, old_CheckSideImpact);
            else    LOG_SKIP("CharacterMotor", "CheckSideImpact");
        } else LOGE("[CLASS MISS] CharacterMotor");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (C && C->thisclass) {
            DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (o1) HOOK_CHECK("CharacterMotorAbilities", "get_JumpLimit", o1, (void*)new_get_JumpLimit, old_get_JumpLimit);
            else    LOG_SKIP("CharacterMotorAbilities", "get_JumpLimit");
            if (o2) HOOK_CHECK("CharacterMotorAbilities", "get_JumpHeight", o2, (void*)new_get_JumpHeight, old_get_JumpHeight);
            else    LOG_SKIP("CharacterMotorAbilities", "get_JumpHeight");
            if (o3) HOOK_CHECK("CharacterMotorAbilities", "get_LaneChangeDuration", o3, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
            else    LOG_SKIP("CharacterMotorAbilities", "get_LaneChangeDuration");
        } else LOGE("[CLASS MISS] CharacterMotorAbilities");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (o) HOOK_CHECK("StumbleBehaviour", "IsAutoReviveEnabled", o, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
            else   LOG_SKIP("StumbleBehaviour", "IsAutoReviveEnabled");
        } else LOGE("[CLASS MISS] StumbleBehaviour");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (o) HOOK_CHECK("PowerConfig", "GetDuration", o, (void*)new_GetDuration, old_GetDuration);
            else   LOG_SKIP("PowerConfig", "GetDuration");
        } else LOGE("[CLASS MISS] PowerConfig");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (o) HOOK_CHECK("ScoreMultiplierManager", "get_BaseMultiplierSum", o, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
            else   LOG_SKIP("ScoreMultiplierManager", "get_BaseMultiplierSum");
        } else LOGE("[CLASS MISS] ScoreMultiplierManager");
    }
    {
        auto C = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (o) HOOK_CHECK("CharacterController", "get_detectCollisions", o, (void*)new_get_detectCollisions, old_get_detectCollisions);
            else   LOG_SKIP("CharacterController", "get_detectCollisions");
        } else LOGE("[CLASS MISS] CharacterController");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (o) HOOK_CHECK("WalletModel", "GetCurrency", o, (void*)new_GetCurrency, old_GetCurrency);
            else   LOG_SKIP("WalletModel", "GetCurrency");
        } else LOGE("[CLASS MISS] WalletModel");
    }
    LOGI("=== PHASE 1 complete ===");
}

// ================================================================
//  PHASE 2A — Currency-related
// ================================================================
static void installPhase2a() {
    LOGI("=== PHASE 2A: currency hooks ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
    if (C && C->thisclass) {
        DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
        if (o) HOOK_CHECK("Currency", "get_IsIAP", o, (void*)new_get_IsIAP, old_get_IsIAP);
        else   LOG_SKIP("Currency", "get_IsIAP");
    } else LOGE("[CLASS MISS] Currency");
    LOGI("=== PHASE 2A complete ===");
}

// ================================================================
//  PHASE 2B — CharacterMotor extras
// ================================================================
static void installPhase2b() {
    LOGI("=== PHASE 2B: CharacterMotor extras ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
    if (C && C->thisclass) {
        DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("ApplyGravity"),     0);
        DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("UpdateMovement"),   0);
        DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("NotifySideImpact"), 3);
        if (o1) HOOK_CHECK("CharacterMotor", "ApplyGravity",     o1, (void*)new_ApplyGravity,     old_ApplyGravity);
        else    LOG_SKIP("CharacterMotor", "ApplyGravity");
        if (o2) HOOK_CHECK("CharacterMotor", "UpdateMovement",   o2, (void*)new_UpdateMovement,   old_UpdateMovement);
        else    LOG_SKIP("CharacterMotor", "UpdateMovement");
        if (o3) HOOK_CHECK("CharacterMotor", "NotifySideImpact", o3, (void*)new_NotifySideImpact, old_NotifySideImpact);
        else    LOG_SKIP("CharacterMotor", "NotifySideImpact");
    } else LOGE("[CLASS MISS] CharacterMotor");
    LOGI("=== PHASE 2B complete ===");
}

// ================================================================
//  PHASE 2C — CharacterMotorAbilities extras
// ================================================================
static void installPhase2c() {
    LOGI("=== PHASE 2C: speed / air jump ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
    if (C && C->thisclass) {
        DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("get_MinSpeed"),      0);
        DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("get_MaxSpeed"),      0);
        DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"), 0);
        if (o1) HOOK_CHECK("CharacterMotorAbilities", "get_MinSpeed",      o1, (void*)new_get_MinSpeed,      old_get_MinSpeed);
        else    LOG_SKIP("CharacterMotorAbilities", "get_MinSpeed");
        if (o2) HOOK_CHECK("CharacterMotorAbilities", "get_MaxSpeed",      o2, (void*)new_get_MaxSpeed,      old_get_MaxSpeed);
        else    LOG_SKIP("CharacterMotorAbilities", "get_MaxSpeed");
        if (o3) HOOK_CHECK("CharacterMotorAbilities", "get_AirJumpHeight", o3, (void*)new_get_AirJumpHeight, old_get_AirJumpHeight);
        else    LOG_SKIP("CharacterMotorAbilities", "get_AirJumpHeight");
    } else LOGE("[CLASS MISS] CharacterMotorAbilities");
    LOGI("=== PHASE 2C complete ===");
}

// ================================================================
//  PHASE 3 — Camera / Stumble / CharacterProps (SUSPICIOUS GROUP)
// ================================================================
static void installPhase3() {
    LOGI("=== PHASE 3: camera / props ===");
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraGroundedModifier"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Apply"), 1);
            if (o) HOOK_CHECK("CameraGroundedModifier", "Apply", o, (void*)new_CameraApply, old_CameraApply);
            else   LOG_SKIP("CameraGroundedModifier", "Apply");
        } else LOGE("[CLASS MISS] CameraGroundedModifier");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunProgressOrderIndicator"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnStumble"), 1);
            if (o) HOOK_CHECK("RunProgressOrderIndicator", "OnStumble", o, (void*)new_OnStumble, old_OnStumble);
            else   LOG_SKIP("RunProgressOrderIndicator", "OnStumble");
        } else LOGE("[CLASS MISS] RunProgressOrderIndicator");
    }
    LOGI("=== PHASE 3 complete ===");
}

// ================================================================
//  PHASE 4 — CharacterPropsController (ONE BY ONE with delays)
// ================================================================
static void installPhase4_Props() {
    LOGI("=== PHASE 4: CharacterPropsController (one-by-one) ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPropsController"));
    if (!C || !C->thisclass) { LOGE("[CLASS MISS] CharacterPropsController"); return; }

    {
        DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowFlight"), 2);
        if (o) HOOK_CHECK("CharacterPropsController", "ShowFlight", o, (void*)new_ShowFlight, old_ShowFlight);
        else   LOG_SKIP("CharacterPropsController", "ShowFlight");
        usleep(300 * 1000);
    }
    {
        DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowPogostick"), 1);
        if (o) HOOK_CHECK("CharacterPropsController", "ShowPogostick", o, (void*)new_ShowPogostick, old_ShowPogostick);
        else   LOG_SKIP("CharacterPropsController", "ShowPogostick");
        usleep(300 * 1000);
    }
    {
        DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowMagnet"), 1);
        if (o) HOOK_CHECK("CharacterPropsController", "ShowMagnet", o, (void*)new_ShowMagnet, old_ShowMagnet);
        else   LOG_SKIP("CharacterPropsController", "ShowMagnet");
        usleep(300 * 1000);
    }
    {
        DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowHoverboard"), 1);
        if (o) HOOK_CHECK("CharacterPropsController", "ShowHoverboard", o, (void*)new_ShowHoverboard, old_ShowHoverboard);
        else   LOG_SKIP("CharacterPropsController", "ShowHoverboard");
        usleep(300 * 1000);
    }
    LOGI("=== PHASE 4 complete ===");
}

// ================================================================
//  PHASE 5 — Misc
// ================================================================
static void installPhase5() {
    LOGI("=== PHASE 5: misc hooks ===");

    // MovingTrain.Displace
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Displace"), 0);
            if (o) HOOK_CHECK("MovingTrain", "Displace", o, (void*)new_Displace, old_Displace);
            else   LOG_SKIP("MovingTrain", "Displace");
        } else LOGE("[CLASS MISS] MovingTrain");
    }

    // RunSessionData
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (C && C->thisclass) {
            // Try both Addcoins and AddCoins (case variants)
            DWORD oC = C->GetMethodOffsetByName(OBFUSCATE("Addcoins"), 1);
            if (!oC) oC = C->GetMethodOffsetByName(OBFUSCATE("AddCoins"), 1);
            if (oC) HOOK_CHECK("RunSessionData", "Addcoins", oC, (void*)new_Addcoins, old_Addcoins);
            else    LOG_SKIP("RunSessionData", "Addcoins");

            DWORD oK = C->GetMethodOffsetByName(OBFUSCATE("AddKeys"), 1);
            if (oK) HOOK_CHECK("RunSessionData", "AddKeys", oK, (void*)new_AddKeys, old_AddKeys);
            else    LOG_SKIP("RunSessionData", "AddKeys");

            DWORD oP = C->GetMethodOffsetByName(OBFUSCATE("AddPoints"), 1);
            if (oP) HOOK_CHECK("RunSessionData", "AddPoints", oP, (void*)new_AddPoints, old_AddPoints);
            else    LOG_SKIP("RunSessionData", "AddPoints");
        } else LOGE("[CLASS MISS] RunSessionData");
    }

    // CharacterPower.UpdatePowers
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPower"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_CHECK("CharacterPower", "UpdatePowers", o, (void*)new_UpdatePowers, old_UpdatePowers);
            else   LOG_SKIP("CharacterPower", "UpdatePowers");
        } else LOGE("[CLASS MISS] CharacterPower");
    }

    // MainPowerSystem.UpdatePowers
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainPowerSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_CHECK("MainPowerSystem", "UpdatePowers", o, (void*)new_MainUpdatePowers, old_MainUpdatePowers);
            else   LOG_SKIP("MainPowerSystem", "UpdatePowers");
        } else LOGE("[CLASS MISS] MainPowerSystem");
    }

    // FlyingState.FinishFlight
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("FlyingState"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("FinishFlight"), 0);
            if (o) HOOK_CHECK("FlyingState", "FinishFlight", o, (void*)new_FinishFlight, old_FinishFlight);
            else   LOG_SKIP("FlyingState", "FinishFlight");
        } else LOGE("[CLASS MISS] FlyingState");
    }

    // Revive systems
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_CHECK("MainReviveSystem", "OnRevivedWithCurrency", o, (void*)new_OnRevivedWithCurrency, old_OnRevivedWithCurrency);
            else   LOG_SKIP("MainReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MainReviveSystem");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems.Mocks"), OBFUSCATE("MockReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_CHECK("MockReviveSystem", "OnRevivedWithCurrency", o, (void*)new_MockRevivedWithCurrency, old_MockRevivedWithCurrency);
            else   LOG_SKIP("MockReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MockReviveSystem");
    }

    // Game screen counters
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenCoinCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnCoinsPickedUp"), 0);
            if (o) HOOK_CHECK("GameScreenCoinCounter", "HandleOnCoinsPickedUp", o, (void*)new_HandleCoinsPickedUp, old_HandleCoinsPickedUp);
            else   LOG_SKIP("GameScreenCoinCounter", "HandleOnCoinsPickedUp");
        } else LOGE("[CLASS MISS] GameScreenCoinCounter");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenScoreCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnPointsEarned"), 0);
            if (o) HOOK_CHECK("GameScreenScoreCounter", "HandleOnPointsEarned", o, (void*)new_HandlePointsEarned, old_HandlePointsEarned);
            else   LOG_SKIP("GameScreenScoreCounter", "HandleOnPointsEarned");
        } else LOGE("[CLASS MISS] GameScreenScoreCounter");
    }

    LOGI("=== PHASE 5 complete ===");
}

// ================================================================
//  HACK THREAD
// ================================================================
void* hack_thread(void*) {
    g_BootStep = 3;
    LOGI(">>> hack_thread started <<<");

    std::string libName = LIB;
    LOGI("Waiting for %s ...", libName.c_str());
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    g_BootStep = 4;
    LOGI(">>> library loaded <<<");

    // Module base — try dlsym first, fallback to /proc/self/maps
    g_BootStep = 5;
    {
        void* sym = dlsym(RTLD_DEFAULT, "il2cpp_init");
        if (sym) {
            Dl_info info;
            if (dladdr(sym, &info)) {
                LOGI("[MODULE] %s base=%p (via dlsym)", info.dli_fname, info.dli_fbase);
            }
        } else {
            void* base = findModuleBase("libil2cpp.so");
            if (base) LOGI("[MODULE] libil2cpp.so base=%p (via /proc/self/maps)", base);
            else      LOGW("[MODULE] could not determine base address");
        }
    }

    // Safe mode / crash loop
    if (safeModeFileExists()) {
        LOGE("[SAFE MODE] SAFE_MODE file present. Skipping hooks.");
        LOGE("[SAFE MODE] Delete: %s", g_SafeModePath.c_str());
        g_BootStep = 13; return nullptr;
    }
    if (previousStartupCrashed()) {
        LOGE("[SAFE MODE] Previous startup crashed. Skipping hooks.");
        LOGE("[SAFE MODE] Delete to retry: %s", g_StartupFlagPath.c_str());
        unlink(g_StartupFlagPath.c_str());
        g_BootStep = 13; return nullptr;
    }
    markStartupPending();

    // ---- multi-phase hook installation ----
    g_BootStep = 6;  installPhase1();        usleep(500 * 1000);
    g_BootStep = 7;  installPhase2a();       usleep(300 * 1000);
    g_BootStep = 8;  installPhase2b();       usleep(300 * 1000);
    g_BootStep = 9;  installPhase2c();       usleep(300 * 1000);
    g_BootStep = 10; installPhase3();        usleep(500 * 1000);
    g_BootStep = 11; installPhase4_Props();  usleep(500 * 1000);
    g_BootStep = 12; installPhase5();        usleep(300 * 1000);

    g_BootStep = 13;
    LOGI(">>> installing GLES render hooks (chams) <<<");
    RenderHook();
    LOGI(">>> GLES hooks done <<<");

    pthread_t hb;
    pthread_create(&hb, nullptr, heartbeat_thread, nullptr);

    LOGI("========================================");
    LOGI("ALL HOOKS INSTALLED — startup complete.");
    LOGI("Watch [ENTER]/[EXIT] pairs — missing EXIT = crash point.");
    LOGI("========================================");
    return nullptr;
}

// ================================================================
//  JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {
        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),
        OBFUSCATE("Category_Runner"),
        OBFUSCATE("100_Toggle_No Head-On Collision"),
        OBFUSCATE("101_Toggle_No Side Collision"),
        OBFUSCATE("102_SeekBar_Jump Limit_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height_0_200"),
        OBFUSCATE("104_Toggle_Auto Revive"),
        OBFUSCATE("130_Toggle_No Gravity"),
        OBFUSCATE("131_Toggle_No Boundary Impact"),
        OBFUSCATE("132_Toggle_No Character Stumble"),
        OBFUSCATE("133_Toggle_Freeze Character"),
        OBFUSCATE("134_SeekBar_Air Jump Height_0_200"),
        OBFUSCATE("Category_Powers"),
        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),
        OBFUSCATE("135_Toggle_Hide Jetpack"),
        OBFUSCATE("136_Toggle_Hide Pogostick"),
        OBFUSCATE("137_Toggle_Hide Magnet"),
        OBFUSCATE("138_Toggle_Hide Hoverboard"),
        OBFUSCATE("140_Toggle_Unlimited Powerup Time"),
        OBFUSCATE("141_Toggle_Unlimited Jetpack Time"),
        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),
        OBFUSCATE("143_Toggle_Freeze Coins"),
        OBFUSCATE("144_Toggle_Freeze Score"),
        OBFUSCATE("Category_Collisions"),
        OBFUSCATE("108_Toggle_No Collision Detect"),
        OBFUSCATE("Category_Shop"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),
        OBFUSCATE("150_Toggle_Unlimited Shop"),
        OBFUSCATE("151_SeekBar_Add Coins Bonus_0_9999"),
        OBFUSCATE("152_SeekBar_Add Keys Bonus_0_9999"),
        OBFUSCATE("153_SeekBar_Add Score Bonus_0_9999"),
        OBFUSCATE("154_Toggle_Revive Reward"),
        OBFUSCATE("Category_Character"),
        OBFUSCATE("160_SeekBar_Character Speed_0_200"),
        OBFUSCATE("Category_Camera"),
        OBFUSCATE("161_Spinner_Camera Mode_Normal,Follow,Stop"),
        OBFUSCATE("Category_World"),
        OBFUSCATE("142_Toggle_Stop Moving Trains"),
        OBFUSCATE("Category_Chams"),
        OBFUSCATE("119_Toggle_Enable Chams"),
        OBFUSCATE("120_Spinner_Chams Mode_Off,Solid Visible,X-Ray,Hybrid Opaque"),
        OBFUSCATE("125_Spinner_Color Preset_Dark Green / Sky Blue,Neon Green / Red,Cyan / Magenta"),
        OBFUSCATE("126_Spinner_Shader Target_Trains Only (_Displacement),All Objects (_MainTex)"),
        OBFUSCATE("127_SeekBar_Max Distance_15_100"),
        OBFUSCATE("121_Toggle_Rainbow Visible"),
        OBFUSCATE("122_Toggle_Rainbow In-Wall"),
    };
    int n = (int)(sizeof(features) / sizeof(features[0]));
    ret = (jobjectArray) env->NewObjectArray(n,
            env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < n; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

// ================================================================
//  JNI — CHANGES HANDLER
// ================================================================
void Changes(JNIEnv*, jclass, jobject, jint featNum, jstring, jint value, jlong, jboolean boolean, jstring) {
    switch (featNum) {
        case 100: g_FrontalImpact.store(boolean);   break;
        case 101: g_SideImpact.store(boolean);      break;
        case 102: g_JumpLimit.store(value);         break;
        case 103: g_JumpHeight.store(value);        break;
        case 104: g_AutoRevive.store(boolean);      break;
        case 105: g_PowerDuration.store(value);     break;
        case 106: g_FastLaneChange.store(boolean);  break;
        case 107: g_ScoreMultiplier.store(value);   break;
        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else         g_NoCollisionStartMs.store(0);
        } break;
        case 110: g_UnlimitedCoins.store(boolean); break;
        case 130: g_NoGravity.store(boolean);            break;
        case 131: g_NoBoundaryImpact.store(boolean);     break;
        case 132: g_NoStumble.store(boolean);            break;
        case 133: g_FreezeCharacter.store(boolean);      break;
        case 134: g_AirJumpHeight.store(value);          break;
        case 135: g_HideJetpack.store(boolean);          break;
        case 136: g_HidePogostick.store(boolean);        break;
        case 137: g_HideMagnet.store(boolean);           break;
        case 138: g_HideHoverboard.store(boolean);       break;
        case 140: g_UnlimitedPowerTime.store(boolean);   break;
        case 141: g_UnlimitedJetpackTime.store(boolean); break;
        case 142: g_StopTrains.store(boolean);           break;
        case 143: g_FreezeCoins.store(boolean);          break;
        case 144: g_FreezeScore.store(boolean);          break;
        case 150: g_UnlimitedShop.store(boolean);        break;
        case 151: g_AddCoinsBonus.store(value);          break;
        case 152: g_AddKeysBonus.store(value);           break;
        case 153: g_AddScoreBonus.store(value);          break;
        case 154: g_ReviveReward.store(boolean);         break;
        case 160: g_CharacterSpeed.store(value);         break;
        case 161: g_CameraMode.store(value);             break;
        case 119: g_ChamsEnabled = boolean;     break;
        case 120: chamsint = value;             break;
        case 125: setColorPreset(value);        break;
        case 126: setShaderPreset(value);       break;
        case 127: g_MaxDistance = (float)value; break;
        case 121: enableRainbow = boolean;      break;
        case 122: enableRainbowWall = boolean;  break;
        case 800: {
            if (boolean) {
                g_FrontalImpact.store(true); g_SideImpact.store(true);
                g_AutoRevive.store(true); g_UnlimitedCoins.store(true);
            } else {
                g_FrontalImpact.store(false); g_SideImpact.store(false);
                g_AutoRevive.store(false); g_UnlimitedCoins.store(false);
            }
        } break;
        case 801: {
            if (boolean) {
                g_FrontalImpact.store(true); g_SideImpact.store(true);
                g_AutoRevive.store(true); g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true); g_JumpLimit.store(100);
                g_JumpHeight.store(50); g_PowerDuration.store(100);
                g_ScoreMultiplier.store(10);
            } else {
                g_JumpLimit.store(0); g_JumpHeight.store(0);
                g_PowerDuration.store(0); g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
            }
        } break;
        case 802: {
            if (boolean) {
                g_FrontalImpact.store(true); g_SideImpact.store(true);
                g_AutoRevive.store(true); g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true); g_NoCollision.store(true);
                g_NoCollisionStartMs.store(nowMs());
                g_JumpLimit.store(999); g_JumpHeight.store(200);
                g_PowerDuration.store(999); g_ScoreMultiplier.store(999);
            } else {
                g_FrontalImpact.store(false); g_SideImpact.store(false);
                g_AutoRevive.store(false); g_FastLaneChange.store(false);
                g_UnlimitedCoins.store(false); g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0); g_JumpLimit.store(0);
                g_JumpHeight.store(0); g_PowerDuration.store(0);
                g_ScoreMultiplier.store(0);
            }
        } break;
        case 803: {
            g_FrontalImpact.store(false); g_SideImpact.store(false);
            g_AutoRevive.store(false); g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false); g_NoCollision.store(false);
            g_NoCollisionStartMs.store(0); g_JumpLimit.store(0);
            g_JumpHeight.store(0); g_PowerDuration.store(0);
            g_ScoreMultiplier.store(0);
            g_NoGravity.store(false);        g_FreezeCharacter.store(false);
            g_CharacterSpeed.store(0);       g_AirJumpHeight.store(0);
            g_CameraMode.store(0);           g_NoBoundaryImpact.store(false);
            g_NoStumble.store(false);        g_HideJetpack.store(false);
            g_HidePogostick.store(false);    g_HideMagnet.store(false);
            g_HideHoverboard.store(false);   g_StopTrains.store(false);
            g_AddCoinsBonus.store(0);        g_AddKeysBonus.store(0);
            g_AddScoreBonus.store(0);        g_UnlimitedPowerTime.store(false);
            g_UnlimitedJetpackTime.store(false); g_ReviveReward.store(false);
            g_FreezeCoins.store(false);      g_FreezeScore.store(false);
            g_UnlimitedShop.store(false);
        } break;
        default: break;
    }
}

// ================================================================
//  STUBS
// ================================================================
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_Draw(JNIEnv*, jclass, jobject, jobject) { }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_SetTeleportTargetNorm(JNIEnv*, jclass, jfloat, jfloat) { }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_GetTeleportEnabled(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_IsSmoothTeleportActive(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Main_setNativeCrashDir(JNIEnv*, jclass, jstring) { }

// ================================================================
//  CONSTRUCTOR
// ================================================================
__attribute__((constructor))
void lib_main() {
    g_BootStep = 0;
    initLogging();            // 1. log first so we see everything
    g_BootStep = 2;
    installCrashHandler();    // 2. crash handler (chains Unity's)
    LOGI("[BOOT] Crash handler installed (chained)");
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}