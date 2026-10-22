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
#include <chrono>
#include <android/log.h>
#include <GLES2/gl2.h>
#include <math.h>
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

bool g_ChamsEnabled = false;
std::string mineShader = "_Displacement"; // Default: Trains Only

// ================================================================
//  Color Presets
// ================================================================
struct ColorPreset { const char* name; Vec4 visible; Vec4 inWall; };
static const ColorPreset kColorPresets[] = {
    { "Dark Green / Sky Blue", {0.0f, 0.5f, 0.0f, 1.0f}, {0.3f, 0.8f, 1.0f, 1.0f} },
    { "Neon Green / Red",      {0.0f, 1.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f} },
    { "Cyan / Magenta",        {0.0f, 1.0f, 1.0f, 1.0f}, {1.0f, 0.0f, 1.0f, 1.0f} },
};
static const int kColorPresetCount = sizeof(kColorPresets) / sizeof(kColorPresets[0]);

bool enableRainbow = false;
Vec4 visibleColor = Vec4(0.0f, 0.5f, 0.0f, 1.0f);
Vec4 inWallColor  = Vec4(0.3f, 0.8f, 1.0f, 1.0f);

// ================================================================
//  Shader Presets
// ================================================================
struct ShaderPreset { const char* name; const char* shader; };
static const ShaderPreset kShaderPresets[] = {
    { "Trains Only (_Displacement)", "_Displacement" },
    { "All Objects (_MainTex)",      "_MainTex" },
};
static const int kShaderPresetCount = sizeof(kShaderPresets) / sizeof(kShaderPresets[0]);

void setColorPreset(int idx) {
    if (idx >= 0 && idx < kColorPresetCount) {
        visibleColor = kColorPresets[idx].visible;
        inWallColor  = kColorPresets[idx].inWall;
        enableRainbow = false;
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
void (*p_glBlendColor)(float, float, float, float)      = nullptr;
void (*p_glEnable)(unsigned int)                        = nullptr;
void (*p_glLineWidth)(float)                            = nullptr;
void (*p_glBlendFunc)(unsigned int, unsigned int)       = nullptr;
void (*p_glDisable)(unsigned int)                       = nullptr;
void (*p_glCullFace)(unsigned int)                      = nullptr;
void (*p_glDepthFunc)(unsigned int)                     = nullptr; 
void (*p_glDepthMask)(unsigned char)                    = nullptr;
void (*p_glGetUniformfv)(unsigned int, int, float*)     = nullptr;

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
//  glDrawElements hook
// ================================================================
void (*old_glDrawElements)(unsigned int, int, unsigned int, const void *) = nullptr;
void new_glDrawElements(unsigned int mode, int count, unsigned int type, const void *indices) {

    // minCount 100 করা হয়েছে যাতে ট্রেনের ছোট অংশ বাদ না পড়ে
    if (mode != GL_TRIANGLES || count < 100) {
        return old_glDrawElements(mode, count, type, indices);
    }

    unsigned char blend = 0;
    p_glGetBooleanv(GL_BLEND, &blend);
    if (blend) return old_glDrawElements(mode, count, type, indices);

    if (!g_ChamsEnabled) {
        return old_glDrawElements(mode, count, type, indices);
    }

    int currProgram = 0;
    p_glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
    
    // Strict Shader Check (কোনো Fallback নেই, তাই Trains Only ঠিকমতো কাজ করবে)
    int shader = old_glGetUniformLocation(currProgram, mineShader.c_str());
    if (shader == -1) {
        return old_glDrawElements(mode, count, type, indices);
    }

    if (enableRainbow) {
        visibleColor.x = redd / 255.0f; visibleColor.y = greenn / 255.0f; visibleColor.z = bluee / 255.0f;
        inWallColor.x = redd / 255.0f; inWallColor.y = greenn / 255.0f; inWallColor.z = bluee / 255.0f;
    }
    performRGBChange();

    // Draw original game object first
    old_glDrawElements(mode, count, type, indices);

    // ============================================================
    // Professional Chams Mode (Only Mode)
    // ============================================================
    p_glEnable(GL_BLEND);
    p_glBlendFunc(GL_ONE, GL_CONSTANT_COLOR); 
    p_glEnable(GL_DEPTH_TEST);
    p_glDepthFunc(GL_LEQUAL); 
    p_glDepthMask(GL_FALSE); 
    
    // 1. Draw inner walls (Sky Blue / InWall Color)
    p_glCullFace(GL_FRONT); 
    p_glBlendColor(inWallColor.x, inWallColor.y, inWallColor.z, 1.0f);
    old_glDrawElements(mode, count, type, indices);

    // 2. Draw outer walls (Dark Green / Visible Color)
    p_glCullFace(GL_BACK); 
    p_glBlendColor(visibleColor.x, visibleColor.y, visibleColor.z, 1.0f);
    old_glDrawElements(mode, count, type, indices);
    
    p_glDepthMask(GL_TRUE);
    p_glDisable(GL_BLEND);
    p_glBlendColor(0, 0, 0, 1);
}

void RenderHook() {
    if (!handle) handle = dlopen("libGLESv2.so", RTLD_LAZY);
    if (!handle) return;

    p_glGetIntegerv  = (void (*)(unsigned int, int *))            dlsym(handle, "glGetIntegerv");
    p_glGetBooleanv  = (void (*)(unsigned int, unsigned char *))  dlsym(handle, "glGetBooleanv");
    p_glBlendColor   = (void (*)(float, float, float, float))     dlsym(handle, "glBlendColor");
    p_glEnable       = (void (*)(unsigned int))                   dlsym(handle, "glEnable");
    p_glLineWidth    = (void (*)(float))                          dlsym(handle, "glLineWidth");
    p_glBlendFunc    = (void (*)(unsigned int, unsigned int))     dlsym(handle, "glBlendFunc");
    p_glDisable      = (void (*)(unsigned int))                   dlsym(handle, "glDisable");
    p_glCullFace     = (void (*)(unsigned int))                   dlsym(handle, "glCullFace");
    
    p_glDepthFunc    = (void (*)(unsigned int))                   dlsym(handle, "glDepthFunc");
    p_glDepthMask    = (void (*)(unsigned char))                  dlsym(handle, "glDepthMask");
    p_glGetUniformfv = (void (*)(unsigned int, int, float*))      dlsym(handle, "glGetUniformfv");

    void *pGet = dlsym(handle, "glGetUniformLocation");
    if (pGet) HOOK_FN(pGet, (void*)new_glGetUniformLocation, (void**)&old_glGetUniformLocation);
    
    void *pDraw = dlsym(handle, "glDrawElements");
    if (pDraw) HOOK_FN(pDraw, (void*)new_glDrawElements, (void**)&old_glDrawElements);
}

#endif