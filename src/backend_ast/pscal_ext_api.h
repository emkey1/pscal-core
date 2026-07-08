// src/backend_ast/pscal_ext_api.h
//
// VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1): the frozen, versioned C ABI
// a dlopen'd plugin uses to talk back to the host VM. This header is the
// entire contract -- a third-party plugin author includes ONLY this file
// (not core/types.h, not backend_ast/builtin.h, not any other pscal-core
// internal header) and links against nothing; every host capability arrives
// through the PscalExtHostApi vtable passed to pscal_ext_register().
//
// Why a vtable instead of letting the plugin call registerVmBuiltin()/
// runtimeError()/makeInt() etc. directly via dlopen-resolved symbols: those
// symbols are real pscal-core internals with no ABI-stability promise --
// their signatures already changed once this VM 2.0 effort (registerVmBuiltin
// gained an EffectMask parameter in Phase 6). A symbol-resolution plugin
// would silently miscall on the next such change (wrong argument in a
// register, garbage effect mask, maybe a crash). A vtable built by a known
// host binary and checked against host_abi at load time fails LOUDLY
// instead: an old plugin against a new host either gets a clean ABI-mismatch
// rejection (major version bump) or keeps working unchanged (a additive
// minor-version field it doesn't know about, appended at the end -- append-
// only, mirroring the VarType/opcode-numbering convention elsewhere in this
// codebase, Docs/pscal_vm2_plan.md §2 and core/var_type.h).
//
// Value accessors: a plugin must never see raw Value internals (the
// AS_*/PSCAL_VALUE_PTR macros in core/types.h dereference boxed heap
// wrappers -- RecordObj, ArrayObj, StringObj, ... -- whose layouts are
// exactly what VM 2.0 Phase 4's accessor-macro discipline exists to hide,
// per that phase's own risk register). This header exposes the *scalar*
// slice of that surface a self-contained handle-table-style extension
// category actually needs (sqlite, yyjson-shaped work): integers, doubles,
// strings, booleans, nil, plus type-tag predicates. A plugin needing
// records/arrays/closures/etc. is out of scope for this phase -- the
// sqlite/yyjson categories, the two extension categories with private
// handle tables today, are exactly the shape this phase targets (§7.1).
//
// `Value` itself (a 16-byte `{ VarType type; uint64_t bits; }` POD, VM 2.0
// Phase 4i checkpoint 3d) is safe to pass by value across the ABI boundary:
// a plugin may read `.type` directly (the discriminant tag, not a boxed
// payload) but must never interpret `.bits` itself -- that's what the
// accessor functions below are for. VarType (core/var_type.h) is its own
// minimal, dependency-free, append-only header for exactly this reason:
// it's the one piece of core/types.h a plugin can safely see whole.
#ifndef PSCAL_EXT_API_H
#define PSCAL_EXT_API_H

#include "core/var_type.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Value: mirrors core/types.h's `ValueStruct` bit-for-bit. Defined here
// (not included from core/types.h) so a plugin never drags in the rest of
// that header's internal struct definitions. If pscal-core's own
// core/types.h is *also* included in the same translation unit (true for
// the in-tree extension categories, false for a real third-party plugin),
// the two definitions must stay identical -- core/types.h defines the same
// guard macro right after its own `Value` typedef specifically so whichever
// header is included first wins and the second is a no-op, never a
// redefinition error.
#ifndef PSCAL_VALUE_TYPE_DEFINED
#define PSCAL_VALUE_TYPE_DEFINED
typedef struct ValueStruct {
    VarType type;
    uint64_t bits;
} Value;
#endif

struct VM_s;

// Mirrors backend_ast/builtin.h's VmBuiltinFn/BuiltinRoutineType, under
// distinct names (PscalExtBuiltinFn/PscalExtBuiltinKind) so this header
// never collides with builtin.h even though both may appear in one TU
// (true for the sqlite-as-plugin proof, which links against pscal-core
// internals to implement its vtable-populating glue; never true for a
// real third-party plugin, which sees only this header).
typedef Value (*PscalExtBuiltinFn)(struct VM_s *vm, int arg_count, Value *args);

typedef enum {
    PSCAL_EXT_BUILTIN_NONE,
    PSCAL_EXT_BUILTIN_PROCEDURE,
    PSCAL_EXT_BUILTIN_FUNCTION
} PscalExtBuiltinKind;

