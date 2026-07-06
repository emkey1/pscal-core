#ifndef UTILS_H
#define UTILS_H
#include "core/type_registry.h"
#include "symbol/symbol.h"
#include "vm/vm.h"

#define OWNED_POINTER_SENTINEL ((AST*)(uintptr_t)(-1))

#define PASCAL_DEFAULT_FLOAT_PRECISION 6
/* =======================
   DEBUG MACROS & GLOBALS
   ======================= */
#ifdef DEBUG
    // In DEBUG builds, use the fprintf logic if dumpExec is enabled
    extern int dumpExec;   /* Global flag for execution debug dump */
    #define DEBUG_PRINT(...) do { if(dumpExec) fprintf(stderr, __VA_ARGS__); } while(0)

    // DEBUG_DUMP_AST will call the debugAST function
    #define DEBUG_DUMP_AST(node, indent) debugAST((node), (indent))
#else
    // In non-DEBUG builds, define DEBUG_PRINT to expand to nothing.
    // The compiler will simply remove any line like DEBUG_PRINT(...)
    #define DEBUG_PRINT(...)

    // DEBUG_DUMP_AST should still be defined as a no-op that takes the arguments
    #define DEBUG_DUMP_AST(node, indent) ((void)0)
#endif

#include "types.h"

// Pascal traditionally models the CHAR type as an 8-bit ordinal with a
// maximum value of 255.  Text output can still render those 8-bit values as
// UTF-8 (for example by mapping the classic CP437 graphics range), but the
// language-level CHAR ordinal range remains 0..255 for compatibility.
#define UNICODE_MAX 0x10FFFF
#define PASCAL_CHAR_MAX 255
// Bytecode related stuff
// Make sure Value and VarType are defined before these.
#define IS_BOOLEAN(value) (VALUE_TYPE(value) == TYPE_BOOLEAN)
#define AS_BOOLEAN(value) (VAL_INT(value) != 0) // Assumes i_val stores 0 for false, 1 for true

// Also useful:
#define IS_INTEGER(value) (isIntlikeType(VALUE_TYPE(value)))
#define AS_INTEGER(value) (asI64(value))
#define IS_REAL(value)    (isRealType(VALUE_TYPE(value)))
#define AS_REAL(value)    (asLd(value))
#define IS_STRING(value)  (isPascalStringType(VALUE_TYPE(value)))
#define AS_STRING(value)  PSCAL_VALUE_FIELD(value, s_val)
#define IS_CHAR(value)    (isPascalCharType(VALUE_TYPE(value)))
#define AS_CHAR(value)    PSCAL_VALUE_FIELD(value, c_val)

static inline bool isPascalStringType(VarType t) {
    return t == TYPE_STRING || t == TYPE_UNICODE_STRING;
}

static inline bool isPascalCharType(VarType t) {
    return t == TYPE_CHAR || t == TYPE_WIDECHAR;
}

// Helper array to map Pscal color codes 0-7 to ANSI base numbers (30-37 or 40-47)
// Pscal: 0=Black, 1=Blue,  2=Green, 3=Cyan, 4=Red, 5=Magenta, 6=Brown(Yellow), 7=LightGray(White)
// ANSI:  x0=Black,x1=Red, x2=Green,x3=Yellow,x4=Blue,x5=Magenta,x6=Cyan,x7=White
static const int pscalToAnsiBase[8] = {
    0, // Pscal Black   -> ANSI Black (30/40 + 0)
    4, // Pscal Blue    -> ANSI Blue  (30/40 + 4)
    2, // Pscal Green   -> ANSI Green (30/40 + 2)
    6, // Pscal Cyan    -> ANSI Cyan  (30/40 + 6)
    1, // Pscal Red     -> ANSI Red   (30/40 + 1)
    5, // Pscal Magenta -> ANSI Magenta(30/40 + 5)
    3, // Pscal Brown   -> ANSI Yellow(30/40 + 3) (Brown is often dark yellow)
    7  // Pscal LtGray  -> ANSI White (30/40 + 7)
};

static inline bool isIntegerFamilyType(VarType t) {
    switch (t) {
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_BYTE:
        case TYPE_WORD:
            return true;
        default:
            return false;
    }
}

static inline bool isIntlikeType(VarType t) {
    if (isIntegerFamilyType(t)) {
        return true;
    }
    switch (t) {
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_WIDECHAR:
        case TYPE_THREAD:
            return true;
        default:
            return false;
    }
}

static inline bool isRealType(VarType t) {
    switch (t) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return true;
        default:
            return false;
    }
}

static inline bool isPackedByteElementType(VarType t) {
    return t == TYPE_BYTE;
}

static inline bool arrayUsesPackedBytes(const Value* v) {
    return v && VALUE_TYPE(*v) == TYPE_ARRAY && ARRAY_IS_PACKED(*v);
}

