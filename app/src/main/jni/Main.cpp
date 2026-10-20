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

// ============ NEW: for logging & crash handling ============
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unwind.h>
#include <dlfcn.h>

#include "Chams.h"

#define LIB OBFUSCATE("libil2cpp.so")
#define LOG_TAG "MODXLAB"

// ================================================================
//  📁 THREAD-SAFE LOGGER + GAME-FOLDER PATHS
// ================================================================
static FILE*             g_LogFile    = nullptr;
static int               g_CrashFd    = -1;
static pthread_mutex_t   g_LogMutex   = PTHREAD_MUTEX_INITIALIZER;
static std::string       g_LogPath;
static std::string       g_CrashPath;
static std::string       g_LogDir;
static std::string       g_PackageName;
static std::atomic<bool> g_LogReady   {false};

// --- Read package name from /proc/self/cmdline ---
static std::string getPackageName() {
    char buf[256] = {0};
    FILE* f = fopen("/proc/self/cmdline", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        (void)n;
        fclose(f);
    }
    std::string s(buf);
    size_t pos = s.find('\0');
    if (pos != std::string::npos) s = s.substr(0, pos);
    return s;
}

// --- mkdir helper ---
static bool ensureDir(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path.c_str(), 0777) == 0;
}

// --- Find a writable log directory (game folder preferred) ---
static std::string findLogDir() {
    g_PackageName = getPackageName();
    std::vector<std::string> candidates;

    // ★ PRIMARY: the game's own external files folder (no root needed)
    if (!g_PackageName.empty()) {
        candidates.push_back("/sdcard/Android/data/" + g_PackageName + "/files/ModXLab");
        candidates.push_back("/storage/emulated/0/Android/data/" + g_PackageName + "/files/ModXLab");
    }
    // Fallbacks
    candidates.push_back("/sdcard/Documents/ModXLab");
    candidates.push_back("/storage/emulated/0/Documents/ModXLab");
    candidates.push_back("/sdcard/ModXLab");
    candidates.push_back("/data/local/tmp/ModXLab");

    for (const auto& c : candidates) {
        if (!ensureDir(c)) continue;
        // Verify we can actually write
        std::string test = c + "/.write_test";
        FILE* f = fopen(test.c_str(), "w");
        if (f) {
            fclose(f);
            remove(test.c_str());
            return c;
        }
    }
    return "";
}

// --- LOGX (existing, now uses g_LogPath) ---
static void LOGX(const char* level, const char* fmt, ...) {
    // Always mirror to logcat
    va_list ap1; va_start(ap1, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, LOG_TAG, fmt, ap1);
    va_end(ap1);

    if (!g_LogReady.load() || !g_LogFile) return;

    pthread_mutex_lock(&g_LogMutex);
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char tb[16]; strftime(tb, sizeof(tb), "%H:%M:%S", tm_info);

    fprintf(g_LogFile, "[%s][%s][%lx] ", tb, level, (unsigned long)pthread_self());
    va_list ap2; va_start(ap2, fmt);
    vfprintf(g_LogFile, fmt, ap2);
    va_end(ap2);
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);
    pthread_mutex_unlock(&g_LogMutex);
}
#define LOGI(...) LOGX("I", __VA_ARGS__)
#define LOGW(...) LOGX("W", __VA_ARGS__)
#define LOGE(...) LOGX("E", __VA_ARGS__)

// --- Signal-safe write (no malloc, no stdio) ---
static void safeWrite(int fd, const char* s) {
    if (fd >= 0 && s) {
        ssize_t r = write(fd, s, strlen(s));
        (void)r;
    }
}

// --- Initialize logging system ---
static void initLogging() {
    g_LogDir = findLogDir();
    if (g_LogDir.empty()) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG,
                            "No writable log dir found — logcat only");
        return;
    }

    g_LogPath   = g_LogDir + "/modxlab.log";
    g_CrashPath = g_LogDir + "/modxlab_crash.log";

    g_LogFile = fopen(g_LogPath.c_str(), "a");
    g_CrashFd = open(g_CrashPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0666);

    if (g_LogFile) g_LogReady.store(true);

    LOGI("========================================");
    LOGI("ModXLab Session Start");
    LOGI("PID        : %d", getpid());
    LOGI("Package    : %s", g_PackageName.c_str());
    LOGI("Log dir    : %s", g_LogDir.c_str());
    LOGI("Main log   : %s", g_LogPath.c_str());
    LOGI("Crash log  : %s", g_CrashPath.c_str());
    LOGI("========================================");
}

