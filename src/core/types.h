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

// VM 2.0 Phase 4e/4f (Docs/pscal_vm2_plan.md §5.10.4/§5.10.3): the
// highest-risk type family in Stage A, per the plan's own risk register.
// `raw`/`elements` are plain owned pointers (matching StringObj/SetObj's
// precedent, not the flexible-array-member sketch) -- a real audit found
// the same whole-buffer-reassignment idiom here too (AS_ARRAY(v)=X /
// AS_ARRAY_RAW(v)=X, ~20 sites across vm.c/builtin.c).
//
// `is_dynamic` keeps its full pre-4e/4f semantic weight, not just a
// storage-strategy flag: dynamic (SetLength-managed) arrays have
// reference semantics today (multiple Values legitimately alias one
// ArrayObj, refcounted) while static arrays have Pascal value semantics
// (always deep-copied). This distinction lives in the copy logic
// (makeCopyOfValue), not in this struct's shape, and must survive
// unchanged -- Stage A only changes storage representation, not
// observable behavior (see plan §5.10.6).
//
// `lower_bound`/`upper_bound` (singular) are kept as their own fields,
// deliberately NOT folded into `lower_bounds[0]`/`upper_bounds[0]` even
// though the plan's §5.10.3 flags that fold as a decided-but-gated
// simplification -- bundling it into this already-highest-risk sub-phase
// was judged not worth the extra surface area; revisit as a separate,
// low-risk cleanup once arrays are otherwise stable.
//
// The refcount lives in the embedded ObjHeader (atomic, from Phase 4a),
// replacing the old separately-malloc'd `uint32_t *array_refcount` +
// `dynamic_array_refcount_mutex`-protected manual increment/decrement.
// IMPORTANT: during Stage A (this sub-phase), the mutex itself is NOT
// retired -- `value_cell_mutex`/`dynamic_array_refcount_mutex`'s pairing
// in `replaceValueCell`/`copyDynamicArraySnapshotValue` (vm.c/utils.c)
// still protects against a concurrent reader observing a torn multi-field
// `Value` struct, which remains a real hazard until Value itself shrinks
// to one atomically-storable word in Stage B (4i/4j) -- see the plan's
// §5.10.5 reasoning, which only applies once that precondition holds.
// Only the *counter mechanism* moves to the atomic ObjHeader refcount
// here; the mutex-protected critical sections around it are unchanged.
typedef struct ArrayObj {
    ObjHeader header; // header.type is always TYPE_ARRAY
    VarType element_type;
    struct AST *element_type_def; // AST typedef not yet visible this early in the file
    bool is_packed;
    bool is_dynamic;
    int dimensions;
    int lower_bound;   // single-dim convenience field; see comment above
    int upper_bound;
    int *lower_bounds;
    int *upper_bounds;
    union {
        uint8_t *raw;               // packed-byte storage
        struct ValueStruct *elements; // flat buffer of Value elements
    };
    // VM 2.0 Phase 4e/4f: non-NULL marks this ArrayObj as a lightweight
    // VIEW produced by makeDynamicArraySliceValue's flat-multi-dimensional
    // case (a partial index into a static array[..,..] yields a sub-array
    // rather than a scalar). A view's lower_bounds/upper_bounds/raw/
    // elements point *into* view_of's own buffers via pointer arithmetic
    // (mirroring the pre-4e/4f design, which shared src's raw pointers +
    // refcount directly) -- it owns none of them. Keeping view_of alive
    // via a retained ObjHeader reference is what keeps those pointers
    // valid; arrayObjDestroy checks this field first and, when set,
    // releases view_of and frees only the view struct itself instead of
    // running the normal full teardown.
    struct ArrayObj *view_of;
} ArrayObj;

// Definition of Type struct for enum metadata
typedef struct EnumType {
    char *name;         // Name of the enum type
    char **members;     // Array of member names
    int member_count;   // Number of members
} Type;

