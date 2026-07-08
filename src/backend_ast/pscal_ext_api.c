// src/backend_ast/pscal_ext_api.c
//
// VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1): the host-side implementation
// of the PscalExtHostApi vtable declared in pscal_ext_api.h, plus the
// generic handle-table helper. This file is the ONLY place that is allowed
// to sit on both sides of the ABI boundary -- it includes the real
// pscal-core internals (core/utils.h, backend_ast/builtin.h) to implement
// each vtable entry as a thin, type-checked wrapper, so a plugin including
// only pscal_ext_api.h never needs to.
#include "backend_ast/pscal_ext_api.h"

#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "ext_builtins/registry.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Thin wrappers. PscalExtBuiltinFn and VmBuiltinFn are structurally
// identical (same Value/VM_s* shape, duplicated across the ABI boundary on
// purpose -- see pscal_ext_api.h's header comment), so a plugin's handler
// can be handed straight to registerVmBuiltin via a cast.
static void pscalExtRegisterBuiltin(const char *vm_name, PscalExtBuiltinFn handler,
                                     PscalExtBuiltinKind kind, const char *display_name,
                                     PscalExtEffectMask effect_mask) {
    BuiltinRoutineType type = (kind == PSCAL_EXT_BUILTIN_FUNCTION)
                                   ? BUILTIN_TYPE_FUNCTION
                                   : (kind == PSCAL_EXT_BUILTIN_PROCEDURE ? BUILTIN_TYPE_PROCEDURE
                                                                          : BUILTIN_TYPE_NONE);
    registerVmBuiltin(vm_name, (VmBuiltinFn)handler, type, display_name, (EffectMask)effect_mask);
}

static void pscalExtRegisterCategory(const char *name) {
    extBuiltinRegisterCategory(name);
}

static void pscalExtRegisterGroup(const char *category, const char *group) {
    extBuiltinRegisterGroup(category, group);
}

static void pscalExtRegisterFunctionEntry(const char *category, const char *group, const char *func) {
    extBuiltinRegisterFunction(category, group, func);
}

static void pscalExtRuntimeError(struct VM_s *vm, const char *format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    runtimeError((VM *)vm, "%s", buf);
}

static Value pscalExtMakeInt(long long val) { return makeInt(val); }
static Value pscalExtMakeInt64(long long val) { return makeInt64(val); }
static Value pscalExtMakeDouble(double val) { return makeDouble(val); }
static Value pscalExtMakeString(const char *val) { return makeString(val); }
static Value pscalExtMakeStringLen(const char *val, size_t len) { return makeStringLen(val, len); }
static Value pscalExtMakeBoolean(int b) { return makeBoolean(b); }
static Value pscalExtMakeNil(void) { return makeNil(); }

static long long pscalExtAsInt64(Value v) { return asI64(v); }
static double pscalExtAsDouble(Value v) { return (double)asLd(v); }
static int pscalExtAsBool(Value v) { return AS_BOOLEAN(v) ? 1 : 0; }
static const char *pscalExtAsCString(Value v) { return AS_STRING(v); }

static bool pscalExtIsStringType(VarType t) { return isPascalStringType(t); }
static bool pscalExtIsIntlikeType(VarType t) { return isIntlikeType(t); }
static bool pscalExtIsRealType(VarType t) { return isRealType(t); }

// ---------------------------------------------------------------------------
// Generic handle table: a realloc-growable, mutex-guarded array of tagged
// slots. Structurally the same shape sqlite_builtins.c/yyjson_builtins.c
// each hand-roll privately (see pscal_ext_api.h's design comment) -- kept
// as a separate implementation here rather than refactoring either in-tree
// file onto it, since §7.1 requires those to stay untouched.
typedef struct {
    int kind; // PSCAL_EXT_HANDLE_KIND_UNUSED (0) means free.
    void *payload;
} PscalExtHandleSlot;

struct PscalExtHandleTable {
    PscalExtHandleSlot *slots;
    size_t capacity;
    pthread_mutex_t mutex;
};

static PscalExtHandleTable *pscalExtHandleTableCreate(void) {
    PscalExtHandleTable *table = (PscalExtHandleTable *)calloc(1, sizeof(PscalExtHandleTable));
    if (!table) {
        return NULL;
    }
    pthread_mutex_init(&table->mutex, NULL);
    return table;
}