static inline long long nowMs() {
    return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ================================================================
//  🛑 CRASH HANDLER + BACKTRACE
// ================================================================
struct BacktraceState {
    void** current;
    void** end;
};

static _Unwind_Reason_Code unwindCallback(struct _Unwind_Context* ctx, void* arg) {
    BacktraceState* st = (BacktraceState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (st->current == st->end) return _URC_END_OF_STACK;
        *st->current++ = (void*)pc;
    }
    return _URC_NO_REASON;
}

static size_t captureBacktrace(void** buffer, size_t max) {
    BacktraceState st = {buffer, buffer + max};
    _Unwind_Backtrace(unwindCallback, &st);
    return st.current - buffer;
}

static const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (segmentation fault)";
        case SIGABRT: return "SIGABRT (abort)";
        case SIGBUS:  return "SIGBUS (bus error)";
        case SIGILL:  return "SIGILL (illegal instruction)";
        case SIGFPE:  return "SIGFPE (floating point exception)";
        default:      return "UNKNOWN";
    }
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
static std::atomic<bool> g_UnlimitedShop        {false};
static std::atomic<bool> g_NoGravity            {false};
static std::atomic<bool> g_FreezeCharacter      {false};
static std::atomic<int>  g_CharacterSpeed       {0};
static std::atomic<int>  g_AirJumpHeight        {0};
static std::atomic<int>  g_CameraMode           {0};
static std::atomic<bool> g_NoBoundaryImpact     {false};
static std::atomic<bool> g_NoStumble            {false};
static std::atomic<bool> g_HideJetpack          {false};
static std::atomic<bool> g_HidePogostick        {false};
static std::atomic<bool> g_HideMagnet           {false};
static std::atomic<bool> g_HideHoverboard       {false};
static std::atomic<bool> g_StopTrains           {false};
static std::atomic<int>  g_AddCoinsBonus        {0};
static std::atomic<int>  g_AddKeysBonus         {0};
static std::atomic<int>  g_AddScoreBonus        {0};
static std::atomic<bool> g_UnlimitedPowerTime   {false};
static std::atomic<bool> g_UnlimitedJetpackTime {false};
static std::atomic<bool> g_ReviveReward         {false};
static std::atomic<bool> g_FreezeCoins          {false};
static std::atomic<bool> g_FreezeScore          {false};

// --- Dump all feature states to a raw fd (signal-safe) ---
static void dumpFeatureStateToFd(int fd) {
    char buf[2048];
    int len = 0;
    #define FMT(...) len += snprintf(buf + len, sizeof(buf) - len, __VA_ARGS__)

    FMT("\n--- Feature State at Crash ---\n");
    FMT("Collisions: Frontal=%d Side=%d NoDetect=%d Boundary=%d\n",
        (int)g_FrontalImpact.load(), (int)g_SideImpact.load(),
        (int)g_NoCollision.load(),  (int)g_NoBoundaryImpact.load());
    FMT("Movement  : NoGravity=%d FreezeChar=%d Speed=%d AirJump=%d\n",
        (int)g_NoGravity.load(), (int)g_FreezeCharacter.load(),
        g_CharacterSpeed.load(), g_AirJumpHeight.load());
    FMT("Jump      : Limit=%d Height=%d\n",
        g_JumpLimit.load(), g_JumpHeight.load());
    FMT("Powers    : Duration=%d UnlimitedPower=%d UnlimitedJetpack=%d\n",
        g_PowerDuration.load(), (int)g_UnlimitedPowerTime.load(),
        (int)g_UnlimitedJetpackTime.load());
    FMT("Visual    : HideJet=%d HidePogo=%d HideMag=%d HideBoard=%d\n",
        (int)g_HideJetpack.load(), (int)g_HidePogostick.load(),
        (int)g_HideMagnet.load(),  (int)g_HideHoverboard.load());
    FMT("Economy   : UnlimitedCoins=%d UnlimitedShop=%d ReviveReward=%d\n",
        (int)g_UnlimitedCoins.load(), (int)g_UnlimitedShop.load(),
        (int)g_ReviveReward.load());
    FMT("Bonuses   : Coins=%d Keys=%d Score=%d\n",
        g_AddCoinsBonus.load(), g_AddKeysBonus.load(), g_AddScoreBonus.load());
    FMT("Score     : Multiplier=%d FreezeCoins=%d FreezeScore=%d\n",
        g_ScoreMultiplier.load(), (int)g_FreezeCoins.load(),
        (int)g_FreezeScore.load());
    FMT("World     : StopTrains=%d Camera=%d AutoRevive=%d FastLane=%d\n",
        (int)g_StopTrains.load(), g_CameraMode.load(),
        (int)g_AutoRevive.load(), (int)g_FastLaneChange.load());
    FMT("Chams     : Enabled=%d Mode=%d MaxDist=%.1f\n",
        (int)g_ChamsEnabled, chamsint, g_MaxDistance);
    FMT("------------------------------\n");

    safeWrite(fd, buf);
    #undef FMT
}

// --- Crash handler (async-signal-safe as possible) ---
static void crashHandler(int sig, siginfo_t* info, void* context) {
    (void)context;

    safeWrite(g_CrashFd, "\n\n========================================\n");
    safeWrite(g_CrashFd, "CRASH DETECTED\n");
    safeWrite(g_CrashFd, "========================================\n");

    char buf[512];
    int len = 0;
    len += snprintf(buf + len, sizeof(buf) - len,
                    "Signal     : %d (%s)\n", sig, signalName(sig));
    len += snprintf(buf + len, sizeof(buf) - len,
                    "PID / TID  : %d / %d\n", getpid(), (int)pthread_self());
    if (info) {
        len += snprintf(buf + len, sizeof(buf) - len,
                        "Fault addr : %p\n", info->si_addr);
    }
    safeWrite(g_CrashFd, buf);

    // Backtrace
    void* bt[64];
    size_t count = captureBacktrace(bt, 64);
    safeWrite(g_CrashFd, "\n--- Backtrace ---\n");
    for (size_t i = 0; i < count; i++) {
        Dl_info dl;
        len = 0;
        if (dladdr(bt[i], &dl) && dl.dli_fname) {
            uintptr_t offset = (uintptr_t)bt[i] - (uintptr_t)dl.dli_fbase;
            len += snprintf(buf + len, sizeof(buf) - len,
                "  #%02zu  %p  %s  (%s + 0x%lx)\n",
                i, bt[i],
                dl.dli_sname ? dl.dli_sname : "?",
                dl.dli_fname,
                (unsigned long)offset);
        } else {
            len += snprintf(buf + len, sizeof(buf) - len,
                "  #%02zu  %p  <unknown>\n", i, bt[i]);
        }
        safeWrite(g_CrashFd, buf);
    }
    safeWrite(g_CrashFd, "-----------------\n");

    // Dump feature state
    dumpFeatureStateToFd(g_CrashFd);

    if (g_CrashFd >= 0) fsync(g_CrashFd);

    // Reset to default handler and re-raise
    signal(sig, SIG_DFL);
    raise(sig);
}

