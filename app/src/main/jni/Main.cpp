// ================================================================
// ModX Lab — Subway Surfers (Unity / IL2CPP)
// Auto-Update Hook System  (32bit + 64bit)
// ----------------------------------------------------------------
// ★ Null-guarded hooks — crash-proof
// ★ Collision FIX: detectCollisions hook বাদ দেওয়া হয়েছে
// ★ নতুন: Ad Block, Speed Multiplier, Infinite Jump, No Stumble
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
//   FEATURE STATE
// ================================================================
static std::atomic<bool> g_FrontalImpact    {false};   // 100
static std::atomic<bool> g_SideImpact       {false};   // 101
static std::atomic<int>  g_JumpLimit        {0};       // 102 (0-999)
static std::atomic<int>  g_JumpHeight       {0};       // 103 (0-200)
static std::atomic<bool> g_AutoRevive       {false};   // 104
static std::atomic<int>  g_PowerDuration    {0};       // 105 (0-999)
static std::atomic<bool> g_FastLaneChange   {false};   // 106
static std::atomic<int>  g_ScoreMultiplier  {0};       // 107 (0-999)
static std::atomic<bool> g_NoCollision      {false};   // 108 (FIXED)
static std::atomic<bool> g_FreeShopping     {false};   // 109
static std::atomic<bool> g_UnlimitedCoins   {false};   // 110

// ---- NEW FEATURES ----
static std::atomic<int>  g_SpeedMultiplier  {0};       // 111 (0=off, N=Nx speed)
static std::atomic<bool> g_BlockAds         {false};   // 112
static std::atomic<bool> g_InfiniteJump     {false};   // 113
static std::atomic<bool> g_NoStumble        {false};   // 114
static std::atomic<bool> g_MagnetAlwaysOn   {false};   // 115
static std::atomic<bool> g_MaxPowerLevel    {false};   // 116
static std::atomic<int>  g_AirJumpHeight    {0};       // 117 (0-200)

// ================================================================
//                     ORIGINAL FUNCTION POINTERS
// ================================================================
// ---- CharacterMotor / CharacterMotorAbilities ----
static bool  (*old_CheckFrontalImpact)(...)     = nullptr;
static bool  (*old_CheckSideImpact)(...)        = nullptr;
static int   (*old_get_JumpLimit)(...)          = nullptr;
static float (*old_get_JumpHeight)(...)         = nullptr;
static float (*old_get_AirJumpHeight)(...)      = nullptr;
static float (*old_get_LaneChangeDuration)(...)= nullptr;
static bool  (*old_get_NoLowerCollision)(...)   = nullptr;
static bool  (*old_get_NoCornerCollision)(...)  = nullptr;
static bool  (*old_get_CanJump)(...)            = nullptr;
static bool  (*old_get_HasMultiJumpsRemaining)(...) = nullptr;

// ---- Power / Score / Shop ----
static bool  (*old_IsAutoReviveEnabled)(...)    = nullptr;
static float (*old_GetDuration)(...)            = nullptr;
static int   (*old_get_BaseMultiplierSum)(...)  = nullptr;
static bool  (*old_get_IsIAP)(...)              = nullptr;
static int   (*old_GetCurrency)(...)            = nullptr;
static void  (*old_ActivateOrAddPower)(...)     = nullptr;
static bool  (*old_IsPowerActive)(...)          = nullptr;

// ---- Speed Config ----
static float (*old_SpeedCfg_get_StartSpeed)(...)   = nullptr;
static float (*old_SpeedCfg_get_EndSpeed)(...)     = nullptr;
static float (*old_Motor_get_StartSpeed)(...)      = nullptr;
static float (*old_Motor_get_EndSpeed)(...)        = nullptr;

