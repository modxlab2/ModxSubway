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

int chamsint = 3; // Default: Hybrid Professional
bool g_ChamsEnabled = false;
std::string mineShader = "_Displacement"; 

// ================================================================
//  Color Presets (Dark Green / Light Sky Blue Default)
// ================================================================
struct ColorPreset { const char* name; Vec4 visible; Vec4 inWall; };
static const ColorPreset kColorPresets[] = {
    { "Dark Green / Sky Blue", {0.00f, 0.45f, 0.00f, 1.0f}, {0.40f, 0.75f, 1.00f, 1.0f} },
    { "Neon Green / Red",      {0.00f, 1.00f, 0.30f, 1.0f}, {1.00f, 0.10f, 0.10f, 1.0f} },
    { "Cyan / Magenta",        {0.00f, 1.00f, 1.00f, 1.0f}, {1.00f, 0.00f, 0.60f, 1.0f} },
    { "Yellow / Blue",         {1.00f, 0.90f, 0.10f, 1.0f}, {0.15f, 0.40f, 1.00f, 1.0f} },
};
static const int kColorPresetCount = sizeof(kColorPresets) / sizeof(kColorPresets[0]);

bool enableRainbow = false, enableRainbowWall = false;
Vec4 visibleColor = Vec4(0.00f, 0.45f, 0.00f, 1.0f); // Dark Green
Vec4 inWallColor  = Vec4(0.40f, 0.75f, 1.00f, 1.0f); // Light Sky Blue

// ================================================================
//  Shader Presets
// ================================================================
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
        enableRainbow = enableRainbowWall = false;
    }
}
void setShaderPreset(int idx) {
    if (idx >= 0 && idx < kShaderPresetCount) mineShader = kShaderPresets[idx].shader;
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
// ★ Added new pointers for Character Fix
void (*p_glDepthFunc)(unsigned int)                     = nullptr; 
void (*p_glDepthMask)(unsigned char)                    = nullptr;

const char *isDumpLocation = "/sdcard/Documents/Shaders.txt";
void dumpShadersToFile() {
    std::ofstream f(isDumpLocation, std::ios::out | std::ios::trunc);
    if (f.is_open()) {
        for (const auto& s : shaders) f << s << "\n";
        f.close();
    }
}

int (*old_glGetUniformLocation)(unsigned int, const char *) = nullptr;
int new_glGetUniformLocation(unsigned int program, const char *name) {
    if (name) {
        bool isExist = false;
        std::string thisShader = name;
        for (const auto& s : shaders) if (s == thisShader) { isExist = true; break; }
        if (!isExist) shaders.push_back(thisShader);
    }
    return old_glGetUniformLocation(program, name);
}

// ================================================================
//  glDrawElements hook — Professional 3D & Character Fix
// ================================================================
void (*old_glDrawElements)(unsigned int, int, unsigned int, const void *) = nullptr;
void new_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {

    // ★ Character Fix: Skip small meshes (Character is usually < 1500 vertices)
    int minCount = (mineShader == "_Displacement") ? 1500 : 3000;
    
    if (mode != GL_TRIANGLES || count < minCount) {
        return old_glDrawElements(mode, count, type, indices);
    }

    unsigned char blend = 0;
    p_glGetBooleanv(GL_BLEND, &blend);
    if (blend) return old_glDrawElements(mode, count, type, indices);

    unsigned char depthTest = 0;
    p_glGetBooleanv(GL_DEPTH_TEST, &depthTest);
    if (!depthTest) return old_glDrawElements(mode, count, type, indices);

    int currProgram = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
    int shader = old_glGetUniformLocation(currProgram, mineShader.c_str());
    
    if (shader == -1 || !g_ChamsEnabled || chamsint == 0) {
        return old_glDrawElements(mode, count, type, indices);
    }

    if (enableRainbow) {
        visibleColor.x = redd / 255.0f; visibleColor.y = greenn / 255.0f; visibleColor.z = bluee / 255.0f;
    }
    if (enableRainbowWall) {
        inWallColor.x = redd / 255.0f; inWallColor.y = greenn / 255.0f; inWallColor.z = bluee / 255.0f;
    }
    performRGBChange();

    // 1. Draw original (Game rendering)
    old_glDrawElements(mode, count, type, indices);

    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // Mode 1: Solid Visible (Only front walls)
    if (chamsint == 1) {
        p_glDepthMask(GL_FALSE); // Don't overwrite character depth
        p_glDepthFunc(GL_LESS);
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 0.8f);
        old_glDrawElements(mode, count, type, indices);
        p_glDepthMask(GL_TRUE);
        p_glDepthFunc(GL_LEQUAL);
    }
    // Mode 2: X-Ray (See through everything)
    else if (chamsint == 2) {
        p_glDisable(GL_DEPTH_TEST);
        p_glDisable(GL_CULL_FACE);
        p_glBlendColor(inWallColor.x, inWallColor.y, inWallColor.z, 0.5f);
        old_glDrawElements(mode, count, type, indices);
        p_glEnable(GL_DEPTH_TEST);
        p_glEnable(GL_CULL_FACE);
    }
    // Mode 3: Hybrid Professional (Dark Green Outer + Sky Blue Inner)
    else if (chamsint == 3) {
        // 1. Draw inner walls (Sky Blue)
        p_glDisable(GL_DEPTH_TEST);
        p_glDisable(GL_CULL_FACE); 
        p_glBlendColor(inWallColor.x, inWallColor.y, inWallColor.z, 0.6f);
        old_glDrawElements(mode, count, type, indices);

        // 2. Draw outer visible walls (Dark Green)
        p_glEnable(GL_DEPTH_TEST);
        p_glDepthFunc(GL_LESS);      // Only draw if strictly in front
        p_glDepthMask(GL_FALSE);     // ★ Don't block the character!
        
        p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1.0f);
        old_glDrawElements(mode, count, type, indices);
        
        // Restore
        p_glDepthMask(GL_TRUE);
        p_glDepthFunc(GL_LEQUAL);
        p_glEnable(GL_CULL_FACE);
    }

    // Restore GL state
    p_glDisable(GL_BLEND);
    p_glBlendColor(0, 0, 0, 1);
}

