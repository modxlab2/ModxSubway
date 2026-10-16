// ================================================================
// ModX Lab — Subway Surfers (Unity / IL2CPP)
// Auto-Update Hook System + Chams + Full Diagnostics Logging
// ----------------------------------------------------------------
// ★ Every hook installation is logged
// ★ Every hook invocation is logged (with rate limiting)
// ★ Thread-safe file logger → /sdcard/Documents/ModXLab_Log.txt
// ★ Crash detection via signal handlers
// ================================================================

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
#include <ctime>
#include <pthread.h>
#include <unistd.h>
#include <string>
#include <csignal>
#include <sys/stat.h>
#include <android/log.h>

#include "Chams.h"

#define LIB OBFUSCATE("libil2cpp.so")

// ================================================================
//  📁 THREAD-SAFE LOGGER — Documents folder
// ================================================================
static FILE*            g_LogFile   = nullptr;
static pthread_mutex_t  g_LogMutex  = PTHREAD_MUTEX_INITIALIZER;
static char             g_LogPath[256] = {0};
static std::atomic<bool> g_LogReady {false};

#define LOG_TAG "MODXLAB"
#define LOG_TO_LOGCAT(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------
//  Open log file
// ---------------------------------------------------------------
static void openLogFile() {
    if (g_LogFile) return;

    const char* candidates[] = {
        "/sdcard/Documents/ModXLab_Log.txt",
        "/storage/emulated/0/Documents/ModXLab_Log.txt",
        "/sdcard/Android/data/com.kiloo.subwaysurf/files/ModXLab_Log.txt",
        "/data/local/tmp/ModXLab_Log.txt"
    };

    for (auto path : candidates) {
        FILE* f = fopen(path, "a");   // append mode
        if (f) {
            g_LogFile = f;
            strncpy(g_LogPath, path, sizeof(g_LogPath) - 1);
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            char tb[64];
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm_info);
            fprintf(g_LogFile, "\n\n════════════════════════════════════════════\n");
            fprintf(g_LogFile, "  ModX Lab Session @ %s\n", tb);
            fprintf(g_LogFile, "════════════════════════════════════════════\n");
            fflush(g_LogFile);
            g_LogReady.store(true);
            return;
        }
    }
}

// ---------------------------------------------------------------
//  Write to file + logcat
// ---------------------------------------------------------------
static void LOGX(const char* level, const char* fmt, ...) {
    // logcat
    va_list ap1;
    va_start(ap1, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap1);
    va_end(ap1);

    if (!g_LogReady.load() || !g_LogFile) return;

    pthread_mutex_lock(&g_LogMutex);
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char tb[16];
    strftime(tb, sizeof(tb), "%H:%M:%S", tm_info);
    fprintf(g_LogFile, "[%s][%s] ", tb, level);
    va_list ap2;
    va_start(ap2, fmt);
    vfprintf(g_LogFile, fmt, ap2);
    va_end(ap2);
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
    pthread_mutex_unlock(&g_LogMutex);
}

#define LOGI(...) LOGX("I", __VA_ARGS__)
#define LOGW(...) LOGX("W", __VA_ARGS__)
#define LOGE(...) LOGX("E", __VA_ARGS__)
#define LOGH(...) LOGX("H", __VA_ARGS__)   // HOOK invocation

// ---------------------------------------------------------------
//  Rate-limited hook logging (prevents I/O flood)
//  Logs first N calls per hook, then every Nth call
// ---------------------------------------------------------------
struct RateLimiter {
    std::atomic<int> count{0};
    std::atomic<long long> lastLogMs{0};
};

static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define HOOK_LOG_RL(limiter, ...)                                 \
    do {                                                          \
        int c = (limiter).count.fetch_add(1);                     \
        long long now = nowMs();                                  \
        long long last = (limiter).lastLogMs.load();              \
        if (c < 5 || (now - last) > 5000) {                       \
            (limiter).lastLogMs.store(now);                       \
            LOGH(__VA_ARGS__);                                    \
        }                                                         \
    } while (0)