// VM 2.0 Phase 4g (Docs/pscal_vm2_plan.md sec 5.10.4/5.10.3): deliberately
// keeps `enum_name`, reversing the plan's original "drop it, derive from
// enum_meta->name" sketch -- a research pass (not assumption) found
// `enum_meta` is populated in only two places in the whole tree (both pure
// copy-forward from an already-set operand, never a fresh lookup/registry
// populate), while `enum_name` is load-bearing for actual logic (enum-type
// equality/compatibility checks via strcmp, Low()/High()'s lookupType() by
// name), not just display. Dropping it would need a real interned
// Type/EnumType registry this codebase doesn't have yet -- out of scope
// for a representation-only sub-phase. `enum_meta` moves in alongside it
// (non-owned, shared/interned elsewhere -- see the poison-pragma listing
// in utils.h, already treating it as boxing-adjacent before this sub-phase
// touched it).
// VM 2.0 Phase 4i (Docs/pscal_vm2_plan.md sec 5.10.4/5.10.3): `enum_type_def`
// added by 4i's own research pass -- a dedicated audit of every
// PTR_BASE_TYPE_NODE call site (undertaken because 4i's physical struct
// collapse leaves nowhere left for an unboxed top-level `base_type_node`
// field to live) found TYPE_ENUM values genuinely read/write it too, not
// just TYPE_POINTER: vm.c's Succ/Pred-family arithmetic macro copies it
// forward from operand to result, and symbol.c/utils.c set it at enum
// construction time. This is NOT the same data as `enum_meta` above --
// `enum_meta` is a non-owned pointer to interned member-name metadata,
// while `enum_type_def` is the enum's own AST_ENUM_TYPE definition node
// (used for type-compatibility bookkeeping), a completely different
// piece of state that happened to share one unboxed top-level field with
// TYPE_POINTER's own AST-node/sentinel usage pre-4i. Non-owned (never
// freed by EnumObj's destructor, exactly like enum_meta and exactly like
// the old top-level base_type_node field was never freed for any type).
typedef struct EnumObj {
    ObjHeader header; // header.type is always TYPE_ENUM
    Type *enum_meta;
    int32_t ordinal;
    char *enum_name;
    struct AST *enum_type_def; // AST typedef not yet visible this early in the file
} EnumObj;

// VM 2.0 Phase 4g (Docs/pscal_vm2_plan.md sec 5.10.4/5.10.3): revised
// before implementation by 4g's own research pass -- the original sketch
// omitted `element_type`/`element_type_def`, but those are legitimately
// part of a typed file's (`file of X`) state, not dead weight, and are
// exactly the pair whose *array*-side collision already broke TYPE_FILE
// once in 4e/4f (see FILE_ELEMENT_TYPE/FILE_ELEMENT_TYPE_DEF's history
// below). Folding them in now closes that loose end. Exempt from Stage
// B's CoW/uniqueness path entirely (a file's OS handle has identity that
// must never be cloned); makeCopyOfValue's TYPE_FILE case retains the
// same FileObj rather than deep-copying, consistent with that exemption.
typedef struct FileObj {
    ObjHeader header; // header.type is always TYPE_FILE
    FILE *f;
    char *filename;
    int record_size;
    bool record_size_explicit;
    VarType element_type;
    struct AST *element_type_def; // AST typedef not yet visible this early in the file
} FileObj;