static void pscalExtHandleTableDestroy(PscalExtHandleTable *table) {
    if (!table) {
        return;
    }
    pthread_mutex_destroy(&table->mutex);
    free(table->slots);
    free(table);
}

static int pscalExtHandleAlloc(PscalExtHandleTable *table, int kind, void *payload) {
    if (!table || kind == PSCAL_EXT_HANDLE_KIND_UNUSED) {
        return -1;
    }
    pthread_mutex_lock(&table->mutex);
    size_t slot = table->capacity;
    for (size_t i = 0; i < table->capacity; ++i) {
        if (table->slots[i].kind == PSCAL_EXT_HANDLE_KIND_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot == table->capacity) {
        size_t new_capacity = table->capacity ? table->capacity * 2 : 16;
        PscalExtHandleSlot *new_slots = (PscalExtHandleSlot *)realloc(
            table->slots, new_capacity * sizeof(PscalExtHandleSlot));
        if (!new_slots) {
            pthread_mutex_unlock(&table->mutex);
            return -1;
        }
        for (size_t i = table->capacity; i < new_capacity; ++i) {
            new_slots[i].kind = PSCAL_EXT_HANDLE_KIND_UNUSED;
            new_slots[i].payload = NULL;
        }
        table->slots = new_slots;
        table->capacity = new_capacity;
    }
    table->slots[slot].kind = kind;
    table->slots[slot].payload = payload;
    pthread_mutex_unlock(&table->mutex);
    return (int)slot;
}

static bool pscalExtHandleLookup(PscalExtHandleTable *table, int handle, int kind, void **out_payload) {
    if (!table || handle < 0) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&table->mutex);
    size_t idx = (size_t)handle;
    if (idx < table->capacity && table->slots[idx].kind == kind && kind != PSCAL_EXT_HANDLE_KIND_UNUSED) {
        if (out_payload) {
            *out_payload = table->slots[idx].payload;
        }
        found = true;
    }
    pthread_mutex_unlock(&table->mutex);
    return found;
}

static void *pscalExtHandleFree(PscalExtHandleTable *table, int handle) {
    if (!table || handle < 0) {
        return NULL;
    }
    void *payload = NULL;
    pthread_mutex_lock(&table->mutex);
    size_t idx = (size_t)handle;
    if (idx < table->capacity) {
        payload = table->slots[idx].payload;
        table->slots[idx].kind = PSCAL_EXT_HANDLE_KIND_UNUSED;
        table->slots[idx].payload = NULL;
    }
    pthread_mutex_unlock(&table->mutex);
    return payload;
}

// ---------------------------------------------------------------------------
// The one host API instance, built once and handed out by address to every
// loaded plugin. Never mutated after this initializer runs, so no locking
// is needed to read it.
const PscalExtHostApi *pscalExtGetHostApi(void) {
    static const PscalExtHostApi api = {
        .register_builtin = pscalExtRegisterBuiltin,
        .register_category = pscalExtRegisterCategory,
        .register_group = pscalExtRegisterGroup,
        .register_function_entry = pscalExtRegisterFunctionEntry,
        .runtime_error = pscalExtRuntimeError,
        .make_int = pscalExtMakeInt,
        .make_int64 = pscalExtMakeInt64,
        .make_double = pscalExtMakeDouble,
        .make_string = pscalExtMakeString,
        .make_string_len = pscalExtMakeStringLen,
        .make_boolean = pscalExtMakeBoolean,
        .make_nil = pscalExtMakeNil,
        .as_int64 = pscalExtAsInt64,
        .as_double = pscalExtAsDouble,
        .as_bool = pscalExtAsBool,
        .as_cstring = pscalExtAsCString,
        .is_string_type = pscalExtIsStringType,
        .is_intlike_type = pscalExtIsIntlikeType,
        .is_real_type = pscalExtIsRealType,
        .handle_table_create = pscalExtHandleTableCreate,
        .handle_table_destroy = pscalExtHandleTableDestroy,
        .handle_alloc = pscalExtHandleAlloc,
        .handle_lookup = pscalExtHandleLookup,
        .handle_free = pscalExtHandleFree,
    };
    return &api;
}