// ================================================================
//  Setup hooks
// ================================================================
void RenderHook() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) return;

    p_glGetIntegerv  = (void (*)(unsigned int, int *))            dlsym(handle, "glGetIntegerv");
    p_glGetBooleanv  = (void (*)(unsigned int, unsigned char *))  dlsym(handle, "glGetBooleanv");
    p_glDepthRangef  = (void (*)(float, float))                   dlsym(handle, "glDepthRangef");
    p_glBlendColor   = (void (*)(float, float, float, float))     dlsym(handle, "glBlendColor");
    p_glEnable       = (void (*)(unsigned int))                   dlsym(handle, "glEnable");
    p_glLineWidth    = (void (*)(float))                          dlsym(handle, "glLineWidth");
    p_glBlendFunc    = (void (*)(unsigned int, unsigned int))     dlsym(handle, "glBlendFunc");
    p_glDisable      = (void (*)(unsigned int))                   dlsym(handle, "glDisable");
    p_glCullFace     = (void (*)(unsigned int))                   dlsym(handle, "glCullFace");
    
    // Load new pointers
    p_glDepthFunc    = (void (*)(unsigned int))                   dlsym(handle, "glDepthFunc");
    p_glDepthMask    = (void (*)(unsigned char))                  dlsym(handle, "glDepthMask");

    void *pGet = dlsym(handle, "glGetUniformLocation");
    if (pGet) HOOK_FN(pGet, (void*)new_glGetUniformLocation, (void**)&old_glGetUniformLocation);
    
    void *pDraw = dlsym(handle, "glDrawElements");
    if (pDraw) HOOK_FN(pDraw, (void*)new_glDrawElements, (void**)&old_glDrawElements);
}

#endif