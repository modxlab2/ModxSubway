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
//   FEATURE STATE (EXISTING)
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
//   FEATURE STATE (NEW – from Unlimited Shop.txt)
// ================================================================
static std::atomic<bool> g_UnlimitedShop        {false};  // get_IsIAP → false
static std::atomic<bool> g_NoGravity            {false};  // ApplyGravity no-op
static std::atomic<bool> g_FreezeCharacter      {false};  // UpdateMovement no-op
static std::atomic<int>  g_CharacterSpeed       {0};      // get_MinSpeed/MaxSpeed
static std::atomic<int>  g_AirJumpHeight        {0};      // get_AirJumpHeight
static std::atomic<int>  g_CameraMode           {0};      // 0=Normal,1=Follow,2=Stop
static std::atomic<bool> g_NoBoundaryImpact     {false};  // NotifySideImpact no-op
static std::atomic<bool> g_NoStumble            {false};  // OnStumble no-op
static std::atomic<bool> g_HideJetpack          {false};  // ShowFlight no-op
static std::atomic<bool> g_HidePogostick        {false};  // ShowPogostick no-op
static std::atomic<bool> g_HideMagnet           {false};  // ShowMagnet no-op
static std::atomic<bool> g_HideHoverboard       {false};  // ShowHoverboard no-op
static std::atomic<bool> g_StopTrains           {false};  // Displace no-op
static std::atomic<int>  g_AddCoinsBonus        {0};      // Addcoins +bonus
static std::atomic<int>  g_AddKeysBonus         {0};      // AddKeys +bonus
static std::atomic<int>  g_AddScoreBonus        {0};      // AddPoints +bonus
static std::atomic<bool> g_UnlimitedPowerTime   {false};  // UpdatePowers no-op
static std::atomic<bool> g_UnlimitedJetpackTime {false};  // FinishFlight no-op
static std::atomic<bool> g_ReviveReward         {false};  // OnRevivedWithCurrency x10
static std::atomic<bool> g_FreezeCoins          {false};  // HandleOnCoinsPickedUp no-op
static std::atomic<bool> g_FreezeScore          {false};  // HandleOnPointsEarned no-op

// ================================================================
//   ORIGINAL FUNCTION POINTERS (EXISTING)
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

// ================================================================
//   ORIGINAL FUNCTION POINTERS (NEW – type-safe)
// ================================================================
static bool   (*old_get_IsIAP)(void*)                          = nullptr;
static void   (*old_ApplyGravity)(void*)                       = nullptr;
static float  (*old_get_MinSpeed)(void*)                       = nullptr;
static float  (*old_get_MaxSpeed)(void*)                       = nullptr;
static void   (*old_UpdateMovement)(void*)                     = nullptr;
static float  (*old_get_AirJumpHeight)(void*)                  = nullptr;
static bool   (*old_CameraApply)(void*, void*)                 = nullptr;
static void   (*old_NotifySideImpact)(void*, int, int, void*)  = nullptr;
static void   (*old_OnStumble)(void*, int)                     = nullptr;
static void   (*old_ShowFlight)(void*, bool, int)              = nullptr;
static void   (*old_ShowPogostick)(void*, bool)                = nullptr;
static void   (*old_ShowMagnet)(void*, bool)                   = nullptr;
static void   (*old_ShowHoverboard)(void*, bool)               = nullptr;
static void   (*old_Displace)(void*)                           = nullptr;
static void   (*old_Addcoins)(void*, int)                      = nullptr;
static void   (*old_AddKeys)(void*, int)                       = nullptr;
static void   (*old_AddPoints)(void*, double)                  = nullptr;
static void   (*old_UpdatePowers)(void*)                       = nullptr;
static void   (*old_MainUpdatePowers)(void*)                   = nullptr;
static void   (*old_FinishFlight)(void*)                       = nullptr;
static void   (*old_OnRevivedWithCurrency)(void*)              = nullptr;
static void   (*old_MockRevivedWithCurrency)(void*)            = nullptr;
static void   (*old_HandleCoinsPickedUp)(void*)                = nullptr;
static void   (*old_HandlePointsEarned)(void*)                 = nullptr;

// ================================================================
//   HOOK IMPLEMENTATIONS (EXISTING)
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
    return (v > 0) ? v : (old_get_JumpLimit ? old_get_JumpLimit(self) : 0);
}

