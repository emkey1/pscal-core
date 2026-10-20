// A small standard library: whole-Text helpers, array helpers, checked
// parsing, rounding, one-call file I/O, the clock and the MStream verbs the
// older surface lacked.
//
// These are the engine half of Aether's modern surface (Aether decisions
// D56, D57, D59, D60, D61). Every helper returns a fresh value and never
// mutates its arguments; Text positions are zero-based codepoint indexes,
// matching Aether's `s[i]`, `length(s)` and slices. Results do not depend on
// the host libc: parsing, case mapping and comparison are done here, byte by
// byte, so Linux and macOS print the same thing.

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "core/utils.h"
#include "core/globals.h"
#include "backend_ast/builtin.h"

/* ------------------------------------------------------------------ */
/* Argument helpers                                                    */
/* ------------------------------------------------------------------ */

/* A VAR-style argument arrives as a pointer to the caller's Value; follow it
 * to an array or Text, never to a record (records are pointer-backed). */
static Value *stdArg(Value *v) {
    if (v && VALUE_TYPE(*v) == TYPE_POINTER && AS_POINTER(*v)) {
        Value *p = (Value *)AS_POINTER(*v);
        if (VALUE_TYPE(*p) == TYPE_ARRAY || isPascalStringType(VALUE_TYPE(*p)) ||
            VALUE_TYPE(*p) == TYPE_MEMORYSTREAM) {
            return p;
        }
    }
    return v;
}

/* C-string view of a Text-like value; a Char (or an indexed Text element,
 * which arrives as a WideChar) is encoded into the 5-byte scratch buffer.
 * NULL for anything else. */
static const char *stdText(Value *v, char scratch5[5]) {
    v = stdArg(v);
    if (!v) return NULL;
    if (isPascalStringType(VALUE_TYPE(*v))) {
        const char *s = AS_STRING(*v);
        return s ? s : "";
    }
    if (VALUE_TYPE(*v) == TYPE_CHAR) {
        scratch5[0] = (char)AS_CHAR(*v);
        scratch5[1] = '\0';
        return scratch5;
    }
    if (VALUE_TYPE(*v) == TYPE_WIDECHAR) {
        size_t n = encodeUtf8Codepoint((uint32_t)AS_CHAR(*v), scratch5);
        scratch5[n] = '\0';
        return scratch5;
    }
    return NULL;
}

static bool stdIsArray(Value *v) {
    return v && VALUE_TYPE(*v) == TYPE_ARRAY;
}

/* The elements and count of a one-dimensional, unpacked array. */
static bool stdArrayView(VM *vm, const char *who, Value *v, Value **elems, int *count) {
    if (!stdIsArray(v)) {
        runtimeError(vm, "%s expects an array.", who);
        return false;
    }
    ArrayObj *a = PSCAL_VALUE_PTR(*v, ArrayObj);
    if (!a) {
        *elems = NULL;
        *count = 0;
        return true;
    }
    if (a->dimensions > 1) {
        runtimeError(vm, "%s expects a one-dimensional array.", who);
        return false;
    }
    if (a->is_packed) {
        runtimeError(vm, "%s does not support Byte arrays.", who);
        return false;
    }
    int n = 0;
    if (a->dimensions == 1 && a->lower_bounds && a->upper_bounds) {
        n = a->upper_bounds[0] - a->lower_bounds[0] + 1;
    }
    if (n < 0) n = 0;
    *elems = n > 0 ? a->elements : NULL;
    *count = n;
    return true;
}

/* A new dynamic one-dimensional array of n default elements, indexed 0..n-1. */
static Value stdNewArray(int n, VarType elemType, AST *elemDef) {
    Value v;
    if (n > 0) {
        int lo = 0, hi = n - 1;
        v = makeArrayND(1, &lo, &hi, elemType, elemDef);
    } else {
        v = makeEmptyArray(elemType, elemDef);
        ArrayObj *a = PSCAL_VALUE_PTR(v, ArrayObj);
        if (a) {
            a->dimensions = 1;
            a->lower_bounds = (int *)malloc(sizeof(int));
            a->upper_bounds = (int *)malloc(sizeof(int));
            if (a->lower_bounds) a->lower_bounds[0] = 0;
            if (a->upper_bounds) a->upper_bounds[0] = -1;
        }
    }
    ArrayObj *a = PSCAL_VALUE_PTR(v, ArrayObj);
    if (a) {
        a->is_dynamic = true;
        a->lower_bound = 0;
        a->upper_bound = n - 1;
    }
    return v;
}

static void stdArraySet(Value *arr, int i, Value item) {
    ArrayObj *a = PSCAL_VALUE_PTR(*arr, ArrayObj);
    if (!a || !a->elements) {
        freeValue(&item);
        return;
    }
    freeValue(&a->elements[i]);
    a->elements[i] = item;
}

/* A value as Aether copies it: arrays are values, so a nested array's rows
 * are copied too (a dynamic array would otherwise be shared); records are
 * pointer-backed and stay shared, exactly as `let b = a;` leaves them. */
static Value stdDeepCopy(const Value *v) {
    if (v && VALUE_TYPE(*v) == TYPE_ARRAY) {
        ArrayObj *a = PSCAL_VALUE_PTR(*v, ArrayObj);
        if (a && a->is_dynamic && !a->is_packed && a->dimensions == 1 &&
            a->lower_bounds && a->upper_bounds) {
            int n = a->upper_bounds[0] - a->lower_bounds[0] + 1;
            if (n < 0) n = 0;
            Value out = stdNewArray(n, a->element_type, a->element_type_def);
            for (int i = 0; i < n; i++) stdArraySet(&out, i, stdDeepCopy(&a->elements[i]));
            return out;
        }
    }
    return makeCopyOfValue(v);
}

static Value stdTextValue(const char *s) {
    return makeUnicodeString(s ? s : "");
}

static Value stdTextValueLen(const char *s, size_t len) {
    return makeUnicodeStringLen(s ? s : "", len);
}

/* ------------------------------------------------------------------ */
/* Growable byte buffer                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
    bool failed;
} StdBuf;

static void stdBufAppend(StdBuf *b, const char *s, size_t n) {
    if (b->failed || n == 0) return;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 64;
        while (cap < b->len + n + 1) {
            if (cap > SIZE_MAX / 2) { b->failed = true; return; }
            cap *= 2;
        }
        char *d = (char *)realloc(b->data, cap);
        if (!d) { b->failed = true; return; }
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static Value stdBufToText(StdBuf *b) {
    Value v = stdTextValueLen(b->data ? b->data : "", b->len);
    free(b->data);
    b->data = NULL;
    return v;
}

/* Results larger than this are refused rather than exhausting memory. */
#define STD_MAX_TEXT_BYTES ((size_t)256 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* Element kinds and comparison                                        */
/* ------------------------------------------------------------------ */

typedef enum { STD_K_INT, STD_K_REAL, STD_K_TEXT, STD_K_OTHER } StdKind;