// VM 2.0 Phase 4g (Docs/pscal_vm2_plan.md sec 5.10.4/5.10.3): boxed ONLY
// `address` at first -- a 4g audit pass found `base_type_node` read/
// written on TYPE_ENUM and (apparently) TYPE_RECORD values too, not just
// TYPE_POINTER, and concluded boxing it into a TYPE_POINTER-only union
// member would reintroduce the "dual-purpose field reached through a
// type-specific union member" crash class already found twice that
// sub-phase. So `base_type_node` was left as a plain, unboxed top-level
// `Value` field, deliberately unboxed by PointerObj.
//
// **Reversed in 4i, by a follow-up audit forced by the physical struct
// collapse (there is nowhere left for an unboxed top-level field to live
// once `Value` becomes one `uint64_t`).** That audit found 4g's "TYPE-
// AGNOSTIC" conclusion was broader than the evidence supported:
// TYPE_RECORD turned out to be a red herring -- zero actual construction
// site anywhere sets a record's own `base_type_node` (the CALL_METHOD
// class-name lookup that reads it does so on a dereferenced record whose
// `base_type_node` is always NULL in practice; `makeValueForType`'s
// `TYPE_RECORD` case never touches it). Only two real users exist:
// TYPE_POINTER (this struct) and TYPE_ENUM (which gets its own
// `enum_type_def` field on `EnumObj` instead -- see that struct's
// comment). Every "generic assignment-preservation" call site that used
// to read/write `PTR_BASE_TYPE_NODE` on a value of unknown type (vm.c's
// `replaceValueCell`/`vmStoreThreadMyself`/`SET_INDIRECT`'s fallback
// path, `resolveRecordTypeFromBaseValue`, `CALL_METHOD`'s className
// lookup, two `DEBUG_PRINT`s in symbol.c) is now guarded by an explicit
// `VALUE_TYPE(v) == TYPE_POINTER` check before calling
// `PTR_BASE_TYPE_NODE`, matching every other `PSCAL_VALUE_FIELD`-based
// accessor's contract (valid only when the Value is actually that type).
// `PTR_BASE_TYPE_NODE`'s macro body now dereferences `ptr_val`, so an
// unguarded call on a non-pointer Value is a real bug (reads garbage
// through the wrong union member), not a latent-but-harmless no-op the
// way it was pre-4i.
//
// The `mode` field replacing the sentinel-in-`base_type_node` overload
// is still separately deferred (a different audit found the proposed
// 4-value enum covers only 3 of 7 real sentinels across ~90 call sites)
// -- this move ships the sentinel scheme completely unchanged, just
// relocated to live inside PointerObj instead of at Value's top level.
//
// **Copy-on-construct, not copy-on-write -- confirmed necessary, not just
// theoretical.** makeCopyOfValue/copyValueForStack must always allocate a
// FRESH, independent PointerObj per copy (never pscalObjRetain an
// existing one). An earlier draft of this file tried a bare, unretained
// alias instead (reasoning by analogy from FileObj, where CALL_BUILTIN's
// argument cleanup always skips freeValue for TYPE_FILE specifically) --
// that reasoning does NOT transfer to pointers: unlike file arguments,
// transient TYPE_POINTER copies get freeValue'd directly and
// unconditionally by plenty of opcodes that carry no such skip (found by
// actually running `px := @x; px^ := 99;`, which crashed -- px^'s read
// pushed a bare-aliased copy of px's own PointerObj via GET_GSLOT, and
// SET_INDIRECT's ordinary freeValue on that transient copy released px's
// real, persistent wrapper out from under it). Every copy must therefore
// be genuinely independent (refcount 1, never shared) -- which in turn
// means CALL_BUILTIN's argument cleanup (vm.c) must NOT skip freeValue
// for TYPE_POINTER the way it still does for TYPE_FILE, since a fresh,
// unshared wrapper needs exactly one release to avoid leaking, and
// nothing else will ever provide it. `base_type_node` is copied forward
// into the fresh wrapper by makeCopyOfValue exactly as `address` is --
// see that function's TYPE_POINTER case.
typedef struct PointerObj {
    ObjHeader header; // header.type is always TYPE_POINTER
    struct ValueStruct *address;
    struct AST *base_type_node; // AST typedef not yet visible this early in the file
} PointerObj;

// VM 2.0 Phase 4i checkpoint 3a (Docs/pscal_vm2_plan.md sec 5.10.3/
// 5.10.4): TYPE_CLOSURE/TYPE_INTERFACE were never actually boxed into a
// single heap pointer in 4b -- that sub-phase ported the *payload*
// (ClosureEnvPayload, `env`/`payload` below) onto ObjHeader, but the
// Value-level union members stayed inline multi-field anonymous structs
// (`entry_offset`+`symbol`+`env`, `type_def`+`payload`). That was fine
// for every prior sub-phase (physical layout was never the concern
// until now), but it means these two types don't fit in a single
// tagged-word `bits` pointer as they stand -- boxing them properly is a
// checkpoint 3 prerequisite, not optional. Field names/order match the
// old inline structs exactly, so every existing `AS_CLOSURE(v).field`/
// `AS_INTERFACE(v).field` call site keeps compiling unchanged once
// AS_CLOSURE/AS_INTERFACE dereference a pointer instead of returning the
// struct by value (same coexistence trick as AS_ENUM/AS_SET).
//
// **Deliberately NOT ObjHeader-based, unlike every other boxed type in
// this file -- a real conflict found only by trying it.** `ObjHeader`'s
// generic retain/release dispatch keys SOLELY on `VarType`, one
// destructor slot per type. But `ClosureEnvPayload` (the `env`/`payload`
// field below) ALREADY claims both `TYPE_CLOSURE` and `TYPE_INTERFACE`
// as ITS OWN `header.type` (see `createClosureEnv`'s `owner_type`
// parameter and `registerClosureEnvDestructor` in core/utils.c, both
// from 4b) -- the payload uses these two tags to distinguish which kind
// of construct owns it, not to mean "this object IS a Value-level
// TYPE_CLOSURE/TYPE_INTERFACE wrapper". A `ClosureObj`/`InterfaceObj`
// with its own embedded `ObjHeader` tagged the same way collides on that
// single destructor slot -- confirmed by `pscalObjRegisterDestructor`
// aborting ("already has a destructor registered") the first time a
// real closure was created and boxed under this scheme.
//
// The fix: these two wrappers don't need refcounting of their own at
// all. The only actually-shared, actually-refcounted resource is
// `env`/`payload` itself (already ObjHeader-based, already retained/
// released via `retainClosureEnv`/`releaseClosureEnv`); `entry_offset`/
// `symbol`/`type_def` are plain copyable data with no ownership
// semantics. So each wrapper is a plain, singly-owned heap allocation
// (no `pscalObjRegisterDestructor`, no `pscalObjRetain`) --
// `makeCopyOfValue` allocates a FRESH wrapper per copy and copies
// `entry_offset`/`symbol`/`type_def` by value, retaining the SAME
// `env`/`payload` (copy-on-construct for the wrapper, share-by-retain
// for the payload it points at) -- observably identical to sharing the
// wrapper itself would have been, since `env`/`payload` identity (not
// wrapper identity) is what every comparison/dispatch site actually
// checks (confirmed: vm.c's closure-equality check compares
// `entry_offset`/`symbol`/`env` field values, never wrapper pointers).
// `freeValue` releases `env`/`payload` then frees the wrapper directly.
typedef struct ClosureObj {
    uint32_t entry_offset;
    struct Symbol_s *symbol;
    ClosureEnvPayload *env;
} ClosureObj;

