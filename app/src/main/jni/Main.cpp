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
#include <android/log.h>

// ============ Safe signal / crash handling ============
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
//  🛡️  BOOT STEP TRACKER — visible in crash log
// ================================================================
static std::atomic<int> g_BootStep{0};
static const char* bootStepName(int s) {
    switch (s) {
        case 0:  return "0-constructor-start";
        case 1:  return "1-init-logging";
        case 2:  return "2-install-crash-handler";
        case 3:  return "3-hack-thread-started";
        case 4:  return "4-library-loaded";
        case 5:  return "5-module-base-logged";
        case 6:  return "6-hooks-phase1-safe";
        case 7:  return "7-hooks-phase2-new";
        case 8:  return "8-render-hook";
        case 9:  return "9-heartbeat-started";
        case 10: return "10-startup-complete";
        default: return "?";
    }
}

// ================================================================
//  📁  RAW-FD LOGGER  (bypasses stdio, always flushed)
// ================================================================
static int              g_LogFd      = -1;   // main log
static int              g_CrashFd    = -1;   // crash log
static std::atomic<bool> g_LogReady  {false};
static std::string      g_LogDir;
static std::string      g_PackageName;
static std::string      g_StartupFlagPath;
static std::string      g_SafeModePath;

// async-signal-safe single-string writer
static inline void safeWriteStr(int fd, const char* s) {
    if (fd < 0 || !s) return;
    size_t len = strlen(s);
    while (len > 0) {
        ssize_t n = write(fd, s, len);
        if (n <= 0) break;
        s += n;
        len -= (size_t)n;
    }
}

// async-signal-safe int -> string
static int safeItoa(long val, char* buf, int bufsz) {
    if (bufsz < 2) return 0;
    char tmp[24];
    int n = 0;
    bool neg = false;
    if (val < 0) { neg = true; val = -val; }
    if (val == 0) tmp[n++] = '0';
    while (val > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (val % 10));
        val /= 10;
    }
    int k = 0;
    if (neg && k < bufsz - 1) buf[k++] = '-';
    while (n > 0 && k < bufsz - 1) buf[k++] = tmp[--n];
    buf[k] = '\0';
    return k;
}

// normal-path formatted writer (uses vsnprintf, fine for non-signal paths)
static void logWrite(const char* level, const char* fmt, ...) {
    char buf[1024];
    int pos = 0;

    // timestamp
    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[%02d:%02d:%02d][%s] ",
                    tmv.tm_hour, tmv.tm_min, tmv.tm_sec, level);

    va_list ap; va_start(ap, fmt);
    pos += vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
    va_end(ap);

    if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
    else { buf[sizeof(buf) - 2] = '\n'; buf[sizeof(buf) - 1] = '\0'; }

    // logcat (always)
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);

    // file (if ready) — append + fsync
    if (g_LogReady.load() && g_LogFd >= 0) {
        safeWriteStr(g_LogFd, buf);
        fsync(g_LogFd);
    }
}

#define LOGI(...) logWrite("I", __VA_ARGS__)
#define LOGW(...) logWrite("W", __VA_ARGS__)
#define LOGE(...) logWrite("E", __VA_ARGS__)

// ================================================================
//  📁  Find writable log dir (game's own external files)
// ================================================================
static std::string getPackageName() {
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        (void)n;
        close(fd);
    }
    std::string s(buf);
    size_t p = s.find('\0');
    if (p != std::string::npos) s = s.substr(0, p);
    // strip trailing ":process_name" (e.g. com.foo:Unity)
    p = s.find(':');
    if (p != std::string::npos) s = s.substr(0, p);
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
    cands.push_back("/sdcard/ModXLab");
    cands.push_back("/data/local/tmp/ModXLab");

    for (const auto& c : cands) {
        if (!ensureDir(c)) continue;
        std::string test = c + "/.w";
        int fd = open(test.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd >= 0) { close(fd); unlink(test.c_str()); return c; }
    }
    return "";
}

// ================================================================
//  🛑  ASYNC-SIGNAL-SAFE CRASH HANDLER
// ================================================================
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

