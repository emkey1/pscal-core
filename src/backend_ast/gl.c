#ifdef SDL
#include "backend_ast/gl.h"
#include "backend_ast/graphics_3d_backend.h"
#include "backend_ast/pscal_sdl_runtime.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "sdl_ios_dispatch.h"

#if defined(PSCAL_TARGET_IOS)
#include <OpenGLES/ES1/gl.h>
#else
#include "core/sdl_headers.h"
#include PSCALI_SDL_OPENGL_HEADER
#endif
#include <stdbool.h>
#include <strings.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#if defined(PSCAL_TARGET_IOS)
static inline Value pscalRunGlBuiltin(VM* vm,
                                      int arg_count,
                                      Value* args,
                                      PscalSdlVmBuiltin function) {
    if (gSdlGLContext != NULL) {
        return pscalRunSdlBuiltinOnMainQueue(function, vm, arg_count, args);
    }
    return function(vm, arg_count, args);
}

#define PSCAL_DEFINE_IOS_GL_BUILTIN(name) \
    static Value name##Impl(VM* vm, int arg_count, Value* args); \
    Value name(VM* vm, int arg_count, Value* args) { \
        return pscalRunGlBuiltin(vm, arg_count, args, name##Impl); \
    } \
    static Value name##Impl(VM* vm, int arg_count, Value* args)
#else
#define PSCAL_DEFINE_IOS_GL_BUILTIN(name) \
    Value name(VM* vm, int arg_count, Value* args)
#endif

#if defined(PSCAL_TARGET_IOS)
#ifndef GL_QUADS
#define GL_QUADS 0x0007
#endif
#endif

static bool ensureGlContext(VM* vm, const char* name) {
    if (!gSdlInitialized || !gSdlWindow ||
        (gSdlGLContext == NULL && gSdlRenderer == NULL)) {
        runtimeError(vm, "Runtime error: %s requires an active 3D graphics window. Call InitGraph3D first.", name);
        return false;
    }
    return true;
}

static bool valueToFloat(Value v, float* out) {
    if (isRealType(VALUE_TYPE(v))) {
        *out = (float)AS_REAL(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = (float)AS_INTEGER(v);
        return true;
    }
    return false;
}

static bool valueToDouble(Value v, double* out) {
    if (isRealType(VALUE_TYPE(v))) {
        *out = (double)AS_REAL(v);
        return true;
    }
    if (IS_INTLIKE(v)) {
        *out = (double)AS_INTEGER(v);
        return true;
    }
    return false;
}

static bool parseMatrixMode(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "modelview") == 0) {
            *mode = GL_MODELVIEW;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "projection") == 0) {
            *mode = GL_PROJECTION;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "texture") == 0) {
            *mode = GL_TEXTURE;
            return true;
        }
    }
    return false;
}

static bool parsePrimitive(Value arg, GLenum* primitive) {
    if (IS_INTLIKE(arg)) {
        *primitive = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "points") == 0) {
            *primitive = GL_POINTS;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "lines") == 0) {
            *primitive = GL_LINES;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "line_strip") == 0 || strcasecmp(AS_STRING(arg), "linestrip") == 0) {
            *primitive = GL_LINE_STRIP;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "line_loop") == 0 || strcasecmp(AS_STRING(arg), "lineloop") == 0) {
            *primitive = GL_LINE_LOOP;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "triangles") == 0) {
            *primitive = GL_TRIANGLES;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "triangle_strip") == 0 || strcasecmp(AS_STRING(arg), "trianglestrip") == 0) {
            *primitive = GL_TRIANGLE_STRIP;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "triangle_fan") == 0 || strcasecmp(AS_STRING(arg), "trianglefan") == 0) {
            *primitive = GL_TRIANGLE_FAN;
            return true;
        }
#ifdef GL_QUADS
        if (strcasecmp(AS_STRING(arg), "quads") == 0) {
            *primitive = GL_QUADS;
            return true;
        }
#endif
#ifdef GL_QUAD_STRIP
        if (strcasecmp(AS_STRING(arg), "quad_strip") == 0 || strcasecmp(AS_STRING(arg), "quadstrip") == 0) {
            *primitive = GL_QUAD_STRIP;
            return true;
        }
#endif
#ifdef GL_POLYGON
        if (strcasecmp(AS_STRING(arg), "polygon") == 0) {
            *primitive = GL_POLYGON;
            return true;
        }
#endif
    }
    return false;
}