typedef struct InterfaceObj {
    struct AST *type_def; // AST typedef not yet visible this early in the file
    ClosureEnvPayload *payload;
} InterfaceObj;

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
        FileObj *f_val; // VM 2.0 Phase 4g: was a plain FILE*
        ArrayObj *array_val; // VM 2.0 Phase 4e/4f: was a plain struct ValueStruct*
        MStream *mstream;
        EnumObj *enum_val; // VM 2.0 Phase 4g: was an embedded struct, now a heap pointer
        PointerObj *ptr_val; // VM 2.0 Phase 4g: was a plain struct ValueStruct*
        ClosureObj *closure; // VM 2.0 Phase 4i: was an embedded struct, now a heap pointer
        InterfaceObj *interface; // VM 2.0 Phase 4i: was an embedded struct, now a heap pointer
    };
    uint8_t *array_raw;
    bool array_is_packed;
    bool array_is_dynamic;
    uint32_t *array_refcount;
    // VM 2.0 Phase 4i: dead weight, like filename/record_size/etc below --
    // moved into PointerObj.base_type_node (TYPE_POINTER) and
    // EnumObj.enum_type_def (TYPE_ENUM). Nothing reads this field
    // directly anymore; deleted at the flag day along with the rest.
    AST *base_type_node;

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

    // VM 2.0 Phase 4i checkpoint 2 (Docs/pscal_vm2_plan.md sec 5.10.1/
    // 5.10.4): the tagged-word encoding from core/obj_header.h, now kept
    // as a live MIRROR of the discrete fields above for every inline-
    // scalar kind that needs no heap allocation to tag (VOID/NIL/
    // BOOLEAN/CHAR/WIDECHAR/BYTE/WORD/INT8-32/UINT8-32/FLOAT/DOUBLE).
    // `Value` stays deliberately oversized during this checkpoint --
    // both representations coexist and are cross-checked (see
    // pscalValueBitsConsistent in core/utils.c, called from freeValue)
    // so a bug in the NEW encoding surfaces as an assertion failure
    // during testing, not silent corruption or a crash the way it would
    // if this landed at the same time as the physical struct collapse.
    // TYPE_INT64/TYPE_UINT64/TYPE_LONG_DOUBLE (which need Int64Box/
    // LongDoubleBox, built in 4h but not yet wired in) and every heap-
    // pointer type (STRING/ARRAY/RECORD/FILE/ENUM/POINTER/SET/CLOSURE/
    // INTERFACE/MEMORYSTREAM) are deferred to checkpoint 3, which
    // combines "everything moves into bits" with the physical shrink --
    // for those types `bits` is not yet meaningful and
    // pscalValueBitsConsistent skips them.
    uint64_t bits;
} Value;

