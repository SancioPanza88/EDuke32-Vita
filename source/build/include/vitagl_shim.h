// Central GL header switch for the Vita vitaGL port (written from scratch).
// On Vita (HAVE_VITAGL + __PSP2__) this pulls the device GL headers.
// Everywhere else it keeps the original desktop glad loader.
#ifndef VITAGL_SHIM_H
#define VITAGL_SHIM_H

#ifdef USE_OPENGL
#if defined HAVE_VITAGL && defined __PSP2__
#include <vitaGL.h>
// Polymost calls a few glad_ entry points directly; map them to plain GL.
#define glad_glActiveTexture glActiveTexture
#define glad_glBindTexture glBindTexture
#define gladLoadGLLoader(x) (1)

// Desktop GL tokens missing from the device headers: define them so the
// Polymost source compiles. Calls using them become harmless no-ops.
#ifndef GL_TEXTURE_BASE_LEVEL
#define GL_TEXTURE_BASE_LEVEL 0x813C
#endif
#ifndef GL_TEXTURE_MAX_LEVEL
#define GL_TEXTURE_MAX_LEVEL 0x813D
#endif
#ifndef GL_RGBA8
#define GL_RGBA8 GL_RGBA
#endif
#ifndef GL_FOG_HINT
#define GL_FOG_HINT 0x0C54
#endif
#ifndef GL_DITHER
#define GL_DITHER 0x0BD0
#endif
#ifndef GL_COMPRESSED_RGBA
#define GL_COMPRESSED_RGBA GL_RGBA
#endif
#ifndef GL_MAP_PERSISTENT_BIT
#define GL_MAP_PERSISTENT_BIT 0x0040
#endif
#ifndef GL_MAP_COHERENT_BIT
#define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_SYNC_GPU_COMMANDS_COMPLETE
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#endif
#ifndef GL_SYNC_FLUSH_COMMANDS_BIT
#define GL_SYNC_FLUSH_COMMANDS_BIT 0x00000001
#endif
#ifndef GL_ALREADY_SIGNALED
#define GL_ALREADY_SIGNALED 0x911A
#endif
#ifndef GL_CONDITION_SATISFIED
#define GL_CONDITION_SATISFIED 0x911C
#endif
#ifndef GL_TIMEOUT_EXPIRED
#define GL_TIMEOUT_EXPIRED 0x911B
#endif
#ifndef GL_WAIT_FAILED
#define GL_WAIT_FAILED 0x911D
#endif
#ifdef __cplusplus
extern "C" {
#endif
static inline GLsync vita_shim_FenceSync(GLenum c, unsigned int f) { (void)c; (void)f; return (GLsync)0; }
static inline GLenum vita_shim_ClientWaitSync(GLsync s, unsigned int f, unsigned long long t) { (void)s; (void)f; (void)t; return (GLenum)GL_ALREADY_SIGNALED; }
static inline void vita_shim_DeleteSync(GLsync s) { (void)s; }
static inline void vita_shim_WaitSync(GLsync s, unsigned int f, unsigned long long t) { (void)s; (void)f; (void)t; }
static inline void vita_shim_DetachShader(unsigned int p, unsigned int s) { (void)p; (void)s; }
#ifdef __cplusplus
}
#endif
#define glFenceSync vita_shim_FenceSync
#define glClientWaitSync vita_shim_ClientWaitSync
#define glDeleteSync vita_shim_DeleteSync
#define glWaitSync vita_shim_WaitSync
#define glDetachShader vita_shim_DetachShader
#define glBufferStorage(t, s, d, f) glBufferData(t, s, d, GL_STREAM_DRAW)
#else
#include "glad/glad.h"
#endif
#endif

#endif