static StdKind stdKind(const Value *v) {
    VarType t = VALUE_TYPE(*v);
    if (isPascalStringType(t) || isPascalCharType(t)) return STD_K_TEXT;
    if (isRealType(t)) return STD_K_REAL;
    if (isIntlikeType(t)) return STD_K_INT;
    return STD_K_OTHER;
}

static int stdCompareText(const Value *a, const Value *b) {
    char sa[5], sb[5];
    const char *x = stdText((Value *)a, sa);
    const char *y = stdText((Value *)b, sb);
    int c = strcmp(x ? x : "", y ? y : "");
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

/* Total order on two scalars of compatible kinds: Int/Real numerically, Text
 * by UTF-8 bytes (which is codepoint order). *ok is cleared for anything
 * else. */
static int stdCompare(const Value *a, const Value *b, bool *ok) {
    StdKind ka = stdKind(a), kb = stdKind(b);
    if (ka == STD_K_TEXT && kb == STD_K_TEXT) return stdCompareText(a, b);
    if (ka == STD_K_INT && kb == STD_K_INT) {
        long long x = asI64(*a), y = asI64(*b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if ((ka == STD_K_INT || ka == STD_K_REAL) && (kb == STD_K_INT || kb == STD_K_REAL)) {
        long double x = asLd(*a), y = asLd(*b);
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    *ok = false;
    return 0;
}

/* Equality for contains/index_of/count: scalars by value. */
static bool stdEqual(VM *vm, const char *who, const Value *a, const Value *b, bool *ok) {
    bool cmpOk = true;
    int c = stdCompare(a, b, &cmpOk);
    if (!cmpOk) {
        if (*ok) {
            runtimeError(vm, "%s compares Int, Real, Text or Bool values only.", who);
        }
        *ok = false;
        return false;
    }
    return c == 0;
}

/* ------------------------------------------------------------------ */
/* UTF-8                                                               */
/* ------------------------------------------------------------------ */

static size_t stdCodepoints(const char *s, size_t len) {
    return utf8CodepointCount(s, len);
}

/* Length in bytes of the UTF-8 sequence starting at s[0] (1 for a stray
 * continuation byte, so malformed input still advances). */
static size_t stdUtf8SeqLen(const unsigned char *s, size_t avail) {
    size_t n = 1;
    if (s[0] >= 0xF0) n = 4;
    else if (s[0] >= 0xE0) n = 3;
    else if (s[0] >= 0xC0) n = 2;
    if (n > avail) n = avail;
    for (size_t i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return i;
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* Text helpers (pure)                                                 */
/* ------------------------------------------------------------------ */

static Value stdCaseMap(VM *vm, int arg_count, Value *args, bool toUpper, const char *who) {
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument (text).", who);
        return stdTextValue("");
    }
    char sb[5];
    const char *s = stdText(&args[0], sb);
    if (!s) {
        runtimeError(vm, "%s expects a Text argument.", who);
        return stdTextValue("");
    }
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) {
        runtimeError(vm, "%s: out of memory.", who);
        return stdTextValue("");
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (toUpper && c >= 'a' && c <= 'z') c = (unsigned char)(c - 'a' + 'A');
        else if (!toUpper && c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        out[i] = (char)c;
    }
    out[n] = '\0';
    Value v = stdTextValueLen(out, n);
    free(out);
    return v;
}

static Value vmBuiltinStdUpper(struct VM_s *vm, int arg_count, Value *args) {
    return stdCaseMap(vm, arg_count, args, true, "upper");
}

static Value vmBuiltinStdLower(struct VM_s *vm, int arg_count, Value *args) {
    return stdCaseMap(vm, arg_count, args, false, "lower");
}

/* replace(s, old, new): every non-overlapping occurrence, left to right. An
 * empty `old` returns s unchanged. */
static Value vmBuiltinStdReplace(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 3) {
        runtimeError(vm, "replace expects 3 arguments (text, old, new).");
        return stdTextValue("");
    }
    char b0[5], b1[5], b2[5];
    const char *s = stdText(&args[0], b0);
    const char *from = stdText(&args[1], b1);
    const char *to = stdText(&args[2], b2);
    if (!s || !from || !to) {
        runtimeError(vm, "replace expects Text arguments (text, old, new).");
        return stdTextValue("");
    }
    size_t fromLen = strlen(from);
    if (fromLen == 0) return stdTextValue(s);
    size_t toLen = strlen(to);
    StdBuf b = {0};
    const char *cur = s;
    const char *hit;
    while ((hit = strstr(cur, from)) != NULL) {
        stdBufAppend(&b, cur, (size_t)(hit - cur));
        stdBufAppend(&b, to, toLen);
        cur = hit + fromLen;
        if (b.len > STD_MAX_TEXT_BYTES) { b.failed = true; break; }
    }
    stdBufAppend(&b, cur, strlen(cur));
    if (b.failed) {
        free(b.data);
        runtimeError(vm, "replace: result is too large.");
        return stdTextValue("");
    }
    return stdBufToText(&b);
}

static Value vmBuiltinStdStartsWith(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "starts_with expects 2 arguments (text, prefix).");
        return makeBoolean(0);
    }
    char b0[5], b1[5];
    const char *s = stdText(&args[0], b0);
    const char *p = stdText(&args[1], b1);
    if (!s || !p) {
        runtimeError(vm, "starts_with expects Text arguments.");
        return makeBoolean(0);
    }
    size_t pl = strlen(p);
    return makeBoolean(strncmp(s, p, pl) == 0);
}

static Value vmBuiltinStdEndsWith(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "ends_with expects 2 arguments (text, suffix).");
        return makeBoolean(0);
    }
    char b0[5], b1[5];
    const char *s = stdText(&args[0], b0);
    const char *p = stdText(&args[1], b1);
    if (!s || !p) {
        runtimeError(vm, "ends_with expects Text arguments.");
        return makeBoolean(0);
    }
    size_t sl = strlen(s), pl = strlen(p);
    return makeBoolean(pl <= sl && memcmp(s + sl - pl, p, pl) == 0);
}

/* Character classes (Aether D67): is_digit(t), is_alpha(t), ... are true when
 * t is non-empty and every character of t is in the class, so one test reads
 * both `is_digit(s[i])` and `is_digit("123")`, as Python's str.isdigit() does.
 * ASCII only and decided here, byte by byte, never by the host's <ctype.h>
 * locale: a non-ASCII character (a UTF-8 lead or continuation byte) is in no
 * class. */
typedef enum {
    STD_CC_DIGIT, STD_CC_ALPHA, STD_CC_ALNUM, STD_CC_SPACE, STD_CC_UPPER, STD_CC_LOWER
} StdCharClass;

static bool stdCharInClass(unsigned char c, StdCharClass cls) {
    bool upper = c >= 'A' && c <= 'Z';
    bool lower = c >= 'a' && c <= 'z';
    bool digit = c >= '0' && c <= '9';
    switch (cls) {
        case STD_CC_DIGIT: return digit;
        case STD_CC_ALPHA: return upper || lower;
        case STD_CC_ALNUM: return upper || lower || digit;
        case STD_CC_SPACE:
            return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
        case STD_CC_UPPER: return upper;
        case STD_CC_LOWER: return lower;
    }
    return false;
}

