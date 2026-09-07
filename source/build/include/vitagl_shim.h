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
#else
#include "glad/glad.h"
#endif
#endif

#endif