// ---------------------------------------------------------------
//  Crash signal handler → writes to log file
// ---------------------------------------------------------------
static void crashHandler(int sig) {
    const char* name = "UNKNOWN";
    switch (sig) {
        case SIGSEGV: name = "SIGSEGV (Segmentation fault)"; break;
        case SIGABRT: name = "SIGABRT (Abort)";              break;
        case SIGFPE:  name = "SIGFPE  (Floating point)";     break;
        case SIGILL:  name = "SIGILL  (Illegal instruction)";break;
        case SIGBUS:  name = "SIGBUS  (Bus error)";          break;
        case SIGTRAP: name = "SIGTRAP (Trap)";               break;
    }
    LOGE("═══════════ CRASH DETECTED ═══════════");
    LOGE("Signal: %d — %s", sig, name);
    LOGE("═══════════════════════════════════════");

    // close file
    if (g_LogFile) {
        fflush(g_LogFile);
        fclose(g_LogFile);
        g_LogFile = nullptr;
    }

    // re-raise default handler
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandlers() {
    signal(SIGSEGV, crashHandler);
    signal(SIGABRT, crashHandler);
    signal(SIGFPE,  crashHandler);
    signal(SIGILL,  crashHandler);
    signal(SIGBUS,  crashHandler);
    signal(SIGTRAP, crashHandler);
}

// ================================================================
//   FEATURE STATE
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
static std::atomic<bool> g_FreeShopping     {false};
static std::atomic<bool> g_UnlimitedCoins   {false};

static std::atomic<long long> g_NoCollisionStartMs {0};

// Rate limiters
static RateLimiter rl_Frontal, rl_Side, rl_JumpLimit, rl_JumpHeight,
                   rl_Revive, rl_Duration, rl_Lane, rl_Score,
                   rl_DetectColl, rl_IAP, rl_Currency;

// ================================================================
//                     ORIGINAL FUNCTION POINTERS
// ================================================================
static bool  (*old_CheckFrontalImpact)(...)     = nullptr;
static bool  (*old_CheckSideImpact)(...)        = nullptr;
static int   (*old_get_JumpLimit)(...)          = nullptr;
static float (*old_get_JumpHeight)(...)         = nullptr;
static bool  (*old_IsAutoReviveEnabled)(...)    = nullptr;
static float (*old_GetDuration)(...)            = nullptr;
static float (*old_get_LaneChangeDuration)(...)= nullptr;
static int   (*old_get_BaseMultiplierSum)(...)  = nullptr;
static bool  (*old_get_detectCollisions)(...)   = nullptr;
static bool  (*old_get_IsIAP)(...)              = nullptr;
static int   (*old_GetCurrency)(...)            = nullptr;

// ================================================================
//                       HOOK IMPLEMENTATIONS
// ================================================================

// ---- CheckFrontalImpact ----
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) {
        LOGW("CheckFrontalImpact: NULL self — returning false");
        return false;
    }
    if (g_FrontalImpact.load()) {
        HOOK_LOG_RL(rl_Frontal, "CheckFrontalImpact(self=%p, other=%p) → SUPPRESS (toggle ON)", self, other);
        return false;
    }
    bool r = old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
    HOOK_LOG_RL(rl_Frontal, "CheckFrontalImpact(self=%p, other=%p) → ORIG=%d", self, other, (int)r);
    return r;
}

// ---- CheckSideImpact ----
static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) {
        LOGW("CheckSideImpact: NULL self — returning false");
        return false;
    }
    if (g_SideImpact.load()) {
        HOOK_LOG_RL(rl_Side, "CheckSideImpact(self=%p, other=%p) → SUPPRESS (toggle ON)", self, other);
        return false;
    }
    bool r = old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
    HOOK_LOG_RL(rl_Side, "CheckSideImpact(self=%p, other=%p) → ORIG=%d", self, other, (int)r);
    return r;
}