static Value stdCharClassTest(struct VM_s *vm, int arg_count, Value *args,
                              const char *who, StdCharClass cls) {
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument (text).", who);
        return makeBoolean(0);
    }
    char b0[5];
    const char *s = stdText(&args[0], b0);
    if (!s) {
        runtimeError(vm, "%s expects a Text argument.", who);
        return makeBoolean(0);
    }
    if (!*s) return makeBoolean(0);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!stdCharInClass(*p, cls)) return makeBoolean(0);
    }
    return makeBoolean(1);
}

/* same(a, b): record identity (D24). `==` on records compares their fields
 * under Aether; `same` answers whether a and b are the one record. nil is the
 * same as nil and as nothing else. */
static Value vmBuiltinStdSame(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "same expects 2 arguments.");
        return makeBoolean(false);
    }
    VarType ta = VALUE_TYPE(args[0]), tb = VALUE_TYPE(args[1]);
    bool aRef = (ta == TYPE_POINTER || ta == TYPE_NIL);
    bool bRef = (tb == TYPE_POINTER || tb == TYPE_NIL);
    if (!aRef || !bRef) {
        runtimeError(vm, "same expects two records.");
        return makeBoolean(false);
    }
    const void *pa = (ta == TYPE_POINTER) ? (const void *)AS_POINTER(args[0]) : NULL;
    const void *pb = (tb == TYPE_POINTER) ? (const void *)AS_POINTER(args[1]) : NULL;
    return makeBoolean(pa == pb);
}

static Value vmBuiltinStdIsDigit(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_digit", STD_CC_DIGIT);
}

static Value vmBuiltinStdIsAlpha(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_alpha", STD_CC_ALPHA);
}

static Value vmBuiltinStdIsAlnum(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_alnum", STD_CC_ALNUM);
}

static Value vmBuiltinStdIsSpace(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_space", STD_CC_SPACE);
}

static Value vmBuiltinStdIsUpper(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_upper", STD_CC_UPPER);
}

static Value vmBuiltinStdIsLower(struct VM_s *vm, int arg_count, Value *args) {
    return stdCharClassTest(vm, arg_count, args, "is_lower", STD_CC_LOWER);
}

/* repeat(s, n): s written n times; "" when n <= 0. */
static Value vmBuiltinStdRepeat(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "repeat expects 2 arguments (text, count).");
        return stdTextValue("");
    }
    char b0[5];
    const char *s = stdText(&args[0], b0);
    Value *nv = stdArg(&args[1]);
    if (!s || !IS_INTLIKE(*nv)) {
        runtimeError(vm, "repeat expects (Text, Int).");
        return stdTextValue("");
    }
    long long n = asI64(*nv);
    size_t sl = strlen(s);
    if (n <= 0 || sl == 0) return stdTextValue("");
    if ((unsigned long long)n > STD_MAX_TEXT_BYTES / sl) {
        runtimeError(vm, "repeat: result is too large (%lld copies).", n);
        return stdTextValue("");
    }
    size_t total = sl * (size_t)n;
    char *out = (char *)malloc(total + 1);
    if (!out) {
        runtimeError(vm, "repeat: out of memory.");
        return stdTextValue("");
    }
    for (long long i = 0; i < n; i++) memcpy(out + (size_t)i * sl, s, sl);
    out[total] = '\0';
    Value v = stdTextValueLen(out, total);
    free(out);
    return v;
}

/* join(xs, sep): the Text elements of xs with sep between them. */
static Value vmBuiltinStdJoin(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "join expects 2 arguments (items, separator).");
        return stdTextValue("");
    }
    Value *arr = stdArg(&args[0]);
    char sbuf[5];
    const char *sep = stdText(&args[1], sbuf);
    Value *elems = NULL;
    int n = 0;
    if (!sep) {
        runtimeError(vm, "join expects a Text separator.");
        return stdTextValue("");
    }
    if (!stdArrayView(vm, "join", arr, &elems, &n)) return stdTextValue("");
    size_t sepLen = strlen(sep);
    StdBuf b = {0};
    for (int i = 0; i < n; i++) {
        char eb[5];
        const char *e = stdText(&elems[i], eb);
        if (!e) {
            free(b.data);
            runtimeError(vm, "join expects a Text[]; convert numbers first with int_to_text or formatfloat.");
            return stdTextValue("");
        }
        if (i > 0) stdBufAppend(&b, sep, sepLen);
        stdBufAppend(&b, e, strlen(e));
        if (b.len > STD_MAX_TEXT_BYTES) b.failed = true;
        if (b.failed) break;
    }
    if (b.failed) {
        free(b.data);
        runtimeError(vm, "join: result is too large.");
        return stdTextValue("");
    }
    return stdBufToText(&b);
}

/* ------------------------------------------------------------------ */
/* Helpers shared by Text and arrays: contains, index_of, count,       */
/* reversed                                                            */
/* ------------------------------------------------------------------ */

static Value vmBuiltinStdContains(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "contains expects 2 arguments (collection, value).");
        return makeBoolean(0);
    }
    Value *hay = stdArg(&args[0]);
    if (stdIsArray(hay)) {
        Value *elems = NULL;
        int n = 0;
        if (!stdArrayView(vm, "contains", hay, &elems, &n)) return makeBoolean(0);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            if (stdEqual(vm, "contains", &elems[i], &args[1], &ok)) return makeBoolean(1);
        }
        return makeBoolean(0);
    }
    char b0[5], b1[5];
    const char *s = stdText(hay, b0);
    const char *x = stdText(&args[1], b1);
    if (!s || !x) {
        runtimeError(vm, "contains expects (Text, Text) or (array, value).");
        return makeBoolean(0);
    }
    return makeBoolean(strstr(s, x) != NULL);
}

static Value vmBuiltinStdIndexOf(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "index_of expects 2 arguments (collection, value).");
        return makeInt64(-1);
    }
    Value *hay = stdArg(&args[0]);
    if (stdIsArray(hay)) {
        Value *elems = NULL;
        int n = 0;
        if (!stdArrayView(vm, "index_of", hay, &elems, &n)) return makeInt64(-1);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            if (stdEqual(vm, "index_of", &elems[i], &args[1], &ok)) return makeInt64(i);
        }
        return makeInt64(-1);
    }
    char b0[5], b1[5];
    const char *s = stdText(hay, b0);
    const char *x = stdText(&args[1], b1);
    if (!s || !x) {
        runtimeError(vm, "index_of expects (Text, Text) or (array, value).");
        return makeInt64(-1);
    }
    const char *hit = strstr(s, x);
    if (!hit) return makeInt64(-1);
    return makeInt64((long long)stdCodepoints(s, (size_t)(hit - s)));
}

/* count(xs, v): elements equal to v; count(s, sub): non-overlapping
 * occurrences of sub in s (0 for an empty sub). */
