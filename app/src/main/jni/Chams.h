#ifndef ZEFF_CHAMS
#define ZEFF_CHAMS

#include <dlfcn.h>
#include <array>
#include <fstream>
#include <sstream>
#include <vector>
#include <iostream>
#include <string>
#include <cstring>
#include <ctime>
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

// ================================================================
//  Global state
// ================================================================
static void *handle = nullptr;
std::vector<std::string> shaders;
std::string mineShader = "_Displacement";

// ★ Default = 4 (Full Silhouette — best for train)
int chamsint = 4;

// ================================================================
//  ★ Color Presets — high contrast for train visibility
// ================================================================
struct ColorPreset {
    const char* name;
    Vec4 visible;
    Vec4 inWall;
};
static const ColorPreset kColorPresets[] = {
    { "Neon Green / Red",   {0.00f, 1.00f, 0.30f, 1.0f}, {1.00f, 0.10f, 0.10f, 1.0f} },
    { "Cyan / Magenta",     {0.00f, 1.00f, 1.00f, 1.0f}, {1.00f, 0.00f, 0.60f, 1.0f} },
    { "Yellow / Blue",      {1.00f, 0.90f, 0.10f, 1.0f}, {0.15f, 0.40f, 1.00f, 1.0f} },
    { "Orange / Purple",    {1.00f, 0.55f, 0.00f, 1.0f}, {0.70f, 0.20f, 1.00f, 1.0f} },
    { "White / Red",        {1.00f, 1.00f, 1.00f, 1.0f}, {1.00f, 0.00f, 0.00f, 1.0f} },
    { "Stealth (Gray/Red)", {0.15f, 0.15f, 0.15f, 1.0f}, {1.00f, 0.00f, 0.00f, 1.0f} },
};
static const int kColorPresetCount = sizeof(kColorPresets) / sizeof(kColorPresets[0]);

bool enableRainbow     = false;
bool enableRainbowWall = false;

// ★ Default = Neon Green/Red (preset 0)
Vec4 visibleColor = Vec4(0.00f, 1.00f, 0.30f, 1.0f);
Vec4 inWallColor  = Vec4(1.00f, 0.10f, 0.10f, 1.0f);

void setColorPreset(int idx) {
    if (idx < 0 || idx >= kColorPresetCount) return;
    visibleColor = kColorPresets[idx].visible;
    inWallColor  = kColorPresets[idx].inWall;
    enableRainbow     = false;
    enableRainbowWall = false;
    CHAMS_LOGI("Color preset -> %s", kColorPresets[idx].name);
}

// ================================================================
//  GL function pointers
// ================================================================
void (*p_glGetIntegerv)(unsigned int, int *)            = nullptr;
void (*p_glGetBooleanv)(unsigned int, unsigned char *)  = nullptr;
void (*p_glDepthRangef)(float, float)                   = nullptr;
void (*p_glBlendColor)(float, float, float, float)      = nullptr;
void (*p_glEnable)(unsigned int)                        = nullptr;
void (*p_glLineWidth)(float)                            = nullptr;
void (*p_glBlendFunc)(unsigned int, unsigned int)       = nullptr;
void (*p_glDisable)(unsigned int)                       = nullptr;
void (*p_glCullFace)(unsigned int)                      = nullptr;

const char *isDumpLocation = "/sdcard/Documents/Shaders.txt";

void dumpShadersToFile() {
    std::ofstream f(isDumpLocation, std::ios::out | std::ios::trunc);
    if (!f.is_open()) { CHAMS_LOGI("Failed to open %s", isDumpLocation); return; }
    time_t now = time(nullptr);
    char tb[64];
    strftime(tb, sizeof(tb), "%Y-%m-%d %H:%M:%S", localtime(&now));
    f << "=== Shader List @ " << tb << " ===\n";
    f << "Total: " << shaders.size() << " shaders\n\n";
    for (const auto& s : shaders) f << s << "\n";
    f.close();
    CHAMS_LOGI("Dumped %zu shaders to %s", shaders.size(), isDumpLocation);
}

// ================================================================
//  glGetUniformLocation hook
// ================================================================
int (*old_glGetUniformLocation)(unsigned int, const char *) = nullptr;
int new_glGetUniformLocation(unsigned int program, const char *name) {
    if (name) {
        bool isExist = false;
        std::string thisShader = name;
        for (const auto& s : shaders) {
            if (s == thisShader) { isExist = true; break; }
        }
        if (!isExist) shaders.push_back(thisShader);
    }
    return old_glGetUniformLocation(program, name);
}

