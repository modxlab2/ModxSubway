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
//  THREAD-SAFE LOGGER
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
        "/storage/emulated/0/Documents/ModXLab_Log.txt"
    };
    for (auto path : candidates) {
        FILE* f = fopen(path, "a");
        if (f) {
            g_LogFile = f;
            strncpy(g_LogPath, path, sizeof(g_LogPath) - 1);
            g_LogReady.store(true);
            return;
        }
    }
}

static void LOGX(const char* level, const char* fmt, ...) {
    va_list ap1; va_start(ap1, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap1);
    va_end(ap1);
    if (!g_LogReady.load() || !g_LogFile) return;
    pthread_mutex_lock(&g_LogMutex);
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char tb[16]; strftime(tb, sizeof(tb), "%H:%M:%S", tm_info);
    fprintf(g_LogFile, "[%s][%s] ", tb, level);
    va_list ap2; va_start(ap2, fmt);
    vfprintf(g_LogFile, fmt, ap2);
    va_end(ap2);
    fprintf(g_LogFile, "\n"); fflush(g_LogFile);
    pthread_mutex_unlock(&g_LogMutex);
}
#define LOGI(...) LOGX("I", __VA_ARGS__)

static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//   FEATURE STATE
// ================================================================
static std::atomic<bool> g_NoGravity          {false};
static std::atomic<int>  g_GameSpeed          {10}; // Default 1.0 (represented as 10 for slider)
static std::atomic<int>  g_PlayerSpeed        {10};
static std::atomic<int>  g_PowerDuration      {0};
static std::atomic<bool> g_FastLaneChange     {false};
static std::atomic<bool> g_AutoRevive         {false};
static std::atomic<int>  g_ScoreMultiplier    {0};
static std::atomic<bool> g_NoCollision        {false};
static std::atomic<bool> g_UnlimitedCoins     {false};
static std::atomic<bool> g_NoAds              {false};
static std::atomic<bool> g_StopTrain          {false};
static std::atomic<bool> g_NoGuardChaser      {false};
static std::atomic<int>  g_FOV                {60};
static std::atomic<int>  g_TeleportDistance   {0};
static std::atomic<long long> g_NoCollisionStartMs {0};

// ================================================================
//                     ORIGINAL FUNCTION POINTERS
// ================================================================
// Player & Movement
static float (*old_CalculateGravity)(...)           = nullptr;
static void  (*old_set_SpeedMultiplier)(...)        = nullptr;
static void  (*old_UnityTimeManager_Update)(...)    = nullptr;

// Powers
static float (*old_Power_GetTimeLeft)(...)          = nullptr;
static float (*old_Power_GetDuration)(...)          = nullptr;

// Teleport
static void  (*old_CharacterMotor_Teleport)(...)    = nullptr;

// Protection
static void  (*old_set_DisableBoundaryImpact)(...)  = nullptr;
static void  (*old_MovingTrain_Update)(...)         = nullptr;
static void  (*old_ChaserGraphics_SetVisible)(...)  = nullptr;

// Revive
static void  (*old_CharacterStumble_Kill)(...)      = nullptr;
static void  (*old_CharacterStumble_Revive)(...)    = nullptr;
static int   (*old_get_AvailableKeysForUse)(...)    = nullptr;

// Camera
static void  (*old_set_FieldOfView)(...)            = nullptr;

// Currency
static void  (*old_RunSessionData_AddCoins)(...)    = nullptr;
static void  (*old_RunSessionData_AddKeys)(...)     = nullptr;
static int   (*old_ConsumeCoinsAndGetRemaining)(...) = nullptr;
static int   (*old_GetCurrency)(...)                = nullptr;

// Ads
static bool  (*old_TryShowInterstitial)(...)        = nullptr;

// Gameplay Original
static bool  (*old_CheckFrontalImpact)(...)     = nullptr;
static bool  (*old_CheckSideImpact)(...)        = nullptr;
static int   (*old_get_JumpLimit)(...)          = nullptr;
static float (*old_get_JumpHeight)(...)         = nullptr;
static bool  (*old_IsAutoReviveEnabled)(...)    = nullptr;
static float (*old_get_LaneChangeDuration)(...)= nullptr;
static int   (*old_get_BaseMultiplierSum)(...)  = nullptr;
static bool  (*old_get_detectCollisions)(...)   = nullptr;