static Value vmBuiltinStdCount(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "count expects 2 arguments (collection, value).");
        return makeInt64(0);
    }
    Value *hay = stdArg(&args[0]);
    long long total = 0;
    if (stdIsArray(hay)) {
        Value *elems = NULL;
        int n = 0;
        if (!stdArrayView(vm, "count", hay, &elems, &n)) return makeInt64(0);
        bool ok = true;
        for (int i = 0; i < n && ok; i++) {
            if (stdEqual(vm, "count", &elems[i], &args[1], &ok)) total++;
        }
        return makeInt64(total);
    }
    char b0[5], b1[5];
    const char *s = stdText(hay, b0);
    const char *x = stdText(&args[1], b1);
    if (!s || !x) {
        runtimeError(vm, "count expects (Text, Text) or (array, value).");
        return makeInt64(0);
    }
    size_t xl = strlen(x);
    if (xl == 0) return makeInt64(0);
    for (const char *p = s; (p = strstr(p, x)) != NULL; p += xl) total++;
    return makeInt64(total);
}

static Value stdCopyArrayShape(Value *src, int n) {
    ArrayObj *a = PSCAL_VALUE_PTR(*src, ArrayObj);
    return stdNewArray(n, a ? a->element_type : TYPE_INT64, a ? a->element_type_def : NULL);
}

static Value vmBuiltinStdReversed(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "reversed expects 1 argument.");
        return makeVoid();
    }
    Value *src = stdArg(&args[0]);
    if (stdIsArray(src)) {
        Value *elems = NULL;
        int n = 0;
        if (!stdArrayView(vm, "reversed", src, &elems, &n)) return stdNewArray(0, TYPE_INT64, NULL);
        Value out = stdCopyArrayShape(src, n);
        for (int i = 0; i < n; i++) stdArraySet(&out, i, stdDeepCopy(&elems[n - 1 - i]));
        return out;
    }
    char b0[5];
    const char *s = stdText(src, b0);
    if (!s) {
        runtimeError(vm, "reversed expects an array or Text.");
        return stdTextValue("");
    }
    size_t len = strlen(s);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        runtimeError(vm, "reversed: out of memory.");
        return stdTextValue("");
    }
    size_t w = len;
    for (size_t i = 0; i < len;) {
        size_t k = stdUtf8SeqLen((const unsigned char *)s + i, len - i);
        w -= k;
        memcpy(out + w, s + i, k);
        i += k;
    }
    out[len] = '\0';
    Value v = stdTextValueLen(out, len);
    free(out);
    return v;
}

/* ------------------------------------------------------------------ */
/* Array aggregates and sorts (pure)                                   */
/* ------------------------------------------------------------------ */

static Value vmBuiltinStdSum(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "sum expects 1 argument (an Int[] or Real[]).");
        return makeInt64(0);
    }
    Value *arr = stdArg(&args[0]);
    Value *elems = NULL;
    int n = 0;
    if (!stdArrayView(vm, "sum", arr, &elems, &n)) return makeInt64(0);
    ArrayObj *a = PSCAL_VALUE_PTR(*arr, ArrayObj);
    bool real = a && isRealType(a->element_type);
    long long isum = 0;
    long double rsum = 0.0L;
    for (int i = 0; i < n; i++) {
        StdKind k = stdKind(&elems[i]);
        if (k == STD_K_REAL) {
            real = true;
        } else if (k != STD_K_INT) {
            runtimeError(vm, "sum expects an Int[] or Real[].");
            return makeInt64(0);
        }
    }
    for (int i = 0; i < n; i++) {
        if (real) {
            rsum += asLd(elems[i]);
        } else if (__builtin_add_overflow(isum, asI64(elems[i]), &isum)) {
            runtimeError(vm, "Runtime Error: Integer overflow."); // as `+` reports it
            return makeInt64(0);
        }
    }
    if (real) return makeReal((double)rsum);
    return makeInt64(isum);
}

static Value stdExtreme(VM *vm, int arg_count, Value *args, bool wantMax, const char *who) {
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument (an array).", who);
        return makeInt64(0);
    }
    Value *arr = stdArg(&args[0]);
    Value *elems = NULL;
    int n = 0;
    if (!stdArrayView(vm, who, arr, &elems, &n)) return makeInt64(0);
    if (n == 0) {
        runtimeError(vm, "%s of an empty array.", who);
        return makeInt64(0);
    }
    int best = 0;
    for (int i = 1; i < n; i++) {
        bool ok = true;
        int c = stdCompare(&elems[i], &elems[best], &ok);
        if (!ok) {
            runtimeError(vm, "%s expects an Int[], Real[] or Text[].", who);
            return makeInt64(0);
        }
        if (wantMax ? c > 0 : c < 0) best = i;
    }
    if (stdKind(&elems[best]) == STD_K_OTHER) {
        runtimeError(vm, "%s expects an Int[], Real[] or Text[].", who);
        return makeInt64(0);
    }
    return makeCopyOfValue(&elems[best]);
}

static Value vmBuiltinStdMinOf(struct VM_s *vm, int arg_count, Value *args) {
    return stdExtreme(vm, arg_count, args, false, "min_of");
}

static Value vmBuiltinStdMaxOf(struct VM_s *vm, int arg_count, Value *args) {
    return stdExtreme(vm, arg_count, args, true, "max_of");
}

/* Sort keys: one per element, taken from the element itself or from a
 * record field. A stable merge sort over indexes. */
typedef struct {
    const Value *key;
    int index;
} StdSortItem;

typedef struct {
    bool desc;
    bool ok;
} StdSortCtx;

static int stdSortCmp(const StdSortItem *x, const StdSortItem *y, StdSortCtx *ctx) {
    int c = stdCompare(x->key, y->key, &ctx->ok);
    if (ctx->desc) c = -c;
    if (c != 0) return c;
    return x->index < y->index ? -1 : (x->index > y->index ? 1 : 0);
}

static void stdMergeSort(StdSortItem *items, StdSortItem *tmp, int n, StdSortCtx *ctx) {
    for (int width = 1; width < n; width *= 2) {
        for (int lo = 0; lo < n; lo += 2 * width) {
            int mid = lo + width < n ? lo + width : n;
            int hi = lo + 2 * width < n ? lo + 2 * width : n;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) {
                if (stdSortCmp(&items[j], &items[i], ctx) < 0) tmp[k++] = items[j++];
                else tmp[k++] = items[i++];
            }
            while (i < mid) tmp[k++] = items[i++];
            while (j < hi) tmp[k++] = items[j++];
        }
        memcpy(items, tmp, sizeof(StdSortItem) * (size_t)n);
    }
}

/* The record a record-array element denotes (elements are pointers to
 * records), or NULL. */
static Value *stdRecordOf(Value *v) {
    if (!v) return NULL;
    if (VALUE_TYPE(*v) == TYPE_RECORD) return v;
    if (VALUE_TYPE(*v) == TYPE_POINTER && AS_POINTER(*v)) {
        Value *p = (Value *)AS_POINTER(*v);
        if (VALUE_TYPE(*p) == TYPE_RECORD) return p;
    }
    return NULL;
}