static void crashHandler(int sig, siginfo_t* info, void* ctx) {
    (void)ctx;

    // 1. FIRST — disable ALL handlers to prevent recursion
    signal(SIGSEGV, SIG_DFL);
    signal(SIGABRT, SIG_DFL);
    signal(SIGBUS,  SIG_DFL);
    signal(SIGILL,  SIG_DFL);
    signal(SIGFPE,  SIG_DFL);

    // 2. Write minimum viable crash info (async-signal-safe)
    char buf[256];
    int n = 0;
    n += snprintf(buf + n, sizeof(buf) - n, "\n\n=== CRASH: %s (%d) ===\n", sigName(sig), sig);
    n += snprintf(buf + n, sizeof(buf) - n, "Boot step: %s\n", bootStepName(g_BootStep.load()));
    if (info) {
        n += snprintf(buf + n, sizeof(buf) - n, "Fault addr: %p\n", info->si_addr);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "PID: %d  TID: %d\n",
                  (int)getpid(), (int)(unsigned long)pthread_self());
    safeWriteStr(g_CrashFd, buf);
    safeWriteStr(g_LogFd,   buf);
    if (g_CrashFd >= 0) fsync(g_CrashFd);
    if (g_LogFd   >= 0) fsync(g_LogFd);

    // 3. Attempt backtrace — if this crashes, handlers are already SIG_DFL,
    //    process terminates cleanly and we still have the info above.
    {
        void* bt[64];
        struct BacktraceState { void** cur; void** end; } st = { bt, bt + 64 };
        _Unwind_Backtrace([](struct _Unwind_Context* c, void* a) -> _Unwind_Reason_Code {
            auto* s = static_cast<BacktraceState*>(a);
            uintptr_t pc = _Unwind_GetIP(c);
            if (pc) { if (s->cur == s->end) return _URC_END_OF_STACK; *(s->cur++) = (void*)pc; }
            return _URC_NO_REASON;
        }, &st);

        int count = (int)(st.cur - bt);
        safeWriteStr(g_CrashFd, "--- Backtrace ---\n");
        for (int i = 0; i < count; i++) {
            Dl_info dl;
            n = 0;
            if (dladdr(bt[i], &dl) && dl.dli_fname) {
                uintptr_t off = (uintptr_t)bt[i] - (uintptr_t)dl.dli_fbase;
                n += snprintf(buf + n, sizeof(buf) - n,
                              "  #%02d  %p  %s  (%s + 0x%lx)\n",
                              i, bt[i], dl.dli_sname ? dl.dli_sname : "?",
                              dl.dli_fname, (unsigned long)off);
            } else {
                n += snprintf(buf + n, sizeof(buf) - n, "  #%02d  %p  <unknown>\n", i, bt[i]);
            }
            safeWriteStr(g_CrashFd, buf);
        }
        if (g_CrashFd >= 0) fsync(g_CrashFd);
    }

    // 4. Re-raise to let OS handle (dump tombstone etc.)
    raise(sig);
}

static void installCrashHandler() {
    // Use fixed-size stack (SIGSTKSZ is NOT a constant in NDK 26!)
    static char s_altStack[64 * 1024];
    stack_t ss;
    ss.ss_sp    = s_altStack;
    ss.ss_size  = sizeof(s_altStack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, nullptr) != 0) {
        LOGW("[CRASH] sigaltstack failed — proceeding without alt stack");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);
}

// ================================================================
//  📁  INIT LOGGING (bulletproof)
// ================================================================
static void initLogging() {
    g_BootStep = 1;

    // 1. locate directory
    g_LogDir = findLogDir();
    if (g_LogDir.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "No writable log dir — logcat only");
        return;
    }

    // 2. open main + crash files
    std::string mainPath  = g_LogDir + "/modxlab.log";
    std::string crashPath = g_LogDir + "/modxlab_crash.log";
    g_LogFd   = open(mainPath.c_str(),  O_WRONLY | O_CREAT | O_APPEND, 0666);
    g_CrashFd = open(crashPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (g_LogFd >= 0) {
        g_LogReady.store(true);
        // 3. Write IMMEDIATELY via raw syscall (before any stdio touches this)
        const char* banner =
            "\n========================================\n"
            "ModXLab Session Start\n"
            "========================================\n";
        safeWriteStr(g_LogFd, banner);
        fsync(g_LogFd);
    }

    // 4. Initialize path variables
    g_StartupFlagPath = g_LogDir + "/startup_pending";
    g_SafeModePath    = g_LogDir + "/SAFE_MODE";

    // 5. Info logs (now safe — g_LogReady is set)
    LOGI("Package    : %s", g_PackageName.c_str());
    LOGI("Log dir    : %s", g_LogDir.c_str());
    LOGI("PID        : %d", (int)getpid());
}