static void installCrashHandler() {
    // Alternate signal stack (survives stack overflow crashes)
    static char s_sigStack[SIGSTKSZ * 4];
    stack_t ss;
    ss.ss_sp    = s_sigStack;
    ss.ss_size  = sizeof(s_sigStack);
    ss.ss_flags = 0;
    sigaltstack(&ss, nullptr);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = crashHandler;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS,  &sa, nullptr);
    sigaction(SIGILL,  &sa, nullptr);
    sigaction(SIGFPE,  &sa, nullptr);

    LOGI("[CRASH] Handler installed");
}

// ================================================================
//   HOOK TRACKING MACROS
// ================================================================
// Track first call of each hook so unused hooks are obvious in the log
#define TRACK_FIRST_CALL(tag) do { \
    static std::atomic<bool> _firstCall{true}; \
    if (_firstCall.exchange(false, std::memory_order_relaxed)) { \
        LOGI("[FIRST-CALL] %s", tag); \
    } \
} while(0)

// Install hook and log success/failure
#define HOOK_AND_LOG(className, methodName, addr, replace, orig) do { \
    HOOK_AU((void*)(addr), (void*)(replace), (orig)); \
    if ((orig) != nullptr) { \
        LOGI("[HOOK OK]   %s.%s  @ %p", className, methodName, (void*)(addr)); \
    } else { \
        LOGE("[HOOK FAIL] %s.%s  @ %p  — original is null", \
             className, methodName, (void*)(addr)); \
    } \
} while(0)

#define LOG_SKIP(className, methodName) \
    LOGE("[HOOK SKIP] %s.%s — offset not found", className, methodName)

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
//   HOOK IMPLEMENTATIONS (EXISTING + tracked)
// ================================================================
static bool new_CheckFrontalImpact(void* self, void* other) {
    TRACK_FIRST_CALL("CharacterMotor.CheckFrontalImpact");
    if (!self) return false;
    if (g_FrontalImpact.load()) return false;
    return old_CheckFrontalImpact ? old_CheckFrontalImpact(self, other) : false;
}

static bool new_CheckSideImpact(void* self, void* other) {
    TRACK_FIRST_CALL("CharacterMotor.CheckSideImpact");
    if (!self) return false;
    if (g_SideImpact.load()) return false;
    return old_CheckSideImpact ? old_CheckSideImpact(self, other) : false;
}

static int new_get_JumpLimit(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_JumpLimit");
    if (!self) return 0;
    int v = g_JumpLimit.load();
    return (v > 0) ? v : (old_get_JumpLimit ? old_get_JumpLimit(self) : 0);
}

static float new_get_JumpHeight(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_JumpHeight");
    if (!self) return 0.0f;
    int v = g_JumpHeight.load();
    return (v > 0) ? (float)v : (old_get_JumpHeight ? old_get_JumpHeight(self) : 0.0f);
}

static float new_get_LaneChangeDuration(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_LaneChangeDuration");
    if (!self) return 0.0f;
    if (g_FastLaneChange.load()) return 0.0f;
    return old_get_LaneChangeDuration ? old_get_LaneChangeDuration(self) : 0.0f;
}

static bool new_IsAutoReviveEnabled(void* self) {
    TRACK_FIRST_CALL("StumbleBehaviour.IsAutoReviveEnabled");
    if (!self) return false;
    if (g_AutoRevive.load()) return true;
    return old_IsAutoReviveEnabled ? old_IsAutoReviveEnabled(self) : false;
}

static float new_GetDuration(void* self, void* cfg) {
    TRACK_FIRST_CALL("PowerConfig.GetDuration");
    if (!self) return 0.0f;
    int v = g_PowerDuration.load();
    return (v > 0) ? (float)v : (old_GetDuration ? old_GetDuration(self, cfg) : 0.0f);
}

static int new_get_BaseMultiplierSum(void* self) {
    TRACK_FIRST_CALL("ScoreMultiplierManager.get_BaseMultiplierSum");
    if (!self) return 0;
    int v = g_ScoreMultiplier.load();
    return (v > 0) ? v : (old_get_BaseMultiplierSum ? old_get_BaseMultiplierSum(self) : 0);
}

static bool new_get_detectCollisions(void* self) {
    TRACK_FIRST_CALL("CharacterController.get_detectCollisions");
    if (!self) return true;
    if (g_NoCollision.load()) {
        long long start = g_NoCollisionStartMs.load();
        if (start > 0 && (nowMs() - start) < 1000) return false;
    }
    return old_get_detectCollisions ? old_get_detectCollisions(self) : true;
}

static int new_GetCurrency(void* self, int currencyType) {
    TRACK_FIRST_CALL("WalletModel.GetCurrency");
    if (!self) return 0;
    if (g_UnlimitedCoins.load()) return 99999999;
    return old_GetCurrency ? old_GetCurrency(self, currencyType) : 0;
}

// ================================================================
//   HOOK IMPLEMENTATIONS (NEW)
// ================================================================

static bool new_get_IsIAP(void* self) {
    TRACK_FIRST_CALL("Currency.get_IsIAP");
    if (g_UnlimitedShop.load()) return false;
    return old_get_IsIAP ? old_get_IsIAP(self) : false;
}

