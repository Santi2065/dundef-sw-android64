#include <EGL/egl.h>
#include <sys/system_properties.h>
#include <GLES2/gl2.h>

#include <mutex>
#include <unordered_map>
#include <vector>

#include "core.h"

namespace {
// EGL handles are host pointers; the guest gets small integers instead.
std::mutex egl_lock;
std::vector<void*> egl_objs{nullptr};
std::unordered_map<void*, u32> egl_ids;

u32 to_g(void* p) {
    if (!p) return 0;
    std::lock_guard lk(egl_lock);
    auto [it, fresh] = egl_ids.try_emplace(p, egl_objs.size());
    if (fresh) egl_objs.push_back(p);
    return it->second;
}
void* to_h(u32 id) {
    std::lock_guard lk(egl_lock);
    return id < egl_objs.size() ? egl_objs[id] : nullptr;
}

// host strings handed to the guest are copied once and kept forever
u32 guest_str(const void* key, const char* s) {
    static std::mutex m;
    static std::unordered_map<std::string, u32> cache;
    if (!s) return 0;
    std::lock_guard lk(m);
    auto& slot = cache[std::to_string(reinterpret_cast<uintptr_t>(key)) + s];
    if (!slot) slot = mem::strdup(s);
    return slot;
}
}  // namespace