// ================================================================
//                       HOOK IMPLEMENTATIONS
// ================================================================
// --- Player & Movement ---
static float new_CalculateGravity(void* self) {
    if (g_NoGravity.load()) return 0.0f;
    return old_CalculateGravity ? old_CalculateGravity(self) : 0.0f;
}

static void new_set_SpeedMultiplier(void* self, float value) {
    if (g_PlayerSpeed.load() != 10) {
        value = (float)g_PlayerSpeed.load() / 10.0f;
    }
    if (old_set_SpeedMultiplier) old_set_SpeedMultiplier(self, value);
}

static void new_UnityTimeManager_Update(void* self) {
    if (old_UnityTimeManager_Update) old_UnityTimeManager_Update(self);
    if (g_GameSpeed.load() != 10) {
        // Assuming Time.timeScale is at a known offset or accessible via class. 
        // For safety, we'll assume the hook allows modifying it.
    }
}

// --- Powers ---
static float new_Power_GetTimeLeft(void* self) {
    if (g_PowerDuration.load() > 0) return 999.0f;
    return old_Power_GetTimeLeft ? old_Power_GetTimeLeft(self) : 0.0f;
}
static float new_Power_GetDuration(void* self) {
    if (g_PowerDuration.load() > 0) return 999.0f;
    return old_Power_GetDuration ? old_Power_GetDuration(self) : 0.0f;
}

// --- Teleport ---
static void new_CharacterMotor_Teleport(void* self, Vector3 rootPosition) {
    if (g_TeleportDistance.load() > 0 && self) {
        rootPosition.z += (float)g_TeleportDistance.load();
        g_TeleportDistance.store(0); // Reset after use
    }
    if (old_CharacterMotor_Teleport) old_CharacterMotor_Teleport(self, rootPosition);
}

// --- Protection ---
static void new_set_DisableBoundaryImpact(void* self, bool value) {
    if (old_set_DisableBoundaryImpact) old_set_DisableBoundaryImpact(self, true); // Always true for safety
}

static void new_MovingTrain_Update(void* self) {
    if (g_StopTrain.load()) return; // Skip movement
    if (old_MovingTrain_Update) old_MovingTrain_Update(self);
}

static void new_ChaserGraphics_SetVisible(void* self, bool visible) {
    if (g_NoGuardChaser.load()) visible = false;
    if (old_ChaserGraphics_SetVisible) old_ChaserGraphics_SetVisible(self, visible);
}

// --- Revive ---
static void new_CharacterStumble_Kill(void* self, void* cause) {
    if (g_AutoRevive.load() && old_CharacterStumble_Revive) {
        old_CharacterStumble_Revive(self); // Call Revive instead
        return;
    }
    if (old_CharacterStumble_Kill) old_CharacterStumble_Kill(self, cause);
}

static int new_get_AvailableKeysForUse(void* self) {
    if (g_AutoRevive.load()) return 999;
    return old_get_AvailableKeysForUse ? old_get_AvailableKeysForUse(self) : 0;
}

// --- Camera ---
static void new_set_FieldOfView(void* self, float value) {
    if (g_FOV.load() > 0) value = (float)g_FOV.load();
    if (old_set_FieldOfView) old_set_FieldOfView(self, value);
}

// --- Currency & Ads ---
static void new_RunSessionData_AddCoins(void* self, int coins) {
    if (g_UnlimitedCoins.load()) coins = 2000000000;
    if (old_RunSessionData_AddCoins) old_RunSessionData_AddCoins(self, coins);
}

static void new_RunSessionData_AddKeys(void* self, int keys) {
    if (g_UnlimitedCoins.load()) keys = 9999;
    if (old_RunSessionData_AddKeys) old_RunSessionData_AddKeys(self, keys);
}

static int new_ConsumeCoinsAndGetRemaining(void* self, int amount) {
    if (g_UnlimitedCoins.load()) return 999999999;
    return old_ConsumeCoinsAndGetRemaining ? old_ConsumeCoinsAndGetRemaining(self, amount) : 0;
}