// ================================================================
//  glDrawElements hook
// ================================================================
void (*old_glDrawElements)(unsigned int, int, unsigned int, const void *) = nullptr;
void new_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {

    if (mode != GL_TRIANGLES)              return old_glDrawElements(mode, count, type, indices);
    if (count < 2000)                       return old_glDrawElements(mode, count, type, indices);

    unsigned char blend = 0;
    p_glGetBooleanv(GL_BLEND, &blend);
    if (blend)                              return old_glDrawElements(mode, count, type, indices);

    unsigned char depthTest = 0;
    p_glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    if (!depthTest)                         return old_glDrawElements(mode, count, type, indices);

    int currProgram = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
    int shader = old_glGetUniformLocation(currProgram, mineShader.c_str());
    if (shader == -1)                       return old_glDrawElements(mode, count, type, indices);

    if (chamsint == 0)                      return old_glDrawElements(mode, count, type, indices);

    // RGB animation
    if (enableRainbow) {
        visibleColor.x = redd / 255.0f;
        visibleColor.y = greenn / 255.0f;
        visibleColor.z = bluee / 255.0f;
    }
    if (enableRainbowWall) {
        inWallColor.x = redd / 255.0f;
        inWallColor.y = greenn / 255.0f;
        inWallColor.z = bluee / 255.0f;
    }
    performRGBChange();

    // ============================================================
    //  Mode 1 — Outline  (black shell + colored edge, clean)
    // ============================================================
    if (chamsint == 1) {
        old_glDrawElements(mode, count, type, indices);
        p_glDepthRangef(1.0f, 0.0f);
        p_glLineWidth(8);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA);
        p_glBlendColor(0, 0, 0, 1);
        old_glDrawElements(mode, count, type, indices);
        p_glDepthRangef(1.0f, 0.5f);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1);
        old_glDrawElements(GL_LINES, count, type, indices);
    }

    // ============================================================
    //  Mode 2 — X-Ray Solid  (through-wall fill, all faces)
    // ============================================================
    if (chamsint == 2) {
        p_glDisable(GL_CULL_FACE);
        p_glDisable(GL_DEPTH_TEST);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_ONE, GL_CONSTANT_COLOR);
        p_glBlendColor(inWallColor.x, inWallColor.y, inWallColor.z, 1);
        old_glDrawElements(mode, count, type, indices);
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthRangef(0.5f, 1.0f);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1);
        old_glDrawElements(mode, count, type, indices);
        p_glEnable(GL_CULL_FACE);
    }

    // ============================================================
    //  Mode 3 — Wireframe  (all edges, cull disabled)
    // ============================================================
    if (chamsint == 3) {
        old_glDrawElements(mode, count, type, indices);
        p_glDisable(GL_CULL_FACE);
        p_glDepthRangef(1.0f, 0.0f);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1);
        p_glLineWidth(3);
        old_glDrawElements(GL_LINES, count, type, indices);
        p_glEnable(GL_CULL_FACE);
    }

    // ============================================================
    //  ★ Mode 4 — Full Silhouette  (BEST for train visibility)
    //     • Cull disabled → see ALL faces (front, side, back, top)
    //     • Colored fill overlay → entire train painted
    //     • Black wireframe outline → crisp start/end walls
    // ============================================================
    if (chamsint == 4) {
        // Pass 1: original render (so game looks normal underneath)
        old_glDrawElements(mode, count, type, indices);

        // Pass 2: Colored fill on ALL faces (cull disabled)
        p_glDisable(GL_CULL_FACE);
        p_glEnable(GL_BLEND);
        p_glBlendFunc(GL_ONE, GL_CONSTANT_COLOR);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1);
        p_glDepthRangef(1.0f, 0.5f);
        old_glDrawElements(mode, count, type, indices);

        // Pass 3: Black wireframe outline (all edges visible)
        p_glBlendFunc(GL_CONSTANT_COLOR, GL_CONSTANT_ALPHA);
        p_glBlendColor(0, 0, 0, 1);
        p_glLineWidth(4);
        p_glDepthRangef(0.5f, 0.0f);
        old_glDrawElements(GL_LINES, count, type, indices);

        p_glEnable(GL_CULL_FACE);
    }

    // Cleanup
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    p_glLineWidth(1);
    p_glBlendColor(0, 0, 0, 1);
    p_glDepthRangef(0, 1);
    p_glDisable(GL_BLEND);
}

// ================================================================
//  Setup hooks
// ================================================================
void RenderHook() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) { CHAMS_LOGI("dlopen libGLESv2.so FAILED"); return; }
    CHAMS_LOGI("libGLESv2.so loaded OK");

    p_glGetIntegerv  = (void (*)(unsigned int, int *))            dlsym(handle, "glGetIntegerv");
    p_glGetBooleanv  = (void (*)(unsigned int, unsigned char *))  dlsym(handle, "glGetBooleanv");
    p_glDepthRangef  = (void (*)(float, float))                   dlsym(handle, "glDepthRangef");
    p_glBlendColor   = (void (*)(float, float, float, float))     dlsym(handle, "glBlendColor");
    p_glEnable       = (void (*)(unsigned int))                   dlsym(handle, "glEnable");
    p_glLineWidth    = (void (*)(float))                          dlsym(handle, "glLineWidth");
    p_glBlendFunc    = (void (*)(unsigned int, unsigned int))     dlsym(handle, "glBlendFunc");
    p_glDisable      = (void (*)(unsigned int))                   dlsym(handle, "glDisable");
    p_glCullFace     = (void (*)(unsigned int))                   dlsym(handle, "glCullFace");

    void *pGet = dlsym(handle, "glGetUniformLocation");
    if (pGet) {
        HOOK_FN(pGet, (void*)new_glGetUniformLocation, (void**)&old_glGetUniformLocation);
        CHAMS_LOGI("glGetUniformLocation hooked");
    }
    void *pDraw = dlsym(handle, "glDrawElements");
    if (pDraw) {
        HOOK_FN(pDraw, (void*)new_glDrawElements, (void**)&old_glDrawElements);
        CHAMS_LOGI("glDrawElements hooked");
    }
    CHAMS_LOGI("Chams hooks installed — shader=%s, default mode=%d", mineShader.c_str(), chamsint);
}

#endif