// VM 2.0 Phase 4i checkpoint 2: dispatches on `dest->type` (already set by
// every call site below -- every scalar constructor sets `.type` before
// calling SET_INT_VALUE/SET_REAL_VALUE/SET_CHAR_VALUE, and SET_VALUE_TYPE
// itself sets `.type` first thing) to compute the matching tagged word.
// Heap-pointer types and TYPE_INT64/TYPE_UINT64/TYPE_LONG_DOUBLE are
// deferred to checkpoint 3 (see Value's own comment) -- `default: break`
// leaves `bits` whatever SET_VALUE_TYPE last put there for those types,
// which pscalValueBitsConsistent knows to skip.
static inline void pscalValueSetIntBits(Value *dest, long long val) {
    switch (dest->type) {
        case TYPE_BOOLEAN:  dest->bits = pscalTagBoolean(val != 0); break;
        case TYPE_CHAR:     dest->bits = pscalTagChar((char)val); break;
        case TYPE_WIDECHAR: dest->bits = pscalTagWideChar((uint32_t)val); break;
        case TYPE_BYTE:     dest->bits = pscalTagByte((uint8_t)val); break;
        case TYPE_WORD:     dest->bits = pscalTagWord((uint16_t)val); break;
        case TYPE_INT8:     dest->bits = pscalTagInt8((int8_t)val); break;
        case TYPE_UINT8:    dest->bits = pscalTagUInt8((uint8_t)val); break;
        case TYPE_INT16:    dest->bits = pscalTagInt16((int16_t)val); break;
        case TYPE_UINT16:   dest->bits = pscalTagUInt16((uint16_t)val); break;
        case TYPE_INT32:    dest->bits = pscalTagInt32((int32_t)val); break;
        case TYPE_UINT32:   dest->bits = pscalTagUInt32((uint32_t)val); break;
        case TYPE_THREAD:   dest->bits = pscalTagInt32((int32_t)val); break;
        default: break; // TYPE_INT64/TYPE_UINT64: deferred to checkpoint 3
    }
}

static inline void pscalValueSetRealBits(Value *dest, long double val) {
    switch (dest->type) {
        case TYPE_FLOAT:  dest->bits = pscalTagFloat((float)val); break;
        case TYPE_DOUBLE: dest->bits = pscalBoxDouble((double)val); break;
        default: break; // TYPE_LONG_DOUBLE: deferred to checkpoint 3 (needs LongDoubleBox)
    }
}

static inline void pscalValueSetCharBits(Value *dest, int val) {
    if (dest->type == TYPE_WIDECHAR) {
        dest->bits = pscalTagWideChar((uint32_t)val);
    } else {
        dest->bits = pscalTagChar((char)val);
    }
}

// VM 2.0 Phase 4i checkpoint 3b: mirrors any of the 8 heap-pointer union
// members (s_val/array_val/record_val/f_val/enum_val/ptr_val/set_val/
// mstream) into `bits`, via the generic pointer-tag scheme from
// core/obj_header.h. Unlike the scalar setters above, there is no
// per-type encoding to dispatch on -- every heap pointer is boxed
// identically (pscalTagPointer), and the decode-side consistency check
// (pscalValueBitsConsistent) re-derives which union member to compare
// against from dest->type, matching the scalar precedent. `ptr` may be
// NULL (a not-yet-constructed or just-released wrapper, or a detached
// alias) -- pscalTagPointer(NULL) round-trips cleanly. Callers pass the
// exact pointer just stored in the union member, not the Value's type --
// this function trusts its caller the same way pscalValueSetIntBits
// trusts dest->type to already be correct.
static inline void pscalValueSetHeapPtrBits(Value *dest, const void *ptr) {
    dest->bits = pscalTagPointer(ptr);
}