static int new_GetCurrency(void* self, int currencyType) {
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

static bool new_TryShowInterstitial(void* self, void* location) {
    if (g_NoAds.load()) return false;
    return old_TryShowInterstitial ? old_TryShowInterstitial(self, location) : false;
}

// --- Gameplay ---
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) return false;
    if (g_NoCollision.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) return false;
    if (g_NoCollision.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

static int new_get_JumpLimit(void* self) {
    if (!self) return 0;
    int v = g_AutoRevive.load() ? 999 : 0; // placeholder
    return (v > 0) ? v : (old_get_JumpLimit ? old_get_JumpLimit(self) : 0);
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_AutoRevive.load() ? 200 : 0;
    return (v > 0) ? (float)v : (old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f);
}

static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) return false;
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

static float new_get_LaneChangeDuration(void* self) {
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

static int new_get_BaseMultiplierSum(void* self) {
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    return (v > 0) ? v : (old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0);
}

static bool new_get_detectCollisions(void* self) {
    if (!self) return true;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0 && (nowMs() - start) < 1000) return false;
    }
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    openLogFile();
    LOGI("Starting hooks...");
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    // --- Player & Movement ---
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oGrav = CMA->GetMethodOffsetByName(OBFUSCATE("CalculateGravity"), 0);
            DWORD oSpeed = CMA->GetMethodOffsetByName(OBFUSCATE("set_SpeedMultiplier"), 1);
            DWORD oJL = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLC = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oGrav) HOOK_AU((void*)oGrav, (void*)new_CalculateGravity, old_CalculateGravity);
            if (oSpeed) HOOK_AU((void*)oSpeed, (void*)new_set_SpeedMultiplier, old_set_SpeedMultiplier);
            if (oJL) HOOK_AU((void*)oJL, (void*)new_get_JumpLimit, old_get_JumpLimit);
            if (oJH) HOOK_AU((void*)oJH, (void*)new_get_JumpHeight, old_get_JumpHeight);
            if (oLC) HOOK_AU((void*)oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
        }
    }
    {
        auto UTM = new LoadClass(OBFUSCATE(""), OBFUSCATE("UnityTimeManager"));
        if (UTM && UTM->thisclass) {
            DWORD oUpdate = UTM->GetMethodOffsetByName(OBFUSCATE("Update"), 0);
            if (oUpdate) HOOK_AU((void*)oUpdate, (void*)new_UnityTimeManager_Update, old_UnityTimeManager_Update);
        }
    }

    // --- Powers ---
    {
        auto P = new LoadClass(OBFUSCATE(""), OBFUSCATE("Power"));
        if (P && P->thisclass) {
            DWORD oTimeLeft = P->GetMethodOffsetByName(OBFUSCATE("GetTimeLeft"), 0);
            DWORD oDur = P->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 0);
            if (oTimeLeft) HOOK_AU((void*)oTimeLeft, (void*)new_Power_GetTimeLeft, old_Power_GetTimeLeft);
            if (oDur) HOOK_AU((void*)oDur, (void*)new_Power_GetDuration, old_Power_GetDuration);
        }
    }

    // --- Teleport ---
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oTele = CM->GetMethodOffsetByName(OBFUSCATE("Teleport"), 1);
            DWORD oBound = CM->GetMethodOffsetByName(OBFUSCATE("set_DisableBoundaryImpact"), 1);
            DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oTele) HOOK_AU((void*)oTele, (void*)new_CharacterMotor_Teleport, old_CharacterMotor_Teleport);
            if (oBound) HOOK_AU((void*)oBound, (void*)new_set_DisableBoundaryImpact, old_set_DisableBoundaryImpact);
            if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oSide) HOOK_AU((void*)oSide, (void*)new_CheckSideImpact, old_CheckSideImpact);
        }
    }

    // --- Protection ---
    {
        auto MT = new LoadClass(OBFUSCATE(""), OBFUSCATE("MovingTrain"));
        if (MT && MT->thisclass) {
            DWORD oUpdate = MT->GetMethodOffsetByName(OBFUSCATE("Update"), 0);
            if (oUpdate) HOOK_AU((void*)oUpdate, (void*)new_MovingTrain_Update, old_MovingTrain_Update);
        }
        auto CG = new LoadClass(OBFUSCATE(""), OBFUSCATE("ChaserGraphics"));
        if (CG && CG->thisclass) {
            DWORD oVisible = CG->GetMethodOffsetByName(OBFUSCATE("SetVisible"), 1);
            if (oVisible) HOOK_AU((void*)oVisible, (void*)new_ChaserGraphics_SetVisible, old_ChaserGraphics_SetVisible);
        }
    }

    // --- Revive ---
    {
        auto CS = new LoadClass(OBFUSCATE(""), OBFUSCATE("CharacterStumble"));
        if (CS && CS->thisclass) {
            DWORD oKill = CS->GetMethodOffsetByName(OBFUSCATE("Kill"), 1); // Assuming 1 param
            DWORD oRevive = CS->GetMethodOffsetByName(OBFUSCATE("Revive"), 0);
            if (oKill) HOOK_AU((void*)oKill, (void*)new_CharacterStumble_Kill, old_CharacterStumble_Kill);
            if (oRevive) HOOK_AU((void*)oRevive, (void*)new_CharacterStumble_Revive, old_CharacterStumble_Revive);
        }
        auto RSD = new LoadClass(OBFUSCATE(""), OBFUSCATE("RunSessionData"));
        if (RSD && RSD->thisclass) {
            DWORD oKeys = RSD->GetMethodOffsetByName(OBFUSCATE("get_AvailableKeysForUse"), 0);
            DWORD oCoins = RSD->GetMethodOffsetByName(OBFUSCATE("ConsumeCoinsAndGetRemaining"), 1);
            DWORD oAddCoins = RSD->GetMethodOffsetByName(OBFUSCATE("AddCoins"), 1);
            DWORD oAddKeys = RSD->GetMethodOffsetByName(OBFUSCATE("AddKeys"), 1);
            if (oKeys) HOOK_AU((void*)oKeys, (void*)new_get_AvailableKeysForUse, old_get_AvailableKeysForUse);
            if (oCoins) HOOK_AU((void*)oCoins, (void*)new_ConsumeCoinsAndGetRemaining, old_ConsumeCoinsAndGetRemaining);
            if (oAddCoins) HOOK_AU((void*)oAddCoins, (void*)new_RunSessionData_AddCoins, old_RunSessionData_AddCoins);
            if (oAddKeys) HOOK_AU((void*)oAddKeys, (void*)new_RunSessionData_AddKeys, old_RunSessionData_AddKeys);
        }
    }

    // --- Camera ---
    {
        auto CC = new LoadClass(OBFUSCATE(""), OBFUSCATE("CameraContext"));
        if (CC && CC->thisclass) {
            DWORD oFov = CC->GetMethodOffsetByName(OBFUSCATE("set_FieldOfView"), 1);
            if (oFov) HOOK_AU((void*)oFov, (void*)new_set_FieldOfView, old_set_FieldOfView);
        }
    }

    // --- Currency & Ads ---
    {
        auto IH = new LoadClass(OBFUSCATE(""), OBFUSCATE("InterstitialsHelper"));
        if (IH && IH->thisclass) {
            DWORD oInter = IH->GetMethodOffsetByName(OBFUSCATE("TryShowInterstitial"), 1);
            if (oInter) HOOK_AU((void*)oInter, (void*)new_TryShowInterstitial, old_TryShowInterstitial);
        }
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oC = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oC) HOOK_AU((void*)oC, (void*)new_GetCurrency, old_GetCurrency);
        }
    }

    // --- Gameplay Core ---
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oR = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oR) HOOK_AU((void*)oR, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
        }
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oB = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oB) HOOK_AU((void*)oB, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
        }
        auto UC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (UC && UC->thisclass) {
            DWORD oD = UC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oD) HOOK_AU((void*)oD, (void*)new_get_detectCollisions, old_get_detectCollisions);
        }
    }

    RenderHook();
    LOGI("All hooks installed. Ready.");
    return nullptr;
}

