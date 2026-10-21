// Main.cpp
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
//  BOOT STEP
// ================================================================
static std::atomic<int> g_BootStep{0};
static const char* bootStepName(int s) {
    static const char* names[] = {
        "0-constructor","1-logging","2-crash-handler","3-hack-thread",
        "4-lib-loaded","5-module-base","6-phase1","7-phase2a",
        "8-phase2b","9-phase2c","10-phase3","11-phase4",
        "12-phase5","13-render","14-complete"
    };
    return (s >= 0 && s < 15) ? names[s] : "?";
}

// ================================================================
//  FILE HANDLES
// ================================================================
static int               g_LogFd   = -1;
static int               g_CrashFd = -1;
static std::atomic<bool> g_LogReady{false};
static std::string       g_LogDir;
static std::string       g_PackageName;
static std::string       g_StartupFlagPath;
static std::string       g_SafeModePath;

// ================================================================
//  ASYNC-SAFE HELPERS
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

// ================================================================
//  LOGGER
// ================================================================
static void logWrite(const char* level, const char* fmt, ...) {
    char buf[1024];
    int pos = 0;
    time_t now = time(nullptr);
    struct tm tmv; localtime_r(&now, &tmv);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "[%02d:%02d:%02d][%s][%lx] ",
                    tmv.tm_hour, tmv.tm_min, tmv.tm_sec, level,
                    (unsigned long)pthread_self());
    va_list ap; va_start(ap, fmt);
    pos += vsnprintf(buf + pos, sizeof(buf) - pos, fmt, ap);
    va_end(ap);
    if (pos < (int)sizeof(buf) - 1) { buf[pos++] = '\n'; buf[pos] = '\0'; }
    else { buf[sizeof(buf) - 2] = '\n'; buf[sizeof(buf) - 1] = '\0'; }
    __android_log_write(ANDROID_LOG_INFO, LOG_TAG, buf);
    if (g_LogReady.load() && g_LogFd >= 0) safeWriteStr(g_LogFd, buf);
}
#define LOGI(...) logWrite("I", __VA_ARGS__)
#define LOGW(...) logWrite("W", __VA_ARGS__)
#define LOGE(...) logWrite("E", __VA_ARGS__)

// ================================================================
//  PATH HELPERS
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
//  CRASH HANDLER
// ================================================================
struct SavedHandler { struct sigaction prev; bool has_prev; };
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

static void crashHandler(int sig, siginfo_t* info, void* ctx) {
    for (int i = 0; i < 5; i++) signal(g_SigList[i], SIG_DFL);
    alarm(3);

    const char* hdr = "\n\n========================================\nNATIVE CRASH DETECTED\n========================================\n";
    safeWriteStr(g_CrashFd, hdr);
    safeWriteStr(g_LogFd,   hdr);

    char buf[256];

    safeWriteStr(g_CrashFd, "Signal  : "); safeWriteStr(g_CrashFd, sigName(sig)); safeWriteStr(g_CrashFd, "\n");
    safeWriteStr(g_LogFd,   "Signal  : "); safeWriteStr(g_LogFd,   sigName(sig)); safeWriteStr(g_LogFd,   "\n");

    safeWriteStr(g_CrashFd, "Step    : "); safeWriteStr(g_CrashFd, bootStepName(g_BootStep.load())); safeWriteStr(g_CrashFd, "\n");
    safeWriteStr(g_LogFd,   "Step    : "); safeWriteStr(g_LogFd,   bootStepName(g_BootStep.load())); safeWriteStr(g_LogFd,   "\n");

    if (info) {
        char hex[24]; safeHexToStr((unsigned long)info->si_addr, hex, sizeof(hex));
        safeWriteStr(g_CrashFd, "Fault   : "); safeWriteStr(g_CrashFd, hex); safeWriteStr(g_CrashFd, "\n");
        safeWriteStr(g_LogFd,   "Fault   : "); safeWriteStr(g_LogFd,   hex); safeWriteStr(g_LogFd,   "\n");
    }

    if (g_CrashFd >= 0) fsync(g_CrashFd);
    if (g_LogFd   >= 0) fsync(g_LogFd);

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
        safeWriteStr(g_LogFd,   "--- Backtrace ---\n");
        for (int i = 0; i < cnt; i++) {
            Dl_info dl; int n = 0;
            if (dladdr(bt[i], &dl) && dl.dli_fname) {
                unsigned long off = (unsigned long)bt[i] - (unsigned long)dl.dli_fbase;
                char offHex[24]; safeHexToStr(off, offHex, sizeof(offHex));
                n = snprintf(buf, sizeof(buf), "  #%02d %s+%s\n", i, dl.dli_fname, offHex);
            } else {
                char hex[24]; safeHexToStr((unsigned long)bt[i], hex, sizeof(hex));
                n = snprintf(buf, sizeof(buf), "  #%02d %s\n", i, hex);
            }
            (void)n;
            safeWriteStr(g_CrashFd, buf);
            safeWriteStr(g_LogFd,   buf);
        }
        if (g_CrashFd >= 0) fsync(g_CrashFd);
        if (g_LogFd   >= 0) fsync(g_LogFd);
    }

    alarm(0);

    for (int i = 0; i < 5; i++) {
        if (g_SigList[i] == sig && g_Saved[i].has_prev) {
            struct sigaction& prev = g_Saved[i].prev;
            if (prev.sa_sigaction && prev.sa_sigaction != crashHandler) {
                prev.sa_sigaction(sig, info, ctx);
                return;
            } else if (prev.sa_handler && prev.sa_handler != SIG_DFL && prev.sa_handler != SIG_IGN) {
                prev.sa_handler(sig);
                return;
            }
        }
    }
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
    if (g_LogDir.empty()) return;
    
    std::string mainPath  = g_LogDir + "/modxlab.log";
    std::string crashPath = g_LogDir + "/modxlab_crash.log";

    g_LogFd   = open(mainPath.c_str(),  O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666);
    g_CrashFd = open(crashPath.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_SYNC, 0666);

    if (g_LogFd >= 0) {
        g_LogReady.store(true);
        safeWriteStr(g_LogFd, "\n\n========================================\nModXLab Session Start\n========================================\n");
    }
    g_StartupFlagPath = g_LogDir + "/startup_pending";
    g_SafeModePath    = g_LogDir + "/SAFE_MODE";

    LOGI("Package : %s", g_PackageName.c_str());
    LOGI("Log dir : %s", g_LogDir.c_str());
    LOGI("PID     : %d", (int)getpid());
}

