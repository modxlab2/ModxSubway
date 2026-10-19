#ifndef ZEFF_CHAMS
#define ZEFF_CHAMS

#include <dlfcn.h>
#include <array>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <android/log.h>
#include <GLES2/gl2.h>
#include "RGB.h"

#if defined(__aarch64__)
    #include "And64InlineHook/And64InlineHook.hpp"
    #define HOOK_FN(addr, replace, orig) \
        A64HookFunction((void*)(addr), (void*)(replace), (void**)(orig))
#else
    #include "Substrate/SubstrateHook.h"
    #define HOOK_FN(addr, replace, orig) \
        MSHookFunction((void*)(addr), (void*)(replace), (void**)(orig))
#endif

using namespace std;

struct Vec4 {
    float x, y, z, w;
    Vec4() : x(0), y(0), z(0), w(1) {}
    Vec4(float _x, float _y, float _z, float _w) : x(_x), y(_y), z(_z), w(_w) {}
};

#define CHAMS_TAG "CHAMS"
#define CHAMS_LOGI(...) __android_log_print(ANDROID_LOG_INFO, CHAMS_TAG, __VA_ARGS__)

static void *handle = nullptr;

// ---------- Atomic shared state (no data races between JNI & render thread) ----------
std::atomic<int>  chamsint{3};
std::atomic<bool> g_ChamsEnabled{false};
std::atomic<int>  g_ShaderPreset{0};
std::atomic<bool> enableRainbow{false};
std::atomic<bool> enableRainbowWall{false};

struct ColorPreset { const char* name; Vec4 visible; Vec4 inWall; };
static const ColorPreset kColorPresets[] = {
    { "Dark Green / Sky Blue", {0.0f, 0.5f, 0.0f, 1.0f}, {0.3f, 0.8f, 1.0f, 1.0f} },
    { "Neon Green / Red",      {0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f} },
    { "Cyan / Magenta",        {0.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 1.0f} },
};
static const int kColorPresetCount = sizeof(kColorPresets) / sizeof(kColorPresets[0]);

Vec4 visibleColor = Vec4(0.0f, 0.5f, 0.0f, 1.0f);
Vec4 inWallColor  = Vec4(0.3f, 0.8f, 1.0f, 1.0f);

struct ShaderPreset { const char* name; const char* shader; };
static const ShaderPreset kShaderPresets[] = {
    { "Trains Only (_Displacement)", "_Displacement" },
    { "All Objects (_MainTex)",      "_MainTex" },
    { "Color (_Color)",              "_Color" },
};
static const int kShaderPresetCount = sizeof(kShaderPresets) / sizeof(kShaderPresets[0]);

void setColorPreset(int idx) {
    if (idx >= 0 && idx < kColorPresetCount) {
        visibleColor = kColorPresets[idx].visible;
        inWallColor  = kColorPresets[idx].inWall;
        enableRainbow.store(false);
        enableRainbowWall.store(false);
    }
}
void setShaderPreset(int idx) {
    if (idx >= 0 && idx < kShaderPresetCount) g_ShaderPreset.store(idx);
}

static const char* GetShaderName() {
    int idx = g_ShaderPreset.load();
    if (idx >= 0 && idx < kShaderPresetCount) return kShaderPresets[idx].shader;
    return "_Displacement";
}

void (*p_glGetIntegerv)(unsigned int, int *)            = nullptr;
void (*p_glGetBooleanv)(unsigned int, unsigned char *)  = nullptr;
void (*p_glBlendColor)(float, float, float, float)      = nullptr;
void (*p_glEnable)(unsigned int)                        = nullptr;
void (*p_glLineWidth)(float)                            = nullptr;
void (*p_glBlendFunc)(unsigned int, unsigned int)       = nullptr;
void (*p_glDisable)(unsigned int)                       = nullptr;
void (*p_glCullFace)(unsigned int)                      = nullptr;
void (*p_glDepthFunc)(unsigned int)                     = nullptr;
void (*p_glDepthMask)(unsigned char)                    = nullptr;

int  (*old_glGetUniformLocation)(unsigned int, const char *) = nullptr;
void (*old_glDrawElements)(unsigned int, int, unsigned int, const void *) = nullptr;

// ★ Removed new_glGetUniformLocation entirely — it was a big freeze source:
//   - vector<string> mutated from Unity worker threads → data race
//   - heap allocation on every uniform lookup during shader warmup
//   - collected data was never used anywhere
// We just need the original pointer for the draw hook's per-program cache.

