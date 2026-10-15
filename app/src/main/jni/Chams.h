#ifndef NORKA_CHAMS
#define NORKA_CHAMS

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <android/log.h>
#include <cstdio>
#include <cstdarg>
#include <ctime>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string>

// ================================================================
//  Architecture-aware hook selection
// ================================================================
#if defined(__aarch64__)
    #include "And64InlineHook/And64InlineHook.hpp"
    #define HOOK_FN(addr, replace, orig) \
        A64HookFunction((void*)(addr), (void*)(replace), (void**)(orig))
#else
    #include "Substrate/SubstrateHook.h"
    #define HOOK_FN(addr, replace, orig) \
        MSHookFunction((void*)(addr), (void*)(replace), (void**)(orig))
#endif

#include "RGB.h"

// ================================================================
//  ⚙️ TUNABLE PARAMETERS
// ================================================================
static const int kMinVertexCount = 3000;

static bool g_DiagnosticMode = false;

// ================================================================
//  📁 FILE LOGGER  →  /sdcard/Documents/ChamsLog.txt
// ================================================================
static FILE*          g_LogFile        = nullptr;
static pthread_mutex_t g_LogMutex      = PTHREAD_MUTEX_INITIALIZER;
static char           g_LogPath[256]   = {0};

// ---------------------------------------------------------------
//  Try several candidate paths, open the first writable one
// ---------------------------------------------------------------
static void openLogFile() {
    if (g_LogFile) return;

    const char* candidates[] = {
        "/sdcard/Documents/ChamsLog.txt",
        "/storage/emulated/0/Documents/ChamsLog.txt",
        "/sdcard/Android/data/com.kiloo.subwaysurf/files/ChamsLog.txt",
        "/data/local/tmp/ChamsLog.txt"
    };

    for (auto path : candidates) {
        // Ensure parent dir exists (only for /sdcard/... which is really /storage/emulated/0)
        FILE* f = fopen(path, "w");
        if (f) {
            g_LogFile = f;
            strncpy(g_LogPath, path, sizeof(g_LogPath) - 1);
            // Timestamp header
            time_t now = time(nullptr);
            struct tm* tm_info = localtime(&now);
            char tb[64];
            strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", tm_info);
            fprintf(g_LogFile, "=== Chams Log @ %s ===\n", tb);
            fflush(g_LogFile);
            return;
        }
    }
    // Couldn't open any file — silently continue with logcat only
}

static void closeLogFile() {
    if (g_LogFile) {
        pthread_mutex_lock(&g_LogMutex);
        fprintf(g_LogFile, "\n=== End of log ===\n");
        fflush(g_LogFile);
        fclose(g_LogFile);
        g_LogFile = nullptr;
        pthread_mutex_unlock(&g_LogMutex);
    }
}

// ---------------------------------------------------------------
//  Dual-output logger: logcat + file
// ---------------------------------------------------------------
static void fileLog(const char* level, const char* fmt, ...) {
    // 1) logcat
    va_list ap1;
    va_start(ap1, fmt);
    __android_log_vprint(ANDROID_LOG_INFO, "CHAMS", fmt, ap1);
    va_end(ap1);

    // 2) file
    if (!g_LogFile) return;
    pthread_mutex_lock(&g_LogMutex);
    time_t now = time(nullptr);
    struct tm* tm_info = localtime(&now);
    char tb[32];
    strftime(tb, sizeof(tb), "%H:%M:%S", tm_info);
    fprintf(g_LogFile, "[%s][%s] ", tb, level);
    va_list ap2;
    va_start(ap2, fmt);
    vfprintf(g_LogFile, fmt, ap2);
    va_end(ap2);
    fprintf(g_LogFile, "\n");
    fflush(g_LogFile);      // flush immediately so user sees live
    pthread_mutex_unlock(&g_LogMutex);
}

#define LOGI(...) fileLog("I", __VA_ARGS__)
#define LOGD(...) fileLog("D", __VA_ARGS__)
#define LOGE(...) fileLog("E", __VA_ARGS__)

// ================================================================
//  Static state
// ================================================================
static void *handle = nullptr;
static const char* shaderName = "_MainTex";
static bool enableWallhack = false;
static bool enableWallhackW = false;
static bool enableWallhackG = false;
static bool enableWallhackO = false;
static bool enableRainbow = false;
static bool isRGB = false;