// ================================================================
//  SAFE MODE FILES
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
static bool skipHookFile(const std::string& hookName) {
    if (g_LogDir.empty()) return false;
    return fileExists(g_LogDir + "/SKIP_" + hookName);
}

// ================================================================
//  PROLOGUE SAFETY CHECK
//  ------------------------------------------------------------------
//  A64HookFunction cannot safely hook tiny functions. Its trampoline
//  redirects the copied RET to (original + 0x10), which lands PAST
//  the function end → SIGILL.
//
//  This was the root cause of:
//    Signal : SIGILL
//    Fault  : 0x72cb5877a4  (= Currency.get_IsIAP + 0x10)
// ================================================================
static const char* checkFunctionSafety(void* addr) {
    if (!addr) return "null-addr";
    uint32_t* code = (uint32_t*)addr;
    uint32_t w0 = code[0], w1 = code[1], w2 = code[2], w3 = code[3];

    if (w0 == 0xD65F03C0) return "empty-function (RET at insn 0)";
    if (w1 == 0xD65F03C0) return "tiny-function (RET at insn 1)";
    if (w2 == 0xD65F03C0) return "tiny-function (RET at insn 2)";
    if (w3 == 0xD65F03C0) return "tiny-function (RET at insn 3)";

    for (int i = 0; i < 4; i++) {
        uint32_t w = code[i];
        if ((w & 0x7C000000) == 0x14000000) return "B in prologue";
        if ((w & 0x7C000000) == 0x94000000) return "BL in prologue";
        if ((w & 0xFFFF0000) == 0xD61F0000) return "BR in prologue";
        if ((w & 0xFFFF0000) == 0xD63F0000) return "BLR in prologue";
    }
    return nullptr;
}

// ================================================================
//  TIME
// ================================================================
static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//  FEATURE FLAGS
// ================================================================
static std::atomic<int>  g_JumpLimit        {0};
static std::atomic<int>  g_JumpHeight       {0};
static std::atomic<bool> g_AutoRevive       {false};
static std::atomic<int>  g_PowerDuration    {0};
static std::atomic<bool> g_FastLaneChange   {false};
static std::atomic<int>  g_ScoreMultiplier  {0};
static std::atomic<bool> g_NoCollision      {false};
static std::atomic<bool> g_UnlimitedCoins   {false};
static std::atomic<long long> g_NoCollisionStartMs{0};

// Free Shipping / Unlimited Shop — via MEMORY PATCH on get_IsIAP
static std::atomic<bool> g_UnlimitedShop       {false};

// No Impact (Front + Side) — single toggle, controls two hooks
static std::atomic<bool> g_NoImpact            {false};

static std::atomic<bool> g_NoGravity           {false};
static std::atomic<bool> g_FreezeCharacter     {false};
static std::atomic<int>  g_CharacterSpeed      {0};
static std::atomic<int>  g_AirJumpHeight       {0};
static std::atomic<int>  g_CameraMode          {0};
static std::atomic<bool> g_NoBoundaryImpact    {false};
static std::atomic<bool> g_HideJetpack         {false};
static std::atomic<bool> g_HidePogostick       {false};
static std::atomic<bool> g_HideMagnet          {false};
static std::atomic<bool> g_HideHoverboard      {false};
static std::atomic<bool> g_StopTrains          {false};
static std::atomic<int>  g_AddCoinsBonus       {0};
static std::atomic<int>  g_AddKeysBonus        {0};
static std::atomic<int>  g_AddScoreBonus       {0};
static std::atomic<bool> g_UnlimitedPowerTime  {false};
static std::atomic<bool> g_UnlimitedJettime    {false}; // (legacy, unused)
static std::atomic<bool> g_UnlimitedJetpackTime{false};
static std::atomic<bool> g_ReviveReward        {false};
static std::atomic<bool> g_FreezeCoins         {false};
static std::atomic<bool> g_FreezeScore         {false};

// ================================================================
//  HOOK MACROS
// ================================================================
#define TRACE_CALL(tag) \
    static std::atomic<int> _tc{0}; \
    int _myCall = _tc.fetch_add(1, std::memory_order_relaxed) + 1; \
    if (_myCall <= 3) LOGI("[ENTER] %s #%d", tag, _myCall)

#define TRACE_EXIT(tag) \
    if (_myCall <= 3) LOGI("[EXIT ] %s #%d", tag, _myCall)

#define HOOK_CHECK(className, methodName, addr, replace, orig) do { \
    if (skipHookFile(methodName)) { \
        LOGW("[HOOK SKIPFILE] %s.%s", className, methodName); \
        break; \
    } \
    { \
        uint32_t* _c = (uint32_t*)(addr); \
        LOGI("[HOOK TRY]  %s.%s @ %p  [%08x %08x %08x %08x]", \
             className, methodName, (void*)(addr), _c[0], _c[1], _c[2], _c[3]); \
    } \
    { \
        const char* _why = checkFunctionSafety((void*)(addr)); \
        if (_why) { \
            LOGE("[HOOK UNSAFE] %s.%s — %s, skipping", className, methodName, _why); \
            break; \
        } \
    } \
    HOOK_AU((void*)(addr), (void*)(replace), (orig)); \
    if ((orig) != nullptr) { \
        LOGI("[HOOK OK]   %s.%s", className, methodName); \
    } else { \
        LOGE("[HOOK FAIL] %s.%s — original pointer is null", className, methodName); \
    } \
} while(0)

