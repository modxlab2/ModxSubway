// ================================================================
// ModX Lab — Subway Surfers (Unity / IL2CPP)
// Auto-Update Hook System
// ----------------------------------------------------------------
// সব offset runtime-এ il2cpp metadata থেকে resolve হয়
// (LoadClass + GetMethodOffsetByName)। গেম আপডেট হলেও,
// offset বদলালেও hook কাজ করবে — এটাই "auto update"।
// ================================================================

#include "Includes.h"
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.h"
#include "Includes/Strings.h"
#include "Includes/Macros.h"
#include "KittyMemory/MemoryPatch.h"
#include "AutoHook/AutoHook.h"

#define LIB OBFUSCATE("libil2cpp.so")

// ================================================================
//                        FEATURE FLAGS
// ================================================================
static std::atomic<bool> g_FrontalImpact    {false};
static std::atomic<bool> g_SideImpact       {false};
static std::atomic<bool> g_JumpLimit        {false};
static std::atomic<bool> g_JumpHeight       {false};
static std::atomic<bool> g_AutoRevive       {false};
static std::atomic<bool> g_PowerDuration    {false};
static std::atomic<bool> g_LaneChangeDur    {false};
static std::atomic<bool> g_ScoreMultiplier  {false};
static std::atomic<bool> g_DetectCollisions {false};
static std::atomic<bool> g_IsIAP            {false};
static std::atomic<bool> g_UnlimitedCoin    {false};

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