static bool parseCapability(Value arg, GLenum* cap) {
    if (IS_INTLIKE(arg)) {
        *cap = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
#ifdef GL_CULL_FACE
        if (strcasecmp(AS_STRING(arg), "cull_face") == 0 || strcasecmp(AS_STRING(arg), "cullface") == 0) {
            *cap = GL_CULL_FACE;
            return true;
        }
#endif
#ifdef GL_LIGHTING
        if (strcasecmp(AS_STRING(arg), "lighting") == 0) {
            *cap = GL_LIGHTING;
            return true;
        }
#endif
#ifdef GL_LIGHT0
        if (strcasecmp(AS_STRING(arg), "light0") == 0) {
            *cap = GL_LIGHT0;
            return true;
        }
#endif
#ifdef GL_LIGHT1
        if (strcasecmp(AS_STRING(arg), "light1") == 0) {
            *cap = GL_LIGHT1;
            return true;
        }
#endif
#ifdef GL_LIGHT2
        if (strcasecmp(AS_STRING(arg), "light2") == 0) {
            *cap = GL_LIGHT2;
            return true;
        }
#endif
#ifdef GL_LIGHT3
        if (strcasecmp(AS_STRING(arg), "light3") == 0) {
            *cap = GL_LIGHT3;
            return true;
        }
#endif
#ifdef GL_LIGHT4
        if (strcasecmp(AS_STRING(arg), "light4") == 0) {
            *cap = GL_LIGHT4;
            return true;
        }
#endif
#ifdef GL_LIGHT5
        if (strcasecmp(AS_STRING(arg), "light5") == 0) {
            *cap = GL_LIGHT5;
            return true;
        }
#endif
#ifdef GL_LIGHT6
        if (strcasecmp(AS_STRING(arg), "light6") == 0) {
            *cap = GL_LIGHT6;
            return true;
        }
#endif
#ifdef GL_LIGHT7
        if (strcasecmp(AS_STRING(arg), "light7") == 0) {
            *cap = GL_LIGHT7;
            return true;
        }
#endif
#ifdef GL_COLOR_MATERIAL
        if (strcasecmp(AS_STRING(arg), "color_material") == 0) {
            *cap = GL_COLOR_MATERIAL;
            return true;
        }
#endif
#ifdef GL_NORMALIZE
        if (strcasecmp(AS_STRING(arg), "normalize") == 0) {
            *cap = GL_NORMALIZE;
            return true;
        }
#endif
        if (strcasecmp(AS_STRING(arg), "blend") == 0) {
            *cap = GL_BLEND;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "cull_face") == 0 ||
            strcasecmp(AS_STRING(arg), "cullface") == 0) {
            *cap = GL_CULL_FACE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "depth_test") == 0 ||
            strcasecmp(AS_STRING(arg), "depthtest") == 0) {
            *cap = GL_DEPTH_TEST;
            return true;
        }
#ifdef GL_FOG
        if (strcasecmp(AS_STRING(arg), "fog") == 0) {
            *cap = GL_FOG;
            return true;
        }
#endif
        if (strcasecmp(AS_STRING(arg), "scissor_test") == 0 ||
            strcasecmp(AS_STRING(arg), "scissortest") == 0) {
            *cap = GL_SCISSOR_TEST;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "texture_2d") == 0) {
            *cap = GL_TEXTURE_2D;
            return true;
        }
    }
    return false;
}

static bool parseCullFaceMode(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "front") == 0) {
            *mode = GL_FRONT;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "back") == 0) {
            *mode = GL_BACK;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "front_and_back") == 0 ||
            strcasecmp(AS_STRING(arg), "frontandback") == 0 ||
            strcasecmp(AS_STRING(arg), "front-and-back") == 0) {
            *mode = GL_FRONT_AND_BACK;
            return true;
        }
    }
    return false;
}

static bool parseDepthFunc(Value arg, GLenum* func) {
    if (IS_INTLIKE(arg)) {
        *func = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "never") == 0) {
            *func = GL_NEVER;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "less") == 0) {
            *func = GL_LESS;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "equal") == 0) {
            *func = GL_EQUAL;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "lequal") == 0 ||
            strcasecmp(AS_STRING(arg), "less_equal") == 0 ||
            strcasecmp(AS_STRING(arg), "less-equal") == 0) {
            *func = GL_LEQUAL;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "greater") == 0) {
            *func = GL_GREATER;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "notequal") == 0 ||
            strcasecmp(AS_STRING(arg), "not_equal") == 0 ||
            strcasecmp(AS_STRING(arg), "not-equal") == 0) {
            *func = GL_NOTEQUAL;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "gequal") == 0 ||
            strcasecmp(AS_STRING(arg), "greater_equal") == 0 ||
            strcasecmp(AS_STRING(arg), "greater-equal") == 0) {
            *func = GL_GEQUAL;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "always") == 0) {
            *func = GL_ALWAYS;
            return true;
        }
    }
    return false;
}

