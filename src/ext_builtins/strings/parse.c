// String parsing / splitting builtins.
//
// These cover the small string standard library that callers reach for when
// turning text into scalars or fields: parse_int / parse_float / parse_bool and
// split.  Names are registered exactly as written (lowercase, underscores
// preserved by canonicalizeBuiltinName) so source code can call them directly.

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

// Returns a C-string view of a text-like argument. A single-character literal
// (e.g. the "," separator) arrives as TYPE_CHAR, so materialize it into the
// caller-provided 2-byte scratch buffer. Returns NULL for non-text values.
static const char* string_arg(Value* v, char* scratch2) {
    if (VALUE_TYPE(*v) == TYPE_STRING || VALUE_TYPE(*v) == TYPE_UNICODE_STRING) return AS_STRING(*v);
    if (VALUE_TYPE(*v) == TYPE_CHAR) {
        scratch2[0] = (char)AS_CHAR(*v);
        scratch2[1] = '\0';
        return scratch2;
    }
    return NULL;
}

static Value vmBuiltinParseInt(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "parse_int expects exactly 1 argument.");
        return makeInt(0);
    }
    char sb[2];
    const char* s = string_arg(&args[0], sb);
    if (!s) {
        runtimeError(vm, "parse_int argument must be a string.");
        return makeInt(0);
    }
    return makeInt((long long)strtoll(s, NULL, 10));
}

static Value vmBuiltinParseFloat(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "parse_float expects exactly 1 argument.");
        return makeReal(0.0L);
    }
    char sb[2];
    const char* s = string_arg(&args[0], sb);
    if (!s) {
        runtimeError(vm, "parse_float argument must be a string.");
        return makeReal(0.0L);
    }
    return makeReal(strtold(s, NULL));
}

static Value vmBuiltinParseBool(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "parse_bool expects exactly 1 argument.");
        return makeBoolean(0);
    }
    char sb[2];
    const char* s = string_arg(&args[0], sb);
    if (!s) {
        runtimeError(vm, "parse_bool argument must be a string.");
        return makeBoolean(0);
    }
    while (*s && isspace((unsigned char)*s)) s++;
    if (!strcasecmp(s, "true") || !strcasecmp(s, "1") ||
        !strcasecmp(s, "yes")  || !strcasecmp(s, "t")) {
        return makeBoolean(1);
    }
    return makeBoolean(0);
}

// split(text, separator) -> Text[]  (0-based, always at least one element)
static Value vmBuiltinSplit(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "split expects exactly 2 arguments (text, separator).");
        int lo = 0, hi = 0;
        return makeArrayND(1, &lo, &hi, TYPE_STRING, NULL);
    }
    char sb[2];
    const char* s = string_arg(&args[0], sb);
    char pb[2];
    const char* sep = string_arg(&args[1], pb);
    if (!s) s = "";

    // Empty separator: return the whole string as a single element.
    if (!sep || sep[0] == '\0') {
        int lo = 0, hi = 0;
        Value arr = makeArrayND(1, &lo, &hi, TYPE_STRING, NULL);
        if (AS_ARRAY(arr)) {
            freeValue(&AS_ARRAY(arr)[0]);
            AS_ARRAY(arr)[0] = makeString(s);
        }
        return arr;
    }

    size_t seplen = strlen(sep);
    int count = 1;
    for (const char* p = s; (p = strstr(p, sep)) != NULL; p += seplen) count++;

    int lo = 0, hi = count - 1;
    Value arr = makeArrayND(1, &lo, &hi, TYPE_STRING, NULL);
    if (!AS_ARRAY(arr)) return arr;

    int idx = 0;
    const char* start = s;
    const char* hit;
    while ((hit = strstr(start, sep)) != NULL) {
        freeValue(&AS_ARRAY(arr)[idx]);
        AS_ARRAY(arr)[idx] = makeStringLen(start, (size_t)(hit - start));
        idx++;
        start = hit + seplen;
    }
    freeValue(&AS_ARRAY(arr)[idx]);
    AS_ARRAY(arr)[idx] = makeString(start);
    return arr;
}

void registerParseBuiltins(void) {
    registerVmBuiltin("parse_int",   vmBuiltinParseInt,   BUILTIN_TYPE_FUNCTION, "parse_int");
    registerVmBuiltin("parse_float", vmBuiltinParseFloat, BUILTIN_TYPE_FUNCTION, "parse_float");
    registerVmBuiltin("parse_bool",  vmBuiltinParseBool,  BUILTIN_TYPE_FUNCTION, "parse_bool");
    registerVmBuiltin("split",       vmBuiltinSplit,      BUILTIN_TYPE_FUNCTION, "split");
    // itoa: alias for the existing IntToStr handler (Int -> Text).
    registerVmBuiltin("itoa",        vmBuiltinInttostr,   BUILTIN_TYPE_FUNCTION, "itoa");
}
