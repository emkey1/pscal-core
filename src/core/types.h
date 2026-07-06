// src/core/types.h
#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include "list.h"
#include <stdbool.h>
#include "common/frontend_symbol_aliases.h"
#include "core/var_type.h"
#include "core/obj_header.h" // ObjHeader is embedded in ClosureEnvPayload/MStream below (VM 2.0 Phase 4b)
#if defined(PSCAL_TARGET_IOS)
#include "runtime/vproc/vproc.h"
#include "runtime/vproc/vproc_stdio_shim.h"
#endif

// Default record size used for untyped files when RESET/REWRITE omit an
// explicit size. Turbo Pascal historically defaults to 128 bytes; mirror that
// so existing code that relies on the legacy behaviour keeps working.
#define PSCAL_DEFAULT_FILE_RECORD_SIZE 128

// Forward declaration of AST struct, as TypeEntry will use AST*
struct AST;

// Define TypeEntry here
typedef struct TypeEntry_s { // Use a named tag for robustness
    char *name;
    struct AST *typeAST; // Uses forward-declared struct AST
    struct TypeEntry_s *next;
} TypeEntry;

struct ValueStruct;
typedef struct FieldValue FieldValue;
struct Symbol_s;

// ObjHeader.type distinguishes TYPE_CLOSURE (a closure's captured-variable
// environment) from TYPE_INTERFACE (the same shape reused for an
// interface's receiver/table/class payload) -- both are set correctly at
// construction time by createClosureEnv's caller (VM 2.0 Phase 4b,
// Docs/pscal_vm2_plan.md §5.10.4/§5.10.3).
typedef struct ClosureEnvPayload {
    ObjHeader header;
    uint16_t slot_count;
    struct Symbol_s *symbol;
    struct ValueStruct **slots;
} ClosureEnvPayload;

// VarType and the TYPE_INTEGER/TYPE_REAL backwards-compatibility aliases
// live in core/var_type.h (included above), not here -- see that file's
// header comment for why.

typedef struct MStream {
    ObjHeader header; // VM 2.0 Phase 4b: header.type is always TYPE_MEMORYSTREAM
    unsigned char *buffer;
    int size;
    int capacity;
} MStream;

// VM 2.0 Phase 4c (Docs/pscal_vm2_plan.md §5.10.4/§5.10.3): field names
// (set_size, set_values) intentionally match the pre-4c embedded
// `Value.set_val` struct's field names, so `AS_SET(v).set_size`/
// `.set_values` call sites keep working unchanged once AS_SET dereferences
// a SetObj* instead of returning the embedded struct by value. `capacity`
// is new -- previously this codebase borrowed the unrelated
// `Value.max_length` field (via STRING_MAX_LENGTH) for set growth capacity
// (see vmAddOrdinalToSet in vm.c), which stops working once
// STRING_MAX_LENGTH is redefined to reach into StringObj instead; sets now
// get a proper field of their own via SET_CAPACITY.
typedef struct SetObj {
    ObjHeader header; // header.type is always TYPE_SET
    int set_size;
    int capacity;
    long long *set_values;
} SetObj;

// VM 2.0 Phase 4c: StringObj replaces the pre-4c pair of independent
// Value-level fields (`s_val` char*, `max_length` int). `buffer` is a
// PLAIN OWNED POINTER, not a flexible array member fused into the same
// allocation as the header -- deliberately, because the existing codebase
// has a widespread idiom of reassigning the whole buffer in place
// (`AS_STRING(v) = new_buf;`, used ~15 times across vm.c/builtin.c/
// symbol.c/exsh's shell_builtins.inc for string growth/concatenation/
// mutation), which a flexible array member cannot support (you cannot
// reassign a flexible array member as a whole, only index into it). This
// costs one extra allocation+indirection versus the single-allocation
// design originally sketched in the plan, but preserves every one of
// those call sites without rewriting the growth/mutation logic itself --
// only construction-order matters now (see pscalStringEnsureObj).
typedef struct StringObj {
    ObjHeader header; // header.type is TYPE_STRING or TYPE_UNICODE_STRING
    int max_length;   // -1 = unbounded (dynamic string); >0 = Pascal string[N]
    char *buffer;     // owned, NUL-terminated, or NULL; reassignable in place
} StringObj;