static float r = 255.0f;
static float g = 0.0f;
static float b = 0.0f;
static int w1 = 2;
static float w2 = 2.0f;
static float w3 = 2.0f;
static int a = 255;

float red  = 0.0f;
float gren = 0.0f;
float blue = 0.0f;
float mi   = 0.0f;

// ----------------------------------------------------------------
//  Setters / getters
// ----------------------------------------------------------------
inline void setShader(const char* s) {
    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    shaderName = s;
    LOGI("Shader name set to: %s", s);
}
inline const char* getShader() { return shaderName; }

inline void SetWallhack(bool e)  { enableWallhack  = e; LOGI("Wallhack: %d", e); }
inline void SetWireframe(bool e) { enableWallhackW = e; LOGI("Wireframe: %d", e); }
inline void SetGlow(bool e)      { enableWallhackG = e; LOGI("Glow: %d", e); }
inline void SetOutline(bool e)   { enableWallhackO = e; LOGI("Outline: %d", e); }
inline void SetRainbow(bool e)   { enableRainbow   = e; LOGI("Rainbow: %d", e); }
inline void SetRGB(bool e)       { isRGB           = e; LOGI("RGB: %d", e); }
inline void SetDiagnostic(bool e){ g_DiagnosticMode = e; LOGI("Diagnostic mode: %d", e); }

inline void SetR(int v) { r = v; }
inline void SetG(int v) { g = v; }
inline void SetB(int v) { b = v; }
inline void SetWireframeWidth(int v) { w1 = v; }
inline void SetGlowWidth(int v)      { w2 = v; }
inline void SetOutlineWidth(int v)   { w3 = v; }

inline bool getWallhackEnabled()  { return enableWallhack;  }
inline bool getWireframeEnabled() { return enableWallhackW; }
inline bool getGlowEnabled()      { return enableWallhackG; }
inline bool getOutlineEnabled()   { return enableWallhackO; }
inline bool getRainbowEnabled()   { return enableRainbow;   }
inline bool getRGB()              { return isRGB;           }

// ----------------------------------------------------------------
//  glGetUniformLocation hook
// ----------------------------------------------------------------
static GLint (*old_glGetUniformLocation)(GLuint, const GLchar *) = nullptr;
static GLint new_glGetUniformLocation(GLuint program, const GLchar *name) {
    if (!old_glGetUniformLocation) return -1;
    return old_glGetUniformLocation(program, name);
}

// ----------------------------------------------------------------
//  Diagnostic: check multiple shader names on the current program
// ----------------------------------------------------------------
static void diagnoseProgram(GLint prog, GLsizei count) {
    const char* candidates[] = {
        "_MainTex", "_BaseMap", "_BaseColor", "_Color", "_Albedo",
        "_Displacement", "_EmissionMap", "_Metallic", "_BumpMap",
        "_SpecColor", "_OutlineColor", "_RimColor"
    };
    char buf[512] = {0};
    int  len = 0;
    for (auto name : candidates) {
        GLint id = old_glGetUniformLocation(prog, name);
        if (id != -1) {
            len += snprintf(buf + len, sizeof(buf) - len, "%s(%d) ", name, id);
            if (len >= (int)sizeof(buf) - 32) break;
        }
    }
    LOGI("[DIAG] prog=%d count=%d  uniforms: %s", prog, count, buf);
}