// ================================================================
//  ORIGINAL POINTERS
// ================================================================
static int   (*old_get_JumpLimit)(void*)                      = nullptr;
static float (*old_get_JumpHeight)(void*)                     = nullptr;
static bool  (*old_IsAutoReviveEnabled)(void*)                = nullptr;
static float (*old_GetDuration)(void*, int)                   = nullptr;
static float (*old_get_LaneChangeDuration)(void*)             = nullptr;
static int   (*old_get_BaseMultiplierSum)(void*)              = nullptr;
static bool  (*old_get_detectCollisions)(void*)               = nullptr;
static int   (*old_GetCurrency)(void*, int)                   = nullptr;

// NOTE: No old_get_IsIAP — we use memory patch, not hook (see applyIsIAPPatch)

static void   (*old_ApplyGravity)(void*)                      = nullptr;
static float  (*old_get_MinSpeed)(void*)                      = nullptr;
static float  (*old_get_MaxSpeed)(void*)                      = nullptr;
static void   (*old_UpdateMovement)(void*)                    = nullptr;
static float  (*old_get_AirJumpHeight)(void*)                 = nullptr;
static bool   (*old_CameraApply)(void*, void*)                = nullptr;
static void   (*old_NotifySideImpact)(void*, int, void*, void*)= nullptr;

// NEW: No Impact hooks (Front + Side)
static bool   (*old_CheckFrontalImpact)(void*, void*)         = nullptr;
static bool   (*old_CheckSideImpact)(void*, void*)            = nullptr;

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
//  get_IsIAP — MEMORY PATCH STORAGE
//  ------------------------------------------------------------------
//  Original 4 instructions (b9401008 7100291f 1a9f17e0 d65f03c0):
//    LDR  W8, [X0, #0x10]
//    CMP  W8, #0xA
//    CSET W0, EQ
//    RET
//
//  Patch (MOV W0, #0; RET):
//    0x52800000  MOV W0, #0
//    0xD65F03C0  RET
//
//  Hex (little-endian): "00008052C0035FD6"
// ================================================================
static void*             g_IsIAPAddr = nullptr;
static uint8_t           g_IsIAPOriginal[16] = {0};
static std::atomic<bool> g_IsIAPOrigSaved{false};

static void bytesToHex(const uint8_t* bytes, size_t len, char* out) {
    static const char hex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i*2]     = hex[(bytes[i] >> 4) & 0xF];
        out[i*2 + 1] = hex[bytes[i] & 0xF];
    }
    out[len*2] = '\0';
}

// Apply or revert the get_IsIAP → return false patch.
// Safe to call any time (falls through if address not captured yet).
static void applyIsIAPPatch(bool enable) {
    if (!g_IsIAPAddr) {
        // Address not captured yet — installPhase2 will re-apply if toggle is on.
        LOGW("[PATCH DEFER] get_IsIAP address not ready (enable=%d)", (int)enable);
        return;
    }
    if (!g_IsIAPOrigSaved.load()) {
        LOGE("[PATCH FAIL] get_IsIAP original bytes not saved");
        return;
    }

    if (enable) {
        // MOV W0, #0; RET
        MemoryPatch patch = MemoryPatch::createWithHex(g_IsIAPAddr, "00008052C0035FD6");
        if (patch.isValid()) {
            patch.Modify();
            uint32_t* c = (uint32_t*)g_IsIAPAddr;
            if (c[0] == 0x52800000) {
                LOGI("[PATCH OK] get_IsIAP -> return false (verified)");
            } else {
                LOGE("[PATCH WARN] get_IsIAP verify failed: got 0x%08x", c[0]);
            }
        } else {
            LOGE("[PATCH FAIL] get_IsIAP enable (patch invalid)");
        }
    } else {
        char hex[33];
        bytesToHex(g_IsIAPOriginal, 16, hex);
        MemoryPatch patch = MemoryPatch::createWithHex(g_IsIAPAddr, hex);
        if (patch.isValid()) {
            patch.Modify();
            LOGI("[PATCH OK] get_IsIAP -> restored original");
        } else {
            LOGE("[PATCH FAIL] get_IsIAP restore (patch invalid)");
        }
    }
}