static inline bool isOrdinalType(VarType t) {
    // Pascal ordinals: integer subranges, enumerations, char, boolean.
    // Treat INTEGER/BYTE/WORD/BOOLEAN/CHAR/ENUM as ordinal.
    return isIntlikeType(t) || isPascalCharType(t) || t == TYPE_ENUM;
}

static inline bool tryValueToOrdinal(const Value* v, long long* out) {
    if (!v || !out) {
        return false;
    }

    switch (v->type) {
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_WORD:
        case TYPE_BYTE:
            *out = (long long)v->u_val;
            return true;
        case TYPE_INT64:
        case TYPE_INT32:
        case TYPE_INT16:
        case TYPE_INT8:
        case TYPE_BOOLEAN:
            *out = v->i_val;
            return true;
        case TYPE_CHAR:
            *out = (unsigned char)v->c_val;
            return true;
        case TYPE_WIDECHAR:
            *out = v->c_val;
            return true;
        case TYPE_ENUM:
            *out = v->enum_val.ordinal;
            return true;
        default:
            return false;
    }
}

static inline long long coerceToI64(const Value* v, VM* vm, const char* who) {
    long long ordinal = 0;
    if (tryValueToOrdinal(v, &ordinal)) {
        return ordinal;
    }
    runtimeError(vm, "Argument error: %s delta must be an ordinal, got %s.",
                 who, varTypeToString(v->type));
    return 0;
}

#define IS_INTLIKE(v) (isIntlikeType(VALUE_TYPE(v)))
#define IS_NUMERIC(v) (IS_INTLIKE(v) || isRealType((v).type))

VarType lookupBuiltinPascalTypeName(const char *name);
const char *builtinPascalTypeName(VarType type);

// Accessors (use your existing Value layout: i_val for INTEGER/BYTE/WORD/BOOLEAN)
static inline long long asI64(Value v) {
    switch (v.type) {
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_WORD:
        case TYPE_BYTE:
            return (long long)v.u_val;
        default:
            return v.i_val;
    }
}
static inline long double asLd(Value v) {
    switch (v.type) {
        case TYPE_FLOAT:
            /*
             * Floats are stored with multiple precision representations in
             * the Value union (long double, double and float).  Using the
             * raw 32-bit float for numeric comparisons causes literals such
             * as `3.14` assigned to a float and later compared to a `3.14`
             * constant to fail equality checks due to rounding differences.
             * Return the highest precision representation so that float
             * values participate in comparisons using their original
             * long‑double precision, matching existing regression
             * expectations.
             */
            return v.real.r_val;
        case TYPE_DOUBLE:
            return v.real.d_val;
        case TYPE_LONG_DOUBLE:
            return v.real.r_val;
        default:
            return (long double)asI64(v);
    }
}

const char *varTypeToString(VarType type);
const char *tokenTypeToString(TokenType type);
const char *astTypeToString(ASTNodeType type);

// Symbol table debugging/dumping functions.
void dumpSymbolTable(void);
void dumpSymbol(Symbol *sym);

MStream *createMStream(void);
void retainMStream(MStream* ms);
void releaseMStream(MStream* ms);
FieldValue *copyRecord(FieldValue *orig);
FieldValue *createEmptyRecord(AST *recordType);
void freeFieldValue(FieldValue *fv);

static inline Value *fieldValueStorage(FieldValue *field) {
    if (!field) {
        return NULL;
    }
    return field->storage ? field->storage : &field->value;
}

static inline const Value *fieldValueStorageConst(const FieldValue *field) {
    if (!field) {
        return NULL;
    }
    return field->storage ? field->storage : &field->value;
}

// Value constructors
Value makeInt(long long val);
Value makeReal(long double val);
Value makeFloat(float val);
Value makeDouble(double val);
Value makeLongDouble(long double val);
Value makeInt8(int8_t val);
Value makeUInt8(uint8_t val);
Value makeInt16(int16_t val);
Value makeUInt16(uint16_t val);
Value makeUInt32(uint32_t val);
Value makeInt64(long long val);
Value makeUInt64(unsigned long long val);
Value makeByte(unsigned char val);
Value makeWord(unsigned int val);
Value makeNil(void);
// Value constructor for creating a Value representing a general pointer.
// Used by the 'new' builtin.
Value makePointer(void* address, AST* base_type_node); // <<< ADD THIS PROTOTYPE >>>
Value makeString(const char *val);
Value makeStringLen(const char *val, size_t len);
Value makeChar(int c);
Value makeUnicodeString(const char *val);
Value makeUnicodeStringLen(const char *val, size_t len);
Value makeWideChar(int c);
Value makeBoolean(int b);
Value makeFile(FILE *f);
Value makeRecord(FieldValue *rec);
Value makeMStream(MStream *ms);
Value makeVoid(void);
Value makeValueForType(VarType type, AST *type_def, Symbol* context_symbol);
ClosureEnvPayload* createClosureEnv(uint16_t slot_count);
void retainClosureEnv(ClosureEnvPayload* env);
void releaseClosureEnv(ClosureEnvPayload* env);
Value makeClosure(uint32_t entry_offset, struct Symbol_s* symbol, ClosureEnvPayload* env);
Value makeInterface(AST* interfaceType, ClosureEnvPayload* payload);