/* A record field by slot (Aether passes the slot its front end resolved
 * from the literal field name; slot 0 is the hidden __vtable) or by name
 * (records built by front ends that keep field names). */
typedef struct {
    int slot;          /* >= 0: match by slot */
    const char *name;  /* otherwise by name */
} StdFieldRef;

static const Value *stdRecordField(Value *elem, const StdFieldRef *ref) {
    Value *rec = stdRecordOf(elem);
    if (!rec) return NULL;
    if (ref->slot >= 0) {
        bool explicitSlots = false;
        int ordinal = 0;
        for (FieldValue *f = AS_RECORD(*rec); f; f = f->next, ordinal++) {
            if (f->slot_index >= 0) explicitSlots = true;
            if ((f->slot_index >= 0 && f->slot_index == ref->slot) ||
                (!explicitSlots && f->slot_index < 0 && ordinal == ref->slot)) {
                return f->storage ? f->storage : &f->value;
            }
        }
        return NULL;
    }
    for (FieldValue *f = AS_RECORD(*rec); f; f = f->next) {
        if (f->name && strcmp(f->name, ref->name) == 0) return f->storage ? f->storage : &f->value;
    }
    for (FieldValue *f = AS_RECORD(*rec); f; f = f->next) {
        if (f->name && strcasecmp(f->name, ref->name) == 0) return f->storage ? f->storage : &f->value;
    }
    return NULL;
}

static Value stdSortedImpl(VM *vm, Value *arrArg, const StdFieldRef *field, bool desc, const char *who) {
    Value *arr = stdArg(arrArg);
    Value *elems = NULL;
    int n = 0;
    if (!stdArrayView(vm, who, arr, &elems, &n)) return stdNewArray(0, TYPE_INT64, NULL);
    Value out = stdCopyArrayShape(arr, n);
    if (n == 0) return out;
    StdSortItem *items = (StdSortItem *)malloc(sizeof(StdSortItem) * (size_t)n);
    StdSortItem *tmp = (StdSortItem *)malloc(sizeof(StdSortItem) * (size_t)n);
    if (!items || !tmp) {
        free(items);
        free(tmp);
        runtimeError(vm, "%s: out of memory.", who);
        return out;
    }
    for (int i = 0; i < n; i++) {
        const Value *key = &elems[i];
        if (field) {
            key = stdRecordField(&elems[i], field);
            if (!key) {
                free(items);
                free(tmp);
                if (field->slot >= 0) runtimeError(vm, "%s: element %d has no field slot %d.", who, i, field->slot);
                else runtimeError(vm, "%s: element %d has no field '%s'.", who, i, field->name);
                return out;
            }
        }
        if (stdKind(key) == STD_K_OTHER) {
            free(items);
            free(tmp);
            if (field) runtimeError(vm, "%s: the sort field is not an Int, Real or Text.", who);
            else runtimeError(vm, "%s expects an Int[], Real[] or Text[].", who);
            return out;
        }
        items[i].key = key;
        items[i].index = i;
    }
    StdSortCtx ctx = { desc, true };
    stdMergeSort(items, tmp, n, &ctx);
    if (!ctx.ok) {
        free(items);
        free(tmp);
        runtimeError(vm, "%s: elements of different kinds cannot be compared.", who);
        return out;
    }
    for (int i = 0; i < n; i++) stdArraySet(&out, i, makeCopyOfValue(&elems[items[i].index]));
    free(items);
    free(tmp);
    return out;
}

static Value vmBuiltinStdSorted(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "sorted expects 1 argument (an array).");
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    return stdSortedImpl(vm, &args[0], NULL, false, "sorted");
}

static Value vmBuiltinStdSortedDesc(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "sorted_desc expects 1 argument (an array).");
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    return stdSortedImpl(vm, &args[0], NULL, true, "sorted_desc");
}

static Value stdSortedBy(VM *vm, int arg_count, Value *args, bool desc, const char *who) {
    if (arg_count != 2) {
        runtimeError(vm, "%s expects 2 arguments (records, \"field\").", who);
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    StdFieldRef ref = { -1, NULL };
    Value *fv = stdArg(&args[1]);
    char fb[5];
    if (IS_INTLIKE(*fv) && !isPascalCharType(VALUE_TYPE(*fv))) {
        ref.slot = (int)asI64(*fv);
    } else {
        ref.name = stdText(fv, fb);
        if (!ref.name || !*ref.name) {
            runtimeError(vm, "%s expects a field name as its second argument.", who);
            return stdNewArray(0, TYPE_INT64, NULL);
        }
    }
    return stdSortedImpl(vm, &args[0], &ref, desc, who);
}

static Value vmBuiltinStdSortedBy(struct VM_s *vm, int arg_count, Value *args) {
    return stdSortedBy(vm, arg_count, args, false, "sorted_by");
}

static Value vmBuiltinStdSortedByDesc(struct VM_s *vm, int arg_count, Value *args) {
    return stdSortedBy(vm, arg_count, args, true, "sorted_by_desc");
}

/* ------------------------------------------------------------------ */
/* Checked parsing (pure). Aether's try_parse_int/try_parse_real are   */
/* front-end functions over these four hidden builtins.                */
/* ------------------------------------------------------------------ */

static bool stdIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/* Decimal integer with optional surrounding whitespace and sign. */
static bool stdParseInt(const char *s, long long *out) {
    *out = 0;
    if (!s) return false;
    while (stdIsSpace(*s)) s++;
    bool neg = false;
    if (*s == '+' || *s == '-') {
        neg = (*s == '-');
        s++;
    }
    if (!(*s >= '0' && *s <= '9')) return false;
    unsigned long long mag = 0;
    const unsigned long long limit = neg ? (unsigned long long)LLONG_MAX + 1ULL
                                         : (unsigned long long)LLONG_MAX;
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned)(*s - '0');
        if (mag > (limit - d) / 10ULL) return false;
        mag = mag * 10ULL + d;
        s++;
    }
    while (stdIsSpace(*s)) s++;
    if (*s != '\0') return false;
    if (neg) {
        *out = (mag == (unsigned long long)LLONG_MAX + 1ULL) ? LLONG_MIN : -(long long)mag;
    } else {
        *out = (long long)mag;
    }
    return true;
}

/* Decimal real: [sign] digits [. digits] [e [sign] digits], at least one
 * mantissa digit, optional surrounding whitespace. No inf/nan/hex, which
 * strtod would accept differently on each libc. */
static bool stdParseReal(const char *s, double *out) {
    *out = 0.0;
    if (!s) return false;
    while (stdIsSpace(*s)) s++;
    const char *start = s;
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    int digits = 0;
    while (*p >= '0' && *p <= '9') { p++; digits++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { p++; digits++; }
    }
    if (digits == 0) return false;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        if (*e == '+' || *e == '-') e++;
        if (!(*e >= '0' && *e <= '9')) return false;
        while (*e >= '0' && *e <= '9') e++;
        p = e;
    }
    const char *end = p;
    while (stdIsSpace(*p)) p++;
    if (*p != '\0') return false;
    size_t n = (size_t)(end - start);
    char stackBuf[128];
    char *buf = n < sizeof(stackBuf) ? stackBuf : (char *)malloc(n + 1);
    if (!buf) return false;
    memcpy(buf, start, n);
    buf[n] = '\0';
    errno = 0;
    double v = strtod(buf, NULL);
    int err = errno;
    if (buf != stackBuf) free(buf);
    if (err == ERANGE && (v == HUGE_VAL || v == -HUGE_VAL)) return false;
    if (isinf(v) || isnan(v)) return false;
    *out = v;
    return true;
}