static bool parseShadeModel(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "smooth") == 0) {
            *mode = GL_SMOOTH;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "flat") == 0) {
            *mode = GL_FLAT;
            return true;
        }
    }
    return false;
}

static bool parseLight(Value arg, GLenum* light) {
    if (IS_INTLIKE(arg)) {
        *light = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
#ifdef GL_LIGHT0
        if (strcasecmp(AS_STRING(arg), "light0") == 0) {
            *light = GL_LIGHT0;
            return true;
        }
#endif
#ifdef GL_LIGHT1
        if (strcasecmp(AS_STRING(arg), "light1") == 0) {
            *light = GL_LIGHT1;
            return true;
        }
#endif
#ifdef GL_LIGHT2
        if (strcasecmp(AS_STRING(arg), "light2") == 0) {
            *light = GL_LIGHT2;
            return true;
        }
#endif
#ifdef GL_LIGHT3
        if (strcasecmp(AS_STRING(arg), "light3") == 0) {
            *light = GL_LIGHT3;
            return true;
        }
#endif
#ifdef GL_LIGHT4
        if (strcasecmp(AS_STRING(arg), "light4") == 0) {
            *light = GL_LIGHT4;
            return true;
        }
#endif
#ifdef GL_LIGHT5
        if (strcasecmp(AS_STRING(arg), "light5") == 0) {
            *light = GL_LIGHT5;
            return true;
        }
#endif
#ifdef GL_LIGHT6
        if (strcasecmp(AS_STRING(arg), "light6") == 0) {
            *light = GL_LIGHT6;
            return true;
        }
#endif
#ifdef GL_LIGHT7
        if (strcasecmp(AS_STRING(arg), "light7") == 0) {
            *light = GL_LIGHT7;
            return true;
        }
#endif
    }
    return false;
}

static bool parseLightParam(Value arg, GLenum* pname) {
    if (IS_INTLIKE(arg)) {
        *pname = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "position") == 0) {
            *pname = GL_POSITION;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "diffuse") == 0) {
            *pname = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "specular") == 0) {
            *pname = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "ambient") == 0) {
            *pname = GL_AMBIENT;
            return true;
        }
    }
    return false;
}

static bool parseMaterialFace(Value arg, GLenum* face) {
    if (IS_INTLIKE(arg)) {
        *face = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "front") == 0) {
            *face = GL_FRONT;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "back") == 0) {
            *face = GL_BACK;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "front_and_back") == 0 ||
            strcasecmp(AS_STRING(arg), "frontandback") == 0) {
            *face = GL_FRONT_AND_BACK;
            return true;
        }
    }
    return false;
}

static bool parseMaterialParam(Value arg, GLenum* pname) {
    if (IS_INTLIKE(arg)) {
        *pname = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "ambient") == 0) {
            *pname = GL_AMBIENT;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "diffuse") == 0) {
            *pname = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "specular") == 0) {
            *pname = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "emission") == 0) {
            *pname = GL_EMISSION;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "ambient_and_diffuse") == 0 ||
            strcasecmp(AS_STRING(arg), "ambientdiffuse") == 0) {
            *pname = GL_AMBIENT_AND_DIFFUSE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "shininess") == 0) {
            *pname = GL_SHININESS;
            return true;
        }
    }
    return false;
}

static bool parseColorMaterialMode(Value arg, GLenum* mode) {
    if (IS_INTLIKE(arg)) {
        *mode = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "ambient") == 0) {
            *mode = GL_AMBIENT;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "diffuse") == 0) {
            *mode = GL_DIFFUSE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "ambient_and_diffuse") == 0 ||
            strcasecmp(AS_STRING(arg), "ambientdiffuse") == 0) {
            *mode = GL_AMBIENT_AND_DIFFUSE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "specular") == 0) {
            *mode = GL_SPECULAR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "emission") == 0) {
            *mode = GL_EMISSION;
            return true;
        }
    }
    return false;
}