void new_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {
    // FAST PATH — Chams off → zero overhead, no GL state queries
    if (!g_ChamsEnabled.load() || chamsint.load() == 0) {
        old_glDrawElements(mode, count, type, indices);
        return;
    }

    const char* shaderName = GetShaderName();
    // '_Displacement'[1] == 'D', '_MainTex'[1] == 'M', '_Color'[1] == 'C'
    int minCount = (shaderName[1] == 'D') ? 1500 : 3000;
    if (mode != GL_TRIANGLES || count < minCount) {
        old_glDrawElements(mode, count, type, indices);
        return;
    }

    unsigned char blend = 0;
    p_glGetBooleanv(GL_BLEND, &blend);
    if (blend) { old_glDrawElements(mode, count, type, indices); return; }

    unsigned char depthTest = 0;
    p_glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    if (!depthTest) { old_glDrawElements(mode, count, type, indices); return; }

    int currProgram = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);

    static int lastProgram     = -1;
    static int cachedShaderLoc = -1;
    if (currProgram != lastProgram) {
        cachedShaderLoc = old_glGetUniformLocation(currProgram, shaderName);
        lastProgram     = currProgram;
    }
    if (cachedShaderLoc == -1) { old_glDrawElements(mode, count, type, indices); return; }

    if (enableRainbow.load()) {
        visibleColor.x = redd / 255.0f;
        visibleColor.y = greenn / 255.0f;
        visibleColor.z = bluee / 255.0f;
    }
    if (enableRainbowWall.load()) {
        inWallColor.x = redd / 255.0f;
        inWallColor.y = greenn / 255.0f;
        inWallColor.z = bluee / 255.0f;
    }
    performRGBChange();

    // Original pass
    old_glDrawElements(mode, count, type, indices);

    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_ONE, GL_CONSTANT_COLOR);

    int m = chamsint.load();
    if (m == 1) {
        p_glDepthMask(GL_FALSE);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1.0f);
        old_glDrawElements(mode, count, type, indices);
        p_glDepthMask(GL_TRUE);
    }
    else if (m == 2) {
        p_glDepthMask(GL_FALSE);
        p_glDisable(GL_DEPTH_TEST);
        p_glDisable(GL_CULL_FACE);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1.0f);
        old_glDrawElements(mode, count, type, indices);
        p_glEnable(GL_DEPTH_TEST);
        p_glEnable(GL_CULL_FACE);
        p_glDepthMask(GL_TRUE);
    }
    else if (m == 3) {
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(GL_LESS);

        p_glCullFace(GL_FRONT);
        p_glBlendColor(inWallColor.x, inWallColor.y, inWallColor.z, 1.0f);
        old_glDrawElements(mode, count, type, indices);

        p_glCullFace(GL_BACK);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1.0f);
        old_glDrawElements(mode, count, type, indices);
    }

    p_glDisable(GL_BLEND);
    p_glBlendColor(0, 0, 0, 1);
}

void RenderHook() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) {
        CHAMS_LOGI("libGLESv2.so not found — Chams disabled");
        return;
    }

    p_glGetIntegerv = (void (*)(unsigned int, int *))           dlsym(handle, "glGetIntegerv");
    p_glGetBooleanv = (void (*)(unsigned int, unsigned char *)) dlsym(handle, "glGetBooleanv");
    p_glBlendColor  = (void (*)(float, float, float, float))    dlsym(handle, "glBlendColor");
    p_glEnable      = (void (*)(unsigned int))                  dlsym(handle, "glEnable");
    p_glLineWidth   = (void (*)(float))                         dlsym(handle, "glLineWidth");
    p_glBlendFunc   = (void (*)(unsigned int, unsigned int))    dlsym(handle, "glBlendFunc");
    p_glDisable     = (void (*)(unsigned int))                  dlsym(handle, "glDisable");
    p_glCullFace    = (void (*)(unsigned int))                  dlsym(handle, "glCullFace");
    p_glDepthFunc   = (void (*)(unsigned int))                  dlsym(handle, "glDepthFunc");
    p_glDepthMask   = (void (*)(unsigned char))                 dlsym(handle, "glDepthMask");

    // NO hook on glGetUniformLocation — just fetch the original pointer.
    old_glGetUniformLocation = (int (*)(unsigned int, const char *))
                               dlsym(handle, "glGetUniformLocation");

    void *pDraw = dlsym(handle, "glDrawElements");
    if (pDraw && old_glGetUniformLocation) {
        HOOK_FN(pDraw, (void*)new_glDrawElements, (void**)&old_glDrawElements);
        CHAMS_LOGI("glDrawElements hooked OK");
    } else {
        CHAMS_LOGI("Chams hook FAILED (missing symbol)");
    }
}

#endif