// --- CharacterMotor ---
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (g_FrontalImpact.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (g_SideImpact.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

// --- CharacterMotorAbilities ---
static int new_get_JumpLimit(void* self) {
    if (g_JumpLimit.load()) return 999;
    return old_get_JumpLimit ? old_get_JumpLimit(self) : 0;
}

static float new_get_JumpHeight(void* self) {
    if (g_JumpHeight.load()) return 35.0f;
    return old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f;
}

static float new_get_LaneChangeDuration(void* self) {
    if (g_LaneChangeDur.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

// --- StumbleBehaviour ---
static bool new_IsAutoReviveEnabled(void* self) {
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

// --- PowerConfig ---
static float new_GetDuration(void* self, void* cfg) {
    if (g_PowerDuration.load()) return 999.0f;
    return old_GetDuration ? old_GetDuration(self, cfg) : 0.0f;
}

// --- ScoreMultiplierManager ---
static int new_get_BaseMultiplierSum(void* self) {
    if (g_ScoreMultiplier.load()) return 999999;
    return old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0;
}

// --- UnityEngine.CharacterController ---
static bool new_get_detectCollisions(void* self) {
    if (g_DetectCollisions.load()) return false;
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}

// --- SYBO.Subway.Core.CommonData.Currency ---
static bool new_get_IsIAP(void* self) {
    if (g_IsIAP.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}

// --- SYBO.Subway.Core.ProfileData.WalletModel ---
static int new_GetCurrency(void* self, int currencyType) {
    if (g_UnlimitedCoin.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    do {
        sleep(1);
    } while (!isLibraryLoaded(LIB));

    LOGD(OBFUSCATE("il2cpp loaded — installing auto-update hooks"));

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterMotor
    // ------------------------------------------------------------
    auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                            OBFUSCATE("CharacterMotor"));
    if (CM) {
        DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
        DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
        if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
        if (oSide)    HOOK_AU((void*)oSide,    (void*)new_CheckSideImpact,    old_CheckSideImpact);
        LOGD(OBFUSCATE("CharacterMotor hooks: frontal=%p side=%p"),
             (void*)oFrontal, (void*)oSide);
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterMotorAbilities
    // ------------------------------------------------------------
    auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                             OBFUSCATE("CharacterMotorAbilities"));
    if (CMA) {
        DWORD oJumpLim = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
        DWORD oJumpH   = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
        DWORD oLane    = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
        if (oJumpLim) HOOK_AU((void*)oJumpLim, (void*)new_get_JumpLimit,        old_get_JumpLimit);
        if (oJumpH)   HOOK_AU((void*)oJumpH,   (void*)new_get_JumpHeight,       old_get_JumpHeight);
        if (oLane)    HOOK_AU((void*)oLane,    (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
        LOGD(OBFUSCATE("Abilities hooks: jl=%p jh=%p lc=%p"),
             (void*)oJumpLim, (void*)oJumpH, (void*)oLane);
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.StumbleBehaviour
    // ------------------------------------------------------------
    auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"),
                            OBFUSCATE("StumbleBehaviour"));
    if (SB) {
        DWORD oRevive = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
        if (oRevive) HOOK_AU((void*)oRevive, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
        LOGD(OBFUSCATE("StumbleBehaviour hook: %p"), (void*)oRevive);
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Powers.PowerConfig
    // ------------------------------------------------------------
    auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"),
                            OBFUSCATE("PowerConfig"));
    if (PC) {
        DWORD oDur = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
        if (oDur) HOOK_AU((void*)oDur, (void*)new_GetDuration, old_GetDuration);
        LOGD(OBFUSCATE("PowerConfig hook: %p"), (void*)oDur);
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.ScoreMultiplierManager
    // ------------------------------------------------------------
    auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"),
                             OBFUSCATE("ScoreMultiplierManager"));
    if (SMM) {
        DWORD oBase = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
        if (oBase) HOOK_AU((void*)oBase, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
        LOGD(OBFUSCATE("ScoreMultiplier hook: %p"), (void*)oBase);
    }

    // ------------------------------------------------------------
    //  UnityEngine.CharacterController
    // ------------------------------------------------------------
    auto CC = new LoadClass(OBFUSCATE("UnityEngine"),
                            OBFUSCATE("CharacterController"));
    if (CC) {
        DWORD oDetect = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
        if (oDetect) HOOK_AU((void*)oDetect, (void*)new_get_detectCollisions, old_get_detectCollisions);
        LOGD(OBFUSCATE("CharacterController hook: %p"), (void*)oDetect);
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.Core.CommonData.Currency
    // ------------------------------------------------------------
    auto CUR = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"),
                             OBFUSCATE("Currency"));
    if (CUR) {
        DWORD oIAP = CUR->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
        if (oIAP) HOOK_AU((void*)oIAP, (void*)new_get_IsIAP, old_get_IsIAP);
        LOGD(OBFUSCATE("Currency hook: %p"), (void*)oIAP);
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.Core.ProfileData.WalletModel
    // ------------------------------------------------------------
    auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"),
                            OBFUSCATE("WalletModel"));
    if (WM) {
        DWORD oGetCur = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
        if (oGetCur) HOOK_AU((void*)oGetCur, (void*)new_GetCurrency, old_GetCurrency);
        LOGD(OBFUSCATE("WalletModel hook: %p"), (void*)oGetCur);
    }

    LOGD(OBFUSCATE("All auto-update hooks installed — ready"));
    return nullptr;
}

// ================================================================
//                      JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        OBFUSCATE("Category_Runner"),

        OBFUSCATE("100_Toggle_No Frontal Impact"),
        OBFUSCATE("101_Toggle_No Side Impact"),
        OBFUSCATE("102_Toggle_Infinite Jump Limit"),
        OBFUSCATE("103_Toggle_High Jump Height"),
        OBFUSCATE("104_Toggle_Auto Revive"),

        OBFUSCATE("Category_Powers"),

        OBFUSCATE("105_Toggle_Long Power Duration"),
        OBFUSCATE("106_Toggle_Instant Lane Change"),

        OBFUSCATE("Category_Score"),

        OBFUSCATE("107_Toggle_Score Multiplier x999999"),

        OBFUSCATE("Category_Collisions"),

        OBFUSCATE("108_Toggle_No Collision Detect"),

        OBFUSCATE("Category_Shop"),

        OBFUSCATE("109_Toggle_Free IAP (Ignore Purchases)"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),
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
    switch (featNum) {
        case 100: g_FrontalImpact.store(boolean);     break;
        case 101: g_SideImpact.store(boolean);        break;
        case 102: g_JumpLimit.store(boolean);         break;
        case 103: g_JumpHeight.store(boolean);        break;
        case 104: g_AutoRevive.store(boolean);        break;
        case 105: g_PowerDuration.store(boolean);     break;
        case 106: g_LaneChangeDur.store(boolean);     break;
        case 107: g_ScoreMultiplier.store(boolean);   break;
        case 108: g_DetectCollisions.store(boolean);  break;
        case 109: g_IsIAP.store(boolean);             break;
        case 110: g_UnlimitedCoin.store(boolean);     break;
    }
}

// ================================================================
//     STUBS — Menu.java যাতে UnsatisfiedLinkError না দেয়
//     (ESP / Teleport non-unity code সরানোর কারণে)
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
//                     CONSTRUCTOR (entry)
// ================================================================
__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}