// Token
Token *newToken(TokenType type, const char *value, int line, int column);
Token *copyToken(const Token *orig_token);
void freeToken(Token *token);

// Misc
void freeProcedureTable(void);
void freeTypeTable(void);
int getTerminalSize(int *rows, int *cols);
void toLowerString(char *str);
void debugASTFile(AST *node);
Value makeEnum(const char *enum_name, int ordinal);
void freeValue(Value *v);
void printValueToStream(Value v, FILE *stream);
size_t encodeUtf8Codepoint(uint32_t codepoint, char out[5]);
size_t encodePascalCharUtf8(int value, char out[5]);
bool isValidUtf8Bytes(const char *text, size_t len);
bool decodeUtf8Codepoint(const char *text, size_t len, uint32_t *out_codepoint, size_t *out_advance);
size_t utf8CodepointCount(const char *text, size_t len);
size_t utf8ByteOffsetForCodepointIndex(const char *text, size_t len, size_t index);
bool isShellIdentifierStartCodepoint(uint32_t codepoint);
bool isShellIdentifierContinueCodepoint(uint32_t codepoint, bool allow_hash);
bool consumeShellIdentifier(const char *text, size_t len, size_t *out_advance, bool allow_hash);
void writePascalText(FILE *stream, const char *text, size_t len);
int calculateArrayTotalSize(const Value* array_val);

// Unit Stuff
char *findUnitFile(const char *unit_name);
void linkUnit(AST *unit_ast, int recursion_depth);
Symbol *buildUnitSymbolTable(AST *interface_ast);
void freeUnitSymbolTable(Symbol *symbol_table);
bool isUnitDocumented(const char *unit_name);

// General helpers
// Helper function to map 0-15 to ANSI FG codes
int map16FgColorToAnsi(int pscalColorCode, bool isBold);
// Helper function to map 0-7 to ANSI BG codes
int map16BgColorToAnsi(int pscalColorCode);
bool applyCurrentTextAttributes(FILE* stream);
void resetTextAttributes(FILE* stream);
uint8_t computeCurrentTextAttr(void);
void syncTextAttrSymbol(void);

void markTextAttrDirty(void);

void setCurrentTextAttrFromByte(uint8_t attr);

// Arrays
Value makeArrayND(int dimensions, int *lower_bounds, int *upper_bounds, VarType element_type, AST *type_def);
Value makeEmptyArray(VarType element_type, AST *type_def);
int computeFlatOffset(Value *array, int *indices);
Value makeCopyOfValue(const Value *src);
Value copyDynamicArraySnapshotValue(const Value *src);
bool makeDynamicArraySliceValue(const Value *src, int consumed_dims, const int *indices,
                                 Value *out, bool *out_of_bounds);
/* Shared with vm.c's replaceValueCell(): both sides of a dynamic array's
 * fresh-publish/read race must serialize on this same mutex, or a reader's
 * whole-struct snapshot copy can interleave with a writer's whole-struct
 * publish and observe a torn header. See utils.c for the definition. */
extern pthread_mutex_t dynamic_array_refcount_mutex;

// Set operations
Value setUnion(Value setA, Value setB);
Value setDifference(Value setA, Value setB);
Value setIntersection(Value setA, Value setB);

/*
 * Optional lint (VM 2.0 Phase 0 item 3): forbid new direct Value payload
 * accesses outside the representation layer, so the accessor sweep cannot
 * silently regress before the Phase 4 representation swap.  Enable with
 * the PSCAL_VALUE_ACCESS_LINT CMake option; the representation layer
 * (core/utils.c, core/cache.c) opts out by defining
 * PSCAL_VALUE_REPRESENTATION_LAYER before its includes.
 *
 * i_val is deliberately not poisoned (the AST node type shares that member
 * name for enum ordinals); `type`/`real`/`closure`/`interface` are too
 * generic to poison; `mstream` is skipped because it is a common local
 * variable name for MStream*.  Everything else payload-shaped is
 * Value-only.
 *
 * Metadata fields (follow-up sweep): only the Value-unique names are
 * poisoned.  `lower_bound(s)`/`upper_bound(s)`, `dimensions`,
 * `element_type(_def)`, `max_length`, `filename` and `record_size` are
 * common local/parameter/other-struct names (clike's AST has
 * element_type; vm.c/builtin.c use bounds-array locals), so they rely on
 * the _Generic guard alone.
 */
#if defined(PSCAL_VALUE_ACCESS_LINT) && !defined(PSCAL_VALUE_REPRESENTATION_LAYER)
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC poison s_val u_val c_val record_val f_val array_val enum_val ptr_val set_val array_raw
#pragma GCC poison array_is_packed array_is_dynamic array_refcount base_type_node record_size_explicit enum_meta
#endif
#endif

#endif // UTILS_H