// ================================================================
//  🔁  CRASH-LOOP DETECTION (startup_pending flag)
// ================================================================
static bool previousStartupCrashed() {
    return !g_StartupFlagPath.empty() && access(g_StartupFlagPath.c_str(), F_OK) == 0;
}
static bool safeModeFileExists() {
    return !g_SafeModePath.empty() && access(g_SafeModePath.c_str(), F_OK) == 0;
}
static void markStartupPending() {
    if (g_StartupFlagPath.empty()) return;
    int fd = open(g_StartupFlagPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { (void)!write(fd, "1", 1); close(fd); }
}
static void markStartupComplete() {
    if (!g_StartupFlagPath.empty()) unlink(g_StartupFlagPath.c_str());
}

// ================================================================
//  TIME HELPER
// ================================================================
static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//  ⚙️  FEATURE STATE
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
//  HOOK TRACKING / LOGGING MACROS
// ================================================================
#define TRACK_FIRST_CALL(tag) do { \
    static std::atomic<bool> _fc{true}; \
    if (_fc.exchange(false, std::memory_order_relaxed)) { \
        LOGI("[FIRST-CALL] %s", tag); \
    } \
} while(0)

#define HOOK_AND_LOG(className, methodName, addr, replace, orig) do { \
    LOGI("[HOOK TRY]  %s.%s @ 0x%lx", className, methodName, (unsigned long)(addr)); \
    HOOK_AU((void*)(addr), (void*)(replace), (orig)); \
    if ((orig) != nullptr) { \
        LOGI("[HOOK OK]   %s.%s", className, methodName); \
    } else { \
        LOGE("[HOOK FAIL] %s.%s  — original pointer is null", className, methodName); \
    } \
} while(0)

#define LOG_SKIP(className, methodName) \
    LOGE("[HOOK SKIP] %s.%s — offset not found", className, methodName)