static const char *stdParseArg(VM *vm, int arg_count, Value *args, const char *who) {
    if (arg_count != 1) {
        runtimeError(vm, "%s expects 1 argument (text).", who);
        return NULL;
    }
    static __thread char sb[5];
    const char *s = stdText(&args[0], sb);
    if (!s) runtimeError(vm, "%s expects a Text argument.", who);
    return s;
}

static Value vmBuiltinStdParseIntOk(struct VM_s *vm, int arg_count, Value *args) {
    const char *s = stdParseArg(vm, arg_count, args, "try_parse_int");
    long long v;
    return makeBoolean(s && stdParseInt(s, &v));
}

static Value vmBuiltinStdParseIntValue(struct VM_s *vm, int arg_count, Value *args) {
    const char *s = stdParseArg(vm, arg_count, args, "try_parse_int");
    long long v = 0;
    if (s) stdParseInt(s, &v);
    return makeInt64(v);
}

static Value vmBuiltinStdParseRealOk(struct VM_s *vm, int arg_count, Value *args) {
    const char *s = stdParseArg(vm, arg_count, args, "try_parse_real");
    double v;
    return makeBoolean(s && stdParseReal(s, &v));
}

static Value vmBuiltinStdParseRealValue(struct VM_s *vm, int arg_count, Value *args) {
    const char *s = stdParseArg(vm, arg_count, args, "try_parse_real");
    double v = 0.0;
    if (s) stdParseReal(s, &v);
    return makeReal(v);
}

/* ------------------------------------------------------------------ */
/* round_to(x, digits): half away from zero, at `digits` decimals       */
/* ------------------------------------------------------------------ */

/* The schoolbook rule a spec usually means: 2.675 -> 2.68 and 0.125 ->
 * 0.13, although neither is exactly representable and the nearest doubles
 * lie just below the half. A scaled value within a few ulps of .5 counts as
 * the half. `digits` may be negative (round_to(1250.0, -2) is 1300.0). */
static Value vmBuiltinStdRoundTo(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "round_to expects 2 arguments (value, digits).");
        return makeReal(0.0);
    }
    Value *xv = stdArg(&args[0]);
    Value *dv = stdArg(&args[1]);
    if (!IS_NUMERIC(*xv) || !IS_INTLIKE(*dv)) {
        runtimeError(vm, "round_to expects (Real, Int).");
        return makeReal(0.0);
    }
    double x = (double)asLd(*xv);
    long long d = asI64(*dv);
    if (d > 15) d = 15;
    if (d < -15) d = -15;
    if (isnan(x) || isinf(x)) return makeReal(x);
    double scale = 1.0;
    for (long long i = 0; i < (d < 0 ? -d : d); i++) scale *= 10.0;
    double y = d >= 0 ? x * scale : x / scale;
    double ay = fabs(y);
    if (ay >= 4503599627370496.0) return makeReal(x); /* 2^52: already whole */
    double whole = floor(ay);
    double frac = ay - whole;
    double ulp = nextafter(ay, INFINITY) - ay;
    double tol = 4.0 * ulp;
    if (frac >= 0.5 - tol) whole += 1.0;
    double r = d >= 0 ? whole / scale : whole * scale;
    if (y < 0) r = -r;
    if (r == 0.0) r = 0.0; /* no negative zero */
    return makeReal(r);
}

/* ------------------------------------------------------------------ */
/* Files (effectful)                                                    */
/* ------------------------------------------------------------------ */

static const char *stdErrnoText(int err) {
    switch (err) {
        case ENOENT: return "no such file or directory";
        case EACCES: return "permission denied";
        case EISDIR: return "is a directory";
        case ENOTDIR: return "a path component is not a directory";
        case ENOSPC: return "no space left on device";
        case EEXIST: return "file exists";
        default: return "input/output error";
    }
}

static bool stdReadWholeFile(VM *vm, const char *who, const char *path, StdBuf *b) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        runtimeError(vm, "%s: cannot open '%s': %s.", who, path, stdErrnoText(errno));
        return false;
    }
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        stdBufAppend(b, chunk, got);
        if (b->failed || b->len > STD_MAX_TEXT_BYTES) {
            fclose(f);
            free(b->data);
            b->data = NULL;
            runtimeError(vm, "%s: '%s' is too large.", who, path);
            return false;
        }
    }
    bool bad = ferror(f) != 0;
    int err = errno;
    fclose(f);
    if (bad) {
        free(b->data);
        b->data = NULL;
        runtimeError(vm, "%s: cannot read '%s': %s.", who, path, stdErrnoText(err));
        return false;
    }
    return true;
}

static const char *stdPathArg(VM *vm, Value *v, const char *who, char scratch5[5]) {
    const char *p = stdText(v, scratch5);
    if (!p) runtimeError(vm, "%s expects a Text path.", who);
    return p;
}

static Value vmBuiltinStdReadFile(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "read_file expects 1 argument (path).");
        return stdTextValue("");
    }
    char pb[5];
    const char *path = stdPathArg(vm, &args[0], "read_file", pb);
    if (!path) return stdTextValue("");
    StdBuf b = {0};
    if (!stdReadWholeFile(vm, "read_file", path, &b)) return stdTextValue("");
    return stdBufToText(&b);
}

/* read_lines(path): the file's lines without their "\n" or "\r\n"; a final
 * line break does not start an extra empty line. */
static Value vmBuiltinStdReadLines(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "read_lines expects 1 argument (path).");
        return stdNewArray(0, TYPE_UNICODE_STRING, NULL);
    }
    char pb[5];
    const char *path = stdPathArg(vm, &args[0], "read_lines", pb);
    if (!path) return stdNewArray(0, TYPE_UNICODE_STRING, NULL);
    StdBuf b = {0};
    if (!stdReadWholeFile(vm, "read_lines", path, &b)) {
        return stdNewArray(0, TYPE_UNICODE_STRING, NULL);
    }
    const char *data = b.data ? b.data : "";
    size_t len = b.len;
    int lines = 0;
    for (size_t i = 0; i < len; i++) if (data[i] == '\n') lines++;
    if (len > 0 && data[len - 1] != '\n') lines++;
    if (lines > 2000000) {
        free(b.data);
        runtimeError(vm, "read_lines: '%s' has too many lines.", path);
        return stdNewArray(0, TYPE_UNICODE_STRING, NULL);
    }
    Value out = stdNewArray(lines, TYPE_UNICODE_STRING, NULL);
    size_t start = 0;
    int idx = 0;
    for (size_t i = 0; i <= len && idx < lines; i++) {
        if (i == len || data[i] == '\n') {
            size_t end = i;
            if (end > start && data[end - 1] == '\r') end--;
            stdArraySet(&out, idx++, stdTextValueLen(data + start, end - start));
            start = i + 1;
        }
    }
    free(b.data);
    return out;
}

