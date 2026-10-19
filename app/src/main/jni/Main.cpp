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

struct Vector3 {
    float x, y, z;
};

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
        "/sdcard/Android/data/com.kiloo.subwaysurf/files/ModXLab_Log.txt",
        "/storage/emulated/0/Android/data/com.kiloo.subwaysurf/files/ModXLab_Log.txt",
        "/sdcard/Documents/ModXLab_Log.txt"
    };
    for (auto path : candidates) {
        FILE* f = fopen(path, "w");
        if (f) {
            g_LogFile = f;
            strncpy(g_LogPath, path, sizeof(g_LogPath) - 1);
            g_LogReady.store(true);
            fprintf(g_LogFile, "=== ModXLab Log Started ===\n");
            fflush(g_LogFile);
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
    fprintf(g_LogFile, "\n"); 
    fflush(g_LogFile);
    pthread_mutex_unlock(&g_LogMutex);
}
#define LOGI(...) LOGX("I", __VA_ARGS__)
#define LOGE(...) LOGX("E", __VA_ARGS__)

static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//   FEATURE STATE
// ================================================================
static std::atomic<bool> g_FrontalImpact      {false};
static std::atomic<bool> g_SideImpact         {false};
static std::atomic<int>  g_JumpLimit          {0};
static std::atomic<int>  g_JumpHeight         {0};
static std::atomic<bool> g_NoGravity          {false};
static std::atomic<int>  g_GameSpeed          {10}; 
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
static std::atomic<int>  g_FOV                {0}; // Default 0 so it doesn't interfere
static std::atomic<int>  g_TeleportDistance   {0};
static std::atomic<long long> g_NoCollisionStartMs {0};

// ================================================================
//                     ORIGINAL FUNCTION POINTERS
// ================================================================
static float (*old_CalculateGravity)(...)           = nullptr;
static void  (*old_set_Gravity)(...)                = nullptr;
static void  (*old_SetT)(...)                       = nullptr;
static void  (*old_SetGravityMultiplier)(...)       = nullptr;
static void  (*old_set_SpeedMultiplier)(...)        = nullptr;

static void  (*old_AdjustDuration)(...)             = nullptr;
static void  (*old_ResetDuration)(...)              = nullptr;
static float (*old_Power_GetTimeLeft)(...)          = nullptr;
static float (*old_Power_GetDuration)(...)          = nullptr;

static void  (*old_CharacterMotor_Teleport)(...)    = nullptr;
static void  (*old_WorldReposition)(...)            = nullptr;

static void  (*old_set_DisableBoundaryImpact)(...)  = nullptr;
static void  (*old_MovingTrain_Update)(...)         = nullptr;
static void  (*old_Displace)(...)                   = nullptr;
static void  (*old_ChaserGraphics_SetVisible)(...)  = nullptr;
static void  (*old_HandleOnCharacterKilled)(...)    = nullptr;
static void  (*old_HandleOnCharacterStumble)(...)   = nullptr;

static void  (*old_CharacterStumble_Kill)(...)      = nullptr;
static void  (*old_CharacterStumble_Revive)(...)    = nullptr;
static void  (*old_OnActivated)(...)                = nullptr;
static void* (*old_CreateActiveBonus)(...)          = nullptr;
static bool  (*old_TryCreateExtraActiveBonus)(...)  = nullptr;
static void  (*old_SetActiveBonus)(...)             = nullptr;
static void  (*old_ActiveReviveBonus_ctor)(...)     = nullptr;

static void  (*old_ForceSetState)(...)              = nullptr;
static void  (*old_SetOverrideState)(...)           = nullptr;
static void  (*old_SpeedUpOrSkipSequence)(...)      = nullptr;
static void  (*old_SwitchState)(...)                = nullptr;
static void  (*old_set_FieldOfView)(...)            = nullptr;

static void  (*old_ApplyRenderSettings)(...)        = nullptr;

static int   (*old_get_AvailableKeysForUse)(...)    = nullptr;
static int   (*old_ConsumeKeysAndGetRemaining)(...) = nullptr;
static void  (*old_AddCoins)(...)                   = nullptr;
static void  (*old_AddKeys)(...)                    = nullptr;
static void  (*old_AddSeasonTokens)(...)            = nullptr;
static void  (*old_AddSeasonPoints)(...)            = nullptr;
static void  (*old_AddCityTourTokens)(...)          = nullptr;
static void  (*old_AddInRunCollectibleTokens)(...)  = nullptr;
static void  (*old_AddBonusCoins)(...)              = nullptr;
static void  (*old_AddPoints)(...)                  = nullptr;
static int   (*old_ConsumeCoinsAndGetRemaining)(...) = nullptr;
static int   (*old_GetCurrency)(...)                = nullptr;

static bool  (*old_TryShowInterstitial)(...)        = nullptr;
static bool  (*old_TryShowPlacement)(...)           = nullptr;

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
static bool new_CheckFrontalImpact(void* self, void* impactState) {
    if (!self) return false;
    if (g_FrontalImpact.load()) return false;
    if (!old_CheckFrontalImpact) return false;
    typedef bool (*OrigFunc)(void*, void*);
    return ((OrigFunc)old_CheckFrontalImpact)(self, impactState);
}

static bool new_CheckSideImpact(void* self, void* impactState) {
    if (!self) return false;
    if (g_SideImpact.load()) return false;
    if (!old_CheckSideImpact) return false;
    typedef bool (*OrigFunc)(void*, void*);
    return ((OrigFunc)old_CheckSideImpact)(self, impactState);
}

static float new_CalculateGravity(void* self) {
    if (g_NoGravity.load()) return 0.0f;
    return old_CalculateGravity ? old_CalculateGravity(self) : 0.0f;
}
static void new_set_Gravity(void* self, float value) {
    if (g_NoGravity.load()) value = 0.0f;
    if (old_set_Gravity) old_set_Gravity(self, value);
}
static void new_SetT(void* self, float t) {
    if (old_SetT) old_SetT(self, t);
}
static void new_SetGravityMultiplier(void* self, float value) {
    if (old_SetGravityMultiplier) old_SetGravityMultiplier(self, value);
}
static void new_set_SpeedMultiplier(void* self, float value) {
    if (g_PlayerSpeed.load() != 10) value = (float)g_PlayerSpeed.load() / 10.0f;
    if (old_set_SpeedMultiplier) old_set_SpeedMultiplier(self, value);
}

static void new_AdjustDuration(void* self, float duration) {
    if (g_PowerDuration.load() > 0) duration = 999.0f;
    if (old_AdjustDuration) old_AdjustDuration(self, duration);
}
static void new_ResetDuration(void* self) {
    if (g_PowerDuration.load() > 0) return;
    if (old_ResetDuration) old_ResetDuration(self);
}
static float new_Power_GetTimeLeft(void* self) {
    if (g_PowerDuration.load() > 0) return 999.0f;
    return old_Power_GetTimeLeft ? old_Power_GetTimeLeft(self) : 0.0f;
}
static float new_Power_GetDuration(void* self) {
    if (g_PowerDuration.load() > 0) return 999.0f;
    return old_Power_GetDuration ? old_Power_GetDuration(self) : 0.0f;
}

static void new_CharacterMotor_Teleport(void* self, Vector3 rootPosition) {
    if (g_TeleportDistance.load() > 0 && self) {
        rootPosition.z += (float)g_TeleportDistance.load();
        g_TeleportDistance.store(0); 
    }
    if (old_CharacterMotor_Teleport) old_CharacterMotor_Teleport(self, rootPosition);
}
static void new_WorldReposition(void* self, float amount) {
    if (old_WorldReposition) old_WorldReposition(self, amount);
}

static void new_set_DisableBoundaryImpact(void* self, bool value) {
    if (g_NoCollision.load()) value = true;
    if (old_set_DisableBoundaryImpact) old_set_DisableBoundaryImpact(self, value); 
}
static void new_MovingTrain_Update(void* self) {
    if (g_StopTrain.load()) return; 
    if (old_MovingTrain_Update) old_MovingTrain_Update(self);
}
static void new_Displace(void* self) {
    if (g_StopTrain.load()) return;
    if (old_Displace) old_Displace(self);
}
static void new_ChaserGraphics_SetVisible(void* self, bool visible) {
    if (g_NoGuardChaser.load()) visible = false;
    if (old_ChaserGraphics_SetVisible) old_ChaserGraphics_SetVisible(self, visible);
}
static void new_HandleOnCharacterKilled(void* self, int cause) {
    if (g_NoGuardChaser.load()) return;
    if (old_HandleOnCharacterKilled) old_HandleOnCharacterKilled(self, cause);
}
static void new_HandleOnCharacterStumble(void* self, int cause) {
    if (g_NoGuardChaser.load()) return;
    if (old_HandleOnCharacterStumble) old_HandleOnCharacterStumble(self, cause);
}

static void new_CharacterStumble_Kill(void* self, void* data) {
    if (g_AutoRevive.load() && old_CharacterStumble_Revive) {
        old_CharacterStumble_Revive(self); 
        return;
    }
    if (old_CharacterStumble_Kill) {
        typedef void (*OrigFunc)(void*, void*);
        ((OrigFunc)old_CharacterStumble_Kill)(self, data);
    }
}
static void new_CharacterStumble_Revive(void* self) {
    if (old_CharacterStumble_Revive) old_CharacterStumble_Revive(self);
}
static void new_OnActivated(void* self) {
    if (old_OnActivated) old_OnActivated(self);
}
static void* new_CreateActiveBonus(void* self) {
    return old_CreateActiveBonus ? old_CreateActiveBonus(self) : nullptr;
}
static bool new_TryCreateExtraActiveBonus(void* self, void* bonus) {
    return old_TryCreateExtraActiveBonus ? old_TryCreateExtraActiveBonus(self, bonus) : false;
}
static void new_SetActiveBonus(void* self, void* bonus) {
    if (old_SetActiveBonus) old_SetActiveBonus(self, bonus);
}
static void new_ActiveReviveBonus_ctor(void* self, void* data, void* reward) {
    if (old_ActiveReviveBonus_ctor) old_ActiveReviveBonus_ctor(self, data, reward);
}

static void new_ForceSetState(void* self, int state) {
    if (old_ForceSetState) old_ForceSetState(self, state);
}
static void new_SetOverrideState(void* self, int state, void* creator) {
    if (old_SetOverrideState) old_SetOverrideState(self, state, creator);
}
static void new_SpeedUpOrSkipSequence(void* self) {
    if (old_SpeedUpOrSkipSequence) old_SpeedUpOrSkipSequence(self);
}
static void new_SwitchState(void* self, int state) {
    if (old_SwitchState) old_SwitchState(self, state);
}
static void new_set_FieldOfView(void* self, float value) {
    if (g_FOV.load() > 0) value = (float)g_FOV.load();
    if (old_set_FieldOfView) old_set_FieldOfView(self, value);
}

static void new_ApplyRenderSettings(void* self) {
    if (old_ApplyRenderSettings) old_ApplyRenderSettings(self);
}

static int new_get_AvailableKeysForUse(void* self) {
    if (g_AutoRevive.load()) return 999;
    return old_get_AvailableKeysForUse ? old_get_AvailableKeysForUse(self) : 0;
}
static int new_ConsumeKeysAndGetRemaining(void* self, int amount) {
    if (g_AutoRevive.load()) return 999;
    return old_ConsumeKeysAndGetRemaining ? old_ConsumeKeysAndGetRemaining(self, amount) : 0;
}
static void new_AddCoins(void* self, int coins) {
    if (g_UnlimitedCoins.load()) coins = 2000000000;
    if (old_AddCoins) old_AddCoins(self, coins);
}
static void new_AddKeys(void* self, int keys) {
    if (g_UnlimitedCoins.load()) keys = 9999;
    if (old_AddKeys) old_AddKeys(self, keys);
}
static void new_AddSeasonTokens(void* self, int amount) {
    if (old_AddSeasonTokens) old_AddSeasonTokens(self, amount);
}
static void new_AddSeasonPoints(void* self, int amount) {
    if (old_AddSeasonPoints) old_AddSeasonPoints(self, amount);
}
static void new_AddCityTourTokens(void* self, int amount) {
    if (old_AddCityTourTokens) old_AddCityTourTokens(self, amount);
}
static void new_AddInRunCollectibleTokens(void* self, int amount) {
    if (old_AddInRunCollectibleTokens) old_AddInRunCollectibleTokens(self, amount);
}
static void new_AddBonusCoins(void* self, int coins) {
    if (old_AddBonusCoins) old_AddBonusCoins(self, coins);
}
static void new_AddPoints(void* self, double points) {
    if (old_AddPoints) old_AddPoints(self, points);
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

static bool new_TryShowInterstitial(void* self, int location) {
    if (g_NoAds.load()) return false;
    return old_TryShowInterstitial ? old_TryShowInterstitial(self, location) : false;
}
static bool new_TryShowPlacement(void* self, void* placement) {
    if (g_NoAds.load()) return false;
    return old_TryShowPlacement ? old_TryShowPlacement(self, placement) : false;
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
    LOGI("Waiting for libil2cpp.so...");
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    LOGI("[1/14] Hooking CharacterMotor...");
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oTele = CM->GetMethodOffsetByName(OBFUSCATE("Teleport"), 1);
            DWORD oWorld = CM->GetMethodOffsetByName(OBFUSCATE("WorldReposition"), 1);
            DWORD oBound = CM->GetMethodOffsetByName(OBFUSCATE("set_DisableBoundaryImpact"), 1);
            DWORD oFrontal = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oSide    = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            
            if (oTele) HOOK_AU((void*)oTele, (void*)new_CharacterMotor_Teleport, old_CharacterMotor_Teleport);
            if (oWorld) HOOK_AU((void*)oWorld, (void*)new_WorldReposition, old_WorldReposition);
            if (oBound) HOOK_AU((void*)oBound, (void*)new_set_DisableBoundaryImpact, old_set_DisableBoundaryImpact);
            if (oFrontal) HOOK_AU((void*)oFrontal, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            if (oSide) HOOK_AU((void*)oSide, (void*)new_CheckSideImpact, old_CheckSideImpact);
            
            LOGI("  ✅ CharacterMotor hooks installed");
        } else LOGE("  ❌ CharacterMotor NOT FOUND");
    }

    LOGI("[2/14] Hooking CharacterMotorAbilities...");
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oGrav = CMA->GetMethodOffsetByName(OBFUSCATE("CalculateGravity"), 0);
            DWORD oSetGrav = CMA->GetMethodOffsetByName(OBFUSCATE("set_Gravity"), 1);
            DWORD oSpeed = CMA->GetMethodOffsetByName(OBFUSCATE("set_SpeedMultiplier"), 1);
            DWORD oJL = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"), 0);
            DWORD oJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"), 0);
            DWORD oLC = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"), 0);
            if (oGrav) HOOK_AU((void*)oGrav, (void*)new_CalculateGravity, old_CalculateGravity);
            if (oSetGrav) HOOK_AU((void*)oSetGrav, (void*)new_set_Gravity, old_set_Gravity);
            if (oSpeed) HOOK_AU((void*)oSpeed, (void*)new_set_SpeedMultiplier, old_set_SpeedMultiplier);
            if (oJL) HOOK_AU((void*)oJL, (void*)new_get_JumpLimit, old_get_JumpLimit);
            if (oJH) HOOK_AU((void*)oJH, (void*)new_get_JumpHeight, old_get_JumpHeight);
            if (oLC) HOOK_AU((void*)oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
            LOGI("  ✅ Abilities hooks installed");
        } else LOGE("  ❌ CharacterMotorAbilities NOT FOUND");
    }

    LOGI("[3/14] Hooking MovementAbilityInstance...");
    {
        auto MAI = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("MovementAbilityInstance"));
        if (MAI && MAI->thisclass) {
            DWORD oT = MAI->GetMethodOffsetByName(OBFUSCATE("SetT"), 1);
            DWORD oGravM = MAI->GetMethodOffsetByName(OBFUSCATE("SetGravityMultiplier"), 1);
            if (oT) HOOK_AU((void*)oT, (void*)new_SetT, old_SetT);
            if (oGravM) HOOK_AU((void*)oGravM, (void*)new_SetGravityMultiplier, old_SetGravityMultiplier);
            LOGI("  ✅ MovementAbilityInstance hooks installed");
        } else LOGE("  ❌ MovementAbilityInstance NOT FOUND");
    }

    LOGI("[4/14] Hooking Power...");
    {
        auto P = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("Power"));
        if (P && P->thisclass) {
            DWORD oAdj = P->GetMethodOffsetByName(OBFUSCATE("AdjustDuration"), 1);
            DWORD oRes = P->GetMethodOffsetByName(OBFUSCATE("ResetDuration"), 0);
            DWORD oTimeLeft = P->GetMethodOffsetByName(OBFUSCATE("GetTimeLeft"), 0);
            DWORD oDur = P->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 0);
            if (oAdj) HOOK_AU((void*)oAdj, (void*)new_AdjustDuration, old_AdjustDuration);
            if (oRes) HOOK_AU((void*)oRes, (void*)new_ResetDuration, old_ResetDuration);
            if (oTimeLeft) HOOK_AU((void*)oTimeLeft, (void*)new_Power_GetTimeLeft, old_Power_GetTimeLeft);
            if (oDur) HOOK_AU((void*)oDur, (void*)new_Power_GetDuration, old_Power_GetDuration);
            LOGI("  ✅ Power hooks installed");
        } else LOGE("  ❌ Power NOT FOUND");
    }

    LOGI("[5/14] Hooking MovingTrain...");
    {
        auto MT = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (MT && MT->thisclass) {
            DWORD oUpdate = MT->GetMethodOffsetByName(OBFUSCATE("Update"), 0);
            DWORD oDisp = MT->GetMethodOffsetByName(OBFUSCATE("Displace"), 0);
            if (oUpdate) HOOK_AU((void*)oUpdate, (void*)new_MovingTrain_Update, old_MovingTrain_Update);
            if (oDisp) HOOK_AU((void*)oDisp, (void*)new_Displace, old_Displace);
            LOGI("  ✅ MovingTrain hooks installed");
        } else LOGE("  ❌ MovingTrain NOT FOUND");
    }

    LOGI("[6/14] Hooking Chaser...");
    {
        auto CG = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ChaserGraphics"));
        if (CG && CG->thisclass) {
            DWORD oVisible = CG->GetMethodOffsetByName(OBFUSCATE("SetVisible"), 1);
            if (oVisible) HOOK_AU((void*)oVisible, (void*)new_ChaserGraphics_SetVisible, old_ChaserGraphics_SetVisible);
        } else LOGE("  ❌ ChaserGraphics NOT FOUND");

        auto CB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ChaserBehaviour"));
        if (CB && CB->thisclass) {
            DWORD oKill = CB->GetMethodOffsetByName(OBFUSCATE("HandleOnCharacterKilled"), 1);
            DWORD oStumble = CB->GetMethodOffsetByName(OBFUSCATE("HandleOnCharacterStumble"), 1);
            if (oKill) HOOK_AU((void*)oKill, (void*)new_HandleOnCharacterKilled, old_HandleOnCharacterKilled);
            if (oStumble) HOOK_AU((void*)oStumble, (void*)new_HandleOnCharacterStumble, old_HandleOnCharacterStumble);
            LOGI("  ✅ Chaser hooks installed");
        } else LOGE("  ❌ ChaserBehaviour NOT FOUND");
    }

    LOGI("[7/14] Hooking Revive System...");
    {
        auto CS = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterStumble"));
        if (CS && CS->thisclass) {
            DWORD oKill = CS->GetMethodOffsetByName(OBFUSCATE("Kill"), 1); 
            DWORD oRevive = CS->GetMethodOffsetByName(OBFUSCATE("Revive"), 0);
            if (oKill) HOOK_AU((void*)oKill, (void*)new_CharacterStumble_Kill, old_CharacterStumble_Kill);
            if (oRevive) HOOK_AU((void*)oRevive, (void*)new_CharacterStumble_Revive, old_CharacterStumble_Revive);
            LOGI("  ✅ CharacterStumble hooks installed");
        } else LOGE("  ❌ CharacterStumble NOT FOUND");

        auto ARP = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("AutoRevivePowerEffect"));
        if (ARP && ARP->thisclass) {
            DWORD oAct = ARP->GetMethodOffsetByName(OBFUSCATE("OnActivated"), 0);
            if (oAct) HOOK_AU((void*)oAct, (void*)new_OnActivated, old_OnActivated);
            LOGI("  ✅ AutoRevivePowerEffect hook installed");
        } else LOGE("  ❌ AutoRevivePowerEffect NOT FOUND");

        auto RS = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ReviveSystem"));
        if (RS && RS->thisclass) {
            DWORD oCreate = RS->GetMethodOffsetByName(OBFUSCATE("CreateActiveBonus"), 0);
            DWORD oTry = RS->GetMethodOffsetByName(OBFUSCATE("TryCreateExtraActiveBonus"), 1);
            DWORD oSet = RS->GetMethodOffsetByName(OBFUSCATE("SetActiveBonus"), 1);
            if (oCreate) HOOK_AU((void*)oCreate, (void*)new_CreateActiveBonus, old_CreateActiveBonus);
            if (oTry) HOOK_AU((void*)oTry, (void*)new_TryCreateExtraActiveBonus, old_TryCreateExtraActiveBonus);
            if (oSet) HOOK_AU((void*)oSet, (void*)new_SetActiveBonus, old_SetActiveBonus);
            LOGI("  ✅ ReviveSystem hooks installed");
        } else LOGE("  ❌ ReviveSystem NOT FOUND");

        auto ARB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ActiveReviveBonus"));
        if (ARB && ARB->thisclass) {
            DWORD oCtor1 = ARB->GetMethodOffsetByName(OBFUSCATE(".ctor"), 2);
            if (oCtor1) HOOK_AU((void*)oCtor1, (void*)new_ActiveReviveBonus_ctor, old_ActiveReviveBonus_ctor);
            LOGI("  ✅ ActiveReviveBonus hook installed");
        } else LOGE("  ❌ ActiveReviveBonus NOT FOUND");
    }

    LOGI("[8/14] Hooking Camera...");
    {
        auto CCtrl = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraController"));
        if (CCtrl && CCtrl->thisclass) {
            DWORD oForce = CCtrl->GetMethodOffsetByName(OBFUSCATE("ForceSetState"), 1);
            DWORD oOverride = CCtrl->GetMethodOffsetByName(OBFUSCATE("SetOverrideState"), 2);
            DWORD oSpeedUp = CCtrl->GetMethodOffsetByName(OBFUSCATE("SpeedUpOrSkipSequence"), 0);
            if (oForce) HOOK_AU((void*)oForce, (void*)new_ForceSetState, old_ForceSetState);
            if (oOverride) HOOK_AU((void*)oOverride, (void*)new_SetOverrideState, old_SetOverrideState);
            if (oSpeedUp) HOOK_AU((void*)oSpeedUp, (void*)new_SpeedUpOrSkipSequence, old_SpeedUpOrSkipSequence);
        } else LOGE("  ❌ CameraController NOT FOUND");

        auto CCtx = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraContext"));
        if (CCtx && CCtx->thisclass) {
            DWORD oSwitch = CCtx->GetMethodOffsetByName(OBFUSCATE("SwitchState"), 1);
            DWORD oFov = CCtx->GetMethodOffsetByName(OBFUSCATE("set_FieldOfView"), 1);
            if (oSwitch) HOOK_AU((void*)oSwitch, (void*)new_SwitchState, old_SwitchState);
            if (oFov) HOOK_AU((void*)oFov, (void*)new_set_FieldOfView, old_set_FieldOfView);
            LOGI("  ✅ Camera hooks installed");
        } else LOGE("  ❌ CameraContext NOT FOUND");
    }

    LOGI("[9/14] Hooking Fog...");
    {
        auto TC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ThemeConfig"));
        if (TC && TC->thisclass) {
            DWORD oApply = TC->GetMethodOffsetByName(OBFUSCATE("ApplyRenderSettings"), 0);
            if (oApply) HOOK_AU((void*)oApply, (void*)new_ApplyRenderSettings, old_ApplyRenderSettings);
            LOGI("  ✅ ThemeConfig hook installed");
        } else LOGE("  ❌ ThemeConfig NOT FOUND");
    }

    LOGI("[10/14] Hooking Currency...");
    {
        auto RSD = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (RSD && RSD->thisclass) {
            DWORD oKeys = RSD->GetMethodOffsetByName(OBFUSCATE("get_AvailableKeysForUse"), 0);
            DWORD oConsKeys = RSD->GetMethodOffsetByName(OBFUSCATE("ConsumeKeysAndGetRemaining"), 1);
            DWORD oConsCoins = RSD->GetMethodOffsetByName(OBFUSCATE("ConsumeCoinsAndGetRemaining"), 1);
            DWORD oAddCoins = RSD->GetMethodOffsetByName(OBFUSCATE("AddCoins"), 1);
            DWORD oAddKeys = RSD->GetMethodOffsetByName(OBFUSCATE("AddKeys"), 1);
            DWORD oAddST = RSD->GetMethodOffsetByName(OBFUSCATE("AddSeasonTokens"), 1);
            DWORD oAddSP = RSD->GetMethodOffsetByName(OBFUSCATE("AddSeasonPoints"), 1);
            DWORD oAddCT = RSD->GetMethodOffsetByName(OBFUSCATE("AddCityTourTokens"), 1);
            DWORD oAddIRT = RSD->GetMethodOffsetByName(OBFUSCATE("AddInRunCollectibleTokens"), 1);
            DWORD oAddBC = RSD->GetMethodOffsetByName(OBFUSCATE("AddBonusCoins"), 1);
            DWORD oAddPts = RSD->GetMethodOffsetByName(OBFUSCATE("AddPoints"), 1);
            
            if (oKeys) HOOK_AU((void*)oKeys, (void*)new_get_AvailableKeysForUse, old_get_AvailableKeysForUse);
            if (oConsKeys) HOOK_AU((void*)oConsKeys, (void*)new_ConsumeKeysAndGetRemaining, old_ConsumeKeysAndGetRemaining);
            if (oConsCoins) HOOK_AU((void*)oConsCoins, (void*)new_ConsumeCoinsAndGetRemaining, old_ConsumeCoinsAndGetRemaining);
            if (oAddCoins) HOOK_AU((void*)oAddCoins, (void*)new_AddCoins, old_AddCoins);
            if (oAddKeys) HOOK_AU((void*)oAddKeys, (void*)new_AddKeys, old_AddKeys);
            if (oAddST) HOOK_AU((void*)oAddST, (void*)new_AddSeasonTokens, old_AddSeasonTokens);
            if (oAddSP) HOOK_AU((void*)oAddSP, (void*)new_AddSeasonPoints, old_AddSeasonPoints);
            if (oAddCT) HOOK_AU((void*)oAddCT, (void*)new_AddCityTourTokens, old_AddCityTourTokens);
            if (oAddIRT) HOOK_AU((void*)oAddIRT, (void*)new_AddInRunCollectibleTokens, old_AddInRunCollectibleTokens);
            if (oAddBC) HOOK_AU((void*)oAddBC, (void*)new_AddBonusCoins, old_AddBonusCoins);
            if (oAddPts) HOOK_AU((void*)oAddPts, (void*)new_AddPoints, old_AddPoints);
            LOGI("  ✅ RunSessionData hooks installed");
        } else LOGE("  ❌ RunSessionData NOT FOUND");

        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oC = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oC) HOOK_AU((void*)oC, (void*)new_GetCurrency, old_GetCurrency);
            LOGI("  ✅ WalletModel hook installed");
        } else LOGE("  ❌ WalletModel NOT FOUND");
    }

    LOGI("[11/14] Hooking Ads...");
    {
        auto IH = new LoadClass(OBFUSCATE("SYBO.Ads"), OBFUSCATE("InterstitialsHelper"));
        if (IH && IH->thisclass) {
            DWORD oInter = IH->GetMethodOffsetByName(OBFUSCATE("TryShowInterstitial"), 1);
            DWORD oPlace = IH->GetMethodOffsetByName(OBFUSCATE("TryShowPlacement"), 1);
            if (oInter) HOOK_AU((void*)oInter, (void*)new_TryShowInterstitial, old_TryShowInterstitial);
            if (oPlace) HOOK_AU((void*)oPlace, (void*)new_TryShowPlacement, old_TryShowPlacement);
            LOGI("  ✅ InterstitialsHelper hooks installed");
        } else LOGE("  ❌ InterstitialsHelper NOT FOUND");
    }

    LOGI("[12/14] Hooking Gameplay Core...");
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oR = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oR) HOOK_AU((void*)oR, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
            LOGI("  ✅ StumbleBehaviour hook installed");
        } else LOGE("  ❌ StumbleBehaviour NOT FOUND");

        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oB = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oB) HOOK_AU((void*)oB, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
            LOGI("  ✅ ScoreMultiplierManager hook installed");
        } else LOGE("  ❌ ScoreMultiplierManager NOT FOUND");

        auto UC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (UC && UC->thisclass) {
            DWORD oD = UC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oD) HOOK_AU((void*)oD, (void*)new_get_detectCollisions, old_get_detectCollisions);
            LOGI("  ✅ CharacterController hook installed");
        } else LOGE("  ❌ CharacterController NOT FOUND");
    }

    LOGI("[13/14] Hooking Chams...");
    RenderHook();
    
    LOGI("ALL HOOKS INSTALLED SUCCESSFULLY");
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

        OBFUSCATE("Category_Player & Movement"),
        OBFUSCATE("200_Toggle_No Gravity"),
        OBFUSCATE("201_SeekBar_Game Speed_1_20"),
        OBFUSCATE("202_SeekBar_Player Speed_1_20"),

        OBFUSCATE("Category_Powers"),
        OBFUSCATE("203_Toggle_Unlimited Power Time"),

        OBFUSCATE("Category_Teleport"),
        OBFUSCATE("205_SeekBar_Teleport Distance_0_5000"),

        OBFUSCATE("Category_Protection"),
        OBFUSCATE("207_Toggle_Stop Moving Train"),
        OBFUSCATE("208_Toggle_No Guard Chaser"),

        OBFUSCATE("Category_Revive"),
        OBFUSCATE("209_Toggle_Auto Revive"),

        OBFUSCATE("Category_Camera"),
        OBFUSCATE("211_SeekBar_Custom Camera FOV_0_120"),

        OBFUSCATE("Category_Currency"),
        OBFUSCATE("212_Toggle_Unlimited Coins"),

        OBFUSCATE("Category_Ads"),
        OBFUSCATE("214_Toggle_Completely No Ads"),

        OBFUSCATE("Category_Gameplay"),
        OBFUSCATE("100_Toggle_No Head-On Collision"),
        OBFUSCATE("101_Toggle_No Side Collision"),
        OBFUSCATE("102_SeekBar_Jump Limit_0_999"),
        OBFUSCATE("103_SeekBar_Jump Height_0_200"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),
        OBFUSCATE("108_Toggle_No Collision Detect"),

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
        case 800: {
            if (boolean) {
                g_FrontalImpact.store(true); g_SideImpact.store(true);
                g_AutoRevive.store(true); g_UnlimitedCoins.store(true);
                g_JumpLimit.store(0); g_JumpHeight.store(0);
                g_PowerDuration.store(0); g_FastLaneChange.store(false);
                g_ScoreMultiplier.store(0); g_NoCollision.store(false);
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
                g_JumpLimit.store(999); g_JumpHeight.store(200);
                g_PowerDuration.store(999); g_ScoreMultiplier.store(999);
                g_StopTrain.store(true); g_NoGuardChaser.store(true);
                g_NoAds.store(true);
            } else {
                g_FrontalImpact.store(false); g_SideImpact.store(false);
                g_AutoRevive.store(false); g_FastLaneChange.store(false);
                g_UnlimitedCoins.store(false); g_NoCollision.store(false);
                g_JumpLimit.store(0); g_JumpHeight.store(0);
                g_PowerDuration.store(0); g_ScoreMultiplier.store(0);
                g_StopTrain.store(false); g_NoGuardChaser.store(false);
                g_NoAds.store(false);
            }
        } break;
        case 803: {
            g_FrontalImpact.store(false); g_SideImpact.store(false);
            g_AutoRevive.store(false); g_FastLaneChange.store(false);
            g_UnlimitedCoins.store(false); g_NoCollision.store(false);
            g_JumpLimit.store(0); g_JumpHeight.store(0);
            g_PowerDuration.store(0); g_ScoreMultiplier.store(0);
            g_StopTrain.store(false); g_NoGuardChaser.store(false);
            g_NoAds.store(false); g_NoGravity.store(false);
            g_GameSpeed.store(10); g_PlayerSpeed.store(10);
            g_FOV.store(0); g_TeleportDistance.store(0);
        } break;

        case 100: g_FrontalImpact.store(boolean); break;
        case 101: g_SideImpact.store(boolean); break;
        case 102: g_JumpLimit.store(value); break;
        case 103: g_JumpHeight.store(value); break;
        case 106: g_FastLaneChange.store(boolean); break;
        case 107: g_ScoreMultiplier.store(value); break;
        case 108: {
            g_NoCollision.store(boolean);
            if (boolean) g_NoCollisionStartMs.store(nowMs());
            else         g_NoCollisionStartMs.store(0);
        } break;
        case 200: g_NoGravity.store(boolean); break;
        case 201: g_GameSpeed.store(value); break;
        case 202: g_PlayerSpeed.store(value); break;
        case 203: g_PowerDuration.store(boolean ? 999 : 0); break;
        case 205: g_TeleportDistance.store(value); break;
        case 207: g_StopTrain.store(boolean); break;
        case 208: g_NoGuardChaser.store(boolean); break;
        case 209: g_AutoRevive.store(boolean); break;
        case 211: g_FOV.store(value); break;
        case 212: g_UnlimitedCoins.store(boolean); break;
        case 214: g_NoAds.store(boolean); break;

        case 119: g_ChamsEnabled = boolean; break;
        case 120: chamsint = value; break;
        case 125: setColorPreset(value); break;
        case 126: setShaderPreset(value); break;
        case 127: /* disabled distance check */ break;
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
    openLogFile();
    LOGI("ModXLab Library Constructed.");
    
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}