static void new_ApplyGravity(void* self) {
    TRACK_FIRST_CALL("CharacterMotor.ApplyGravity");
    if (g_NoGravity.load()) return;
    if (old_ApplyGravity) old_ApplyGravity(self);
}

static float new_get_MinSpeed(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_MinSpeed");
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MinSpeed ? old_get_MinSpeed(self) : 0.0f;
}

static float new_get_MaxSpeed(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_MaxSpeed");
    int v = g_CharacterSpeed.load();
    if (v > 0) return (float)v;
    return old_get_MaxSpeed ? old_get_MaxSpeed(self) : 0.0f;
}

static void new_UpdateMovement(void* self) {
    TRACK_FIRST_CALL("CharacterMotor.UpdateMovement");
    if (g_FreezeCharacter.load()) return;
    if (old_UpdateMovement) old_UpdateMovement(self);
}

static float new_get_AirJumpHeight(void* self) {
    TRACK_FIRST_CALL("CharacterMotorAbilities.get_AirJumpHeight");
    int v = g_AirJumpHeight.load();
    if (v > 0) return (float)v;
    return old_get_AirJumpHeight ? old_get_AirJumpHeight(self) : 0.0f;
}

static bool new_CameraApply(void* self, void* context) {
    TRACK_FIRST_CALL("CameraGroundedModifier.Apply");
    int mode = g_CameraMode.load();
    if (mode == 1) return true;
    if (mode == 2) return false;
    return old_CameraApply ? old_CameraApply(self, context) : false;
}

static void new_NotifySideImpact(void* self, int horizType, int entityType, void* entity) {
    TRACK_FIRST_CALL("CharacterMotor.NotifySideImpact");
    if (g_NoBoundaryImpact.load()) return;
    if (old_NotifySideImpact) old_NotifySideImpact(self, horizType, entityType, entity);
}

static void new_OnStumble(void* self, int deathCause) {
    TRACK_FIRST_CALL("RunProgressOrderIndicator.OnStumble");
    if (g_NoStumble.load()) return;
    if (old_OnStumble) old_OnStumble(self, deathCause);
}

static void new_ShowFlight(void* self, bool show, int flightMode) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowFlight");
    if (g_HideJetpack.load()) return;
    if (old_ShowFlight) old_ShowFlight(self, show, flightMode);
}

static void new_ShowPogostick(void* self, bool show) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowPogostick");
    if (g_HidePogostick.load()) return;
    if (old_ShowPogostick) old_ShowPogostick(self, show);
}

static void new_ShowMagnet(void* self, bool show) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowMagnet");
    if (g_HideMagnet.load()) return;
    if (old_ShowMagnet) old_ShowMagnet(self, show);
}

static void new_ShowHoverboard(void* self, bool randomBoard) {
    TRACK_FIRST_CALL("CharacterPropsController.ShowHoverboard");
    if (g_HideHoverboard.load()) return;
    if (old_ShowHoverboard) old_ShowHoverboard(self, randomBoard);
}

static void new_Displace(void* self) {
    TRACK_FIRST_CALL("MovingTrain.Displace");
    if (g_StopTrains.load()) return;
    if (old_Displace) old_Displace(self);
}

static void new_Addcoins(void* self, int coins) {
    TRACK_FIRST_CALL("RunSessionData.Addcoins");
    int bonus = g_AddCoinsBonus.load();
    if (old_Addcoins) {
        if (bonus > 0) old_Addcoins(self, coins + bonus);
        else           old_Addcoins(self, coins);
    }
}

static void new_AddKeys(void* self, int keys) {
    TRACK_FIRST_CALL("RunSessionData.AddKeys");
    int bonus = g_AddKeysBonus.load();
    if (old_AddKeys) {
        if (bonus > 0) old_AddKeys(self, keys + bonus);
        else           old_AddKeys(self, keys);
    }
}

static void new_AddPoints(void* self, double points) {
    TRACK_FIRST_CALL("RunSessionData.AddPoints");
    int bonus = g_AddScoreBonus.load();
    if (old_AddPoints) {
        if (bonus > 0) old_AddPoints(self, points + (double)bonus);
        else           old_AddPoints(self, points);
    }
}

static void new_UpdatePowers(void* self) {
    TRACK_FIRST_CALL("CharacterPower.UpdatePowers");
    if (g_UnlimitedPowerTime.load()) return;
    if (old_UpdatePowers) old_UpdatePowers(self);
}

static void new_MainUpdatePowers(void* self) {
    TRACK_FIRST_CALL("MainPowerSystem.UpdatePowers");
    if (g_UnlimitedPowerTime.load()) return;
    if (old_MainUpdatePowers) old_MainUpdatePowers(self);
}

static void new_FinishFlight(void* self) {
    TRACK_FIRST_CALL("FlyingState.FinishFlight");
    if (g_UnlimitedJetpackTime.load()) return;
    if (old_FinishFlight) old_FinishFlight(self);
}

static void new_OnRevivedWithCurrency(void* self) {
    TRACK_FIRST_CALL("MainReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) {
        if (old_OnRevivedWithCurrency) old_OnRevivedWithCurrency(self);
    }
}

static void new_MockRevivedWithCurrency(void* self) {
    TRACK_FIRST_CALL("MockReviveSystem.OnRevivedWithCurrency");
    int n = g_ReviveReward.load() ? 10 : 1;
    for (int i = 0; i < n; i++) {
        if (old_MockRevivedWithCurrency) old_MockRevivedWithCurrency(self);
    }
}