static Value stdWriteImpl(VM *vm, int arg_count, Value *args, const char *mode, const char *who) {
    if (arg_count != 2) {
        runtimeError(vm, "%s expects 2 arguments (path, text).", who);
        return makeVoid();
    }
    char pb[5], tb[5];
    const char *path = stdPathArg(vm, &args[0], who, pb);
    const char *text = stdText(&args[1], tb);
    if (!path) return makeVoid();
    if (!text) {
        runtimeError(vm, "%s expects Text to write.", who);
        return makeVoid();
    }
    FILE *f = fopen(path, mode);
    if (!f) {
        runtimeError(vm, "%s: cannot open '%s': %s.", who, path, stdErrnoText(errno));
        return makeVoid();
    }
    size_t n = strlen(text);
    bool bad = n > 0 && fwrite(text, 1, n, f) != n;
    int err = errno;
    if (fclose(f) != 0 && !bad) {
        bad = true;
        err = errno;
    }
    if (bad) runtimeError(vm, "%s: cannot write '%s': %s.", who, path, stdErrnoText(err));
    return makeVoid();
}

static Value vmBuiltinStdWriteFile(struct VM_s *vm, int arg_count, Value *args) {
    return stdWriteImpl(vm, arg_count, args, "wb", "write_file");
}

static Value vmBuiltinStdAppendFile(struct VM_s *vm, int arg_count, Value *args) {
    return stdWriteImpl(vm, arg_count, args, "ab", "append_file");
}

static Value vmBuiltinStdDeleteFile(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "delete_file expects 1 argument (path).");
        return makeBoolean(0);
    }
    char pb[5];
    const char *path = stdPathArg(vm, &args[0], "delete_file", pb);
    if (!path) return makeBoolean(0);
    return makeBoolean(remove(path) == 0);
}

static Value vmBuiltinStdRenameFile(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "rename_file expects 2 arguments (from, to).");
        return makeBoolean(0);
    }
    char ab[5], bb[5];
    const char *from = stdPathArg(vm, &args[0], "rename_file", ab);
    const char *to = stdPathArg(vm, &args[1], "rename_file", bb);
    if (!from || !to) return makeBoolean(0);
    return makeBoolean(rename(from, to) == 0);
}

/* ------------------------------------------------------------------ */
/* Arguments and the clock (effectful)                                  */
/* ------------------------------------------------------------------ */

/* args(): the command-line arguments after the program, as Text[]. */
static Value vmBuiltinStdArgs(struct VM_s *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "args expects no arguments.");
        return stdNewArray(0, TYPE_UNICODE_STRING, NULL);
    }
    int n = gParamCount > 0 && gParamValues ? gParamCount : 0;
    Value out = stdNewArray(n, TYPE_UNICODE_STRING, NULL);
    for (int i = 0; i < n; i++) stdArraySet(&out, i, stdTextValue(gParamValues[i]));
    return out;
}

/* clock_ms(): milliseconds since 1970-01-01 00:00 UTC. */
static Value vmBuiltinStdClockMs(struct VM_s *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "clock_ms expects no arguments.");
        return makeInt64(0);
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return makeInt64((long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000));
}

/* now_text(): local date and time as "YYYY-MM-DD HH:MM:SS". */
static Value vmBuiltinStdNowText(struct VM_s *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "now_text expects no arguments.");
        return stdTextValue("");
    }
    time_t t = time(NULL);
    struct tm tmv;
    char buf[64];
    if (!localtime_r(&t, &tmv)) return stdTextValue("");
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tmv.tm_year + 1900,
             tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return stdTextValue(buf);
}

/* ------------------------------------------------------------------ */
/* MStream verbs                                                        */
/* ------------------------------------------------------------------ */

static MStream *stdMStreamArg(VM *vm, Value *v, const char *who) {
    v = stdArg(v);
    if (!v || VALUE_TYPE(*v) != TYPE_MEMORYSTREAM || !AS_MSTREAM(*v)) {
        runtimeError(vm, "%s expects an MStream (from mstream_new()).", who);
        return NULL;
    }
    return AS_MSTREAM(*v);
}

static bool stdMStreamAppend(MStream *ms, const char *data, size_t n) {
    if (n == 0) return true;
    if ((size_t)ms->size + n + 1 > (size_t)INT_MAX) return false;
    if ((size_t)ms->capacity < (size_t)ms->size + n + 1) {
        size_t cap = ms->capacity > 0 ? (size_t)ms->capacity : 64;
        while (cap < (size_t)ms->size + n + 1) cap *= 2;
        if (cap > (size_t)INT_MAX) cap = (size_t)INT_MAX;
        unsigned char *nb = (unsigned char *)realloc(ms->buffer, cap);
        if (!nb) return false;
        ms->buffer = nb;
        ms->capacity = (int)cap;
    }
    memcpy(ms->buffer + ms->size, data, n);
    ms->size += (int)n;
    ms->buffer[ms->size] = '\0';
    return true;
}

/* Aether's mstream_write_text(ms, t) (D66; it was mstream_append_text). The
 * engine-private name below keeps "append" so an Aether front end of either
 * age resolves against this engine. */
static Value vmBuiltinStdMStreamAppendText(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "mstream_write_text expects 2 arguments (ms, text).");
        return makeVoid();
    }
    MStream *ms = stdMStreamArg(vm, &args[0], "mstream_write_text");
    char tb[5];
    const char *t = stdText(&args[1], tb);
    if (!ms) return makeVoid();
    if (!t) {
        runtimeError(vm, "mstream_write_text expects Text to write.");
        return makeVoid();
    }
    if (!stdMStreamAppend(ms, t, strlen(t))) runtimeError(vm, "mstream_write_text: out of memory.");
    return makeVoid();
}

static Value vmBuiltinStdMStreamLength(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "mstream_length expects 1 argument (ms).");
        return makeInt64(0);
    }
    MStream *ms = stdMStreamArg(vm, &args[0], "mstream_length");
    return makeInt64(ms ? ms->size : 0);
}

/* mstream_load(path): a new MStream holding the file's bytes. */
static Value vmBuiltinStdMStreamLoad(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        runtimeError(vm, "mstream_load expects 1 argument (path).");
        return makeMStream(createMStream());
    }
    char pb[5];
    const char *path = stdPathArg(vm, &args[0], "mstream_load", pb);
    MStream *ms = createMStream();
    if (!ms) {
        runtimeError(vm, "mstream_load: out of memory.");
        return makeMStream(NULL);
    }
    if (!path) return makeMStream(ms);
    StdBuf b = {0};
    if (!stdReadWholeFile(vm, "mstream_load", path, &b)) return makeMStream(ms);
    if (b.data) {
        ms->buffer = (unsigned char *)b.data;
        ms->size = (int)b.len;
        ms->capacity = (int)(b.cap > (size_t)INT_MAX ? (size_t)INT_MAX : b.cap);
    }
    return makeMStream(ms);
}