// ================================================================
//  FUNCTION POINTERS (existing + new)
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
//  HOOK IMPLEMENTATIONS — EXISTING
// ================================================================
static bool new_CheckFrontalImpact(void* self, void* other) {
    TRACK_FIRST_CALL("CharacterMotor.CheckFrontalImpact");
    if (!self) return false;
    if (g_FrontalImpact.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}
static bool new_CheckSideImpact(void* self, void* other) {
    TRACK_FIRST_CALL("CharacterMotor.CheckSideImpact");
    if (!self) return false;
    if (g_SideImpact.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}
static int new_get_JumpLimit(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_JumpLimit");
    if (!self) return 0;
    int v = g_JumpLimit.load();
    return (v > 0) ? v : (old_get_JumpLimit ? old_get_JumpLimit(self) : 0);
}
static float new_get_JumpHeight(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_JumpHeight");
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    return (v > 0) ? (float)v : (old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f);
}
static float new_get_LaneChangeDuration(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_LaneChangeDuration");
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}
static bool new_IsAutoReviveEnabled(void* self) {
    TRACK_FIRST_CALL("StumbleBehaviour.IsAutoReviveEnabled");
    if (!self) return false;
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}
static float new_GetDuration(void* self, void* cfg) {
    TRACK_FIRST_CALL("PowerConfig.GetDuration");
    if (!self) return 0.0f;
    int v = g_PowerDuration.load();
    return (v > 0) ? (float)v : (old_GetDuration ? old_GetDuration(self, cfg) : 0.0f);
}
static int new_get_BaseMultiplierSum(void* self) {
    TRACK_FIRST_CALL("ScoreMultiplierManager.get_BaseMultiplierSum");
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    return (v > 0) ? v : (old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0);
}
static bool new_get_detectCollisions(void* self) {
    TRACK_FIRST_CALL("CharacterController.get_detectCollisions");
    if (!self) return true;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0 && (nowMs() - start) < 1000) return false;
    }
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}
static int new_GetCurrency(void* self, int currencyType) {
    TRACK_FIRST_CALL("WalletModel.GetCurrency");
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//  HOOK IMPLEMENTATIONS — NEW
// ================================================================
static bool new_get_IsIAP(void* self) {
    TRACK_FIRST_CALL("Currency.get_IsIAP");
    if (g_UnlimitedShop.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}
static void new_ApplyGravity(void* self) {
    TRACK_FIRST_CALL("CharacterMotor.ApplyGravity");
    if (g_NoGravity.load()) return;
    if (old_ApplyGravity) old_ApplyGravity(self);
}
static float new_get_MinSpeed(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_MinSpeed");
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MinSpeed ? old_get_MinSpeed(self) : 0.0f;
}
static float new_get_MaxSpeed(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_MaxSpeed");
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MaxSpeed ? old_get_MaxSpeed(self) : 0.0f;
}
static void new_UpdateMovement(void* self) {
    TRACK_FIRST_CALL("CharacterMotor.UpdateMovement");
    if (g_FreezeCharacter.load()) return;
    if (old_UpdateMovement) old_UpdateMovement(self);
}
static float new_get_AirJumpHeight(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_AirJumpHeight");
    int v = g_AirJumpHeight.load();
    if (v > 0) return (float)v;
    return old_get_AirJumpHeight ? old_get_AirJumpHeight(self) : 0.0f;
}
static bool new_CameraApply(void* self, void* context) {
    TRACK_FIRST_CALL("CameraGroundedModifier.Apply");
    int mode = g_CameraMode.load();
    if (mode == 1) return true;
    if (mode == 2) return false;
    return old_CameraApply ? old_CameraApply(self, context) : false;
}
static void new_NotifySideImpact(void* self, int h, int e, void* ent) {
    TRACK_FIRST_CALL("CharacterMotor.NotifySideImpact");
    if (g_NoBoundaryImpact.load()) return;
    if (old_NotifySideImpact) old_NotifySideImpact(self, h, e, ent);
}
static void new_OnStumble(void* self, int dc) {
    TRACK_FIRST_CALL("RunProgressOrderIndicator.OnStumble");
    if (g_NoStumble.load()) return;
    if (old_OnStumble) old_OnStumble(self, dc);
}
static void new_ShowFlight(void* self, bool show, int mode) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowFlight");
    if (g_HideJetpack.load()) return;
    if (old_ShowFlight) old_ShowFlight(self, show, mode);
}
static void new_ShowPogostick(void* self, bool show) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowPogostick");
    if (g_HidePogostick.load()) return;
    if (old_ShowPogostick) old_ShowPogostick(self, show);
}
static void new_ShowMagnet(void* self, bool show) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowMagnet");
    if (g_HideMagnet.load()) return;
    if (old_ShowMagnet) old_ShowMagnet(self, show);
}
static void new_ShowHoverboard(void* self, bool rb) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowHoverboard");
    if (g_HideHoverboard.load()) return;
    if (old_ShowHoverboard) old_ShowHoverboard(self, rb);
}
static void new_Displace(void* self) {
    TRACK_FIRST_CALL("MovingTrain.Displace");
    if (g_StopTrains.load()) return;
    if (old_Displace) old_Displace(self);
}
static void new_Addcoins(void* self, int coins) {
    TRACK_FIRST_CALL("RunSessionData.Addcoins");
    int b = g_AddCoinsBonus.load();
    if (old_Addcoins) {
        if (b > 0) old_Addcoins(self, coins + b);
        else       old_Addcoins(self, coins);
    }
}
static void new_AddKeys(void* self, int keys) {
    TRACK_FIRST_CALL("RunSessionData.AddKeys");
    int b = g_AddKeysBonus.load();
    if (old_AddKeys) {
        if (b > 0) old_AddKeys(self, keys + b);
        else       old_AddKeys(self, keys);
    }
}
static void new_AddPoints(void* self, double points) {
    TRACK_FIRST_CALL("RunSessionData.AddPoints");
    int b = g_AddScoreBonus.load();
    if (old_AddPoints) {
        if (b > 0) old_AddPoints(self, points + (double)b);
        else       old_AddPoints(self, points);
    }
}
static void new_UpdatePowers(void* self) {
    TRACK_FIRST_CALL("CharacterPower.UpdatePowers");
    if (g_UnlimitedPowerTime.load()) return;
    if (old_UpdatePowers) old_UpdatePowers(self);
}
static void new_MainUpdatePowers(void* self) {
    TRACK_FIRST_CALL("MainPowerSystem.UpdatePowers");
    if (g_UnlimitedPowerTime.load()) return;
    if (old_MainUpdatePowers) old_MainUpdatePowers(self);
}
static void new_FinishFlight(void* self) {
    TRACK_FIRST_CALL("FlyingState.FinishFlight");
    if (g_UnlimitedJetpackTime.load()) return;
    if (old_FinishFlight) old_FinishFlight(self);
}
static void new_OnRevivedWithCurrency(void* self) {
    TRACK_FIRST_CALL("MainReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++)
        if (old_OnRevivedWithCurrency) old_OnRevivedWithCurrency(self);
}
static void new_MockRevivedWithCurrency(void* self) {
    TRACK_FIRST_CALL("MockReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++)
        if (old_MockRevivedWithCurrency) old_MockRevivedWithCurrency(self);
}
static void new_HandleCoinsPickedUp(void* self) {
    TRACK_FIRST_CALL("GameScreenCoinCounter.HandleOnCoinsPickedUp");
    if (g_FreezeCoins.load()) return;
    if (old_HandleCoinsPickedUp) old_HandleCoinsPickedUp(self);
}
static void new_HandlePointsEarned(void* self) {
    TRACK_FIRST_CALL("GameScreenScoreCounter.HandleOnPointsEarned");
    if (g_FreezeScore.load()) return;
    if (old_HandlePointsEarned) old_HandlePointsEarned(self);
}

// ================================================================
//  HEARTBEAT
// ================================================================
static void* heartbeat_thread(void*) {
    int n = 0;
    while (true) {
        sleep(60); n++;
        LOGI("[HEARTBEAT %02d] alive | chams=%d mode=%d step=%s",
             n, (int)g_ChamsEnabled, chamsint, bootStepName(g_BootStep.load()));
        // If we've been alive for 2 minutes without crash, mark startup complete
        if (n == 2) {
            markStartupComplete();
            LOGI("[BOOT] Startup marked complete — future crashes won't trigger safe mode");
        }
    }
    return nullptr;
}

// ================================================================
//  HOOK INSTALLATION PHASE 1 — PROVEN SAFE
// ================================================================
static void installPhase1() {
    LOGI("=== PHASE 1: installing original/proven hooks ===");

    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (C && C->thisclass) {
            DWORD oF = C->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oS = C->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oF) HOOK_AND_LOG("CharacterMotor", "CheckFrontalImpact", oF, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            else    LOG_SKIP("CharacterMotor", "CheckFrontalImpact");
            if (oS) HOOK_AND_LOG("CharacterMotor", "CheckSideImpact", oS, (void*)new_CheckSideImpact, old_CheckSideImpact);
            else    LOG_SKIP("CharacterMotor", "CheckSideImpact");
        } else LOGE("[CLASS MISS] CharacterMotor");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (C && C->thisclass) {
            DWORD oJL = C->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJH = C->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLC = C->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oJL) HOOK_AND_LOG("CharacterMotorAbilities", "get_JumpLimit", oJL, (void*)new_get_JumpLimit, old_get_JumpLimit);
            else     LOG_SKIP("CharacterMotorAbilities", "get_JumpLimit");
            if (oJH) HOOK_AND_LOG("CharacterMotorAbilities", "get_JumpHeight", oJH, (void*)new_get_JumpHeight, old_get_JumpHeight);
            else     LOG_SKIP("CharacterMotorAbilities", "get_JumpHeight");
            if (oLC) HOOK_AND_LOG("CharacterMotorAbilities", "get_LaneChangeDuration", oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
            else     LOG_SKIP("CharacterMotorAbilities", "get_LaneChangeDuration");
        } else LOGE("[CLASS MISS] CharacterMotorAbilities");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (o) HOOK_AND_LOG("StumbleBehaviour", "IsAutoReviveEnabled", o, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
            else   LOG_SKIP("StumbleBehaviour", "IsAutoReviveEnabled");
        } else LOGE("[CLASS MISS] StumbleBehaviour");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (o) HOOK_AND_LOG("PowerConfig", "GetDuration", o, (void*)new_GetDuration, old_GetDuration);
            else   LOG_SKIP("PowerConfig", "GetDuration");
        } else LOGE("[CLASS MISS] PowerConfig");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (o) HOOK_AND_LOG("ScoreMultiplierManager", "get_BaseMultiplierSum", o, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
            else   LOG_SKIP("ScoreMultiplierManager", "get_BaseMultiplierSum");
        } else LOGE("[CLASS MISS] ScoreMultiplierManager");
    }
    {
        auto C = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (o) HOOK_AND_LOG("CharacterController", "get_detectCollisions", o, (void*)new_get_detectCollisions, old_get_detectCollisions);
            else   LOG_SKIP("CharacterController", "get_detectCollisions");
        } else LOGE("[CLASS MISS] CharacterController");
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (o) HOOK_AND_LOG("WalletModel", "GetCurrency", o, (void*)new_GetCurrency, old_GetCurrency);
            else   LOG_SKIP("WalletModel", "GetCurrency");
        } else LOGE("[CLASS MISS] WalletModel");
    }
    LOGI("=== PHASE 1 complete ===");
}

