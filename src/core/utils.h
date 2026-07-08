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
// VM 2.0 Phase 4c: s_val is now a StringObj*; AS_STRING dereferences to its
// buffer so existing `AS_STRING(v)`/`AS_STRING(v) = X` call sites keep
// working -- but callers doing the latter must ensure v->s_val is already
// non-NULL first (see pscalStringEnsureObj) since dereferencing a NULL
// StringObj* to assign its buffer crashes exactly like any other NULL
// deref would.
#define AS_STRING(value)  (PSCAL_VALUE_PTR(value, StringObj)->buffer)
#define IS_CHAR(value)    (isPascalCharType(VALUE_TYPE(value)))
// VM 2.0 Phase 4i checkpoint 3d: c_val is gone; VAL_INT's TYPE_CHAR/
// TYPE_WIDECHAR cases already decode the exact codepoint from `bits`.
#define AS_CHAR(value)    ((int)VAL_INT(value))

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
            *out = (long long)VAL_UINT(*v);
            return true;
        case TYPE_INT64:
        case TYPE_INT32:
        case TYPE_INT16:
        case TYPE_INT8:
        case TYPE_BOOLEAN:
            *out = VAL_INT(*v);
            return true;
        case TYPE_CHAR:
            *out = (unsigned char)AS_CHAR(*v);
            return true;
        case TYPE_WIDECHAR:
            *out = AS_CHAR(*v);
            return true;
        case TYPE_ENUM: {
            EnumObj *e = PSCAL_VALUE_PTR(*v, EnumObj);
            *out = e ? e->ordinal : 0;
            return true;
        }
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
            return (long long)VAL_UINT(v);
        default:
            return VAL_INT(v);
    }
}
static inline long double asLd(Value v) {
    switch (v.type) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            /*
             * VM 2.0 Phase 4i checkpoint 3d: VAL_REAL_LD already decodes
             * each of these three at its own natural precision (float's
             * bit pattern widened, double's bit pattern widened, or the
             * LongDoubleBox's own long double value) -- no separate
             * "widen the float specially" step needed anymore, since
             * there's only one stored representation per Value now, not
             * three kept in sync simultaneously.
             */
            return VAL_REAL_LD(v);
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

// VM 2.0 Phase 4c. pscalStringObjCreate: refcount=1, buffer=NULL.
// owner_type must be TYPE_STRING or TYPE_UNICODE_STRING -- both share this
// payload shape; owner_type becomes the ObjHeader's type tag, matching the
// same discipline createClosureEnv established in Phase 4b (load-bearing
// once ObjHeader.type becomes the sole record of a boxed Value's type in
// Phase 4i, not just a destructor-dispatch key today).
// pscalStringEnsureObj: if v->s_val is already non-NULL, a no-op; if NULL
// (a Value that's been memset+type-tagged but never had a string
// constructor run for it -- a common idiom in this codebase, e.g.
// building a temporary string Value by hand rather than calling
// makeString), allocates a fresh wrapper via
// pscalStringObjCreate(-1, VALUE_TYPE(*v)) (falling back to TYPE_STRING if
// v isn't yet tagged as either string type). Call this before any
// `AS_STRING(v) = X` / `STRING_MAX_LENGTH(v) = X` write where v->s_val
// might not exist yet.
StringObj *pscalStringObjCreate(int max_length, VarType owner_type);
void pscalStringEnsureObj(Value *v);
// VM 2.0 Phase 4d. `fields` may be NULL (no known content yet); the
// wrapper itself is never NULL once created, matching StringObj/SetObj.
RecordObj *pscalRecordObjCreate(FieldValue *fields);

// VM 2.0 Phase 4e/4f. Returns a fresh ArrayObj with refcount=1 and every
// field zeroed/NULL; callers set element_type/dimensions/bounds/etc.
// themselves afterward (mirroring how makeArrayND/makeEmptyArray/the
// pointer-to-array-type opcode handlers already build array metadata up
// field-by-field). pscalArrayEnsureObj lazily allocates one via this if
// v->array_val is NULL -- needed at the handful of sites (the
// pointer-to-array-type "temp_wrapper" pattern in vm.c) that build a
// TYPE_ARRAY Value's payload from scratch rather than through
// makeArrayND/makeEmptyArray.
ArrayObj *pscalArrayObjCreate(void);
void pscalArrayEnsureObj(Value *v);

// VM 2.0 Phase 4g. Returns a fresh EnumObj (refcount=1, enum_meta/
// enum_name NULL, ordinal 0); callers set fields afterward, same pattern
// as pscalArrayObjCreate. pscalEnumEnsureObj lazily allocates one via this
// if v->enum_val is NULL.
EnumObj *pscalEnumObjCreate(void);
void pscalEnumEnsureObj(Value *v);

// VM 2.0 Phase 4g. Returns a fresh FileObj (refcount=1, every field
// zeroed/NULL); callers set f/filename/record_size/etc afterward, same
// pattern as pscalArrayObjCreate. pscalFileEnsureObj lazily allocates one
// via this if v->f_val is NULL -- needed at vmBuiltinFopen (builtin.c),
// which flips a TYPE_VOID Value to TYPE_FILE and writes through
// AS_FILE/FILE_FILENAME without going through makeFile()/
// makeValueForType() first.
FileObj *pscalFileObjCreate(void);
void pscalFileEnsureObj(Value *v);

// VM 2.0 Phase 4g. Returns a fresh PointerObj (refcount=1, address/
// base_type_node NULL); callers set fields afterward, same pattern as
// pscalArrayObjCreate. Unlike FileObj's ensure-helper, copies of a
// TYPE_POINTER Value must always call this to allocate an INDEPENDENT
// PointerObj (never pscalObjRetain an existing one) -- see PointerObj's
// comment in core/types.h for why.
PointerObj *pscalPointerObjCreate(void);
void pscalPointerEnsureObj(Value *v);

// VM 2.0 Phase 4i checkpoint 3a. Returns a fresh, refcount=1 ClosureObj/
// InterfaceObj with every field zeroed/NULL; callers set fields
// afterward, same pattern as pscalArrayObjCreate. Copies retain the
// SAME wrapper (see ClosureObj's comment in core/types.h for why this
// is the file-like sharing precedent, not the pointer-like
// copy-on-construct one).
ClosureObj *pscalClosureObjCreate(void);
InterfaceObj *pscalInterfaceObjCreate(void);

MStream *createMStream(void);
void retainMStream(MStream* ms);
void releaseMStream(MStream* ms);
SetObj *pscalSetObjCreate(void); // VM 2.0 Phase 4c: refcount=1, empty
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

// Generic integer opcodes/constant-folding compute a `long long` result and
// need a Value as wide as their widest operand -- makeInt() alone always
// tags TYPE_INT32, whose tagged immediate is a real 32-bit budget (VM 2.0
// Phase 4i checkpoint 3d made this authoritative), so blindly using it
// would silently truncate int64/uint64 arithmetic.
static inline Value pscalIntResultLike1(Value a, long long result) {
    if (VALUE_TYPE(a) == TYPE_UINT64) return makeUInt64((unsigned long long)result);
    if (VALUE_TYPE(a) == TYPE_INT64) return makeInt64(result);
    return makeInt(result);
}
static inline Value pscalIntResultLike2(Value a, Value b, long long result) {
    if (VALUE_TYPE(a) == TYPE_UINT64 || VALUE_TYPE(b) == TYPE_UINT64) return makeUInt64((unsigned long long)result);
    if (VALUE_TYPE(a) == TYPE_INT64 || VALUE_TYPE(b) == TYPE_INT64) return makeInt64(result);
    return makeInt(result);
}

Value makeByte(unsigned char val);
Value makeWord(unsigned int val);
Value makeNil(void);
// VM 2.0 Phase 4j (Stage B): call before taking any address INTO a
// CoW-eligible boxed Value's payload that could be used to mutate it in
// place -- GET_ELEMENT_ADDRESS/GET_ELEMENT_ADDRESS_CONST,
// GET_FIELD_ADDRESS/16/_KEEP/16, and the string-char-address sites.
// Read-only value loads (LOAD_ELEMENT_VALUE, LOAD_FIELD_VALUE*) never take
// such an address to begin with, so they never need to call this.
// If `target`'s underlying object is shared (ObjHeader.refcount > 1),
// clones it via makeCopyOfValue so the caller-about-to-mutate has a
// private copy; a no-op otherwise (unique already, or not a CoW-eligible
// type). TYPE_ARRAY with is_dynamic==true and TYPE_FILE are permanent
// exemptions -- see Docs/pscal_vm2_plan.md sec 5.10.5.
void valueEnsureUnique(Value *target);
// Exchanges the contents of two Value cells (the `Swap` builtin). A raw
// three-way struct swap conserves whatever references each cell already
// held (no clone, no retain/release needed either side), so this is just a
// named home for the pattern rather than a leftover raw Value copy outside
// the representation layer.
void pscalValueSwap(Value *a, Value *b);
// Value constructor for creating a Value representing a general pointer.
// Used by the 'new' builtin.
Value makePointer(void* address, AST* base_type_node); // <<< ADD THIS PROTOTYPE >>>
// Like makePointer(), but for an address that points directly INTO a live
// dynamic array's own storage (a GET_ELEMENT_ADDRESS/GET_ELEMENT_ADDRESS_CONST
// result) rather than a freestanding allocation. `owner`'s ObjHeader
// reference is TRANSFERRED into the resulting PointerObj (caller must
// already hold a retained reference to hand off, e.g. from
// copyDynamicArraySnapshotValue() -- this function does not retain it
// itself); freeValue() releases it (dynamic_array_refcount_mutex-protected)
// when the pointer Value is torn down, keeping `owner` alive for exactly as
// long as this pointer is, even across a concurrent SetLength() on the same
// array. See PointerObj.retained_array's comment (core/types.h).
Value makeRetainedArrayElementPointer(void* address, AST* base_type_node, struct ArrayObj* owner);
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
// VM 2.0 Phase 5a checkpoint 5a-i: `owner` is the VM whose threads[] pool
// slot `threadId` names -- see TaskObj's comment in core/types.h.
struct VM_s;
TaskObj *createTaskObj(int threadId, struct VM_s *owner);
Value makeTask(int threadId, struct VM_s *owner);
Value makeVoid(void);
Value makeValueForType(VarType type, AST *type_def, Symbol* context_symbol);
// owner_type must be TYPE_CLOSURE or TYPE_INTERFACE -- both share this
// same payload shape; owner_type becomes the ObjHeader's type tag so it's
// recoverable later (VM 2.0 Phase 4b, Docs/pscal_vm2_plan.md §5.10.3).
ClosureEnvPayload* createClosureEnv(uint16_t slot_count, VarType owner_type);
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

// VM 2.0 Phase 4i checkpoint 2. Returns true if `v->bits` (the tagged-
// word mirror) decodes to the same value the discrete legacy fields
// hold, for every scalar kind checkpoint 2 covers (VOID/NIL/BOOLEAN/
// CHAR/WIDECHAR/BYTE/WORD/INT8-32/UINT8-32/FLOAT/DOUBLE). Returns true
// (vacuously) for every type deferred to checkpoint 3 (heap-pointer
// types, TYPE_INT64/TYPE_UINT64/TYPE_LONG_DOUBLE) -- there is nothing to
// cross-check yet for those. Called unconditionally from freeValue
// (not gated behind DEBUG) so the full test suite actually exercises
// it; remove once checkpoint 3 makes bits the sole representation and
// there is nothing left to compare it against.
bool pscalValueBitsConsistent(const Value *v);
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
/* Shared with vm.c's copyValueForStack()/replaceValueCell(): VM 2.0 Phase
 * 4j's valueEnsureUnique() (utils.c) must serialize its check-then-clone
 * sequence on this same mutex, or two threads racing a write-time clone
 * (or a clone racing a cheap retain-share copy) against the SAME shared
 * object can each decide independently to clone, then stomp on each
 * other's install/release -- confirmed as a real heap-corruption crash
 * under a multithreaded stress test (two threads concurrently cloning the
 * same shared TYPE_RECORD). See vm.c for the definition. */
extern pthread_mutex_t value_cell_mutex;

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