// ----------------------------------------------------------------
//  glDrawElements hook — all chams logic
// ----------------------------------------------------------------
static void (*old_glDrawElements)(GLenum, GLsizei, GLenum, const void *) = nullptr;
static void new_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (!old_glDrawElements) return;

    old_glDrawElements(mode, count, type, indices);

    if (mode != GL_TRIANGLES) return;

    // --------------------------------------------
    //  DIAGNOSTIC MODE
    // --------------------------------------------
    if (g_DiagnosticMode && count >= 500) {
        GLint prog; glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        diagnoseProgram(prog, count);
    }

    // --------------------------------------------
    //  FILTER: only large meshes (trains/obstacles)
    // --------------------------------------------
    if (count < kMinVertexCount) return;

    GLint currProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
    GLint id = old_glGetUniformLocation(currProgram, getShader());
    if (id == -1) return;

    static GLint lastLoggedProg = -1;
    if (currProgram != lastLoggedProg) {
        LOGD("Chams applying → prog=%d count=%d", currProgram, count);
        lastLoggedProg = currProgram;
    }

    if (getWireframeEnabled()) {
        glDepthRangef(1, 0.0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(w1);
        performRGBChange();
        if (isRGB) glBlendColor(redd * 0.00392f, greenn * 0.00392f, bluee * 0.00392f, 1.0f);
        else       glBlendColor(GLfloat(r/255), GLfloat(g/255), GLfloat(b/255), 1.0f);
        glColorMask(1, 1, 1, 1);
        old_glDrawElements(GL_LINE_LOOP, count, type, indices);
    }

    if (getWallhackEnabled()) {
        glDepthRangef(1, 0.0);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        performRGBChange();
        if (isRGB) glBlendColor(redd * 0.00392f, greenn * 0.00392f, bluee * 0.00392f, 1.0f);
        else       glBlendColor(GLfloat(r/255), GLfloat(g/255), GLfloat(b/255), 1.0f);
        glColorMask(1, 1, 1, 1);
    }

    if (getGlowEnabled()) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glLineWidth(w2);
        glDepthRangef(0.5, 1);
        performRGBChange();
        if (isRGB) glBlendColor(redd * 0.00392f, greenn * 0.00392f, bluee * 0.00392f, 1.0f);
        else       glBlendColor(GLfloat(r/255), GLfloat(g/255), GLfloat(b/255), 1.0f);
        glColorMask(1, 1, 1, 1);
        old_glDrawElements(GL_LINES, count, type, indices);
        glBlendColor(1, 1, 1, 1);
        glDepthRangef(1, 0.0);
        old_glDrawElements(GL_TRIANGLES, count, type, indices);
    }

    if (getOutlineEnabled()) {
        glDepthRangef(1, 0.0);
        glLineWidth(w3);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        performRGBChange();
        glBlendColor(0, 0, 0, 1);
        glColorMask(1, 1, 1, 1);
        old_glDrawElements(GL_TRIANGLES, count, type, indices);
        if (isRGB) glBlendColor(redd * 0.00392f, greenn * 0.00392f, bluee * 0.00392f, 1.0f);
        else       glBlendColor(GLfloat(r/255), GLfloat(g/255), GLfloat(b/255), 1.0f);
        glDepthRangef(1, 0.5);
        old_glDrawElements(GL_LINES, count, type, indices);
    }

    if (getRainbowEnabled()) {
        if (red == 255) { if (blue == 0) { if (gren != 255) gren += 1.0f; } }
        if (gren == 255) { if (red != 0) red -= 1.0f; }
        if (gren == 255) { if (red == 0) { if (blue != 255) blue += 1.0f; } }
        if (blue == 255) { if (gren == 0) { mi = 1; red += 1.0f; } else { gren -= 1.0f; } }
        if (mi == 1) { if (red == 255) { if (blue != 0) blue -= 1.0f; } }
        SetR((int)red); SetG((int)gren); SetB((int)blue);
    }

    old_glDrawElements(mode, count, type, indices);
    glDepthRangef(0.5, 1);
    glColorMask(1, 1, 1, 1);
    glDisable(GL_BLEND);
}

// ----------------------------------------------------------------
//  Init
// ----------------------------------------------------------------
inline bool mlovinit() {
    openLogFile();          // ★ open file FIRST so we log everything
    LOGI("=== Chams initialization started ===");
    if (g_LogPath[0]) LOGI("Log file: %s", g_LogPath);
    else              LOGI("Log file could not be opened — logcat only");

    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) { LOGE("dlopen libGLESv2.so FAILED"); return false; }
    LOGI("libGLESv2.so loaded OK");
    return true;
}

inline void LogShaders() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) return;
    void *p = dlsym(handle, "glGetUniformLocation");
    if (p) {
        HOOK_FN(p, (void*)new_glGetUniformLocation, (void**)&old_glGetUniformLocation);
        LOGI("glGetUniformLocation hooked @ %p", p);
    } else {
        LOGE("dlsym glGetUniformLocation FAILED");
    }
}

inline void Wallhack() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) return;
    void *p = dlsym(handle, "glDrawElements");
    if (p) {
        HOOK_FN(p, (void*)new_glDrawElements, (void**)&old_glDrawElements);
        LOGI("glDrawElements hooked @ %p", p);
    } else {
        LOGE("dlsym glDrawElements FAILED");
    }
}

#endif