// ================================================================
//  HOOK INSTALLATION PHASE 2 — NEW (risky, logged one-by-one)
// ================================================================
static void installPhase2() {
    LOGI("=== PHASE 2: installing new hooks ===");

    // 1) Currency.get_IsIAP
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (o) HOOK_AND_LOG("Currency", "get_IsIAP", o, (void*)new_get_IsIAP, old_get_IsIAP);
            else   LOG_SKIP("Currency", "get_IsIAP");
        } else LOGE("[CLASS MISS] Currency");
    }

    // 2) CharacterMotor: ApplyGravity, UpdateMovement, NotifySideImpact
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (C && C->thisclass) {
            DWORD oG = C->GetMethodOffsetByName(OBFUSCATE("ApplyGravity"),     0);
            DWORD oU = C->GetMethodOffsetByName(OBFUSCATE("UpdateMovement"),   0);
            DWORD oN = C->GetMethodOffsetByName(OBFUSCATE("NotifySideImpact"), 3);
            if (oG) HOOK_AND_LOG("CharacterMotor", "ApplyGravity",     oG, (void*)new_ApplyGravity,     old_ApplyGravity);
            else    LOG_SKIP("CharacterMotor", "ApplyGravity");
            if (oU) HOOK_AND_LOG("CharacterMotor", "UpdateMovement",   oU, (void*)new_UpdateMovement,   old_UpdateMovement);
            else    LOG_SKIP("CharacterMotor", "UpdateMovement");
            if (oN) HOOK_AND_LOG("CharacterMotor", "NotifySideImpact", oN, (void*)new_NotifySideImpact, old_NotifySideImpact);
            else    LOG_SKIP("CharacterMotor", "NotifySideImpact");
        }
    }

    // 3) CharacterMotorAbilities: MinSpeed, MaxSpeed, AirJumpHeight
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (C && C->thisclass) {
            DWORD oMin = C->GetMethodOffsetByName(OBFUSCATE("get_MinSpeed"),      0);
            DWORD oMax = C->GetMethodOffsetByName(OBFUSCATE("get_MaxSpeed"),      0);
            DWORD oAJH = C->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"), 0);
            if (oMin) HOOK_AND_LOG("CharacterMotorAbilities", "get_MinSpeed",      oMin, (void*)new_get_MinSpeed,      old_get_MinSpeed);
            else      LOG_SKIP("CharacterMotorAbilities", "get_MinSpeed");
            if (oMax) HOOK_AND_LOG("CharacterMotorAbilities", "get_MaxSpeed",      oMax, (void*)new_get_MaxSpeed,      old_get_MaxSpeed);
            else      LOG_SKIP("CharacterMotorAbilities", "get_MaxSpeed");
            if (oAJH) HOOK_AND_LOG("CharacterMotorAbilities", "get_AirJumpHeight", oAJH, (void*)new_get_AirJumpHeight, old_get_AirJumpHeight);
            else      LOG_SKIP("CharacterMotorAbilities", "get_AirJumpHeight");
        }
    }

    // 4) CameraGroundedModifier.Apply
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraGroundedModifier"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Apply"), 1);
            if (o) HOOK_AND_LOG("CameraGroundedModifier", "Apply", o, (void*)new_CameraApply, old_CameraApply);
            else   LOG_SKIP("CameraGroundedModifier", "Apply");
        } else LOGE("[CLASS MISS] CameraGroundedModifier");
    }

    // 5) RunProgressOrderIndicator.OnStumble
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunProgressOrderIndicator"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnStumble"), 1);
            if (o) HOOK_AND_LOG("RunProgressOrderIndicator", "OnStumble", o, (void*)new_OnStumble, old_OnStumble);
            else   LOG_SKIP("RunProgressOrderIndicator", "OnStumble");
        } else LOGE("[CLASS MISS] RunProgressOrderIndicator");
    }

    // 6) CharacterPropsController (4 hooks)
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPropsController"));
        if (C && C->thisclass) {
            DWORD oF = C->GetMethodOffsetByName(OBFUSCATE("ShowFlight"),     2);
            DWORD oP = C->GetMethodOffsetByName(OBFUSCATE("ShowPogostick"),  1);
            DWORD oM = C->GetMethodOffsetByName(OBFUSCATE("ShowMagnet"),     1);
            DWORD oH = C->GetMethodOffsetByName(OBFUSCATE("ShowHoverboard"), 1);
            if (oF) HOOK_AND_LOG("CharacterPropsController", "ShowFlight",     oF, (void*)new_ShowFlight,     old_ShowFlight);
            else    LOG_SKIP("CharacterPropsController", "ShowFlight");
            if (oP) HOOK_AND_LOG("CharacterPropsController", "ShowPogostick",  oP, (void*)new_ShowPogostick,  old_ShowPogostick);
            else    LOG_SKIP("CharacterPropsController", "ShowPogostick");
            if (oM) HOOK_AND_LOG("CharacterPropsController", "ShowMagnet",     oM, (void*)new_ShowMagnet,     old_ShowMagnet);
            else    LOG_SKIP("CharacterPropsController", "ShowMagnet");
            if (oH) HOOK_AND_LOG("CharacterPropsController", "ShowHoverboard", oH, (void*)new_ShowHoverboard, old_ShowHoverboard);
            else    LOG_SKIP("CharacterPropsController", "ShowHoverboard");
        } else LOGE("[CLASS MISS] CharacterPropsController");
    }

    // 7) MovingTrain.Displace
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Displace"), 0);
            if (o) HOOK_AND_LOG("MovingTrain", "Displace", o, (void*)new_Displace, old_Displace);
            else   LOG_SKIP("MovingTrain", "Displace");
        } else LOGE("[CLASS MISS] MovingTrain");
    }

    // 8) RunSessionData (3 hooks)
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (C && C->thisclass) {
            DWORD oC = C->GetMethodOffsetByName(OBFUSCATE("Addcoins"),  1);
            DWORD oK = C->GetMethodOffsetByName(OBFUSCATE("AddKeys"),   1);
            DWORD oP = C->GetMethodOffsetByName(OBFUSCATE("AddPoints"), 1);
            if (oC) HOOK_AND_LOG("RunSessionData", "Addcoins",  oC, (void*)new_Addcoins,  old_Addcoins);
            else    LOG_SKIP("RunSessionData", "Addcoins");
            if (oK) HOOK_AND_LOG("RunSessionData", "AddKeys",   oK, (void*)new_AddKeys,   old_AddKeys);
            else    LOG_SKIP("RunSessionData", "AddKeys");
            if (oP) HOOK_AND_LOG("RunSessionData", "AddPoints", oP, (void*)new_AddPoints, old_AddPoints);
            else    LOG_SKIP("RunSessionData", "AddPoints");
        } else LOGE("[CLASS MISS] RunSessionData");
    }

    // 9) CharacterPower.UpdatePowers
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPower"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AND_LOG("CharacterPower", "UpdatePowers", o, (void*)new_UpdatePowers, old_UpdatePowers);
            else   LOG_SKIP("CharacterPower", "UpdatePowers");
        } else LOGE("[CLASS MISS] CharacterPower");
    }

    // 10) MainPowerSystem.UpdatePowers
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainPowerSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AND_LOG("MainPowerSystem", "UpdatePowers", o, (void*)new_MainUpdatePowers, old_MainUpdatePowers);
            else   LOG_SKIP("MainPowerSystem", "UpdatePowers");
        } else LOGE("[CLASS MISS] MainPowerSystem");
    }

    // 11) FlyingState.FinishFlight
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("FlyingState"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("FinishFlight"), 0);
            if (o) HOOK_AND_LOG("FlyingState", "FinishFlight", o, (void*)new_FinishFlight, old_FinishFlight);
            else   LOG_SKIP("FlyingState", "FinishFlight");
        } else LOGE("[CLASS MISS] FlyingState");
    }

    // 12) MainReviveSystem.OnRevivedWithCurrency
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AND_LOG("MainReviveSystem", "OnRevivedWithCurrency", o, (void*)new_OnRevivedWithCurrency, old_OnRevivedWithCurrency);
            else   LOG_SKIP("MainReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MainReviveSystem");
    }

    // 13) MockReviveSystem.OnRevivedWithCurrency
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems.Mocks"), OBFUSCATE("MockReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AND_LOG("MockReviveSystem", "OnRevivedWithCurrency", o, (void*)new_MockRevivedWithCurrency, old_MockRevivedWithCurrency);
            else   LOG_SKIP("MockReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MockReviveSystem");
    }

    // 14) GameScreenCoinCounter.HandleOnCoinsPickedUp
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenCoinCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnCoinsPickedUp"), 0);
            if (o) HOOK_AND_LOG("GameScreenCoinCounter", "HandleOnCoinsPickedUp", o, (void*)new_HandleCoinsPickedUp, old_HandleCoinsPickedUp);
            else   LOG_SKIP("GameScreenCoinCounter", "HandleOnCoinsPickedUp");
        } else LOGE("[CLASS MISS] GameScreenCoinCounter");
    }

    // 15) GameScreenScoreCounter.HandleOnPointsEarned
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenScoreCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnPointsEarned"), 0);
            if (o) HOOK_AND_LOG("GameScreenScoreCounter", "HandleOnPointsEarned", o, (void*)new_HandlePointsEarned, old_HandlePointsEarned);
            else   LOG_SKIP("GameScreenScoreCounter", "HandleOnPointsEarned");
        } else LOGE("[CLASS MISS] GameScreenScoreCounter");
    }

    LOGI("=== PHASE 2 complete ===");
}