// ================================================================
//  HOOK IMPLEMENTATIONS
// ================================================================
static int new_get_JumpLimit(void* s) {
    int v = g_JumpLimit.load();
    if (v > 0) { TRACE_CALL("CharacterMotorAbilities.get_JumpLimit"); TRACE_EXIT("CharacterMotorAbilities.get_JumpLimit"); return v; }
    return old_get_JumpLimit ? old_get_JumpLimit(s) : 0;
}
static float new_get_JumpHeight(void* s) {
    int v = g_JumpHeight.load();
    if (v > 0) { TRACE_CALL("CharacterMotorAbilities.get_JumpHeight"); TRACE_EXIT("CharacterMotorAbilities.get_JumpHeight"); return (float)v; }
    return old_get_JumpHeight ? old_get_JumpHeight(s) : 0.0f;
}
static float new_get_LaneChangeDuration(void* s) {
    if (g_FastLaneChange.load()) { TRACE_CALL("CharacterMotorAbilities.get_LaneChangeDuration"); TRACE_EXIT("CharacterMotorAbilities.get_LaneChangeDuration"); return 0.0f; }
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(s) : 0.0f;
}
static bool new_IsAutoReviveEnabled(void* s) {
    if (g_AutoRevive.load()) { TRACE_CALL("StumbleBehaviour.IsAutoReviveEnabled"); TRACE_EXIT("StumbleBehaviour.IsAutoReviveEnabled"); return true; }
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(s) : false;
}
static float new_GetDuration(void* s, int level) {
    int v = g_PowerDuration.load();
    if (v > 0) { TRACE_CALL("PowerConfig.GetDuration"); TRACE_EXIT("PowerConfig.GetDuration"); return (float)v; }
    return old_GetDuration ? old_GetDuration(s, level) : 0.0f;
}
static int new_get_BaseMultiplierSum(void* s) {
    int v = g_ScoreMultiplier.load();
    if (v > 0) { TRACE_CALL("ScoreMultiplierManager.get_BaseMultiplierSum"); TRACE_EXIT("ScoreMultiplierManager.get_BaseMultiplierSum"); return v; }
    return old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(s) : 0;
}
static bool new_get_detectCollisions(void* s) {
    if (g_NoCollision.load()) {
        TRACE_CALL("CharacterController.get_detectCollisions"); TRACE_EXIT("CharacterController.get_detectCollisions");
        long long st = g_NoCollisionStartMs.load();
        if (st > 0 && (nowMs() - st) < 1000) return false;
    }
    return old_get_detectCollisions ? old_get_detectCollisions(s) : true;
}
static int new_GetCurrency(void* s, int t) {
    if (g_UnlimitedCoins.load()) { TRACE_CALL("WalletModel.GetCurrency"); TRACE_EXIT("WalletModel.GetCurrency"); return 99999999; }
    return old_GetCurrency ? old_GetCurrency(s, t) : 0;
}

// ---- No Impact (Front + Side) ----
static bool new_CheckFrontalImpact(void* s, void* hit) {
    if (g_NoImpact.load()) {
        TRACE_CALL("CharacterMotor.CheckFrontalImpact");
        TRACE_EXIT("CharacterMotor.CheckFrontalImpact");
        return false;
    }
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(s, hit) : true;
}
static bool new_CheckSideImpact(void* s, void* hit) {
    if (g_NoImpact.load()) {
        TRACE_CALL("CharacterMotor.CheckSideImpact");
        TRACE_EXIT("CharacterMotor.CheckSideImpact");
        return false;
    }
    return old_CheckSideImpact ? old_CheckSideImpact(s, hit) : true;
}