void init_gl() {
    IMPORT(glActiveTexture); IMPORT(glAttachShader); IMPORT(glBindAttribLocation); IMPORT(glBindBuffer);
    IMPORT(glBindFramebuffer); IMPORT(glBindRenderbuffer); IMPORT(glBindTexture); IMPORT(glBlendEquationSeparate);
    IMPORT(glBlendFuncSeparate); IMPORT(glBufferData); IMPORT(glCheckFramebufferStatus);
    IMPORT(glClearColor); IMPORT(glClearDepthf); IMPORT(glClearStencil); IMPORT(glColorMask); IMPORT(glCompileShader);
    IMPORT(glCompressedTexImage2D); IMPORT(glCreateProgram); IMPORT(glCreateShader); IMPORT(glDeleteBuffers);
    IMPORT(glDeleteFramebuffers); IMPORT(glDeleteProgram); IMPORT(glDeleteRenderbuffers); IMPORT(glDeleteShader);
    IMPORT(glDeleteTextures); IMPORT(glDepthFunc); IMPORT(glDepthMask); IMPORT(glDepthRangef); IMPORT(glDisable);
    IMPORT(glDisableVertexAttribArray); IMPORT(glDrawArrays); IMPORT(glDrawElements); IMPORT(glEnable);
    IMPORT(glEnableVertexAttribArray); IMPORT(glFinish); IMPORT(glFramebufferRenderbuffer);
    IMPORT(glFramebufferTexture2D); IMPORT(glFrontFace); IMPORT(glGenBuffers); IMPORT(glGenFramebuffers);
    IMPORT(glGenRenderbuffers); IMPORT(glGenTextures); IMPORT(glGetActiveUniform); IMPORT(glGetAttribLocation);
    IMPORT(glGetBooleanv); IMPORT(glGetError); IMPORT(glGetIntegerv); IMPORT(glGetProgramInfoLog);
    IMPORT(glGetProgramiv); IMPORT(glGetShaderInfoLog); IMPORT(glGetShaderiv); IMPORT(glGetUniformLocation);
    IMPORT(glIsShader); IMPORT(glLinkProgram); IMPORT(glPixelStorei); IMPORT(glReadPixels);
    IMPORT(glRenderbufferStorage); IMPORT(glScissor); IMPORT(glStencilFunc); IMPORT(glStencilMask);
    IMPORT(glStencilOp); IMPORT(glTexImage2D); IMPORT(glTexParameterf); IMPORT(glTexParameteri);
    IMPORT(glUniform1fv); IMPORT(glUniform1i); IMPORT(glUniform2f); IMPORT(glUniform3fv); IMPORT(glUniform4fv);
    IMPORT(glUniformMatrix4fv); IMPORT(glUseProgram); IMPORT(glValidateProgram); IMPORT(glVertexAttribPointer);
    IMPORT(glViewport);

    // testing aid: `adb shell setprop debug.ddport.novsync 1` lets frames run past the display's vsync
    register_import("glClear", [](Ctx& c) {
        static thread_local bool checked;
        if (!checked) {
            checked = true;
            char v[PROP_VALUE_MAX] = "";
            __system_property_get("debug.ddport.novsync", v);
            if (v[0] == '1') eglSwapInterval(eglGetCurrentDisplay(), 0);
        }
        glClear(c.arg(0));
    });
    register_import("glGetString", [](Ctx& c) {
        c.ret(guest_str(nullptr, reinterpret_cast<const char*>(glGetString(c.arg(0)))));
    });
    register_import("glShaderSource", [](Ctx& c) {
        u32 n = c.arg(1);
        std::vector<const char*> src(n);
        for (u32 i = 0; i < n; i++) src[i] = mem::h<char>(mem::rd32(c.arg(2) + 4 * i));
        glShaderSource(c.arg(0), n, src.data(), mem::h<GLint>(c.arg(3)));
    });

    // --- EGL (the game normally drives EGL from Java; kept for completeness)
    register_import("eglGetDisplay", [](Ctx& c) { c.ret(to_g(eglGetDisplay(EGL_DEFAULT_DISPLAY))); });
    register_import("eglInitialize", [](Ctx& c) {
        c.ret(eglInitialize(to_h(c.arg(0)), mem::h<EGLint>(c.arg(1)), mem::h<EGLint>(c.arg(2))));
    });
    register_import("eglTerminate", [](Ctx& c) { c.ret(eglTerminate(to_h(c.arg(0)))); });
    register_import("eglGetError", [](Ctx& c) { c.ret(eglGetError()); });
    register_import("eglBindAPI", [](Ctx& c) { c.ret(eglBindAPI(c.arg(0))); });
    register_import("eglReleaseThread", [](Ctx& c) { c.ret(eglReleaseThread()); });
    register_import("eglQueryString", [](Ctx& c) {
        c.ret(guest_str(reinterpret_cast<void*>(1), eglQueryString(to_h(c.arg(0)), c.arg(1))));
    });
    register_import("eglChooseConfig", [](Ctx& c) {
        EGLint size = c.arg(3);
        std::vector<EGLConfig> cfgs(size > 0 ? size : 0);
        EGLint num = 0;
        EGLBoolean r = eglChooseConfig(to_h(c.arg(0)), mem::h<EGLint>(c.arg(1)), c.arg(2) ? cfgs.data() : nullptr,
                                       size, &num);
        for (EGLint i = 0; c.arg(2) && i < num; i++) mem::wr32(c.arg(2) + 4 * i, to_g(cfgs[i]));
        if (c.arg(4)) mem::wr32(c.arg(4), num);
        c.ret(r);
    });
    register_import("eglGetConfigAttrib", [](Ctx& c) {
        c.ret(eglGetConfigAttrib(to_h(c.arg(0)), to_h(c.arg(1)), c.arg(2), mem::h<EGLint>(c.arg(3))));
    });
    register_import("eglCreateContext", [](Ctx& c) {
        c.ret(to_g(eglCreateContext(to_h(c.arg(0)), to_h(c.arg(1)), to_h(c.arg(2)), mem::h<EGLint>(c.arg(3)))));
    });
    register_import("eglCreateWindowSurface", [](Ctx& c) {
        c.ret(to_g(eglCreateWindowSurface(to_h(c.arg(0)), to_h(c.arg(1)),
                                          reinterpret_cast<EGLNativeWindowType>(mem::h(c.arg(2))),
                                          mem::h<EGLint>(c.arg(3)))));
    });
    register_import("eglDestroyContext", [](Ctx& c) { c.ret(eglDestroyContext(to_h(c.arg(0)), to_h(c.arg(1)))); });
    register_import("eglDestroySurface", [](Ctx& c) { c.ret(eglDestroySurface(to_h(c.arg(0)), to_h(c.arg(1)))); });
    register_import("eglMakeCurrent", [](Ctx& c) {
        c.ret(eglMakeCurrent(to_h(c.arg(0)), to_h(c.arg(1)), to_h(c.arg(2)), to_h(c.arg(3))));
    });
    register_import("eglSwapBuffers", [](Ctx& c) { c.ret(eglSwapBuffers(to_h(c.arg(0)), to_h(c.arg(1)))); });
    register_import("eglQuerySurface", [](Ctx& c) {
        c.ret(eglQuerySurface(to_h(c.arg(0)), to_h(c.arg(1)), c.arg(2), mem::h<EGLint>(c.arg(3))));
    });
}