static float new_get_JumpHeight(void* self) {
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    return (v > 0) ? (float)v : (old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f);
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
    return (v > 0) ? (float)v : (old_GetDuration ? old_GetDuration(self, cfg) : 0.0f);
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

static int new_GetCurrency(void* self, int currencyType) {
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//   HOOK IMPLEMENTATIONS (NEW)
// ================================================================

// --- Currency.get_IsIAP → false (Unlimited Shop) ---
static bool new_get_IsIAP(void* self) {
    if (g_UnlimitedShop.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}

// --- CharacterMotor.ApplyGravity → no-op (No Gravity) ---
static void new_ApplyGravity(void* self) {
    if (g_NoGravity.load()) return;
    if (old_ApplyGravity) old_ApplyGravity(self);
}

// --- CharacterMotorAbilities.get_MinSpeed / get_MaxSpeed ---
static float new_get_MinSpeed(void* self) {
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MinSpeed ? old_get_MinSpeed(self) : 0.0f;
}

static float new_get_MaxSpeed(void* self) {
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MaxSpeed ? old_get_MaxSpeed(self) : 0.0f;
}

// --- CharacterMotor.UpdateMovement → no-op (Freeze Character) ---
static void new_UpdateMovement(void* self) {
    if (g_FreezeCharacter.load()) return;
    if (old_UpdateMovement) old_UpdateMovement(self);
}

// --- CharacterMotorAbilities.get_AirJumpHeight ---
static float new_get_AirJumpHeight(void* self) {
    int v = g_AirJumpHeight.load();
    if (v > 0) return (float)v;
    return old_get_AirJumpHeight ? old_get_AirJumpHeight(self) : 0.0f;
}

// --- CameraGroundedModifier.Apply ---
// mode 0 = Normal, 1 = Follow (return true), 2 = Stop (return false)
static bool new_CameraApply(void* self, void* context) {
    int mode = g_CameraMode.load();
    if (mode == 1) return true;
    if (mode == 2) return false;
    return old_CameraApply ? old_CameraApply(self, context) : false;
}

// --- CharacterMotor.NotifySideImpact → no-op (No Boundary Impact) ---
static void new_NotifySideImpact(void* self, int horizType, int entityType, void* entity) {
    if (g_NoBoundaryImpact.load()) return;
    if (old_NotifySideImpact) old_NotifySideImpact(self, horizType, entityType, entity);
}

// --- RunProgressOrderIndicator.OnStumble → no-op (No Character Stumble) ---
static void new_OnStumble(void* self, int deathCause) {
    if (g_NoStumble.load()) return;
    if (old_OnStumble) old_OnStumble(self, deathCause);
}

// --- CharacterPropsController.ShowFlight → no-op (Hide Jetpack) ---
static void new_ShowFlight(void* self, bool show, int flightMode) {
    if (g_HideJetpack.load()) return;
    if (old_ShowFlight) old_ShowFlight(self, show, flightMode);
}

// --- CharacterPropsController.ShowPogostick → no-op (Hide Pogostick) ---
static void new_ShowPogostick(void* self, bool show) {
    if (g_HidePogostick.load()) return;
    if (old_ShowPogostick) old_ShowPogostick(self, show);
}

// --- CharacterPropsController.ShowMagnet → no-op (Hide Magnet) ---
static void new_ShowMagnet(void* self, bool show) {
    if (g_HideMagnet.load()) return;
    if (old_ShowMagnet) old_ShowMagnet(self, show);
}

// --- CharacterPropsController.ShowHoverboard → no-op (Hide Hoverboard) ---
static void new_ShowHoverboard(void* self, bool randomBoard) {
    if (g_HideHoverboard.load()) return;
    if (old_ShowHoverboard) old_ShowHoverboard(self, randomBoard);
}

// --- MovingTrain.Displace → no-op (Stop Moving Trains) ---
static void new_Displace(void* self) {
    if (g_StopTrains.load()) return;
    if (old_Displace) old_Displace(self);
}

// --- RunSessionData.Addcoins ---
static void new_Addcoins(void* self, int coins) {
    int bonus = g_AddCoinsBonus.load();
    if (old_Addcoins) {
        if (bonus > 0) old_Addcoins(self, coins + bonus);
        else           old_Addcoins(self, coins);
    }
}

// --- RunSessionData.AddKeys ---
static void new_AddKeys(void* self, int keys) {
    int bonus = g_AddKeysBonus.load();
    if (old_AddKeys) {
        if (bonus > 0) old_AddKeys(self, keys + bonus);
        else           old_AddKeys(self, keys);
    }
}

// --- RunSessionData.AddPoints ---
static void new_AddPoints(void* self, double points) {
    int bonus = g_AddScoreBonus.load();
    if (old_AddPoints) {
        if (bonus > 0) old_AddPoints(self, points + (double)bonus);
        else           old_AddPoints(self, points);
    }
}

// --- CharacterPower.UpdatePowers → no-op (Unlimited Powerup Time) ---
static void new_UpdatePowers(void* self) {
    if (g_UnlimitedPowerTime.load()) return;
    if (old_UpdatePowers) old_UpdatePowers(self);
}

// --- MainPowerSystem.UpdatePowers → no-op (alternative) ---
static void new_MainUpdatePowers(void* self) {
    if (g_UnlimitedPowerTime.load()) return;
    if (old_MainUpdatePowers) old_MainUpdatePowers(self);
}

// --- FlyingState.FinishFlight → no-op (Unlimited Jetpack Time) ---
static void new_FinishFlight(void* self) {
    if (g_UnlimitedJetpackTime.load()) return;
    if (old_FinishFlight) old_FinishFlight(self);
}

// --- MainReviveSystem.OnRevivedWithCurrency (Revive Reward) ---
static void new_OnRevivedWithCurrency(void* self) {
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) {
        if (old_OnRevivedWithCurrency) old_OnRevivedWithCurrency(self);
    }
}

// --- MockReviveSystem.OnRevivedWithCurrency (fallback) ---
static void new_MockRevivedWithCurrency(void* self) {
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) {
        if (old_MockRevivedWithCurrency) old_MockRevivedWithCurrency(self);
    }
}

// --- GameScreenCoinCounter.HandleOnCoinsPickedUp → no-op (Freeze Coins) ---
static void new_HandleCoinsPickedUp(void* self) {
    if (g_FreezeCoins.load()) return;
    if (old_HandleCoinsPickedUp) old_HandleCoinsPickedUp(self);
}

// --- GameScreenScoreCounter.HandleOnPointsEarned → no-op (Freeze Score) ---
static void new_HandlePointsEarned(void* self) {
    if (g_FreezeScore.load()) return;
    if (old_HandlePointsEarned) old_HandlePointsEarned(self);
}

// ================================================================
//   HACK THREAD (ALL HOOKS INSTALLED HERE)
// ================================================================
void* hack_thread(void*) {
    openLogFile();
    LOGI("Starting hooks...");
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    // ---------- EXISTING HOOKS ----------
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oF = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oS = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oF) HOOK_AU((void*)oF, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oS) HOOK_AU((void*)oS, (void*)new_CheckSideImpact,    old_CheckSideImpact);
        }
    }
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJL = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLC = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oJL) HOOK_AU((void*)oJL, (void*)new_get_JumpLimit, old_get_JumpLimit);
            if (oJH) HOOK_AU((void*)oJH, (void*)new_get_JumpHeight, old_get_JumpHeight);
            if (oLC) HOOK_AU((void*)oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
        }
    }
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oR = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oR) HOOK_AU((void*)oR, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
        }
    }
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oD = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oD) HOOK_AU((void*)oD, (void*)new_GetDuration, old_GetDuration);
        }
    }
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oB = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oB) HOOK_AU((void*)oB, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
        }
    }
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oD = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oD) HOOK_AU((void*)oD, (void*)new_get_detectCollisions, old_get_detectCollisions);
        }
    }
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oC = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oC) HOOK_AU((void*)oC, (void*)new_GetCurrency, old_GetCurrency);
        }
    }

    // ---------- NEW HOOKS (Unlimited Shop.txt) ----------

    // 1) Unlimited Shop — Currency.get_IsIAP
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_get_IsIAP, old_get_IsIAP);
        }
    }

    // 2) CharacterMotor — ApplyGravity, UpdateMovement, NotifySideImpact
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oG = CM->GetMethodOffsetByName(OBFUSCATE("ApplyGravity"),      0);
            DWORD oU = CM->GetMethodOffsetByName(OBFUSCATE("UpdateMovement"),    0);
            DWORD oN = CM->GetMethodOffsetByName(OBFUSCATE("NotifySideImpact"),  3);
            if (oG) HOOK_AU((void*)oG, (void*)new_ApplyGravity,      old_ApplyGravity);
            if (oU) HOOK_AU((void*)oU, (void*)new_UpdateMovement,    old_UpdateMovement);
            if (oN) HOOK_AU((void*)oN, (void*)new_NotifySideImpact,  old_NotifySideImpact);
        }
    }

    // 3) CharacterMotorAbilities — MinSpeed, MaxSpeed, AirJumpHeight
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oMin = CMA->GetMethodOffsetByName(OBFUSCATE("get_MinSpeed"),       0);
            DWORD oMax = CMA->GetMethodOffsetByName(OBFUSCATE("get_MaxSpeed"),       0);
            DWORD oAJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"),  0);
            if (oMin) HOOK_AU((void*)oMin, (void*)new_get_MinSpeed,       old_get_MinSpeed);
            if (oMax) HOOK_AU((void*)oMax, (void*)new_get_MaxSpeed,       old_get_MaxSpeed);
            if (oAJH) HOOK_AU((void*)oAJH, (void*)new_get_AirJumpHeight,  old_get_AirJumpHeight);
        }
    }

    // 4) CameraGroundedModifier.Apply
    {
        auto CGM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraGroundedModifier"));
        if (CGM && CGM->thisclass) {
            DWORD o = CGM->GetMethodOffsetByName(OBFUSCATE("Apply"), 1);
            if (o) HOOK_AU((void*)o, (void*)new_CameraApply, old_CameraApply);
        }
    }

    // 5) RunProgressOrderIndicator.OnStumble
    {
        auto RPOI = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunProgressOrderIndicator"));
        if (RPOI && RPOI->thisclass) {
            DWORD o = RPOI->GetMethodOffsetByName(OBFUSCATE("OnStumble"), 1);
            if (o) HOOK_AU((void*)o, (void*)new_OnStumble, old_OnStumble);
        }
    }

    // 6) CharacterPropsController — ShowFlight, ShowPogostick, ShowMagnet, ShowHoverboard
    {
        auto CPC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPropsController"));
        if (CPC && CPC->thisclass) {
            DWORD oF = CPC->GetMethodOffsetByName(OBFUSCATE("ShowFlight"),     2);
            DWORD oP = CPC->GetMethodOffsetByName(OBFUSCATE("ShowPogostick"),  1);
            DWORD oM = CPC->GetMethodOffsetByName(OBFUSCATE("ShowMagnet"),     1);
            DWORD oH = CPC->GetMethodOffsetByName(OBFUSCATE("ShowHoverboard"), 1);
            if (oF) HOOK_AU((void*)oF, (void*)new_ShowFlight,     old_ShowFlight);
            if (oP) HOOK_AU((void*)oP, (void*)new_ShowPogostick,  old_ShowPogostick);
            if (oM) HOOK_AU((void*)oM, (void*)new_ShowMagnet,     old_ShowMagnet);
            if (oH) HOOK_AU((void*)oH, (void*)new_ShowHoverboard, old_ShowHoverboard);
        }
    }

    // 7) MovingTrain.Displace
    {
        auto MT = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (MT && MT->thisclass) {
            DWORD o = MT->GetMethodOffsetByName(OBFUSCATE("Displace"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_Displace, old_Displace);
        }
    }

    // 8) RunSessionData — Addcoins, AddKeys, AddPoints
    {
        auto RSD = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (RSD && RSD->thisclass) {
            DWORD oC = RSD->GetMethodOffsetByName(OBFUSCATE("Addcoins"),  1);
            DWORD oK = RSD->GetMethodOffsetByName(OBFUSCATE("AddKeys"),   1);
            DWORD oP = RSD->GetMethodOffsetByName(OBFUSCATE("AddPoints"), 1);
            if (oC) HOOK_AU((void*)oC, (void*)new_Addcoins,  old_Addcoins);
            if (oK) HOOK_AU((void*)oK, (void*)new_AddKeys,   old_AddKeys);
            if (oP) HOOK_AU((void*)oP, (void*)new_AddPoints, old_AddPoints);
        }
    }

    // 9) CharacterPower.UpdatePowers
    {
        auto CP = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPower"));
        if (CP && CP->thisclass) {
            DWORD o = CP->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_UpdatePowers, old_UpdatePowers);
        }
    }

    // 10) MainPowerSystem.UpdatePowers (alternative)
    {
        auto MPS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainPowerSystem"));
        if (MPS && MPS->thisclass) {
            DWORD o = MPS->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_MainUpdatePowers, old_MainUpdatePowers);
        }
    }

    // 11) FlyingState.FinishFlight
    {
        auto FS = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("FlyingState"));
        if (FS && FS->thisclass) {
            DWORD o = FS->GetMethodOffsetByName(OBFUSCATE("FinishFlight"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_FinishFlight, old_FinishFlight);
        }
    }

    // 12) MainReviveSystem.OnRevivedWithCurrency
    {
        auto MRS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainReviveSystem"));
        if (MRS && MRS->thisclass) {
            DWORD o = MRS->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_OnRevivedWithCurrency, old_OnRevivedWithCurrency);
        }
    }

    // 13) MockReviveSystem.OnRevivedWithCurrency (fallback)
    {
        auto MRS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems.Mocks"), OBFUSCATE("MockReviveSystem"));
        if (MRS && MRS->thisclass) {
            DWORD o = MRS->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_MockRevivedWithCurrency, old_MockRevivedWithCurrency);
        }
    }

    // 14) GameScreenCoinCounter.HandleOnCoinsPickedUp
    {
        auto GSCC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenCoinCounter"));
        if (GSCC && GSCC->thisclass) {
            DWORD o = GSCC->GetMethodOffsetByName(OBFUSCATE("HandleOnCoinsPickedUp"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_HandleCoinsPickedUp, old_HandleCoinsPickedUp);
        }
    }

    // 15) GameScreenScoreCounter.HandleOnPointsEarned
    {
        auto GSSC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenScoreCounter"));
        if (GSSC && GSSC->thisclass) {
            DWORD o = GSSC->GetMethodOffsetByName(OBFUSCATE("HandleOnPointsEarned"), 0);
            if (o) HOOK_AU((void*)o, (void*)new_HandlePointsEarned, old_HandlePointsEarned);
        }
    }

    RenderHook();
    LOGI("All hooks installed. Ready.");
    return nullptr;
}