// ---- get_JumpLimit ----
static int new_get_JumpLimit(void* self) {
    if (!self) {
        LOGW("get_JumpLimit: NULL self");
        return 0;
    }
    int v = g_JumpLimit.load();
    if (v > 0) {
        HOOK_LOG_RL(rl_JumpLimit, "get_JumpLimit(self=%p) → OVERRIDE=%d", self, v);
        return v;
    }
    int r = old_get_JumpLimit ? old_get_JumpLimit(self) : 0;
    HOOK_LOG_RL(rl_JumpLimit, "get_JumpLimit(self=%p) → ORIG=%d", self, r);
    return r;
}

// ---- get_JumpHeight ----
static float new_get_JumpHeight(void* self) {
    if (!self) {
        LOGW("get_JumpHeight: NULL self");
        return 0.0f;
    }
    int v = g_JumpHeight.load();
    if (v > 0) {
        HOOK_LOG_RL(rl_JumpHeight, "get_JumpHeight(self=%p) → OVERRIDE=%.2f", self, (float)v);
        return (float)v;
    }
    float r = old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f;
    HOOK_LOG_RL(rl_JumpHeight, "get_JumpHeight(self=%p) → ORIG=%.2f", self, r);
    return r;
}

// ---- get_LaneChangeDuration ----
static float new_get_LaneChangeDuration(void* self) {
    if (!self) {
        LOGW("get_LaneChangeDuration: NULL self");
        return 0.0f;
    }
    if (g_FastLaneChange.load()) {
        HOOK_LOG_RL(rl_Lane, "get_LaneChangeDuration(self=%p) → OVERRIDE=0.00", self);
        return 0.0f;
    }
    float r = old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
    HOOK_LOG_RL(rl_Lane, "get_LaneChangeDuration(self=%p) → ORIG=%.3f", self, r);
    return r;
}

// ---- IsAutoReviveEnabled ----
static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) {
        LOGW("IsAutoReviveEnabled: NULL self");
        return false;
    }
    if (g_AutoRevive.load()) {
        HOOK_LOG_RL(rl_Revive, "IsAutoReviveEnabled(self=%p) → OVERRIDE=true", self);
        return true;
    }
    bool r = old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
    HOOK_LOG_RL(rl_Revive, "IsAutoReviveEnabled(self=%p) → ORIG=%d", self, (int)r);
    return r;
}

// ---- GetDuration ----
static float new_GetDuration(void* self, void* cfg) {
    if (!self) {
        LOGW("GetDuration: NULL self");
        return 0.0f;
    }
    int v = g_PowerDuration.load();
    if (v > 0) {
        HOOK_LOG_RL(rl_Duration, "GetDuration(self=%p, cfg=%p) → OVERRIDE=%.2f", self, cfg, (float)v);
        return (float)v;
    }
    float r = old_GetDuration ? old_GetDuration(self, cfg) : 0.0f;
    HOOK_LOG_RL(rl_Duration, "GetDuration(self=%p, cfg=%p) → ORIG=%.3f", self, cfg, r);
    return r;
}

// ---- get_BaseMultiplierSum ----
static int new_get_BaseMultiplierSum(void* self) {
    if (!self) {
        LOGW("get_BaseMultiplierSum: NULL self");
        return 0;
    }
    int v = g_ScoreMultiplier.load();
    if (v > 0) {
        HOOK_LOG_RL(rl_Score, "get_BaseMultiplierSum(self=%p) → OVERRIDE=%d", self, v);
        return v;
    }
    int r = old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0;
    HOOK_LOG_RL(rl_Score, "get_BaseMultiplierSum(self=%p) → ORIG=%d", self, r);
    return r;
}

// ---- get_detectCollisions ----
static bool new_get_detectCollisions(void* self) {
    if (!self) {
        LOGW("get_detectCollisions: NULL self — returning true");
        return true;
    }
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0) {
            long long elapsed = nowMs() - start;
            if (elapsed >= 0 && elapsed < 1000) {
                HOOK_LOG_RL(rl_DetectColl, "get_detectCollisions(self=%p) → DISABLED (window %lldms)", self, elapsed);
                return false;
            }
        }
    }
    bool r = old_get_detectCollisions ? old_get_detectCollisions(self) : true;
    HOOK_LOG_RL(rl_DetectColl, "get_detectCollisions(self=%p) → ORIG=%d", self, (int)r);
    return r;
}

