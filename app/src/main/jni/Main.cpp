// ================================================================
// ModX Lab — Subway Surfers
// Auto-Update Hook System + Chams (Train-Wall Optimized)
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
#include <android/log.h>

#include "Chams.h"

#define LIB OBFUSCATE("libil2cpp.so")

// ================================================================
//  📁 THREAD-SAFE LOGGER
// ================================================================
static FILE*             g_LogFile  = nullptr;
static pthread_mutex_t   g_LogMutex = PTHREAD_MUTEX_INITIALIZER;
static char              g_LogPath[256] = {0};
static std::atomic<bool> g_LogReady {false};

#define LOG_TAG "MODXLAB"

static void openLogFile() {
    if (g_LogFile) return;
    const char* candidates[] = {
        "/sdcard/Documents/ModXLab_Log.txt",
        "/storage/emulated/0/Documents/ModXLab_Log.txt",
        "/sdcard/Android/data/com.kiloo.subwaysurf/files/ModXLab_Log.txt",
        "/data/local/tmp/ModXLab_Log.txt"
    };
    for (auto path : candidates) {
        FILE* f = fopen(path, "a");
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

static void LOGX(const char* level, const char* fmt, ...) {
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

static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//   FEATURE STATE  (★ g_FreeShopping REMOVED)
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
static int   (*old_GetCurrency)(...)            = nullptr;
// ★ old_get_IsIAP REMOVED

// ================================================================
//                       HOOK IMPLEMENTATIONS
// ================================================================
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) return false;
    if (g_FrontalImpact.load()) return false;
    if (!old_CheckFrontalImpact) return false;
    return old_CheckFrontalImpact(self, other);
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) return false;
    if (g_SideImpact.load()) return false;
    if (!old_CheckSideImpact) return false;
    return old_CheckSideImpact(self, other);
}

static int new_get_JumpLimit(void* self) {
    if (!self) return 0;
    int v = g_JumpLimit.load();
    if (v > 0) return v;
    if (!old_get_JumpLimit) return 0;
    return old_get_JumpLimit(self);
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    if (v > 0) return (float)v;
    if (!old_get_JumpHeight) return 0.0f;
    return old_get_JumpHeight(self);
}

static float new_get_LaneChangeDuration(void* self) {
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    if (!old_get_LaneChangeDuration) return 0.0f;
    return old_get_LaneChangeDuration(self);
}

static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) return false;
    if (g_AutoRevive.load()) return true;
    if (!old_IsAutoReviveEnabled) return false;
    return old_IsAutoReviveEnabled(self);
}

static float new_GetDuration(void* self, void* cfg) {
    if (!self) return 0.0f;
    int v = g_PowerDuration.load();
    if (v > 0) return (float)v;
    if (!old_GetDuration) return 0.0f;
    return old_GetDuration(self, cfg);
}

static int new_get_BaseMultiplierSum(void* self) {
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    if (v > 0) return v;
    if (!old_get_BaseMultiplierSum) return 0;
    return old_get_BaseMultiplierSum(self);
}

static bool new_get_detectCollisions(void* self) {
    if (!self) return true;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0) {
            long long elapsed = nowMs() - start;
            if (elapsed >= 0 && elapsed < 1000) return false;
        }
    }
    if (!old_get_detectCollisions) return true;
    return old_get_detectCollisions(self);
}

// ★ get_IsIAP hook REMOVED — was causing SIGILL crash