// VM 2.0 Phase 4d (Docs/pscal_vm2_plan.md §5.10.4/§5.10.3): thin wrapper
// only -- FieldValue itself is unchanged, still a linked list, still
// supports the owns_storage/aliased-storage trick OOP field-address-taking
// depends on (copyRecord/freeFieldValue operate entirely on FieldValue*,
// never touching this wrapper). Verified before assuming: zero external
// `AS_RECORD(v) = X` whole-pointer-reassignment sites and zero external
// `SET_VALUE_TYPE(v, TYPE_RECORD)` field-by-field construction sites exist
// anywhere in the tree -- record construction always goes through
// makeRecord/makeValueForType/makeCopyOfValue, unlike strings (4c), so
// this sub-phase needed no pscalStringEnsureObj-style lazy-init helper.
typedef struct RecordObj {
    ObjHeader header; // header.type is always TYPE_RECORD
    FieldValue *fields;
} RecordObj;

// Definition of Type struct for enum metadata
typedef struct EnumType {
    char *name;         // Name of the enum type
    char **members;     // Array of member names
    int member_count;   // Number of members
} Type;

// Forward declaration of AST
typedef struct AST AST;

typedef struct RealValue { float f32_val; double d_val; long double r_val; } RealValue;

typedef struct ValueStruct {
    VarType type;
    Type *enum_meta;
    long long i_val;
    unsigned long long u_val;
    RealValue real;
    union {
        StringObj *s_val; // VM 2.0 Phase 4c: was a plain owned char*
        int c_val;
        RecordObj *record_val; // VM 2.0 Phase 4d: was a plain FieldValue*
        FILE *f_val;
        struct ValueStruct *array_val;
        MStream *mstream;
        struct {
            char *enum_name; // Name of the enumerated type
            int ordinal;     // Ordinal value
        } enum_val;
        struct ValueStruct *ptr_val; // Pointer to another Value (for heap data)
        struct {
            uint32_t entry_offset;
            struct Symbol_s *symbol;
            ClosureEnvPayload *env;
        } closure;
        struct {
            struct AST *type_def;
            ClosureEnvPayload *payload;
        } interface;
    };
    uint8_t *array_raw;
    bool array_is_packed;
    bool array_is_dynamic;
    uint32_t *array_refcount;
    AST *base_type_node; // AST node defining the type this pointer points to
                         // Needed for new(), dispose(), dereferencing type checks.

    char *filename;
    int record_size;      // Active record size for untyped file operations
    bool record_size_explicit; // Whether the record size was explicitly requested
    int lower_bound;    // For single-dimensional arrays
    int upper_bound;    // For single-dimensional arrays
    int max_length;     // For fixed length strings (text: string[100];)
    VarType element_type;
    int dimensions;       // number of dimensions (e.g. 2 for [1..2, 1..3])
    int *lower_bounds;    // array of lower bounds for each dimension
    int *upper_bounds;    // array of upper bounds for each dimension
    AST *element_type_def; // AST node defining the element type
    SetObj *set_val; // VM 2.0 Phase 4c: was an embedded struct, now a heap pointer
} Value;

/* Helpers to initialise numeric fields consistently. */
#define SET_INT_VALUE(dest, val) \
    do { (dest)->i_val = (long long)(val); (dest)->u_val = (unsigned long long)(val); } while(0)
#define SET_REAL_VALUE(dest, val) \
    do { (dest)->real.r_val = (long double)(val); (dest)->real.d_val = (double)(dest)->real.r_val; \
         (dest)->real.f32_val = (float)(dest)->real.r_val; } while(0)
#define SET_CHAR_VALUE(dest, val) \
    do { PSCAL_VALUE_FIELD(*(dest), c_val) = (val); } while(0)
#define SET_VALUE_TYPE(dest, t) \
    do { PSCAL_VALUE_FIELD(*(dest), type) = (t); } while(0)