static void new_HandleCoinsPickedUp(void* self) {
    TRACK_FIRST_CALL("GameScreenCoinCounter.HandleOnCoinsPickedUp");
    if (g_FreezeCoins.load()) return;
    if (old_HandleCoinsPickedUp) old_HandleCoinsPickedUp(self);
}

static void new_HandlePointsEarned(void* self) {
    TRACK_FIRST_CALL("GameScreenScoreCounter.HandleOnPointsEarned");
    if (g_FreezeScore.load()) return;
    if (old_HandlePointsEarned) old_HandlePointsEarned(self);
}

// ================================================================
//   HEARTBEAT THREAD (tells you if game froze silently)
// ================================================================
static void* heartbeat_thread(void*) {
    int n = 0;
    while (true) {
        sleep(60);
        n++;
        LOGI("[HEARTBEAT %02d] Game still alive  |  Chams=%d  mode=%d",
             n, (int)g_ChamsEnabled, chamsint);
    }
    return nullptr;
}

// ================================================================
//   HACK THREAD
// ================================================================
void* hack_thread(void*) {
    LOGI("Waiting for %s ...", LIB);
    while (!isLibraryLoaded(LIB)) sleep(1);
    sleep(3);

    // ★ Log module base address so backtrace RVAs can be computed
    {
        void* sym = dlsym(RTLD_DEFAULT, "il2cpp_init");
        if (sym) {
            Dl_info info;
            if (dladdr(sym, &info)) {
                LOGI("[MODULE] %s  base = %p", info.dli_fname, info.dli_fbase);
            }
        } else {
            LOGW("[MODULE] il2cpp_init symbol not found — cannot compute base");
        }
    }

    LOGI("========================================");
    LOGI("Installing hooks ...");
    LOGI("========================================");

    // ---------- EXISTING HOOKS ----------
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oF = CM->GetMethodOffsetByName(OBFUSCATE("CheckFrontalImpact"), 1);
            DWORD oS = CM->GetMethodOffsetByName(OBFUSCATE("CheckSideImpact"),    1);
            if (oF) HOOK_AND_LOG("CharacterMotor", "CheckFrontalImpact", oF, (void*)new_CheckFrontalImpact, old_CheckFrontalImpact);
            else    LOG_SKIP("CharacterMotor", "CheckFrontalImpact");
            if (oS) HOOK_AND_LOG("CharacterMotor", "CheckSideImpact", oS, (void*)new_CheckSideImpact, old_CheckSideImpact);
            else    LOG_SKIP("CharacterMotor", "CheckSideImpact");
        } else LOGE("[CLASS MISS] CharacterMotor");
    }
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oJL = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpLimit"),        0);
            DWORD oJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_JumpHeight"),       0);
            DWORD oLC = CMA->GetMethodOffsetByName(OBFUSCATE("get_LaneChangeDuration"),0);
            if (oJL) HOOK_AND_LOG("CharacterMotorAbilities", "get_JumpLimit",         oJL, (void*)new_get_JumpLimit,         old_get_JumpLimit);
            else     LOG_SKIP("CharacterMotorAbilities", "get_JumpLimit");
            if (oJH) HOOK_AND_LOG("CharacterMotorAbilities", "get_JumpHeight",        oJH, (void*)new_get_JumpHeight,        old_get_JumpHeight);
            else     LOG_SKIP("CharacterMotorAbilities", "get_JumpHeight");
            if (oLC) HOOK_AND_LOG("CharacterMotorAbilities", "get_LaneChangeDuration",oLC, (void*)new_get_LaneChangeDuration, old_get_LaneChangeDuration);
            else     LOG_SKIP("CharacterMotorAbilities", "get_LaneChangeDuration");
        } else LOGE("[CLASS MISS] CharacterMotorAbilities");
    }
    {
        auto SB = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("StumbleBehaviour"));
        if (SB && SB->thisclass) {
            DWORD oR = SB->GetMethodOffsetByName(OBFUSCATE("IsAutoReviveEnabled"), 0);
            if (oR) HOOK_AND_LOG("StumbleBehaviour", "IsAutoReviveEnabled", oR, (void*)new_IsAutoReviveEnabled, old_IsAutoReviveEnabled);
            else    LOG_SKIP("StumbleBehaviour", "IsAutoReviveEnabled");
        } else LOGE("[CLASS MISS] StumbleBehaviour");
    }
    {
        auto PC = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Powers"), OBFUSCATE("PowerConfig"));
        if (PC && PC->thisclass) {
            DWORD oD = PC->GetMethodOffsetByName(OBFUSCATE("GetDuration"), 1);
            if (oD) HOOK_AND_LOG("PowerConfig", "GetDuration", oD, (void*)new_GetDuration, old_GetDuration);
            else    LOG_SKIP("PowerConfig", "GetDuration");
        } else LOGE("[CLASS MISS] PowerConfig");
    }
    {
        auto SMM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("ScoreMultiplierManager"));
        if (SMM && SMM->thisclass) {
            DWORD oB = SMM->GetMethodOffsetByName(OBFUSCATE("get_BaseMultiplierSum"), 0);
            if (oB) HOOK_AND_LOG("ScoreMultiplierManager", "get_BaseMultiplierSum", oB, (void*)new_get_BaseMultiplierSum, old_get_BaseMultiplierSum);
            else    LOG_SKIP("ScoreMultiplierManager", "get_BaseMultiplierSum");
        } else LOGE("[CLASS MISS] ScoreMultiplierManager");
    }
    {
        auto CC = new LoadClass(OBFUSCATE("UnityEngine"), OBFUSCATE("CharacterController"));
        if (CC && CC->thisclass) {
            DWORD oD = CC->GetMethodOffsetByName(OBFUSCATE("get_detectCollisions"), 0);
            if (oD) HOOK_AND_LOG("CharacterController", "get_detectCollisions", oD, (void*)new_get_detectCollisions, old_get_detectCollisions);
            else    LOG_SKIP("CharacterController", "get_detectCollisions");
        } else LOGE("[CLASS MISS] CharacterController");
    }
    {
        auto WM = new LoadClass(OBFUSCATE("SYBO.Subway.Core.ProfileData"), OBFUSCATE("WalletModel"));
        if (WM && WM->thisclass) {
            DWORD oC = WM->GetMethodOffsetByName(OBFUSCATE("GetCurrency"), 1);
            if (oC) HOOK_AND_LOG("WalletModel", "GetCurrency", oC, (void*)new_GetCurrency, old_GetCurrency);
            else    LOG_SKIP("WalletModel", "GetCurrency");
        } else LOGE("[CLASS MISS] WalletModel");
    }

    // ---------- NEW HOOKS ----------
    // 1) Unlimited Shop
    {
        auto C = new LoadClass(OBFUSCATE("SYBO.Subway.Core.CommonData"), OBFUSCATE("Currency"));
        if (C && C->thisclass) {
            DWORD o = C->GetMethodOffsetByName(OBFUSCATE("get_IsIAP"), 0);
            if (o) HOOK_AND_LOG("Currency", "get_IsIAP", o, (void*)new_get_IsIAP, old_get_IsIAP);
            else    LOG_SKIP("Currency", "get_IsIAP");
        } else LOGE("[CLASS MISS] Currency");
    }
    // 2) CharacterMotor (gravity, movement, side impact)
    {
        auto CM = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotor"));
        if (CM && CM->thisclass) {
            DWORD oG = CM->GetMethodOffsetByName(OBFUSCATE("ApplyGravity"),     0);
            DWORD oU = CM->GetMethodOffsetByName(OBFUSCATE("UpdateMovement"),   0);
            DWORD oN = CM->GetMethodOffsetByName(OBFUSCATE("NotifySideImpact"), 3);
            if (oG) HOOK_AND_LOG("CharacterMotor", "ApplyGravity",     oG, (void*)new_ApplyGravity,     old_ApplyGravity);
            else    LOG_SKIP("CharacterMotor", "ApplyGravity");
            if (oU) HOOK_AND_LOG("CharacterMotor", "UpdateMovement",   oU, (void*)new_UpdateMovement,   old_UpdateMovement);
            else    LOG_SKIP("CharacterMotor", "UpdateMovement");
            if (oN) HOOK_AND_LOG("CharacterMotor", "NotifySideImpact", oN, (void*)new_NotifySideImpact, old_NotifySideImpact);
            else    LOG_SKIP("CharacterMotor", "NotifySideImpact");
        }
    }
    // 3) CharacterMotorAbilities (speed, air jump)
    {
        auto CMA = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("CharacterMotorAbilities"));
        if (CMA && CMA->thisclass) {
            DWORD oMin = CMA->GetMethodOffsetByName(OBFUSCATE("get_MinSpeed"),      0);
            DWORD oMax = CMA->GetMethodOffsetByName(OBFUSCATE("get_MaxSpeed"),      0);
            DWORD oAJH = CMA->GetMethodOffsetByName(OBFUSCATE("get_AirJumpHeight"), 0);
            if (oMin) HOOK_AND_LOG("CharacterMotorAbilities", "get_MinSpeed",      oMin, (void*)new_get_MinSpeed,      old_get_MinSpeed);
            else      LOG_SKIP("CharacterMotorAbilities", "get_MinSpeed");
            if (oMax) HOOK_AND_LOG("CharacterMotorAbilities", "get_MaxSpeed",      oMax, (void*)new_get_MaxSpeed,      old_get_MaxSpeed);
            else      LOG_SKIP("CharacterMotorAbilities", "get_MaxSpeed");
            if (oAJH) HOOK_AND_LOG("CharacterMotorAbilities", "get_AirJumpHeight", oAJH, (void*)new_get_AirJumpHeight, old_get_AirJumpHeight);
            else      LOG_SKIP("CharacterMotorAbilities", "get_AirJumpHeight");
        }
    }
    // 4) Camera
    {
        auto CGM = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CameraGroundedModifier"));
        if (CGM && CGM->thisclass) {
            DWORD o = CGM->GetMethodOffsetByName(OBFUSCATE("Apply"), 1);
            if (o) HOOK_AND_LOG("CameraGroundedModifier", "Apply", o, (void*)new_CameraApply, old_CameraApply);
            else    LOG_SKIP("CameraGroundedModifier", "Apply");
        } else LOGE("[CLASS MISS] CameraGroundedModifier");
    }
    // 5) OnStumble
    {
        auto RPOI = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunProgressOrderIndicator"));
        if (RPOI && RPOI->thisclass) {
            DWORD o = RPOI->GetMethodOffsetByName(OBFUSCATE("OnStumble"), 1);
            if (o) HOOK_AND_LOG("RunProgressOrderIndicator", "OnStumble", o, (void*)new_OnStumble, old_OnStumble);
            else    LOG_SKIP("RunProgressOrderIndicator", "OnStumble");
        } else LOGE("[CLASS MISS] RunProgressOrderIndicator");
    }
    // 6) CharacterPropsController
    {
        auto CPC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPropsController"));
        if (CPC && CPC->thisclass) {
            DWORD oF = CPC->GetMethodOffsetByName(OBFUSCATE("ShowFlight"),     2);
            DWORD oP = CPC->GetMethodOffsetByName(OBFUSCATE("ShowPogostick"),  1);
            DWORD oM = CPC->GetMethodOffsetByName(OBFUSCATE("ShowMagnet"),     1);
            DWORD oH = CPC->GetMethodOffsetByName(OBFUSCATE("ShowHoverboard"), 1);
            if (oF) HOOK_AND_LOG("CharacterPropsController", "ShowFlight",     oF, (void*)new_ShowFlight,     old_ShowFlight);
            else    LOG_SKIP("CharacterPropsController", "ShowFlight");
            if (oP) HOOK_AND_LOG("CharacterPropsController", "ShowPogostick",  oP, (void*)new_ShowPogostick,  old_ShowPogostick);
            else    LOG_SKIP("CharacterPropsController", "ShowPogostick");
            if (oM) HOOK_AND_LOG("CharacterPropsController", "ShowMagnet",     oM, (void*)new_ShowMagnet,     old_ShowMagnet);
            else    LOG_SKIP("CharacterPropsController", "ShowMagnet");
            if (oH) HOOK_AND_LOG("CharacterPropsController", "ShowHoverboard", oH, (void*)new_ShowHoverboard, old_ShowHoverboard);
            else    LOG_SKIP("CharacterPropsController", "ShowHoverboard");
        } else LOGE("[CLASS MISS] CharacterPropsController");
    }
    // 7) MovingTrain.Displace
    {
        auto MT = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("MovingTrain"));
        if (MT && MT->thisclass) {
            DWORD o = MT->GetMethodOffsetByName(OBFUSCATE("Displace"), 0);
            if (o) HOOK_AND_LOG("MovingTrain", "Displace", o, (void*)new_Displace, old_Displace);
            else    LOG_SKIP("MovingTrain", "Displace");
        } else LOGE("[CLASS MISS] MovingTrain");
    }
    // 8) RunSessionData
    {
        auto RSD = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("RunSessionData"));
        if (RSD && RSD->thisclass) {
            DWORD oC = RSD->GetMethodOffsetByName(OBFUSCATE("Addcoins"),  1);
            DWORD oK = RSD->GetMethodOffsetByName(OBFUSCATE("AddKeys"),   1);
            DWORD oP = RSD->GetMethodOffsetByName(OBFUSCATE("AddPoints"), 1);
            if (oC) HOOK_AND_LOG("RunSessionData", "Addcoins",  oC, (void*)new_Addcoins,  old_Addcoins);
            else    LOG_SKIP("RunSessionData", "Addcoins");
            if (oK) HOOK_AND_LOG("RunSessionData", "AddKeys",   oK, (void*)new_AddKeys,   old_AddKeys);
            else    LOG_SKIP("RunSessionData", "AddKeys");
            if (oP) HOOK_AND_LOG("RunSessionData", "AddPoints", oP, (void*)new_AddPoints, old_AddPoints);
            else    LOG_SKIP("RunSessionData", "AddPoints");
        } else LOGE("[CLASS MISS] RunSessionData");
    }
    // 9) CharacterPower.UpdatePowers
    {
        auto CP = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("CharacterPower"));
        if (CP && CP->thisclass) {
            DWORD o = CP->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AND_LOG("CharacterPower", "UpdatePowers", o, (void*)new_UpdatePowers, old_UpdatePowers);
            else    LOG_SKIP("CharacterPower", "UpdatePowers");
        } else LOGE("[CLASS MISS] CharacterPower");
    }
    // 10) MainPowerSystem.UpdatePowers
    {
        auto MPS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainPowerSystem"));
        if (MPS && MPS->thisclass) {
            DWORD o = MPS->GetMethodOffsetByName(OBFUSCATE("UpdatePowers"), 0);
            if (o) HOOK_AND_LOG("MainPowerSystem", "UpdatePowers", o, (void*)new_MainUpdatePowers, old_MainUpdatePowers);
            else    LOG_SKIP("MainPowerSystem", "UpdatePowers");
        } else LOGE("[CLASS MISS] MainPowerSystem");
    }
    // 11) FlyingState.FinishFlight
    {
        auto FS = new LoadClass(OBFUSCATE("SYBO.RunnerCore.Character"), OBFUSCATE("FlyingState"));
        if (FS && FS->thisclass) {
            DWORD o = FS->GetMethodOffsetByName(OBFUSCATE("FinishFlight"), 0);
            if (o) HOOK_AND_LOG("FlyingState", "FinishFlight", o, (void*)new_FinishFlight, old_FinishFlight);
            else    LOG_SKIP("FlyingState", "FinishFlight");
        } else LOGE("[CLASS MISS] FlyingState");
    }
    // 12) MainReviveSystem.OnRevivedWithCurrency
    {
        auto MRS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems"), OBFUSCATE("MainReviveSystem"));
        if (MRS && MRS->thisclass) {
            DWORD o = MRS->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AND_LOG("MainReviveSystem", "OnRevivedWithCurrency", o, (void*)new_OnRevivedWithCurrency, old_OnRevivedWithCurrency);
            else    LOG_SKIP("MainReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MainReviveSystem");
    }
    // 13) MockReviveSystem.OnRevivedWithCurrency
    {
        auto MRS = new LoadClass(OBFUSCATE("SYBO.Subway.Systems.Mocks"), OBFUSCATE("MockReviveSystem"));
        if (MRS && MRS->thisclass) {
            DWORD o = MRS->GetMethodOffsetByName(OBFUSCATE("OnRevivedWithCurrency"), 0);
            if (o) HOOK_AND_LOG("MockReviveSystem", "OnRevivedWithCurrency", o, (void*)new_MockRevivedWithCurrency, old_MockRevivedWithCurrency);
            else    LOG_SKIP("MockReviveSystem", "OnRevivedWithCurrency");
        } else LOGE("[CLASS MISS] MockReviveSystem");
    }
    // 14) GameScreenCoinCounter.HandleOnCoinsPickedUp
    {
        auto GSCC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenCoinCounter"));
        if (GSCC && GSCC->thisclass) {
            DWORD o = GSCC->GetMethodOffsetByName(OBFUSCATE("HandleOnCoinsPickedUp"), 0);
            if (o) HOOK_AND_LOG("GameScreenCoinCounter", "HandleOnCoinsPickedUp", o, (void*)new_HandleCoinsPickedUp, old_HandleCoinsPickedUp);
            else    LOG_SKIP("GameScreenCoinCounter", "HandleOnCoinsPickedUp");
        } else LOGE("[CLASS MISS] GameScreenCoinCounter");
    }
    // 15) GameScreenScoreCounter.HandleOnPointsEarned
    {
        auto GSSC = new LoadClass(OBFUSCATE("SYBO.Subway"), OBFUSCATE("GameScreenScoreCounter"));
        if (GSSC && GSSC->thisclass) {
            DWORD o = GSSC->GetMethodOffsetByName(OBFUSCATE("HandleOnPointsEarned"), 0);
            if (o) HOOK_AND_LOG("GameScreenScoreCounter", "HandleOnPointsEarned", o, (void*)new_HandlePointsEarned, old_HandlePointsEarned);
            else    LOG_SKIP("GameScreenScoreCounter", "HandleOnPointsEarned");
        } else LOGE("[CLASS MISS] GameScreenScoreCounter");
    }

    // ---------- CHAMS ----------
    RenderHook();

    // ---------- HEARTBEAT ----------
    pthread_t hb;
    pthread_create(&hb, nullptr, heartbeat_thread, nullptr);

    LOGI("========================================");
    LOGI("All hooks installation pass complete.");
    LOGI("Watch for [FIRST-CALL] lines to verify each hook fires.");
    LOGI("========================================");
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

        OBFUSCATE("Category_Powers"),
        OBFUSCATE("105_SeekBar_Power Duration_0_999"),
        OBFUSCATE("106_Toggle_Fast Lane Change"),
        OBFUSCATE("135_Toggle_Hide Jetpack"),
        OBFUSCATE("136_Toggle_Hide Pogostick"),
        OBFUSCATE("137_Toggle_Hide Magnet"),
        OBFUSCATE("138_Toggle_Hide Hoverboard"),
        OBFUSCATE("140_Toggle_Unlimited Powerup Time"),
        OBFUSCATE("141_Toggle_Unlimited Jetpack Time"),

        OBFUSCATE("Category_Score"),
        OBFUSCATE("107_SeekBar_Score Multiplier_0_999"),
        OBFUSCATE("143_Toggle_Freeze Coins"),
        OBFUSCATE("144_Toggle_Freeze Score"),

        OBFUSCATE("Category_Collisions"),
        OBFUSCATE("108_Toggle_No Collision Detect"),

        OBFUSCATE("Category_Shop"),
        OBFUSCATE("110_Toggle_Unlimited Coins"),
        OBFUSCATE("150_Toggle_Unlimited Shop"),
        OBFUSCATE("151_SeekBar_Add Coins Bonus_0_9999"),
        OBFUSCATE("152_SeekBar_Add Keys Bonus_0_9999"),
        OBFUSCATE("153_SeekBar_Add Score Bonus_0_9999"),
        OBFUSCATE("154_Toggle_Revive Reward"),

        OBFUSCATE("Category_Character"),
        OBFUSCATE("160_SeekBar_Character Speed_0_200"),

        OBFUSCATE("Category_Camera"),
        OBFUSCATE("161_Spinner_Camera Mode_Normal,Follow,Stop"),

        OBFUSCATE("Category_World"),
        OBFUSCATE("142_Toggle_Stop Moving Trains"),

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

        // ---------- NEW ----------
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

        // ---------- CHAMS ----------
        case 119: g_ChamsEnabled = boolean;     break;
        case 120: chamsint = value;             break;
        case 125: setColorPreset(value);        break;
        case 126: setShaderPreset(value);       break;
        case 127: g_MaxDistance = (float)value; break;
        case 121: enableRainbow = boolean;      break;
        case 122: enableRainbowWall = boolean;  break;

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
//   STUBS
// ================================================================
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_Draw(JNIEnv*, jclass, jobject, jobject) { }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Menu_SetTeleportTargetNorm(JNIEnv*, jclass, jfloat, jfloat) { }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_GetTeleportEnabled(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT jboolean JNICALL Java_com_android_support_Menu_IsSmoothTeleportActive(JNIEnv*, jclass) { return JNI_FALSE; }
extern "C" JNIEXPORT void JNICALL Java_com_android_support_Main_setNativeCrashDir(JNIEnv*, jclass, jstring) { }

// ================================================================
//   CONSTRUCTOR — init logging + crash handler FIRST
// ================================================================
__attribute__((constructor))
void lib_main() {
    initLogging();             // sets up game-folder log path + crash fd
    installCrashHandler();     // installs SIGSEGV/SIGABRT/... handlers

    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}