// Resets `bits` to a zero-valued placeholder matching `t` -- called
// whenever a Value's type changes, so `bits` never silently carries a
// stale kind tag left over from whatever type the Value used to be
// (confirmed necessary, not theoretical: makeWideChar builds on makeChar
// and then overwrites .type, which would otherwise leave a CHAR-kind tag
// on a WIDECHAR value). Deferred (non-scalar) types get bits=0, a
// harmless placeholder pscalValueBitsConsistent knows to skip.
static inline void pscalValueResetBitsForType(Value *dest, VarType t) {
    switch (t) {
        case TYPE_VOID:     dest->bits = pscalTagVoid(); break;
        case TYPE_NIL:      dest->bits = pscalTagNil(); break;
        case TYPE_BOOLEAN:  dest->bits = pscalTagBoolean(false); break;
        case TYPE_CHAR:     dest->bits = pscalTagChar(0); break;
        case TYPE_WIDECHAR: dest->bits = pscalTagWideChar(0); break;
        case TYPE_BYTE:     dest->bits = pscalTagByte(0); break;
        case TYPE_WORD:     dest->bits = pscalTagWord(0); break;
        case TYPE_INT8:     dest->bits = pscalTagInt8(0); break;
        case TYPE_UINT8:    dest->bits = pscalTagUInt8(0); break;
        case TYPE_INT16:    dest->bits = pscalTagInt16(0); break;
        case TYPE_UINT16:   dest->bits = pscalTagUInt16(0); break;
        case TYPE_INT32:    dest->bits = pscalTagInt32(0); break;
        case TYPE_UINT32:   dest->bits = pscalTagUInt32(0); break;
        case TYPE_FLOAT:    dest->bits = pscalTagFloat(0.0f); break;
        case TYPE_DOUBLE:   dest->bits = pscalBoxDouble(0.0); break;
        case TYPE_THREAD:   dest->bits = pscalTagInt32(0); break;
        // VM 2.0 Phase 4i checkpoint 3b: heap-pointer types get a real
        // nil-pointer placeholder (not bare 0) the moment a Value is
        // retyped to one of these, matching the scalar precedent of
        // never leaving a stale kind tag around -- and closing off the
        // same "makeX()-then-retype" hazard checkpoint 2 found for
        // TYPE_THREAD: any retype to a heap type now round-trips through
        // pscalValueBitsConsistent even before the real wrapper exists,
        // instead of leaving bits=0 (which pscalTaggedWordKind would
        // misread as an inline PSCAL_TAG_VOID-shaped immediate, not a
        // pointer, once checkpoint 3b's consistency check below stops
        // treating these types as vacuously true).
        case TYPE_STRING:
        case TYPE_UNICODE_STRING:
        case TYPE_ARRAY:
        case TYPE_RECORD:
        case TYPE_FILE:
        case TYPE_ENUM:
        case TYPE_POINTER:
        case TYPE_SET:
        case TYPE_MEMORYSTREAM:
            dest->bits = pscalTagPointer(NULL); break;
        default:            dest->bits = 0; break; // deferred: INT64/UINT64/LONG_DOUBLE (checkpoint 3c)
    }
}

/* Helpers to initialise numeric fields consistently.
 *
 * Each macro captures `val` into a block-scoped local exactly ONCE before
 * using it more than once in the expansion (i_val+u_val+bits for
 * SET_INT_VALUE; c_val+bits for SET_CHAR_VALUE). This is not just style --
 * `val` is substituted literally at every occurrence in a macro expansion,
 * so a caller passing a SELF-REFERENTIAL expression that reads back
 * through `dest` (e.g. `SET_INT_VALUE(&v, -VAL_INT(v))`) would otherwise
 * re-evaluate against an already-mutated field partway through the
 * expansion (the first substitution's write already changed `dest`,
 * so the second/third substitution's read sees the NEW value, not the
 * original) -- confirmed as a real, live bug (VM 2.0 Phase 4i checkpoint
 * 2's bits-consistency verification caught it: a compile-time unary-minus
 * evaluator's `SET_INT_VALUE(&v, -VAL_INT(v))`-shaped call silently
 * corrupted u_val/bits because i_val had already flipped sign by the time
 * the later substitutions ran). SET_REAL_VALUE already derives d_val/
 * f32_val/bits from the just-written r_val field rather than re-reading
 * `val`, so it doesn't need this treatment. */
#define SET_INT_VALUE(dest, val) \
    do { long long _pscal_set_int_tmp = (long long)(val); \
         (dest)->i_val = _pscal_set_int_tmp; (dest)->u_val = (unsigned long long)_pscal_set_int_tmp; \
         pscalValueSetIntBits((dest), _pscal_set_int_tmp); } while(0)
#define SET_REAL_VALUE(dest, val) \
    do { (dest)->real.r_val = (long double)(val); (dest)->real.d_val = (double)(dest)->real.r_val; \
         (dest)->real.f32_val = (float)(dest)->real.r_val; \
         pscalValueSetRealBits((dest), (dest)->real.r_val); } while(0)
