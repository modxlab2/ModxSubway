// ================================================================
// ModX Lab — Subway Surfers (Unity / IL2CPP)
// Auto-Update Hook System  (32bit + 64bit)
// ----------------------------------------------------------------
// সব offset runtime-এ il2cpp metadata থেকে resolve হয়
// (LoadClass + GetMethodOffsetByName)। গেম আপডেট হলেও,
// offset বদলালেও hook কাজ করবে — এটাই "auto update"।
//
// ★ সব hook function এ null guard — crash-proof
// ★ armeabi-v7a + arm64-v8a — দুটোতেই কাজ করবে
// ★ Main tab এ 4টি Mode Button:
//     800 = Simple Mode
//     801 = Max Mode
//     802 = Ultra Max Mode
//     803 = None (Reset)
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
//                     ★ Null-guarded (crash-proof)
// ================================================================

// --- SYBO.RunnerCore.Character.CharacterMotor ---
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) return false;                                // ★ NULL GUARD
    if (g_FrontalImpact.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) return false;                                // ★ NULL GUARD
    if (g_SideImpact.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

// --- SYBO.RunnerCore.Character.CharacterMotorAbilities ---
static int new_get_JumpLimit(void* self) {
    if (!self) return 0;                                    // ★ NULL GUARD
    if (g_JumpLimit.load()) return 999;
    return old_get_JumpLimit ? old_get_JumpLimit(self) : 0;
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;                                 // ★ NULL GUARD
    if (g_JumpHeight.load()) return 35.0f;
    return old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f;
}

static float new_get_LaneChangeDuration(void* self) {
    if (!self) return 0.0f;                                 // ★ NULL GUARD
    if (g_LaneChangeDur.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

// --- SYBO.Subway.StumbleBehaviour ---
static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) return false;                                // ★ NULL GUARD
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

// --- SYBO.RunnerCore.Powers.PowerConfig ---
static float new_GetDuration(void* self, void* cfg) {
    if (!self) return 0.0f;                                 // ★ NULL GUARD
    if (g_PowerDuration.load()) return 999.0f;
    return old_GetDuration ? old_GetDuration(self, cfg) : 0.0f;
}

// --- SYBO.Subway.ScoreMultiplierManager ---
static int new_get_BaseMultiplierSum(void* self) {
    if (!self) return 0;                                    // ★ NULL GUARD
    if (g_ScoreMultiplier.load()) return 999999;
    return old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0;
}

// --- UnityEngine.CharacterController ---
static bool new_get_detectCollisions(void* self) {
    if (!self) return true;                                 // ★ NULL GUARD
    if (g_DetectCollisions.load()) return false;
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}

// --- SYBO.Subway.Core.CommonData.Currency ---
static bool new_get_IsIAP(void* self) {
    if (!self) return false;                                // ★ NULL GUARD
    if (g_IsIAP.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}

// --- SYBO.Subway.Core.ProfileData.WalletModel ---
static int new_GetCurrency(void* self, int currencyType) {
    if (!self) return 0;                                    // ★ NULL GUARD
    if (g_UnlimitedCoin.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    // Step 1 — libil2cpp.so লোড হওয়ার অপেক্ষা
    do {
        sleep(1);
    } while (!isLibraryLoaded(LIB));

    // Step 2 — il2cpp fully initialize হওয়ার জন্য buffer
    sleep(3);

    LOGD(OBFUSCATE("il2cpp loaded — installing auto-update hooks"));

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterMotor
    // ------------------------------------------------------------
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oSide)    HOOK_AU((void*)oSide,    (void*)new_CheckSideImpact,    old_CheckSideImpact);
            LOGD(OBFUSCATE("CharacterMotor hooks: frontal=%p side=%p"),
                 (void*)oFrontal, (void*)oSide);
        } else {
            LOGD(OBFUSCATE("CharacterMotor class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterMotorAbilities
    // ------------------------------------------------------------
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                 OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJumpLim = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJumpH   = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLane    = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oJumpLim) HOOK_AU((void*)oJumpLim, (void*)new_get_JumpLimit,           old_get_JumpLimit);
            if (oJumpH)   HOOK_AU((void*)oJumpH,   (void*)new_get_JumpHeight,          old_get_JumpHeight);
            if (oLane)    HOOK_AU((void*)oLane,    (void*)new_get_LaneChangeDuration,  old_get_LaneChangeDuration);
            LOGD(OBFUSCATE("Abilities hooks: jl=%p jh=%p lc=%p"),
                 (void*)oJumpLim, (void*)oJumpH, (void*)oLane);
        } else {
            LOGD(OBFUSCATE("CharacterMotorAbilities class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.StumbleBehaviour
    // ------------------------------------------------------------
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"),
                                OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oRevive = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oRevive) HOOK_AU((void*)oRevive, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
            LOGD(OBFUSCATE("StumbleBehaviour hook: %p"), (void*)oRevive);
        } else {
            LOGD(OBFUSCATE("StumbleBehaviour class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Powers.PowerConfig
    // ------------------------------------------------------------
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"),
                                OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oDur = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oDur) HOOK_AU((void*)oDur, (void*)new_GetDuration, old_GetDuration);
            LOGD(OBFUSCATE("PowerConfig hook: %p"), (void*)oDur);
        } else {
            LOGD(OBFUSCATE("PowerConfig class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.ScoreMultiplierManager
    // ------------------------------------------------------------
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"),
                                 OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oBase = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oBase) HOOK_AU((void*)oBase, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
            LOGD(OBFUSCATE("ScoreMultiplier hook: %p"), (void*)oBase);
        } else {
            LOGD(OBFUSCATE("ScoreMultiplierManager class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  UnityEngine.CharacterController
    // ------------------------------------------------------------
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"),
                                OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oDetect = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oDetect) HOOK_AU((void*)oDetect, (void*)new_get_detectCollisions, old_get_detectCollisions);
            LOGD(OBFUSCATE("CharacterController hook: %p"), (void*)oDetect);
        } else {
            LOGD(OBFUSCATE("CharacterController class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.Core.CommonData.Currency
    // ------------------------------------------------------------
    {
        auto CUR = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"),
                                 OBFUSCATE("Currency"));
        if (CUR && CUR->thisclass) {
            DWORD oIAP = CUR->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (oIAP) HOOK_AU((void*)oIAP, (void*)new_get_IsIAP, old_get_IsIAP);
            LOGD(OBFUSCATE("Currency hook: %p"), (void*)oIAP);
        } else {
            LOGD(OBFUSCATE("Currency class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.Core.ProfileData.WalletModel
    // ------------------------------------------------------------
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"),
                                OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oGetCur = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oGetCur) HOOK_AU((void*)oGetCur, (void*)new_GetCurrency, old_GetCurrency);
            LOGD(OBFUSCATE("WalletModel hook: %p"), (void*)oGetCur);
        } else {
            LOGD(OBFUSCATE("WalletModel class not found — skipped"));
        }
    }

    LOGD(OBFUSCATE("All auto-update hooks installed — ready"));
    return nullptr;
}

// ================================================================
//                      JNI — FEATURE LIST
// ----------------------------------------------------------------
//  ★ Main tab এ 4টি ButtonOnOff (Simple / Max / Ultra Max / None)
//  ★ তারপর প্রতিটি Category (Runner / Powers / Score / ...)
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        // ========================================================
        //  MAIN TAB — Mode Presets (ButtonOnOff)
        // ========================================================
        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),

        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Simple:</font></b> No Frontal/Side Impact, Auto Revive, Unlimited Coins"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Max:</font></b> Simple + Jump, Power, Score Multiplier"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Ultra:</font></b> Max + No Collision, Free IAP"),
        OBFUSCATE("SmallTextView_<b><font color='#FFBB33'>None:</font></b> Reset all features to OFF"),

        // ========================================================
        //  CATEGORIES
        // ========================================================
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
// ----------------------------------------------------------------
//  Mode Buttons (800/801/802/803) সব feature flags কে সেট করে
//  Toggle-গুলোও (100..110) individual ভাবে কাজ করে
// ================================================================
void Changes(JNIEnv*, jclass, jobject,
             jint featNum, jstring, jint value,
             jlong, jboolean boolean, jstring) {

    switch (featNum) {

        // --------------------------------------------------------
        //  Individual toggles
        // --------------------------------------------------------
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

        // --------------------------------------------------------
        //  800 — Simple Mode
        //     Basic safety + coin boost
        // --------------------------------------------------------
        case 800: {
            if (boolean) {
                // ---- ON: Basic features ----
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_UnlimitedCoin.store(true);

                // ---- OFF: Advanced features ----
                g_JumpLimit.store(false);
                g_JumpHeight.store(false);
                g_PowerDuration.store(false);
                g_LaneChangeDur.store(false);
                g_ScoreMultiplier.store(false);
                g_DetectCollisions.store(false);
                g_IsIAP.store(false);
            } else {
                // ---- OFF → reset basic too ----
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_UnlimitedCoin.store(false);
            }
            LOGD(OBFUSCATE("MODE: Simple = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  801 — Max Mode
        //     Simple + gameplay boost (no risky features)
        // --------------------------------------------------------
        case 801: {
            if (boolean) {
                // ---- ON: All main gameplay ----
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_JumpLimit.store(true);
                g_JumpHeight.store(true);
                g_AutoRevive.store(true);
                g_PowerDuration.store(true);
                g_LaneChangeDur.store(true);
                g_ScoreMultiplier.store(true);
                g_UnlimitedCoin.store(true);

                // ---- OFF: Risky features ----
                g_DetectCollisions.store(false);
                g_IsIAP.store(false);
            } else {
                // ---- OFF: Advanced gameplay off (keep basic) ----
                g_JumpLimit.store(false);
                g_JumpHeight.store(false);
                g_PowerDuration.store(false);
                g_LaneChangeDur.store(false);
                g_ScoreMultiplier.store(false);
            }
            LOGD(OBFUSCATE("MODE: Max = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  802 — Ultra Max Mode
        //     Everything ON
        // --------------------------------------------------------
        case 802: {
            if (boolean) {
                // ---- ON: ALL features ----
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_JumpLimit.store(true);
                g_JumpHeight.store(true);
                g_AutoRevive.store(true);
                g_PowerDuration.store(true);
                g_LaneChangeDur.store(true);
                g_ScoreMultiplier.store(true);
                g_DetectCollisions.store(true);
                g_IsIAP.store(true);
                g_UnlimitedCoin.store(true);
            } else {
                // ---- OFF: ALL features ----
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_JumpLimit.store(false);
                g_JumpHeight.store(false);
                g_AutoRevive.store(false);
                g_PowerDuration.store(false);
                g_LaneChangeDur.store(false);
                g_ScoreMultiplier.store(false);
                g_DetectCollisions.store(false);
                g_IsIAP.store(false);
                g_UnlimitedCoin.store(false);
            }
            LOGD(OBFUSCATE("MODE: UltraMax = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  803 — None (Reset all)
        // --------------------------------------------------------
        case 803: {
            // ---- ALL features OFF ----
            g_FrontalImpact.store(false);
            g_SideImpact.store(false);
            g_JumpLimit.store(false);
            g_JumpHeight.store(false);
            g_AutoRevive.store(false);
            g_PowerDuration.store(false);
            g_LaneChangeDur.store(false);
            g_ScoreMultiplier.store(false);
            g_DetectCollisions.store(false);
            g_IsIAP.store(false);
            g_UnlimitedCoin.store(false);
            LOGD(OBFUSCATE("MODE: None = %d"), (int)boolean);
        } break;

        default:
            break;
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