/*
 * Value tag and payload accessors (VM 2.0 Phase 0 accessor sweep; see
 * Docs/pscal_vm2_plan.md section 4).  Each macro is an exact alias for the
 * underlying field today; Phase 4 re-representation redefines these instead
 * of rewriting call sites.  Where available, C11 _Generic pins the receiver
 * to Value so that accidentally applying a macro to a Symbol/AST/Token
 * (which also carry a `type` member) is a compile error, not a silent
 * semantic change.
 *
 * Reads and pointer-payload writes go through these accessors; writes to
 * immediate payloads (integers, reals, chars, the type tag) go through the
 * SET_*_VALUE helpers above so that Phase 4 can re-encode on store.
 * The representation layer itself (Value constructors, freeValue,
 * makeCopyOfValue, the bytecode-cache serializer) intentionally keeps raw
 * field access.
 */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__cplusplus)
#define PSCAL_VALUE_FIELD(v, f) (_Generic((v), Value: (v), const Value: (v)).f)
#else
#define PSCAL_VALUE_FIELD(v, f) ((v).f)
#endif

#define VALUE_TYPE(v)    PSCAL_VALUE_FIELD(v, type)

/* Exact immediate-payload accessors (no coercion; contrast AS_INTEGER /
 * AS_REAL in core/utils.h which coerce across the numeric families). */
#define VAL_INT(v)       PSCAL_VALUE_FIELD(v, i_val)
#define VAL_UINT(v)      PSCAL_VALUE_FIELD(v, u_val)
#define VAL_REAL32(v)    PSCAL_VALUE_FIELD(v, real.f32_val)
#define VAL_REAL64(v)    PSCAL_VALUE_FIELD(v, real.d_val)
#define VAL_REAL_LD(v)   PSCAL_VALUE_FIELD(v, real.r_val)

/* Heap/pointer payload accessors (lvalue-capable). */
// VM 2.0 Phase 4d: record_val is now a RecordObj*; AS_RECORD dereferences
// to its fields so existing `AS_RECORD(v)` reads keep returning a
// FieldValue* unchanged. No known assignment-through-AS_RECORD call sites
// exist (verified), but the expansion is still a valid lvalue if one ever
// shows up, since `.fields` is a plain reassignable pointer field.
#define AS_RECORD(v)     (PSCAL_VALUE_FIELD(v, record_val)->fields)
#define AS_ARRAY(v)      PSCAL_VALUE_FIELD(v, array_val)
#define AS_FILE(v)       PSCAL_VALUE_FIELD(v, f_val)
#define AS_MSTREAM(v)    PSCAL_VALUE_FIELD(v, mstream)
#define AS_POINTER(v)    PSCAL_VALUE_FIELD(v, ptr_val)
#define AS_ENUM(v)       PSCAL_VALUE_FIELD(v, enum_val)
#define AS_CLOSURE(v)    PSCAL_VALUE_FIELD(v, closure)
#define AS_INTERFACE(v)  PSCAL_VALUE_FIELD(v, interface)
// VM 2.0 Phase 4c: set_val is now a SetObj* (was an embedded struct); AS_SET
// dereferences it so existing `AS_SET(v).set_size`/`.set_values` call sites
// keep working with `.` unchanged (SetObj's field names match the old
// embedded struct's on purpose -- see core/types.h's SetObj comment).
#define AS_SET(v)        (*PSCAL_VALUE_FIELD(v, set_val))
#define SET_CAPACITY(v)  (PSCAL_VALUE_FIELD(v, set_val)->capacity)
#define AS_ARRAY_RAW(v)  PSCAL_VALUE_FIELD(v, array_raw)

/* Array/string/file/pointer metadata accessors.  Phase 4 Stage A moves
 * these fields into the heap array/string objects; until then they are
 * exact aliases like the payload accessors above. */