#define SET_CHAR_VALUE(dest, val) \
    do { int _pscal_set_char_tmp = (val); \
         PSCAL_VALUE_FIELD(*(dest), c_val) = _pscal_set_char_tmp; \
         pscalValueSetCharBits((dest), _pscal_set_char_tmp); } while(0)
#define SET_VALUE_TYPE(dest, t) \
    do { PSCAL_VALUE_FIELD(*(dest), type) = (t); pscalValueResetBitsForType((dest), (t)); } while(0)

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
// VM 2.0 Phase 4e/4f: array_val is now an ArrayObj*; AS_ARRAY/AS_ARRAY_RAW
// dereference to its element/raw-byte buffer so existing
// `AS_ARRAY(v) = X` / `AS_ARRAY_RAW(v) = X` whole-buffer-reassignment call
// sites (~20 of them, the same idiom found in 4c for strings) keep working
// unchanged, given the wrapper already exists (see pscalArrayEnsureObj for
// the sites where it doesn't yet).
#define AS_ARRAY(v)      (PSCAL_VALUE_FIELD(v, array_val)->elements)
// VM 2.0 Phase 4g: f_val is now a FileObj* (was a plain FILE*); AS_FILE
// dereferences it so existing `AS_FILE(v)` reads/writes (a FILE*) keep
// working unchanged, given the wrapper already exists (see
// pscalFileEnsureObj for the one site where it doesn't yet).
#define AS_FILE(v)       (PSCAL_VALUE_FIELD(v, f_val)->f)
#define AS_MSTREAM(v)    PSCAL_VALUE_FIELD(v, mstream)
// VM 2.0 Phase 4g: ptr_val is now a PointerObj* (was a plain struct
// ValueStruct*); AS_POINTER dereferences it so existing `AS_POINTER(v)`
// reads/writes (an address) keep working unchanged, given the wrapper
// already exists (see pscalPointerEnsureObj for chicken-egg sites).
#define AS_POINTER(v)    (PSCAL_VALUE_FIELD(v, ptr_val)->address)
// VM 2.0 Phase 4g: enum_val is now an EnumObj* (was an embedded struct);
// AS_ENUM dereferences it so existing `AS_ENUM(v).enum_name`/`.ordinal`
// call sites keep working with `.` unchanged (EnumObj's field names match
// the old embedded struct's on purpose, same trick as AS_SET/SetObj).
#define AS_ENUM(v)       (*PSCAL_VALUE_FIELD(v, enum_val))
#define AS_CLOSURE(v)    (*PSCAL_VALUE_FIELD(v, closure))
#define AS_INTERFACE(v)  (*PSCAL_VALUE_FIELD(v, interface))
// VM 2.0 Phase 4c: set_val is now a SetObj* (was an embedded struct); AS_SET
// dereferences it so existing `AS_SET(v).set_size`/`.set_values` call sites
// keep working with `.` unchanged (SetObj's field names match the old
// embedded struct's on purpose -- see core/types.h's SetObj comment).
#define AS_SET(v)        (*PSCAL_VALUE_FIELD(v, set_val))
#define SET_CAPACITY(v)  (PSCAL_VALUE_FIELD(v, set_val)->capacity)
#define AS_ARRAY_RAW(v)  (PSCAL_VALUE_FIELD(v, array_val)->raw)

/* Array/string/file/pointer metadata accessors. VM 2.0 Phase 4e/4f moves
 * the array ones into ArrayObj; string ones already moved into StringObj
 * in 4c. All of these require v.array_val/.s_val to already be a valid
 * wrapper -- see pscalArrayEnsureObj/pscalStringEnsureObj for where that
 * isn't guaranteed yet by construction order alone. */