static void new_ApplyGravity(void* s) {
    if (g_NoGravity.load()) { TRACE_CALL("CharacterMotor.ApplyGravity"); TRACE_EXIT("CharacterMotor.ApplyGravity"); return; }
    if (old_ApplyGravity) old_ApplyGravity(s);
}
static float new_get_MinSpeed(void* s) {
    int v = g_CharacterSpeed.load();
    if (v > 0) { TRACE_CALL("CharacterMotorAbilities.get_MinSpeed"); TRACE_EXIT("CharacterMotorAbilities.get_MinSpeed"); return (float)v; }
    return old_get_MinSpeed ? old_get_MinSpeed(s) : 0.0f;
}
static float new_get_MaxSpeed(void* s) {
    int v = g_CharacterSpeed.load();
    if (v > 0) { TRACE_CALL("CharacterMotorAbilities.get_MaxSpeed"); TRACE_EXIT("CharacterMotorAbilities.get_MaxSpeed"); return (float)v; }
    return old_get_MaxSpeed ? old_get_MaxSpeed(s) : 0.0f;
}
static void new_UpdateMovement(void* s) {
    if (g_FreezeCharacter.load()) { TRACE_CALL("CharacterMotor.UpdateMovement"); TRACE_EXIT("CharacterMotor.UpdateMovement"); return; }
    if (old_UpdateMovement) old_UpdateMovement(s);
}
static float new_get_AirJumpHeight(void* s) {
    int v = g_AirJumpHeight.load();
    if (v > 0) { TRACE_CALL("CharacterMotorAbilities.get_AirJumpHeight"); TRACE_EXIT("CharacterMotorAbilities.get_AirJumpHeight"); return (float)v; }
    return old_get_AirJumpHeight ? old_get_AirJumpHeight(s) : 0.0f;
}
static bool new_CameraApply(void* s, void* c) {
    int m = g_CameraMode.load();
    if (m == 1) { TRACE_CALL("CameraGroundedModifier.Apply"); TRACE_EXIT("CameraGroundedModifier.Apply"); return true; }
    if (m == 2) { TRACE_CALL("CameraGroundedModifier.Apply"); TRACE_EXIT("CameraGroundedModifier.Apply"); return false; }
    return old_CameraApply ? old_CameraApply(s, c) : false;
}
static void new_NotifySideImpact(void* s, int h, void* e, void* ent) {
    if (g_NoBoundaryImpact.load()) {
        TRACE_CALL("CharacterMotor.NotifySideImpact"); TRACE_EXIT("CharacterMotor.NotifySideImpact");
        return;
    }
    if (old_NotifySideImpact) old_NotifySideImpact(s, h, e, ent);
}
static void new_ShowFlight(void* s, bool sh, int m) {
    if (g_HideJetpack.load()) { TRACE_CALL("CharacterPropsController.ShowFlight"); TRACE_EXIT("CharacterPropsController.ShowFlight"); return; }
    if (old_ShowFlight) old_ShowFlight(s, sh, m);
}
static void new_ShowPogostick(void* s, bool sh) {
    if (g_HidePogostick.load()) { TRACE_CALL("CharacterPropsController.ShowPogostick"); TRACE_EXIT("CharacterPropsController.ShowPogostick"); return; }
    if (old_ShowPogostick) old_ShowPogostick(s, sh);
}
static void new_ShowMagnet(void* s, bool sh) {
    if (g_HideMagnet.load()) { TRACE_CALL("CharacterPropsController.ShowMagnet"); TRACE_EXIT("CharacterPropsController.ShowMagnet"); return; }
    if (old_ShowMagnet) old_ShowMagnet(s, sh);
}
static void new_ShowHoverboard(void* s, bool rb) {
    if (g_HideHoverboard.load()) { TRACE_CALL("CharacterPropsController.ShowHoverboard"); TRACE_EXIT("CharacterPropsController.ShowHoverboard"); return; }
    if (old_ShowHoverboard) old_ShowHoverboard(s, rb);
}
static void new_Displace(void* s) {
    if (g_StopTrains.load()) { TRACE_CALL("MovingTrain.Displace"); TRACE_EXIT("MovingTrain.Displace"); return; }
    if (old_Displace) old_Displace(s);
}
static void new_Addcoins(void* s, int c) {
    int b = g_AddCoinsBonus.load();
    if (b > 0) { TRACE_CALL("RunSessionData.Addcoins"); TRACE_EXIT("RunSessionData.Addcoins"); if (old_Addcoins) old_Addcoins(s, c + b); }
    else { if (old_Addcoins) old_Addcoins(s, c); }
}
static void new_AddKeys(void* s, int k) {
    int b = g_AddKeysBonus.load();
    if (b > 0) { TRACE_CALL("RunSessionData.AddKeys"); TRACE_EXIT("RunSessionData.AddKeys"); if (old_AddKeys) old_AddKeys(s, k + b); }
    else { if (old_AddKeys) old_AddKeys(s, k); }
}
static void new_AddPoints(void* s, double p) {
    int b = g_AddScoreBonus.load();
    if (b > 0) { TRACE_CALL("RunSessionData.AddPoints"); TRACE_EXIT("RunSessionData.AddPoints"); if (old_AddPoints) old_AddPoints(s, p + (double)b); }
    else { if (old_AddPoints) old_AddPoints(s, p); }
}
static void new_UpdatePowers(void* s) {
    if (g_UnlimitedPowerTime.load()) { TRACE_CALL("CharacterPower.UpdatePowers"); TRACE_EXIT("CharacterPower.UpdatePowers"); return; }
    if (old_UpdatePowers) old_UpdatePowers(s);
}
static void new_MainUpdatePowers(void* s) {
    if (g_UnlimitedPowerTime.load()) { TRACE_CALL("MainPowerSystem.UpdatePowers"); TRACE_EXIT("MainPowerSystem.UpdatePowers"); return; }
    if (old_MainUpdatePowers) old_MainUpdatePowers(s);
}
static void new_FinishFlight(void* s) {
    if (g_UnlimitedJetpackTime.load()) { TRACE_CALL("FlyingState.FinishFlight"); TRACE_EXIT("FlyingState.FinishFlight"); return; }
    if (old_FinishFlight) old_FinishFlight(s);
}
static void new_OnRevivedWithCurrency(void* s) {
    if (g_ReviveReward.load()) { TRACE_CALL("MainReviveSystem.OnRevivedWithCurrency"); TRACE_EXIT("MainReviveSystem.OnRevivedWithCurrency"); }
    if (old_OnRevivedWithCurrency) old_OnRevivedWithCurrency(s);
}
static void new_MockRevivedWithCurrency(void* s) {
    if (g_ReviveReward.load()) { TRACE_CALL("MockReviveSystem.OnRevivedWithCurrency"); TRACE_EXIT("MockReviveSystem.OnRevivedWithCurrency"); }
    if (old_MockRevivedWithCurrency) old_MockRevivedWithCurrency(s);
}
static void new_HandleCoinsPickedUp(void* s) {
    if (g_FreezeCoins.load()) { TRACE_CALL("GameScreenCoinCounter.HandleOnCoinsPickedUp"); TRACE_EXIT("GameScreenCoinCounter.HandleOnCoinsPickedUp"); return; }
    if (old_HandleCoinsPickedUp) old_HandleCoinsPickedUp(s);
}
static void new_HandlePointsEarned(void* s) {
    if (g_FreezeScore.load()) { TRACE_CALL("GameScreenScoreCounter.HandleOnPointsEarned"); TRACE_EXIT("GameScreenScoreCounter.HandleOnPointsEarned"); return; }
    if (old_HandlePointsEarned) old_HandlePointsEarned(s);
}

// ================================================================
//  HEARTBEAT
// ================================================================
static void* heartbeat_thread(void*) {
    int n = 0;
    while (true) {
        sleep(60); n++;
        LOGI("[HEARTBEAT %02d] alive", n);
        if (n == 1) { markStartupComplete(); }
    }
    return nullptr;
}

// ================================================================
//  PHASES
// ================================================================
static void installPhase1() {
    LOGI("=== PHASE 1: core hooks ===");
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (C && C->thisclass) {
            DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"),0);
            DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"),0);
            DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"),0);
            if (o1) HOOK_CHECK("CharacterMotorAbilities","get_JumpLimit",o1,(void*)new_get_JumpLimit,old_get_JumpLimit);
            if (o2) HOOK_CHECK("CharacterMotorAbilities","get_JumpHeight",o2,(void*)new_get_JumpHeight,old_get_JumpHeight);
            if (o3) HOOK_CHECK("CharacterMotorAbilities","get_LaneChangeDuration",o3,(void*)new_get_LaneChangeDuration,old_get_LaneChangeDuration);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"),0);
            if (o) HOOK_CHECK("StumbleBehaviour","IsAutoReviveEnabled",o,(void*)new_IsAutoReviveEnabled,old_IsAutoReviveEnabled);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetDuration"),1);
            if (o) HOOK_CHECK("PowerConfig","GetDuration",o,(void*)new_GetDuration,old_GetDuration);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"),0);
            if (o) HOOK_CHECK("ScoreMultiplierManager","get_BaseMultiplierSum",o,(void*)new_get_BaseMultiplierSum,old_get_BaseMultiplierSum);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"),0);
            if (o) HOOK_CHECK("CharacterController","get_detectCollisions",o,(void*)new_get_detectCollisions,old_get_detectCollisions);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("GetCurrency"),1);
            if (o) HOOK_CHECK("WalletModel","GetCurrency",o,(void*)new_GetCurrency,old_GetCurrency);
        }
    }
    LOGI("=== PHASE 1 complete ===");
}