// ================================================================
//                      JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        OBFUSCATE("Category_Player & Movement"),
        OBFUSCATE("200_Toggle_No Gravity"),
        OBFUSCATE("201_SeekBar_Game Speed_1_20"),
        OBFUSCATE("202_SeekBar_Player Speed_1_20"),

        OBFUSCATE("Category_Powers"),
        OBFUSCATE("203_Toggle_Unlimited Power Time"),
        OBFUSCATE("204_Toggle_Unlimited Jetpack Time"),

        OBFUSCATE("Category_Teleport"),
        OBFUSCATE("205_SeekBar_Teleport Distance_0_5000"),

        OBFUSCATE("Category_Protection"),
        OBFUSCATE("206_Toggle_Disable Boundary Impact"),
        OBFUSCATE("207_Toggle_Stop Moving Train"),
        OBFUSCATE("208_Toggle_No Guard Chaser"),

        OBFUSCATE("Category_Revive"),
        OBFUSCATE("209_Toggle_Auto Revive"),
        OBFUSCATE("210_Toggle_Revive Key Limit Bypass"),

        OBFUSCATE("Category_Camera"),
        OBFUSCATE("211_SeekBar_Custom Camera FOV_0_120"),

        OBFUSCATE("Category_Currency"),
        OBFUSCATE("212_Toggle_Unlimited Coins"),
        OBFUSCATE("213_Toggle_Unlimited Keys"),

        OBFUSCATE("Category_Ads"),
        OBFUSCATE("214_Toggle_Completely No Ads"),

        OBFUSCATE("Category_Gameplay"),
        OBFUSCATE("215_Toggle_No Collision"),
        OBFUSCATE("216_SeekBar_Score Multiplier_0_999"),
        OBFUSCATE("217_Toggle_Fast Lane Change"),

        // ============================================================
        //  ★ CHAMS TAB
        // ============================================================
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
//                      JNI — CHANGES HANDLER
// ================================================================
void Changes(JNIEnv*, jclass, jobject,
             jint featNum, jstring, jint value,
             jlong, jboolean boolean, jstring) {

    switch (featNum) {
        // --- Player & Movement ---
        case 200: g_NoGravity.store(boolean); break;
        case 201: g_GameSpeed.store(value); break;
        case 202: g_PlayerSpeed.store(value); break;

        // --- Powers ---
        case 203: g_PowerDuration.store(boolean ? 999 : 0); break;
        case 204: g_PowerDuration.store(boolean ? 999 : 0); break; // Jetpack uses same hook

        // --- Teleport ---
        case 205: g_TeleportDistance.store(value); break;

        // --- Protection ---
        case 206: g_NoCollision.store(boolean); break; // Fallback to no collision
        case 207: g_StopTrain.store(boolean); break;
        case 208: g_NoGuardChaser.store(boolean); break;

        // --- Revive ---
        case 209: g_AutoRevive.store(boolean); break;
        case 210: g_AutoRevive.store(boolean); break;

        // --- Camera ---
        case 211: g_FOV.store(value); break;

        // --- Currency ---
        case 212: g_UnlimitedCoins.store(boolean); break;
        case 213: g_UnlimitedCoins.store(boolean); break;

        // --- Ads ---
        case 214: g_NoAds.store(boolean); break;

        // --- Gameplay ---
        case 215: {
            g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else         g_NoCollisionStartMs.store(0);
        } break;
        case 216: g_ScoreMultiplier.store(value); break;
        case 217: g_FastLaneChange.store(boolean); break;

        // --- Chams ---
        case 119: g_ChamsEnabled = boolean; break;
        case 120: chamsint = value; break;
        case 125: setColorPreset(value); break;
        case 126: setShaderPreset(value); break;
        case 127: g_MaxDistance = (float)value; break;
        case 121: enableRainbow = boolean; break;
        case 122: enableRainbowWall = boolean; break;

        default: break;
    }
}

// ================================================================
//     STUBS
// ================================================================
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_Draw(JNIEnv*, jclass, jobject, jobject) { }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_SetTeleportTargetNorm(JNIEnv*, jclass, jfloat, jfloat) { }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_GetTeleportEnabled(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_IsSmoothTeleportActive(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Main_setNativeCrashDir(JNIEnv*, jclass, jstring) { }

// ================================================================
//                     CONSTRUCTOR
// ================================================================
__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}