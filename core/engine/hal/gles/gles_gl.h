#pragma once
// Dynamically loaded EGL / GLES 3.1 entry points (no link-time dependency: a device without libGLESv3 simply fails to
// create the backend, and the same code runs against ANGLE on the desktop). Types and enums come from the Khronos
// headers (the NDK sysroot ships them; on the desktop the same include directory is used, see hal/gles/CMakeLists.txt).
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES3/gl31.h>
#include <GLES2/gl2ext.h>

namespace dapple::hal::gles {

#define DAPPLE_GL_FUNCS(X)                                                                                              \
  X(glGetString) X(glGetStringi) X(glGetIntegerv) X(glGetIntegeri_v) X(glGetError) X(glFlush) X(glFinish)               \
  X(glCreateShader) X(glShaderSource) X(glCompileShader) X(glGetShaderiv) X(glGetShaderInfoLog) X(glDeleteShader)       \
  X(glCreateProgram) X(glAttachShader) X(glLinkProgram) X(glGetProgramiv) X(glGetProgramInfoLog) X(glDeleteProgram)     \
  X(glUseProgram) X(glGetUniformLocation) X(glUniform4i) X(glUniform2i) X(glUniform3ui) X(glUniform1ui) X(glUniform1i)                                 \
  X(glGetProgramBinary) X(glProgramBinary) X(glProgramParameteri) X(glDetachShader)                                     \
  X(glGenBuffers) X(glDeleteBuffers) X(glBindBuffer) X(glBufferData) X(glBufferSubData) X(glBindBufferBase)             \
  X(glBindBufferRange) X(glMapBufferRange) X(glUnmapBuffer) X(glCopyBufferSubData) X(glGetBufferParameteriv)                                      \
  X(glGenTextures) X(glDeleteTextures) X(glBindTexture) X(glTexStorage2D) X(glTexStorage3D) X(glTexSubImage2D)          \
  X(glTexSubImage3D) X(glActiveTexture) X(glBindImageTexture) X(glPixelStorei)                                          \
  X(glGenSamplers) X(glDeleteSamplers) X(glSamplerParameteri) X(glBindSampler)                                          \
  X(glDispatchCompute) X(glMemoryBarrier) X(glFenceSync) X(glClientWaitSync) X(glDeleteSync)

struct GlFuncs {
#define X(n) decltype(&::n) n = nullptr;
  DAPPLE_GL_FUNCS(X)
#undef X
};

struct EglFuncs {
  PFNEGLGETPROCADDRESSPROC getProcAddress = nullptr;
  PFNEGLGETDISPLAYPROC getDisplay = nullptr;
  PFNEGLGETPLATFORMDISPLAYEXTPROC getPlatformDisplayEXT = nullptr;
  PFNEGLINITIALIZEPROC initialize = nullptr;
  PFNEGLTERMINATEPROC terminate = nullptr;
  PFNEGLCHOOSECONFIGPROC chooseConfig = nullptr;
  PFNEGLCREATECONTEXTPROC createContext = nullptr;
  PFNEGLDESTROYCONTEXTPROC destroyContext = nullptr;
  PFNEGLCREATEPBUFFERSURFACEPROC createPbufferSurface = nullptr;
  PFNEGLDESTROYSURFACEPROC destroySurface = nullptr;
  PFNEGLMAKECURRENTPROC makeCurrent = nullptr;
  PFNEGLQUERYSTRINGPROC queryString = nullptr;
  PFNEGLBINDAPIPROC bindAPI = nullptr;
  PFNEGLGETERRORPROC getError = nullptr;
};

}  // namespace dapple::hal::gles