// ---- get_IsIAP ----
static bool new_get_IsIAP(void* self) {
    if (!self) {
        LOGW("get_IsIAP: NULL self");
        return false;
    }
    if (g_FreeShopping.load()) {
        HOOK_LOG_RL(rl_IAP, "get_IsIAP(self=%p) → OVERRIDE=false", self);
        return false;
    }
    bool r = old_get_IsIAP ? old_get_IsIAP(self) : false;
    HOOK_LOG_RL(rl_IAP, "get_IsIAP(self=%p) → ORIG=%d", self, (int)r);
    return r;
}

// ---- GetCurrency ----
static int new_GetCurrency(void* self, int currencyType) {
    if (!self) {
        LOGW("GetCurrency: NULL self");
        return 0;
    }
    if (g_UnlimitedCoins.load()) {
        HOOK_LOG_RL(rl_Currency, "GetCurrency(self=%p, type=%d) → OVERRIDE=99999999", self, currencyType);
        return 99999999;
    }
    int r = old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
    HOOK_LOG_RL(rl_Currency, "GetCurrency(self=%p, type=%d) → ORIG=%d", self, currencyType, r);
    return r;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    LOGI("════════════════════════════════════════════");
    LOGI("   ModX Lab hack_thread starting...");
    LOGI("════════════════════════════════════════════");

    // Wait for libil2cpp.so
    LOGI("Waiting for %s to load...", "libil2cpp.so");
    int waitCount = 0;
    while (!isLibraryLoaded(LIB)) {
        sleep(1);
        waitCount++;
        if (waitCount % 5 == 0) LOGI("  still waiting (%d sec)...", waitCount);
    }
    LOGI("✅ libil2cpp.so loaded after %d sec", waitCount);

    sleep(3);   // IL2CPP init buffer
    LOGI("IL2CPP init buffer complete — installing hooks");

    // ------------------------------------------------------------
    //  1) CharacterMotor
    // ------------------------------------------------------------
    LOGI("─── [1/8] Hooking CharacterMotor ───");
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);

            if (oFrontal) {
                HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
                LOGI("  ✅ CheckFrontalImpact hooked @ offset 0x%X → new=%p, orig=%p",
                     oFrontal, new_CheckFrontalImpact, old_CheckFrontalImpact);
            } else {
                LOGW("  ❌ CheckFrontalImpact offset = 0 (not found)");
            }

            if (oSide) {
                HOOK_AU((void*)oSide, (void*)new_CheckSideImpact, old_CheckSideImpact);
                LOGI("  ✅ CheckSideImpact hooked @ offset 0x%X → new=%p, orig=%p",
                     oSide, new_CheckSideImpact, old_CheckSideImpact);
            } else {
                LOGW("  ❌ CheckSideImpact offset = 0 (not found)");
            }
        } else {
            LOGE("  ❌ CharacterMotor class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  2) CharacterMotorAbilities
    // ------------------------------------------------------------
    LOGI("─── [2/8] Hooking CharacterMotorAbilities ───");
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                 OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJumpLim = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJumpH   = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLane    = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);

            if (oJumpLim) {
                HOOK_AU((void*)oJumpLim, (void*)new_get_JumpLimit, old_get_JumpLimit);
                LOGI("  ✅ get_JumpLimit hooked @ 0x%X", oJumpLim);
            } else LOGW("  ❌ get_JumpLimit NOT FOUND");

            if (oJumpH) {
                HOOK_AU((void*)oJumpH, (void*)new_get_JumpHeight, old_get_JumpHeight);
                LOGI("  ✅ get_JumpHeight hooked @ 0x%X", oJumpH);
            } else LOGW("  ❌ get_JumpHeight NOT FOUND");

            if (oLane) {
                HOOK_AU((void*)oLane, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
                LOGI("  ✅ get_LaneChangeDuration hooked @ 0x%X", oLane);
            } else LOGW("  ❌ get_LaneChangeDuration NOT FOUND");
        } else {
            LOGE("  ❌ CharacterMotorAbilities class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  3) StumbleBehaviour
    // ------------------------------------------------------------
    LOGI("─── [3/8] Hooking StumbleBehaviour ───");
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oRevive = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oRevive) {
                HOOK_AU((void*)oRevive, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
                LOGI("  ✅ IsAutoReviveEnabled hooked @ 0x%X", oRevive);
            } else LOGW("  ❌ IsAutoReviveEnabled NOT FOUND");
        } else {
            LOGE("  ❌ StumbleBehaviour class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  4) PowerConfig
    // ------------------------------------------------------------
    LOGI("─── [4/8] Hooking PowerConfig ───");
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oDur = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oDur) {
                HOOK_AU((void*)oDur, (void*)new_GetDuration, old_GetDuration);
                LOGI("  ✅ GetDuration hooked @ 0x%X", oDur);
            } else LOGW("  ❌ GetDuration NOT FOUND");
        } else {
            LOGE("  ❌ PowerConfig class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  5) ScoreMultiplierManager
    // ------------------------------------------------------------
    LOGI("─── [5/8] Hooking ScoreMultiplierManager ───");
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oBase = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oBase) {
                HOOK_AU((void*)oBase, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
                LOGI("  ✅ get_BaseMultiplierSum hooked @ 0x%X", oBase);
            } else LOGW("  ❌ get_BaseMultiplierSum NOT FOUND");
        } else {
            LOGE("  ❌ ScoreMultiplierManager class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  6) CharacterController
    // ------------------------------------------------------------
    LOGI("─── [6/8] Hooking CharacterController ───");
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oDetect = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oDetect) {
                HOOK_AU((void*)oDetect, (void*)new_get_detectCollisions, old_get_detectCollisions);
                LOGI("  ✅ get_detectCollisions hooked @ 0x%X", oDetect);
            } else LOGW("  ❌ get_detectCollisions NOT FOUND");
        } else {
            LOGE("  ❌ CharacterController class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  7) Currency
    // ------------------------------------------------------------
    LOGI("─── [7/8] Hooking Currency ───");
    {
        auto CUR = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (CUR && CUR->thisclass) {
            DWORD oIAP = CUR->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (oIAP) {
                HOOK_AU((void*)oIAP, (void*)new_get_IsIAP, old_get_IsIAP);
                LOGI("  ✅ get_IsIAP hooked @ 0x%X", oIAP);
            } else LOGW("  ❌ get_IsIAP NOT FOUND");
        } else {
            LOGE("  ❌ Currency class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  8) WalletModel
    // ------------------------------------------------------------
    LOGI("─── [8/8] Hooking WalletModel ───");
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oGetCur = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oGetCur) {
                HOOK_AU((void*)oGetCur, (void*)new_GetCurrency, old_GetCurrency);
                LOGI("  ✅ GetCurrency hooked @ 0x%X", oGetCur);
            } else LOGW("  ❌ GetCurrency NOT FOUND");
        } else {
            LOGE("  ❌ WalletModel class NOT FOUND");
        }
    }

    // ------------------------------------------------------------
    //  ★ Chams GLES hooks
    // ------------------------------------------------------------
    LOGI("─── [Chams] Installing GLES2 hooks ───");
    RenderHook();
    LOGI("  ✅ Chams hooks installed (shader=%s)", mineShader.c_str());

    LOGI("════════════════════════════════════════════");
    LOGI("  ✅ ALL HOOKS INSTALLED — ModX Lab ready");
    LOGI("  Log file: %s", g_LogPath[0] ? g_LogPath : "(unavailable)");
    LOGI("════════════════════════════════════════════");

    return nullptr;
}

// ================================================================
//                      JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Simple:</font></b> Basic Safety + Coins"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Max:</font></b> Simple + Jump, Power, Score"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Ultra:</font></b> Max + No Collision, Free Shop"),
        OBFUSCATE("SmallTextView_<b><font color='#FFBB33'>None:</font></b> Reset all features"),

        OBFUSCATE("Category_Runner"),
        OBFUSCATE("100_Toggle_No Head-On Collision"),
        OBFUSCATE("101_Toggle_No Side Collision"),
        OBFUSCATE("102_SeekBar_Jump Limit_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height_0_200"),
        OBFUSCATE("104_Toggle_Auto Revive"),

        OBFUSCATE("Category_Powers"),
        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),

        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),

        OBFUSCATE("Category_Collisions"),
        OBFUSCATE("108_Toggle_No Collision Detect"),
        OBFUSCATE("SmallTextView_<b><font color='#FF4444'>⚠ Turn ON before game starts</font></b>"),

        OBFUSCATE("Category_Shop"),
        OBFUSCATE("109_Toggle_Free Shopping (IAP)"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),

        // ─── Chams Tab ───
        OBFUSCATE("Category_Chams"),
        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>──── CHAMS MODES ────</font></b>"),
        OBFUSCATE("120_SeekBar_Chams Mode_0_7"),
        OBFUSCATE("SmallTextView_<b>0</b>=OFF  <b>1</b>=Solid  <b>2</b>=Wall X-Ray ★  <b>3</b>=Wall+Outline"),
        OBFUSCATE("SmallTextView_<b>4</b>=Wireframe  <b>5</b>=Full Skeleton  <b>6</b>=Glow  <b>7</b>=X-Ray Wire"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>★ Mode 2 = best for train wall detection</font></b>"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>──── COLOR PRESETS ────</font></b>"),
        OBFUSCATE("125_SeekBar_Color Preset_0_5"),
        OBFUSCATE("SmallTextView_<b>0</b>=Neon Green/Red ★  <b>1</b>=Cyan/Magenta  <b>2</b>=Yellow/Blue"),
        OBFUSCATE("SmallTextView_<b>3</b>=Orange/Purple  <b>4</b>=White/Red  <b>5</b>=Stealth"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>──── RGB ANIMATION ────</font></b>"),
        OBFUSCATE("121_Toggle_Rainbow Visible Color"),
        OBFUSCATE("122_Toggle_Rainbow In-Wall Color"),
        OBFUSCATE("SmallTextView_<b><font color='#FFBB33'>Tip:</font></b> Rainbow overrides color preset."),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>──── DIAGNOSTICS ────</font></b>"),
        OBFUSCATE("124_Toggle_Dump Shader List to File"),
        OBFUSCATE("SmallTextView_<b><font color='#AAAAAA'>Log: /sdcard/Documents/ModXLab_Log.txt</font></b>"),
    };

    int n = (int)(sizeof(features) / sizeof(features[0]));
    ret = (jobjectArray) env->NewObjectArray(
            n,
            env->FindClass(OBFUSCATE("java/lang/String")),
            env->NewStringUTF(""));

    for (int i = 0; i < n; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return ret;
}

// ================================================================
//                      JNI — CHANGES HANDLER
// ================================================================
void Changes(JNIEnv*, jclass, jobject,
             jint featNum, jstring, jint value,
             jlong, jboolean boolean, jstring) {

    LOGI("Changes() featNum=%d value=%d bool=%d", featNum, value, (int)boolean);

    switch (featNum) {

        case 100: g_FrontalImpact.store(boolean);   LOGI("  → Frontal Impact = %d", (int)boolean); break;
        case 101: g_SideImpact.store(boolean);      LOGI("  → Side Impact = %d", (int)boolean); break;
        case 102: g_JumpLimit.store(value);         LOGI("  → Jump Limit = %d", value); break;
        case 103: g_JumpHeight.store(value);        LOGI("  → Jump Height = %d", value); break;
        case 104: g_AutoRevive.store(boolean);      LOGI("  → Auto Revive = %d", (int)boolean); break;
        case 105: g_PowerDuration.store(value);     LOGI("  → Power Duration = %d", value); break;
        case 106: g_FastLaneChange.store(boolean);  LOGI("  → Fast Lane Change = %d", (int)boolean); break;
        case 107: g_ScoreMultiplier.store(value);   LOGI("  → Score Multiplier = %d", value); break;

        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) {
                g_NoCollisionStartMs.store(nowMs());
                LOGI("  → No Collision Detect = ON (1s window)");
            } else {
                g_NoCollisionStartMs.store(0);
                LOGI("  → No Collision Detect = OFF");
            }
        } break;

        case 109: g_FreeShopping.store(boolean);      LOGI("  → Free Shopping = %d", (int)boolean); break;
        case 110: g_UnlimitedCoins.store(boolean);    LOGI("  → Unlimited Coins = %d", (int)boolean); break;

        case 120: chamsint = value;                    LOGI("  → Chams Mode = %d", value); break;
        case 121: enableRainbow = boolean;             LOGI("  → Rainbow Visible = %d", (int)boolean); break;
        case 122: enableRainbowWall = boolean;         LOGI("  → Rainbow In-Wall = %d", (int)boolean); break;
        case 123: /* legacy */ break;
        case 124: if (boolean) { dumpShadersToFile(); LOGI("  → Shader dump requested"); } break;
        case 125: setColorPreset(value);               LOGI("  → Color preset = %d", value); break;

        // ---------- Presets ----------
        case 800: {
            if (boolean) {
                LOGI("  → Applying SIMPLE mode");
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_UnlimitedCoins.store(true);
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
                g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
                g_FreeShopping.store(false);
            } else {
                LOGI("  → Removing SIMPLE mode");
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_UnlimitedCoins.store(false);
            }
        } break;

        case 801: {
            if (boolean) {
                LOGI("  → Applying MAX mode");
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true);
                g_JumpLimit.store(100);
                g_JumpHeight.store(50);
                g_PowerDuration.store(100);
                g_ScoreMultiplier.store(10);
                g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
                g_FreeShopping.store(false);
            } else {
                LOGI("  → Removing MAX mode");
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
            }
        } break;

        case 802: {
            if (boolean) {
                LOGI("  → Applying ULTRA MAX mode");
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true);
                g_NoCollision.store(true);
                g_NoCollisionStartMs.store(nowMs());
                g_FreeShopping.store(true);
                g_JumpLimit.store(999);
                g_JumpHeight.store(200);
                g_PowerDuration.store(999);
                g_ScoreMultiplier.store(999);
            } else {
                LOGI("  → Removing ULTRA MAX mode");
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_FastLaneChange.store(false);
                g_UnlimitedCoins.store(false);
                g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
                g_FreeShopping.store(false);
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_ScoreMultiplier.store(0);
            }
        } break;

        case 803: {
            LOGI("  → RESET ALL");
            g_FrontalImpact.store(false);
            g_SideImpact.store(false);
            g_AutoRevive.store(false);
            g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false);
            g_NoCollision.store(false);
            g_NoCollisionStartMs.store(0);
            g_FreeShopping.store(false);
            g_JumpLimit.store(0);
            g_JumpHeight.store(0);
            g_PowerDuration.store(0);
            g_ScoreMultiplier.store(0);
        } break;

        default:
            LOGW("Changes() unknown featNum = %d", featNum);
            break;
    }
}

// ================================================================
//     STUBS
// ================================================================
extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Menu_Draw(JNIEnv*, jclass, jobject, jobject) { }

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Menu_SetTeleportTargetNorm(JNIEnv*, jclass, jfloat, jfloat) { }

extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_support_Menu_GetTeleportEnabled(JNIEnv*, jclass) { return JNI_FALSE; }

extern "C" JNIEXPORT jboolean JNICALL
Java_com_android_support_Menu_IsSmoothTeleportActive(JNIEnv*, jclass) { return JNI_FALSE; }

extern "C" JNIEXPORT void JNICALL
Java_com_android_support_Main_setNativeCrashDir(JNIEnv*, jclass, jstring) { }

// ================================================================
//                     CONSTRUCTOR
// ================================================================
__attribute__((constructor))
void lib_main() {
    openLogFile();
    installCrashHandlers();

    LOGI("════════════════════════════════════════════");
    LOGI("  ModX Lab library constructed");
    LOGI("  Log file: %s", g_LogPath[0] ? g_LogPath : "(unavailable)");
    LOGI("════════════════════════════════════════════");

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}