// ================================================================
//  HACK THREAD
// ================================================================
void* hack_thread(void*) {
    g_BootStep = 3;
    LOGI(">>> hack_thread started <<<");

    // --- read LIB once as std::string (avoids variadic OBFUSCATE issue) ---
    std::string libName = LIB;
    LOGI("Waiting for %s ...", libName.c_str());
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    g_BootStep = 4;
    LOGI(">>> library loaded <<<");

    // --- module base (helps RVA from crash backtrace) ---
    {
        void* sym = dlsym(RTLD_DEFAULT, "il2cpp_init");
        if (sym) {
            Dl_info info;
            if (dladdr(sym, &info)) {
                LOGI("[MODULE] %s  base = %p", info.dli_fname, info.dli_fbase);
            }
        } else LOGW("[MODULE] il2cpp_init not found");
    }
    g_BootStep = 5;

    // --- crash-loop / safe-mode protection ---
    if (safeModeFileExists()) {
        LOGE("[SAFE MODE] SAFE_MODE file present — skipping ALL hook installs.");
        LOGE("[SAFE MODE] File: %s", g_SafeModePath.c_str());
        LOGE("[SAFE MODE] Delete this file to re-enable hooks.");
        g_BootStep = 10;
        return nullptr;
    }
    if (previousStartupCrashed()) {
        LOGE("[SAFE MODE] Previous startup crashed (startup_pending flag).");
        LOGE("[SAFE MODE] Skipping ALL hook installs this launch.");
        LOGE("[SAFE MODE] The game should now start normally.");
        LOGE("[SAFE MODE] To re-enable hooks, delete: %s", g_StartupFlagPath.c_str());
        unlink(g_StartupFlagPath.c_str());
        g_BootStep = 10;
        return nullptr;
    }
    markStartupPending();
    LOGI("[BOOT] Startup pending flag set. Will clear after 2 stable minutes.");

    // --- install hooks ---
    g_BootStep = 6;
    installPhase1();

    sleep(2);   // let game breathe between phases

    g_BootStep = 7;
    installPhase2();

    sleep(1);

    // --- GLES hooks for chams ---
    g_BootStep = 8;
    LOGI(">>> installing GLES render hooks (chams) <<<");
    RenderHook();
    LOGI(">>> GLES hooks done <<<");

    // --- heartbeat (delayed completion marker) ---
    g_BootStep = 9;
    pthread_t hb;
    pthread_create(&hb, nullptr, heartbeat_thread, nullptr);

    g_BootStep = 10;
    LOGI("========================================");
    LOGI("ALL HOOKS INSTALLED — startup pass complete.");
    LOGI("Watch for [FIRST-CALL] lines to verify each hook fires.");
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
    ret = (jobjectArray) env->NewObjectArray(
        n, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < n; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

// ================================================================
//  JNI — CHANGES HANDLER
// ================================================================
void Changes(JNIEnv*, jclass, jobject,
             jint featNum, jstring, jint value,
             jlong, jboolean boolean, jstring) {
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
                g_JumpLimit.store(0); g_JumpHeight.store(0);
                g_PowerDuration.store(0); g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0); g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
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
                g_ScoreMultiplier.store(10); g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
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
//  CONSTRUCTOR — order matters: log FIRST, crash handler SECOND
// ================================================================
__attribute__((constructor))
void lib_main() {
    g_BootStep = 0;

    // 1) Logging first — so any crash during installCrashHandler is logged
    initLogging();

    // 2) Crash handler second — protects hack_thread
    g_BootStep = 2;
    installCrashHandler();
    LOGI("[BOOT] Crash handler installed.");

    // 3) Spawn worker thread
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}