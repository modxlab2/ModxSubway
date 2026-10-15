#ifndef NORKA_CHAMS
#define NORKA_CHAMS

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>

// ★ আপনার project-এ যেভাবে আছে সেই path দিন
#include "And64InlineHook/And64InlineHook.hpp"
#include "RGB.h"

static void *handle;
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

float red = 0.0f;
float gren = 0.0f;
float blue = 0.0f;
float mi = 0.0f;

inline void setShader(const char* s) {
    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    shaderName = s;
}
inline const char* getShader() { return shaderName; }

inline void SetWallhack(bool enable) { enableWallhack = enable; }
inline void SetWireframe(bool enable) { enableWallhackW = enable; }
inline void SetGlow(bool enable) { enableWallhackG = enable; }
inline void SetOutline(bool enable) { enableWallhackO = enable; }
inline void SetRainbow(bool enable) { enableRainbow = enable; }
inline void SetRGB(bool enable) { isRGB = enable; }

inline void SetR(int set) { r = set; }
inline void SetG(int set) { g = set; }
inline void SetB(int set) { b = set; }
inline void SetWireframeWidth(int set) { w1 = set; }
inline void SetGlowWidth(int set) { w2 = set; }
inline void SetOutlineWidth(int set) { w3 = set; }

inline bool getWallhackEnabled() { return enableWallhack; }
inline bool getWireframeEnabled() { return enableWallhackW; }
inline bool getGlowEnabled() { return enableWallhackG; }
inline bool getOutlineEnabled() { return enableWallhackO; }
inline bool getRainbowEnabled() { return enableRainbow; }
inline bool getRGB() { return isRGB; }

// ---------------------------------------------------------------
//  Hook: glGetUniformLocation
// ---------------------------------------------------------------
static int (*old_glGetUniformLocation)(GLuint, const GLchar *) = nullptr;
static GLint new_glGetUniformLocation(GLuint program, const GLchar *name) {
    if (!old_glGetUniformLocation) return -1;
    return old_glGetUniformLocation(program, name);
}

// ---------------------------------------------------------------
//  Hook: glDrawElements
// ---------------------------------------------------------------
static void (*old_glDrawElements)(GLenum mode, GLsizei count, GLenum type, const void *indices) = nullptr;
static void new_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {
    if (!old_glDrawElements) return;

    old_glDrawElements(mode, count, type, indices);
    if (mode != GL_TRIANGLES || count < 500) return;

    GLint currProgram;
    glGetIntegerv(GL_CURRENT_PROGRAM, &currProgram);
    GLint id = old_glGetUniformLocation(currProgram, getShader());
    if (id == -1) return;

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

// ---------------------------------------------------------------
//  Init
// ---------------------------------------------------------------
inline bool mlovinit() {
    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    return handle != NULL;
}

inline void LogShaders() {
    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    auto p = (const void*(*)(...))dlsym(handle, "glGetUniformLocation");
    if (!dlerror()) {
        A64HookFunction(reinterpret_cast<void*>(p),
                        reinterpret_cast<void*>(new_glGetUniformLocation),
                        reinterpret_cast<void**>(&old_glGetUniformLocation));
    }
}

inline void Wallhack() {
    handle = dlopen("libGLESv2.so", RTLD_LAZY);
    auto p = (const void*(*)(...))dlsym(handle, "glDrawElements");
    if (!dlerror()) {
        A64HookFunction(reinterpret_cast<void*>(p),
                        reinterpret_cast<void*>(new_glDrawElements),
                        reinterpret_cast<void**>(&old_glDrawElements));
    }
}

#endif