static void installPhase2() {
    LOGI("=== PHASE 2: currency / gravity / movement / impact ===");

    // ================================================================
    //  Currency.get_IsIAP — CAPTURE ADDRESS (do NOT hook!)
    //
    //  This function is only 4 instructions long:
    //    b9401008  LDR  W8, [X0, #0x10]
    //    7100291f  CMP  W8, #0xA
    //    1a9f17e0  CSET W0, EQ
    //    d65f03c0  RET
    //
    //  A64HookFunction's trampoline unconditionally jumps to
    //  (original + 0x10) after replaying 4 saved instructions — but
    //  +0x10 == end-of-function → SIGILL. We therefore use a
    //  MemoryPatch on the FIRST 8 bytes only, applied on toggle.
    // ================================================================
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (o) {
                g_IsIAPAddr = (void*)(uintptr_t)o;
                memcpy(g_IsIAPOriginal, g_IsIAPAddr, 16);
                g_IsIAPOrigSaved.store(true);
                uint32_t* c = (uint32_t*)g_IsIAPAddr;
                LOGI("[CAPTURE] get_IsIAP @ %p  [%08x %08x %08x %08x]",
                     g_IsIAPAddr, c[0], c[1], c[2], c[3]);

                // If toggle was ON before Phase 2, apply now.
                if (g_UnlimitedShop.load()) {
                    LOGI("[PATCH] Deferred apply (toggle was ON before Phase 2)");
                    applyIsIAPPatch(true);
                }
            } else {
                LOGE("[MISS] Currency.get_IsIAP offset not found");
            }
        } else {
            LOGE("[MISS] Currency class not found");
        }
    }

    // ================================================================
    //  CharacterMotor — gravity / movement / side notification
    //                   + NEW: CheckFrontalImpact / CheckSideImpact
    // ================================================================
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (C && C->thisclass) {
            DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("ApplyGravity"),0);
            DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("UpdateMovement"),0);
            DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("NotifySideImpact"),3);
            DWORD o4 = C->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"),1);
            DWORD o5 = C->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),1);

            if (o1) HOOK_CHECK("CharacterMotor","ApplyGravity",o1,(void*)new_ApplyGravity,old_ApplyGravity);
            if (o2) HOOK_CHECK("CharacterMotor","UpdateMovement",o2,(void*)new_UpdateMovement,old_UpdateMovement);
            if (o3) HOOK_CHECK("CharacterMotor","NotifySideImpact",o3,(void*)new_NotifySideImpact,old_NotifySideImpact);
            if (o4) HOOK_CHECK("CharacterMotor","CheckFrontalImpact",o4,(void*)new_CheckFrontalImpact,old_CheckFrontalImpact);
            if (o5) HOOK_CHECK("CharacterMotor","CheckSideImpact",o5,(void*)new_CheckSideImpact,old_CheckSideImpact);
        }
    }
    LOGI("=== PHASE 2 complete ===");
}

static void installPhase3() {
    LOGI("=== PHASE 3: camera ===");
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraGroundedModifier"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Apply"),1);
            if (o) HOOK_CHECK("CameraGroundedModifier","Apply",o,(void*)new_CameraApply,old_CameraApply);
        }
    }
    LOGI("=== PHASE 3 complete ===");
}