// ================================================================
//   JNI — FEATURE LIST
// ================================================================
jobjectArray GetFeatureList(JNIEnv* env, jobject) {
    jobjectArray ret;
    const char* features[] = {

        OBFUSCATE("800_ButtonOnOff_Simple Mode"),
        OBFUSCATE("801_ButtonOnOff_Max Mode"),
        OBFUSCATE("802_ButtonOnOff_Ultra Max Mode"),
        OBFUSCATE("803_ButtonOnOff_None (Reset)"),

        // ===== RUNNER =====
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

        // ===== POWERS =====
        OBFUSCATE("Category_Powers"),
        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),
        OBFUSCATE("135_Toggle_Hide Jetpack"),
        OBFUSCATE("136_Toggle_Hide Pogostick"),
        OBFUSCATE("137_Toggle_Hide Magnet"),
        OBFUSCATE("138_Toggle_Hide Hoverboard"),
        OBFUSCATE("140_Toggle_Unlimited Powerup Time"),
        OBFUSCATE("141_Toggle_Unlimited Jetpack Time"),

        // ===== SCORE =====
        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),
        OBFUSCATE("143_Toggle_Freeze Coins"),
        OBFUSCATE("144_Toggle_Freeze Score"),

        // ===== COLLISIONS =====
        OBFUSCATE("Category_Collisions"),
        OBFUSCATE("108_Toggle_No Collision Detect"),

        // ===== SHOP =====
        OBFUSCATE("Category_Shop"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),
        OBFUSCATE("150_Toggle_Unlimited Shop"),
        OBFUSCATE("151_SeekBar_Add Coins Bonus_0_9999"),
        OBFUSCATE("152_SeekBar_Add Keys Bonus_0_9999"),
        OBFUSCATE("153_SeekBar_Add Score Bonus_0_9999"),
        OBFUSCATE("154_Toggle_Revive Reward"),

        // ===== CHARACTER =====
        OBFUSCATE("Category_Character"),
        OBFUSCATE("160_SeekBar_Character Speed_0_200"),

        // ===== CAMERA =====
        OBFUSCATE("Category_Camera"),
        OBFUSCATE("161_Spinner_Camera Mode_Normal,Follow,Stop"),

        // ===== WORLD =====
        OBFUSCATE("Category_World"),
        OBFUSCATE("142_Toggle_Stop Moving Trains"),

        // ===== CHAMS =====
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
//   JNI — CHANGES HANDLER
// ================================================================
void Changes(JNIEnv*, jclass, jobject,
             jint featNum, jstring, jint value,
             jlong, jboolean boolean, jstring) {

    switch (featNum) {

        // ---------- EXISTING ----------
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

        // ---------- NEW (Unlimited Shop.txt) ----------
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

        // ---------- CHAMS (EXISTING) ----------
        case 119: g_ChamsEnabled = boolean; break;
        case 120: chamsint = value;         break;
        case 125: setColorPreset(value);    break;
        case 126: setShaderPreset(value);   break;
        case 127: g_MaxDistance = (float)value; break;
        case 121: enableRainbow = boolean;     break;
        case 122: enableRainbowWall = boolean; break;

        // ---------- PRESETS ----------
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
            // Also reset new features
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