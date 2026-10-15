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

#include "Chams.h"

#define LIB OBFUSCATE("libil2cpp.so")

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

static inline long long now_ms() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

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
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) return false;
    if (g_FrontalImpact.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) return false;
    if (g_SideImpact.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

static int new_get_JumpLimit(void* self) {
    if (!self) return 0;
    int v = g_JumpLimit.load();
    if (v > 0) return v;
    return old_get_JumpLimit ? old_get_JumpLimit(self) : 0;
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    if (v > 0) return (float) v;
    return old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f;
}

static float new_get_LaneChangeDuration(void* self) {
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) return false;
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

static float new_GetDuration(void* self, void* cfg) {
    if (!self) return 0.0f;
    int v = g_PowerDuration.load();
    if (v > 0) return (float) v;
    return old_GetDuration ? old_GetDuration(self, cfg) : 0.0f;
}

static int new_get_BaseMultiplierSum(void* self) {
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    if (v > 0) return v;
    return old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0;
}

static bool new_get_detectCollisions(void* self) {
    if (!self) return true;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0) {
            long long elapsed = now_ms() - start;
            if (elapsed >= 0 && elapsed < 1000) return false;
        }
    }
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}

static bool new_get_IsIAP(void* self) {
    if (!self) return false;
    if (g_FreeShopping.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}

static int new_GetCurrency(void* self, int currencyType) {
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    do { sleep(1); } while (!isLibraryLoaded(LIB));
    sleep(3);
    LOGD(OBFUSCATE("il2cpp loaded — installing hooks"));

    // ---- CharacterMotor ----
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oSide)    HOOK_AU((void*)oSide,    (void*)new_CheckSideImpact,    old_CheckSideImpact);
        }
    }

    // ---- CharacterMotorAbilities ----
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                 OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJumpLim = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJumpH   = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLane    = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oJumpLim) HOOK_AU((void*)oJumpLim, (void*)new_get_JumpLimit,          old_get_JumpLimit);
            if (oJumpH)   HOOK_AU((void*)oJumpH,   (void*)new_get_JumpHeight,         old_get_JumpHeight);
            if (oLane)    HOOK_AU((void*)oLane,    (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
        }
    }

    // ---- StumbleBehaviour ----
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oRevive = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oRevive) HOOK_AU((void*)oRevive, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
        }
    }

    // ---- PowerConfig ----
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oDur = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oDur) HOOK_AU((void*)oDur, (void*)new_GetDuration, old_GetDuration);
        }
    }

    // ---- ScoreMultiplierManager ----
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oBase = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oBase) HOOK_AU((void*)oBase, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
        }
    }

    // ---- CharacterController ----
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oDetect = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oDetect) HOOK_AU((void*)oDetect, (void*)new_get_detectCollisions, old_get_detectCollisions);
        }
    }

    // ---- Currency ----
    {
        auto CUR = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (CUR && CUR->thisclass) {
            DWORD oIAP = CUR->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (oIAP) HOOK_AU((void*)oIAP, (void*)new_get_IsIAP, old_get_IsIAP);
        }
    }

    // ---- WalletModel ----
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oGetCur = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oGetCur) HOOK_AU((void*)oGetCur, (void*)new_GetCurrency, old_GetCurrency);
        }
    }

    // ---- ★ Chams ----
    RenderHook();

    LOGD(OBFUSCATE("All auto-update hooks installed — ready"));
    return nullptr;
}

// ================================================================
//                      JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        // ---------- Mode Presets ----------
        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Simple:</font></b> Basic Safety + Coins"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Max:</font></b> Simple + Jump, Power, Score"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Ultra:</font></b> Max + No Collision, Free Shop"),
        OBFUSCATE("SmallTextView_<b><font color='#FFBB33'>None:</font></b> Reset all features"),

        // ---------- Runner ----------
        OBFUSCATE("Category_Runner"),
        OBFUSCATE("100_Toggle_No Head-On Collision"),
        OBFUSCATE("101_Toggle_No Side Collision"),
        OBFUSCATE("102_SeekBar_Jump Limit_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height_0_200"),
        OBFUSCATE("104_Toggle_Auto Revive"),

        // ---------- Powers ----------
        OBFUSCATE("Category_Powers"),
        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),

        // ---------- Score ----------
        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),

        // ---------- Collisions ----------
        OBFUSCATE("Category_Collisions"),
        OBFUSCATE("108_Toggle_No Collision Detect"),
        OBFUSCATE("SmallTextView_<b><font color='#FF4444'>⚠ Turn ON before game starts</font></b>"),

        // ---------- Shop ----------
        OBFUSCATE("Category_Shop"),
        OBFUSCATE("109_Toggle_Free Shopping (IAP)"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),

        // ============================================================
        //  ★ CHAMS TAB  — fresh, simplified
        // ============================================================
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
        OBFUSCATE("SmallTextView_<b><font color='#AAAAAA'>File: /sdcard/Documents/Shaders.txt</font></b>"),
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
        case 102: g_JumpLimit.store(value);           break;
        case 103: g_JumpHeight.store(value);          break;
        case 104: g_AutoRevive.store(boolean);        break;
        case 105: g_PowerDuration.store(value);       break;
        case 106: g_FastLaneChange.store(boolean);    break;
        case 107: g_ScoreMultiplier.store(value);     break;

        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) {
                g_NoCollisionStartMs.store(now_ms());
                LOGD(OBFUSCATE("No Collision Detect: ON — 1s window"));
            } else {
                g_NoCollisionStartMs.store(0);
                LOGD(OBFUSCATE("No Collision Detect: OFF"));
            }
        } break;

        case 109: g_FreeShopping.store(boolean);      break;
        case 110: g_UnlimitedCoins.store(boolean);    break;

        // ---------- ★ Chams ----------
        case 120: {
            chamsint = value;
            LOGD(OBFUSCATE("Chams mode = %d"), value);
        } break;

        case 121: {
            enableRainbow = boolean;
            LOGD(OBFUSCATE("Rainbow Visible = %d"), (int)boolean);
        } break;

        case 122: {
            enableRainbowWall = boolean;
            LOGD(OBFUSCATE("Rainbow In-Wall = %d"), (int)boolean);
        } break;

        case 124: {
            if (boolean) {
                dumpShadersToFile();
                LOGD(OBFUSCATE("Shader list dumped"));
            }
        } break;

        case 125: {
            setColorPreset(value);
        } break;

        // ---------- Simple Mode ----------
        case 800: {
            if (boolean) {
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
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_UnlimitedCoins.store(false);
            }
        } break;

        // ---------- Max Mode ----------
        case 801: {
            if (boolean) {
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
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
            }
        } break;

        // ---------- Ultra Max ----------
        case 802: {
            if (boolean) {
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true);
                g_NoCollision.store(true);
                g_NoCollisionStartMs.store(now_ms());
                g_FreeShopping.store(true);
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
                g_FreeShopping.store(false);
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_ScoreMultiplier.store(0);
            }
        } break;

        // ---------- None (Reset) ----------
        case 803: {
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
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}