static void installPhase4() {
    LOGI("=== PHASE 4: speed / air jump ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
    if (C && C->thisclass) {
        DWORD o1 = C->GetMethodOffsetByName(OBFUSCATE("get_MinSpeed"),0);
        DWORD o2 = C->GetMethodOffsetByName(OBFUSCATE("get_MaxSpeed"),0);
        DWORD o3 = C->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"),0);
        if (o1) HOOK_CHECK("CharacterMotorAbilities","get_MinSpeed",o1,(void*)new_get_MinSpeed,old_get_MinSpeed);
        if (o2) HOOK_CHECK("CharacterMotorAbilities","get_MaxSpeed",o2,(void*)new_get_MaxSpeed,old_get_MaxSpeed);
        if (o3) HOOK_CHECK("CharacterMotorAbilities","get_AirJumpHeight",o3,(void*)new_get_AirJumpHeight,old_get_AirJumpHeight);
    }
    LOGI("=== PHASE 4 complete ===");
}

static void installPhase5() {
    LOGI("=== PHASE 5: character props ===");
    auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPropsController"));
    if (!C || !C->thisclass) return;

    { DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowFlight"),2);
      if (o) HOOK_CHECK("CharacterPropsController","ShowFlight",o,(void*)new_ShowFlight,old_ShowFlight);
      usleep(100*1000); }
    { DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowPogostick"),1);
      if (o) HOOK_CHECK("CharacterPropsController","ShowPogostick",o,(void*)new_ShowPogostick,old_ShowPogostick);
      usleep(100*1000); }
    { DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowMagnet"),1);
      if (o) HOOK_CHECK("CharacterPropsController","ShowMagnet",o,(void*)new_ShowMagnet,old_ShowMagnet);
      usleep(100*1000); }
    { DWORD o = C->GetMethodOffsetByName(OBFUSCATE("ShowHoverboard"),1);
      if (o) HOOK_CHECK("CharacterPropsController","ShowHoverboard",o,(void*)new_ShowHoverboard,old_ShowHoverboard);
      usleep(100*1000); }
    LOGI("=== PHASE 5 complete ===");
}

static void installPhase6() {
    LOGI("=== PHASE 6: misc ===");
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Displace"),0);
            if (o) HOOK_CHECK("MovingTrain","Displace",o,(void*)new_Displace,old_Displace);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("Addcoins"),1);
            if (!o) o = C->GetMethodOffsetByName(OBFUSCATE("AddCoins"),1);
            if (o) HOOK_CHECK("RunSessionData","Addcoins",o,(void*)new_Addcoins,old_Addcoins);
            o = C->GetMethodOffsetByName(OBFUSCATE("AddKeys"),1);
            if (o) HOOK_CHECK("RunSessionData","AddKeys",o,(void*)new_AddKeys,old_AddKeys);
            o = C->GetMethodOffsetByName(OBFUSCATE("AddPoints"),1);
            if (o) HOOK_CHECK("RunSessionData","AddPoints",o,(void*)new_AddPoints,old_AddPoints);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPower"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"),0);
            if (o) HOOK_CHECK("CharacterPower","UpdatePowers",o,(void*)new_UpdatePowers,old_UpdatePowers);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainPowerSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"),0);
            if (o) HOOK_CHECK("MainPowerSystem","UpdatePowers",o,(void*)new_MainUpdatePowers,old_MainUpdatePowers);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("FlyingState"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("FinishFlight"),0);
            if (o) HOOK_CHECK("FlyingState","FinishFlight",o,(void*)new_FinishFlight,old_FinishFlight);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"),0);
            if (o) HOOK_CHECK("MainReviveSystem","OnRevivedWithCurrency",o,(void*)new_OnRevivedWithCurrency,old_OnRevivedWithCurrency);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Systems.Mocks"), OBFUSCATE("MockReviveSystem"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"),0);
            if (o) HOOK_CHECK("MockReviveSystem","OnRevivedWithCurrency",o,(void*)new_MockRevivedWithCurrency,old_MockRevivedWithCurrency);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenCoinCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnCoinsPickedUp"),0);
            if (o) HOOK_CHECK("GameScreenCoinCounter","HandleOnCoinsPickedUp",o,(void*)new_HandleCoinsPickedUp,old_HandleCoinsPickedUp);
        }
    }
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenScoreCounter"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("HandleOnPointsEarned"),0);
            if (o) HOOK_CHECK("GameScreenScoreCounter","HandleOnPointsEarned",o,(void*)new_HandlePointsEarned,old_HandlePointsEarned);
        }
    }
    LOGI("=== PHASE 6 complete ===");
}

// ================================================================
//  HACK THREAD
// ================================================================
void* hack_thread(void*) {
    g_BootStep = 3;
    LOGI(">>> hack_thread started <<<");
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);
    g_BootStep = 4;
    LOGI(">>> library loaded <<<");

    g_BootStep = 5;
    if (safeModeFileExists()) { g_BootStep = 14; return nullptr; }
    if (previousStartupCrashed()) { unlink(g_StartupFlagPath.c_str()); g_BootStep = 14; return nullptr; }
    markStartupPending();

    g_BootStep = 6;  installPhase1(); usleep(500*1000);
    g_BootStep = 7;  installPhase2(); usleep(500*1000);
    g_BootStep = 8;  installPhase3(); usleep(500*1000);
    g_BootStep = 9;  installPhase4(); usleep(500*1000);
    g_BootStep = 10; installPhase5(); usleep(500*1000);
    g_BootStep = 11; installPhase6(); usleep(500*1000);

    g_BootStep = 13;
    LOGI(">>> installing GLES render hooks (chams) <<<");
    RenderHook();
    LOGI(">>> GLES hooks done <<<");

    pthread_t hb;
    pthread_create(&hb, nullptr, heartbeat_thread, nullptr);

    g_BootStep = 14;
    LOGI("ALL HOOKS INSTALLED — startup complete.");
    return nullptr;
}