// ---- Stumble / Death ----
static void  (*old_Stumble_EntityType)(...)     = nullptr;
static void  (*old_Stumble_StumbleType)(...)    = nullptr;
static void  (*old_Stumble_CollisionData)(...)  = nullptr;
static void  (*old_Kill_CollisionData)(...)     = nullptr;
static void  (*old_Kill_StumbleType)(...)       = nullptr;

// ---- Ads ----
static bool  (*old_IsRewardedCapped)(...)       = nullptr;
static bool  (*old_IsInterstitialCapped)(...)   = nullptr;
static bool  (*old_IsBannerCapped)(...)         = nullptr;

// ================================================================
//                       HOOK IMPLEMENTATIONS
// ================================================================

// --- SYBO.RunnerCore.Character.CharacterMotor ---
static bool new_CheckFrontalImpact(void* self, void* other) {
    if (!self) return false;
    if (g_FrontalImpact.load() || g_NoCollision.load() || g_NoStumble.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    if (!self) return false;
    if (g_SideImpact.load() || g_NoCollision.load() || g_NoStumble.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

// ★ FIXED No-Collision: এই দুটো hook-ই আসল "no collision" feature দেয়
// কিন্তু trigger/coin collection অক্ষত থাকে
static bool new_get_NoLowerCollision(void* self) {
    if (!self) return false;
    if (g_NoCollision.load() || g_NoStumble.load()) return true;
    return old_get_NoLowerCollision ? old_get_NoLowerCollision(self) : false;
}

static bool new_get_NoCornerCollision(void* self) {
    if (!self) return false;
    if (g_NoCollision.load() || g_NoStumble.load()) return true;
    return old_get_NoCornerCollision ? old_get_NoCornerCollision(self) : false;
}

// ---- Infinite Jump ----
static bool new_get_CanJump(void* self) {
    if (!self) return true;
    if (g_InfiniteJump.load()) return true;
    return old_get_CanJump ? old_get_CanJump(self) : true;
}

static bool new_get_HasMultiJumpsRemaining(void* self) {
    if (!self) return true;
    if (g_InfiniteJump.load()) return true;
    return old_get_HasMultiJumpsRemaining ? old_get_HasMultiJumpsRemaining(self) : true;
}

// --- CharacterMotorAbilities ---
static int new_get_JumpLimit(void* self) {
    if (!self) return 0;
    int v = g_JumpLimit.load();
    if (g_InfiniteJump.load()) return 9999;
    if (v > 0) return v;
    return old_get_JumpLimit ? old_get_JumpLimit(self) : 0;
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    if (v > 0) return (float) v;
    return old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f;
}

static float new_get_AirJumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_AirJumpHeight.load();
    if (v > 0) return (float) v;
    // infinite jump-এর সাথে air jump height ও boost করি
    if (g_InfiniteJump.load()) return 100.0f;
    return old_get_AirJumpHeight ? old_get_AirJumpHeight(self) : 0.0f;
}

static float new_get_LaneChangeDuration(void* self) {
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

// --- SYBO.Subway.StumbleBehaviour ---
static bool new_IsAutoReviveEnabled(void* self) {
    if (!self) return false;
    if (g_AutoRevive.load() || g_NoStumble.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

// --- SYBO.RunnerCore.Powers.PowerConfig ---
static float new_GetDuration(void* self, void* cfg) {
    if (!self) return 0.0f;
    int v = g_PowerDuration.load();
    if (v > 0) return (float) v;
    if (g_MaxPowerLevel.load()) return 999.0f;
    return old_GetDuration ? old_GetDuration(self, cfg) : 0.0f;
}

// --- SYBO.Subway.ScoreMultiplierManager ---
static int new_get_BaseMultiplierSum(void* self) {
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    if (v > 0) return v;
    return old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0;
}

// --- Currency / IAP ---
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
//              ★ NEW: Speed Multiplier hooks
// ================================================================
static float new_SpeedCfg_get_StartSpeed(void* self) {
    if (!self) return 0.0f;
    float orig = old_SpeedCfg_get_StartSpeed ? old_SpeedCfg_get_StartSpeed(self) : 0.0f;
    int m = g_SpeedMultiplier.load();
    if (m > 0) return orig * (float) m;
    return orig;
}

static float new_SpeedCfg_get_EndSpeed(void* self) {
    if (!self) return 0.0f;
    float orig = old_SpeedCfg_get_EndSpeed ? old_SpeedCfg_get_EndSpeed(self) : 0.0f;
    int m = g_SpeedMultiplier.load();
    if (m > 0) return orig * (float) m;
    return orig;
}

static float new_Motor_get_StartSpeed(void* self) {
    if (!self) return 0.0f;
    float orig = old_Motor_get_StartSpeed ? old_Motor_get_StartSpeed(self) : 0.0f;
    int m = g_SpeedMultiplier.load();
    if (m > 0) return orig * (float) m;
    return orig;
}

static float new_Motor_get_EndSpeed(void* self) {
    if (!self) return 0.0f;
    float orig = old_Motor_get_EndSpeed ? old_Motor_get_EndSpeed(self) : 0.0f;
    int m = g_SpeedMultiplier.load();
    if (m > 0) return orig * (float) m;
    return orig;
}

// ================================================================
//              ★ NEW: Stumble/Death blockers
// ================================================================
static void new_Stumble_EntityType(void* self, int group, int entityType) {
    if (!self) return;
    if (g_NoStumble.load() || g_NoCollision.load()) return;   // stumble এড়িয়ে যাই
    if (old_Stumble_EntityType) old_Stumble_EntityType(self, group, entityType);
}

static void new_Stumble_StumbleType(void* self, int group, int stumbleType) {
    if (!self) return;
    if (g_NoStumble.load() || g_NoCollision.load()) return;
    if (old_Stumble_StumbleType) old_Stumble_StumbleType(self, group, stumbleType);
}

static void new_Stumble_CollisionData(void* self, int group, void* eventData) {
    if (!self) return;
    if (g_NoStumble.load() || g_NoCollision.load()) return;
    if (old_Stumble_CollisionData) old_Stumble_CollisionData(self, group, eventData);
}

static void new_Kill_CollisionData(void* self, void* eventData) {
    if (!self) return;
    if (g_NoStumble.load()) return;
    if (old_Kill_CollisionData) old_Kill_CollisionData(self, eventData);
}

static void new_Kill_StumbleType(void* self, int stumbleType) {
    if (!self) return;
    if (g_NoStumble.load()) return;
    if (old_Kill_StumbleType) old_Kill_StumbleType(self, stumbleType);
}

// ================================================================
//              ★ NEW: Magnet Always On (power active check)
// ================================================================
// PowerType enum: Magnet = 102 (from dump)
static bool new_IsPowerActive(void* self, int powerType) {
    if (!self) return false;
    if (g_MagnetAlwaysOn.load() && powerType == 102) return true;  // Magnet
    return old_IsPowerActive ? old_IsPowerActive(self, powerType) : false;
}

// ================================================================
//              ★ NEW: Ad Block hooks
// ================================================================
static bool new_IsRewardedAdCapped(void* self, void* placementName) {
    if (g_BlockAds.load()) return true;   // capped = ad দেখাবে না
    return old_IsRewardedCapped ? old_IsRewardedCapped(self, placementName) : false;
}

static bool new_IsInterstitialCapped(void* self, void* placementName) {
    if (g_BlockAds.load()) return true;
    return old_IsInterstitialCapped ? old_IsInterstitialCapped(self, placementName) : false;
}

static bool new_IsBannerCapped(void* self, void* placementName) {
    if (g_BlockAds.load()) return true;
    return old_IsBannerCapped ? old_IsBannerCapped(self, placementName) : false;
}

// ================================================================
//                       HACK THREAD
// ================================================================
void* hack_thread(void*) {
    do {
        sleep(1);
    } while (!isLibraryLoaded(LIB));

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
            DWORD oCanJump = CM->GetMethodOffsetByName(OBFUSCATE("get_CanJump"),        0);
            DWORD oMultiJ  = CM->GetMethodOffsetByName(OBFUSCATE("get_HasMultiJumpsRemaining"), 0);
            DWORD oStartS  = CM->GetMethodOffsetByName(OBFUSCATE("get_StartSpeed"),     0);
            DWORD oEndS    = CM->GetMethodOffsetByName(OBFUSCATE("get_EndSpeed"),       0);

            if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oSide)    HOOK_AU((void*)oSide,    (void*)new_CheckSideImpact,    old_CheckSideImpact);
            if (oCanJump) HOOK_AU((void*)oCanJump, (void*)new_get_CanJump,        old_get_CanJump);
            if (oMultiJ)  HOOK_AU((void*)oMultiJ,  (void*)new_get_HasMultiJumpsRemaining, old_get_HasMultiJumpsRemaining);
            if (oStartS)  HOOK_AU((void*)oStartS,  (void*)new_Motor_get_StartSpeed, old_Motor_get_StartSpeed);
            if (oEndS)    HOOK_AU((void*)oEndS,    (void*)new_Motor_get_EndSpeed,   old_Motor_get_EndSpeed);

            LOGD(OBFUSCATE("CharacterMotor hooks: frontal=%p side=%p canjump=%p"),
                 (void*)oFrontal, (void*)oSide, (void*)oCanJump);
        } else {
            LOGD(OBFUSCATE("CharacterMotor class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterMotorAbilities
    //  ★ FIX: detectCollisions এর বদলে NoLowerCollision / NoCornerCollision
    // ------------------------------------------------------------
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                 OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJumpLim  = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"),          0);
            DWORD oJumpH    = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"),         0);
            DWORD oAirJumpH = CMA->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"),      0);
            DWORD oLane     = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            DWORD oNoLower  = CMA->GetMethodOffsetByName(OBFUSCATE("get_NoLowerCollision"),   0);
            DWORD oNoCorner = CMA->GetMethodOffsetByName(OBFUSCATE("get_NoCornerCollision"),  0);

            if (oJumpLim)  HOOK_AU((void*)oJumpLim,  (void*)new_get_JumpLimit,          old_get_JumpLimit);
            if (oJumpH)    HOOK_AU((void*)oJumpH,    (void*)new_get_JumpHeight,         old_get_JumpHeight);
            if (oAirJumpH) HOOK_AU((void*)oAirJumpH, (void*)new_get_AirJumpHeight,      old_get_AirJumpHeight);
            if (oLane)     HOOK_AU((void*)oLane,     (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
            if (oNoLower)  HOOK_AU((void*)oNoLower,  (void*)new_get_NoLowerCollision,   old_get_NoLowerCollision);
            if (oNoCorner) HOOK_AU((void*)oNoCorner, (void*)new_get_NoCornerCollision,  old_get_NoCornerCollision);

            LOGD(OBFUSCATE("Abilities hooks: jl=%p jh=%p ajh=%p lc=%p nlc=%p ncc=%p"),
                 (void*)oJumpLim, (void*)oJumpH, (void*)oAirJumpH, (void*)oLane,
                 (void*)oNoLower, (void*)oNoCorner);
        } else {
            LOGD(OBFUSCATE("CharacterMotorAbilities class not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.RunnerCore.Character.CharacterSpeedConfigInstance
    //  ★ NEW: Character Speed Multiplier
    // ------------------------------------------------------------
    {
        auto SC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"),
                                OBFUSCATE("CharacterSpeedConfigInstance"));
        if (SC && SC->thisclass) {
            DWORD oStart = SC->GetMethodOffsetByName(OBFUSCATE("get_StartSpeed"), 0);
            DWORD oEnd   = SC->GetMethodOffsetByName(OBFUSCATE("get_EndSpeed"),   0);
            if (oStart) HOOK_AU((void*)oStart, (void*)new_SpeedCfg_get_StartSpeed, old_SpeedCfg_get_StartSpeed);
            if (oEnd)   HOOK_AU((void*)oEnd,   (void*)new_SpeedCfg_get_EndSpeed,   old_SpeedCfg_get_EndSpeed);
            LOGD(OBFUSCATE("SpeedConfig hooks: start=%p end=%p"), (void*)oStart, (void*)oEnd);
        } else {
            LOGD(OBFUSCATE("CharacterSpeedConfigInstance not found — skipped"));
        }
    }

    // ------------------------------------------------------------
    //  SYBO.Subway.StumbleBehaviour  (Auto Revive + Stumble blocker)
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
    //  SYBO.Subway.CharacterStumble  (Stumble/Kill blocker)
    // ------------------------------------------------------------
    {
        auto CS = new LoadClass(OBFUSCATE("SYBO.Subway"),
                                OBFUSCATE("CharacterStumble"));
        if (CS && CS->thisclass) {
            // NOTE: overloaded methods — param count দিয়ে try করছি
            DWORD oStumbleE = CS->GetMethodOffsetByName(OBFUSCATE("Stumble"), 2);
            DWORD oKillC    = CS->GetMethodOffsetByName(OBFUSCATE("Kill"),    1);
            if (oStumbleE) HOOK_AU((void*)oStumbleE, (void*)new_Stumble_CollisionData, old_Stumble_CollisionData);
            if (oKillC)    HOOK_AU((void*)oKillC,    (void*)new_Kill_CollisionData,    old_Kill_CollisionData);
            LOGD(OBFUSCATE("CharacterStumble hooks: stumble=%p kill=%p"),
                 (void*)oStumbleE, (void*)oKillC);
        } else {
            LOGD(OBFUSCATE("CharacterStumble class not found — skipped"));
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
    //  SYBO.Subway.CharacterPower  (Magnet + Power activate)
    // ------------------------------------------------------------
    {
        auto CP = new LoadClass(OBFUSCATE("SYBO.Subway"),
                                OBFUSCATE("CharacterPower"));
        if (CP && CP->thisclass) {
            DWORD oIsActive = CP->GetMethodOffsetByName(OBFUSCATE("IsPowerActive"), 1);
            if (oIsActive) HOOK_AU((void*)oIsActive, (void*)new_IsPowerActive, old_IsPowerActive);
            LOGD(OBFUSCATE("CharacterPower hook: %p"), (void*)oIsActive);
        } else {
            LOGD(OBFUSCATE("CharacterPower class not found — skipped"));
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

    // ------------------------------------------------------------
    //  ★ NEW: Ad Block hooks
    //  SubwayBackfillAdManagerProvider : AdManagerProvider<T>
    // ------------------------------------------------------------
    {
        const char* adClasses[] = {
            "AdManagerProvider",
            "AdManager",
            "AdsManager",
            "AdController"
        };
        const char* adNamespaces[] = {
            "SYBO.Ads",
            "SYBO.Subway",
            "SYBO.Ads.Mediation"
        };

        for (const char* ns : adNamespaces) {
            for (const char* cn : adClasses) {
                auto AC = new LoadClass(OBFUSCATE(ns), OBFUSCATE(cn));
                if (AC && AC->thisclass) {
                    DWORD oRwd  = AC->GetMethodOffsetByName(OBFUSCATE("IsRewardedAdCapped"), 1);
                    DWORD oInt  = AC->GetMethodOffsetByName(OBFUSCATE("IsInterstitialCapped"), 1);
                    DWORD oBan  = AC->GetMethodOffsetByName(OBFUSCATE("IsBannerCapped"), 1);
                    if (oRwd) HOOK_AU((void*)oRwd, (void*)new_IsRewardedAdCapped,     old_IsRewardedCapped);
                    if (oInt) HOOK_AU((void*)oInt, (void*)new_IsInterstitialCapped,   old_IsInterstitialCapped);
                    if (oBan) HOOK_AU((void*)oBan, (void*)new_IsBannerCapped,         old_IsBannerCapped);
                    LOGD(OBFUSCATE("AdManager %s.%s hooks: rwd=%p int=%p ban=%p"),
                         ns, cn, (void*)oRwd, (void*)oInt, (void*)oBan);
                }
            }
        }
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

        // ========================================================
        //  MAIN TAB — Mode Presets
        // ========================================================
        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),

        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Simple:</font></b> Safety + Coins"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Max:</font></b> + Jump, Power, Score"),
        OBFUSCATE("SmallTextView_<b><font color='#3DDB87'>Ultra:</font></b> + NoCollision, Free"),
        OBFUSCATE("SmallTextView_<b><font color='#FFBB33'>None:</font></b> Reset all"),

        // ========================================================
        //  Runner — Movement & Survival
        // ========================================================
        OBFUSCATE("Category_Runner"),

        OBFUSCATE("100_Toggle_No Head-On Collision"),
        OBFUSCATE("101_Toggle_No Side Collision"),
        OBFUSCATE("102_SeekBar_Jump Limit_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height_0_200"),
        OBFUSCATE("104_Toggle_Auto Revive"),
        OBFUSCATE("108_Toggle_No Collision (Fixed)"),
        OBFUSCATE("111_SeekBar_Speed Multiplier_0_20"),
        OBFUSCATE("113_Toggle_Infinite Jump"),
        OBFUSCATE("114_Toggle_God Mode (No Stumble)"),
        OBFUSCATE("117_SeekBar_Air Jump Height_0_200"),

        // ========================================================
        //  Powers — Power-up related
        // ========================================================
        OBFUSCATE("Category_Powers"),

        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),
        OBFUSCATE("115_Toggle_Magnet Always On"),
        OBFUSCATE("116_Toggle_Max Power Level"),

        // ========================================================
        //  Score — Score boosting
        // ========================================================
        OBFUSCATE("Category_Score"),

        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),

        // ========================================================
        //  Shop — Currency / IAP
        // ========================================================
        OBFUSCATE("Category_Shop"),

        OBFUSCATE("109_Toggle_Free Shopping (IAP)"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),

        // ========================================================
        //  Ads — Advertisement
        // ========================================================
        OBFUSCATE("Category_Ads"),

        OBFUSCATE("112_Toggle_Block Ads (All)"),
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

        // -------- Individual features --------
        case 100: g_FrontalImpact.store(boolean);     break;
        case 101: g_SideImpact.store(boolean);        break;
        case 102: g_JumpLimit.store(value);           break;
        case 103: g_JumpHeight.store(value);          break;
        case 104: g_AutoRevive.store(boolean);        break;
        case 105: g_PowerDuration.store(value);       break;
        case 106: g_FastLaneChange.store(boolean);    break;
        case 107: g_ScoreMultiplier.store(value);     break;
        case 108: g_NoCollision.store(boolean);       break;   // ★ FIXED
        case 109: g_FreeShopping.store(boolean);      break;
        case 110: g_UnlimitedCoins.store(boolean);    break;
        // -------- NEW --------
        case 111: g_SpeedMultiplier.store(value);     break;
        case 112: g_BlockAds.store(boolean);          break;
        case 113: g_InfiniteJump.store(boolean);      break;
        case 114: g_NoStumble.store(boolean);         break;
        case 115: g_MagnetAlwaysOn.store(boolean);    break;
        case 116: g_MaxPowerLevel.store(boolean);     break;
        case 117: g_AirJumpHeight.store(value);       break;

        // --------------------------------------------------------
        //  800 — Simple Mode
        // --------------------------------------------------------
        case 800: {
            if (boolean) {
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_UnlimitedCoins.store(true);
                // Advanced OFF
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
                g_NoCollision.store(false);
                g_FreeShopping.store(false);
                g_SpeedMultiplier.store(0);
                g_BlockAds.store(false);
                g_InfiniteJump.store(false);
                g_NoStumble.store(false);
                g_MagnetAlwaysOn.store(false);
                g_MaxPowerLevel.store(false);
                g_AirJumpHeight.store(0);
            } else {
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_UnlimitedCoins.store(false);
            }
            LOGD(OBFUSCATE("MODE: Simple = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  801 — Max Mode
        // --------------------------------------------------------
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
                // Risky OFF
                g_NoCollision.store(false);
                g_FreeShopping.store(false);
                g_SpeedMultiplier.store(0);
                g_BlockAds.store(false);
                g_InfiniteJump.store(false);
                g_NoStumble.store(false);
                g_MagnetAlwaysOn.store(false);
                g_MaxPowerLevel.store(false);
                g_AirJumpHeight.store(0);
            } else {
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_PowerDuration.store(0);
                g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0);
            }
            LOGD(OBFUSCATE("MODE: Max = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  802 — Ultra Max Mode
        // --------------------------------------------------------
        case 802: {
            if (boolean) {
                g_FrontalImpact.store(true);
                g_SideImpact.store(true);
                g_AutoRevive.store(true);
                g_FastLaneChange.store(true);
                g_UnlimitedCoins.store(true);
                g_NoCollision.store(true);
                g_FreeShopping.store(true);
                g_BlockAds.store(true);
                g_InfiniteJump.store(true);
                g_NoStumble.store(true);
                g_MagnetAlwaysOn.store(true);
                g_MaxPowerLevel.store(true);
                g_JumpLimit.store(999);
                g_JumpHeight.store(200);
                g_AirJumpHeight.store(200);
                g_PowerDuration.store(999);
                g_ScoreMultiplier.store(999);
                g_SpeedMultiplier.store(2);
            } else {
                g_FrontalImpact.store(false);
                g_SideImpact.store(false);
                g_AutoRevive.store(false);
                g_FastLaneChange.store(false);
                g_UnlimitedCoins.store(false);
                g_NoCollision.store(false);
                g_FreeShopping.store(false);
                g_BlockAds.store(false);
                g_InfiniteJump.store(false);
                g_NoStumble.store(false);
                g_MagnetAlwaysOn.store(false);
                g_MaxPowerLevel.store(false);
                g_JumpLimit.store(0);
                g_JumpHeight.store(0);
                g_AirJumpHeight.store(0);
                g_PowerDuration.store(0);
                g_ScoreMultiplier.store(0);
                g_SpeedMultiplier.store(0);
            }
            LOGD(OBFUSCATE("MODE: UltraMax = %d"), (int)boolean);
        } break;

        // --------------------------------------------------------
        //  803 — None (Reset)
        // --------------------------------------------------------
        case 803: {
            g_FrontalImpact.store(false);
            g_SideImpact.store(false);
            g_AutoRevive.store(false);
            g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false);
            g_NoCollision.store(false);
            g_FreeShopping.store(false);
            g_BlockAds.store(false);
            g_InfiniteJump.store(false);
            g_NoStumble.store(false);
            g_MagnetAlwaysOn.store(false);
            g_MaxPowerLevel.store(false);
            g_JumpLimit.store(0);
            g_JumpHeight.store(0);
            g_AirJumpHeight.store(0);
            g_PowerDuration.store(0);
            g_ScoreMultiplier.store(0);
            g_SpeedMultiplier.store(0);
            LOGD(OBFUSCATE("MODE: None = %d"), (int)boolean);
        } break;

        default:
            break;
    }
}

// ================================================================
//     STUBS — Menu.java যাতে UnsatisfiedLinkError না দেয়
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