static int new_GetCurrency(void* self, int currencyType) {
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    if (!old_GetCurrency) return 0;
    return old_GetCurrency(self, currencyType);
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    LOGI("════════════════════════════════════════════");
    LOGI("   ModX Lab hack_thread starting...");
    LOGI("════════════════════════════════════════════");

    LOGI("Waiting for libil2cpp.so...");
    int waitCount = 0;
    while (!isLibraryLoaded(LIB)) {
        sleep(1);
        waitCount++;
        if (waitCount % 5 == 0) LOGI("  still waiting (%d sec)...", waitCount);
    }
    LOGI("✅ libil2cpp.so loaded after %d sec", waitCount);

    sleep(3);
    LOGI("IL2CPP init buffer complete — installing hooks");

    // 1) CharacterMotor
    LOGI("─── [1/7] CharacterMotor ───");
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oF = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oS = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oF) { HOOK_AU((void*)oF, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact); LOGI("  ✅ CheckFrontalImpact @ 0x%X", oF); }
            if (oS) { HOOK_AU((void*)oS, (void*)new_CheckSideImpact,    old_CheckSideImpact);    LOGI("  ✅ CheckSideImpact @ 0x%X", oS); }
        } else LOGE("  ❌ CharacterMotor NOT FOUND");
    }

    // 2) CharacterMotorAbilities
    LOGI("─── [2/7] CharacterMotorAbilities ───");
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                 OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJL = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLC = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oJL) { HOOK_AU((void*)oJL, (void*)new_get_JumpLimit, old_get_JumpLimit); LOGI("  ✅ get_JumpLimit @ 0x%X", oJL); }
            if (oJH) { HOOK_AU((void*)oJH, (void*)new_get_JumpHeight, old_get_JumpHeight); LOGI("  ✅ get_JumpHeight @ 0x%X", oJH); }
            if (oLC) { HOOK_AU((void*)oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration); LOGI("  ✅ get_LaneChangeDuration @ 0x%X", oLC); }
        } else LOGE("  ❌ CharacterMotorAbilities NOT FOUND");
    }

    // 3) StumbleBehaviour
    LOGI("─── [3/7] StumbleBehaviour ───");
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oR = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oR) { HOOK_AU((void*)oR, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled); LOGI("  ✅ IsAutoReviveEnabled @ 0x%X", oR); }
        } else LOGE("  ❌ StumbleBehaviour NOT FOUND");
    }

    // 4) PowerConfig
    LOGI("─── [4/7] PowerConfig ───");
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oD = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oD) { HOOK_AU((void*)oD, (void*)new_GetDuration, old_GetDuration); LOGI("  ✅ GetDuration @ 0x%X", oD); }
        } else LOGE("  ❌ PowerConfig NOT FOUND");
    }

    // 5) ScoreMultiplierManager
    LOGI("─── [5/7] ScoreMultiplierManager ───");
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oB = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oB) { HOOK_AU((void*)oB, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum); LOGI("  ✅ get_BaseMultiplierSum @ 0x%X", oB); }
        } else LOGE("  ❌ ScoreMultiplierManager NOT FOUND");
    }

    // 6) CharacterController
    LOGI("─── [6/7] CharacterController ───");
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oD = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oD) { HOOK_AU((void*)oD, (void*)new_get_detectCollisions, old_get_detectCollisions); LOGI("  ✅ get_detectCollisions @ 0x%X", oD); }
        } else LOGE("  ❌ CharacterController NOT FOUND");
    }

    // 7) WalletModel
    LOGI("─── [7/7] WalletModel ───");
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oC = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oC) { HOOK_AU((void*)oC, (void*)new_GetCurrency, old_GetCurrency); LOGI("  ✅ GetCurrency @ 0x%X", oC); }
        } else LOGE("  ❌ WalletModel NOT FOUND");
    }

    // Chams
    LOGI("─── [Chams] GLES2 hooks ───");
    RenderHook();

    LOGI("════════════════════════════════════════════");
    LOGI("  ✅ ALL HOOKS INSTALLED");
    LOGI("  Log: %s", g_LogPath[0] ? g_LogPath : "(none)");
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

        OBFUSCATE("Category_Shop"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),

        // ============================================================
        //  ★ CHAMS TAB — Clean & Focused
        // ============================================================
        OBFUSCATE("Category_Chams"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>CHAMS MODE</font></b>"),
        OBFUSCATE("120_SeekBar_Mode_0_4"),
        OBFUSCATE("SmallTextView_<font color='#CCCCCC'>0=OFF  1=Outline  2=X-Ray  3=Wire  4=Full★</font>"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>COLOR</font></b>"),
        OBFUSCATE("125_SeekBar_Preset_0_5"),
        OBFUSCATE("SmallTextView_<font color='#CCCCCC'>0=Green  1=Cyan  2=Yellow  3=Orange  4=White  5=Stealth</font>"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>ANIMATION</font></b>"),
        OBFUSCATE("121_Toggle_Rainbow Visible"),
        OBFUSCATE("122_Toggle_Rainbow In-Wall"),

        OBFUSCATE("SmallTextView_<b><font color='#00FFFF'>DIAGNOSTICS</font></b>"),
        OBFUSCATE("124_Toggle_Dump Shaders"),
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

        case 100: g_FrontalImpact.store(boolean);   LOGI("  Frontal=%d", (int)boolean); break;
        case 101: g_SideImpact.store(boolean);      LOGI("  Side=%d", (int)boolean); break;
        case 102: g_JumpLimit.store(value);         LOGI("  JumpLimit=%d", value); break;
        case 103: g_JumpHeight.store(value);        LOGI("  JumpHeight=%d", value); break;
        case 104: g_AutoRevive.store(boolean);      LOGI("  AutoRevive=%d", (int)boolean); break;
        case 105: g_PowerDuration.store(value);     LOGI("  PowerDur=%d", value); break;
        case 106: g_FastLaneChange.store(boolean);  LOGI("  FastLane=%d", (int)boolean); break;
        case 107: g_ScoreMultiplier.store(value);   LOGI("  Score=%d", value); break;

        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else         g_NoCollisionStartMs.store(0);
            LOGI("  NoCollision=%d", (int)boolean);
        } break;

        // case 109 (Free Shopping) REMOVED — was causing crash
        case 110: g_UnlimitedCoins.store(boolean); LOGI("  UnlimCoins=%d", (int)boolean); break;

        // ---------- ★ Chams ----------
        case 120: {
            chamsint = value;
            LOGI("  Chams Mode = %d", value);
        } break;

        case 121: enableRainbow = boolean;       LOGI("  Rainbow Visible=%d", (int)boolean); break;
        case 122: enableRainbowWall = boolean;   LOGI("  Rainbow InWall=%d", (int)boolean); break;
        case 124: if (boolean) { dumpShadersToFile(); LOGI("  Shader dump"); } break;
        case 125: setColorPreset(value);          LOGI("  Color Preset = %d", value); break;

        // ---------- Presets ----------
        case 800: {
            if (boolean) {
                LOGI("  → SIMPLE");
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
            } else {
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_UnlimitedCoins.store(false);
            }
        } break;

        case 801: {
            if (boolean) {
                LOGI("  → MAX");
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
            } else {
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
            }
        } break;

        case 802: {
            if (boolean) {
                LOGI("  → ULTRA MAX");
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true);
                g_NoCollision.store(true);
                g_NoCollisionStartMs.store(nowMs());
                g_JumpLimit.store(999);
                g_JumpHeight.store(200);
                g_PowerDuration.store(999);
                g_ScoreMultiplier.store(999);
            } else {
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_FastLaneChange.store(false);
                g_UnlimitedCoins.store(false);
                g_NoCollision.store(false);
                g_NoCollisionStartMs.store(0);
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_ScoreMultiplier.store(0);
            }
        } break;

        case 803: {
            LOGI("  → RESET");
            g_FrontalImpact.store(false);
            g_SideImpact.store(false);
            g_AutoRevive.store(false);
            g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false);
            g_NoCollision.store(false);
            g_NoCollisionStartMs.store(0);
            g_JumpLimit.store(0);
            g_JumpHeight.store(0);
            g_PowerDuration.store(0);
            g_ScoreMultiplier.store(0);
        } break;

        default:
            LOGW("Unknown featNum=%d", featNum);
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
    LOGI("════════════════════════════════════════════");
    LOGI("  ModX Lab constructed");
    LOGI("  Log: %s", g_LogPath[0] ? g_LogPath : "(none)");
    LOGI("════════════════════════════════════════════");

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}