// ================================================================
//  JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {
        OBFUSCATE("800_ButtonOnOff_★ Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_★★ Max Mode"),
        OBFUSCATE("802_ButtonOnOff_★★★ Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_↺ Reset All Features"),
        OBFUSCATE("Category_Player"),
        OBFUSCATE("102_SeekBar_Max Jump Count_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height Boost_0_200"),
        OBFUSCATE("104_Toggle_Auto Revive"),
        OBFUSCATE("130_Toggle_Low Gravity (Float)"),
        OBFUSCATE("131_Toggle_No Wall Impact"),
        OBFUSCATE("133_Toggle_Freeze Player Movement"),
        OBFUSCATE("134_SeekBar_Air Jump Height Boost_0_200"),
        OBFUSCATE("160_SeekBar_Player Speed Boost_0_200"),
        OBFUSCATE("Category_Powerups"),
        OBFUSCATE("105_SeekBar_Powerup Duration_0_999"),
        OBFUSCATE("106_Toggle_Instant Lane Change"),
        OBFUSCATE("135_Toggle_Hide Jetpack Visual"),
        OBFUSCATE("136_Toggle_Hide Pogo Stick Visual"),
        OBFUSCATE("137_Toggle_Hide Magnet Visual"),
        OBFUSCATE("138_Toggle_Hide Hoverboard Visual"),
        OBFUSCATE("140_Toggle_Infinite Powerup Duration"),
        OBFUSCATE("141_Toggle_Infinite Jetpack Duration"),
        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier Boost_0_999"),
        OBFUSCATE("143_Toggle_Freeze Coin Counter"),
        OBFUSCATE("144_Toggle_Freeze Score Counter"),
        OBFUSCATE("Category_Collision"),
        OBFUSCATE("108_Toggle_God Mode (No Collision Detect)"),
        OBFUSCATE("132_Toggle_No Impact (Front + Side)"),
        OBFUSCATE("Category_Shop"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),
        OBFUSCATE("150_Toggle_Unlock All Shop Items (Free IAP)"),
        OBFUSCATE("151_SeekBar_Extra Coins Per Pickup_0_9999"),
        OBFUSCATE("152_SeekBar_Extra Keys Per Pickup_0_9999"),
        OBFUSCATE("153_SeekBar_Extra Score Per Point_0_9999"),
        OBFUSCATE("154_Toggle_Revive Reward Multiplier"),
        OBFUSCATE("Category_Camera"),
        OBFUSCATE("161_Spinner_Camera Mode_Normal,Follow Player Y,Stop Vertical"),
        OBFUSCATE("Category_World"),
        OBFUSCATE("142_Toggle_Freeze Moving Trains"),
        OBFUSCATE("Category_Chams"),
        OBFUSCATE("119_Toggle_Enable Chams"),
        OBFUSCATE("120_Spinner_Chams Mode_Off,Solid Visible,X-Ray,Hybrid Opaque"),
        OBFUSCATE("125_Spinner_Color Preset_Dark Green / Sky Blue,Neon Green / Red,Cyan / Magenta"),
        OBFUSCATE("126_Spinner_Shader Target_Trains Only (_Displacement),All Objects (_MainTex)"),
        OBFUSCATE("127_SeekBar_Max Distance_15_100"),
        OBFUSCATE("121_Toggle_Rainbow Visible"),
        OBFUSCATE("122_Toggle_Rainbow In-Wall"),
    };
    int n = (int)(sizeof(features)/sizeof(features[0]));
    ret = (jobjectArray) env->NewObjectArray(n, env->FindClass(OBFUSCATE("java/lang/String")), env->NewStringUTF(""));
    for (int i = 0; i < n; i++) env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));
    return ret;
}

// ================================================================
//  JNI — CHANGES
// ================================================================
void Changes(JNIEnv*, jclass, jobject, jint featNum, jstring, jint value, jlong, jboolean boolean, jstring) {
    switch (featNum) {
        case 102: g_JumpLimit.store(value);        break;
        case 103: g_JumpHeight.store(value);       break;
        case 104: g_AutoRevive.store(boolean);     break;
        case 105: g_PowerDuration.store(value);    break;
        case 106: g_FastLaneChange.store(boolean); break;
        case 107: g_ScoreMultiplier.store(value);  break;
        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else g_NoCollisionStartMs.store(0);
        } break;
        case 110: g_UnlimitedCoins.store(boolean); break;

        // ---- NEW: No Impact (Front + Side) ----
        case 132: g_NoImpact.store(boolean);       break;

        case 130: g_NoGravity.store(boolean);            break;
        case 131: g_NoBoundaryImpact.store(boolean);     break;
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

        // ---- Unlimited Shop (Free IAP) — MEMORY PATCH ----
        case 150: {
            g_UnlimitedShop.store(boolean);
            applyIsIAPPatch(boolean);
        } break;

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
            g_AutoRevive.store(boolean);    g_UnlimitedCoins.store(boolean);
        } break;
        case 801: {
            g_AutoRevive.store(boolean);    g_FastLaneChange.store(boolean);
            g_UnlimitedCoins.store(boolean);
            if (boolean) { g_JumpLimit.store(100); g_JumpHeight.store(50);
                           g_PowerDuration.store(100); g_ScoreMultiplier.store(10); }
            else { g_JumpLimit.store(0); g_JumpHeight.store(0);
                   g_PowerDuration.store(0); g_ScoreMultiplier.store(0); }
        } break;
        case 802: {
            g_AutoRevive.store(boolean);    g_FastLaneChange.store(boolean);
            g_UnlimitedCoins.store(boolean); g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else g_NoCollisionStartMs.store(0);
            if (boolean) { g_JumpLimit.store(999); g_JumpHeight.store(200);
                           g_PowerDuration.store(999); g_ScoreMultiplier.store(999); }
            else { g_JumpLimit.store(0); g_JumpHeight.store(0);
                   g_PowerDuration.store(0); g_ScoreMultiplier.store(0); }
        } break;
        case 803: {
            g_AutoRevive.store(false);    g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false); g_NoCollision.store(false);
            g_NoCollisionStartMs.store(0); g_JumpLimit.store(0);
            g_JumpHeight.store(0); g_PowerDuration.store(0);
            g_ScoreMultiplier.store(0);
            g_NoImpact.store(false);   // NEW: reset impact toggle too
            g_NoGravity.store(false); g_FreezeCharacter.store(false);
            g_CharacterSpeed.store(0); g_AirJumpHeight.store(0);
            g_CameraMode.store(0); g_NoBoundaryImpact.store(false);
            g_HideJetpack.store(false);
            g_HidePogostick.store(false); g_HideMagnet.store(false);
            g_HideHoverboard.store(false); g_StopTrains.store(false);
            g_AddCoinsBonus.store(0); g_AddKeysBonus.store(0);
            g_AddScoreBonus.store(0); g_UnlimitedPowerTime.store(false);
            g_UnlimitedJetpackTime.store(false); g_ReviveReward.store(false);
            g_FreezeCoins.store(false); g_FreezeScore.store(false);
            // NOTE: g_UnlimitedShop reset requires patch revert.
            if (g_UnlimitedShop.load()) {
                g_UnlimitedShop.store(false);
                applyIsIAPPatch(false);
            }
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
    initLogging();
    g_BootStep = 2;
    installCrashHandler();
    LOGI("[BOOT] Crash handler installed");
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}