// Mirrors core/effect_mask.h's EffectMask bitmask (duplicated for the same
// reason; the two are `uint32_t`s with the same bit assignments and must be
// kept in sync manually -- effect classes are a Phase 6 concept, not
// expected to grow often, and getting this wrong just misclassifies a
// plugin builtin's sandbox/replay behavior rather than corrupting memory).
typedef uint32_t PscalExtEffectMask;
#define PSCAL_EXT_FX_PURE   ((PscalExtEffectMask)0)
#define PSCAL_EXT_FX_IO     ((PscalExtEffectMask)(1u << 0))
#define PSCAL_EXT_FX_NET    ((PscalExtEffectMask)(1u << 1))
#define PSCAL_EXT_FX_PROC   ((PscalExtEffectMask)(1u << 2))
#define PSCAL_EXT_FX_CLOCK  ((PscalExtEffectMask)(1u << 3))
#define PSCAL_EXT_FX_RANDOM ((PscalExtEffectMask)(1u << 4))

// ---------------------------------------------------------------------------
// Generic handle table. sqlite_builtins.c and yyjson_builtins.c each hand-
// roll an identical realloc-growable, mutex-guarded, tagged-slot array
// (alloc scans for a free slot, grows by doubling, reset-on-free) purely to
// hand programs a small integer "handle" instead of a raw pointer. That
// duplication is exactly the kind of boilerplate a plugin author outside
// pscal-core shouldn't have to re-derive (and re-debug: both in-tree copies
// independently reimplement the same free-slot-scan and growth-lock logic).
// The host owns the table; a plugin gets an opaque `PscalExtHandleTable*`
// and never sees its internal array. Kind is a plugin-defined tag (e.g.
// "this is a db handle" vs "this is a statement handle") so one table can
// back multiple handle spaces the way sqlite's does.
//
// Deliberately NOT retrofitted onto the in-tree sqlite/yyjson categories in
// this phase -- those keep static registration and their private tables
// untouched (§7.1's "existing in-tree categories keep static registration");
// this helper exists for plugins (and the sqlite-as-plugin proof uses it),
// proving the mechanism without touching working, already-shipped code.
typedef struct PscalExtHandleTable PscalExtHandleTable;

#define PSCAL_EXT_HANDLE_KIND_UNUSED 0

// ---------------------------------------------------------------------------
// The host API vtable. Passed to pscal_ext_register() by value-through-
// pointer (the plugin must not free or outlive it -- it's owned by the host
// process and valid for the process lifetime). Append new fields only at
// the end and bump PSCAL_EXT_ABI_MINOR, never reorder/remove existing ones
// -- that's what keeps an old plugin loadable against a newer host with an
// additive-only ABI change.
typedef struct PscalExtHostApi {
    // --- Registration -------------------------------------------------
    // Mirrors registerVmBuiltin()'s real signature as of VM 2.0 Phase 6
    // (backend_ast/builtin.h): runtime binding by name PLUS synthesizing
    // the compile-time function/procedure declaration every frontend's
    // semantic pass resolves against (ch4.md §4.0) -- one call does both.
    void (*register_builtin)(const char *vm_name, PscalExtBuiltinFn handler,
                              PscalExtBuiltinKind kind, const char *display_name,
                              PscalExtEffectMask effect_mask);

    // Introspection catalog entries (ext_builtins/registry.h), so a loaded
    // plugin's functions show up in `--dump-ext-builtins` like any in-tree
    // category's do. Safe to no-op these calls; only register_builtin is
    // load-bearing for a plugin's builtins to actually work.
    void (*register_category)(const char *name);
    void (*register_group)(const char *category, const char *group);
    void (*register_function_entry)(const char *category, const char *group,
                                     const char *func);

    // --- Diagnostics ----------------------------------------------------
    // Mirrors runtimeError(VM*, const char*, ...) (vm/vm.h) exactly.
    void (*runtime_error)(struct VM_s *vm, const char *format, ...);

    // --- Value constructors ---------------------------------------------
    Value (*make_int)(long long val);
    Value (*make_int64)(long long val);
    Value (*make_double)(double val);
    Value (*make_string)(const char *val);
    Value (*make_string_len)(const char *val, size_t len);
    Value (*make_boolean)(int b);
    Value (*make_nil)(void);

    // --- Value accessors --------------------------------------------------
    // Callers must check value_type()/the is_*_type predicates first, the
    // same discipline the in-tree categories already follow -- these do not
    // themselves re-validate the tag (matching AS_INTEGER/AS_STRING/etc.,
    // which are unchecked too; the checked entry points are IS_INTLIKE/
    // IS_REAL/isPascalStringType, mirrored below as predicates over VarType).
    long long (*as_int64)(Value v);
    double (*as_double)(Value v);
    int (*as_bool)(Value v);
    // Returns the live string buffer (NOT a copy) -- valid exactly as long
    // as the Value itself is (matching AS_STRING's own contract). NULL for
    // a NIL/empty string, matching the in-tree convention.
    const char *(*as_cstring)(Value v);

    // --- Type-tag predicates ------------------------------------------
    // Mirror isPascalStringType/isIntlikeType/isRealType (core/utils.h),
    // operating on the safe-to-read `Value.type` field directly -- a plugin
    // may read `v.type` itself (it's the plain discriminant, not a boxed
    // payload) and pass it here instead of re-deriving these switches.
    bool (*is_string_type)(VarType t);
    bool (*is_intlike_type)(VarType t);
    bool (*is_real_type)(VarType t);

    // --- Generic handle table -------------------------------------------
    // Thread-safe: alloc/lookup/free all take an internal lock. `kind` is
    // caller-defined (use small positive ints distinct per handle space);
    // PSCAL_EXT_HANDLE_KIND_UNUSED (0) is reserved and must not be passed to
    // handle_alloc. `payload` is an opaque caller pointer stored verbatim.
    PscalExtHandleTable *(*handle_table_create)(void);
    void (*handle_table_destroy)(PscalExtHandleTable *table);
    // Returns a non-negative handle, or -1 on allocation failure.
    int (*handle_alloc)(PscalExtHandleTable *table, int kind, void *payload);
    // Returns false (payload untouched) if `handle` is out of range, freed,
    // or of a different kind than requested.
    bool (*handle_lookup)(PscalExtHandleTable *table, int handle, int kind,
                           void **out_payload);
    // Frees the slot (safe to call on an already-freed/invalid handle --
    // a no-op in that case, matching the in-tree tables' reset-on-close
    // idempotence). Returns the payload that was stored there (NULL if
    // there wasn't one), so the caller can release whatever it points to.
    void *(*handle_free)(PscalExtHandleTable *table, int handle);
} PscalExtHostApi;