#define ARRAY_LOWER_BOUND(v)       PSCAL_VALUE_FIELD(v, lower_bound)
#define ARRAY_UPPER_BOUND(v)       PSCAL_VALUE_FIELD(v, upper_bound)
#define ARRAY_LOWER_BOUNDS(v)      PSCAL_VALUE_FIELD(v, lower_bounds)
#define ARRAY_UPPER_BOUNDS(v)      PSCAL_VALUE_FIELD(v, upper_bounds)
#define ARRAY_DIMENSIONS(v)        PSCAL_VALUE_FIELD(v, dimensions)
#define ARRAY_ELEMENT_TYPE(v)      PSCAL_VALUE_FIELD(v, element_type)
#define ARRAY_ELEMENT_TYPE_DEF(v)  PSCAL_VALUE_FIELD(v, element_type_def)
#define ARRAY_IS_PACKED(v)         PSCAL_VALUE_FIELD(v, array_is_packed)
#define ARRAY_IS_DYNAMIC(v)        PSCAL_VALUE_FIELD(v, array_is_dynamic)
#define ARRAY_REFCOUNT(v)          PSCAL_VALUE_FIELD(v, array_refcount)
// VM 2.0 Phase 4c: max_length now lives inside StringObj, not directly on
// Value (the old Value.max_length field is dead weight until Phase 4i's
// struct shrink deletes it -- nothing should read it directly anymore).
// Requires v.s_val to already be a valid StringObj*; see AS_STRING's
// comment (core/utils.h) and pscalStringEnsureObj for why/when that holds.
#define STRING_MAX_LENGTH(v)       (PSCAL_VALUE_FIELD(v, s_val)->max_length)
#define PTR_BASE_TYPE_NODE(v)      PSCAL_VALUE_FIELD(v, base_type_node)
#define FILE_FILENAME(v)           PSCAL_VALUE_FIELD(v, filename)
#define FILE_RECORD_SIZE(v)        PSCAL_VALUE_FIELD(v, record_size)
#define FILE_RECORD_SIZE_EXPLICIT(v) PSCAL_VALUE_FIELD(v, record_size_explicit)
#define ENUM_META(v)               PSCAL_VALUE_FIELD(v, enum_meta)

typedef struct FieldValue {
    char *name;
    struct ValueStruct value;
    struct ValueStruct *storage;
    struct AST *type_def;
    VarType declared_type;
    int slot_index;
    bool owns_storage;
    struct FieldValue *next;
} FieldValue;

typedef enum {
    TOKEN_PROGRAM, TOKEN_VAR, TOKEN_BEGIN, TOKEN_END, TOKEN_IF, TOKEN_THEN,
    TOKEN_ELSE, TOKEN_WHILE, TOKEN_DO, TOKEN_FOR, TOKEN_TO, TOKEN_DOWNTO,
    TOKEN_REPEAT, TOKEN_UNTIL, TOKEN_PROCEDURE, TOKEN_FUNCTION, TOKEN_CONST,
    TOKEN_TYPE, TOKEN_WRITE, TOKEN_WRITELN, TOKEN_READ, TOKEN_READLN,
    TOKEN_INT_DIV, TOKEN_MOD, TOKEN_RECORD, TOKEN_IDENTIFIER, TOKEN_INTEGER_CONST,
    TOKEN_REAL_CONST, TOKEN_STRING_CONST, TOKEN_SEMICOLON, TOKEN_GREATER,
    TOKEN_GREATER_EQUAL, TOKEN_EQUAL, TOKEN_NOT_EQUAL, TOKEN_LESS_EQUAL,
    TOKEN_LESS, TOKEN_COLON, TOKEN_QUESTION, TOKEN_COMMA, TOKEN_PERIOD, TOKEN_ASSIGN,
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL, TOKEN_MUL_EQUAL, TOKEN_SLASH_EQUAL, TOKEN_PERCENT_EQUAL,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_MUL, TOKEN_SLASH, TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACKET, TOKEN_RBRACKET, TOKEN_DOTDOT, TOKEN_ARRAY, TOKEN_AS, TOKEN_OF,
    TOKEN_AND, TOKEN_OR, TOKEN_TRUE, TOKEN_FALSE, TOKEN_NOT, TOKEN_CASE,
    TOKEN_USES, TOKEN_EOF, TOKEN_HEX_CONST, TOKEN_UNKNOWN, TOKEN_UNIT,
    TOKEN_INTERFACE, TOKEN_IMPLEMENTATION, TOKEN_INITIALIZATION, TOKEN_ENUM,
    TOKEN_IN, TOKEN_IS, TOKEN_XOR, TOKEN_BREAK, TOKEN_CONTINUE, TOKEN_RETURN, TOKEN_OUT, TOKEN_SHL, TOKEN_SHR,
    TOKEN_SET, TOKEN_POINTER, TOKEN_CARET, TOKEN_NIL, TOKEN_INLINE, TOKEN_FORWARD, TOKEN_SPAWN, TOKEN_JOIN,
    TOKEN_TRY, TOKEN_EXCEPT, TOKEN_FINALLY, TOKEN_ON, TOKEN_RAISE, TOKEN_WITH, TOKEN_AT, TOKEN_LABEL, TOKEN_GOTO
} TokenType;

