// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once

#include <gl/GL.h>

// This header contains just the small subset of GL function
// definitions and defines needed for GL/CL interop, without
// pulling in all of glext.h

#define GL_ARRAY_BUFFER 0x8892
#define GL_RENDERBUFFER 0x8D41
#define GL_TEXTURE_BUFFER 0x8C2A
#define GL_TEXTURE_1D 0x0DE0
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_3D 0x806F
#define GL_TEXTURE_RECTANGLE 0x84F5
#define GL_TEXTURE_1D_ARRAY 0x8C18
#define GL_TEXTURE_2D_ARRAY 0x8C1A
#define GL_TEXTURE_CUBE_MAP_ARRAY 0x9009
#define GL_TEXTURE_CUBE_MAP 0x8513
#define GL_TEXTURE_CUBE_MAP_POSITIVE_X 0x8515
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_X 0x8516
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Y 0x8517
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Y 0x8518
#define GL_TEXTURE_CUBE_MAP_POSITIVE_Z 0x8519
#define GL_TEXTURE_CUBE_MAP_NEGATIVE_Z 0x851A
#define GL_TEXTURE_2D_MULTISAMPLE 0x9100
#define GL_TEXTURE_2D_MULTISAMPLE_ARRAY 0x9102

#define GL_RGBA8                          0x8058
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_RGBA                           0x1908
#define GL_BGRA                           0x80E1
#define GL_UNSIGNED_INT_8_8_8_8_REV       0x8367
#define GL_RGBA8I                         0x8D8E
#define GL_RGBA16I                        0x8D88
#define GL_RGBA32I                        0x8D82
#define GL_RGBA8UI                        0x8D7C
#define GL_RGBA16UI                       0x8D76
#define GL_RGBA32UI                       0x8D70
#define GL_RGBA8_SNORM                    0x8F97
#define GL_RGBA16                         0x805B
#define GL_RGBA16_SNORM                   0x8F9B
#define GL_RGBA16F                        0x881A
#define GL_RGBA32F                        0x8814
#define GL_R8                             0x8229
#define GL_R8_SNORM                       0x8F94
#define GL_R16                            0x822A
#define GL_R16_SNORM                      0x8F98
#define GL_R16F                           0x822D
#define GL_R32F                           0x822E
#define GL_R8I                            0x8231
#define GL_R16I                           0x8233
#define GL_R32I                           0x8235
#define GL_R8UI                           0x8232
#define GL_R16UI                          0x8234
#define GL_R32UI                          0x8236
#define GL_RG8                            0x822B
#define GL_RG8_SNORM                      0x8F95
#define GL_RG16                           0x822C
#define GL_RG16_SNORM                     0x8F99
#define GL_RG16F                          0x822F
#define GL_RG32F                          0x8230
#define GL_RG8I                           0x8237
#define GL_RG16I                          0x8239
#define GL_RG32I                          0x823B
#define GL_RG8UI                          0x8238
#define GL_RG16UI                         0x823A
#define GL_RG32UI                         0x823C

#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

HGLRC WINAPI wglCreateContextAttribsARB(HDC, HGLRC, const int *);

typedef struct __GLsync *GLsync;
void WINAPI glWaitSync(GLsync, unsigned flags, uint64_t timeout);
void WINAPI glDeleteSync(GLsync);

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig;
typedef unsigned int EGLBoolean, EGLenum;
typedef intptr_t EGLAttrib;
typedef int32_t EGLint;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
EGLDisplay WINAPI eglGetPlatformDisplay(EGLenum, void *, const EGLAttrib *);
EGLBoolean WINAPI eglInitialize(EGLDisplay, EGLint *, EGLint *);
EGLBoolean WINAPI eglTerminate(EGLDisplay);
EGLContext WINAPI eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const int32_t *);
EGLBoolean WINAPI eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean WINAPI eglDestroyContext(EGLDisplay, EGLContext);
EGLContext WINAPI eglGetCurrentContext();
__eglMustCastToProperFunctionPointerType WINAPI eglGetProcAddress(const char *);