#define ARRAY_LOWER_BOUND(v)       (PSCAL_VALUE_FIELD(v, array_val)->lower_bound)
#define ARRAY_UPPER_BOUND(v)       (PSCAL_VALUE_FIELD(v, array_val)->upper_bound)
#define ARRAY_LOWER_BOUNDS(v)      (PSCAL_VALUE_FIELD(v, array_val)->lower_bounds)
#define ARRAY_UPPER_BOUNDS(v)      (PSCAL_VALUE_FIELD(v, array_val)->upper_bounds)
#define ARRAY_DIMENSIONS(v)        (PSCAL_VALUE_FIELD(v, array_val)->dimensions)
#define ARRAY_ELEMENT_TYPE(v)      (PSCAL_VALUE_FIELD(v, array_val)->element_type)
#define ARRAY_ELEMENT_TYPE_DEF(v)  (PSCAL_VALUE_FIELD(v, array_val)->element_type_def)
#define ARRAY_IS_PACKED(v)         (PSCAL_VALUE_FIELD(v, array_val)->is_packed)
#define ARRAY_IS_DYNAMIC(v)        (PSCAL_VALUE_FIELD(v, array_val)->is_dynamic)
// VM 2.0 Phase 4g: element_type/element_type_def moved inside FileObj
// (folded in during file boxing, closing the loose end 4e/4f's postmortem
// flagged -- see FileObj's comment). Requires v.f_val to already be a
// valid FileObj*.
#define FILE_ELEMENT_TYPE(v)      (PSCAL_VALUE_FIELD(v, f_val)->element_type)
#define FILE_ELEMENT_TYPE_DEF(v)  (PSCAL_VALUE_FIELD(v, f_val)->element_type_def)
// ARRAY_REFCOUNT is intentionally removed (VM 2.0 Phase 4e/4f): the old
// uint32_t* scheme is gone. Every former call site now uses
// pscalObjRetain/pscalObjRelease on &v.array_val->header instead, inside
// the SAME mutex-protected critical sections as before -- see
// core/types.h's ArrayObj comment for why the mutex itself isn't retired
// yet (that's Stage B/4j's job, once Value itself is a single atomic word).
// VM 2.0 Phase 4c: max_length now lives inside StringObj, not directly on
// Value (the old Value.max_length field is dead weight until Phase 4i's
// struct shrink deletes it -- nothing should read it directly anymore).
// Requires v.s_val to already be a valid StringObj*; see AS_STRING's
// comment (core/utils.h) and pscalStringEnsureObj for why/when that holds.
#define STRING_MAX_LENGTH(v)       (PSCAL_VALUE_FIELD(v, s_val)->max_length)
// VM 2.0 Phase 4i: moved inside PointerObj (see that struct's comment for
// the full history -- 4g left this unboxed reasoning from an
// over-broad "type-agnostic" audit finding; 4i's follow-up audit found
// TYPE_RECORD was a red herring and TYPE_ENUM gets its own dedicated
// ENUM_TYPE_DEF macro/field instead). Valid ONLY when VALUE_TYPE(v) ==
// TYPE_POINTER, exactly like every other PSCAL_VALUE_FIELD-based
// accessor -- callers that used to call this on a value of unknown type
// (the "generic assignment-preservation" call sites) now check
// VALUE_TYPE first. Requires v.ptr_val to already be a valid PointerObj*
// (always true once a Value is TYPE_POINTER, matching every other boxed
// type's "wrapper always present" invariant).
#define PTR_BASE_TYPE_NODE(v)      (PSCAL_VALUE_FIELD(v, ptr_val)->base_type_node)
// VM 2.0 Phase 4i: the enum-specific counterpart to PTR_BASE_TYPE_NODE --
// see EnumObj's comment for why this is a separate field/macro rather
// than sharing PointerObj's. Valid ONLY when VALUE_TYPE(v) == TYPE_ENUM.
// Requires v.enum_val to already be a valid EnumObj*.
#define ENUM_TYPE_DEF(v)           (PSCAL_VALUE_FIELD(v, enum_val)->enum_type_def)
// VM 2.0 Phase 4g: filename/record_size/record_size_explicit moved inside
// FileObj; the old top-level Value fields are dead weight until Phase
// 4i's struct shrink deletes them. Requires v.f_val to already be a valid
// FileObj*.
#define FILE_FILENAME(v)           (PSCAL_VALUE_FIELD(v, f_val)->filename)
#define FILE_RECORD_SIZE(v)        (PSCAL_VALUE_FIELD(v, f_val)->record_size)
#define FILE_RECORD_SIZE_EXPLICIT(v) (PSCAL_VALUE_FIELD(v, f_val)->record_size_explicit)
// VM 2.0 Phase 4g: enum_meta moved inside EnumObj; the old top-level
// Value.enum_meta field is dead weight until Phase 4i's struct shrink
// deletes it -- nothing should read it directly anymore. Requires
// v.enum_val to already be a valid EnumObj* (always true once a Value is
// TYPE_ENUM, matching StringObj/SetObj/RecordObj precedent).
#define ENUM_META(v)               (PSCAL_VALUE_FIELD(v, enum_val)->enum_meta)

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