/* ------------------------------------------------------------------ */
/* [v; n]: n copies of v (Aether lowers the literal to this call)       */
/* ------------------------------------------------------------------ */

/* The element as an Aether value: an Int literal arrives 32-bit and a
 * one-character Text literal as a Char, so normalise to 64-bit Int and
 * Text; everything else is copied as Aether copies it. */
static Value stdAetherElement(const Value *v) {
    VarType t = VALUE_TYPE(*v);
    if (isIntegerFamilyType(t)) return makeInt64(asI64(*v));
    if (t == TYPE_CHAR || t == TYPE_WIDECHAR) {
        char sb[5];
        const char *s = stdText((Value *)v, sb);
        return stdTextValue(s ? s : "");
    }
    return stdDeepCopy(v);
}

static Value vmBuiltinStdRepeatArray(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 2) {
        runtimeError(vm, "[v; n] expects a value and a count.");
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    Value *nv = stdArg(&args[1]);
    if (!IS_INTLIKE(*nv) || isPascalCharType(VALUE_TYPE(*nv)) ||
        VALUE_TYPE(*nv) == TYPE_BOOLEAN) {
        runtimeError(vm, "[v; n]: the count n must be an Int.");
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    long long n = asI64(*nv);
    if (n < 0) n = 0;
    if (n > 2000000) {
        runtimeError(vm, "[v; n]: %lld elements is more than an array may hold (2,000,000).", n);
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    Value *v = &args[0];
    if (VALUE_TYPE(*v) == TYPE_POINTER && AS_POINTER(*v)) {
        Value *pv = (Value *)AS_POINTER(*v);
        if (VALUE_TYPE(*pv) == TYPE_ARRAY) v = pv;
    }
    if (stdRecordOf(v)) {
        runtimeError(vm, "[v; n] would put the same record in every slot; build the records in a loop.");
        return stdNewArray(0, TYPE_INT64, NULL);
    }
    Value first = stdAetherElement(v);
    Value out = stdNewArray((int)n, VALUE_TYPE(first), NULL);
    if (n == 0) {
        freeValue(&first);
        return out;
    }
    stdArraySet(&out, 0, first);
    for (long long i = 1; i < n; i++) stdArraySet(&out, (int)i, stdAetherElement(v));
    return out;
}

/* Registered under Aether-private engine names (`aether_upper`, ...): the
 * Aether front end aliases its surface names (`upper`, `count`, `sum`) to
 * them. Plain names would join every front end's namespace, and Pascal
 * resolves a builtin's name ahead of a program's own variable of that name
 * (a `count` or `sum` local stopped compiling). */
void registerStdlibBuiltins(void) {
    /* Text */
    registerVmBuiltin("aether_upper", vmBuiltinStdUpper, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_lower", vmBuiltinStdLower, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_replace", vmBuiltinStdReplace, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_starts_with", vmBuiltinStdStartsWith, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_ends_with", vmBuiltinStdEndsWith, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_repeat", vmBuiltinStdRepeat, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_join", vmBuiltinStdJoin, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    /* Character classes (D67) */
    registerVmBuiltin("aether_is_digit", vmBuiltinStdIsDigit, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_is_alpha", vmBuiltinStdIsAlpha, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_is_alnum", vmBuiltinStdIsAlnum, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_is_space", vmBuiltinStdIsSpace, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_is_upper", vmBuiltinStdIsUpper, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_is_lower", vmBuiltinStdIsLower, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    /* Text and arrays */
    registerVmBuiltin("aether_contains", vmBuiltinStdContains, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_index_of", vmBuiltinStdIndexOf, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_count", vmBuiltinStdCount, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_repeat_array", vmBuiltinStdRepeatArray, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_reversed", vmBuiltinStdReversed, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    /* Arrays */
    registerVmBuiltin("aether_sum", vmBuiltinStdSum, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_min_of", vmBuiltinStdMinOf, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_max_of", vmBuiltinStdMaxOf, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_sorted", vmBuiltinStdSorted, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_sorted_desc", vmBuiltinStdSortedDesc, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_sorted_by", vmBuiltinStdSortedBy, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_sorted_by_desc", vmBuiltinStdSortedByDesc, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    /* Parsing (hidden: the surface is try_parse_int / try_parse_real) */
    registerVmBuiltin("aetherparseintok", vmBuiltinStdParseIntOk, BUILTIN_TYPE_FUNCTION, "aetherparseintok", FX_PURE);
    registerVmBuiltin("aetherparseintvalue", vmBuiltinStdParseIntValue, BUILTIN_TYPE_FUNCTION, "aetherparseintvalue", FX_PURE);
    registerVmBuiltin("aetherparserealok", vmBuiltinStdParseRealOk, BUILTIN_TYPE_FUNCTION, "aetherparserealok", FX_PURE);
    registerVmBuiltin("aetherparserealvalue", vmBuiltinStdParseRealValue, BUILTIN_TYPE_FUNCTION, "aetherparserealvalue", FX_PURE);
    /* Numbers */
    registerVmBuiltin("aether_round_to", vmBuiltinStdRoundTo, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    /* Files */
    registerVmBuiltin("aether_read_file", vmBuiltinStdReadFile, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    registerVmBuiltin("aether_read_lines", vmBuiltinStdReadLines, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    registerVmBuiltin("aether_write_file", vmBuiltinStdWriteFile, BUILTIN_TYPE_PROCEDURE, NULL, FX_IO);
    registerVmBuiltin("aether_append_file", vmBuiltinStdAppendFile, BUILTIN_TYPE_PROCEDURE, NULL, FX_IO);
    registerVmBuiltin("aether_delete_file", vmBuiltinStdDeleteFile, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    registerVmBuiltin("aether_rename_file", vmBuiltinStdRenameFile, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    /* Arguments and the clock */
    registerVmBuiltin("aether_args", vmBuiltinStdArgs, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    registerVmBuiltin("aether_clock_ms", vmBuiltinStdClockMs, BUILTIN_TYPE_FUNCTION, NULL, FX_CLOCK);
    registerVmBuiltin("aether_now_text", vmBuiltinStdNowText, BUILTIN_TYPE_FUNCTION, NULL, FX_CLOCK);
    /* MStream (aether_mstream_append_text is the surface's mstream_write_text) */
    registerVmBuiltin("aether_mstream_append_text", vmBuiltinStdMStreamAppendText, BUILTIN_TYPE_PROCEDURE, NULL, FX_PURE);
    registerVmBuiltin("aether_mstream_length", vmBuiltinStdMStreamLength, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
    registerVmBuiltin("aether_mstream_load", vmBuiltinStdMStreamLoad, BUILTIN_TYPE_FUNCTION, NULL, FX_IO);
    /* Record identity (D24); appended last so no earlier builtin id moves. */
    registerVmBuiltin("aether_same", vmBuiltinStdSame, BUILTIN_TYPE_FUNCTION, NULL, FX_PURE);
}