// ---------------------------------------------------------------------------
// ABI versioning. `host_abi` passed to pscal_ext_register() is
// (major << 16) | minor. A plugin built against this header rejects itself
// against the wrong major (breaking field removal/reorder/signature change)
// but accepts any minor >= the minor it was compiled against (purely
// additive vtable growth). PSCAL_EXT_ABI_CURRENT is what the host passes;
// PSCAL_EXT_ABI_MAKE/·_MAJOR/·_MINOR are for plugins doing their own check.
#define PSCAL_EXT_ABI_MAJOR 1
#define PSCAL_EXT_ABI_MINOR 0
#define PSCAL_EXT_ABI_MAKE(major, minor) \
    ((uint32_t)(((uint32_t)(major) << 16) | (uint16_t)(minor)))
#define PSCAL_EXT_ABI_MAJOR_OF(abi) ((uint32_t)(abi) >> 16)
#define PSCAL_EXT_ABI_MINOR_OF(abi) ((uint32_t)(abi) & 0xFFFFu)
#define PSCAL_EXT_ABI_CURRENT PSCAL_EXT_ABI_MAKE(PSCAL_EXT_ABI_MAJOR, PSCAL_EXT_ABI_MINOR)

// Every plugin shared object must export exactly this symbol. `host_abi` is
// PSCAL_EXT_ABI_CURRENT as defined by the *host* binary at the moment it
// loaded the plugin -- compare PSCAL_EXT_ABI_MAJOR_OF(host_abi) against the
// major the plugin was built against before touching `host` at all. Return
// 0 on success; any nonzero return is treated by the loader as a clean
// registration failure (the plugin is unloaded, no builtins from it are
// assumed registered). Must not throw/longjmp/abort -- a plugin that dies
// inside this call is exactly the "malformed plugin" case the loader's
// adversarial tests (Tests/vm_ext_plugin/) cover, and the loader can only
// make that clean if the entry point itself behaves.
typedef int (*PscalExtRegisterFn)(const PscalExtHostApi *host, uint32_t host_abi);
#define PSCAL_EXT_ENTRY_POINT_NAME "pscal_ext_register"

#ifdef __cplusplus
}
#endif

#endif // PSCAL_EXT_API_H