static bool parseBlendFactor(Value arg, GLenum* factor) {
    if (IS_INTLIKE(arg)) {
        *factor = (GLenum)AS_INTEGER(arg);
        return true;
    }
    if (isPascalStringType(VALUE_TYPE(arg)) && AS_STRING(arg)) {
        if (strcasecmp(AS_STRING(arg), "zero") == 0) {
            *factor = GL_ZERO;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "one") == 0) {
            *factor = GL_ONE;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "src_color") == 0 ||
            strcasecmp(AS_STRING(arg), "srccolor") == 0) {
            *factor = GL_SRC_COLOR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "one_minus_src_color") == 0 ||
            strcasecmp(AS_STRING(arg), "oneminussrccolor") == 0) {
            *factor = GL_ONE_MINUS_SRC_COLOR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "dst_color") == 0 ||
            strcasecmp(AS_STRING(arg), "dstcolor") == 0) {
            *factor = GL_DST_COLOR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "one_minus_dst_color") == 0 ||
            strcasecmp(AS_STRING(arg), "oneminusdstcolor") == 0) {
            *factor = GL_ONE_MINUS_DST_COLOR;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "src_alpha") == 0 ||
            strcasecmp(AS_STRING(arg), "srcalpha") == 0) {
            *factor = GL_SRC_ALPHA;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "one_minus_src_alpha") == 0 ||
            strcasecmp(AS_STRING(arg), "oneminussrcalpha") == 0) {
            *factor = GL_ONE_MINUS_SRC_ALPHA;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "dst_alpha") == 0 ||
            strcasecmp(AS_STRING(arg), "dstalpha") == 0) {
            *factor = GL_DST_ALPHA;
            return true;
        }
        if (strcasecmp(AS_STRING(arg), "one_minus_dst_alpha") == 0 ||
            strcasecmp(AS_STRING(arg), "oneminusdstalpha") == 0) {
            *factor = GL_ONE_MINUS_DST_ALPHA;
            return true;
        }
    }
    return false;
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlclearcolor) {
    if (arg_count != 4) {
        runtimeError(vm, "GLClearColor expects 4 numeric arguments (r, g, b, a).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClearColor")) return makeVoid();

    float comps[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &comps[i])) {
            runtimeError(vm, "GLClearColor component %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (comps[i] < 0.0f) comps[i] = 0.0f;
        if (comps[i] > 1.0f) comps[i] = 1.0f;
    }

    gfx3dClearColor(comps[0], comps[1], comps[2], comps[3]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlclear) {
    if (arg_count > 1) {
        runtimeError(vm, "GLClear expects 0 or 1 argument (GLbitfield mask).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClear")) return makeVoid();

    GLbitfield mask = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;
    if (arg_count == 1) {
        if (!IS_INTLIKE(args[0])) {
            runtimeError(vm, "GLClear mask must be an integer-like value.");
            return makeVoid();
        }
        mask = (GLbitfield)AS_INTEGER(args[0]);
    }

    gfx3dClear(mask);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlcleardepth) {
    if (arg_count != 1) {
        runtimeError(vm, "GLClearDepth expects 1 numeric argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLClearDepth")) return makeVoid();

    double depth;
    if (!valueToDouble(args[0], &depth)) {
        runtimeError(vm, "GLClearDepth argument must be numeric.");
        return makeVoid();
    }
    if (depth < 0.0) depth = 0.0;
    if (depth > 1.0) depth = 1.0;

    gfx3dClearDepth(depth);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlviewport) {
    if (arg_count != 4) {
        runtimeError(vm, "GLViewport expects 4 integer arguments (x, y, width, height).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLViewport")) return makeVoid();

    for (int i = 0; i < 4; ++i) {
        if (!IS_INTLIKE(args[i])) {
            runtimeError(vm, "GLViewport argument %d must be integer-like.", i + 1);
            return makeVoid();
        }
    }

    gfx3dViewport((int)AS_INTEGER(args[0]), (int)AS_INTEGER(args[1]),
                  (int)AS_INTEGER(args[2]), (int)AS_INTEGER(args[3]));
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlmatrixmode) {
    if (arg_count != 1) {
        runtimeError(vm, "GLMatrixMode expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMatrixMode")) return makeVoid();

    GLenum mode;
    if (!parseMatrixMode(args[0], &mode)) {
        runtimeError(vm, "GLMatrixMode accepts 'modelview', 'projection', 'texture', or an integer GLenum.");
        return makeVoid();
    }

    gfx3dMatrixMode(mode);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlloadidentity) {
    if (arg_count != 0) {
        runtimeError(vm, "GLLoadIdentity expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLLoadIdentity")) return makeVoid();

    gfx3dLoadIdentity();
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGltranslatef) {
    if (arg_count != 3) {
        runtimeError(vm, "GLTranslatef expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLTranslatef")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLTranslatef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dTranslatef(vals[0], vals[1], vals[2]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlrotatef) {
    if (arg_count != 4) {
        runtimeError(vm, "GLRotatef expects 4 numeric arguments (angle, x, y, z).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLRotatef")) return makeVoid();

    float vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLRotatef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dRotatef(vals[0], vals[1], vals[2], vals[3]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlscalef) {
    if (arg_count != 3) {
        runtimeError(vm, "GLScalef expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLScalef")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLScalef argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dScalef(vals[0], vals[1], vals[2]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlfrustum) {
    if (arg_count != 6) {
        runtimeError(vm, "GLFrustum expects 6 numeric arguments (left, right, bottom, top, near, far).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLFrustum")) return makeVoid();

    double vals[6];
    for (int i = 0; i < 6; ++i) {
        if (!valueToDouble(args[i], &vals[i])) {
            runtimeError(vm, "GLFrustum argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    if (vals[4] <= 0.0 || vals[5] <= 0.0 || vals[4] >= vals[5]) {
        runtimeError(vm, "GLFrustum requires near > 0, far > 0, and far > near.");
        return makeVoid();
    }

    gfx3dFrustum(vals[0], vals[1], vals[2], vals[3], vals[4], vals[5]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlperspective) {
    if (arg_count != 4) {
        runtimeError(vm, "GLPerspective expects 4 numeric arguments (fovY, aspect, near, far).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPerspective")) return makeVoid();

    double fovY, aspect, nearPlane, farPlane;
    if (!valueToDouble(args[0], &fovY) || !valueToDouble(args[1], &aspect) ||
        !valueToDouble(args[2], &nearPlane) || !valueToDouble(args[3], &farPlane)) {
        runtimeError(vm, "GLPerspective arguments must be numeric.");
        return makeVoid();
    }

    if (aspect == 0.0) {
        runtimeError(vm, "GLPerspective aspect ratio cannot be zero.");
        return makeVoid();
    }
    if (nearPlane <= 0.0 || farPlane <= 0.0 || nearPlane >= farPlane) {
        runtimeError(vm, "GLPerspective requires near > 0, far > 0, and far > near.");
        return makeVoid();
    }

    if (fovY <= 0.0 || fovY >= 180.0) {
        runtimeError(vm, "GLPerspective fovY must be between 0 and 180 degrees.");
        return makeVoid();
    }

    double f = tan((fovY * 0.5) * (M_PI / 180.0));
    double top = nearPlane * f;
    double bottom = -top;
    double right = top * aspect;
    double left = -right;

    gfx3dFrustum(left, right, bottom, top, nearPlane, farPlane);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlpushmatrix) {
    if (arg_count != 0) {
        runtimeError(vm, "GLPushMatrix expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPushMatrix")) return makeVoid();

    gfx3dPushMatrix();
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlpopmatrix) {
    if (arg_count != 0) {
        runtimeError(vm, "GLPopMatrix expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLPopMatrix")) return makeVoid();

    gfx3dPopMatrix();
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlbegin) {
    if (arg_count != 1) {
        runtimeError(vm, "GLBegin expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLBegin")) return makeVoid();

    GLenum primitive;
    if (!parsePrimitive(args[0], &primitive)) {
        runtimeError(vm, "GLBegin accepts primitive names like 'triangles', 'quads', 'lines', or an integer GLenum.");
        return makeVoid();
    }

    gfx3dBegin(primitive);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlend) {
    if (arg_count != 0) {
        runtimeError(vm, "GLEnd expects 0 arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLEnd")) return makeVoid();

    gfx3dEnd();
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlcolor3f) {
    if (arg_count != 3) {
        runtimeError(vm, "GLColor3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColor3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLColor3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (vals[i] < 0.0f) vals[i] = 0.0f;
        if (vals[i] > 1.0f) vals[i] = 1.0f;
    }

    gfx3dColor3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlcolor4f) {
    if (arg_count != 4) {
        runtimeError(vm, "GLColor4f expects 4 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColor4f")) return makeVoid();

    float vals[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLColor4f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
        if (vals[i] < 0.0f) vals[i] = 0.0f;
        if (vals[i] > 1.0f) vals[i] = 1.0f;
    }

    gfx3dColor4f(vals[0], vals[1], vals[2], vals[3]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlvertex3f) {
    if (arg_count != 3) {
        runtimeError(vm, "GLVertex3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLVertex3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLVertex3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dVertex3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlnormal3f) {
    if (arg_count != 3) {
        runtimeError(vm, "GLNormal3f expects 3 numeric arguments.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLNormal3f")) return makeVoid();

    float vals[3];
    for (int i = 0; i < 3; ++i) {
        if (!valueToFloat(args[i], &vals[i])) {
            runtimeError(vm, "GLNormal3f argument %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dNormal3f(vals[0], vals[1], vals[2]);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlenable) {
    if (arg_count != 1) {
        runtimeError(vm, "GLEnable expects 1 argument (GL capability).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLEnable")) return makeVoid();

    GLenum cap;
    if (!parseCapability(args[0], &cap)) {
        runtimeError(vm, "GLEnable argument must be a known capability name or GLenum value.");
        return makeVoid();
    }

    gfx3dEnable(cap);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGldisable) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDisable expects 1 argument (GL capability).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDisable")) return makeVoid();

    GLenum cap;
    if (!parseCapability(args[0], &cap)) {
        runtimeError(vm, "GLDisable argument must be a known capability name or GLenum value.");
        return makeVoid();
    }

    gfx3dDisable(cap);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlshademodel) {
    if (arg_count != 1) {
        runtimeError(vm, "GLShadeModel expects 1 argument (string or GLenum).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLShadeModel")) return makeVoid();

    GLenum mode;
    if (!parseShadeModel(args[0], &mode)) {
        runtimeError(vm, "GLShadeModel argument must be 'flat', 'smooth', or a GLenum value.");
        return makeVoid();
    }

    gfx3dShadeModel(mode);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGllightfv) {
    if (arg_count != 6) {
        runtimeError(vm, "GLLightfv expects 6 arguments (light, pname, x, y, z, w).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLLightfv")) return makeVoid();

    GLenum light;
    if (!parseLight(args[0], &light)) {
        runtimeError(vm, "GLLightfv light must be 'light0'..'light7' or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseLightParam(args[1], &pname)) {
        runtimeError(vm, "GLLightfv pname must be 'position', 'ambient', 'diffuse', 'specular', or a GLenum value.");
        return makeVoid();
    }

    float values[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[2 + i], &values[i])) {
            runtimeError(vm, "GLLightfv component %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dLightfv(light, pname, values);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlmaterialfv) {
    if (arg_count != 6) {
        runtimeError(vm, "GLMaterialfv expects 6 arguments (face, pname, r, g, b, a).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMaterialfv")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLMaterialfv face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseMaterialParam(args[1], &pname)) {
        runtimeError(vm, "GLMaterialfv pname must be 'ambient', 'diffuse', 'specular', 'emission', 'ambient_and_diffuse', or a GLenum value.");
        return makeVoid();
    }

    float values[4];
    for (int i = 0; i < 4; ++i) {
        if (!valueToFloat(args[2 + i], &values[i])) {
            runtimeError(vm, "GLMaterialfv component %d must be numeric.", i + 1);
            return makeVoid();
        }
    }

    gfx3dMaterialfv(face, pname, values);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlmaterialf) {
    if (arg_count != 3) {
        runtimeError(vm, "GLMaterialf expects 3 arguments (face, pname, value).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLMaterialf")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLMaterialf face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum pname;
    if (!parseMaterialParam(args[1], &pname)) {
        runtimeError(vm, "GLMaterialf pname must be 'shininess' or a GLenum value.");
        return makeVoid();
    }
    if (pname != GL_SHININESS) {
        runtimeError(vm, "GLMaterialf currently supports only the 'shininess' parameter.");
        return makeVoid();
    }

    float value;
    if (!valueToFloat(args[2], &value)) {
        runtimeError(vm, "GLMaterialf value must be numeric.");
        return makeVoid();
    }

    gfx3dMaterialf(face, pname, value);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlcolormaterial) {
    if (arg_count != 2) {
        runtimeError(vm, "GLColorMaterial expects 2 arguments (face, mode).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLColorMaterial")) return makeVoid();

    GLenum face;
    if (!parseMaterialFace(args[0], &face)) {
        runtimeError(vm, "GLColorMaterial face must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    GLenum mode;
    if (!parseColorMaterialMode(args[1], &mode)) {
        runtimeError(vm, "GLColorMaterial mode must be 'ambient', 'diffuse', 'ambient_and_diffuse', 'specular', 'emission', or a GLenum value.");
        return makeVoid();
    }

    gfx3dColorMaterial(face, mode);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlblendfunc) {
    if (arg_count != 2) {
        runtimeError(vm, "GLBlendFunc expects 2 arguments (sfactor, dfactor).");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLBlendFunc")) return makeVoid();

    GLenum sfactor;
    if (!parseBlendFactor(args[0], &sfactor)) {
        runtimeError(vm, "GLBlendFunc sfactor must be a known blend factor name or GLenum value.");
        return makeVoid();
    }

    GLenum dfactor;
    if (!parseBlendFactor(args[1], &dfactor)) {
        runtimeError(vm, "GLBlendFunc dfactor must be a known blend factor name or GLenum value.");
        return makeVoid();
    }

    gfx3dBlendFunc(sfactor, dfactor);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlcullface) {
    if (arg_count != 1) {
        runtimeError(vm, "GLCullFace expects 1 argument specifying a face to cull.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLCullFace")) return makeVoid();

    GLenum mode;
    if (!parseCullFaceMode(args[0], &mode)) {
        runtimeError(vm, "GLCullFace argument must be 'front', 'back', 'front_and_back', or a GLenum value.");
        return makeVoid();
    }

    gfx3dCullFace(mode);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGldepthtest) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDepthTest expects 1 boolean or integer argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDepthTest")) return makeVoid();

    bool enable;
    if (VALUE_TYPE(args[0]) == TYPE_BOOLEAN) {
        enable = AS_BOOLEAN(args[0]);
    } else if (IS_INTLIKE(args[0])) {
        enable = AS_INTEGER(args[0]) != 0;
    } else if (isRealType(VALUE_TYPE(args[0]))) {
        enable = AS_REAL(args[0]) != 0.0;
    } else {
        runtimeError(vm, "GLDepthTest argument must be boolean or numeric.");
        return makeVoid();
    }

    if (enable) {
        gfx3dEnable(GL_DEPTH_TEST);
    } else {
        gfx3dDisable(GL_DEPTH_TEST);
    }
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGldepthmask) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDepthMask expects 1 boolean or numeric argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDepthMask")) return makeVoid();

    bool enable;
    if (VALUE_TYPE(args[0]) == TYPE_BOOLEAN) {
        enable = AS_BOOLEAN(args[0]);
    } else if (IS_INTLIKE(args[0])) {
        enable = AS_INTEGER(args[0]) != 0;
    } else if (isRealType(VALUE_TYPE(args[0]))) {
        enable = AS_REAL(args[0]) != 0.0;
    } else {
        runtimeError(vm, "GLDepthMask argument must be boolean or numeric.");
        return makeVoid();
    }

    gfx3dDepthMask(enable ? GL_TRUE : GL_FALSE);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGldepthfunc) {
    if (arg_count != 1) {
        runtimeError(vm, "GLDepthFunc expects 1 argument specifying the depth comparison.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLDepthFunc")) return makeVoid();

    GLenum func;
    if (!parseDepthFunc(args[0], &func)) {
        runtimeError(vm, "GLDepthFunc argument must be a known depth function name (less, lequal, equal, greater, gequal, notequal, always, never) or a GLenum value.");
        return makeVoid();
    }

    gfx3dDepthFunc(func);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGllinewidth) {
    if (arg_count != 1) {
        runtimeError(vm, "GLLineWidth expects 1 numeric argument.");
        return makeVoid();
    }
    if (!ensureGlContext(vm, "GLLineWidth")) return makeVoid();

    float width;
    if (!valueToFloat(args[0], &width)) {
        runtimeError(vm, "GLLineWidth argument must be numeric.");
        return makeVoid();
    }
    if (width <= 0.0f) {
        runtimeError(vm, "GLLineWidth requires a positive width.");
        return makeVoid();
    }

    gfx3dLineWidth(width);
    return makeVoid();
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlishardwareaccelerated) {
    if (arg_count != 0) {
        runtimeError(vm, "GLIsHardwareAccelerated does not take any arguments.");
        return makeBoolean(false);
    }
    if (!ensureGlContext(vm, "GLIsHardwareAccelerated")) {
        return makeBoolean(false);
    }

    if (gSdlGLContext != NULL) {
        int accelerated = 0;
#if defined(PSCALI_SDL3)
        if (!SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &accelerated)) {
#else
        if (SDL_GL_GetAttribute(SDL_GL_ACCELERATED_VISUAL, &accelerated) != 0) {
#endif
            runtimeError(vm, "GLIsHardwareAccelerated: SDL_GL_GetAttribute failed: %s", SDL_GetError());
            return makeBoolean(false);
        }
        return makeBoolean(accelerated != 0);
    }

    if (gSdlRenderer != NULL) {
        SDL_RendererInfo info;
        memset(&info, 0, sizeof(info));
        if (SDL_GetRendererInfo(gSdlRenderer, &info) == 0) {
            return makeBoolean((info.flags & SDL_RENDERER_ACCELERATED) != 0);
        }
    }

    return makeBoolean(false);
}

PSCAL_DEFINE_IOS_GL_BUILTIN(vmBuiltinGlsaveframebufferpng) {
    const char* name = "GLSaveFramebufferPng";
    if (arg_count != 1 && arg_count != 2) {
        runtimeError(vm, "%s expects 1 or 2 arguments (Path: String [, FlipVertical: Boolean]).", name);
        return makeBoolean(false);
    }
    if (!ensureGlContext(vm, name)) {
        return makeBoolean(false);
    }
    if (!isPascalStringType(VALUE_TYPE(args[0])) || !AS_STRING(args[0])) {
        runtimeError(vm, "%s expects the first argument to be a file path string.", name);
        return makeBoolean(false);
    }

    const char* path = AS_STRING(args[0]);
    bool flipVertical = true;
    if (arg_count == 2) {
        if (VALUE_TYPE(args[1]) == TYPE_BOOLEAN) {
            flipVertical = AS_BOOLEAN(args[1]);
        } else if (IS_INTLIKE(args[1])) {
            flipVertical = AS_INTEGER(args[1]) != 0;
        } else if (isRealType(VALUE_TYPE(args[1]))) {
            flipVertical = AS_REAL(args[1]) != 0.0;
        } else {
            runtimeError(vm, "%s expects a boolean flipVertical flag as the second argument.", name);
            return makeBoolean(false);
        }
    }

    int width = 0;
    int height = 0;
    if (gSdlGLContext != NULL) {
        SDL_GL_GetDrawableSize(gSdlWindow, &width, &height);
    } else {
        width = gSdlWidth;
        height = gSdlHeight;
    }
    if (width <= 0 || height <= 0) {
        runtimeError(vm, "%s could not determine the drawable size.", name);
        return makeBoolean(false);
    }

    size_t stride = (size_t)width * 4u;
    size_t bufferSize = stride * (size_t)height;
    uint8_t* pixels = (uint8_t*)malloc(bufferSize);
    if (!pixels) {
        runtimeError(vm, "%s could not allocate %zu bytes for the framebuffer capture.", name, bufferSize);
        return makeBoolean(false);
    }

    gfx3dPixelStorei(GL_PACK_ALIGNMENT, 1);
    gfx3dReadBuffer(GL_BACK);
    gfx3dReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    GLenum error = gfx3dGetError();
    if (error != GL_NO_ERROR) {
        free(pixels);
        runtimeError(vm, "%s failed to read pixels (GL error %u).", name, (unsigned int)error);
        return makeBoolean(false);
    }

    SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        free(pixels);
        runtimeError(vm, "%s could not allocate an SDL surface: %s.", name, SDL_GetError());
        return makeBoolean(false);
    }

    uint8_t* dest = (uint8_t*)surface->pixels;
    for (int y = 0; y < height; ++y) {
        int sourceY = flipVertical ? (height - 1 - y) : y;
        memcpy(dest + (size_t)y * surface->pitch,
               pixels + (size_t)sourceY * stride,
               stride);
    }

    free(pixels);

    if (!gSdlImageInitialized) {
        int imgFlags = IMG_INIT_PNG | IMG_INIT_JPG;
        if (!(IMG_Init(imgFlags) & imgFlags)) {
            runtimeError(vm, "%s failed to initialise SDL_image: %s.", name, IMG_GetError());
            SDL_FreeSurface(surface);
            return makeBoolean(false);
        }
        gSdlImageInitialized = true;
    }

    if (IMG_SavePNG(surface, path) != 0) {
        runtimeError(vm, "%s failed to write '%s': %s.", name, path, IMG_GetError());
        SDL_FreeSurface(surface);
        return makeBoolean(false);
    }

    SDL_FreeSurface(surface);
    return makeBoolean(true);
}

#endif // SDL