typedef struct {
    TokenType type;
    char *value;
    size_t length;
    int line;
    int column;
    bool is_char_code;
    uint32_t char_code_value;
} Token;

/* =======================
   AST DEFINITIONS & HELPERS
   ======================= */
typedef enum {
    AST_NOOP,
    AST_PROGRAM,
    AST_BLOCK,
    AST_CONST_DECL,
    AST_TYPE_DECL,
    AST_VAR_DECL,
    AST_ASSIGN,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_TERNARY,
    AST_NUMBER,
    AST_STRING,
    AST_VARIABLE,
    AST_COMPOUND,
    AST_IF,
    AST_WHILE,
    AST_REPEAT,
    AST_FOR_TO,
    AST_FOR_DOWNTO,
    AST_WRITELN,
    AST_WRITE,
    AST_READLN,
    AST_READ,
    AST_RETURN,
    AST_EXPR_STMT,
    AST_PROCEDURE_DECL,
    AST_PROCEDURE_CALL,
    AST_FUNCTION_DECL,
    AST_CASE,
    AST_CASE_BRANCH,
    AST_RECORD_TYPE,
    AST_RECORD_LITERAL,
    AST_FIELD_ACCESS,
    AST_ARRAY_TYPE,
    AST_ARRAY_ACCESS,
    AST_BOOLEAN,
    AST_FORMATTED_EXPR,
    AST_TYPE_REFERENCE,
    AST_TYPE_IDENTIFIER, // Added: Represents a simple type identifier like "integer" or "MyCustomType"
    AST_TYPE_ASSERT,
    AST_SUBRANGE,
    AST_USES_CLAUSE,
    AST_IMPORT,
    AST_UNIT,
    AST_MODULE,
    AST_INTERFACE,
    AST_IMPLEMENTATION,
    AST_INITIALIZATION,
    AST_LIST,
    AST_ENUM_TYPE,
    AST_ENUM_VALUE,
    AST_SET,
    AST_ARRAY_LITERAL,
    AST_BREAK,
    AST_CONTINUE,
    AST_THREAD_SPAWN,
    AST_THREAD_JOIN,
    AST_POINTER_TYPE,
    AST_PROC_PTR_TYPE,
    AST_DEREFERENCE,
    AST_ADDR_OF,
    AST_NIL,
    AST_NEW,
    AST_MATCH,
    AST_MATCH_BRANCH,
    AST_PATTERN_BINDING,
    AST_TRY,
    AST_CATCH,
    AST_FINALLY,
    AST_THROW,
    AST_WITH,
    AST_LABEL_DECL,
    AST_LABEL,
    AST_GOTO
} ASTNodeType;

// Define the function pointer type for built-in handlers
typedef Value (*BuiltinHandler)(AST *node);

// Structure to map built-in names to handlers
typedef struct {
    const char *name;       // Lowercase name of the built-in
    BuiltinHandler handler; // Pointer to the C function implementation
} BuiltinMapping;

const char *varTypeToString(VarType type);
const char *tokenTypeToString(TokenType type);
const char *astTypeToString(ASTNodeType type);

// Function prototypes
void setTypeValue(Value *val, VarType type);
VarType inferBinaryOpType(VarType left, VarType right);

#endif // TYPES_H
