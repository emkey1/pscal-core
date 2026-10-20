// src/vm/vm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <stdbool.h> // For bool, true, false
#include <ctype.h>
#include <pthread.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "vm/vm.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "core/types.h"
#include "core/obj_header.h" // VM 2.0 Phase 4a: pointer-width startup canary
#include "core/utils.h"    // For runtimeError, printValueToStream, makeNil, freeValue, Type helper macros
#include "symbol/symbol.h" // For HashTable, createHashTable, hashTableLookup, hashTableInsert
#include "core/globals.h"
#include "common/frontend_kind.h"
#include "common/runtime_tty.h"
#include "backend_ast/audio.h"
#include "ast/ast.h"
#include "vm/string_sentinels.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h> // For the operand stack's mmap/mprotect-backed growable storage (VM 2.0 Phase 3)
#include "backend_ast/builtin.h"
#include "backend_ast/runtime_signal_flags.h"
#include "vm/vm_fx_policy.h"
#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

/* ---- Deep array equality ---------------------------------------------------
 * The EQUAL/NOT_EQUAL opcode used to reject two array operands outright
 * ("Operands not comparable"), which made `xs == ys` a runtime error in every
 * front end. Two arrays are equal when they have the same rank and bounds and
 * every element pair is equal under the same rules the scalar comparison
 * applies; nested arrays recurse. Return values: 1 equal, 0 not equal, -1 when
 * an element pair has no defined equality (the caller then reports exactly what
 * it would for those scalars on their own). */
int calculateArrayTotalSize(const Value* array_val);
typedef struct PscalEqMemo PscalEqMemo;
static int pscalValuesDeepEqualD(const Value *a, const Value *b, int depth, PscalEqMemo *memo);

/* ---- Aether record equality (D24) -------------------------------------------
 * Under Aether, `==` / `<>` on two records (and tuples, which are records)
 * compares them field by field, recursing into nested records and arrays; the
 * identity comparison is the `same(a, b)` builtin. Records are pointers to a
 * TYPE_RECORD value, so this replaces the pointer-identity compare for two
 * non-nil record pointers. The front end rejects `==` on a self-referential
 * record type (REC-001), but not one that reaches itself through a map, so the
 * graph compared here can share substructure and even be cyclic. Past a small
 * depth, every pair of records being compared is remembered and a pair met
 * again counts as equal: it is either already proven equal or still being
 * compared further up (a cycle, which is equal if nothing else differs). That
 * keeps the walk linear in the size of the graph -- without it a DAG whose
 * nodes are each reached twice took time exponential in its depth -- and lets
 * cycles compare instead of hitting the depth cap. Results on acyclic data are
 * unchanged, since any difference ends the whole comparison the moment it is
 * found. The depth cap remains a backstop that reports "no defined equality"
 * (-1). Other front ends keep pointer identity. */
#define PSCAL_DEEP_EQUAL_MAX_DEPTH 512
#define PSCAL_DEEP_EQUAL_MEMO_DEPTH 8

typedef struct {
    const void *a;
    const void *b;
} PscalEqPair;

struct PscalEqMemo {
    PscalEqPair *slots; /* open addressing; a == NULL marks an empty slot */
    size_t cap;         /* a power of two, or 0 before first use */
    size_t count;
};

static size_t pscalEqPairHash(const void *a, const void *b) {
    uint64_t h = (uint64_t)(uintptr_t)a * 0x9E3779B97F4A7C15ULL;
    h ^= (uint64_t)(uintptr_t)b * 0xC2B2AE3D27D4EB4FULL;
    h ^= h >> 29;
    return (size_t)h;
}

/* Records the record pair (a, b); true if it was already recorded. If the
 * table cannot grow the pair simply goes unrecorded (the walk is then only
 * slower, and a cycle falls back to the depth cap). */
static bool pscalEqMemoSeen(PscalEqMemo *m, const void *a, const void *b) {
    if (m->count + 1 > m->cap / 2) {
        size_t new_cap = m->cap ? m->cap * 2 : 64;
        PscalEqPair *grown = (PscalEqPair *)calloc(new_cap, sizeof(PscalEqPair));
        if (!grown) {
            return false;
        }
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->slots[i].a) continue;
            size_t j = pscalEqPairHash(m->slots[i].a, m->slots[i].b) & (new_cap - 1);
            while (grown[j].a) j = (j + 1) & (new_cap - 1);
            grown[j] = m->slots[i];
        }
        free(m->slots);
        m->slots = grown;
        m->cap = new_cap;
    }
    size_t j = pscalEqPairHash(a, b) & (m->cap - 1);
    while (m->slots[j].a) {
        if (m->slots[j].a == a && m->slots[j].b == b) return true;
        j = (j + 1) & (m->cap - 1);
    }
    m->slots[j].a = a;
    m->slots[j].b = b;
    m->count++;
    return false;
}

static int pscalRecordFieldsDeepEqual(const FieldValue *fa, const FieldValue *fb, int depth,
                                      PscalEqMemo *memo) {
    if (depth > PSCAL_DEEP_EQUAL_MEMO_DEPTH && fa && fb && pscalEqMemoSeen(memo, fa, fb)) {
        return 1;
    }
    while (fa && fb) {
        /* Compiler-owned fields (the hidden __vtable) are not data. */
        bool hiddenA = fa->name && fa->name[0] == '_' && fa->name[1] == '_';
        bool hiddenB = fb->name && fb->name[0] == '_' && fb->name[1] == '_';
        if (hiddenA != hiddenB) return 0;
        if (!hiddenA) {
            if (fa->name && fb->name && strcasecmp(fa->name, fb->name) != 0) return 0;
            const Value *va = fieldValueStorageConst(fa);
            const Value *vb = fieldValueStorageConst(fb);
            if (!va || !vb) {
                if (va != vb) return 0;
            } else {
                int r = pscalValuesDeepEqualD(va, vb, depth, memo);
                if (r != 1) return r;
            }
        }
        fa = fa->next;
        fb = fb->next;
    }
    return (fa == NULL && fb == NULL) ? 1 : 0;
}

/* Two record pointers under Aether: identical, both nil, or field-wise equal. */
static int pscalRecordPointersDeepEqualD(const Value *a, const Value *b, int depth, PscalEqMemo *memo) {
    Value *pa = AS_POINTER(*a);
    Value *pb = AS_POINTER(*b);
    if (pa == pb) return 1;
    if (!pa || !pb) return 0;
    if (depth > PSCAL_DEEP_EQUAL_MAX_DEPTH) return -1;
    if (VALUE_TYPE(*pa) == TYPE_RECORD && VALUE_TYPE(*pb) == TYPE_RECORD)
        return pscalRecordFieldsDeepEqual(AS_RECORD(*pa), AS_RECORD(*pb), depth + 1, memo);
    return 0;
}

static const char *pscalTextForCompare(const Value *v, char *buf /* >= 5 bytes */) {
    if (IS_STRING(*v)) {
        return AS_STRING(*v) ? AS_STRING(*v) : "";
    }
    if (VALUE_TYPE(*v) == TYPE_WIDECHAR) {
        encodeUtf8Codepoint((uint32_t)AS_CHAR(*v), buf);
        return buf;
    }
    buf[0] = (char)AS_CHAR(*v);
    buf[1] = '\0';
    return buf;
}

static int pscalArraysDeepEqualD(const Value *a, const Value *b, int depth, PscalEqMemo *memo) {
    ArrayObj *pa = PSCAL_VALUE_PTR(*a, ArrayObj);
    ArrayObj *pb = PSCAL_VALUE_PTR(*b, ArrayObj);
    if (pa == pb) return 1;
    if (!pa || !pb) return 0;
    if (pa->dimensions != pb->dimensions) return 0;
    if (pa->dimensions > 0 && (!pa->lower_bounds || !pb->lower_bounds ||
                               !pa->upper_bounds || !pb->upper_bounds)) {
        return 0;
    }
    for (int d = 0; d < pa->dimensions; d++) {
        if (pa->lower_bounds[d] != pb->lower_bounds[d] ||
            pa->upper_bounds[d] != pb->upper_bounds[d]) {
            return 0;
        }
    }
    int total = calculateArrayTotalSize(a);
    if (total != calculateArrayTotalSize(b)) return 0;
    if (total <= 0) return 1;
    if (pa->is_packed && pb->is_packed) {
        if (!pa->raw || !pb->raw) return (pa->raw == pb->raw) ? 1 : 0;
        return memcmp(pa->raw, pb->raw, (size_t)total) == 0 ? 1 : 0;
    }
    if (pa->is_packed || pb->is_packed) {
        /* One packed byte buffer against one boxed buffer: compare as integers. */
        const ArrayObj *packed = pa->is_packed ? pa : pb;
        const ArrayObj *boxed = pa->is_packed ? pb : pa;
        if (!packed->raw || !boxed->elements) return 0;
        for (int i = 0; i < total; i++) {
            const Value *e = &boxed->elements[i];
            if (!IS_INTLIKE(*e) && !IS_CHAR(*e) && !IS_BOOLEAN(*e)) return -1;
            if ((long long)packed->raw[i] != asI64(*e)) return 0;
        }
        return 1;
    }
    if (!pa->elements || !pb->elements) return (pa->elements == pb->elements) ? 1 : 0;
    for (int i = 0; i < total; i++) {
        int r = pscalValuesDeepEqualD(&pa->elements[i], &pb->elements[i], depth + 1, memo);
        if (r != 1) return r;
    }
    return 1;
}

static int pscalArraysDeepEqual(const Value *a, const Value *b) {
    PscalEqMemo memo = {0};
    int r = pscalArraysDeepEqualD(a, b, 0, &memo);
    free(memo.slots);
    return r;
}

/* Aether maps (D68, D24): the same key set and equal values, order ignored;
 * values compare by the same structural rules (records field by field). */
static int pscalMapsDeepEqualD(const Value *a, const Value *b, int depth, PscalEqMemo *memo) {
    MapObj *ma = PSCAL_VALUE_PTR(*a, MapObj);
    MapObj *mb = PSCAL_VALUE_PTR(*b, MapObj);
    if (ma == mb) return 1;
    int na = ma ? ma->live_count : 0, nb = mb ? mb->live_count : 0;
    if (na != nb) return 0;
    if (na == 0) return 1;
    if (depth > PSCAL_DEEP_EQUAL_MAX_DEPTH) return -1;
    for (int i = 0; i < ma->entry_count; i++) {
        const MapEntry *e = &ma->entries[i];
        if (!e->live) continue;
        Value *other = pscalMapFind(mb, e->key, e->key_len);
        if (!other) return 0;
        int r = pscalValuesDeepEqualD(e->value, other, depth + 1, memo);
        if (r != 1) return r;
    }
    return 1;
}

static int pscalMapsDeepEqual(const Value *a, const Value *b) {
    PscalEqMemo memo = {0};
    int r = pscalMapsDeepEqualD(a, b, 0, &memo);
    free(memo.slots);
    return r;
}

static int pscalRecordPointersDeepEqual(const Value *a, const Value *b) {
    PscalEqMemo memo = {0};
    int r = pscalRecordPointersDeepEqualD(a, b, 0, &memo);
    free(memo.slots);
    return r;
}

static int pscalValuesDeepEqualD(const Value *a, const Value *b, int depth, PscalEqMemo *memo) {
    VarType ta = VALUE_TYPE(*a), tb = VALUE_TYPE(*b);
    if (depth > PSCAL_DEEP_EQUAL_MAX_DEPTH) return -1;
    if (ta == TYPE_ARRAY && tb == TYPE_ARRAY) return pscalArraysDeepEqualD(a, b, depth, memo);
    if (ta == TYPE_MAP && tb == TYPE_MAP) return pscalMapsDeepEqualD(a, b, depth, memo);
    if (ta == TYPE_NIL && tb == TYPE_NIL) return 1;
    if (ta == TYPE_NIL || tb == TYPE_NIL) {
        const Value *other = (ta == TYPE_NIL) ? b : a;
        if (VALUE_TYPE(*other) == TYPE_POINTER) return AS_POINTER(*other) == NULL ? 1 : 0;
        return 0;
    }
    if (IS_NUMERIC(*a) && IS_NUMERIC(*b)) {
        if (isRealType(ta) || isRealType(tb)) return asLd(*a) == asLd(*b) ? 1 : 0;
        return asI64(*a) == asI64(*b) ? 1 : 0;
    }
    if (IS_BOOLEAN(*a) && IS_BOOLEAN(*b)) return AS_BOOLEAN(*a) == AS_BOOLEAN(*b) ? 1 : 0;
    if ((IS_STRING(*a) || IS_CHAR(*a)) && (IS_STRING(*b) || IS_CHAR(*b))) {
        char abuf[5] = {0}, bbuf[5] = {0};
        return strcmp(pscalTextForCompare(a, abuf), pscalTextForCompare(b, bbuf)) == 0 ? 1 : 0;
    }
    if (ta == TYPE_POINTER && tb == TYPE_POINTER) {
        if (frontendIsAether()) return pscalRecordPointersDeepEqualD(a, b, depth, memo);
        return AS_POINTER(*a) == AS_POINTER(*b) ? 1 : 0;
    }
    if (ta == TYPE_RECORD && tb == TYPE_RECORD && frontendIsAether())
        return pscalRecordFieldsDeepEqual(AS_RECORD(*a), AS_RECORD(*b), depth + 1, memo);
    if (ta == TYPE_MEMORYSTREAM && tb == TYPE_MEMORYSTREAM) return AS_MSTREAM(*a) == AS_MSTREAM(*b) ? 1 : 0;
    return -1;
}

#if defined(PSCAL_TARGET_IOS)
#include <os/log.h>
#endif
#if defined(PSCAL_TARGET_IOS)
#include "runtime/vproc/vproc.h"
#endif

static bool vmHandleGlobalInterrupt(VM* vm);
static bool vmConsumeSuspendRequest(VM* vm);
static bool vmIsRootExecutor(const VM* vm);

static char* vmStringifyValueForConcat(const Value* value) {
    if (!value) {
        return strdup("");
    }

    FILE* stream = NULL;
    char* buffer = NULL;
    size_t size = 0;

#if defined(__APPLE__) || defined(_GNU_SOURCE) || defined(__linux__)
    stream = open_memstream(&buffer, &size);
#endif
    if (!stream) {
        return NULL;
    }

    printValueToStream(*value, stream);
    if (fclose(stream) != 0) {
        free(buffer);
        return NULL;
    }

    if (!buffer) {
        return strdup("");
    }
    return buffer;
}

#define VM_PROC_REGISTRY_CAPACITY 1024

static pthread_mutex_t gVmProcRegistryLock = PTHREAD_MUTEX_INITIALIZER;
// Not static: VM 2.0 Phase 4j's valueEnsureUnique() (core/utils.c) shares
// this same mutex -- see the extern declaration and comment in
// core/utils.h.
pthread_mutex_t value_cell_mutex = PTHREAD_MUTEX_INITIALIZER;
static VM* gVmProcRegistry[VM_PROC_REGISTRY_CAPACITY];
static size_t gVmProcRegistryCount = 0;

static size_t vmCountTableSymbols(const HashTable* table) {
    if (!table) {
        return 0;
    }
    size_t count = 0;
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (const Symbol* sym = table->buckets[i]; sym; sym = sym->next) {
            count++;
        }
    }
    return count;
}

static void vmProcRegister(VM* vm) {
    if (!vm) {
        return;
    }
    pthread_mutex_lock(&gVmProcRegistryLock);
    for (size_t i = 0; i < gVmProcRegistryCount; ++i) {
        if (gVmProcRegistry[i] == vm) {
            pthread_mutex_unlock(&gVmProcRegistryLock);
            return;
        }
    }
    if (gVmProcRegistryCount < VM_PROC_REGISTRY_CAPACITY) {
        gVmProcRegistry[gVmProcRegistryCount++] = vm;
    }
    pthread_mutex_unlock(&gVmProcRegistryLock);
}

static void vmProcUnregister(VM* vm) {
    if (!vm) {
        return;
    }
    pthread_mutex_lock(&gVmProcRegistryLock);
    for (size_t i = 0; i < gVmProcRegistryCount; ++i) {
        if (gVmProcRegistry[i] == vm) {
            gVmProcRegistry[i] = gVmProcRegistry[gVmProcRegistryCount - 1];
            gVmProcRegistry[gVmProcRegistryCount - 1] = NULL;
            gVmProcRegistryCount--;
            break;
        }
    }
    pthread_mutex_unlock(&gVmProcRegistryLock);
}

static void vmProcFillSnapshot(const VM* vm, VMProcSnapshot* out) {
    if (!vm || !out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->vm_address = (uintptr_t)vm;
    out->thread_owner_address = (uintptr_t)(vm->threadOwner ? vm->threadOwner : vm);
    out->frontend_context_address = (uintptr_t)vm->frontendContext;
    out->chunk_address = (uintptr_t)vm->chunk;
    out->globals_address = (uintptr_t)vm->vmGlobalSymbols;
    out->const_globals_address = (uintptr_t)vm->vmConstGlobalSymbols;
    out->procedures_address = (uintptr_t)vm->procedureTable;
    out->mutex_owner_address = (uintptr_t)(vm->mutexOwner ? vm->mutexOwner : vm);
    out->thread_id = vm->threadId;
    out->thread_count = vm->threadCount;
    out->worker_count = vm->workerCount;
    out->available_workers = vm->availableWorkers;
    out->mutex_count = vm->mutexCount;
    out->frame_count = vm->frameCount;
    out->trace_head_instructions = vm->trace_head_instructions;
    out->trace_executed = vm->trace_executed;
    out->chunk_bytecode_count = vm->chunk ? vm->chunk->count : 0;
    out->stack_depth = (vm->stackTop > vm->stack) ? (size_t)(vm->stackTop - vm->stack) : 0;
    out->global_symbol_count = vmCountTableSymbols(vm->vmGlobalSymbols);
    out->const_symbol_count = vmCountTableSymbols(vm->vmConstGlobalSymbols);
    out->procedure_symbol_count = vmCountTableSymbols(vm->procedureTable);
    out->is_root_vm = (vm->threadOwner == NULL || vm->threadOwner == vm);
    out->has_job_queue = (vm->jobQueue != NULL);
    out->shell_indexing = vm->zeroBasedIndexing;
    out->exit_requested = vm->exit_requested;
    out->abort_requested = vm->abort_requested;
    out->suspend_unwind_requested = vm->suspend_unwind_requested;
}

#if defined(PSCAL_TARGET_IOS)
static uint64_t vmNowMonoNs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    return 0;
}

static bool vmIosDebugEnabled(void) {
    const char* env = getenv("PSCALI_VM_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        return false;
    }
    return true;
}

static bool vmIosMutexDebugEnabled(void) {
    const char* env = getenv("PSCALI_VM_MUTEX_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        return false;
    }
    return true;
}

static bool vmRuntimeSignalAppliesToCurrentVproc(VM* vm);

static bool vmRuntimeScopeFilterEnabled(void) {
    const char* env = getenv("PSCALI_VM_SCOPE_FILTER");
    if (!env || env[0] == '\0') {
        return false;
    }
    return strcmp(env, "0") != 0;
}

static void vmIosDebugLogf(const char* format, ...) {
    if (!vmIosDebugEnabled() || !format) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    size_t len = strlen(buf);
    if (len == 0) {
        return;
    }
    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
#if defined(__APPLE__)
#if defined(PSCAL_TARGET_IOS)
    os_log_with_type(OS_LOG_DEFAULT, OS_LOG_TYPE_DEFAULT, "%{public}s", buf);
#endif
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    static void (*debug_log)(const char *message) = NULL;
    static int debug_log_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
    }
    if (!debug_log_checked) {
        debug_log_checked = 1;
        debug_log = (void (*)(const char *))dlsym(RTLD_DEFAULT, "pscalRuntimeDebugLog");
    }
    if (debug_log) {
        debug_log(buf);
    }
#endif
#if !defined(PSCAL_TARGET_IOS)
    fprintf(stderr, "%s\n", buf);
#endif
}

static bool vmRuntimeSignalAppliesCached(VM* vm, const char* tag) {
    if (!vmRuntimeScopeFilterEnabled()) {
        return true;
    }
    static _Thread_local uint64_t tlsLastScopeSampleNs = 0;
    static _Thread_local bool tlsLastScopeAllowed = true;
    static _Thread_local uint64_t tlsLastScopeLogNs = 0;

    uint64_t scope_start_ns = vmNowMonoNs();
    if (scope_start_ns == 0 ||
        tlsLastScopeSampleNs == 0 ||
        scope_start_ns - tlsLastScopeSampleNs >= 100000000ull) {
        tlsLastScopeAllowed = vmRuntimeSignalAppliesToCurrentVproc(vm);
        tlsLastScopeSampleNs = vmNowMonoNs();
        if (tlsLastScopeSampleNs == 0) {
            tlsLastScopeSampleNs = scope_start_ns;
        }
    }

    uint64_t scope_end_ns = vmNowMonoNs();
    if (scope_start_ns > 0 && scope_end_ns >= scope_start_ns) {
        uint64_t scope_elapsed_ns = scope_end_ns - scope_start_ns;
        if (scope_elapsed_ns >= 5000000ull &&
            (tlsLastScopeLogNs == 0 || scope_end_ns - tlsLastScopeLogNs >= 250000000ull)) {
            vmIosDebugLogf("[vm-int] scope-check slow tag=%s vm=%p elapsed_ms=%" PRIu64 " allow=%d",
                           tag ? tag : "?",
                           (void*)vm,
                           scope_elapsed_ns / 1000000ull,
                           tlsLastScopeAllowed ? 1 : 0);
            tlsLastScopeLogNs = scope_end_ns;
        }
    }
    return tlsLastScopeAllowed;
}

static bool vmRuntimeSignalAppliesToCurrentVproc(VM* vm) {
    int fg_pgid = -1;
    if (!vprocGetShellJobControlState(NULL, NULL, NULL, &fg_pgid)) {
        return true;
    }
    if (fg_pgid <= 0) {
        return true;
    }

    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        pid = vprocGetShellSelfPid();
    }
    if (pid <= 0) {
        return true;
    }

    int pgid = vprocGetPgid(pid);
    if (pgid <= 0) {
        return true;
    }

    if (pgid == fg_pgid) {
        return true;
    }

    /* Always honor explicit VM-local abort/exit flags. */
    if (vm && (vm->abort_requested || vm->exit_requested)) {
        return true;
    }
    return false;
}

static bool vmShouldUseCooperativeSuspendForCurrentVproc(void) {
    if (vprocIsShellSelfThread()) {
        return true;
    }
    int pid = (int)vprocGetPidShim();
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0) {
        pid = shell_pid;
    }
    if (pid <= 0) {
        return true;
    }
    if (shell_pid > 0 && pid == shell_pid) {
        return true;
    }
    return vprocGetStopUnsupported(pid);
}

static bool vmRequestHardSuspendCurrentVproc(void) {
    int pid = (int)vprocGetPidShim();
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0) {
        pid = shell_pid;
    }
    int pgid = (pid > 0) ? vprocGetPgid(pid) : -1;

    if (vprocRequestControlSignal(SIGTSTP)) {
        return true;
    }
    if (pgid > 0 && vprocKillShim(-pgid, SIGTSTP) == 0) {
        return true;
    }
    if (pid > 0 && vprocKillShim(pid, SIGTSTP) == 0) {
        return true;
    }
    return false;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define VM_USE_COMPUTED_GOTO 1
#else
#define VM_USE_COMPUTED_GOTO 0
#endif

// The opcode list is generated from compiler/opcodes.def (the single source
// of truth for the opcode page).  kOpcodeNames, the computed-goto dispatch
// table and its labels below all include that file.

static unsigned long long gVmOpcodeCounts[OPCODE_COUNT];
static bool gVmOpcodeProfileEnabled = false;
static FILE *gVmOpcodeProfileStream = NULL;
static pthread_once_t gVmOpcodeProfileOnce = PTHREAD_ONCE_INIT;
static bool gVmOpcodeProfileHeaderPrinted = false;
static bool gVmOpcodeProfileStreamOwned = false;
static bool gVmBuiltinProfileEnabled = false;
static unsigned long long gVmBuiltinCallCounts[UINT16_MAX + 1];
typedef struct {
    char *name;
    unsigned long long count;
} VmShellBuiltinProfileEntry;

static VmShellBuiltinProfileEntry *gVmShellBuiltinProfiles = NULL;
static size_t gVmShellBuiltinProfileCount = 0;
static size_t gVmShellBuiltinProfileCapacity = 0;

static void vmOpcodeProfileInitOnce(void);
static void vmOpcodeProfileRecord(uint8_t opcode);
static void vmOpcodeProfileAtExit(void);

static bool s_vmVerboseErrors = false;

static const char *const kOpcodeNames[OPCODE_COUNT] = {
#define OP(name, value, operands, stack_in, stack_out) [value] = #name,
#include "compiler/opcodes.def"

#undef OP
};

static void vmOpcodeProfileInitOnce(void) {
    const char *spec = getenv("EXSH_PROFILE_OPCODES");
    if (!spec || *spec == '\0') {
        gVmOpcodeProfileEnabled = false;
        gVmBuiltinProfileEnabled = false;
        return;
    }

    gVmOpcodeProfileEnabled = true;
    gVmBuiltinProfileEnabled = true;
    if (strcmp(spec, "stderr") == 0 || strcmp(spec, "2") == 0 || strcmp(spec, "true") == 0) {
        gVmOpcodeProfileStream = stderr;
    } else if (strcmp(spec, "stdout") == 0) {
        gVmOpcodeProfileStream = stdout;
    } else if (strcmp(spec, "1") == 0) {
        gVmOpcodeProfileStream = stderr;
    } else {
        FILE *fp = fopen(spec, "a");
        if (fp) {
            gVmOpcodeProfileStream = fp;
            gVmOpcodeProfileStreamOwned = true;
            atexit(vmOpcodeProfileAtExit);
        } else {
            gVmOpcodeProfileStream = stderr;
        }
    }

    if (!gVmOpcodeProfileStream) {
        gVmOpcodeProfileStream = stderr;
    }
}

static void vmOpcodeProfileAtExit(void) {
    if (gVmOpcodeProfileStreamOwned && gVmOpcodeProfileStream &&
        gVmOpcodeProfileStream != stdout && gVmOpcodeProfileStream != stderr) {
        fclose(gVmOpcodeProfileStream);
        gVmOpcodeProfileStream = NULL;
        gVmOpcodeProfileStreamOwned = false;
    }
}

static void vmFreeShellBuiltinProfiles(void) {
    for (size_t i = 0; i < gVmShellBuiltinProfileCount; ++i) {
        free(gVmShellBuiltinProfiles[i].name);
        gVmShellBuiltinProfiles[i].name = NULL;
        gVmShellBuiltinProfiles[i].count = 0;
    }
    free(gVmShellBuiltinProfiles);
    gVmShellBuiltinProfiles = NULL;
    gVmShellBuiltinProfileCount = 0;
    gVmShellBuiltinProfileCapacity = 0;
}

static void vmOpcodeProfileRecord(uint8_t opcode) {
    if (!gVmOpcodeProfileEnabled || opcode >= OPCODE_COUNT) {
        return;
    }
    gVmOpcodeCounts[opcode]++;
}

void vmOpcodeProfileDump(void) {
    if (!vmOpcodeProfileIsEnabled() || !gVmOpcodeProfileStream) {
        return;
    }

    uint64_t total = 0;
    for (size_t i = 0; i < OPCODE_COUNT; ++i) {
        total += gVmOpcodeCounts[i];
    }
    if (total == 0) {
        return;
    }

    FILE *out = gVmOpcodeProfileStream;
    if (!gVmOpcodeProfileHeaderPrinted) {
        fprintf(out, "== exsh opcode profile ==\n");
        gVmOpcodeProfileHeaderPrinted = true;
    }
    for (size_t i = 0; i < OPCODE_COUNT; ++i) {
        if (gVmOpcodeCounts[i] == 0) {
            continue;
        }
        fprintf(out, "%-24s %" PRIu64 "\n", kOpcodeNames[i], (uint64_t)gVmOpcodeCounts[i]);
    }
    fprintf(out, "%-24s %" PRIu64 "\n\n", "TOTAL", total);
    fflush(out);
    memset(gVmOpcodeCounts, 0, sizeof(gVmOpcodeCounts));

    if (gVmBuiltinProfileEnabled) {
        bool printed_header = false;
        for (size_t i = 0; i < sizeof(gVmBuiltinCallCounts) / sizeof(gVmBuiltinCallCounts[0]); ++i) {
            uint64_t count = gVmBuiltinCallCounts[i];
            if (count == 0) {
                continue;
            }
            if (!printed_header) {
                fprintf(out, "== exsh builtin profile ==\n");
                printed_header = true;
            }
            const char *name = getVmBuiltinNameById((int)i);
            if (!name || !*name) {
                fprintf(out, "builtin#%zu           %" PRIu64 "\n", i, count);
            } else {
                fprintf(out, "%-24s %" PRIu64 "\n", name, count);
            }
        }
        if (printed_header) {
            fprintf(out, "\n");
            fflush(out);
        }
        memset(gVmBuiltinCallCounts, 0, sizeof(gVmBuiltinCallCounts));

        if (gVmShellBuiltinProfileCount > 0) {
            fprintf(out, "== exsh shell builtin profile ==\n");
            for (size_t i = 0; i < gVmShellBuiltinProfileCount; ++i) {
                VmShellBuiltinProfileEntry *entry = &gVmShellBuiltinProfiles[i];
                if (!entry->name) {
                    continue;
                }
                fprintf(out, "%-24s %" PRIu64 "\n", entry->name, (uint64_t)entry->count);
                entry->count = 0;
            }
            fprintf(out, "\n");
            fflush(out);
        }
    }
}

void vmSetVerboseErrors(bool enabled) {
    s_vmVerboseErrors = enabled;
}

// --- VM Helper Functions ---
static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

// --- VM 2.0 Phase 3: growable, never-relocated stack storage ---
//
// Effective ceilings are read once per process (consistent with the
// PSCAL_VM_SKIP_VERIFY convention from the Phase 1e verifier) so the runtime
// and the load-time verifier (bytecode_verify.c) always agree on the same
// bound rather than drifting if only one of them were made configurable.
size_t pscalVmStackCeilingValues(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t v = (size_t)VM_STACK_MAX;
        const char* env = getenv("PSCAL_VM_MAX_STACK_VALUES");
        if (env && *env) {
            char* end = NULL;
            unsigned long long parsed = strtoull(env, &end, 10);
            if (end && *end == '\0' && parsed > 0) v = (size_t)parsed;
        }
        cached = v;
    }
    return cached;
}

size_t pscalVmCallFrameCeiling(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t v = (size_t)VM_CALL_STACK_MAX;
        const char* env = getenv("PSCAL_VM_MAX_CALL_FRAMES");
        if (env && *env) {
            char* end = NULL;
            unsigned long long parsed = strtoull(env, &end, 10);
            if (end && *end == '\0' && parsed > 0) v = (size_t)parsed;
        }
        cached = v;
    }
    return cached;
}

static size_t vmPageRoundUp(size_t nbytes) {
    static size_t page_size = 0;
    if (page_size == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        page_size = (ps > 0) ? (size_t)ps : 4096;
    }
    return ((nbytes + page_size - 1) / page_size) * page_size;
}

// Reserve the operand stack's full virtual address range up front
// (PROT_NONE — costs address space, not physical memory) and commit only a
// small initial prefix (PROT_READ|PROT_WRITE). `vm->stack` is fixed for the
// life of the VM: GET_LOCAL_ADDRESS/GET_GSLOT_ADDRESS and VAR-parameter
// passing push real `Value*` pointers into this region (see manual ch1 §1.2
// and CallFrame.slots), so the base address must never move or be freed
// until freeVM(). Returns false on allocation failure (fatal in initVM()).
static bool vmAllocStackStorage(VM* vm) {
    size_t ceiling = pscalVmStackCeilingValues();
    size_t initial = VM_STACK_INITIAL_VALUES;
    if (initial > ceiling) initial = ceiling;

    size_t reserve_bytes = vmPageRoundUp(ceiling * sizeof(Value));
    void* mapped = mmap(NULL, reserve_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return false;
    }

    size_t commit_bytes = vmPageRoundUp(initial * sizeof(Value));
    if (commit_bytes > reserve_bytes) commit_bytes = reserve_bytes;
    if (commit_bytes > 0 && mprotect(mapped, commit_bytes, PROT_READ | PROT_WRITE) != 0) {
        munmap(mapped, reserve_bytes);
        return false;
    }

    vm->stack = (Value*)mapped;
    vm->stackTop = vm->stack;
    vm->stackMappedBytes = reserve_bytes;
    vm->stackReservedValues = reserve_bytes / sizeof(Value);
    vm->stackCommittedValues = commit_bytes / sizeof(Value);
    return true;
}

static void vmFreeStackStorage(VM* vm) {
    if (vm->stack) {
        munmap(vm->stack, vm->stackMappedBytes);
    }
    vm->stack = NULL;
    vm->stackTop = NULL;
    vm->stackMappedBytes = 0;
    vm->stackReservedValues = 0;
    vm->stackCommittedValues = 0;
}

// Extend the accessible prefix in place (mprotect only — never mmap/munmap
// the base) so `neededValues` Values are usable. Doubles the committed
// region each time to keep amortized growth cost low. Returns false only
// when `neededValues` exceeds the hard ceiling (stackReservedValues) or the
// mprotect() call itself fails — both are reported as a clean stack-overflow
// runtime error by the caller, never a crash.
static bool vmGrowStackStorage(VM* vm, size_t neededValues) {
    if (neededValues <= vm->stackCommittedValues) return true;
    if (neededValues > vm->stackReservedValues) return false;

    size_t newCommitted = vm->stackCommittedValues * 2;
    if (newCommitted < neededValues) newCommitted = neededValues;
    if (newCommitted > vm->stackReservedValues) newCommitted = vm->stackReservedValues;

    size_t oldCommitBytes = vmPageRoundUp(vm->stackCommittedValues * sizeof(Value));
    size_t newCommitBytes = vmPageRoundUp(newCommitted * sizeof(Value));
    if (newCommitBytes > oldCommitBytes) {
        if (mprotect((char*)vm->stack + oldCommitBytes, newCommitBytes - oldCommitBytes,
                     PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
    }
    vm->stackCommittedValues = newCommitted;
    return true;
}

// Grow frames[] via ordinary realloc (see vm.h: no long-lived CallFrame*
// escapes, so relocation on growth is safe). Returns false only when
// frameCount has reached the hard ceiling or the realloc itself fails.
static bool vmGrowFrames(VM* vm) {
    size_t ceiling = pscalVmCallFrameCeiling();
    if ((size_t)vm->frameCount >= ceiling) return false;

    size_t newCap = vm->frameCapacity ? vm->frameCapacity * 2 : VM_CALL_FRAME_INITIAL_CAPACITY;
    if (newCap > ceiling) newCap = ceiling;
    if (newCap <= vm->frameCapacity) return false;

    CallFrame* newFrames = (CallFrame*)realloc(vm->frames, newCap * sizeof(CallFrame));
    if (!newFrames) return false;

    memset(newFrames + vm->frameCapacity, 0, (newCap - vm->frameCapacity) * sizeof(CallFrame));
    vm->frames = newFrames;
    vm->frameCapacity = newCap;
    return true;
}

static inline bool vmEnsureFrameCapacity(VM* vm) {
    if ((size_t)vm->frameCount < vm->frameCapacity) return true;
    return vmGrowFrames(vm);
}

// Resolve a value to its underlying record by chasing pointer chains.
// Returns NULL if a nil pointer is encountered.  If the original value is
// neither a pointer nor a record, *invalid_type is set to true.
static void push(VM* vm, Value value);
static Value copyValueForStack(const Value* src);

// --- dispose() quarantine (see vmDisposeValueCell in vm.h) ---
// dispose(p) can only nil the copies of p that vmNullifyAliases finds
// (globals and stack slots); one stored in a record field, an array element
// or another heap cell keeps the old address. Keeping the disposed cell as a
// tombstone makes a later p^ through such a copy a clean runtime error. The
// quarantine is bounded: once VM_DISPOSE_QUARANTINE newer cells have been
// disposed, the oldest tombstone is freed for real.
#define VM_DISPOSED_CELL_BITS 0xD15D05EDD15D05EDull
#define VM_DISPOSE_QUARANTINE 4096
static Value* s_disposeQuarantine[VM_DISPOSE_QUARANTINE];
static size_t s_disposeQuarantineNext = 0;
static pthread_mutex_t s_disposeQuarantineMutex = PTHREAD_MUTEX_INITIALIZER;

bool vmValueIsDisposed(const Value* cell) {
    return cell && cell->type == TYPE_VOID && cell->bits == VM_DISPOSED_CELL_BITS;
}

void vmDisposeValueCell(Value* cell) {
    if (!cell) {
        return;
    }
    freeValue(cell);
    cell->type = TYPE_VOID;
    cell->bits = VM_DISPOSED_CELL_BITS;
    pthread_mutex_lock(&s_disposeQuarantineMutex);
    Value* evicted = s_disposeQuarantine[s_disposeQuarantineNext];
    s_disposeQuarantine[s_disposeQuarantineNext] = cell;
    s_disposeQuarantineNext = (s_disposeQuarantineNext + 1) % VM_DISPOSE_QUARANTINE;
    pthread_mutex_unlock(&s_disposeQuarantineMutex);
    free(evicted);
}

// Longest pointer-to-pointer chain resolveRecord follows; anything longer is
// a cycle (q := @q).
#define VM_MAX_POINTER_CHAIN 64

// Stops at a disposed cell (vmValueIsDisposed) or, for a cyclic chain, at the
// pointer where the hop limit ran out; callers see a non-record either way.
static Value* resolveRecord(Value* base, bool* invalid_type) {
    if (invalid_type) *invalid_type = false;
    if (VALUE_TYPE(*base) != TYPE_POINTER && VALUE_TYPE(*base) != TYPE_RECORD) {
        if (invalid_type) *invalid_type = true;
        return NULL;
    }
    Value* current = base;
    int hops = 0;
    while (current && VALUE_TYPE(*current) == TYPE_POINTER && hops++ < VM_MAX_POINTER_CHAIN) {
        current = AS_POINTER(*current);
    }
    return current;
}

// The error for a field access whose base resolved (resolveRecord) to
// something other than a record.
static void vmReportFieldBaseNotRecord(VM* vm, const Value* resolved) {
    if (vmValueIsDisposed(resolved)) {
        runtimeError(vm, "VM Error: Cannot access field through a dangling pointer (its target was disposed).");
    } else if (VALUE_TYPE(*resolved) == TYPE_POINTER) {
        runtimeError(vm, "VM Error: Cannot access field: the pointer chain is cyclic.");
    } else {
        runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
    }
}

static Value* resolveRecordForField(VM* vm, Value* base_val_ptr) {
    bool invalid_type = false;
    Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
    if (invalid_type) {
        runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
        return NULL;
    }
    if (record_struct_ptr == NULL) {
        runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
        return NULL;
    }
    if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
        vmReportFieldBaseNotRecord(vm, record_struct_ptr);
        return NULL;
    }
    return record_struct_ptr;
}

static bool vmResolveStringIndex(VM* vm, long long raw_index, size_t len, size_t* out_offset,
                                 bool allow_length_query, bool* out_length_query);

// The byte a STRING_CHAR_PTR_SENTINEL pointer names. One made by
// makeStringCharPointer re-indexes its pinned StringObj's current buffer, with
// a bounds check: the string may have been reassigned (the pin keeps the old
// one alive) or shortened in place since the address was taken. A raw one
// made elsewhere uses its address. Reports and returns NULL when there is no
// such character any more.
static char* vmStringCharTarget(VM* vm, const Value* pointer, const char* null_message) {
    PointerObj* po = PSCAL_VALUE_PTR(*pointer, PointerObj);
    if (!po || !po->retained_string) {
        char* addr = po ? (char*)po->address : NULL;
        if (!addr) {
            runtimeError(vm, "%s", null_message);
        }
        return addr;
    }
    char* buf = po->retained_string->buffer;
    // memchr finds a terminator at or before string_index iff the string
    // is now too short to hold that character.
    if (!buf || memchr(buf, '\0', po->string_index + 1) != NULL) {
        runtimeError(vm, "Runtime Error: String index out of range: the string was shortened after the character was addressed.");
        return NULL;
    }
    return buf + po->string_index;
}

// Byte span of character `index` of a UnicodeString's UTF-8 `buf`. Text that is
// all ASCII or not valid UTF-8 is indexed by byte, like utf8CodepointCount and
// GET_CHAR_FROM_STRING do.
static bool vmUnicodeCharSpan(const char* buf, size_t index, size_t* out_offset, size_t* out_len) {
    size_t blen = strlen(buf);
    if (utf8CodepointCount(buf, blen) == blen) {
        if (index >= blen) return false;
        *out_offset = index;
        *out_len = 1;
        return true;
    }
    size_t offset = utf8ByteOffsetForCodepointIndex(buf, blen, index);
    if (offset >= blen) return false;
    uint32_t codepoint = 0;
    size_t advance = 1;
    if (!decodeUtf8Codepoint(buf + offset, blen - offset, &codepoint, &advance)) {
        advance = 1;
    }
    *out_offset = offset;
    *out_len = advance;
    return true;
}

// Reads the character a code-point-indexed (UnicodeString) char pointer names.
static bool vmLoadUnicodeChar(VM* vm, const PointerObj* po, Value* out) {
    const char* buf = po->retained_string->buffer;
    size_t offset = 0, len = 0;
    if (!buf || !vmUnicodeCharSpan(buf, po->string_index, &offset, &len)) {
        runtimeError(vm, "Runtime Error: String index out of range: the string was shortened after the character was addressed.");
        return false;
    }
    uint32_t codepoint = 0;
    size_t advance = 0;
    if (!decodeUtf8Codepoint(buf + offset, len, &codepoint, &advance)) {
        codepoint = (unsigned char)buf[offset];
    }
    *out = makeWideChar((int)codepoint);
    return true;
}

// Stores `value` as character po->string_index of a UnicodeString: encodes it
// as UTF-8 and splices it over the old character's bytes (their lengths can
// differ), so the text stays valid UTF-8.
static bool vmStoreUnicodeChar(VM* vm, PointerObj* po, const Value* value) {
    uint32_t codepoint = 0;
    if (VALUE_TYPE(*value) == TYPE_CHAR) {
        codepoint = (unsigned char)AS_CHAR(*value);
    } else if (VALUE_TYPE(*value) == TYPE_WIDECHAR) {
        long long wide = AS_CHAR(*value);
        if (wide < 0 || wide > 0x10FFFF) {
            runtimeError(vm, "VM Error: Invalid character code point %lld.", wide);
            return false;
        }
        codepoint = (uint32_t)wide;
    } else if (isPascalStringType(VALUE_TYPE(*value)) && AS_STRING(*value)) {
        const char* text = AS_STRING(*value);
        size_t text_len = strlen(text);
        size_t advance = 0;
        if (text_len == 0 || !decodeUtf8Codepoint(text, text_len, &codepoint, &advance) || advance != text_len) {
            runtimeError(vm, "VM Error: Cannot assign multi-character or empty string to a single character location.");
            return false;
        }
    } else {
        runtimeError(vm, "VM Error: Type mismatch for character assignment. Expected CHAR or single-char STRING, got %s.",
                     varTypeToString(VALUE_TYPE(*value)));
        return false;
    }

    StringObj* so = po->retained_string;
    size_t offset = 0, old_len = 0;
    if (!so->buffer || !vmUnicodeCharSpan(so->buffer, po->string_index, &offset, &old_len)) {
        runtimeError(vm, "Runtime Error: String index out of range: the string was shortened after the character was addressed.");
        return false;
    }
    char encoded[5] = {0};
    size_t encoded_len = encodeUtf8Codepoint(codepoint, encoded);
    if (encoded_len == old_len) {
        memcpy(so->buffer + offset, encoded, encoded_len);
        return true;
    }
    size_t old_total = strlen(so->buffer);
    size_t new_total = old_total - old_len + encoded_len;
    if (so->max_length > 0 && new_total > (size_t)so->max_length) {
        runtimeError(vm, "Runtime Error: Character does not fit in string[%d].", so->max_length);
        return false;
    }
    // A string[N] buffer keeps its N+1 capacity; other code writes up to it.
    size_t capacity = (so->max_length > 0 && (size_t)so->max_length > new_total) ? (size_t)so->max_length : new_total;
    char* spliced = (char*)calloc(capacity + 1, 1);
    if (!spliced) {
        runtimeError(vm, "VM Error: Out of memory writing a string character.");
        return false;
    }
    memcpy(spliced, so->buffer, offset);
    memcpy(spliced + offset, encoded, encoded_len);
    memcpy(spliced + offset + encoded_len, so->buffer + offset + old_len, old_total - offset - old_len);
    free(so->buffer);
    so->buffer = spliced;
    return true;
}

// Character `raw_index` (as the program wrote it) of the string in
// *string_val, as a pinned char pointer (makeStringCharPointer): bounds-
// checked by byte, or by code point for a UnicodeString. Pushes it, or a
// STRING_LENGTH_SENTINEL pointer for Pascal's s[0], and returns false after
// reporting an out-of-range index. The string must already be unique.
static bool vmPushStringCharAddress(VM* vm, Value* string_val, long long raw_index, bool allow_length_query) {
    const char* str = AS_STRING(*string_val) ? AS_STRING(*string_val) : "";
    size_t byte_len = strlen(str);
    bool unicode = VALUE_TYPE(*string_val) == TYPE_UNICODE_STRING;
    size_t len = unicode ? utf8CodepointCount(str, byte_len) : byte_len;
    size_t char_offset = 0;
    bool wants_length = false;
    if (!vmResolveStringIndex(vm, raw_index, len, &char_offset, allow_length_query, &wants_length)) {
        return false;
    }
    if (!frontendIsZeroBasedStrings() && wants_length) {
        push(vm, makePointer(string_val, STRING_LENGTH_SENTINEL));
        return true;
    }
    size_t byte_offset = (unicode && len != byte_len)
                             ? utf8ByteOffsetForCodepointIndex(str, byte_len, char_offset)
                             : char_offset;
    push(vm, makeStringCharPointer(string_val, char_offset, unicode, &AS_STRING(*string_val)[byte_offset]));
    return true;
}

static AST* vmResolveTypeAlias(AST* type_node) {
    AST* last = NULL;
    while (type_node && type_node != last) {
        last = type_node;
        if ((type_node->type == AST_TYPE_REFERENCE || type_node->type == AST_VARIABLE) &&
            type_node->token && type_node->token->value) {
            AST* looked = lookupType(type_node->token->value);
            if (!looked || looked == type_node) {
                break;
            }
            type_node = looked;
            continue;
        }
        if (type_node->type == AST_TYPE_DECL && type_node->left) {
            type_node = type_node->left;
            continue;
        }
        break;
    }
    return type_node;
}

static VarType vmDeclaredVarType(AST* type_node) {
    type_node = vmResolveTypeAlias(type_node);
    if (!type_node) {
        return TYPE_VOID;
    }
    if (type_node->var_type != TYPE_VOID && type_node->var_type != TYPE_UNKNOWN) {
        return type_node->var_type;
    }

    switch (type_node->type) {
        case AST_RECORD_TYPE:
            return TYPE_RECORD;
        case AST_ARRAY_TYPE:
            return TYPE_ARRAY;
        case AST_ENUM_TYPE:
            return TYPE_ENUM;
        case AST_POINTER_TYPE:
        case AST_PROC_PTR_TYPE:
            return TYPE_POINTER;
        case AST_INTERFACE:
            return TYPE_INTERFACE;
        case AST_VARIABLE:
        case AST_TYPE_IDENTIFIER:
            if (type_node->token && type_node->token->value) {
                const char* tn = type_node->token->value;
                if (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) return TYPE_INT32;
                if (strcasecmp(tn, "longint") == 0 || strcasecmp(tn, "int64") == 0) return TYPE_INT64;
                if (strcasecmp(tn, "cardinal") == 0) return TYPE_UINT32;
                if (strcasecmp(tn, "char") == 0) return TYPE_CHAR;
                if (strcasecmp(tn, "widechar") == 0) return TYPE_WIDECHAR;
                if (strcasecmp(tn, "string") == 0) return TYPE_STRING;
                if (strcasecmp(tn, "unicodestring") == 0) return TYPE_UNICODE_STRING;
                if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) return TYPE_BOOLEAN;
                if (strcasecmp(tn, "byte") == 0) return TYPE_BYTE;
                if (strcasecmp(tn, "word") == 0) return TYPE_WORD;
                if (strcasecmp(tn, "single") == 0 || strcasecmp(tn, "float") == 0) return TYPE_FLOAT;
                if (strcasecmp(tn, "double") == 0 || strcasecmp(tn, "real") == 0) return TYPE_DOUBLE;
            }
            break;
        default:
            break;
    }

    return TYPE_VOID;
}

static FieldValue* findRecordFieldBySlot(Value* record_struct_ptr, uint16_t field_index) {
    if (!record_struct_ptr || VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
        return NULL;
    }

    bool sawExplicitSlots = false;
    FieldValue* fallback = AS_RECORD(*record_struct_ptr);
    for (uint16_t ordinal = 0; fallback; fallback = fallback->next, ordinal++) {
        if (fallback->slot_index >= 0) {
            sawExplicitSlots = true;
            if ((uint16_t)fallback->slot_index == field_index) {
                return fallback;
            }
        } else if (!sawExplicitSlots && ordinal == field_index) {
            return fallback;
        }
    }

    if (!sawExplicitSlots) {
        return NULL;
    }

    return NULL;
}

static AST* resolveRecordTypeFromBaseValue(Value* base_val_ptr) {
    AST* type_node;

    if (!base_val_ptr) {
        return NULL;
    }

    // VM 2.0 Phase 4i: PTR_BASE_TYPE_NODE now requires TYPE_POINTER.
    // base_val_ptr can legitimately be a raw TYPE_RECORD here (a value-
    // typed record on the stack, not accessed through a pointer) --
    // TYPE_RECORD never populates base_type_node in practice (confirmed
    // by a dedicated audit), so this guard preserves the pre-4i
    // always-NULL-for-records behavior exactly while avoiding an unsafe
    // read through the wrong union member.
    type_node = (VALUE_TYPE(*base_val_ptr) == TYPE_POINTER)
                    ? PTR_BASE_TYPE_NODE(*base_val_ptr)
                    : NULL;
    type_node = vmResolveTypeAlias(type_node);
    if (type_node && type_node->type == AST_POINTER_TYPE && type_node->right) {
        type_node = vmResolveTypeAlias(type_node->right);
    }
    if (type_node && type_node->type == AST_TYPE_DECL && type_node->left) {
        type_node = vmResolveTypeAlias(type_node->left);
    }
    if (type_node && type_node->type == AST_RECORD_TYPE) {
        return type_node;
    }
    return NULL;
}

static void hydrateFieldMetadataFromBaseValue(Value* base_val_ptr,
                                              FieldValue* field,
                                              uint16_t field_index) {
    AST* record_type;
    FieldValue* prototype_fields;
    FieldValue* prototype_field;

    if (!field) {
        return;
    }
    if (field->type_def &&
        field->declared_type != TYPE_UNKNOWN &&
        field->declared_type != TYPE_VOID &&
        field->name) {
        return;
    }

    record_type = resolveRecordTypeFromBaseValue(base_val_ptr);
    if (!record_type) {
        return;
    }

    prototype_fields = createEmptyRecord(record_type);
    if (!prototype_fields) {
        return;
    }

    Value prototype_probe = makeRecord(prototype_fields);
    prototype_field = findRecordFieldBySlot(&prototype_probe, field_index);
    if (prototype_field) {
        if (!field->name && prototype_field->name) {
            field->name = strdup(prototype_field->name);
        }
        if (!field->type_def && prototype_field->type_def) {
            field->type_def = prototype_field->type_def;
        }
        if (field->declared_type == TYPE_UNKNOWN || field->declared_type == TYPE_VOID) {
            field->declared_type = prototype_field->declared_type;
        }
    }

    // The probe's RecordObj owns prototype_fields; freeing only the field
    // list leaked the RecordObj on every call.
    freeValue(&prototype_probe);
}

static bool pushFieldValueByOffset(VM* vm, Value* base_val_ptr, uint16_t field_index) {
    Value* record_struct_ptr = resolveRecordForField(vm, base_val_ptr);
    if (!record_struct_ptr) {
        return false;
    }

    FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
    if (!current) {
        runtimeError(vm, "VM Error: Field index out of range.");
        return false;
    }
    hydrateFieldMetadataFromBaseValue(base_val_ptr, current, field_index);

    push(vm, copyValueForStack(fieldValueStorage(current)));
    return true;
}

static FieldValue* findRecordFieldByName(Value* record_struct_ptr, const char* field_name) {
    FieldValue* current = NULL;
    if (!record_struct_ptr || VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD || !field_name) {
        return NULL;
    }

    current = AS_RECORD(*record_struct_ptr);
    while (current) {
        if (current->name && strcmp(current->name, field_name) == 0) {
            return current;
        }
        current = current->next;
    }

    current = AS_RECORD(*record_struct_ptr);
    while (current) {
        if (current->name && strcasecmp(current->name, field_name) == 0) {
            return current;
        }
        current = current->next;
    }

    return NULL;
}

static bool pushFieldValueByName(VM* vm, Value* base_val_ptr, const char* field_name) {
    if (!field_name) {
        runtimeError(vm, "VM Error: Field name constant is invalid or NULL.");
        return false;
    }

    Value* record_struct_ptr = resolveRecordForField(vm, base_val_ptr);
    if (!record_struct_ptr) {
        return false;
    }

    FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
    if (current) {
        Value* fieldStorage = fieldValueStorage(current);
        // VM 2.0 follow-up to dynamic_array_fresh_publish_race (pscal-core
        // f65432e): checking ARRAY_IS_DYNAMIC(*fieldStorage) here first,
        // before any lock, would itself race a concurrent SetLength publish
        // on this same field -- same "peek before lock" shape as
        // LOAD_ELEMENT_VALUE had before f65432e, just for a record field
        // instead of a plain global. copyDynamicArraySnapshotValue() takes
        // the whole-struct copy, the is_dynamic check, and the conditional
        // retain all under one critical section, so call it unconditionally
        // first and only branch on dynamic-vs-static from the now-safe
        // local copy.
        if (fieldStorage && VALUE_TYPE(*fieldStorage) == TYPE_ARRAY) {
            Value snapshot = copyDynamicArraySnapshotValue(fieldStorage);
            if (ARRAY_IS_DYNAMIC(snapshot)) {
                push(vm, snapshot);
            } else {
                push(vm, copyValueForStack(fieldStorage));
            }
        } else {
            push(vm, copyValueForStack(fieldStorage));
        }
        return true;
    }

    runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
    return false;
}

static bool coerceValueToBoolean(const Value* value, bool* out_truth) {
    if (!value || !out_truth) {
        return false;
    }
    if (IS_BOOLEAN(*value)) {
        *out_truth = AS_BOOLEAN(*value);
        return true;
    }
    if (IS_INTLIKE(*value)) {
        *out_truth = AS_INTEGER(*value) != 0;
        return true;
    }
    if (IS_REAL(*value)) {
        *out_truth = AS_REAL(*value) != 0.0;
        return true;
    }
    if (IS_CHAR(*value)) {
        *out_truth = AS_CHAR(*value) != '\0';
        return true;
    }
    if (VALUE_TYPE(*value) == TYPE_NIL) {
        *out_truth = false;
        return true;
    }
    // A pointer is truthy when it refers to something and falsy when nil. This
    // lets a safe-cast result (e.g. `iface is T`, which yields the narrowed
    // object pointer or nil) be used directly in a boolean context.
    if (VALUE_TYPE(*value) == TYPE_POINTER) {
        *out_truth = (AS_POINTER(*value) != NULL);
        return true;
    }
    return false;
}

static bool vmResolveStringIndex(VM* vm,
                                 long long raw_index,
                                 size_t len,
                                 size_t* out_offset,
                                 bool allow_length_query,
                                 bool* out_length_query) {
    bool is_zero_based = vm ? vm->zeroBasedIndexing : frontendIsZeroBasedStrings();

    if (!is_zero_based)
    {
        if (allow_length_query && raw_index == 0) {
            if (out_length_query) {
                *out_length_query = true;
            }
            if (out_offset) {
                *out_offset = 0;
            }
            return true;
        }

        if (raw_index < 1 || (size_t)raw_index > len) {
            runtimeError(vm,
                         "Runtime Error: String index (%lld) out of bounds for string of length %zu.",
                         raw_index,
                         len);
            return false;
        }

        if (out_length_query) {
            *out_length_query = false;
        }
        if (out_offset) {
            *out_offset = (size_t)(raw_index - 1);
        }
        return true;
    }

    (void)allow_length_query;
    if (raw_index < 0 || (size_t)raw_index >= len) {
        runtimeError(vm,
                     "Runtime Error: String index (%lld) out of bounds for string of length %zu.",
                     raw_index,
                     len);
        return false;
    }

    if (out_length_query) {
        *out_length_query = false;
    }
    if (out_offset) {
        *out_offset = (size_t)raw_index;
    }
    return true;
}

// The dynamic array a derived address must keep alive: the retained owner of
// the pointer it was derived from (a field or element inside an element of a
// dynamic array), retained once more for the new pointer. NULL otherwise.
static ArrayObj* vmRetainPointerOwner(const Value* pointer) {
    if (!pointer || VALUE_TYPE(*pointer) != TYPE_POINTER) return NULL;
    PointerObj* po = PSCAL_VALUE_PTR(*pointer, PointerObj);
    if (!po || !po->retained_array) return NULL;
    pthread_mutex_lock(&dynamic_array_refcount_mutex);
    pscalObjRetain(&po->retained_array->header);
    pthread_mutex_unlock(&dynamic_array_refcount_mutex);
    return po->retained_array;
}

static VarType vmResolveArrayElementType(AST* arrayType) {
    if (!arrayType || arrayType->type != AST_ARRAY_TYPE) {
        return TYPE_UNKNOWN;
    }
    AST* elem = arrayType->right;
    if (!elem) {
        return TYPE_UNKNOWN;
    }
    if (elem->type == AST_TYPE_REFERENCE && elem->token && elem->token->value) {
        AST* looked = lookupType(elem->token->value);
        if (looked) {
            if (looked->type == AST_TYPE_DECL && looked->left) {
                elem = looked->left;
            } else {
                elem = looked;
            }
        }
    }
    if (elem->type == AST_TYPE_DECL && elem->left) {
        elem = elem->left;
    }
    return elem ? elem->var_type : TYPE_UNKNOWN;
}

// True when a pointer's target is a character, byte, length query or opaque
// address rather than a Value that can be indexed or dereferenced as one.
static bool vmPointerTargetIsNotValue(const Value* pointer) {
    AST* base = PTR_BASE_TYPE_NODE(*pointer);
    return isPointerSentinel(base) && base != OWNED_POINTER_SENTINEL;
}

static Value makeOwnedString(char* data, size_t len) {
    Value v;
    memset(&v, 0, sizeof(Value));
    SET_VALUE_TYPE(&v, TYPE_STRING);
    pscalStringEnsureObj(&v); // v.s_val has no wrapper yet after memset
    AS_STRING(v) = data;
    STRING_MAX_LENGTH(v) = -1;
    if (data) {
        data[len] = '\0';
    }
    return v;
}

static unsigned long long vmDisplayIndexFromOffset(size_t offset) {
    if (frontendIsZeroBasedStrings()) {
        return (unsigned long long)offset;
    }
    return (unsigned long long)(offset + 1);
}

static bool vmIndexValueToInt(VM* vm, const Value* index_val, int* out_index) {
    if (!index_val || !out_index) {
        runtimeError(vm, "VM Error: Invalid array index conversion request.");
        return false;
    }

    long long ordinal = 0;
    if (tryValueToOrdinal(index_val, &ordinal)) {
        if (ordinal < INT_MIN || ordinal > INT_MAX) {
            runtimeError(vm, "VM Error: Array index %lld is outside the supported range.", ordinal);
            return false;
        }
        *out_index = (int)ordinal;
        return true;
    }

    if (isRealType(VALUE_TYPE(*index_val))) {
        long double real_index = AS_REAL(*index_val);
        if (real_index < (long double)INT_MIN || real_index > (long double)INT_MAX) {
            runtimeError(vm, "VM Error: Array index %.0Lf is outside the supported range.", real_index);
            return false;
        }
        *out_index = (int)real_index;
        return true;
    }

    runtimeError(vm, "VM Error: Array index must be an ordinal value.");
    return false;
}

static bool vmResolveArraySubrangeBounds(VM* vm, AST* subrange, int* out_lower, int* out_upper) {
    if (!subrange || subrange->type != AST_SUBRANGE || !subrange->left || !subrange->right ||
        !out_lower || !out_upper) {
        runtimeError(vm, "VM Error: Invalid array subrange metadata.");
        return false;
    }

    Value low_val = evaluateCompileTimeValue(subrange->left);
    Value high_val = evaluateCompileTimeValue(subrange->right);
    long long low_ord = 0;
    long long high_ord = 0;
    bool ok = tryValueToOrdinal(&low_val, &low_ord) &&
              tryValueToOrdinal(&high_val, &high_ord) &&
              low_ord >= INT_MIN && low_ord <= INT_MAX &&
              high_ord >= INT_MIN && high_ord <= INT_MAX;
    freeValue(&low_val);
    freeValue(&high_val);

    if (!ok) {
        runtimeError(vm, "VM Error: Array bounds must be constant ordinal values.");
        return false;
    }

    *out_lower = (int)low_ord;
    *out_upper = (int)high_ord;
    return true;
}

// VM 2.0 Phase 4e/4f: several opcode handlers below build a stack-local
// "temp_wrapper" Value whose ArrayObj aliases a raw pointer (a Pascal
// pointer-to-array-type dereference) as array storage without taking
// ownership of it -- the elements/raw buffer belongs to whatever the
// pointer points at, not to temp_wrapper. These two helpers keep that
// lifecycle centralized: init gives temp_wrapper a real (but empty)
// ArrayObj so the ARRAY_*/AS_ARRAY* macros are safe to use on it from the
// start (avoiding the chicken-and-egg NULL-deref those macros would hit on
// an uninitialized wrapper), and free tears down only what temp_wrapper
// itself owns (the wrapper struct and the bounds arrays this code mallocs)
// -- never the aliased elements/raw buffer, and never via
// pscalObjRelease/arrayObjDestroy, which would wrongly try to free/
// freeValue the borrowed storage as if temp_wrapper owned it.
static void vmInitArrayIndexWrapper(Value* temp_wrapper) {
    memset(temp_wrapper, 0, sizeof(Value));
    pscalArrayEnsureObj(temp_wrapper);
}

static void vmFreeArrayIndexWrapper(Value* temp_wrapper) {
    ArrayObj *arr = PSCAL_VALUE_PTR(*temp_wrapper, ArrayObj);
    if (!arr) {
        return;
    }
    free(arr->lower_bounds);
    free(arr->upper_bounds);
    free(arr);
    pscalValueSetHeapPtrBits(temp_wrapper, NULL);
}

static bool adjustLocalByDelta(VM* vm, Value* slot, long long delta, const char* opcode_name) {
    if (!slot) {
        runtimeError(vm, "VM Error: %s encountered a null local slot pointer.", opcode_name);
        return false;
    }

    if (VALUE_TYPE(*slot) == TYPE_ENUM) {
        // The EnumObj may be shared by reference count with copies of this
        // local: unshare it before the in-place write.
        valueEnsureUnique(slot);
        long long new_ord = (long long)AS_ENUM(*slot).ordinal + delta;
        AS_ENUM(*slot).ordinal = (int)new_ord;
        SET_INT_VALUE(slot, AS_ENUM(*slot).ordinal);
        return true;
    }

    if (isIntlikeType(VALUE_TYPE(*slot))) {
        // Same check as ADD/SUBTRACT on the GET_LOCAL/CONSTANT/SET_LOCAL
        // sequence this opcode replaces.
        long long new_val;
        if (__builtin_add_overflow(AS_INTEGER(*slot), delta, &new_val)) {
            runtimeError(vm, "Runtime Error: Integer overflow.");
            return false;
        }
        switch (VALUE_TYPE(*slot)) {
            case TYPE_BOOLEAN:
                SET_INT_VALUE(slot, (new_val != 0));
                break;
            case TYPE_CHAR:
                SET_CHAR_VALUE(slot, (int)new_val);
                SET_INT_VALUE(slot, AS_CHAR(*slot));
                break;
            case TYPE_UINT8:
            case TYPE_BYTE:
            case TYPE_UINT16:
            case TYPE_WORD:
            case TYPE_UINT32:
            case TYPE_UINT64:
                SET_INT_VALUE(slot, (unsigned long long)new_val);
                break;
            default:
                SET_INT_VALUE(slot, new_val);
                break;
        }
        return true;
    }

    if (isRealType(VALUE_TYPE(*slot))) {
        long double current = AS_REAL(*slot);
        long double updated = current + (long double)delta;

        switch (VALUE_TYPE(*slot)) {
            case TYPE_FLOAT: {
                float f = (float)updated;
                SET_REAL_VALUE(slot, f);
                break;
            }
            case TYPE_DOUBLE: {
                double d = (double)updated;
                SET_REAL_VALUE(slot, d);
                break;
            }
            case TYPE_LONG_DOUBLE:
            default:
                SET_REAL_VALUE(slot, updated);
                break;
        }
        SET_INT_VALUE(slot, (long long)updated);
        return true;
    }

    runtimeError(vm, "VM Error: %s requires an ordinal or real local, got %s.",
                 opcode_name, varTypeToString(VALUE_TYPE(*slot)));
    return false;
}

// TYPE_DOUBLE/TYPE_FLOAT operands can be computed and compared in double
// directly. Going through long double gives bit-identical results for them
// (one +,-,*,/ rounded to long double and then to double is the same as
// rounding once, since long double carries at least 2*53+2 bits wherever
// it is wider than double) but on Linux/aarch64 long double is software
// binary128, so every real op paid for emulated arithmetic and conversions.
// Integers stay on the long double path: one above 2^53 would round
// differently if converted to double first.
static inline bool vmIsDoubleOrFloat(VarType type) {
    return type == TYPE_DOUBLE || type == TYPE_FLOAT;
}

static inline Value vmMakeDoubleValue(double d) {
    Value v;
    v.type = TYPE_DOUBLE;
    v.bits = pscalBoxDouble(d);
    return v;
}

// True for the types whose whole payload lives in Value.bits (no heap data).
static inline bool vmTypeIsInlineScalar(VarType type) {
    switch (type) {
        case TYPE_VOID:
        case TYPE_INT32:
        case TYPE_DOUBLE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_NIL:
            return true;
        default:
            return false;
    }
}

static void replaceValueCell(Value* target, Value replacement, AST* preserved_base_type) {
    if (!target) {
        return;
    }

    // Same inline scalar type in and out (an Integer variable taking an
    // Integer): only `bits` changes, and one aligned 64-bit store cannot be
    // seen torn, so skip both global locks. Every local store goes through
    // here, and with worker threads those locks serialised the program.
    if (VALUE_TYPE(*target) == VALUE_TYPE(replacement) &&
        vmTypeIsInlineScalar(VALUE_TYPE(replacement))) {
        __atomic_store_n(&target->bits, replacement.bits, __ATOMIC_RELAXED);
        return;
    }

    /* The `*target = replacement` below is a whole-struct copy (Value is
     * far bigger than a machine word), so it is not atomic with respect to
     * a concurrent reader. copyDynamicArraySnapshotValue() (core/utils.c)
     * takes an unsynchronized whole-struct snapshot of exactly this kind of
     * cell -- e.g. a background thread's `Engine^.Matrix := BuildGrid(...)`
     * racing a main-thread read of Engine^.Matrix -- guarded only by
     * dynamic_array_refcount_mutex. Without also taking that same mutex
     * here, the reader can observe a torn header (new dims/bounds paired
     * with the old data pointer, or vice versa), which showed up as
     * intermittent "Array element index out of bounds" crashes. Lock order
     * (value_cell_mutex, then dynamic_array_refcount_mutex) matches the
     * nesting already used by copyValueForStack -> makeCopyOfValue. */
    pthread_mutex_lock(&value_cell_mutex);
    pthread_mutex_lock(&dynamic_array_refcount_mutex);
    Value old_value = *target;
    *target = replacement;
    if (VALUE_TYPE(*target) == TYPE_POINTER && PTR_BASE_TYPE_NODE(*target) == NULL && preserved_base_type) {
        PTR_BASE_TYPE_NODE(*target) = preserved_base_type;
    }
    pthread_mutex_unlock(&dynamic_array_refcount_mutex);
    pthread_mutex_unlock(&value_cell_mutex);
    freeValue(&old_value);
}

// VM 2.0 Phase 2b (plan §5.7): resolves "__pas_exc_pending" via its
// reserved slot (chunk->global_pas_exc_pending_slot, set by the link step)
// instead of a name-hash lookup. Still locks globals_mutex around the read
// of sym->value's *contents* (unlike GET_GSLOT's unlocked fast path) --
// this mirrors the pre-2b implementation's own locking choice, which
// existed because this value is set/cleared at exception-handling
// boundaries where a torn read would be a real correctness bug, not merely
// a benign race on a pointer.
static bool vmPasExceptionPending(VM* vm) {
    if (!vm || !vm->chunk || vm->chunk->global_pas_exc_pending_slot < 0) {
        return false;
    }

    bool pending = false;
    pthread_mutex_lock(&globals_mutex);
    Symbol* sym = vm->chunk->global_slots[vm->chunk->global_pas_exc_pending_slot].symbol;
    if (sym && sym->value && IS_BOOLEAN(*sym->value)) {
        pending = AS_BOOLEAN(*sym->value);
    }
    pthread_mutex_unlock(&globals_mutex);
    return pending;
}

static Value vmLoadThreadMyselfCopy(VM* vm) {
    if (!vm) {
        return makeNil();
    }
    return copyValueForStack(&vm->threadMyself);
}

static void vmStoreThreadMyself(VM* vm, Value value) {
    if (!vm) {
        freeValue(&value);
        return;
    }
    // VM 2.0 Phase 4i: PTR_BASE_TYPE_NODE now requires TYPE_POINTER (the
    // field moved inside PointerObj) -- threadMyself starts life as
    // makeNil() (TYPE_NIL) before any receiver is ever stored, so this
    // must be guarded rather than called unconditionally.
    AST* preserved_base = (VALUE_TYPE(vm->threadMyself) == TYPE_POINTER)
                               ? PTR_BASE_TYPE_NODE(vm->threadMyself)
                               : NULL;
    Value replacement = makeCopyOfValue(&value);
    replaceValueCell(&vm->threadMyself, replacement, preserved_base);
    freeValue(&value);
}

// --- Class method registration helpers ---
void vmRegisterClassMethod(VM* vm, const char* className, uint16_t methodIndex, Symbol* methodSymbol) {
    if (!vm || !vm->procedureTable || !className || !methodSymbol) return;
    char key[256];
    snprintf(key, sizeof(key), "%s::%u", className, methodIndex);
    Symbol* alias = (Symbol*)malloc(sizeof(Symbol));
    if (!alias) return;
    *alias = *methodSymbol;
    alias->name = strdup(key);
    alias->is_alias = true;
    alias->real_symbol = methodSymbol;
    alias->next = NULL;
    hashTableInsert(vm->procedureTable, alias);
}

Symbol* vmFindClassMethod(VM* vm, const char* className, uint16_t methodIndex) {
    if (!vm || !vm->procedureTable || !className) return NULL;
    char key[256];
    snprintf(key, sizeof(key), "%s::%u", className, methodIndex);
    Symbol* sym = hashTableLookup(vm->procedureTable, key);
    if (sym && sym->is_alias && sym->real_symbol) return sym->real_symbol;
    return sym;
}

// --- Threading Helpers ---
typedef enum {
    THREAD_JOB_BYTECODE,
    THREAD_JOB_CALLBACK,
    THREAD_JOB_BUILTIN
} ThreadJobKind;

typedef struct ThreadJob {
    ThreadJobKind kind;
    uint32_t entry;
    BytecodeChunk* chunk;
    int argc;
    Value* args;
    VMThreadCallback callback;
    VMThreadCleanup cleanup;
    void* user_data;
    VmBuiltinFn builtin;
    int builtin_id;
    char* builtin_name;
    VM* parentVm;
    Value** capturedUpvalues;
    uint8_t capturedUpvalueCount;
    ClosureEnvPayload* closureEnv;
    Symbol* closureSymbol;
    Thread* assignedThread;
    int assignedThreadId;
    bool assignmentSatisfied;
    bool assignmentConsumed;
    pthread_mutex_t assignmentMutex;
    pthread_cond_t assignmentCond;
    bool assignmentSyncInitialized;
    bool submitOnly;
    char name[THREAD_NAME_MAX];
    struct timespec queuedAt;
    struct ThreadJob* next;
} ThreadJob;

typedef struct ThreadJobQueue {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    ThreadJob* head;
    ThreadJob* tail;
    int pending;
    bool shuttingDown;
} ThreadJobQueue;

typedef struct {
    Thread* thread;
    VM* owner;
    int threadId;
    ThreadJob* initialJob;
} ThreadStartArgs;

static bool vmThreadCaptureUpvaluesForJob(VM* vm, ThreadJob* job);
static void vmThreadJobDestroy(ThreadJob* job);

static ThreadJobQueue* vmThreadJobQueueCreate(void) {
    ThreadJobQueue* queue = (ThreadJobQueue*)calloc(1, sizeof(ThreadJobQueue));
    if (!queue) {
        return NULL;
    }
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->cond, NULL);
    queue->head = NULL;
    queue->tail = NULL;
    queue->pending = 0;
    queue->shuttingDown = false;
    return queue;
}

static void vmThreadJobQueueDestroy(ThreadJobQueue* queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_lock(&queue->mutex);
    queue->shuttingDown = true;
    pthread_cond_broadcast(&queue->cond);
    ThreadJob* job = queue->head;
    queue->head = queue->tail = NULL;
    queue->pending = 0;
    pthread_mutex_unlock(&queue->mutex);
    while (job) {
        ThreadJob* next = job->next;
        vmThreadJobDestroy(job);
        job = next;
    }
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
    free(queue);
}

static void vmThreadMetricsReset(ThreadMetrics* metrics) {
    if (!metrics) {
        return;
    }
    memset(metrics, 0, sizeof(ThreadMetrics));
    metrics->start.valid = false;
    metrics->end.valid = false;
}

static size_t vmThreadConvertRssToBytes(long rss) {
#if defined(__APPLE__) && defined(__MACH__)
    return rss < 0 ? 0 : (size_t)rss;
#else
    return rss < 0 ? 0 : (size_t)rss * 1024u;
#endif
}

static void vmThreadMetricsCapture(ThreadMetricsSample* sample) {
    if (!sample) {
        return;
    }
    struct timespec cpuTime;
    bool success = false;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpuTime) == 0) {
        sample->cpuTime = cpuTime;
        success = true;
    } else {
        memset(&sample->cpuTime, 0, sizeof(struct timespec));
    }

    struct rusage usage;
#ifdef RUSAGE_THREAD
    if (getrusage(RUSAGE_THREAD, &usage) == 0) {
#else
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#endif
        sample->usage = usage;
        sample->rssBytes = vmThreadConvertRssToBytes(usage.ru_maxrss);
        success = true;
    } else {
        memset(&sample->usage, 0, sizeof(struct rusage));
        sample->rssBytes = 0;
    }

    sample->valid = success;
}

static void vmThreadJobDestroy(ThreadJob* job) {
    if (!job) {
        return;
    }
    if (job->assignmentSyncInitialized) {
        pthread_mutex_destroy(&job->assignmentMutex);
        pthread_cond_destroy(&job->assignmentCond);
        job->assignmentSyncInitialized = false;
    }
    if (job->closureEnv) {
        releaseClosureEnv(job->closureEnv);
        job->closureEnv = NULL;
    }
    if (job->capturedUpvalues) {
        free(job->capturedUpvalues);
        job->capturedUpvalues = NULL;
    }
    if (job->args) {
        for (int i = 0; i < job->argc; ++i) {
            freeValue(&job->args[i]);
        }
        free(job->args);
    }
    free(job->builtin_name);
    free(job);
}

// A worker must not free a job until the creator that spawned/queued it has
// finished reading job->assignedThread/assignedThreadId back out of it: for
// the queue-reuse path (createThreadJob's "push" branch) that read happens
// asynchronously, racing against how fast the worker can pick up, run, and
// tear down a (possibly trivial/instant) job. Freeing the job as soon as it
// finishes -- without this handshake -- destroys assignmentMutex/Cond out
// from under a creator that hasn't started (or is mid-) waiting on them,
// which is a use-after-free that manifests as the creator hanging forever
// on a condvar nobody will ever signal again. Call this instead of
// vmThreadJobDestroy() at every worker-side teardown point.
static void vmThreadJobAwaitConsumedAndDestroy(ThreadJob* job) {
    if (!job) {
        return;
    }
    if (job->assignmentSyncInitialized) {
        pthread_mutex_lock(&job->assignmentMutex);
        while (!job->assignmentConsumed) {
            pthread_cond_wait(&job->assignmentCond, &job->assignmentMutex);
        }
        pthread_mutex_unlock(&job->assignmentMutex);
    }
    vmThreadJobDestroy(job);
}

static bool vmThreadJobQueuePush(ThreadJobQueue* queue, ThreadJob* job) {
    if (!queue || !job) {
        return false;
    }
    pthread_mutex_lock(&queue->mutex);
    if (queue->shuttingDown) {
        pthread_mutex_unlock(&queue->mutex);
        return false;
    }
    job->next = NULL;
    if (!queue->head) {
        queue->head = queue->tail = job;
    } else {
        queue->tail->next = job;
        queue->tail = job;
    }
    queue->pending++;
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
    return true;
}

static ThreadJob* vmThreadJobQueuePop(ThreadJobQueue* queue,
                                     atomic_bool* shuttingDownFlag,
                                     Thread* thread) {
    if (!queue) {
        return NULL;
    }
    pthread_mutex_lock(&queue->mutex);
    for (;;) {
        if (queue->shuttingDown) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        if (thread &&
            (atomic_load(&thread->killRequested) || atomic_load(&thread->cancelRequested))) {
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        if (queue->head) {
            ThreadJob* job = queue->head;
            queue->head = job->next;
            if (!queue->head) {
                queue->tail = NULL;
            }
            queue->pending--;
            job->next = NULL;
            pthread_mutex_unlock(&queue->mutex);
            return job;
        }
        pthread_cond_wait(&queue->cond, &queue->mutex);
        if (shuttingDownFlag && atomic_load(shuttingDownFlag)) {
            queue->shuttingDown = true;
        }
    }
}

static void vmThreadJobQueueWake(ThreadJobQueue* queue) {
    if (!queue) {
        return;
    }
    pthread_mutex_lock(&queue->mutex);
    pthread_cond_broadcast(&queue->cond);
    pthread_mutex_unlock(&queue->mutex);
}

// VM 2.0 Phase 2a's per-call-site global-access cache helpers
// (vmGetCachedGlobalSymbol/vmCacheGlobalSymbolLocked) were removed in
// Phase 2b: GET_GLOBAL/SET_GLOBAL and their 16-bit variants, the only
// opcodes that ever carried a cache_id, are retired (opcodes.def
// 0x22/0x23/0x25/0x26). chunk->cache_count/chunk->caches/CacheSlot
// themselves are NOT removed -- they remain live infrastructure reserved
// for a future Phase 8 quickening state machine (bytecode.h's CacheSlot
// doc comment) -- but nothing populates or reads them for new chunks now,
// since cache_count is always 0 for anything compiled post-2b.

static ThreadJob* vmThreadJobCreate(VM* vm,
                                    ThreadJobKind kind,
                                    BytecodeChunk* chunk,
                                    uint32_t entry,
                                    ClosureEnvPayload* closureEnv,
                                    Symbol* closureSymbol,
                                    int argc,
                                    const Value* argv,
                                    VMThreadCallback callback,
                                    VMThreadCleanup cleanup,
                                    void* user_data,
                                    VmBuiltinFn builtin,
                                    int builtin_id,
                                    const char* builtin_name,
                                    bool submitOnly,
                                    const char* explicitName) {
    (void)vm;
    ThreadJob* job = (ThreadJob*)calloc(1, sizeof(ThreadJob));
    if (!job) {
        return NULL;
    }
    job->kind = kind;
    job->chunk = chunk;
    job->entry = entry;
    job->argc = 0;
    job->args = NULL;
    job->callback = callback;
    job->cleanup = cleanup;
    job->user_data = user_data;
    job->builtin = builtin;
    job->builtin_id = builtin_id;
    job->builtin_name = builtin_name ? strdup(builtin_name) : NULL;
    job->parentVm = vm;
    job->capturedUpvalues = NULL;
    job->capturedUpvalueCount = 0;
    job->closureEnv = NULL;
    job->closureSymbol = closureSymbol;
    job->assignedThread = NULL;
    job->assignedThreadId = -1;
    job->assignmentSatisfied = false;
    job->assignmentConsumed = false;
    job->assignmentSyncInitialized = false;
    job->submitOnly = submitOnly;
    job->next = NULL;
    clock_gettime(CLOCK_REALTIME, &job->queuedAt);

    if (pthread_mutex_init(&job->assignmentMutex, NULL) == 0 &&
        pthread_cond_init(&job->assignmentCond, NULL) == 0) {
        job->assignmentSyncInitialized = true;
    }

    if (!job->assignmentSyncInitialized) {
        vmThreadJobDestroy(job);
        return NULL;
    }

    if (closureEnv) {
        job->closureEnv = closureEnv;
        retainClosureEnv(job->closureEnv);
    }

    if (argc > 0 && argv) {
        job->args = (Value*)calloc(argc, sizeof(Value));
        if (!job->args) {
            vmThreadJobDestroy(job);
            return NULL;
        }
        for (int i = 0; i < argc; ++i) {
            job->args[i] = makeCopyOfValue(&argv[i]);
        }
        job->argc = argc;
    }

    if (explicitName && explicitName[0] != '\0') {
        strncpy(job->name, explicitName, sizeof(job->name) - 1);
        job->name[sizeof(job->name) - 1] = '\0';
    } else if (builtin_name) {
        strncpy(job->name, builtin_name, sizeof(job->name) - 1);
        job->name[sizeof(job->name) - 1] = '\0';
    } else {
        snprintf(job->name, sizeof(job->name), "thread-%d", builtin_id >= 0 ? builtin_id : 0);
    }

    if (!vmThreadCaptureUpvaluesForJob(vm, job)) {
        vmThreadJobDestroy(job);
        return NULL;
    }

    return job;
}

// Forward declarations for helpers used by threadStart.
static void push(VM* vm, Value value);
static Symbol* findProcedureByAddress(HashTable* table, uint32_t address);
static void vmPopulateProcedureAddressCache(VM* vm);
static Symbol* vmGetProcedureByAddress(VM* vm, uint32_t address);

static bool vmThreadCaptureUpvaluesForJob(VM* vm, ThreadJob* job) {
    if (!vm || !job || job->kind != THREAD_JOB_BYTECODE) {
        return true;
    }

    if (job->closureEnv) {
        return true;
    }

    Symbol* proc_symbol = vmGetProcedureByAddress(vm, job->entry);
    if (!proc_symbol || proc_symbol->upvalue_count == 0) {
        return true;
    }

    const char* proc_name = proc_symbol->name ? proc_symbol->name : "<anonymous>";
    CallFrame* parent_frame = NULL;

    if (proc_symbol->enclosing) {
        for (int fi = vm->frameCount - 1; fi >= 0 && !parent_frame; --fi) {
            CallFrame* candidate = &vm->frames[fi];
            Symbol* frame_symbol = candidate->function_symbol;
            while (frame_symbol) {
                if (frame_symbol == proc_symbol->enclosing) {
                    parent_frame = candidate;
                    break;
                }
                frame_symbol = frame_symbol->enclosing;
            }
        }
    } else if (vm->frameCount > 0) {
        parent_frame = &vm->frames[vm->frameCount - 1];
    }

    if (!parent_frame) {
        runtimeError(vm, "VM Error: Cannot spawn nested procedure '%s' without active parent frame.", proc_name);
        return false;
    }

    job->capturedUpvalueCount = proc_symbol->upvalue_count;
    job->capturedUpvalues = (Value**)calloc(job->capturedUpvalueCount, sizeof(Value*));
    if (!job->capturedUpvalues) {
        job->capturedUpvalueCount = 0;
        runtimeError(vm, "VM Error: Out of memory capturing upvalues for thread spawn of '%s'.", proc_name);
        return false;
    }

    for (int i = 0; i < proc_symbol->upvalue_count; ++i) {
        Value* slot_ptr = NULL;
        if (proc_symbol->upvalues[i].isLocal) {
            uint8_t slot_index = proc_symbol->upvalues[i].index;
            if (parent_frame->slots && slot_index < parent_frame->slotCount) {
                slot_ptr = parent_frame->slots + slot_index;
            }
        } else if (parent_frame->upvalues) {
            uint8_t up_index = proc_symbol->upvalues[i].index;
            if (up_index < parent_frame->upvalue_count) {
                slot_ptr = parent_frame->upvalues[up_index];
            }
        }

        if (!slot_ptr) {
            runtimeError(vm, "VM Error: Failed to capture lexical variable for thread spawn of '%s'.", proc_name);
            free(job->capturedUpvalues);
            job->capturedUpvalues = NULL;
            job->capturedUpvalueCount = 0;
            return false;
        }

        job->capturedUpvalues[i] = slot_ptr;
    }

    return true;
}

static void vmThreadAssignInternalName(Thread* thread, int threadId, const char* requestedName) {
    if (!thread) {
        return;
    }
    if (requestedName && requestedName[0] != '\0') {
        strncpy(thread->name, requestedName, sizeof(thread->name) - 1);
        thread->name[sizeof(thread->name) - 1] = '\0';
    } else {
        snprintf(thread->name, sizeof(thread->name), "worker-%d", threadId);
    }
}

static bool vmThreadPrepareWorkerVm(Thread* thread, VM* owner, ThreadJob* job, int threadId) {
    if (!thread || !owner) {
        return false;
    }
    if (!thread->vm) {
        thread->vm = (VM*)calloc(1, sizeof(VM));
        if (!thread->vm) {
            return false;
        }
        initVM(thread->vm);
        thread->ownsVm = true;
    }

    vmResetExecutionState(thread->vm);

    VM* sourceVm = job && job->parentVm ? job->parentVm : owner;
    if (sourceVm) {
        thread->vm->vmGlobalSymbols = sourceVm->vmGlobalSymbols;
        thread->vm->vmConstGlobalSymbols = sourceVm->vmConstGlobalSymbols;
        thread->vm->procedureTable = sourceVm->procedureTable;
        memcpy(thread->vm->host_functions, sourceVm->host_functions, sizeof(sourceVm->host_functions));
        thread->vm->chunk = (job && job->chunk) ? job->chunk : sourceVm->chunk;
        thread->vm->mutexOwner = sourceVm->mutexOwner ? sourceVm->mutexOwner : sourceVm;
        thread->vm->mutexCount = thread->vm->mutexOwner->mutexCount;
        thread->vm->threadOwner = sourceVm->threadOwner ? sourceVm->threadOwner : sourceVm;
        thread->vm->trace_head_instructions = sourceVm->trace_head_instructions;
    } else {
        thread->vm->chunk = (job && job->chunk) ? job->chunk : owner->chunk;
        thread->vm->mutexOwner = owner;
        thread->vm->threadOwner = owner;
        thread->vm->trace_head_instructions = owner->trace_head_instructions;
    }
#if defined(PSCAL_TARGET_IOS)
    globalSymbols = thread->vm->vmGlobalSymbols;
    constGlobalSymbols = thread->vm->vmConstGlobalSymbols;
    procedure_table = thread->vm->procedureTable;
    current_procedure_table = thread->vm->procedureTable;
#endif
    thread->vm->trace_executed = 0;
    thread->vm->owningThread = thread;
    thread->vm->threadId = threadId;
    return true;
}

static void vmThreadJobSignalAssignment(ThreadJob* job, Thread* thread, int threadId) {
    if (!job || !job->assignmentSyncInitialized) {
        return;
    }
    pthread_mutex_lock(&job->assignmentMutex);
    job->assignedThread = thread;
    job->assignedThreadId = threadId;
    job->assignmentSatisfied = true;
    pthread_cond_broadcast(&job->assignmentCond);
    pthread_mutex_unlock(&job->assignmentMutex);
}

static bool vmThreadAwaitResume(Thread* thread) {
    if (!thread) {
        return false;
    }
    if (!thread->stateSyncInitialized) {
        return true;
    }
    bool continueWork = true;
    pthread_mutex_lock(&thread->stateMutex);
    while (atomic_load(&thread->paused) && !atomic_load(&thread->killRequested)) {
        pthread_cond_wait(&thread->stateCond, &thread->stateMutex);
    }
    if (atomic_load(&thread->cancelRequested) || atomic_load(&thread->killRequested)) {
        continueWork = false;
    }
    pthread_mutex_unlock(&thread->stateMutex);
    return continueWork;
}

static void vmThreadWakeStateWaiters(Thread* thread) {
    if (!thread) {
        return;
    }
    if (thread->stateSyncInitialized) {
        pthread_mutex_lock(&thread->stateMutex);
        pthread_cond_broadcast(&thread->stateCond);
        pthread_mutex_unlock(&thread->stateMutex);
    }
    if (thread->syncInitialized) {
        pthread_mutex_lock(&thread->resultMutex);
        pthread_cond_broadcast(&thread->resultCond);
        pthread_mutex_unlock(&thread->resultMutex);
    }
}

static void vmThreadStoreResultDirect(Thread* thread, const Value* result, bool success) {
    if (!thread || !thread->syncInitialized) {
        return;
    }
    pthread_mutex_lock(&thread->resultMutex);
    if (thread->resultReady) {
        freeValue(&thread->resultValue);
        thread->resultReady = false;
    }
    if (result) {
        thread->resultValue = makeCopyOfValue(result);
    } else {
        thread->resultValue = makeNil();
    }
    thread->resultReady = result != NULL;
    thread->resultConsumed = false;
    thread->statusFlag = success;
    thread->statusReady = true;
    thread->statusConsumed = false;
    pthread_cond_broadcast(&thread->resultCond);
    pthread_mutex_unlock(&thread->resultMutex);
}

static void vmThreadResetResult(Thread* thread) {
    if (!thread) {
        return;
    }
    // resultValue/resultReady/resultConsumed/statusReady/statusFlag/
    // statusConsumed/awaitingReuse are read under resultMutex by
    // joinThreadInternal/vmThreadTakeResult (see those functions) -- reset
    // them under the same lock rather than whatever the caller happens to
    // hold. Pre-fix, every caller here reset these fields while holding only
    // stateMutex, a cross-mutex race TSan confirmed
    // (joinThreadInternal vs. this function, statusReady). Every real call
    // site already holds stateMutex around the call to this function (or is
    // running before the thread slot is shared with any other thread), so
    // nesting resultMutex inside stateMutex here is the one safe nesting
    // order in this file -- never the reverse (see vmThreadTakeResult /
    // vmJoinThreadById, which always fully release resultMutex before
    // acquiring stateMutex).
    pthread_mutex_lock(&thread->resultMutex);
    if (thread->resultReady) {
        freeValue(&thread->resultValue);
    }
    thread->resultValue = makeNil();
    thread->resultReady = false;
    thread->resultConsumed = false;
    thread->statusReady = false;
    thread->statusFlag = false;
    thread->statusConsumed = false;
    thread->awaitingReuse = false;
    pthread_mutex_unlock(&thread->resultMutex);

    thread->currentJob = NULL;
    thread->readyForReuse = false;
    thread->queuedAt = (struct timespec){0, 0};
    thread->startedAt = (struct timespec){0, 0};
    thread->finishedAt = (struct timespec){0, 0};
    vmThreadMetricsReset(&thread->metrics);
    // VM 2.0 Phase 5a checkpoint 5a-iii: clear native-task progress counters
    // on slot reuse so a later, unrelated job (bytecode or a different
    // native task) reusing this slot never inherits stale progress from
    // whatever ran here before.
    atomic_store(&thread->nativeProgressNow, 0);
    atomic_store(&thread->nativeProgressTotal, 0);
}

static void vmThreadInitSlot(Thread* thread) {
    if (!thread) {
        return;
    }
    if (!thread->syncInitialized) {
        pthread_mutex_init(&thread->resultMutex, NULL);
        pthread_cond_init(&thread->resultCond, NULL);
        thread->syncInitialized = true;
    }
    if (!thread->stateSyncInitialized) {
        pthread_mutex_init(&thread->stateMutex, NULL);
        pthread_cond_init(&thread->stateCond, NULL);
        thread->stateSyncInitialized = true;
    }
    atomic_store(&thread->active, false);
    thread->vm = NULL;
    thread->ownsVm = false;
    atomic_store(&thread->inPool, false);
    atomic_store(&thread->needsJoin, false);
    thread->idle = false;
    thread->shouldExit = false;
    thread->awaitingReuse = false;
    thread->readyForReuse = false;
    thread->poolGeneration = 0;
    thread->poolWorker = false;
    thread->currentJob = NULL;
    atomic_store(&thread->paused, false);
    atomic_store(&thread->cancelRequested, false);
    atomic_store(&thread->killRequested, false);
    vmThreadResetResult(thread);
}

static void vmThreadDestroySlot(Thread* thread) {
    if (!thread) {
        return;
    }
    if (thread->syncInitialized) {
        pthread_mutex_destroy(&thread->resultMutex);
        pthread_cond_destroy(&thread->resultCond);
        thread->syncInitialized = false;
    }
    if (thread->stateSyncInitialized) {
        pthread_mutex_destroy(&thread->stateMutex);
        pthread_cond_destroy(&thread->stateCond);
        thread->stateSyncInitialized = false;
    }
    if (thread->resultReady) {
        freeValue(&thread->resultValue);
        thread->resultReady = false;
    }
}

// VM 2.0 Phase 5a checkpoint 5a-ii (Docs/pscal_vm2_plan.md Sec 6.1):
// threads[]/mutexes[] growable storage, mmap-reservation + mprotect-extend
// -- the operand stack's technique (vmAllocStackStorage above), not
// frames[]'s realloc technique. Confirmed necessary, not assumed: a
// dedicated audit found lockMutex/unlockMutex hold a raw Mutex* across a
// released mutexRegistryLock into a potentially unbounded
// pthread_mutex_lock/unlock call, joinThreadInternal/vmThreadTakeResult
// hold a raw Thread* across a resultCond wait loop the same way, and
// VM_s.owningThread (vmThreadPrepareWorkerVm) plus threadStart's own
// persistent `Thread*` parameter both outlive any single call scope for a
// pooled worker's entire lifetime -- realloc relocating the array under
// any of these would dangle a live pointer.

size_t pscalVmThreadsCeiling(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t v = (size_t)VM_MAX_THREADS;
        const char* env = getenv("PSCAL_VM_MAX_THREADS");
        if (env && *env) {
            char* end = NULL;
            unsigned long long parsed = strtoull(env, &end, 10);
            if (end && *end == '\0' && parsed > 0) v = (size_t)parsed;
        }
        cached = v;
    }
    return cached;
}

size_t pscalVmMutexesCeiling(void) {
    static size_t cached = 0;
    if (cached == 0) {
        size_t v = (size_t)VM_MAX_MUTEXES;
        const char* env = getenv("PSCAL_VM_MAX_MUTEXES");
        if (env && *env) {
            char* end = NULL;
            unsigned long long parsed = strtoull(env, &end, 10);
            if (end && *end == '\0' && parsed > 0) v = (size_t)parsed;
        }
        cached = v;
    }
    return cached;
}

// Reserves the full ceiling up front (PROT_NONE) and commits+vmThreadInitSlot
// (real pthread_mutex_init/pthread_cond_init calls, not just zero-fill) the
// initial VM_THREADS_INITIAL_COUNT slots -- matching this codebase's exact
// pre-5a-ii behavior for a VM that never needs to grow. Returns false on
// mmap/mprotect failure (fatal in initVM(), same contract as
// vmAllocStackStorage).
static bool vmAllocThreadStorage(VM* vm) {
    size_t ceiling = pscalVmThreadsCeiling();
    size_t initial = VM_THREADS_INITIAL_COUNT;
    if (initial > ceiling) initial = ceiling;

    size_t reserve_bytes = vmPageRoundUp(ceiling * sizeof(Thread));
    void* mapped = mmap(NULL, reserve_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return false;
    }

    size_t commit_bytes = vmPageRoundUp(initial * sizeof(Thread));
    if (commit_bytes > reserve_bytes) commit_bytes = reserve_bytes;
    if (commit_bytes > 0 && mprotect(mapped, commit_bytes, PROT_READ | PROT_WRITE) != 0) {
        munmap(mapped, reserve_bytes);
        return false;
    }

    vm->threads = (Thread*)mapped;
    vm->threadsMappedBytes = reserve_bytes;
    vm->threadsReservedCount = reserve_bytes / sizeof(Thread);
    vm->threadsCommittedCount = commit_bytes / sizeof(Thread);
    for (size_t i = 0; i < vm->threadsCommittedCount; i++) {
        vmThreadInitSlot(&vm->threads[i]);
    }
    return true;
}

static void vmFreeThreadStorage(VM* vm) {
    if (vm->threads) {
        for (size_t i = 0; i < vm->threadsCommittedCount; i++) {
            vmThreadDestroySlot(&vm->threads[i]);
        }
        munmap(vm->threads, vm->threadsMappedBytes);
    }
    vm->threads = NULL;
    vm->threadsMappedBytes = 0;
    vm->threadsReservedCount = 0;
    vm->threadsCommittedCount = 0;
}

// Extends the committed prefix (mprotect only, base never moves) so at
// least `neededCount` slots are usable, then vmThreadInitSlot's the newly
// committed range (real pthread objects, not just zero-fill -- committed-
// but-never-yet-`vmThreadInitSlot`'d memory would have `syncInitialized==
// false` from the zero page, which every caller already treats as "not
// constructed yet," but createThreadJob's slot-scan and the growth path
// below both expect every committed slot to already be construction-ready
// the moment it becomes visible, matching how the ORIGINAL VM_THREADS_
// INITIAL_COUNT slots are already all vmThreadInitSlot'd up front).
// Caller must hold threadRegistryLock (matches vm->workerCount/
// vm->threadCount's own locking discipline in createThreadJob). Returns
// false only when neededCount exceeds the hard ceiling or mprotect fails.
static bool vmGrowThreadStorage(VM* vm, size_t neededCount) {
    if (neededCount <= vm->threadsCommittedCount) return true;
    if (neededCount > vm->threadsReservedCount) return false;

    size_t newCommitted = vm->threadsCommittedCount * 2;
    if (newCommitted < neededCount) newCommitted = neededCount;
    if (newCommitted > vm->threadsReservedCount) newCommitted = vm->threadsReservedCount;

    size_t oldCommitBytes = vmPageRoundUp(vm->threadsCommittedCount * sizeof(Thread));
    size_t newCommitBytes = vmPageRoundUp(newCommitted * sizeof(Thread));
    if (newCommitBytes > oldCommitBytes) {
        if (mprotect((char*)vm->threads + oldCommitBytes, newCommitBytes - oldCommitBytes,
                     PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
    }
    size_t oldCommittedCount = vm->threadsCommittedCount;
    vm->threadsCommittedCount = newCommitted;
    for (size_t i = oldCommittedCount; i < newCommitted; i++) {
        vmThreadInitSlot(&vm->threads[i]);
    }
    return true;
}

// Mutex slots need no per-slot construction on commit (unlike threads):
// createMutex() itself calls pthread_mutex_init lazily, only for a slot it
// is about to hand out, and the zero-filled `active` flag on every other
// committed-but-unused slot already means "not a real mutex" -- matching
// this codebase's exact pre-5a-ii behavior (initVM only ever set
// `active = false` for the original VM_MUTEXES_INITIAL_COUNT slots, never
// pthread_mutex_init'd them eagerly).
static bool vmAllocMutexStorage(VM* vm) {
    size_t ceiling = pscalVmMutexesCeiling();
    size_t initial = VM_MUTEXES_INITIAL_COUNT;
    if (initial > ceiling) initial = ceiling;

    size_t reserve_bytes = vmPageRoundUp(ceiling * sizeof(Mutex));
    void* mapped = mmap(NULL, reserve_bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        return false;
    }

    size_t commit_bytes = vmPageRoundUp(initial * sizeof(Mutex));
    if (commit_bytes > reserve_bytes) commit_bytes = reserve_bytes;
    if (commit_bytes > 0 && mprotect(mapped, commit_bytes, PROT_READ | PROT_WRITE) != 0) {
        munmap(mapped, reserve_bytes);
        return false;
    }

    vm->mutexes = (Mutex*)mapped;
    vm->mutexesMappedBytes = reserve_bytes;
    vm->mutexesReservedCount = reserve_bytes / sizeof(Mutex);
    vm->mutexesCommittedCount = commit_bytes / sizeof(Mutex);
    return true;
}

static void vmFreeMutexStorage(VM* vm) {
    if (vm->mutexes) {
        munmap(vm->mutexes, vm->mutexesMappedBytes);
    }
    vm->mutexes = NULL;
    vm->mutexesMappedBytes = 0;
    vm->mutexesReservedCount = 0;
    vm->mutexesCommittedCount = 0;
}

// Caller must hold mutexRegistryLock. Newly committed slots are already
// correctly zero-filled (active=false) by mprotect'ing fresh anonymous
// pages -- no per-slot init loop needed, unlike threads above.
static bool vmGrowMutexStorage(VM* vm, size_t neededCount) {
    if (neededCount <= vm->mutexesCommittedCount) return true;
    if (neededCount > vm->mutexesReservedCount) return false;

    size_t newCommitted = vm->mutexesCommittedCount * 2;
    if (newCommitted < neededCount) newCommitted = neededCount;
    if (newCommitted > vm->mutexesReservedCount) newCommitted = vm->mutexesReservedCount;

    size_t oldCommitBytes = vmPageRoundUp(vm->mutexesCommittedCount * sizeof(Mutex));
    size_t newCommitBytes = vmPageRoundUp(newCommitted * sizeof(Mutex));
    if (newCommitBytes > oldCommitBytes) {
        if (mprotect((char*)vm->mutexes + oldCommitBytes, newCommitBytes - oldCommitBytes,
                     PROT_READ | PROT_WRITE) != 0) {
            return false;
        }
    }
    vm->mutexesCommittedCount = newCommitted;
    return true;
}

// A slot's job is finished -- and its result safe to hand out -- once the
// worker has published awaitingReuse, not merely once a status is stored: a
// job stores its status mid-run (threadStart's job switch, or HTTP async from
// inside its callback) and the worker then still tears the job down before it
// parks. joinThreadInternal/vmThreadTakeResult/vmTaskIsDone wait for this, so
// a completed join always leaves the slot in a state ThreadGetResult accepts
// (vmThreadIsRunning false), and a release always finds the worker parked for
// this job. A slot whose worker has gone (!active) counts as finished too.
// Caller holds thread->resultMutex.
static bool vmThreadJobFinishedLocked(Thread* thread) {
    return thread->statusReady && (thread->awaitingReuse || !atomic_load(&thread->active));
}

// Post-job handoff on the worker: publish the finished job, wait until a
// consumer releases the slot (vmThreadReleaseForReuse), a cancel/kill sets
// readyForReuse, or the pool shuts down, then leave the finished state.
// Returns false when the worker should exit instead of taking another job.
static bool vmThreadAwaitRelease(Thread* thread, VM* owner) {
    // awaitingReuse is read under resultMutex by joinThreadInternal/
    // vmThreadTakeResult -- toggle it under that same lock, never nested
    // inside stateMutex from this side. See vmThreadResetResult's comment
    // for the full rationale (TSan-confirmed cross-mutex race). The
    // broadcast wakes joiners waiting on vmThreadJobFinishedLocked.
    pthread_mutex_lock(&thread->resultMutex);
    thread->awaitingReuse = true;
    pthread_cond_broadcast(&thread->resultCond);
    pthread_mutex_unlock(&thread->resultMutex);

    pthread_mutex_lock(&thread->stateMutex);
    pthread_cond_broadcast(&thread->stateCond);
    while (!thread->readyForReuse && !atomic_load(&thread->killRequested) && !atomic_load(&owner->shuttingDownWorkers)) {
        pthread_cond_wait(&thread->stateCond, &thread->stateMutex);
    }
    bool exitLoop = atomic_load(&thread->killRequested) || atomic_load(&owner->shuttingDownWorkers);
    thread->readyForReuse = false;
    // Leave the finished state in the same stateMutex section that consumed
    // readyForReuse, clearing active before awaitingReuse. A reader holding
    // resultMutex then never sees this job's slot as running (active &&
    // !awaitingReuse) once it has finished, and vmThreadReleaseForReuse,
    // which holds stateMutex, never mistakes a consumed release for a
    // pending one. Previously awaitingReuse dropped in its own section and
    // active only after a separate reset, so a late ThreadGetResult could
    // read "still running" and a second release could leave a stale
    // readyForReuse behind for the slot's next job.
    atomic_store(&thread->active, false);
    if (exitLoop) {
        pthread_mutex_lock(&thread->resultMutex);
        thread->awaitingReuse = false;
        pthread_mutex_unlock(&thread->resultMutex);
    } else {
        vmThreadResetResult(thread);
    }
    pthread_mutex_unlock(&thread->stateMutex);
    return !exitLoop;
}

static void* threadStart(void* arg) {
    ThreadStartArgs* args = (ThreadStartArgs*)arg;
    if (!args) {
        return NULL;
    }

    Thread* thread = args->thread;
    VM* owner = args->owner;
    int threadId = args->threadId;
    ThreadJob* job = args->initialJob;
    free(args);

    if (!thread || !owner) {
        if (job) {
            vmThreadJobDestroy(job);
        }
        return NULL;
    }

    while (!atomic_load(&owner->shuttingDownWorkers) && !atomic_load(&thread->killRequested)) {
        (void)vmHandleGlobalInterrupt(owner);
        if (atomic_load(&owner->shuttingDownWorkers) || atomic_load(&thread->killRequested)) {
            break;
        }
        if (!job) {
            pthread_mutex_lock(&owner->threadRegistryLock);
            // vmThreadReleaseForReuse may already have counted this worker
            // as available when it released the slot.
            if (!thread->idle) {
                owner->availableWorkers++;
                thread->idle = true;
            }
            pthread_mutex_unlock(&owner->threadRegistryLock);

            job = vmThreadJobQueuePop(owner->jobQueue, &owner->shuttingDownWorkers, thread);

            pthread_mutex_lock(&owner->threadRegistryLock);
            if (owner->availableWorkers > 0) {
                owner->availableWorkers--;
            }
            thread->idle = false;
            pthread_mutex_unlock(&owner->threadRegistryLock);

            if (!job) {
                break;
            }
        }

        pthread_mutex_lock(&thread->stateMutex);
        vmThreadResetResult(thread);
        vmThreadAssignInternalName(thread, threadId, job->name);
        thread->queuedAt = job->queuedAt;
        thread->currentJob = job;
        thread->poolWorker = job->submitOnly;
        atomic_store(&thread->active, true);
        atomic_store(&thread->cancelRequested, false);
        atomic_store(&thread->paused, false);
        pthread_mutex_unlock(&thread->stateMutex);
        vmThreadJobSignalAssignment(job, thread, threadId);

        if (!vmThreadPrepareWorkerVm(thread, owner, job, threadId)) {
            vmThreadStoreResultDirect(thread, NULL, false);
            vmThreadJobAwaitConsumedAndDestroy(job);
            job = NULL;
            // Hand the failure over like any finished job, or a joiner would
            // wait forever for the awaitingReuse it never published.
            if (!vmThreadAwaitRelease(thread, owner)) {
                break;
            }
            continue;
        }

        if (!vmThreadAwaitResume(thread)) {
            atomic_store(&thread->cancelRequested, true);
        }

        VM* workerVm = thread->vm;
        bool canceled = atomic_load(&thread->cancelRequested);
        bool killed = atomic_load(&thread->killRequested) || atomic_load(&owner->shuttingDownWorkers);

        if (vmHandleGlobalInterrupt(owner) ||
            (workerVm && (workerVm->abort_requested || workerVm->exit_requested))) {
            canceled = true;
            killed = true;
        }

        if (!canceled && !killed) {
            clock_gettime(CLOCK_REALTIME, &thread->startedAt);
            pthread_mutex_lock(&thread->stateMutex);
            vmThreadMetricsCapture(&thread->metrics.start);
            pthread_mutex_unlock(&thread->stateMutex);
        }

        if (!workerVm) {
            canceled = true;
        }

        if (!canceled && !killed && workerVm) {
            workerVm->current_builtin_name = NULL;
            workerVm->abort_requested = false;
            workerVm->runtime_error_raised = false;
            workerVm->exit_requested = false;
            workerVm->suspend_unwind_requested = false;
        }

        switch (job->kind) {
            case THREAD_JOB_CALLBACK:
                if (!canceled && !killed && job->callback && workerVm) {
                    job->callback(workerVm, job->user_data);
                }
                if (job->cleanup) {
                    job->cleanup(job->user_data);
                }
                if (canceled || killed) {
                    vmThreadStoreResultDirect(thread, NULL, false);
                }
                break;
            case THREAD_JOB_BUILTIN: {
                bool success = false;
                if (!canceled && !killed && job->builtin && workerVm) {
                    const char* previous_builtin = workerVm->current_builtin_name;
                    if (job->builtin_name) {
                        workerVm->current_builtin_name = job->builtin_name;
                    }
                    Value resultValue = job->builtin(workerVm, job->argc, job->args);
                    success = !workerVm->abort_requested;
                    vmThreadStoreResult(workerVm, &resultValue, success);
                    freeValue(&resultValue);
                    workerVm->current_builtin_name = previous_builtin;
                } else {
                    vmThreadStoreResultDirect(thread, NULL, false);
                }
                break;
            }
            case THREAD_JOB_BYTECODE: {
                if (!canceled && !killed && workerVm) {
                    Symbol* proc_symbol = job->closureSymbol ? job->closureSymbol
                                                            : vmGetProcedureByAddress(workerVm, job->entry);
                    CallFrame* frame = &workerVm->frames[workerVm->frameCount++];
                    frame->return_address = NULL;
                    frame->slots = workerVm->stack;
                    frame->function_symbol = proc_symbol;
                    frame->slotCount = 0;
                    frame->locals_count = proc_symbol ? proc_symbol->locals_count : 0;
                    frame->upvalue_count = proc_symbol ? proc_symbol->upvalue_count : 0;
                    frame->upvalues = NULL;
                    frame->owns_upvalues = false;
                    frame->closureEnv = NULL;
                    frame->discard_result_on_return = false;
                    frame->vtable = NULL;

                    bool ready_for_execution = true;

                    if (job->closureEnv) {
                        if (!proc_symbol) {
                            runtimeError(workerVm,
                                         "VM Error: Missing symbol for closure thread entry at %u.",
                                         job->entry);
                            ready_for_execution = false;
                        } else if (job->closureEnv->slot_count != proc_symbol->upvalue_count) {
                            runtimeError(workerVm,
                                         "VM Error: Closure environment mismatch for thread entry '%s'.",
                                         proc_symbol->name ? proc_symbol->name : "<anonymous>");
                            ready_for_execution = false;
                        } else {
                            frame->closureEnv = job->closureEnv;
                            retainClosureEnv(frame->closureEnv);
                            frame->upvalues = frame->closureEnv->slots;
                            frame->owns_upvalues = false;
                        }
                    } else if (proc_symbol && proc_symbol->upvalue_count > 0) {
                        frame->upvalues = calloc(proc_symbol->upvalue_count, sizeof(Value*));
                        frame->owns_upvalues = frame->upvalues != NULL;
                        if (frame->upvalues) {
                            for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                                frame->upvalues[i] = NULL;
                            }
                        }
                    }

                    int expected = proc_symbol && proc_symbol->type_def ? proc_symbol->type_def->child_count : job->argc;
                    int pushed_args = 0;
                    int limit = job->argc;
                    if (limit > 8) limit = 8;
                    for (int i = 0; i < expected && i < limit; i++) {
                        // VM 2.0 Phase 4g: must be an independent copy, not a
                        // bare struct alias -- job->args[i] is later freed by
                        // vmThreadJobDestroy, and a bare `Value v = job->args[i]`
                        // would alias job->args[i]'s own boxed wrapper (e.g. a
                        // TYPE_POINTER's PointerObj), double-releasing it once
                        // both this pushed copy and the job's own copy get
                        // freed independently.
                        Value v = job->args ? makeCopyOfValue(&job->args[i]) : makeNil();
                        if (proc_symbol && proc_symbol->type_def) {
                            AST* param_ast = proc_symbol->type_def->children[i];
                            if (param_ast) {
                                if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(v))) {
                                    long double tmp = asLd(v);
                                    setTypeValue(&v, param_ast->var_type);
                                    SET_REAL_VALUE(&v, tmp);
                                }
                            }
                        }
                        push(workerVm, v);
                        pushed_args++;
                    }

                    for (int i = 0; proc_symbol && i < proc_symbol->locals_count; i++) {
                        push(workerVm, makeNil());
                    }

                    if (proc_symbol) {
                        frame->slotCount = (uint16_t)(pushed_args + proc_symbol->locals_count);
                    } else {
                        frame->slotCount = (uint16_t)pushed_args;
                    }

                    if (ready_for_execution && proc_symbol && proc_symbol->upvalue_count > 0 && !job->closureEnv) {
                        if (!frame->upvalues || !job->capturedUpvalues ||
                            job->capturedUpvalueCount != proc_symbol->upvalue_count) {
                            runtimeError(workerVm,
                                         "VM Error: Missing lexical context for thread entry '%s'.",
                                         proc_symbol->name ? proc_symbol->name : "<anonymous>");
                            ready_for_execution = false;
                        } else {
                            for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                                frame->upvalues[i] = job->capturedUpvalues[i];
                                if (!frame->upvalues[i]) {
                                    runtimeError(workerVm,
                                                 "VM Error: Incomplete lexical capture for thread entry '%s'.",
                                                 proc_symbol->name ? proc_symbol->name : "<anonymous>");
                                    ready_for_execution = false;
                                    break;
                                }
                            }
                        }
                    }

                    if (!ready_for_execution) {
                        if (frame->closureEnv) {
                            releaseClosureEnv(frame->closureEnv);
                            frame->closureEnv = NULL;
                        } else if (frame->owns_upvalues && frame->upvalues) {
                            free(frame->upvalues);
                            frame->upvalues = NULL;
                        }
                        frame->owns_upvalues = false;
                        workerVm->frameCount--;
                        vmThreadStoreResultDirect(thread, NULL, false);
                    } else {
                        InterpretResult jobResult = interpretBytecode(workerVm, workerVm->chunk, workerVm->vmGlobalSymbols,
                                          workerVm->vmConstGlobalSymbols, workerVm->procedureTable, job->entry);
                        // VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md
                        // Sec 6.1): THREAD_JOB_BYTECODE previously never stored a
                        // result at all -- THREAD_JOIN's own doc comment already
                        // says it "consumes and discards" the result, which was
                        // literally true because nothing upstream of it ever
                        // captured one. TaskAwait needs an actual value for
                        // function tasks, so capture it here: returnFromCall's
                        // halt path (frameCount reaching 0) leaves a function's
                        // result pushed on workerVm's own stack, exactly mirroring
                        // what a synchronous CALL's caller would otherwise pop --
                        // this manually-installed top-level frame has no such
                        // caller, so the value just sits there until taken. Only
                        // pop it for an actual function (has_result, same
                        // condition returnFromCall itself uses); a bare procedure
                        // has nothing on the stack to pop.
                        bool has_result = proc_symbol && proc_symbol->type != TYPE_VOID;
                        if (jobResult == INTERPRET_OK && has_result &&
                            workerVm->stackTop > workerVm->stack) {
                            // pop() is defined later in this file (static,
                            // no forward declaration) -- inlined here rather
                            // than reordering; matches pop()'s own body.
                            workerVm->stackTop--;
                            Value taskResult = *workerVm->stackTop;
                            SET_VALUE_TYPE(workerVm->stackTop, TYPE_VOID);
                            pscalValueSetHeapPtrBits(workerVm->stackTop, NULL);
                            vmThreadStoreResultDirect(thread, &taskResult, true);
                            freeValue(&taskResult);
                        } else if (jobResult != INTERPRET_OK) {
                            // The job aborted (runtimeError already printed its
                            // diagnostic and set workerVm->abort_requested) --
                            // report failure explicitly, mirroring
                            // THREAD_JOB_BUILTIN's `success = !workerVm->
                            // abort_requested` above. Without this, falling
                            // through left statusReady unset here, so the
                            // generic post-switch fallback below stored
                            // success=true (it only checks the pre-run
                            // canceled/killed flags, not whether the job
                            // itself crashed) -- silently turning a crashed
                            // task into what looked like a normal nil-
                            // returning success to TaskAwait.
                            vmThreadStoreResultDirect(thread, NULL, false);
                        }
                        // else: jobResult == INTERPRET_OK but nothing to pop
                        // (void procedure, or a function whose result was
                        // already consumed) -- fall through to the generic
                        // success=true store below, unchanged.
                    }
                } else {
                    vmThreadStoreResultDirect(thread, NULL, false);
                }
                break;
            }
            default:
                break;
        }

        // VM 2.0 Phase 5a checkpoint 5a-i: this read of statusReady was
        // already unlocked before this checkpoint (TSan just never had a
        // reason to catch it -- nothing awaited a THREAD_JOB_BYTECODE job's
        // result fast enough to overlap it until TaskAwait's own stress
        // fixtures did). Only T37 (this worker) ever WRITES statusReady at
        // this point in its lifecycle, so the decision below is always
        // internally consistent from T37's own perspective regardless of
        // locking -- but an awaiter thread can concurrently read the same
        // field under resultMutex (vmThreadTakeResult), and an unsynchronized
        // access racing a locked one is still undefined behavior. Read
        // under the same lock so there's no race, even though the
        // lock-then-unlock-then-maybe-store below is not a single atomic
        // step -- that's fine, since only this thread writes here.
        pthread_mutex_lock(&thread->resultMutex);
        bool alreadyStored = thread->statusReady;
        pthread_mutex_unlock(&thread->resultMutex);
        if (!alreadyStored) {
            vmThreadStoreResultDirect(thread, NULL, !(canceled || killed));
        }

        if (!canceled && !killed) {
            pthread_mutex_lock(&thread->stateMutex);
            vmThreadMetricsCapture(&thread->metrics.end);
            pthread_mutex_unlock(&thread->stateMutex);
            clock_gettime(CLOCK_REALTIME, &thread->finishedAt);
        } else {
            vmThreadStoreResultDirect(thread, NULL, false);
        }

        vmThreadJobAwaitConsumedAndDestroy(job);
        job = NULL;

        if (!vmThreadAwaitRelease(thread, owner)) {
            break;
        }
    }

    pthread_mutex_lock(&owner->threadRegistryLock);
    if (owner->availableWorkers > 0 && thread->idle) {
        owner->availableWorkers--;
    }
    pthread_mutex_lock(&thread->stateMutex);
    thread->idle = false;
    pthread_mutex_unlock(&thread->stateMutex);
    atomic_store(&thread->active, false);
    owner->workerCount--;
    pthread_mutex_unlock(&owner->threadRegistryLock);

    if (thread->ownsVm && thread->vm) {
        freeVM(thread->vm);
        free(thread->vm);
        thread->vm = NULL;
        thread->ownsVm = false;
    }
    pthread_mutex_lock(&thread->stateMutex);
    vmThreadResetResult(thread);
    pthread_mutex_unlock(&thread->stateMutex);
    // VM 2.0 Phase 5a checkpoint 5a-i: currentJob=NULL MUST happen before
    // inPool's seq_cst store flips to false, not after -- a spawner
    // reusing this slot only synchronizes on inPool (createThreadJob's
    // slot-scan reads it, also seq_cst), so inPool going false is the
    // "safe to reuse" publish signal every other plain field write here
    // must precede in program order for the spawner to be guaranteed to
    // see it (the standard atomic-flag-as-publish-gate idiom -- getting
    // the order backwards, as an earlier draft of this function did, is
    // a real, TSan-confirmed, reachable race: createThreadJob vm.c:2430
    // could write currentJob=job for the new job concurrently with this
    // thread still writing currentJob=NULL for the one it just finished).
    thread->currentJob = NULL;
    atomic_store(&thread->inPool, false);

    return NULL;
}

static int createThreadJob(VM* vm,
                           ThreadJobKind kind,
                           BytecodeChunk* chunk,
                           uint32_t entry,
                           ClosureEnvPayload* closureEnv,
                           Symbol* closureSymbol,
                           int argc,
                           const Value* argv,
                           VMThreadCallback callback,
                           VMThreadCleanup cleanup,
                           void* user_data,
                           VmBuiltinFn builtin,
                           int builtin_id,
                           const char* builtin_name,
                           bool submitOnly,
                           const char* threadName) {
    if (!vm) {
        return -1;
    }

    ThreadJob* job = vmThreadJobCreate(vm, kind, chunk, entry, closureEnv, closureSymbol, argc, argv, callback, cleanup, user_data,
                                       builtin, builtin_id, builtin_name, submitOnly, threadName);
    if (!job) {
        return -1;
    }

    int assignedId = -1;
    Thread* assignedThread = NULL;
    bool spawnNewWorker = false;

    pthread_mutex_lock(&vm->threadRegistryLock);
    if (!vm->jobQueue) {
        pthread_mutex_unlock(&vm->threadRegistryLock);
        vmThreadJobDestroy(job);
        return -1;
    }

    // Prefer an already-idle pooled worker over spawning a brand-new OS
    // thread: a worker that finished a job loops back to the top of
    // threadStart and parks in vmThreadJobQueuePop (announcing itself via
    // availableWorkers++, still inPool==true -- see threadStart's job==NULL
    // branch) rather than clearing inPool, since inPool marks "this slot is
    // a live pooled worker", not "this worker is currently busy". The
    // free-slot scan below only ever finds inPool==false on a slot whose
    // worker has fully exited (kill/shutdown), which natural job completion
    // never triggers -- so without this check every spawn here would burn a
    // fresh pthread even in a sequential spawn/wait/consume/repeat loop that
    // only ever needs one worker at a time (confirmed via
    // thread_worker_reuse.exsh: 18 sequential cycles produced 18 unique
    // thread IDs). When an idle worker exists, fall through to the
    // job-queue push below instead -- that path already reads
    // job->assignedThread/assignedThreadId back out once the idle worker's
    // threadStart loop pops the job and calls vmThreadJobSignalAssignment,
    // and is exercised (pool-at-ceiling case) and TSan-clean already.
    //
    // VM 2.0 Phase 5a checkpoint 5a-ii: reuse a free slot within the
    // currently committed range first (identical scan to pre-5a-ii); only
    // grow (vmGrowThreadStorage) when every committed slot is occupied but
    // the ceiling still allows more -- keeps the common case (a hole
    // exists) exactly as cheap as before, growth only on genuine pressure.
    size_t threadsCeiling = pscalVmThreadsCeiling();
    if (vm->availableWorkers == 0 && (size_t)vm->workerCount < threadsCeiling - 1) {
        for (size_t i = 1; i < vm->threadsCommittedCount; ++i) {
            Thread* candidate = &vm->threads[i];
            if (!atomic_load(&candidate->inPool)) {
                // This slot's previous occupant (if any) has already run to
                // completion -- inPool only goes false as the very last step
                // before threadStart returns (see needsJoin's vm.h comment)
                // -- but nothing has reaped its OS thread yet if it exited
                // on its own (e.g. an explicit per-task cancel/kill) rather
                // than through a full VM teardown. Reap it here, before
                // pthread_create below overwrites candidate->handle with a
                // new thread's id: otherwise the old, already-finished
                // pthread's handle is lost forever (nobody left holding it
                // to join), a genuine, confirmed leak distinct from the
                // needsJoin/shutdown-race one (reproduced via
                // Tests/vm_thread_stress/http_async_task_stress.pas and
                // task_cancel_race.pas, which explicitly cancel mid-flight
                // tasks and churn this exact slot-reuse path). This join
                // returns immediately since the thread has already exited.
                if (atomic_load(&candidate->needsJoin)) {
                    pthread_join(candidate->handle, NULL);
                    atomic_store(&candidate->needsJoin, false);
                }
                assignedThread = candidate;
                assignedId = (int)i;
                spawnNewWorker = true;
                break;
            }
        }
        if (!spawnNewWorker && vm->threadsCommittedCount < threadsCeiling) {
            size_t newSlotIndex = vm->threadsCommittedCount;
            if (vmGrowThreadStorage(vm, newSlotIndex + 1)) {
                assignedThread = &vm->threads[newSlotIndex];
                assignedId = (int)newSlotIndex;
                spawnNewWorker = true;
            }
        }
    }

    if (spawnNewWorker && assignedThread) {
        int originalGeneration = assignedThread->poolGeneration;
        atomic_store(&assignedThread->inPool, true);
        atomic_store(&assignedThread->active, true);
        assignedThread->idle = false;
        assignedThread->poolGeneration++;
        vm->workerCount++;
        pthread_mutex_unlock(&vm->threadRegistryLock);

        vmThreadAssignInternalName(assignedThread, assignedId, job->name);
        assignedThread->queuedAt = job->queuedAt;
        assignedThread->currentJob = job;
        assignedThread->readyForReuse = false;
        assignedThread->awaitingReuse = false;
        atomic_store(&assignedThread->cancelRequested, false);
        atomic_store(&assignedThread->killRequested, false);

        ThreadStartArgs* args = (ThreadStartArgs*)calloc(1, sizeof(ThreadStartArgs));
        if (!args) {
            pthread_mutex_lock(&vm->threadRegistryLock);
            vm->workerCount--;
            atomic_store(&assignedThread->inPool, false);
            assignedThread->poolGeneration = originalGeneration;
            atomic_store(&assignedThread->active, false);
            assignedThread->idle = false;
            assignedThread->poolWorker = false;
            assignedThread->awaitingReuse = false;
            assignedThread->readyForReuse = false;
            assignedThread->currentJob = NULL;
            assignedThread->queuedAt = (struct timespec){0, 0};
            memset(&assignedThread->handle, 0, sizeof(pthread_t));
            pthread_mutex_unlock(&vm->threadRegistryLock);
            vmThreadResetResult(assignedThread);
            atomic_store(&assignedThread->cancelRequested, false);
            atomic_store(&assignedThread->killRequested, false);
            atomic_store(&assignedThread->paused, false);
            vmThreadJobDestroy(job);
            return -1;
        }

        args->thread = assignedThread;
        args->owner = vm;
        args->threadId = assignedId;
        args->initialJob = job;

        if (pthread_create(&assignedThread->handle, NULL, threadStart, args) != 0) {
            free(args);
            pthread_mutex_lock(&vm->threadRegistryLock);
            vm->workerCount--;
            atomic_store(&assignedThread->inPool, false);
            assignedThread->poolGeneration = originalGeneration;
            atomic_store(&assignedThread->active, false);
            assignedThread->idle = false;
            assignedThread->poolWorker = false;
            assignedThread->awaitingReuse = false;
            assignedThread->readyForReuse = false;
            assignedThread->currentJob = NULL;
            assignedThread->queuedAt = (struct timespec){0, 0};
            memset(&assignedThread->handle, 0, sizeof(pthread_t));
            pthread_mutex_unlock(&vm->threadRegistryLock);
            vmThreadResetResult(assignedThread);
            atomic_store(&assignedThread->cancelRequested, false);
            atomic_store(&assignedThread->killRequested, false);
            atomic_store(&assignedThread->paused, false);
            vmThreadJobDestroy(job);
            return -1;
        }

        // pthread_create succeeded: this slot now owns a live, joinable OS
        // thread that only ever gets reaped by a teardown path's
        // pthread_join call (see needsJoin's declaration comment in vm.h) --
        // set this before any other thread can observe the handle as valid.
        atomic_store(&assignedThread->needsJoin, true);

        vmThreadJobSignalAssignment(job, assignedThread, assignedId);
        // We already have assignedThread/assignedId locally (we created this
        // worker ourselves), so we won't read them back out of job -- mark
        // consumed immediately so the worker's teardown isn't left waiting
        // for an ack we'd otherwise never send on this path.
        if (job->assignmentSyncInitialized) {
            pthread_mutex_lock(&job->assignmentMutex);
            job->assignmentConsumed = true;
            pthread_cond_broadcast(&job->assignmentCond);
            pthread_mutex_unlock(&job->assignmentMutex);
        }
        if (assignedId >= vm->threadCount) {
            vm->threadCount = assignedId + 1;
        }
        return assignedId;
    }

    pthread_mutex_unlock(&vm->threadRegistryLock);

    if (!vmThreadJobQueuePush(vm->jobQueue, job)) {
        vmThreadJobDestroy(job);
        return -1;
    }

    if (job->assignmentSyncInitialized) {
        pthread_mutex_lock(&job->assignmentMutex);
        while (!job->assignmentSatisfied) {
            pthread_cond_wait(&job->assignmentCond, &job->assignmentMutex);
        }
        assignedThread = job->assignedThread;
        assignedId = job->assignedThreadId;
        // Tell the worker it's safe to free this job now that we've copied
        // out the fields we need -- see vmThreadJobAwaitConsumedAndDestroy().
        job->assignmentConsumed = true;
        pthread_cond_broadcast(&job->assignmentCond);
        pthread_mutex_unlock(&job->assignmentMutex);
    }

    return assignedThread ? assignedId : -1;
}

static int createThreadWithArgs(VM* vm,
                                uint32_t entry,
                                ClosureEnvPayload* closureEnv,
                                Symbol* closureSymbol,
                                int argc,
                                const Value* argv) {
    return createThreadJob(vm, THREAD_JOB_BYTECODE, vm ? vm->chunk : NULL, entry, closureEnv, closureSymbol, argc, argv, NULL,
                           NULL, NULL, NULL, -1, NULL, false, NULL);
}

// Backward-compatible helper: no argument provided, pass NIL
static int createThread(VM* vm, uint32_t entry) {
    return createThreadWithArgs(vm, entry, NULL, NULL, 0, NULL);
}

// VM 2.0 Phase 5a checkpoint 5a-i: non-blocking "has this task's job
// completed" poll for TaskDone -- reads Thread.statusReady under
// resultMutex (the lock joinThreadInternal/vmThreadTakeResult already treat
// as authoritative for this field, see vmThreadResetResult's comment) so
// builtin.c never has to reach into Thread's fields directly, which would
// reintroduce the exact cross-mutex race class fixed just before this
// phase started. Does not consume or block -- a later TaskAwait/TaskDone
// on the same task still sees a consistent state.
bool vmTaskIsDone(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool) || !thread->syncInitialized) {
        return false;
    }
    pthread_mutex_lock(&thread->resultMutex);
    bool done = vmThreadJobFinishedLocked(thread);
    pthread_mutex_unlock(&thread->resultMutex);
    return done;
}

bool vmThreadIsRunning(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->syncInitialized) {
        return atomic_load(&thread->active);
    }
    pthread_mutex_lock(&thread->resultMutex);
    bool running = atomic_load(&thread->active) && !thread->awaitingReuse;
    pthread_mutex_unlock(&thread->resultMutex);
    return running;
}

int vmSpawnCallbackThread(VM* vm, VMThreadCallback callback, void* user_data, VMThreadCleanup cleanup) {
    if (!vm || !callback) {
        if (cleanup && user_data) {
            cleanup(user_data);
        }
        return -1;
    }
    return createThreadJob(vm, THREAD_JOB_CALLBACK, vm->chunk, 0, NULL, NULL, 0, NULL, callback, cleanup, user_data, NULL, -1, NULL, false, NULL);
}

// VM 2.0 Phase 5a checkpoint 5a-iii (Docs/pscal_vm2_plan.md Sec 6.1): see
// the vm.h declaration for the full rationale. Unlike vmSpawnCallbackThread,
// guarantees cleanup runs exactly once regardless of why spawning failed
// (not just the narrow !vm||!callback case) -- createThreadJob's own
// internal failure paths (pool exhausted, mmap-growth failed, pthread_create
// failed) call vmThreadJobDestroy, which frees the job's own bookkeeping but
// deliberately never touches job->user_data/cleanup (a job can be destroyed
// before a worker ever picks it up, so "call cleanup" and "destroy the job
// struct" are different concerns) -- so this wrapper closes that gap for
// callers who want a simple, uniform contract.
int vmTaskCreateNative(VM* vm, VMThreadCallback work_fn, void* user_data, VMThreadCleanup cleanup) {
    int id = vmSpawnCallbackThread(vm, work_fn, user_data, cleanup);
    if (id < 0) {
        // vmSpawnCallbackThread already ran cleanup for the !vm||!callback
        // case; for every other failure inside createThreadJob it did not,
        // so this call is a no-op there and the only real invocation here
        // for those cases. Callers must not double-free user_data
        // themselves after calling this function.
        if (vm && work_fn && cleanup && user_data) {
            cleanup(user_data);
        }
        return -1;
    }
    // Progress counters only -- no cancel hook to set here, see the vm.h
    // declaration's comment for why (nativeCancelFn was removed after a
    // confirmed race against this exact slot's first-job-pickup
    // vmThreadResetResult call on the worker's own thread).
    VM* owner = vm->threadOwner ? vm->threadOwner : vm;
    Thread* thread = &owner->threads[id];
    atomic_store(&thread->nativeProgressNow, 0);
    atomic_store(&thread->nativeProgressTotal, 0);
    return id;
}

void vmTaskReportProgress(VM* threadVm, long long now, long long total) {
    if (!threadVm || !threadVm->owningThread) {
        return;
    }
    atomic_store(&threadVm->owningThread->nativeProgressNow, now);
    atomic_store(&threadVm->owningThread->nativeProgressTotal, total);
}

bool vmTaskGetProgress(VM* owner, int threadId, long long* outNow, long long* outTotal) {
    if (!owner || threadId <= 0 || (size_t)threadId >= owner->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &owner->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    if (outNow) *outNow = atomic_load(&thread->nativeProgressNow);
    if (outTotal) *outTotal = atomic_load(&thread->nativeProgressTotal);
    return true;
}

int vmSpawnBuiltinThread(VM* vm, int builtinId, const char* builtinName, int argCount,
                         const Value* args, bool submitOnly, const char* threadName) {
    if (!vm || builtinId < 0) {
        return -1;
    }
    VmBuiltinFn handler = getVmBuiltinHandlerById(builtinId);
    if (!handler) {
        return -1;
    }
    return createThreadJob(vm, THREAD_JOB_BUILTIN, vm->chunk, 0, NULL, NULL, argCount, args, NULL, NULL, NULL,
                           handler, builtinId, builtinName, submitOnly, threadName);
}

static void vmMarkAbortRequested(VM* vm) {
    if (!vm) {
        return;
    }
    vm->abort_requested = true;
    vm->exit_requested = true;
    vm->suspend_unwind_requested = false;
    if (vm->threadOwner) {
        vm->threadOwner->abort_requested = true;
        vm->threadOwner->exit_requested = true;
        vm->threadOwner->suspend_unwind_requested = false;
    }
}

static bool vmIsRootExecutor(const VM* vm) {
    if (!vm) {
        return true;
    }
    const VM* root = vm->threadOwner ? vm->threadOwner : vm;
    return vm == root;
}

static bool vmConsumeInterruptRequest(VM* vm) {
    VM* root = vm ? (vm->threadOwner ? vm->threadOwner : vm) : NULL;
    bool poll_runtime_signal = (vm == NULL) || vmIsRootExecutor(vm);
#if defined(PSCAL_TARGET_IOS)
    bool allow_runtime_signal = false;
    if (poll_runtime_signal) {
        allow_runtime_signal = vmRuntimeSignalAppliesCached(root ? root : vm, "consume-int");
    }
#else
    bool allow_runtime_signal = true;
    (void)poll_runtime_signal;
#endif
 #if defined(PSCAL_TARGET_IOS)
    if (poll_runtime_signal && vmIosDebugEnabled()) {
        static _Thread_local uint64_t tlsLastHeartbeatNs = 0;
        uint64_t now_ns = vmNowMonoNs();
        if (now_ns > 0 && (tlsLastHeartbeatNs == 0 || now_ns - tlsLastHeartbeatNs >= 1000000000ull)) {
            vmIosDebugLogf("[vm-int] heartbeat vm=%p root=%p allow=%d abort=%d exit=%d sigflag=%d sigseen=%d",
                           (void*)vm,
                           (void*)root,
                           allow_runtime_signal ? 1 : 0,
                           root ? (root->abort_requested ? 1 : 0) : 0,
                           root ? (root->exit_requested ? 1 : 0) : 0,
                           pscalRuntimeInterruptFlag() ? 1 : 0,
                           pscalRuntimeSigintPending() ? 1 : 0);
            tlsLastHeartbeatNs = now_ns;
        }
    }
 #endif
    if (poll_runtime_signal && vmHandleGlobalInterrupt(root)) {
        return true;
    }
    if (poll_runtime_signal &&
        allow_runtime_signal &&
        pscalRuntimeSigintPending() &&
        pscalRuntimeConsumeSigint()) {
        vmMarkAbortRequested(root ? root : vm);
        (void)vmHandleGlobalInterrupt(root);
        return true;
    }
    if (root && (root->abort_requested || root->exit_requested)) {
        return true;
    }
    if (vm && vm != root && (vm->abort_requested || vm->exit_requested)) {
        return true;
    }
    return false;
}

static bool vmConsumeSuspendRequest(VM* vm) {
    VM* root = vm ? (vm->threadOwner ? vm->threadOwner : vm) : NULL;
    bool poll_runtime_signal = (vm == NULL) || vmIsRootExecutor(vm);
#if defined(PSCAL_TARGET_IOS)
    bool allow_runtime_signal = poll_runtime_signal &&
                                vmRuntimeSignalAppliesCached(root ? root : vm, "consume-suspend");
#else
    bool allow_runtime_signal = true;
    (void)poll_runtime_signal;
#endif
    if (!allow_runtime_signal || !pscalRuntimeConsumeSigtstp()) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (!vmShouldUseCooperativeSuspendForCurrentVproc() &&
        vmRequestHardSuspendCurrentVproc()) {
        (void)vprocWaitIfStopped(vprocCurrent());
        return false;
    }
#endif
    VM* target = root ? root : vm;
    if (target) {
        target->abort_requested = false;
        target->exit_requested = true;
        target->suspend_unwind_requested = true;
        target->current_builtin_name = "signal";
    }
    shellRuntimeSetLastStatus(128 + SIGTSTP);
    return true;
}

static bool vmHandleGlobalInterrupt(VM* vm) {
#if defined(PSCAL_TARGET_IOS)
    bool allow_runtime_signal = vmRuntimeSignalAppliesCached(vm, "handle-global");
#else
    bool allow_runtime_signal = true;
#endif
    bool pending = allow_runtime_signal && (pscalRuntimeInterruptFlag() || pscalRuntimeSigintPending());
    if (!pending && vm) {
        // A worker VM whose own job failed keeps abort_requested (with
        // runtime_error_raised) until its next job resets it. That failure is
        // reported through the job's thread status; it is not a request to
        // stop the program. The workers it spawned poll it as their owner,
        // and escalating it here shut every worker down and set the root's
        // exit_requested, so the program ended early with exit 0, or a joiner
        // deadlocked. A shutdown or an exit still sets exit_requested.
        bool workerJobFailed = vm->threadOwner && vm->threadOwner != vm &&
                               vm->runtime_error_raised && !vm->exit_requested;
        pending = !workerJobFailed && (vm->abort_requested || vm->exit_requested);
    }
    if (!pending) {
        return false;
    }

    VM* root = vm ? (vm->threadOwner ? vm->threadOwner : vm) : NULL;
    if (root) {
        root->abort_requested = true;
        root->exit_requested = true;
        root->suspend_unwind_requested = false;
        atomic_store(&root->shuttingDownWorkers, true);
        if (root->jobQueue) {
            pthread_mutex_lock(&root->jobQueue->mutex);
            root->jobQueue->shuttingDown = true;
            pthread_cond_broadcast(&root->jobQueue->cond);
            pthread_mutex_unlock(&root->jobQueue->mutex);
        }
        for (size_t i = 1; i < root->threadsCommittedCount; ++i) {
            Thread* thread = &root->threads[i];
            if (!atomic_load(&thread->inPool)) {
                continue;
            }
            atomic_store(&thread->cancelRequested, true);
            atomic_store(&thread->killRequested, true);
            if (thread->vm) {
                thread->vm->abort_requested = true;
                thread->vm->exit_requested = true;
                thread->vm->suspend_unwind_requested = false;
            }
            vmThreadWakeStateWaiters(thread);
            pthread_mutex_lock(&thread->resultMutex);
            pthread_cond_broadcast(&thread->resultCond);
            pthread_mutex_unlock(&thread->resultMutex);
        }
        vmThreadJobQueueWake(root->jobQueue);
    }
    if (allow_runtime_signal) {
        pscalRuntimeClearInterruptFlag();
    }
    return true;
}

static void vmComputeDeadline(struct timespec* out, long millis) {
    if (!out) {
        return;
    }
    clock_gettime(CLOCK_REALTIME, out);
    out->tv_nsec += millis * 1000000L;
    out->tv_sec += out->tv_nsec / 1000000000L;
    out->tv_nsec = out->tv_nsec % 1000000000L;
}

void vmThreadStoreResult(VM* vm, const Value* result, bool success) {
    if (!vm || !vm->owningThread) {
        return;
    }
    Thread* thread = vm->owningThread;
    pthread_mutex_lock(&thread->resultMutex);
    if (thread->resultReady) {
        freeValue(&thread->resultValue);
        thread->resultReady = false;
    }
    if (result) {
        thread->resultValue = makeCopyOfValue(result);
    } else {
        thread->resultValue = makeNil();
    }
    thread->resultReady = true;
    thread->resultConsumed = false;
    thread->statusFlag = success;
    thread->statusReady = true;
    thread->statusConsumed = false;
    pthread_cond_broadcast(&thread->resultCond);
    pthread_mutex_unlock(&thread->resultMutex);
}

// Hand a finished slot back to its worker. Lock order is threadRegistryLock
// -> stateMutex -> resultMutex, as in threadStart's exit path and
// vmThreadResetResult. Acts only while the worker is parked for this job
// (awaitingReuse set, readyForReuse not yet), which vmThreadAwaitRelease
// makes exact, so a repeated or late release is a no-op. It also counts the
// worker as available here, before waking it: a spawn issued straight after
// the release (a spawn/wait/consume loop) used to find availableWorkers == 0
// until the woken worker got back to vmThreadJobQueuePop, and started a
// second worker instead (thread_worker_reuse.exsh's "unique:2").
static void vmThreadReleaseForReuse(VM* owner, Thread* thread) {
    pthread_mutex_lock(&owner->threadRegistryLock);
    pthread_mutex_lock(&thread->stateMutex);
    pthread_mutex_lock(&thread->resultMutex);
    bool parked = thread->awaitingReuse;
    pthread_mutex_unlock(&thread->resultMutex);
    if (parked && !thread->readyForReuse) {
        if (!thread->idle) {
            thread->idle = true;
            owner->availableWorkers++;
        }
        thread->readyForReuse = true;
        pthread_cond_broadcast(&thread->stateCond);
    }
    pthread_mutex_unlock(&thread->stateMutex);
    pthread_mutex_unlock(&owner->threadRegistryLock);
}

// A joiner waits holding the joined thread's resultMutex, but an interrupt
// poll can run vmHandleGlobalInterrupt, which locks every thread's
// resultMutex to wake its waiters: polling with the lock held deadlocked the
// joiner on itself (a nested par branch that failed hung the program). Poll
// with it released; the caller rechecks the job state after re-locking.
static bool vmJoinPollInterruptUnlocked(VM* vm, Thread* thread) {
    pthread_mutex_unlock(&thread->resultMutex);
    bool interrupted = vmConsumeInterruptRequest(vm);
    pthread_mutex_lock(&thread->resultMutex);
    return interrupted;
}

bool vmThreadTakeResult(VM* vm, int threadId, Value* outResult, bool takeValue, bool* outStatus, bool takeStatus) {
    if (!vm) {
        return false;
    }
    if (threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->syncInitialized) {
        return false;
    }

    pthread_mutex_lock(&thread->resultMutex);
    while (!vmThreadJobFinishedLocked(thread)) {
        if (!atomic_load(&thread->active) && !thread->awaitingReuse) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
        if (vmJoinPollInterruptUnlocked(vm, thread)) {
            pthread_mutex_unlock(&thread->resultMutex);
            vmThreadCancel(vm, threadId);
            return false;
        }
        if (vmThreadJobFinishedLocked(thread)) {
            break;
        }
        struct timespec deadline;
        vmComputeDeadline(&deadline, 100);
        int wait_status = pthread_cond_timedwait(&thread->resultCond, &thread->resultMutex, &deadline);
        if (wait_status == ETIMEDOUT || wait_status == EINTR) {
            continue;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
    }

    if (outStatus) {
        *outStatus = thread->statusFlag;
    }
    if (takeStatus) {
        thread->statusConsumed = true;
    }

    if (takeValue) {
        if (thread->resultReady) {
            if (outResult) {
                *outResult = thread->resultValue;
            } else {
                freeValue(&thread->resultValue);
            }
            thread->resultValue = makeNil();
            thread->resultReady = false;
        } else if (outResult) {
            *outResult = makeNil();
        }
        thread->resultConsumed = true;
    } else if (outResult) {
        if (thread->resultReady) {
            *outResult = makeCopyOfValue(&thread->resultValue);
        } else {
            *outResult = makeNil();
        }
    }

    bool releaseWorker = false;
    if (thread->statusConsumed && (!thread->resultReady || thread->resultConsumed)) {
        if (thread->resultReady) {
            freeValue(&thread->resultValue);
            thread->resultValue = makeNil();
            thread->resultReady = false;
        }
        thread->statusReady = false;
        thread->statusConsumed = false;
        thread->resultConsumed = false;
        releaseWorker = true;
    }
    pthread_mutex_unlock(&thread->resultMutex);

    if (releaseWorker) {
        vmThreadReleaseForReuse(vm, thread);
    }
    return true;
}

static bool joinThreadInternal(VM* vm, int id) {
    if (!vm) {
        return false;
    }
    if (id <= 0 || (size_t)id >= vm->threadsCommittedCount) {
        return false;
    }

    Thread* thread = &vm->threads[id];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    pthread_mutex_lock(&thread->resultMutex);
    while (!vmThreadJobFinishedLocked(thread)) {
        if (!atomic_load(&thread->active) && !thread->awaitingReuse) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
        if (vmJoinPollInterruptUnlocked(vm, thread)) {
            pthread_mutex_unlock(&thread->resultMutex);
            vmThreadCancel(vm, id);
            return false;
        }
        if (vmThreadJobFinishedLocked(thread)) {
            break;
        }
        struct timespec deadline;
        vmComputeDeadline(&deadline, 100);
        int wait_status = pthread_cond_timedwait(&thread->resultCond, &thread->resultMutex, &deadline);
        if (wait_status == ETIMEDOUT || wait_status == EINTR) {
            continue;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
    }
    pthread_mutex_unlock(&thread->resultMutex);
    return true;
}

// VM 2.0 Phase 5b checkpoint 5b-i (Docs/pscal_vm2_plan.md Sec 6.2): Channel
// blocking Send/Receive follow the identical wait-loop shape
// vmThreadTakeResult/joinThreadInternal above already established --
// pthread_cond_timedwait on a 100ms deadline, rechecking cancellation and
// the general interrupt-request hook on every wake rather than a single
// unbounded wait. Two differences from those functions' cancellation
// check: (1) there is no single "the thread this operation is about" to
// call vmThreadCancel on the way those functions' vmConsumeInterruptRequest
// callers do -- a channel op blocks the CALLING thread itself, not some
// other thread's job, so on interrupt this just unblocks and returns
// rather than canceling anything; (2) it additionally polls the calling
// thread's OWN Thread.cancelRequested (via vm->owningThread, NULL when
// running on the main/root VM) so a task blocked on
// ChannelSend/ChannelReceive actually wakes when TaskCancel'd -- the same
// poll-don't-callback pattern 5a-iii's HTTP async cancellation uses (see
// httpJobCancelRequested's comment in builtin_network_api.c for why a
// callback-based hook was tried and reverted there).
static bool vmChannelOperationInterrupted(VM* vm) {
    if (!vm) {
        return false;
    }
    if (vm->owningThread && atomic_load(&vm->owningThread->cancelRequested)) {
        return true;
    }
    return vmConsumeInterruptRequest(vm);
}

VmChannelStatus vmChannelSend(VM* vm, ChannelObj* ch, Value value) {
    if (!ch) {
        freeValue(&value);
        return VM_CHANNEL_CLOSED;
    }
    pthread_mutex_lock(&ch->lock);
    while (ch->count >= ch->capacity && !ch->closed) {
        if (vmChannelOperationInterrupted(vm)) {
            pthread_mutex_unlock(&ch->lock);
            freeValue(&value);
            return VM_CHANNEL_INTERRUPTED;
        }
        struct timespec deadline;
        vmComputeDeadline(&deadline, 100);
        int wait_status = pthread_cond_timedwait(&ch->notFull, &ch->lock, &deadline);
        if (wait_status == ETIMEDOUT || wait_status == EINTR) {
            continue;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&ch->lock);
            freeValue(&value);
            return VM_CHANNEL_INTERRUPTED;
        }
    }
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        freeValue(&value);
        return VM_CHANNEL_CLOSED;
    }
    size_t tail = (ch->head + ch->count) % ch->capacity;
    ch->buf[tail] = value; // ownership transferred into the ring buffer
    ch->count++;
    pthread_cond_signal(&ch->notEmpty); // one item added -- wake at most one receiver, not all
    pthread_mutex_unlock(&ch->lock);
    return VM_CHANNEL_OK;
}

VmChannelStatus vmChannelReceive(VM* vm, ChannelObj* ch, Value* outValue) {
    if (!ch) {
        if (outValue) *outValue = makeNil();
        return VM_CHANNEL_CLOSED;
    }
    pthread_mutex_lock(&ch->lock);
    while (ch->count == 0 && !ch->closed) {
        if (vmChannelOperationInterrupted(vm)) {
            pthread_mutex_unlock(&ch->lock);
            if (outValue) *outValue = makeNil();
            return VM_CHANNEL_INTERRUPTED;
        }
        struct timespec deadline;
        vmComputeDeadline(&deadline, 100);
        int wait_status = pthread_cond_timedwait(&ch->notEmpty, &ch->lock, &deadline);
        if (wait_status == ETIMEDOUT || wait_status == EINTR) {
            continue;
        }
        if (wait_status != 0) {
            pthread_mutex_unlock(&ch->lock);
            if (outValue) *outValue = makeNil();
            return VM_CHANNEL_INTERRUPTED;
        }
    }
    if (ch->count == 0) {
        // Closed and drained -- not an error, see VmChannelStatus's comment.
        pthread_mutex_unlock(&ch->lock);
        if (outValue) *outValue = makeNil();
        return VM_CHANNEL_CLOSED;
    }
    Value v = ch->buf[ch->head];
    ch->buf[ch->head] = makeNil(); // vacated slot cleared, no dangling ownership
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->notFull); // one slot freed -- wake at most one sender, not all
    pthread_mutex_unlock(&ch->lock);
    if (outValue) {
        *outValue = v; // ownership transferred out to the caller
    } else {
        freeValue(&v);
    }
    return VM_CHANNEL_OK;
}

VmChannelStatus vmChannelTrySend(ChannelObj* ch, Value value) {
    if (!ch) {
        freeValue(&value);
        return VM_CHANNEL_CLOSED;
    }
    pthread_mutex_lock(&ch->lock);
    if (ch->closed) {
        pthread_mutex_unlock(&ch->lock);
        freeValue(&value);
        return VM_CHANNEL_CLOSED;
    }
    if (ch->count >= ch->capacity) {
        pthread_mutex_unlock(&ch->lock);
        freeValue(&value);
        return VM_CHANNEL_WOULD_BLOCK;
    }
    size_t tail = (ch->head + ch->count) % ch->capacity;
    ch->buf[tail] = value;
    ch->count++;
    pthread_cond_signal(&ch->notEmpty);
    pthread_mutex_unlock(&ch->lock);
    return VM_CHANNEL_OK;
}

VmChannelStatus vmChannelTryReceive(ChannelObj* ch, Value* outValue) {
    if (!ch) {
        if (outValue) *outValue = makeNil();
        return VM_CHANNEL_CLOSED;
    }
    pthread_mutex_lock(&ch->lock);
    if (ch->count == 0) {
        VmChannelStatus status = ch->closed ? VM_CHANNEL_CLOSED : VM_CHANNEL_WOULD_BLOCK;
        pthread_mutex_unlock(&ch->lock);
        if (outValue) *outValue = makeNil();
        return status;
    }
    Value v = ch->buf[ch->head];
    ch->buf[ch->head] = makeNil();
    ch->head = (ch->head + 1) % ch->capacity;
    ch->count--;
    pthread_cond_signal(&ch->notFull);
    pthread_mutex_unlock(&ch->lock);
    if (outValue) {
        *outValue = v;
    } else {
        freeValue(&v);
    }
    return VM_CHANNEL_OK;
}

void vmChannelClose(ChannelObj* ch) {
    if (!ch) {
        return;
    }
    pthread_mutex_lock(&ch->lock);
    ch->closed = true; // monotonic -- never reset to false
    pthread_cond_broadcast(&ch->notEmpty); // wake EVERY blocked receiver, not just one
    pthread_cond_broadcast(&ch->notFull);  // wake EVERY blocked sender, not just one
    pthread_mutex_unlock(&ch->lock);
}

bool vmChannelIsClosed(ChannelObj* ch) {
    if (!ch) {
        return true; // a nil/never-created channel is inert, same as a closed one
    }
    pthread_mutex_lock(&ch->lock);
    bool closed = ch->closed;
    pthread_mutex_unlock(&ch->lock);
    return closed;
}

static void joinThread(VM* vm, int id) {
    if (!vm) {
        return;
    }
    joinThreadInternal(vm, id);
}

bool vmJoinThreadById(VM* vm, int id) {
    joinThread(vm, id);

    /* Ensure any pending status/result is consumed so the worker can be reused. */
    if (vm && id > 0 && (size_t)id < vm->threadsCommittedCount) {
        vmThreadTakeResult(vm, id, NULL, false, NULL, true);
        Thread *thread = &vm->threads[id];
        if (thread && atomic_load(&thread->inPool) && thread->syncInitialized) {
            // vmThreadTakeResult already released the slot if nothing is left
            // to consume; a status still standing means a result value is
            // pending for ThreadGetResult, which releases it then. With no
            // status left, release again: it is a no-op unless the worker
            // is still parked for this job.
            pthread_mutex_lock(&thread->resultMutex);
            bool nothing_pending = !thread->statusReady;
            pthread_mutex_unlock(&thread->resultMutex);
            if (nothing_pending) {
                vmThreadReleaseForReuse(vm, thread);
            }
        }
    }
    return true;
}

bool vmThreadAssignName(VM* vm, int threadId, const char* name) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    pthread_mutex_lock(&vm->threadRegistryLock);
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        pthread_mutex_unlock(&vm->threadRegistryLock);
        return false;
    }
    vmThreadAssignInternalName(thread, threadId, name);
    pthread_mutex_unlock(&vm->threadRegistryLock);
    return true;
}

int vmThreadFindIdByName(VM* vm, const char* name) {
    if (!vm || !name) {
        return -1;
    }
    for (size_t i = 1; i < vm->threadsCommittedCount; ++i) {
        Thread* thread = &vm->threads[i];
        if (!atomic_load(&thread->inPool)) {
            continue;
        }
        if (strncmp(thread->name, name, THREAD_NAME_MAX) == 0) {
            return (int)i;
        }
    }
    return -1;
}

bool vmThreadPause(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    atomic_store(&thread->paused, true);
    vmThreadWakeStateWaiters(thread);
    return true;
}

bool vmThreadResume(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    atomic_store(&thread->paused, false);
    vmThreadWakeStateWaiters(thread);
    return true;
}

bool vmThreadCancel(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    atomic_store(&thread->cancelRequested, true);
    // VM 2.0 Phase 5a checkpoint 5a-iii: native (non-bytecode) work has no
    // interpreter safe points to cooperatively observe cancelRequested at,
    // unlike bytecode where the interpreter loop already polls it. There is
    // deliberately no separate native-cancel-hook mechanism here: an earlier
    // draft added a per-slot Thread.nativeCancelFn invoked from this
    // function, but that write raced against this exact worker's own
    // threadStart job-pickup code resetting the slot on its first loop
    // iteration (confirmed via a failing cancel-demo run, not just
    // theorized). Native work_fn implementations must instead poll
    // Thread.cancelRequested directly at their own natural safe points
    // (e.g. HTTP async's curl progress callback does) -- that needs no new
    // plumbing since every native work_fn already has its own Thread*.
    vmThreadWakeStateWaiters(thread);
    pthread_mutex_lock(&thread->stateMutex);
    thread->readyForReuse = true;
    pthread_cond_broadcast(&thread->stateCond);
    pthread_mutex_unlock(&thread->stateMutex);
    vmThreadJobQueueWake(vm->jobQueue);
    return true;
}

bool vmThreadKill(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || (size_t)threadId >= vm->threadsCommittedCount) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!atomic_load(&thread->inPool)) {
        return false;
    }
    atomic_store(&thread->killRequested, true);
    vmThreadWakeStateWaiters(thread);
    pthread_mutex_lock(&thread->stateMutex);
    thread->readyForReuse = true;
    pthread_cond_broadcast(&thread->stateCond);
    pthread_mutex_unlock(&thread->stateMutex);
    vmThreadJobQueueWake(vm->jobQueue);
    return true;
}

size_t vmSnapshotWorkerUsage(VM* vm, ThreadMetrics* outMetrics, size_t capacity) {
    if (!vm || !outMetrics || capacity == 0) {
        return 0;
    }
    size_t count = 0;
    for (size_t i = 1; i < vm->threadsCommittedCount && count < capacity; ++i) {
        Thread* thread = &vm->threads[i];
        if (!atomic_load(&thread->inPool)) {
            continue;
        }
        if (pthread_mutex_trylock(&thread->stateMutex) != 0) {
            continue;
        }
        ThreadMetrics snapshot = thread->metrics;
        if (atomic_load(&thread->active)) {
            ThreadMetricsSample currentSample = snapshot.start;
            vmThreadMetricsCapture(&currentSample);
            snapshot.end = currentSample;
        }
        outMetrics[count++] = snapshot;
        pthread_mutex_unlock(&thread->stateMutex);
    }
    return count;
}

size_t vmSnapshotProcState(VMProcSnapshot* out, size_t capacity) {
    if (!out || capacity == 0) {
        return 0;
    }
    pthread_mutex_lock(&gVmProcRegistryLock);
    size_t count = 0;
    size_t limit = gVmProcRegistryCount;
    if (limit > capacity) {
        limit = capacity;
    }
    for (size_t i = 0; i < limit; ++i) {
        VM* vm = gVmProcRegistry[i];
        if (!vm) {
            continue;
        }
        vmProcFillSnapshot(vm, &out[count++]);
    }
    pthread_mutex_unlock(&gVmProcRegistryLock);
    return count;
}

size_t vmSnapshotProcWorkers(uintptr_t vm_address,
                             VMProcWorkerSnapshot* out,
                             size_t capacity) {
    if (!out || capacity == 0 || vm_address == 0) {
        return 0;
    }

    pthread_mutex_lock(&gVmProcRegistryLock);
    VM* vm = NULL;
    for (size_t i = 0; i < gVmProcRegistryCount; ++i) {
        if ((uintptr_t)gVmProcRegistry[i] == vm_address) {
            vm = gVmProcRegistry[i];
            break;
        }
    }
    if (!vm) {
        pthread_mutex_unlock(&gVmProcRegistryLock);
        return 0;
    }

    VM* root = vm->threadOwner ? vm->threadOwner : vm;
    size_t count = 0;
    for (size_t i = 1; i < root->threadsCommittedCount && count < capacity; ++i) {
        Thread* thread = &root->threads[i];
        bool include = atomic_load(&thread->inPool) || atomic_load(&thread->active) || thread->vm != NULL;
        if (!include) {
            continue;
        }

        VMProcWorkerSnapshot snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.slot_id = (int)i;
        snapshot.vm_address = (uintptr_t)thread->vm;

        bool locked = (pthread_mutex_trylock(&thread->stateMutex) == 0);
        snapshot.in_pool = atomic_load(&thread->inPool);
        snapshot.active = atomic_load(&thread->active);
        snapshot.idle = thread->idle;
        snapshot.owns_vm = thread->ownsVm;
        snapshot.pool_worker = thread->poolWorker;
        snapshot.awaiting_reuse = thread->awaitingReuse;
        snapshot.ready_for_reuse = thread->readyForReuse;
        snapshot.status_ready = thread->statusReady;
        snapshot.result_ready = thread->resultReady;
        snapshot.paused = atomic_load(&thread->paused);
        snapshot.cancel_requested = atomic_load(&thread->cancelRequested);
        snapshot.kill_requested = atomic_load(&thread->killRequested);
        snapshot.pool_generation = thread->poolGeneration;
        snapshot.queued_at = thread->queuedAt;
        snapshot.started_at = thread->startedAt;
        snapshot.finished_at = thread->finishedAt;
        snapshot.metrics = thread->metrics;
        if (atomic_load(&thread->active) && snapshot.metrics.start.valid) {
            ThreadMetricsSample currentSample = snapshot.metrics.start;
            vmThreadMetricsCapture(&currentSample);
            snapshot.metrics.end = currentSample;
        }
        if (thread->name[0]) {
            strncpy(snapshot.name, thread->name, sizeof(snapshot.name) - 1);
            snapshot.name[sizeof(snapshot.name) - 1] = '\0';
        } else {
            snprintf(snapshot.name, sizeof(snapshot.name), "worker-%d", (int)i);
        }
        if (locked) {
            pthread_mutex_unlock(&thread->stateMutex);
        }
        out[count++] = snapshot;
    }

    pthread_mutex_unlock(&gVmProcRegistryLock);
    return count;
}

// --- Mutex Helpers ---
static int createMutex(VM* vm, bool recursive) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    int id = -1;
    // Look for an inactive slot to reuse.

    for (int i = 0; i < owner->mutexCount; i++) {
        if (!owner->mutexes[i].active) {
            id = i;
            break;
        }
    }
    // If none found, append a new mutex if capacity allows (VM 2.0 Phase 5a
    // checkpoint 5a-ii: grow the committed range on demand, up to the
    // ceiling, mirroring createThreadJob's slot-scan-then-grow shape).
    if (id == -1) {
        if ((size_t)owner->mutexCount >= pscalVmMutexesCeiling()) {
            pthread_mutex_unlock(&owner->mutexRegistryLock);
            return -1;
        }
        if (!vmGrowMutexStorage(owner, (size_t)owner->mutexCount + 1)) {
            pthread_mutex_unlock(&owner->mutexRegistryLock);
            return -1;
        }
        id = owner->mutexCount;
        owner->mutexCount++;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutexattr_t attr;
    pthread_mutexattr_t* attr_ptr = NULL;
    if (recursive) {
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        attr_ptr = &attr;
    }
    if (pthread_mutex_init(&m->handle, attr_ptr) != 0) {
        if (attr_ptr) pthread_mutexattr_destroy(&attr);
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return -1;
    }
    if (attr_ptr) pthread_mutexattr_destroy(&attr);
    m->active = true;
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return id;
}

static bool lockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutex_unlock(&owner->mutexRegistryLock);
#if defined(PSCAL_TARGET_IOS)
    if (vmIosMutexDebugEnabled()) {
        int rc = pthread_mutex_trylock(&m->handle);
        if (rc == 0) {
            return true;
        }
        if (rc != EBUSY) {
            vmIosDebugLogf("[vm-mutex] trylock failed vm=%p owner=%p mid=%d rc=%d",
                           (void*)vm, (void*)owner, id, rc);
            return false;
        }

        uint64_t start_ns = vmNowMonoNs();
        uint64_t last_log_ns = start_ns;
        vmIosDebugLogf("[vm-mutex] wait begin vm=%p owner=%p mid=%d tid=%p",
                       (void*)vm, (void*)owner, id, (void*)pthread_self());
        while (true) {
            struct timespec pause_for = {0, 10000000L}; /* 10ms */
            nanosleep(&pause_for, NULL);
            int lock_rc = pthread_mutex_trylock(&m->handle);
            if (lock_rc == 0) {
                uint64_t waited_ms = (vmNowMonoNs() - start_ns) / 1000000ull;
                vmIosDebugLogf("[vm-mutex] wait end vm=%p mid=%d waited_ms=%" PRIu64 " tid=%p",
                               (void*)vm, id, waited_ms, (void*)pthread_self());
                return true;
            }
            if (lock_rc != EBUSY) {
                vmIosDebugLogf("[vm-mutex] trylock-loop failed vm=%p owner=%p mid=%d rc=%d",
                               (void*)vm, (void*)owner, id, lock_rc);
                return false;
            }

            uint64_t now_ns = vmNowMonoNs();
            if (now_ns >= last_log_ns + 1000000000ull) {
                uint64_t waited_ms = (now_ns - start_ns) / 1000000ull;
                vmIosDebugLogf("[vm-mutex] wait heartbeat vm=%p mid=%d waited_ms=%" PRIu64 " tid=%p",
                               (void*)vm, id, waited_ms, (void*)pthread_self());
                last_log_ns = now_ns;
            }
        }
    }
#endif
    return pthread_mutex_lock(&m->handle) == 0;
}

static bool unlockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return pthread_mutex_unlock(&m->handle) == 0;
}

// Permanently frees a mutex created by mutex()/rcmutex(), making its ID unusable.
static bool destroyMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    if (pthread_mutex_destroy(&owner->mutexes[id].handle) != 0) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    owner->mutexes[id].active = false;

    // If this was the highest-index mutex, shrink the count so new mutexes can reuse slots.
    while (owner->mutexCount > 0 && !owner->mutexes[owner->mutexCount - 1].active) {
        owner->mutexCount--;

    }
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return true;
}

// Internal function shared by stack dump helpers
// Most operand-stack values a dump prints. Runaway recursion leaves hundreds
// of thousands on the stack (11 MB of stderr, printed twice); the top of the
// stack is where the error happened.
#define VM_STACK_DUMP_MAX_SLOTS 32

static void vmDumpStackInternal(VM* vm, bool detailed) {
    if (!vm) return;

    Value* first = vm->stack;
    ptrdiff_t depth = vm->stackTop - vm->stack;
    if (depth > VM_STACK_DUMP_MAX_SLOTS) {
        first = vm->stackTop - VM_STACK_DUMP_MAX_SLOTS;
        if (detailed) {
            fprintf(stderr, "  ... %td older value(s) not shown\n", depth - VM_STACK_DUMP_MAX_SLOTS);
        } else {
            fprintf(stderr, "[... %td more] ", depth - VM_STACK_DUMP_MAX_SLOTS);
        }
    }

    if (detailed) {
        for (Value* slot = first; slot < vm->stackTop; slot++) {
            fprintf(stderr, "  [ ");
            printValueToStream(*slot, stderr);
            fprintf(stderr, " ]\n");
        }
    } else {
        for (Value* slot = first; slot < vm->stackTop; slot++) {
            fprintf(stderr, "[");
            printValueToStream(*slot, stderr);
            fprintf(stderr, "] ");
        }
        fprintf(stderr, "\n");
    }
}

static void assignRealToIntChecked(VM* vm, Value* dest, long double real_val) {
    bool range_error = false;
    // NaN fails no range test below, and converting it to an integer is
    // undefined: store 0 and report it like any other out-of-range value.
    bool was_nan = isnan(real_val) && VALUE_TYPE(*dest) != TYPE_BOOLEAN;
    if (was_nan) {
        range_error = true;
        real_val = 0.0L;
    }
    switch (VALUE_TYPE(*dest)) {
        case TYPE_BOOLEAN: {
            long long tmp = (real_val != 0.0L) ? 1 : 0;
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_CHAR: {
            int tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0;
            } else if (real_val > (long double)UCHAR_MAX) {
                range_error = true;
                tmp = UCHAR_MAX;
            } else {
                tmp = (int)real_val;
            }
            SET_CHAR_VALUE(dest, tmp);
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT8:
        case TYPE_BYTE: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT8_MAX) {
                range_error = true;
                tmp = UINT8_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT8: {
            long long tmp;
            if (real_val < (long double)INT8_MIN) {
                range_error = true;
                tmp = INT8_MIN;
            } else if (real_val > (long double)INT8_MAX) {
                range_error = true;
                tmp = INT8_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT16:
        case TYPE_WORD: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT16_MAX) {
                range_error = true;
                tmp = UINT16_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT16: {
            long long tmp;
            if (real_val < (long double)INT16_MIN) {
                range_error = true;
                tmp = INT16_MIN;
            } else if (real_val > (long double)INT16_MAX) {
                range_error = true;
                tmp = INT16_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT32: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT32_MAX) {
                range_error = true;
                tmp = UINT32_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT32: {
            long long tmp;
            if (real_val < (long double)INT32_MIN) {
                range_error = true;
                tmp = INT32_MIN;
            } else if (real_val > (long double)INT32_MAX) {
                range_error = true;
                tmp = INT32_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT64: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val >= 18446744073709551616.0L) {
                // 2^64: (long double)UINT64_MAX rounds up to it where long
                // double is double, so `>` let 2^64 itself through.
                range_error = true;
                tmp = UINT64_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            // VM 2.0 Phase 4i checkpoint 3d: this branch used to write
            // VAL_UINT/VAL_INT directly with deliberately asymmetric
            // semantics (VAL_UINT exact, VAL_INT clamped to LLONG_MAX) --
            // representable back when they were two independent fields.
            // Now that both decode from the SAME single boxed value,
            // that asymmetry no longer exists to preserve: SET_INT_VALUE
            // bit-reinterprets `tmp`, matching every other case here.
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT64: {
            long long tmp;
            if (real_val < (long double)LLONG_MIN) {
                range_error = true;
                tmp = LLONG_MIN;
            } else if (real_val >= 9223372036854775808.0L) {
                // 2^63, for the same reason as UINT64 above.
                range_error = true;
                tmp = LLONG_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        default: {
            long long tmp;
            if (real_val < (long double)LLONG_MIN) {
                range_error = true;
                tmp = LLONG_MIN;
            } else if (real_val >= 9223372036854775808.0L) {
                range_error = true;
                tmp = LLONG_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
    }
    if (was_nan) {
        runtimeWarning(vm, "Warning: Range check error assigning REAL NaN to %s.",
                       varTypeToString(VALUE_TYPE(*dest)));
    } else if (range_error) {
        runtimeWarning(vm, "Warning: Range check error assigning REAL %Lf to %s.",
                       real_val, varTypeToString(VALUE_TYPE(*dest)));
    }
}

static bool g_suppress_vm_state_dump = false;

void vmDumpStackInfoDetailed(VM* vm, const char* context_message) {
#if defined(PSCAL_TARGET_IOS)
    (void)vm;
    (void)context_message;
    return;
#endif
    const char *force_dump = getenv("PSCAL_VM_DUMP");
    if (g_suppress_vm_state_dump) {
        if (!force_dump || *force_dump == '\0' || *force_dump == '0') {
            return;
        }
    }
    if (!vm) return; // Safety check

    fprintf(stderr, "\n--- VM State Dump (%s) ---\n", context_message ? context_message : "Runtime Context");
    ptrdiff_t stack_size = vm->stackTop - vm->stack;
    fprintf(stderr, "Stack Size: %td, Frame Count: %d\n", stack_size, vm->frameCount);
    fprintf(stderr, "Stack Contents (bottom to top):\n");
    vmDumpStackInternal(vm, true);
    fprintf(stderr, "--------------------------\n");
}

// --- Helper function to dump stack and frame info ---
void vmDumpStackInfo(VM* vm) {
    long current_offset = vm->ip - vm->chunk->code;
    int line = (current_offset > 0 && current_offset <= vm->chunk->count) ? vm->chunk->lines[current_offset - 1] : 0;

    ptrdiff_t stack_size = vm->stackTop - vm->stack;
    fprintf(stderr, "[VM_DEBUG] Offset: %04ld, Line: %4d, Stack Size: %td, Frame Count: %d\n",
            current_offset, line, stack_size, vm->frameCount);

    // Disassemble and print the current instruction
    if (current_offset < vm->chunk->count) {
        int disasm_offset;
        if (current_offset < 0) {
            disasm_offset = 0;
        } else if (current_offset > INT_MAX) {
            disasm_offset = INT_MAX;
        } else {
            disasm_offset = (int)current_offset;
        }
        disassembleInstruction(vm->chunk, disasm_offset, vm->procedureTable);
    } else {
        fprintf(stderr, "         (End of bytecode or invalid offset)\n");
    }

    // Print stack contents for more detailed debugging:
    fprintf(stderr, "[VM_DEBUG] Stack Contents: ");
    vmDumpStackInternal(vm, false);
}

void vmSetSuppressStateDump(bool suppress) {
    g_suppress_vm_state_dump = suppress;
}

static bool vmSetContains(const Value* setVal, const Value* itemVal) {
    if (!setVal || VALUE_TYPE(*setVal) != TYPE_SET || !itemVal) {
        return false;
    }

    // The same ordinal mapping MAKE_SET_* used to build the set, so every
    // ordinal type (int64, shortint, cardinal, widechar, ...) tests
    // membership the way it was added.
    long long item_ord;
    if (!tryValueToOrdinal(itemVal, &item_ord)) return false; // Item is not of a type that can be in a set

    // Search for the ordinal value in the set's values array
    if (!AS_SET(*setVal).set_values) return false;
    for (int i = 0; i < AS_SET(*setVal).set_size; i++) {
        if (AS_SET(*setVal).set_values[i] == item_ord) {
            return true;
        }
    }
    return false;
}

static bool vmAddOrdinalToSet(Value* setVal, long long ordinal) {
    if (!setVal || VALUE_TYPE(*setVal) != TYPE_SET) {
        return false;
    }
    for (int i = 0; i < AS_SET(*setVal).set_size; i++) {
        if (AS_SET(*setVal).set_values[i] == ordinal) {
            return true;
        }
    }
    if (AS_SET(*setVal).set_size >= SET_CAPACITY(*setVal)) {
        int new_capacity = (SET_CAPACITY(*setVal) == 0) ? 8 : SET_CAPACITY(*setVal) * 2;
        long long* new_values = realloc(AS_SET(*setVal).set_values, sizeof(long long) * new_capacity);
        if (!new_values) {
            return false;
        }
        AS_SET(*setVal).set_values = new_values;
        SET_CAPACITY(*setVal) = new_capacity;
    }
    AS_SET(*setVal).set_values[AS_SET(*setVal).set_size++] = ordinal;
    return true;
}

static bool vmBuildSetFromOrdinal(Value* outSet, long long ordinal) {
    if (!outSet) {
        return false;
    }
    *outSet = makeValueForType(TYPE_SET, NULL, NULL);
    return vmAddOrdinalToSet(outSet, ordinal);
}

// Largest set a range constructor may build. A set is a flat array of
// ordinals, so `[0..maxint]` would otherwise try to allocate 16 GiB (and
// overflow SetObj's int size/capacity on the way).
#define VM_SET_RANGE_MAX_ELEMENTS (1u << 20)

typedef enum {
    VM_SET_RANGE_OK,
    VM_SET_RANGE_TOO_LARGE,
    VM_SET_RANGE_NO_MEMORY
} VmSetRangeStatus;

// `[start..end]`: every ordinal from start to end, and the empty set when
// start > end (ISO Pascal 6.7.1). A fresh range holds distinct ordinals, so
// it is sized once and filled directly rather than through
// vmAddOrdinalToSet's per-element duplicate scan. On failure *outSet is nil
// and owns nothing.
static VmSetRangeStatus vmBuildSetFromRange(Value* outSet, long long start_ord, long long end_ord) {
    *outSet = makeValueForType(TYPE_SET, NULL, NULL);
    if (start_ord > end_ord) {
        return VM_SET_RANGE_OK;
    }
    // end >= start, so the unsigned difference is exact even across the
    // whole long long range.
    unsigned long long span = (unsigned long long)end_ord - (unsigned long long)start_ord;
    if (span >= VM_SET_RANGE_MAX_ELEMENTS) {
        freeValue(outSet);
        *outSet = makeNil();
        return VM_SET_RANGE_TOO_LARGE;
    }
    size_t count = (size_t)span + 1;
    long long* values = (long long*)malloc(sizeof(long long) * count);
    if (!values) {
        freeValue(outSet);
        *outSet = makeNil();
        return VM_SET_RANGE_NO_MEMORY;
    }
    for (size_t i = 0; i < count; i++) {
        values[i] = start_ord + (long long)i;
    }
    free(AS_SET(*outSet).set_values);
    AS_SET(*outSet).set_values = values;
    AS_SET(*outSet).set_size = (int)count;
    SET_CAPACITY(*outSet) = (int)count;
    return VM_SET_RANGE_OK;
}

static int vmCompareOrdinals(const void* x, const void* y) {
    long long a = *(const long long*)x;
    long long b = *(const long long*)y;
    return (a > b) - (a < b);
}

// Is every member of `sub` also a member of `super`? Membership only, so
// the order the ordinals were added in does not matter. Small sets are
// compared pairwise; larger ones against a sorted copy of `super`.
// Returns -1 if that copy cannot be allocated.
static int vmSetIsSubset(const Value* sub, const Value* super) {
    const SetObj* s = PSCAL_VALUE_PTR(*sub, SetObj);
    const SetObj* t = PSCAL_VALUE_PTR(*super, SetObj);
    int s_size = (s && s->set_values) ? s->set_size : 0;
    int t_size = (t && t->set_values) ? t->set_size : 0;
    if (s_size == 0) {
        return 1;
    }
    if (t_size == 0) {
        return 0;
    }
    if ((long long)s_size * (long long)t_size <= 4096) {
        for (int i = 0; i < s_size; i++) {
            bool found = false;
            for (int j = 0; j < t_size; j++) {
                if (t->set_values[j] == s->set_values[i]) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                return 0;
            }
        }
        return 1;
    }
    long long* sorted = (long long*)malloc(sizeof(long long) * (size_t)t_size);
    if (!sorted) {
        return -1;
    }
    memcpy(sorted, t->set_values, sizeof(long long) * (size_t)t_size);
    qsort(sorted, (size_t)t_size, sizeof(long long), vmCompareOrdinals);
    int result = 1;
    for (int i = 0; i < s_size; i++) {
        if (!bsearch(&s->set_values[i], sorted, (size_t)t_size, sizeof(long long), vmCompareOrdinals)) {
            result = 0;
            break;
        }
    }
    free(sorted);
    return result;
}

// --- Integer arithmetic with a UINT64 operand ---
//
// asI64() reads a UINT64 as its two's-complement bit pattern, so a qword of
// 2^63 or more turns negative in signed arithmetic and compares. These
// helpers carry each operand as sign + magnitude instead: a UINT64 is never
// negative, and every other integer keeps the sign asI64() gives it.
typedef struct {
    bool neg;
    unsigned long long mag;
} VmWideInt;

static inline VmWideInt vmWideIntOf(Value v) {
    VmWideInt w;
    if (VALUE_TYPE(v) == TYPE_UINT64) {
        w.neg = false;
        w.mag = VAL_UINT(v);
        return w;
    }
    long long s = asI64(v);
    w.neg = s < 0;
    w.mag = w.neg ? 0ULL - (unsigned long long)s : (unsigned long long)s;
    return w;
}

// Three-way compare of two integer-like Values, exact over both the INT64
// and the UINT64 range.
static inline int vmCompareIntlike(Value a, Value b) {
    if (VALUE_TYPE(a) != TYPE_UINT64 && VALUE_TYPE(b) != TYPE_UINT64) {
        long long ia = asI64(a);
        long long ib = asI64(b);
        return (ia > ib) - (ia < ib);
    }
    VmWideInt x = vmWideIntOf(a);
    VmWideInt y = vmWideIntOf(b);
    if (x.neg != y.neg) {
        return x.neg ? -1 : 1;
    }
    int c = (x.mag > y.mag) - (x.mag < y.mag);
    return x.neg ? -c : c;
}

// Packs a result back into the 64-bit word a UINT64 result is stored in.
// Like every other integer op, the result only has to fit 64 bits read one
// way or the other, [INT64_MIN, UINT64_MAX]: a negative result wraps (qword
// 0 - 1 is high(qword)) the way a narrower unsigned type wraps at its store.
static inline bool vmWideIntToBits(VmWideInt r, unsigned long long* out) {
    if (r.mag == 0) {
        r.neg = false;
    }
    if (r.neg && r.mag > (1ULL << 63)) {
        return false;
    }
    *out = r.neg ? 0ULL - r.mag : r.mag;
    return true;
}

// a + b, a - b or a * b when either side is UINT64. False on overflow.
static bool vmUInt64Arith(int op, Value a, Value b, unsigned long long* out) {
    VmWideInt x = vmWideIntOf(a);
    VmWideInt y = vmWideIntOf(b);
    VmWideInt r = { false, 0 };
    if (op == SUBTRACT) {
        y.neg = !y.neg && y.mag != 0;
        op = ADD;
    }
    if (op == ADD) {
        if (x.neg == y.neg) {
            if (__builtin_add_overflow(x.mag, y.mag, &r.mag)) {
                return false;
            }
            r.neg = x.neg;
        } else if (x.mag >= y.mag) {
            r.mag = x.mag - y.mag;
            r.neg = x.neg;
        } else {
            r.mag = y.mag - x.mag;
            r.neg = y.neg;
        }
    } else if (op == MULTIPLY) {
        if (__builtin_mul_overflow(x.mag, y.mag, &r.mag)) {
            return false;
        }
        r.neg = x.neg != y.neg;
    } else {
        return false;
    }
    return vmWideIntToBits(r, out);
}

// a div b, or a mod b when want_mod, when either side is UINT64 (b != 0).
// Both truncate, as for the signed types: the quotient's sign combines the
// operands' signs and the remainder takes the dividend's. False on overflow
// (high(qword) div -1).
static bool vmUInt64DivMod(Value a, Value b, bool want_mod, unsigned long long* out) {
    VmWideInt x = vmWideIntOf(a);
    VmWideInt y = vmWideIntOf(b);
    VmWideInt r;
    if (want_mod) {
        r.mag = x.mag % y.mag;
        r.neg = x.neg;
    } else {
        r.mag = x.mag / y.mag;
        r.neg = x.neg != y.neg;
    }
    return vmWideIntToBits(r, out);
}

// Scans all global symbols and the entire VM value stack to find and nullify
// any pointers that are aliases of a memory address that is being disposed.
//
// The caller must hold `globals_mutex` before invoking this function to ensure
// thread-safe access to global interpreter state.
void vmNullifyAliases(VM* vm, uintptr_t disposedAddrValue) {
    // 1. Scan global symbols using the existing hash table helper
    if (vm->vmGlobalSymbols) {
        nullifyPointerAliasesByAddrValue(vm->vmGlobalSymbols, disposedAddrValue);
    }

    // 2. Scan the entire VM value stack for local variables and parameters
    //    across all active call frames.
    for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
        if (VALUE_TYPE(*slot) == TYPE_POINTER && (uintptr_t)AS_POINTER(*slot) == disposedAddrValue) {
            AS_POINTER(*slot) = NULL; // This is an alias, set it to nil.
        }
    }
}

static void computeRuntimeLocation(VM* vm, size_t* offset_out, int* line_out) {
    size_t instruction_offset = 0;
    int source_line = 0;

    if (vm && vm->chunk && vm->lastInstruction && vm->chunk->code && vm->chunk->lines) {
        if (vm->lastInstruction >= vm->chunk->code) {
            instruction_offset = (size_t)(vm->lastInstruction - vm->chunk->code);
            if (instruction_offset < (size_t)vm->chunk->count) {
                source_line = vm->chunk->lines[instruction_offset];
            }
        }
    } else if (vm && vm->chunk && vm->chunk->count > 0 && vm->chunk->lines) {
        instruction_offset = 0;
        source_line = vm->chunk->lines[0];
    }

    if (offset_out) {
        *offset_out = instruction_offset;
    }
    if (line_out) {
        *line_out = source_line;
    }
}

static void emitRuntimeLocation(VM* vm, const char* label) {
    size_t instruction_offset = 0;
    int source_line = 0;
    computeRuntimeLocation(vm, &instruction_offset, &source_line);

    if (!label) {
        label = "[Runtime Location]";
    }

    fprintf(stderr, "%s Offset: %zu, Line: %d\n", label, instruction_offset, source_line);
}

void runtimeWarning(VM* vm, const char* format, ...) {
    if (pscalRuntimeStdoutIsInteractive()) {
        fflush(stdout);
        resetTextAttributes(stdout);
        fflush(stdout);
    }
    if (pscalRuntimeStderrIsInteractive()) {
        resetTextAttributes(stderr);
    }

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    write(STDERR_FILENO, message, strlen(message));
    write(STDERR_FILENO, "\n", 1);

    if (s_vmVerboseErrors) {
        emitRuntimeLocation(vm, "[Warning Location]");
    }
}

/* Per-dimension bounds check for an indexed array access.
 *
 * computeFlatOffset() (core/utils.c) already bounds-checks every dimension, but
 * it takes only (Value*, int*) and has no VM pointer, so all it can do on a
 * violation is fprintf a bare line and EXIT_FAILURE_HANDLER():
 *
 *     Runtime error: Index 1 out of bounds [0..0] in dimension 1.
 *
 * No source path, no line, no diagnostic code -- and it exits before the VM's
 * own flat-offset check downstream can produce a located error. That cost a real
 * benchmark case: a generated edit-distance program wrote `dp[i][j-1]` where it
 * meant `row[j-1]` (row i is not appended to dp until after the inner loop), and
 * the message named none of the four dp[...] accesses in the function. Three
 * repair rounds failed to find a one-token bug because nothing said *where*.
 *
 * Checking here, where `vm` is in scope, lets the failure go through
 * runtimeError(), which prefixes `source:line:` when the chunk knows them.
 *
 * Mirrors computeFlatOffset()'s own loop exactly -- same descending dimension
 * order, same lower_bounds/upper_bounds arrays -- so it cannot disagree about
 * what is in bounds. Those per-dimension arrays are authoritative: ArrayObj also
 * carries singular lower_bound/upper_bound convenience fields, which are NOT
 * folded into element [0] (see core/types.h) and must not be used here.
 * computeFlatOffset()'s check stays as a backstop for its non-VM callers.
 */
static bool vmCheckArrayIndexBounds(VM* vm, Value* array_val_ptr, int* indices) {
    if (!array_val_ptr || !indices) return true;
    ArrayObj* a = PSCAL_VALUE_PTR(*array_val_ptr, ArrayObj);
    if (!a || !a->lower_bounds || !a->upper_bounds) return true;

    for (int i = a->dimensions - 1; i >= 0; i--) {
        int idx = indices[i];
        int lo = a->lower_bounds[i];
        int hi = a->upper_bounds[i];
        if (idx >= lo && idx <= hi) continue;

        /* Dimension numbering is 1-based here to match the long-standing
         * wording in computeFlatOffset(); the indices themselves keep whatever
         * base the array declares (Aether arrays are 0-based, but Pascal
         * frontends may declare any lower bound, which is why the valid range
         * is printed rather than assumed). */
        if (frontendIsAether()) {
            runtimeError(vm,
                         "[ARR-003] array index %d is out of bounds in dimension %d of %d: "
                         "valid indices are %d..%d.\n"
                         "hint: the index is past the end of what has been appended so far -- "
                         "check the loop bound, and whether you meant the row currently being "
                         "built rather than one already stored.\n"
                         "help: see ARR-003 in the Aether guide",
                         idx, i + 1, a->dimensions, lo, hi);
        } else {
            runtimeError(vm, "Index %d out of bounds [%d..%d] in dimension %d.",
                         idx, lo, hi, i + 1);
        }
        return false;
    }
    return true;
}

// SHL/SHR on 64-bit integers, defined for every count >= 0. A count of 64 or
// more shifts every bit out: SHL gives 0 and SHR the sign fill (0, or -1 for a
// negative signed value). The C operators are undefined there (`1 << 64` came
// back as 1 on arm64) and for a left shift of a negative value, so this shifts
// the unsigned bits. An unsigned left operand shifts right logically.
static inline long long vmShiftInt64(long long a, long long count, bool left, bool isUnsigned) {
    unsigned long long bits = (unsigned long long)a;
    if (left) return count >= 64 ? 0 : (long long)(bits << count);
    if (isUnsigned) return count >= 64 ? 0 : (long long)(bits >> count);
    if (count >= 64) return a < 0 ? -1 : 0;
    return a >> count;
}

// runtimeError - Assuming your existing one is fine.
// Instructions shown before the error point in a verbose runtime error.
#define VM_ERROR_RECENT_INSTRUCTIONS 5

// Offset of the instruction VM_ERROR_RECENT_INSTRUCTIONS before the one at
// `error_offset`, found by decoding forward from an instruction boundary
// (the current routine's entry, else the start of the chunk). Backing up a
// fixed number of bytes usually landed inside an operand and disassembled
// garbage. Returns error_offset when no boundary walk reaches it.
static int vmRecentInstructionStart(VM* vm, int error_offset) {
    if (!vm->chunk || error_offset <= 0 || error_offset > vm->chunk->count) {
        return error_offset;
    }
    int entries[2] = { 0, 0 };
    int entry_count = 0;
    CallFrame* frame = (vm->frameCount > 0) ? &vm->frames[vm->frameCount - 1] : NULL;
    if (frame && frame->function_symbol && frame->function_symbol->bytecode_address > 0 &&
        frame->function_symbol->bytecode_address < error_offset) {
        entries[entry_count++] = frame->function_symbol->bytecode_address;
    }
    entries[entry_count++] = 0;
    for (int e = 0; e < entry_count; e++) {
        int recent[VM_ERROR_RECENT_INSTRUCTIONS];
        int seen = 0;
        int offset = entries[e];
        while (offset < error_offset) {
            recent[seen % VM_ERROR_RECENT_INSTRUCTIONS] = offset;
            seen++;
            int length = getInstructionLength(vm->chunk, offset);
            if (length <= 0) {
                break;
            }
            offset += length;
        }
        if (offset == error_offset && seen > 0) {
            return (seen < VM_ERROR_RECENT_INSTRUCTIONS) ? recent[0]
                                                         : recent[seen % VM_ERROR_RECENT_INSTRUCTIONS];
        }
    }
    return error_offset;
}

void runtimeError(VM* vm, const char* format, ...) {
    if (vm) {
        vm->abort_requested = true;
        // A handler that reports an error without returning (push()/pop()/
        // peek() cannot return one) must still stop the program: the
        // dispatch loop checks this before the next instruction. The abort
        // flag alone can't tell it so, because interrupts and exits set it
        // too, and those unwind a single frame and carry on.
        vm->runtime_error_raised = true;
    }

#ifdef SDL
    if (sdlIsGraphicsActive()) {
        cleanupSdlWindowResources();
    }
#endif

    if (pscalRuntimeStdoutIsInteractive()) {
        fflush(stdout);
        resetTextAttributes(stdout);
        fflush(stdout);
    }
    if (pscalRuntimeStderrIsInteractive()) {
        resetTextAttributes(stderr);
    }

    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    size_t instruction_offset = 0;
    int error_line = 0;
    bool have_runtime_location = false;
    if (vm) {
        computeRuntimeLocation(vm, &instruction_offset, &error_line);
        if (vm->chunk && vm->chunk->source_path && error_line > 0) {
            fprintf(stderr, "%s:%d: %s\n",
                    bytecodeDisplayNameForPath(vm->chunk->source_path),
                    error_line,
                    message);
        } else {
            write(STDERR_FILENO, message, strlen(message));
            write(STDERR_FILENO, "\n", 1);
        }
#if !defined(PSCAL_TARGET_IOS)
        fprintf(stderr, "[Error Location] Offset: %zu, Line: %d\n", instruction_offset, error_line);
#endif
        have_runtime_location = true;
    } else {
        write(STDERR_FILENO, message, strlen(message));
        write(STDERR_FILENO, "\n", 1);
    }

    if (s_vmVerboseErrors && vm) {
        fprintf(stderr, "\n--- VM Crash Context ---\n");
        fprintf(stderr, "Instruction Pointer (IP): %p\n", (void*)vm->ip);
        fprintf(stderr, "Code Base: %p\n", vm->chunk ? (void*)vm->chunk->code : (void*)NULL);

        fprintf(stderr, "Current Instruction (at IP, might be the instruction that IP tried to fetch/decode):\n");
        if (vm->chunk && vm->ip >= vm->chunk->code && (vm->ip - vm->chunk->code) < vm->chunk->count) {
            disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code), vm->procedureTable);
        } else {
            fprintf(stderr, "  (IP is out of bytecode bounds: %p)\n", (void*)vm->ip);
        }

        int start_dump_offset = have_runtime_location
                                    ? vmRecentInstructionStart(vm, (int)instruction_offset)
                                    : (int)instruction_offset;

        fprintf(stderr, "\nLast Instructions executed (leading to crash, up to %d before error point):\n",
                VM_ERROR_RECENT_INSTRUCTIONS);
        if (vm->chunk) {
            for (int offset = start_dump_offset; offset < (int)instruction_offset; ) {
                offset = disassembleInstruction(vm->chunk, offset, vm->procedureTable);
            }
        }
        if (start_dump_offset == (int)instruction_offset) {
            fprintf(stderr, "  (No preceding instructions in buffer to display)\n");
        }

        vmDumpStackInfoDetailed(vm, "Full Stack at Crash");
    }

    // resetStack(vm); // Keep this commented out for post-mortem analysis purposes if you want to inspect stack in debugger
    // ... (rest of runtimeError function, calls EXIT_FAILURE_HANDLER()) ...
}

// The body of copyValueForStack without the lock, for sources no other
// thread can replace while we read them: the chunk's constant pool (never
// written after load) and this VM's own operand stack (DUP).
static Value copyValueForStackUnlocked(const Value* src) {
    if (!src) {
        return makeNil();
    }

    switch (VALUE_TYPE(*src)) {
        case TYPE_VOID:
        case TYPE_INT32:
        case TYPE_DOUBLE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_FLOAT:
        case TYPE_NIL:
            return *src;
        // TYPE_LONG_DOUBLE owns a LongDoubleBox: a bitwise `Value copy =
        // *src;` would alias the SAME box as `*src` with no retain, and both
        // eventually being freed independently double-frees it (confirmed by
        // a real heap-use-after-free under ASan when TYPE_INT64 was boxed the
        // same way). Falls through to makeCopyOfValue's TYPE_LONG_DOUBLE
        // case, which allocates a fresh box per copy.
        default:
            break;
    }

    if (VALUE_TYPE(*src) == TYPE_MEMORYSTREAM) {
        Value alias = *src;
        if (AS_MSTREAM(alias)) {
            retainMStream(AS_MSTREAM(alias));
        }
        return alias;
    }

    // VM 2.0 Phase 5a checkpoint 5a-i: same shape as TYPE_MEMORYSTREAM above
    // -- a Task's identity IS the shared pool slot, so DUP/CONSTANT/push's
    // fast path always retains-and-shares, never clones (there is no
    // valueEnsureUnique()/clone-on-write path for TYPE_TASK at all, unlike
    // STRING/SET/RECORD/ENUM/ARRAY below -- a task has no in-place-mutable
    // contents to protect).
    if (VALUE_TYPE(*src) == TYPE_TASK) {
        Value alias = *src;
        if (AS_TASK(alias)) {
            pscalObjRetain(&AS_TASK(alias)->header);
        }
        return alias;
    }

    // VM 2.0 Phase 4j (Stage B): STRING/SET/RECORD/ENUM/ARRAY used to fall
    // through to makeCopyOfValue's deep clone unconditionally (a fresh
    // wrapper + a fresh copy of its contents on EVERY read of a variable
    // holding one of these -- correct value semantics, but far more
    // allocation than necessary for the common case of "read it, maybe
    // pass it somewhere, never mutate it"). Now a cheap alias + retain,
    // matching TYPE_MEMORYSTREAM's shape above; any actual in-place
    // mutation (GET_ELEMENT_ADDRESS/GET_FIELD_ADDRESS* and the
    // string-char-address opcodes -- the only places that take an address
    // into one of these payloads, since reads use LOAD_ELEMENT_VALUE/
    // LOAD_FIELD_VALUE* and never touch a raw address at all) calls
    // valueEnsureUnique() first to clone-on-write if the object turns out
    // to still be shared at that point. TYPE_ARRAY needs no is_dynamic
    // branch here -- retain-sharing on COPY is correct and desired for
    // both static and dynamic arrays; the static-vs-dynamic distinction
    // only matters at valueEnsureUnique()'s WRITE-time choke point.
    switch (VALUE_TYPE(*src)) {
        case TYPE_STRING:
        case TYPE_UNICODE_STRING: {
            Value alias = *src;
            StringObj *s = PSCAL_VALUE_PTR(alias, StringObj);
            if (s) pscalObjRetain(&s->header);
            return alias;
        }
        case TYPE_SET: {
            Value alias = *src;
            SetObj *s = PSCAL_VALUE_PTR(alias, SetObj);
            if (s) pscalObjRetain(&s->header);
            return alias;
        }
        case TYPE_RECORD: {
            Value alias = *src;
            RecordObj *r = PSCAL_VALUE_PTR(alias, RecordObj);
            if (r) pscalObjRetain(&r->header);
            return alias;
        }
        case TYPE_ENUM: {
            Value alias = *src;
            EnumObj *e = PSCAL_VALUE_PTR(alias, EnumObj);
            if (e) pscalObjRetain(&e->header);
            return alias;
        }
        case TYPE_ARRAY: {
            Value alias = *src;
            ArrayObj *a = PSCAL_VALUE_PTR(alias, ArrayObj);
            if (a) pscalObjRetain(&a->header);
            return alias;
        }
        case TYPE_MAP: {
            // Aether maps (D68) are copy-on-write values: share here, clone
            // in valueEnsureUnique before any write.
            Value alias = *src;
            MapObj *m = PSCAL_VALUE_PTR(alias, MapObj);
            if (m) pscalObjRetain(&m->header);
            return alias;
        }
        default:
            break;
    }

    // VM 2.0 Phase 4i checkpoint 3a: TYPE_CLOSURE used to have its own
    // fast-path here identical in shape to TYPE_MEMORYSTREAM's above
    // (`Value alias = *src;` then retain the shared resource) -- correct
    // when `.closure` was inline data (env was the only owned resource),
    // WRONG now that ClosureObj is itself a plain, singly-owned heap
    // allocation with no refcount of its own: `alias.closure` ended up
    // aliasing the SAME ClosureObj* as `*src`, and both eventually being
    // freed independently double-freed it (confirmed by a real crash --
    // a closure captured, stored, and invoked twice). makeCopyOfValue's
    // TYPE_CLOSURE case (below, via the generic fallthrough) already
    // does this correctly: allocates a fresh wrapper, copies the plain
    // fields, and retains only the actually-shared `env`.

    Value copy = makeCopyOfValue(src);
    return copy;
}

// Copies a cell's value for pushing onto the operand stack: scalars by
// value, shared payloads by retain (copy-on-write), the rest via
// makeCopyOfValue. Use this for cells another thread can replace
// (locals reachable through VAR params and captures, globals, heap cells):
// it holds value_cell_mutex so the copy never races replaceValueCell.
static Value copyValueForStack(const Value* src) {
    if (!src) {
        return makeNil();
    }

    // Scalars own no heap data, so they need no lock: read the cell once
    // and decide on that snapshot. Re-reading *src after the type test could
    // pair a scalar type with a racing writer's heap pointer, which would
    // then escape unretained. A torn snapshot whose type is a scalar is at
    // worst a wrong number in a program that races on the variable.
    Value snapshot = *src;
    if (vmTypeIsInlineScalar(VALUE_TYPE(snapshot))) {
        return snapshot;
    }

    pthread_mutex_lock(&value_cell_mutex);
    Value copy = copyValueForStackUnlocked(src);
    pthread_mutex_unlock(&value_cell_mutex);
    return copy;
}

static void push(VM* vm, Value value) { // Using your original name 'push'
    size_t used = (size_t)(vm->stackTop - vm->stack);
    if (used >= vm->stackCommittedValues && !vmGrowStackStorage(vm, used + 1)) {
        runtimeError(vm, "VM Error: Stack overflow.");
        return;
    }
    *vm->stackTop = value;
    vm->stackTop++;
}

static Value copyInterfaceReceiverAlias(Value* receiverCell) {
    Value alias = copyValueForStack(receiverCell);
    if (VALUE_TYPE(alias) == TYPE_POINTER && PTR_BASE_TYPE_NODE(alias) == OWNED_POINTER_SENTINEL) {
        PTR_BASE_TYPE_NODE(alias) = NULL;
    }
    return alias;
}

static Symbol* findProcedureByAddress(HashTable* table, uint32_t address) {
    if (!table) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (s->is_defined && s->bytecode_address == address) {
                return s;
            }
            if (s->type_def && s->type_def->symbol_table) {
                Symbol* nested = findProcedureByAddress((HashTable*)s->type_def->symbol_table, address);
                if (nested) return nested;
            }
        }
    }
    return NULL;
}

static Symbol* resolveProcedureAlias(Symbol* symbol) {
    if (symbol && symbol->is_alias && symbol->real_symbol) {
        return symbol->real_symbol;
    }
    return symbol;
}

static void populateProcedureCacheFromTable(VM* vm, HashTable* table) {
    if (!vm || !table || !vm->procedureByAddress) {
        return;
    }

    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol* entry = table->buckets[i]; entry; entry = entry->next) {
            Symbol* resolved = resolveProcedureAlias(entry);
            if (resolved && resolved->is_defined && resolved->bytecode_address >= 0) {
                size_t address = (size_t)resolved->bytecode_address;
                if (address < vm->procedureByAddressSize) {
                    vm->procedureByAddress[address] = resolved;
                }
            }
            if (entry->type_def && entry->type_def->symbol_table) {
                populateProcedureCacheFromTable(vm, (HashTable*)entry->type_def->symbol_table);
            }
        }
    }
}

static void vmPopulateProcedureAddressCache(VM* vm) {
    if (!vm) return;

    if (!vm->chunk || !vm->procedureTable) {
        if (vm->procedureByAddress && vm->procedureByAddressSize > 0) {
            memset(vm->procedureByAddress, 0, vm->procedureByAddressSize * sizeof(Symbol*));
        }
        return;
    }

    size_t requiredSize = (size_t)vm->chunk->count;
    if (requiredSize == 0) {
        if (vm->procedureByAddress && vm->procedureByAddressSize > 0) {
            memset(vm->procedureByAddress, 0, vm->procedureByAddressSize * sizeof(Symbol*));
        }
        return;
    }

    if (vm->procedureByAddressSize < requiredSize) {
        Symbol** newCache = calloc(requiredSize, sizeof(Symbol*));
        if (!newCache) {
            return;
        }
        if (vm->procedureByAddress) {
            free(vm->procedureByAddress);
        }
        vm->procedureByAddress = newCache;
        vm->procedureByAddressSize = requiredSize;
    } else {
        memset(vm->procedureByAddress, 0, vm->procedureByAddressSize * sizeof(Symbol*));
    }

    populateProcedureCacheFromTable(vm, vm->procedureTable);
}

static Symbol* vmGetProcedureByAddress(VM* vm, uint32_t address) {
    if (!vm) return NULL;

    Symbol* symbol = NULL;
    if (vm->procedureByAddress && (size_t)address < vm->procedureByAddressSize) {
        symbol = vm->procedureByAddress[address];
    }

    if (!symbol) {
        symbol = findProcedureByAddress(vm->procedureTable, address);
    }

    Symbol* resolved = resolveProcedureAlias(symbol);
    if (resolved && vm->procedureByAddress && (size_t)address < vm->procedureByAddressSize) {
        vm->procedureByAddress[address] = resolved;
    }
    return resolved;
}

static bool procedureVisibleFromFrames(VM* vm, Symbol* symbol) {
    if (!symbol) return false;
    if (!symbol->enclosing) return true;
    if (!vm) return false;

    for (int fi = vm->frameCount - 1; fi >= 0; fi--) {
        Symbol* frame_symbol = vm->frames[fi].function_symbol;
        while (frame_symbol) {
            if (frame_symbol == symbol->enclosing) {
                return true;
            }
            frame_symbol = frame_symbol->enclosing;
        }
    }
    return false;
}

static Symbol* findProcedureByName(HashTable* table, const char* lookup_name, VM* vm) {
    if (!table || !lookup_name) return NULL;

    Symbol* sym = resolveProcedureAlias(hashTableLookup(table, lookup_name));
    if (sym && procedureVisibleFromFrames(vm, sym)) {
        return sym;
    }

    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol* entry = table->buckets[i]; entry; entry = entry->next) {
            if (entry->type_def && entry->type_def->symbol_table) {
                Symbol* nested = findProcedureByName((HashTable*)entry->type_def->symbol_table, lookup_name, vm);
                if (nested) {
                    return nested;
                }
            }
        }
    }

    return NULL;
}

// Resolves a CALL_USER_PROC callee the way the source scopes it: routines
// nested in the calling routine first, then those nested in each enclosing
// routine outward, then the global table. findProcedureByName (global table
// first, then any nested routine whose parent is anywhere on the call
// stack) stays as the last resort; on its own it ran the global routine
// instead of a nested one that shadows it, and a sibling routine's nested
// helper instead of the caller's own.
static Symbol* vmResolveUserProc(VM* vm, const char* lookup_name) {
    if (vm->frameCount > 0) {
        for (Symbol* scope = vm->frames[vm->frameCount - 1].function_symbol;
             scope; scope = scope->enclosing) {
            HashTable* nested = (scope->type_def && scope->type_def->symbol_table)
                                    ? (HashTable*)scope->type_def->symbol_table
                                    : NULL;
            Symbol* hit = nested ? resolveProcedureAlias(hashTableLookup(nested, lookup_name)) : NULL;
            if (hit) {
                return hit;
            }
        }
    }
    Symbol* global = resolveProcedureAlias(hashTableLookup(vm->procedureTable, lookup_name));
    if (global) {
        return global;
    }
    return findProcedureByName(vm->procedureTable, lookup_name, vm);
}

static void vmFreeUserProcSiteCache(VM* vm) {
    free(vm->userProcBySite);
    vm->userProcBySite = NULL;
    vm->userProcBySiteSize = 0;
    vm->userProcBySiteChunk = NULL;
}

// Lexical resolution depends only on where the call is, so each call site's
// callee is looked up once per chunk instead of on every call (a name copy,
// lowercase and hash lookup each time, and for a nested callee a scan of
// every procedure table).
static Symbol* vmUserProcSiteLookup(VM* vm, size_t site) {
    if (vm->userProcBySiteChunk != vm->chunk || site >= vm->userProcBySiteSize) {
        return NULL;
    }
    return vm->userProcBySite[site];
}

static void vmUserProcSiteStore(VM* vm, size_t site, Symbol* symbol) {
    if (!vm->chunk || vm->chunk->count <= 0) {
        return;
    }
    if (vm->userProcBySiteChunk != vm->chunk ||
        vm->userProcBySiteSize != (size_t)vm->chunk->count) {
        vmFreeUserProcSiteCache(vm);
        vm->userProcBySite = (Symbol**)calloc((size_t)vm->chunk->count, sizeof(Symbol*));
        if (!vm->userProcBySite) {
            return;
        }
        vm->userProcBySiteSize = (size_t)vm->chunk->count;
        vm->userProcBySiteChunk = vm->chunk;
    }
    if (site < vm->userProcBySiteSize) {
        vm->userProcBySite[site] = symbol;
    }
}

// push()/pop()/peek() can't return an error, so on a fault they report and
// hand back nil (pop/peek) or drop the value (push). The handler finishes on
// that and the dispatch loop stops before the next instruction (see
// runtime_error_raised).
static Value pop(VM* vm) {
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow (pop from empty stack).");
        return makeNil();
    }
    vm->stackTop--;
    Value result = *vm->stackTop; // Make a copy of the value to return.

    // Mark the slot as vacant without incurring the cost of makeNil(). This
    // needs a raw bits write, not AS_POINTER(*vm->stackTop) = NULL: that
    // macro now dereferences the payload as a PointerObj* (VM 2.0 Phase
    // 4g), but the vacated slot's payload could have held ANY type before
    // this (an EnumObj*, ArrayObj*, etc, now stale garbage) --
    // dereferencing it as a pointer wrapper would crash. The actual intent
    // here has always been "zero the raw payload bits," which pre-4g/4i
    // happened to be spelled the same as "clear the pointer payload"
    // (ptr_val was a plain field then); post-collapse those are different
    // operations, and this one wants the former.
    SET_VALUE_TYPE(vm->stackTop, TYPE_VOID);
    pscalValueSetHeapPtrBits(vm->stackTop, NULL);

    return result; // Return the copy, which the caller is now responsible for.
}
static Value peek(VM* vm, int distance) { // Using your original name 'peek'
    if (vm->stackTop - vm->stack < distance + 1) {
        runtimeError(vm, "VM Error: Stack underflow (peek too deep).");
        return makeNil();
    }
    return vm->stackTop[-(distance + 1)];
}

// --- Host Function C Implementations ---
static Value vmHostQuitRequested(VM* vm) {
    // break_requested is an atomic flag from globals.h/globals.c
    // makeBoolean is from core/utils.h
    return makeBoolean(atomic_load(&break_requested));
}

static Value vmHostCreateThreadAddr(VM* vm) {
    // New layout: [addr, arg0, arg1, ..., argc] — argc on top.
    Value argcVal = pop(vm);
    if (IS_INTLIKE(argcVal)) {
        int argc = (int)AS_INTEGER(argcVal);
        if (argc < 0) argc = 0;
        int totalArgs = argc;
        Value args[8];
        if (argc > 8) argc = 8;
        for (int i = totalArgs - 1; i >= 0; i--) {
            Value v = pop(vm);
            if (i < 8) {
                args[i] = v;
            } else {
                freeValue(&v);
            }
        }
        Value addrVal = pop(vm);
        uint32_t entry = 0;
        bool validEntry = false;
        ClosureEnvPayload* closureEnv = NULL;
        Symbol* closureSymbol = NULL;
        if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
            entry = (uint32_t)AS_CLOSURE(addrVal).entry_offset;
            closureEnv = AS_CLOSURE(addrVal).env;
            closureSymbol = AS_CLOSURE(addrVal).symbol;
            if (closureEnv) {
                retainClosureEnv(closureEnv);
            }
            validEntry = true;
        } else if (VALUE_TYPE(addrVal) == TYPE_STRING && AS_STRING(addrVal)) {
            char lookup_name[MAX_SYMBOL_LENGTH + 1];
            strncpy(lookup_name, AS_STRING(addrVal), MAX_SYMBOL_LENGTH);
            lookup_name[MAX_SYMBOL_LENGTH] = '\0';
            toLowerString(lookup_name);
            Symbol* proc_symbol = findProcedureByName(vm->procedureTable, lookup_name, vm);
            if (proc_symbol && proc_symbol->is_defined && proc_symbol->bytecode_address >= 0) {
                entry = (uint32_t)proc_symbol->bytecode_address;
                closureSymbol = proc_symbol;
                validEntry = true;
            }
        } else if (IS_INTLIKE(addrVal)) {
            entry = (uint32_t)AS_INTEGER(addrVal);
            validEntry = true;
        }
        freeValue(&addrVal);

        if (!validEntry) {
            for (int i = 0; i < argc && i < 8; ++i) {
                freeValue(&args[i]);
            }
            if (closureEnv) {
                releaseClosureEnv(closureEnv);
            }
            runtimeError(vm, "VM Error: CreateThread requires a procedure pointer or closure.");
            freeValue(&argcVal);
            return makeInt(-1);
        }

        int id = createThreadWithArgs(vm, entry, closureEnv, closureSymbol, argc, args);
        if (closureEnv) {
            releaseClosureEnv(closureEnv);
        }
        return makeInt(id < 0 ? -1 : id);
    } else {
        // Backwards-compatible path: [addr, arg]
        Value argVal = argcVal; // already popped
        Value addrVal = pop(vm);
        uint32_t entry = 0;
        bool validEntry = false;
        ClosureEnvPayload* closureEnv = NULL;
        Symbol* closureSymbol = NULL;
        if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
            entry = (uint32_t)AS_CLOSURE(addrVal).entry_offset;
            closureEnv = AS_CLOSURE(addrVal).env;
            closureSymbol = AS_CLOSURE(addrVal).symbol;
            if (closureEnv) {
                retainClosureEnv(closureEnv);
            }
            validEntry = true;
        } else if (VALUE_TYPE(addrVal) == TYPE_STRING && AS_STRING(addrVal)) {
            char lookup_name[MAX_SYMBOL_LENGTH + 1];
            strncpy(lookup_name, AS_STRING(addrVal), MAX_SYMBOL_LENGTH);
            lookup_name[MAX_SYMBOL_LENGTH] = '\0';
            toLowerString(lookup_name);
            Symbol* proc_symbol = findProcedureByName(vm->procedureTable, lookup_name, vm);
            if (proc_symbol && proc_symbol->is_defined && proc_symbol->bytecode_address >= 0) {
                entry = (uint32_t)proc_symbol->bytecode_address;
                closureSymbol = proc_symbol;
                validEntry = true;
            }
        } else if (IS_INTLIKE(addrVal)) {
            entry = (uint32_t)AS_INTEGER(addrVal);
            validEntry = true;
        }
        freeValue(&addrVal);

        if (!validEntry) {
            runtimeError(vm, "VM Error: CreateThread requires a procedure pointer or closure.");
            freeValue(&argVal);
            if (closureEnv) {
                releaseClosureEnv(closureEnv);
            }
            return makeInt(-1);
        }

        int id = createThreadWithArgs(vm, entry, closureEnv, closureSymbol, 1, &argVal);
        if (closureEnv) {
            releaseClosureEnv(closureEnv);
        }
        return makeInt(id < 0 ? -1 : id);
    }
}

// VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1):
// TaskSpawn's entry-resolution logic -- the exact same closure/procedure-
// name/raw-offset dispatch vmHostCreateThreadAddr above uses, over
// createThreadWithArgs (the SAME createThreadJob/ThreadJobQueue path, not a
// second pool). Unlike vmHostCreateThreadAddr (a CALL_HOST target that pops
// a variable number of stack args into a fixed [8] local array), this takes
// `fnVal` and an already-materialized `argv` array directly, matching a
// VmBuiltinFn's calling convention -- no 8-arg cap, since that cap was an
// artifact of the stack-popping shape, not of createThreadWithArgs itself.
// Does not free/consume fnVal or argv -- the caller (a builtin) owns that
// via its normal argument-cleanup path. Returns the new thread id, or -1
// with runtimeError already raised.
int vmHostCreateTaskEntry(VM* vm, Value fnVal, int argc, const Value* argv) {
    if (!vm) {
        return -1;
    }
    uint32_t entry = 0;
    bool validEntry = false;
    ClosureEnvPayload* closureEnv = NULL;
    Symbol* closureSymbol = NULL;
    if (VALUE_TYPE(fnVal) == TYPE_CLOSURE) {
        entry = (uint32_t)AS_CLOSURE(fnVal).entry_offset;
        closureEnv = AS_CLOSURE(fnVal).env;
        closureSymbol = AS_CLOSURE(fnVal).symbol;
        if (closureEnv) {
            retainClosureEnv(closureEnv);
        }
        validEntry = true;
    } else if (isPascalStringType(VALUE_TYPE(fnVal)) && AS_STRING(fnVal)) {
        char lookup_name[MAX_SYMBOL_LENGTH + 1];
        strncpy(lookup_name, AS_STRING(fnVal), MAX_SYMBOL_LENGTH);
        lookup_name[MAX_SYMBOL_LENGTH] = '\0';
        toLowerString(lookup_name);
        Symbol* proc_symbol = findProcedureByName(vm->procedureTable, lookup_name, vm);
        if (proc_symbol && proc_symbol->is_defined && proc_symbol->bytecode_address >= 0) {
            entry = (uint32_t)proc_symbol->bytecode_address;
            closureSymbol = proc_symbol;
            validEntry = true;
        }
    } else if (IS_INTLIKE(fnVal)) {
        entry = (uint32_t)AS_INTEGER(fnVal);
        validEntry = true;
    }

    if (!validEntry) {
        if (closureEnv) {
            releaseClosureEnv(closureEnv);
        }
        runtimeError(vm, "TaskSpawn requires a procedure pointer or closure.");
        return -1;
    }

    int id = createThreadWithArgs(vm, entry, closureEnv, closureSymbol, argc, argv);
    if (closureEnv) {
        releaseClosureEnv(closureEnv);
    }
    if (id < 0 && !vm->abort_requested) {
        runtimeError(vm, "Task limit exceeded.");
    }
    return id;
}

static Value vmHostCreateClosure(VM* vm) {
    Value entryVal = pop(vm);
    if (!IS_INTLIKE(entryVal)) {
        freeValue(&entryVal);
        runtimeError(vm, "VM Error: Closure creation requires integer entry address.");
        return makeNil();
    }
    uint32_t entry = (uint32_t)AS_INTEGER(entryVal);
    freeValue(&entryVal);

    Value countVal = pop(vm);
    int capture_count = 0;
    if (IS_INTLIKE(countVal)) {
        capture_count = (int)AS_INTEGER(countVal);
    }
    freeValue(&countVal);
    if (capture_count < 0) {
        runtimeError(vm, "VM Error: Closure capture count cannot be negative.");
        return makeNil();
    }

    Symbol* proc_symbol = vmGetProcedureByAddress(vm, entry);
    if (!proc_symbol) {
        for (int i = 0; i < capture_count; ++i) {
            Value discard = pop(vm);
            freeValue(&discard);
        }
        runtimeError(vm, "VM Error: Unknown procedure for closure entry %u.", entry);
        return makeNil();
    }

    if (capture_count != proc_symbol->upvalue_count) {
        for (int i = 0; i < capture_count; ++i) {
            Value discard = pop(vm);
            freeValue(&discard);
        }
        runtimeError(vm,
                     "VM Error: Closure capture count mismatch for '%s' (expected %d, got %d).",
                     proc_symbol->name ? proc_symbol->name : "<anonymous>",
                     proc_symbol->upvalue_count, capture_count);
        return makeNil();
    }

    ClosureEnvPayload* env = createClosureEnv((uint16_t)capture_count, TYPE_CLOSURE);
    env->symbol = proc_symbol;

    for (int i = capture_count - 1; i >= 0; --i) {
        Value captured = pop(vm);
        if (VALUE_TYPE(captured) != TYPE_POINTER || AS_POINTER(captured) == NULL) {
            freeValue(&captured);
            releaseClosureEnv(env);
            runtimeError(vm, "VM Error: Expected pointer for captured closure variable.");
            return makeNil();
        }
        if (proc_symbol->closure_escapes) {
            Value* cell = (Value*)malloc(sizeof(Value));
            if (!cell) {
                freeValue(&captured);
                releaseClosureEnv(env);
                runtimeError(vm, "VM Error: Out of memory initialising escaping closure environment.");
                return makeNil();
            }
            *cell = makeCopyOfValue((Value*)AS_POINTER(captured));
            env->slots[i] = cell;
        } else {
            env->slots[i] = (Value*)AS_POINTER(captured);
        }
        freeValue(&captured);
    }

    Value closure = makeClosure(entry, proc_symbol, env);
    releaseClosureEnv(env);
    return closure;
}

typedef struct RuntimeVTableEntry {
    char* className;
    Value* table;
    struct RuntimeVTableEntry* next;
} RuntimeVTableEntry;

// Interface method tables, one per "<class>|<interface>" pair, shared by
// every VM in the process (worker threads box interfaces too), so the list
// is only touched under runtimeVTablesMutex. An entry's table is never
// changed once built; interface values point straight at it.
static RuntimeVTableEntry* runtimeVTables = NULL;
static pthread_mutex_t runtimeVTablesMutex = PTHREAD_MUTEX_INITIALIZER;

static RuntimeVTableEntry* findRuntimeVTableEntry(const char* classNameLower) {
    for (RuntimeVTableEntry* entry = runtimeVTables; entry; entry = entry->next) {
        if (entry->className && strcasecmp(entry->className, classNameLower) == 0) {
            return entry;
        }
    }
    return NULL;
}

static RuntimeVTableEntry* createRuntimeVTableEntry(const char* classNameLower) {
    RuntimeVTableEntry* entry = (RuntimeVTableEntry*)calloc(1, sizeof(RuntimeVTableEntry));
    if (!entry) {
        return NULL;
    }
    entry->className = strdup(classNameLower);
    if (!entry->className) {
        free(entry);
        return NULL;
    }
    entry->table = NULL;
    entry->next = runtimeVTables;
    runtimeVTables = entry;
    return entry;
}

static void vmFreeRuntimeVTables(void) {
    pthread_mutex_lock(&runtimeVTablesMutex);
    RuntimeVTableEntry* entry = runtimeVTables;
    runtimeVTables = NULL;
    pthread_mutex_unlock(&runtimeVTablesMutex);
    while (entry) {
        RuntimeVTableEntry* next = entry->next;
        free(entry->className);
        if (entry->table) {
            freeValue(entry->table);
            free(entry->table);
        }
        free(entry);
        entry = next;
    }
}

static void vmCleanupGlobalCachesIfIdle(void) {
    // Pre-existing peek-before-lock race (TSan-confirmed, Docs/pscal_vm2_plan.md
    // Phase 5a prerequisite fix): vmProcRegistryIsEmpty() used to check
    // gVmProcRegistryCount==0 and release gVmProcRegistryLock before the
    // caller acted on the answer, so two threads tearing down their VMs at
    // nearly the same moment (e.g. two pool workers freeVM-ing concurrently
    // at shutdown) could both observe "empty" and both free the same
    // process-global runtimeVTables list / shell-builtin-profile arrays --
    // same bug shape as the dynamic-array peek-before-lock races fixed just
    // before this phase. Fix: hold the registry lock across the check *and*
    // the frees, exactly like every other mutation of this registry does.
    pthread_mutex_lock(&gVmProcRegistryLock);
    if (gVmProcRegistryCount == 0) {
        vmFreeRuntimeVTables();
        vmFreeShellBuiltinProfiles();
    }
    pthread_mutex_unlock(&gVmProcRegistryLock);
}

static AST* vmResolveInterfaceAST(AST* interfaceType) {
    AST* current = interfaceType;
    int depth = 0;
    while (current && depth < 16) {
        if (current->type == AST_TYPE_DECL && current->left) {
            current = current->left;
            depth++;
            continue;
        }
        if (current->type == AST_TYPE_REFERENCE && current->token && current->token->value) {
            AST* looked = lookupType(current->token->value);
            if (!looked || looked == current) {
                break;
            }
            current = looked;
            depth++;
            continue;
        }
        break;
    }
    return current;
}

static bool runtimeAddInterfaceMethod(AST*** methods, int* count, int* capacity, AST* method) {
    if (!methods || !count || !capacity || !method || !method->token || !method->token->value) {
        return false;
    }

    for (int i = 0; i < *count; i++) {
        AST* existing = (*methods)[i];
        if (existing && existing->token && existing->token->value &&
            strcasecmp(existing->token->value, method->token->value) == 0) {
            return true;
        }
    }

    if (*count == *capacity) {
        int newCapacity = (*capacity == 0) ? 4 : (*capacity * 2);
        AST** resized = (AST**)realloc(*methods, sizeof(AST*) * (size_t)newCapacity);
        if (!resized) {
            return false;
        }
        *methods = resized;
        *capacity = newCapacity;
    }

    (*methods)[(*count)++] = method;
    return true;
}

static bool collectRuntimeInterfaceMethods(AST* interfaceType,
                                           AST*** methods,
                                           int* count,
                                           int* capacity,
                                           int depth) {
    if (depth > 32) {
        return false;
    }

    interfaceType = vmResolveInterfaceAST(interfaceType);
    if (!interfaceType || interfaceType->type != AST_INTERFACE) {
        return false;
    }

    AST* baseList = interfaceType->extra;
    if (baseList) {
        if (baseList->type == AST_LIST) {
            for (int i = 0; i < baseList->child_count; i++) {
                if (!collectRuntimeInterfaceMethods(baseList->children[i], methods, count, capacity, depth + 1)) {
                    return false;
                }
            }
        } else {
            if (!collectRuntimeInterfaceMethods(baseList, methods, count, capacity, depth + 1)) {
                return false;
            }
        }
    }

    for (int i = 0; i < interfaceType->child_count; i++) {
        AST* method = interfaceType->children[i];
        if (!method) {
            continue;
        }
        if (method->type != AST_PROCEDURE_DECL && method->type != AST_FUNCTION_DECL) {
            continue;
        }
        if (!runtimeAddInterfaceMethod(methods, count, capacity, method)) {
            return false;
        }
    }

    return true;
}

// Builds the method-address table of `className`'s implementation of
// `interfaceType`, indexed by interface method slot.
static bool buildRuntimeInterfaceVTable(const char* className,
                                        const char* class_lower,
                                        AST* interfaceType,
                                        Value* out) {
    int method_capacity = 0;
    int method_count = 0;
    int* addrs = NULL;

    AST** methods = NULL;
    int collected_count = 0;
    int collected_capacity = 0;
    if (!collectRuntimeInterfaceMethods(interfaceType,
                                        &methods,
                                        &collected_count,
                                        &collected_capacity,
                                        0)) {
        free(methods);
        return false;
    }

    for (int i = 0; i < collected_count; i++) {
        AST* method = methods[i];
        const char* method_name = (method && method->token) ? method->token->value : NULL;
        if (!method_name) {
            free(methods);
            free(addrs);
            return false;
        }

        int slot = method->i_val >= 0 ? method->i_val : i;
        if (slot >= method_capacity) {
            int new_capacity = method_capacity < 4 ? 4 : method_capacity * 2;
            while (new_capacity <= slot) {
                new_capacity *= 2;
            }
            int* resized = (int*)realloc(addrs, (size_t)new_capacity * sizeof(int));
            if (!resized) {
                free(methods);
                free(addrs);
                return false;
            }
            for (int j = method_capacity; j < new_capacity; j++) {
                resized[j] = -1;
            }
            addrs = resized;
            method_capacity = new_capacity;
        }

        char method_lower[MAX_SYMBOL_LENGTH];
        strncpy(method_lower, method_name, sizeof(method_lower) - 1);
        method_lower[sizeof(method_lower) - 1] = '\0';
        toLowerString(method_lower);

        char qualified[MAX_SYMBOL_LENGTH * 2 + 2];
        snprintf(qualified, sizeof(qualified), "%s.%s", class_lower, method_lower);
        Symbol* sym = lookupProcedure(qualified);
        if (!sym) {
            snprintf(qualified, sizeof(qualified), "%s.%s", className, method_name);
            sym = lookupProcedure(qualified);
        }
        if (!sym) {
            free(methods);
            free(addrs);
            return false;
        }

        Symbol* base = sym->is_alias ? sym->real_symbol : sym;
        if (!base) {
            free(methods);
            free(addrs);
            return false;
        }

        addrs[slot] = base->bytecode_address;
        if (slot + 1 > method_count) {
            method_count = slot + 1;
        }
    }

    free(methods);

    if (method_count == 0) {
        free(addrs);
        return false;
    }

    int lower = 0;
    int upper = method_count - 1;
    Value arr = makeArrayND(1, &lower, &upper, TYPE_INT32, NULL);
    if (!AS_ARRAY(arr)) {
        free(addrs);
        return false;
    }

    for (int i = 0; i < method_count; i++) {
        int addr = (i < method_capacity && addrs) ? addrs[i] : -1;
        AS_ARRAY(arr)[i] = makeInt(addr >= 0 ? addr : 0);
    }
    free(addrs);
    *out = arr;
    return true;
}

// The interface method table for `className` implementing the interface
// named `interfaceName`, built on first use and cached per (class,
// interface) pair. The key must name the interface: the interface AST's own
// token is the `interface` keyword for every Pascal interface, so a key
// built from it gave all of a class's interfaces the table of whichever
// was boxed first, and a call through the second dispatched to the wrong
// method. The table lives in the cache only -- it is never written into
// the receiver, whose single hidden __vtable field cannot hold one table
// per interface. NULL if the table cannot be built.
static Value* ensureRuntimeInterfaceVTable(VM* vm,
                                           const char* className,
                                           const char* interfaceName,
                                           AST* interfaceType) {
    if (!vm || !className || !interfaceName || !interfaceType || !vm->procedureTable) {
        return NULL;
    }

    char class_lower[MAX_SYMBOL_LENGTH];
    strncpy(class_lower, className, sizeof(class_lower) - 1);
    class_lower[sizeof(class_lower) - 1] = '\0';
    toLowerString(class_lower);

    char interface_lower[MAX_SYMBOL_LENGTH];
    strncpy(interface_lower, interfaceName, sizeof(interface_lower) - 1);
    interface_lower[sizeof(interface_lower) - 1] = '\0';
    toLowerString(interface_lower);

    char cache_key[(MAX_SYMBOL_LENGTH * 2) + 4];
    snprintf(cache_key, sizeof(cache_key), "%s|%s", class_lower, interface_lower);

    pthread_mutex_lock(&runtimeVTablesMutex);
    RuntimeVTableEntry* entry = findRuntimeVTableEntry(cache_key);
    if (!entry || !entry->table || VALUE_TYPE(*entry->table) != TYPE_ARRAY || !AS_ARRAY(*entry->table)) {
        Value arr;
        if (!buildRuntimeInterfaceVTable(className, class_lower, interfaceType, &arr)) {
            pthread_mutex_unlock(&runtimeVTablesMutex);
            return NULL;
        }
        if (!entry) {
            entry = createRuntimeVTableEntry(cache_key);
            if (!entry) {
                freeValue(&arr);
                pthread_mutex_unlock(&runtimeVTablesMutex);
                return NULL;
            }
        }
        if (!entry->table) {
            entry->table = (Value*)malloc(sizeof(Value));
            if (!entry->table) {
                freeValue(&arr);
                pthread_mutex_unlock(&runtimeVTablesMutex);
                return NULL;
            }
        } else {
            freeValue(entry->table);
        }
        *(entry->table) = arr;
    }
    Value* table = entry->table;
    pthread_mutex_unlock(&runtimeVTablesMutex);
    return table;
}

static Value vmHostBoxInterface(VM* vm) {
    if (!vm) {
        return makeNil();
    }

    Value typeNameVal = pop(vm);
    Value classNameVal = pop(vm);
    Value receiverVal = pop(vm);
    Value tablePtrVal = pop(vm);

    if (VALUE_TYPE(typeNameVal) != TYPE_STRING || !AS_STRING(typeNameVal)) {
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Interface cast requires interface type name string.");
        return makeNil();
    }

    if (VALUE_TYPE(classNameVal) != TYPE_STRING || !AS_STRING(classNameVal)) {
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Interface cast requires class type name string.");
        return makeNil();
    }

    AST* interfaceType = lookupType(AS_STRING(typeNameVal));
    if (!interfaceType) {
        // Report before the frees: freeValue clears the name being printed.
        runtimeError(vm, "VM Error: Unknown interface type '%s'.", AS_STRING(typeNameVal));
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        return makeNil();
    }

    if (VALUE_TYPE(receiverVal) != TYPE_POINTER) {
        Value* clone = (Value*)malloc(sizeof(Value));
        if (!clone) {
            freeValue(&classNameVal);
            freeValue(&typeNameVal);
            freeValue(&receiverVal);
            freeValue(&tablePtrVal);
            runtimeError(vm, "VM Error: Out of memory boxing interface receiver.");
            return makeNil();
        }
        *clone = makeCopyOfValue(&receiverVal);
        freeValue(&receiverVal);
        receiverVal = makePointer(clone, NULL);
        PTR_BASE_TYPE_NODE(receiverVal) = OWNED_POINTER_SENTINEL;

        bool invalid_type = false;
        Value* clonedRecord = resolveRecord(&receiverVal, &invalid_type);
        if (!clonedRecord || invalid_type || VALUE_TYPE(*clonedRecord) != TYPE_RECORD) {
            freeValue(&classNameVal);
            freeValue(&typeNameVal);
            freeValue(&receiverVal);
            freeValue(&tablePtrVal);
            runtimeError(vm, "VM Error: Unable to resolve cloned receiver for interface boxing.");
            return makeNil();
        }
    }

    const char* class_name_str = AS_STRING(classNameVal);
    Value* tableValuePtr = ensureRuntimeInterfaceVTable(vm, class_name_str, AS_STRING(typeNameVal), interfaceType);
    if (!tableValuePtr) {
        runtimeError(vm, "VM Error: Unable to initialise interface table for class '%s'.",
                     class_name_str ? class_name_str : "<unknown>");
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        return makeNil();
    }

    ClosureEnvPayload* payload = createClosureEnv(3, TYPE_INTERFACE);
    if (!payload) {
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Out of memory creating interface payload.");
        return makeNil();
    }

    Value* receiverCell = (Value*)malloc(sizeof(Value));
    Value* tableCell = (Value*)malloc(sizeof(Value));
    Value* classCell = (Value*)malloc(sizeof(Value));
    if (!receiverCell || !tableCell || !classCell) {
        if (receiverCell) free(receiverCell);
        if (tableCell) free(tableCell);
        if (classCell) free(classCell);
        releaseClosureEnv(payload);
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Out of memory capturing interface payload.");
        return makeNil();
    }

    *receiverCell = makeCopyOfValue(&receiverVal);
    if (VALUE_TYPE(*receiverCell) == TYPE_POINTER && PTR_BASE_TYPE_NODE(*receiverCell) == NULL &&
        PTR_BASE_TYPE_NODE(receiverVal) == OWNED_POINTER_SENTINEL) {
        PTR_BASE_TYPE_NODE(*receiverCell) = OWNED_POINTER_SENTINEL;
    }
    *tableCell = makePointer(tableValuePtr, NULL);
    const char* class_identity_source = class_name_str ? class_name_str : "";
    size_t class_identity_len = strlen(class_identity_source);
    char* lowered_identity = (char*)malloc(class_identity_len + 1);
    if (!lowered_identity) {
        free(receiverCell);
        free(tableCell);
        free(classCell);
        releaseClosureEnv(payload);
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Out of memory normalising class identity for interface payload.");
        return makeNil();
    }
    for (size_t i = 0; i < class_identity_len; ++i) {
        lowered_identity[i] = (char)tolower((unsigned char)class_identity_source[i]);
    }
    lowered_identity[class_identity_len] = '\0';
    *classCell = makeString(lowered_identity);
    free(lowered_identity);
    payload->slots[0] = receiverCell;
    payload->slots[1] = tableCell;
    payload->slots[2] = classCell;
    payload->symbol = NULL;

    Value iface = makeInterface(interfaceType, payload);

    releaseClosureEnv(payload);
    freeValue(&classNameVal);
    freeValue(&typeNameVal);
    if (VALUE_TYPE(receiverVal) == TYPE_POINTER && PTR_BASE_TYPE_NODE(receiverVal) == OWNED_POINTER_SENTINEL) {
        PTR_BASE_TYPE_NODE(receiverVal) = NULL;
    }
    freeValue(&receiverVal);
    freeValue(&tablePtrVal);
    return iface;
}

static Value vmHostInterfaceLookup(VM* vm) {
    if (!vm) {
        return makeNil();
    }

    Value methodIndexVal = pop(vm);
    Value ifaceVal = pop(vm);

    if (VALUE_TYPE(ifaceVal) != TYPE_INTERFACE) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface dispatch requires interface value.");
        return makeNil();
    }

    if (!IS_INTLIKE(methodIndexVal)) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface dispatch slot must be an integer.");
        return makeNil();
    }

    ClosureEnvPayload* payload = AS_INTERFACE(ifaceVal).payload;
    if (!payload || payload->slot_count < 2) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver data.");
        return makeNil();
    }

    Value* receiverCell = payload->slots[0];
    Value* tableCell = payload->slots[1];
    if (!receiverCell || !tableCell) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver or table.");
        return makeNil();
    }

    Value* tableValue = tableCell;
    while (tableValue && VALUE_TYPE(*tableValue) == TYPE_POINTER) {
        if (!AS_POINTER(*tableValue)) {
            freeValue(&methodIndexVal);
            freeValue(&ifaceVal);
            runtimeError(vm, "VM Error: Interface method table pointer is nil.");
            return makeNil();
        }
        tableValue = (Value*)AS_POINTER(*tableValue);
    }

    if (!tableValue || VALUE_TYPE(*tableValue) != TYPE_ARRAY || !AS_ARRAY(*tableValue)) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method table is not an array.");
        return makeNil();
    }

    if (ARRAY_DIMENSIONS(*tableValue) <= 0) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method table is empty.");
        return makeNil();
    }

    if (ARRAY_DIMENSIONS(*tableValue) != 1) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method table must be one-dimensional.");
        return makeNil();
    }

    if (!ARRAY_LOWER_BOUNDS(*tableValue) || !ARRAY_UPPER_BOUNDS(*tableValue)) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method table missing bounds metadata.");
        return makeNil();
    }

    int lower = ARRAY_LOWER_BOUNDS(*tableValue)[0];
    int upper = ARRAY_UPPER_BOUNDS(*tableValue)[0];
    int total = calculateArrayTotalSize(tableValue);
    if (total <= 0 || upper < lower) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method table has invalid bounds.");
        return makeNil();
    }

    int methodIndex = (int)AS_INTEGER(methodIndexVal);
    if (methodIndex < lower || methodIndex > upper) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method slot %d out of range.", methodIndex);
        return makeNil();
    }

    int offset = methodIndex - lower;
    if (offset < 0 || offset >= total) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method slot %d out of range.", methodIndex);
        return makeNil();
    }

    Value entry = AS_ARRAY(*tableValue)[offset];
    if (!IS_INTLIKE(entry)) {
        freeValue(&methodIndexVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface method entry must be an address.");
        return makeNil();
    }

    vmStoreThreadMyself(vm, copyInterfaceReceiverAlias(receiverCell));

    uint32_t target_address = (uint32_t)AS_INTEGER(entry);

    freeValue(&methodIndexVal);
    freeValue(&ifaceVal);
    return makeInt(target_address);
}

static Value vmHostInterfaceAssert(VM* vm) {
    if (!vm) {
        return makeNil();
    }

    Value targetTypeVal = pop(vm);
    Value ifaceVal = pop(vm);

    if (VALUE_TYPE(ifaceVal) != TYPE_INTERFACE) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface assertion requires interface value.");
        return makeNil();
    }

    if (VALUE_TYPE(targetTypeVal) != TYPE_STRING || !AS_STRING(targetTypeVal)) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface assertion requires target type name string.");
        return makeNil();
    }

    ClosureEnvPayload* payload = AS_INTERFACE(ifaceVal).payload;
    if (!payload || payload->slot_count < 2) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver metadata.");
        return makeNil();
    }

    Value* receiverCell = payload->slots[0];
    Value* classCell = (payload->slot_count >= 3) ? payload->slots[2] : NULL;
    if (!receiverCell) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver value.");
        return makeNil();
    }

    if (!classCell || VALUE_TYPE(*classCell) != TYPE_STRING || !AS_STRING(*classCell)) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing class identity.");
        return makeNil();
    }

    size_t type_len = strlen(AS_STRING(targetTypeVal));
    char* lowered_target = (char*)malloc(type_len + 1);
    if (!lowered_target) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Out of memory during interface assertion.");
        return makeNil();
    }
    for (size_t i = 0; i < type_len; ++i) {
        lowered_target[i] = (char)tolower((unsigned char)AS_STRING(targetTypeVal)[i]);
    }
    lowered_target[type_len] = '\0';

    bool matches = (strcmp(lowered_target, AS_STRING(*classCell)) == 0);
    free(lowered_target);

    if (!matches) {
        const char* actual = AS_STRING(*classCell) ? AS_STRING(*classCell) : "<unknown>";
        runtimeError(vm, "VM Error: Interface assertion expected '%s' but receiver is '%s'.",
                     AS_STRING(targetTypeVal), actual);
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        return makeNil();
    }

    Value result = copyInterfaceReceiverAlias(receiverCell);

    freeValue(&targetTypeVal);
    freeValue(&ifaceVal);
    return result;
}

static Value vmHostInterfaceIs(VM* vm) {
    if (!vm) {
        return makeBoolean(false);
    }

    Value targetTypeVal = pop(vm);
    Value ifaceVal = pop(vm);

    if (VALUE_TYPE(ifaceVal) != TYPE_INTERFACE) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface type test requires interface value.");
        return makeBoolean(false);
    }

    if (VALUE_TYPE(targetTypeVal) != TYPE_STRING || !AS_STRING(targetTypeVal)) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface type test requires target type name string.");
        return makeBoolean(false);
    }

    ClosureEnvPayload* payload = AS_INTERFACE(ifaceVal).payload;
    if (!payload || payload->slot_count < 3) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver metadata.");
        return makeNil();
    }

    Value* receiverCell = payload->slots[0];
    Value* classCell = payload->slots[2];
    if (!receiverCell) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing receiver value.");
        return makeNil();
    }
    if (!classCell || VALUE_TYPE(*classCell) != TYPE_STRING || !AS_STRING(*classCell)) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Interface payload missing class identity.");
        return makeNil();
    }

    size_t type_len = strlen(AS_STRING(targetTypeVal));
    char* lowered_target = (char*)malloc(type_len + 1);
    if (!lowered_target) {
        freeValue(&targetTypeVal);
        freeValue(&ifaceVal);
        runtimeError(vm, "VM Error: Out of memory during interface type test.");
        return makeBoolean(false);
    }

    for (size_t i = 0; i < type_len; ++i) {
        lowered_target[i] = (char)tolower((unsigned char)AS_STRING(targetTypeVal)[i]);
    }
    lowered_target[type_len] = '\0';

    bool matches = (strcmp(lowered_target, AS_STRING(*classCell)) == 0);
    free(lowered_target);

    // In this dialect `is` is a safe cast: yield the narrowed receiver pointer
    // when the dynamic type matches, or nil otherwise. This mirrors `as`
    // (vmHostInterfaceAssert), which returns the same receiver alias but raises
    // instead of returning nil on a mismatch. Returning the pointer (rather than
    // a boolean) lets callers compare the result against the original instance
    // and use it as a non-nil / nil truthy value.
    //
    // Materialise the alias BEFORE releasing ifaceVal: receiverCell points into
    // ifaceVal's payload, so freeing the interface first would leave
    // copyInterfaceReceiverAlias reading freed memory (mirrors the ordering in
    // vmHostInterfaceAssert).
    Value result = matches ? copyInterfaceReceiverAlias(receiverCell) : makeNil();
    freeValue(&targetTypeVal);
    freeValue(&ifaceVal);
    return result;
}

static Value vmHostWaitThread(VM* vm) {
    // Expects top of stack: integer thread id
    Value tidVal = pop(vm);
    if (VALUE_TYPE(tidVal) == TYPE_THREAD || IS_INTLIKE(tidVal)) {
        int id = (int)AS_INTEGER(tidVal);
        // joinThread() only waits for the job's status; it never consumes the
        // result or flips readyForReuse, so the worker thread would sit in its
        // post-job wait loop (vm.c threadStart, awaitingReuse) forever and its
        // pool slot would never be recyclable. vmJoinThreadById() performs the
        // same wait plus the release handshake that vmBuiltinWaitForThread
        // (the indirect-call path) already relies on -- mirror that here so
        // CALL_HOST/HOST_FN_WAIT_THREAD (the direct WaitForThread(id) call
        // form) actually returns the worker to the pool.
        VM* thread_vm = vm;
        if (vm && vm->threadOwner) {
            thread_vm = vm->threadOwner;
        }
        bool joined = thread_vm ? vmJoinThreadById(thread_vm, id) : false;
        if (!joined && thread_vm && thread_vm != vm) {
            vmJoinThreadById(vm, id);
        }
    }
    freeValue(&tidVal);
    return makeInt(0);
}

// Base-type nodes for INIT_LOCAL_POINTER's builtin pointee types
// (`^integer`), which have no registered type to point at. A pointer keeps
// its base-type node and may outlive the frame, so the node cannot be freed
// with it; instead one node per type name is built on first use and kept,
// rather than a new node on every execution (a routine with a `^integer`
// local leaked one per call).
typedef struct VmBuiltinPointerBase {
    char* name;
    AST* node;
    struct VmBuiltinPointerBase* next;
} VmBuiltinPointerBase;

static VmBuiltinPointerBase* gVmBuiltinPointerBases = NULL;
static pthread_mutex_t gVmBuiltinPointerBasesMutex = PTHREAD_MUTEX_INITIALIZER;

static AST* vmBuiltinPointerBaseNode(const char* tn) {
    pthread_mutex_lock(&gVmBuiltinPointerBasesMutex);
    for (VmBuiltinPointerBase* entry = gVmBuiltinPointerBases; entry; entry = entry->next) {
        if (strcmp(entry->name, tn) == 0) {
            pthread_mutex_unlock(&gVmBuiltinPointerBasesMutex);
            return entry->node;
        }
    }
    VmBuiltinPointerBase* entry = (VmBuiltinPointerBase*)calloc(1, sizeof(VmBuiltinPointerBase));
    char* name = strdup(tn);
    if (!entry || !name) {
        free(entry);
        free(name);
        pthread_mutex_unlock(&gVmBuiltinPointerBasesMutex);
        return NULL;
    }
    Token* baseTok = newToken(TOKEN_IDENTIFIER, tn, 0, 0);
    AST* node = newASTNode(AST_VARIABLE, baseTok);
    if      (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) setTypeAST(node, TYPE_INT32);
    else if (strcasecmp(tn, "real")    == 0 || strcasecmp(tn, "double") == 0) setTypeAST(node, TYPE_DOUBLE);
    else if (strcasecmp(tn, "single")  == 0 || strcasecmp(tn, "float")  == 0) setTypeAST(node, TYPE_FLOAT);
    else if (strcasecmp(tn, "char")    == 0) setTypeAST(node, TYPE_CHAR);
    else if (strcasecmp(tn, "widechar")== 0) setTypeAST(node, TYPE_WIDECHAR);
    else if (strcasecmp(tn, "unicodestring")== 0) setTypeAST(node, TYPE_UNICODE_STRING);
    else if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) setTypeAST(node, TYPE_BOOLEAN);
    else if (strcasecmp(tn, "byte")    == 0) setTypeAST(node, TYPE_BYTE);
    else if (strcasecmp(tn, "word")    == 0) setTypeAST(node, TYPE_WORD);
    else if (strcasecmp(tn, "int64")   == 0 || strcasecmp(tn, "longint") == 0) setTypeAST(node, TYPE_INT64);
    else if (strcasecmp(tn, "cardinal")== 0) setTypeAST(node, TYPE_UINT32);
    else setTypeAST(node, TYPE_VOID);
    freeToken(baseTok);
    entry->name = name;
    entry->node = node;
    entry->next = gVmBuiltinPointerBases;
    gVmBuiltinPointerBases = entry;
    pthread_mutex_unlock(&gVmBuiltinPointerBasesMutex);
    return node;
}

// A call must pass exactly as many arguments as the callee has parameters:
// otherwise they land in the wrong slots (the frame window keeps that
// memory-safe, not correct). Reports a runtime error and returns false.
static bool vmCheckCallArity(VM* vm, const Symbol* callee, int passed) {
    if (callee->arity != passed) {
        runtimeError(vm, "VM Error: '%s' takes %d argument(s) but the call passes %d.",
                     callee->name ? callee->name : "<anonymous>", (int)callee->arity, passed);
        return false;
    }
    return true;
}

// Checks one dimension of a local or field array before makeArrayND sees
// it: both bounds must fit an int (a plain cast turned array[1..4294967297]
// into array[1..1]) and the dimension must hold between 1 and INT_MAX
// elements (makeArrayND exits the whole process on an empty one). Reports a
// runtime error and returns false otherwise.
static bool vmCheckArrayDimension(VM* vm, long long lower, long long upper, int* out_lower, int* out_upper) {
    if (lower < INT_MIN || lower > INT_MAX || upper < INT_MIN || upper > INT_MAX) {
        runtimeError(vm, "Runtime Error: Array bounds %lld..%lld are out of range.", lower, upper);
        return false;
    }
    if (upper < lower) {
        runtimeError(vm, "Runtime Error: Array dimension %lld..%lld has no elements.", lower, upper);
        return false;
    }
    if (upper - lower >= INT_MAX) {
        runtimeError(vm, "Runtime Error: Array dimension %lld..%lld is too large.", lower, upper);
        return false;
    }
    *out_lower = (int)lower;
    *out_upper = (int)upper;
    return true;
}

// vsnprintf into a fresh heap buffer sized for the whole result (NULL if
// out of memory). The caller frees it.
static char* vmFormatAlloc(const char* format, ...) __attribute__((format(printf, 1, 2)));
static char* vmFormatAlloc(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int needed = vsnprintf(NULL, 0, format, ap);
    va_end(ap);
    if (needed < 0) {
        return NULL;
    }
    char* out = (char*)malloc((size_t)needed + 1);
    if (!out) {
        return NULL;
    }
    va_start(ap, format);
    vsnprintf(out, (size_t)needed + 1, format, ap);
    va_end(ap);
    return out;
}

// Largest field width or precision vmHostPrintf accepts. Anything larger is
// reported as an error instead of being silently dropped with its argument.
#define VM_PRINTF_MAX_FIELD 65535

// Reads a run of decimal digits at fmt[*j]; false if the number is above
// VM_PRINTF_MAX_FIELD (the digits are consumed either way).
static bool vmPrintfReadField(const char* fmt, size_t flen, size_t* j, int* out) {
    int value = 0;
    bool fits = true;
    while (*j < flen && isdigit((unsigned char)fmt[*j])) {
        if (fits) {
            value = value * 10 + (fmt[*j] - '0');
            if (value > VM_PRINTF_MAX_FIELD) {
                fits = false;
            }
        }
        (*j)++;
    }
    *out = value;
    return fits;
}

// printf for the Pascal and Rea front ends. Each conversion is re-issued to
// the C library from its parsed parts with an argument of exactly the type
// that conversion expects: an integer is always passed as a 64-bit
// (unsigned) long long under `ll`, after narrowing it to the width the
// user's length modifier asks for (hh: 8 bits, h: 16, l/ll/j/z/t/L: 64, none:
// the operand's own width, 32 or 64); a real is passed as a double, or as a
// long double under `L`. Output goes straight to stdout, so no conversion
// is cut short by a fixed buffer.
static Value vmHostPrintf(VM* vm) {
    Value countVal = pop(vm);
    int arg_count = 0;
    if (IS_INTLIKE(countVal)) arg_count = (int)AS_INTEGER(countVal);
    freeValue(&countVal);
    if (arg_count <= 0) return makeInt(0);

    Value* args = (Value*)malloc(sizeof(Value) * arg_count);
    if (!args) return makeInt(0);
    for (int i = 0; i < arg_count; ++i) {
        args[arg_count - 1 - i] = pop(vm);
    }

    const char* fmt = (isPascalStringType(VALUE_TYPE(args[0])) && AS_STRING(args[0])) ? AS_STRING(args[0]) : "";
    int arg_index = 1;
    size_t flen = strlen(fmt);
    for (size_t i = 0; i < flen; ++i) {
        if (fmt[i] == '%' && i + 1 < flen) {
            if (fmt[i + 1] == '%') {
                fputc('%', stdout);
                i++;
            } else if (arg_index < arg_count) {
                size_t j = i + 1;
                // Flags; a repeated flag means the same as one, so each is
                // kept once.
                char flags[8];
                size_t flag_len = 0;
                const char* flag_chars = "-+ #0'";
                while (j < flen && strchr(flag_chars, fmt[j]) != NULL) {
                    if (!memchr(flags, fmt[j], flag_len)) {
                        flags[flag_len++] = fmt[j];
                    }
                    j++;
                }
                bool fields_fit = true;
                int width = -1;
                if (j < flen && isdigit((unsigned char)fmt[j])) {
                    fields_fit = vmPrintfReadField(fmt, flen, &j, &width);
                }
                int precision = -1;
                if (j < flen && fmt[j] == '.') {
                    j++;
                    if (!vmPrintfReadField(fmt, flen, &j, &precision)) {
                        fields_fit = false;
                    }
                }
                const char* length_mods = "hlLjzt";
                size_t mod_start = j;
                bool mods_all_h = true;
                bool mod_long_double = false;
                while (j < flen && strchr(length_mods, fmt[j]) != NULL) {
                    if (fmt[j] != 'h') mods_all_h = false;
                    if (fmt[j] == 'L') mod_long_double = true;
                    j++;
                }
                size_t mod_len = j - mod_start;
                char spec = (j < flen) ? fmt[j] : '\0';

                Value v = args[arg_index++];
                if (!fields_fit) {
                    size_t spec_len = (j < flen ? j + 1 : flen) - i;
                    runtimeError(vm, "Runtime Error: printf field width or precision above %d in '%.*s%s'.",
                                 VM_PRINTF_MAX_FIELD, (int)(spec_len > 40 ? 40 : spec_len), fmt + i,
                                 spec_len > 40 ? "..." : "");
                    freeValue(&v);
                    break;
                }

                // '%' + flags + width + '.' + precision + "ll" + spec + NUL
                char conv[40];
                size_t pos = 0;
                conv[pos++] = '%';
                memcpy(conv + pos, flags, flag_len);
                pos += flag_len;
                if (width >= 0) {
                    pos += (size_t)snprintf(conv + pos, sizeof(conv) - pos, "%d", width);
                }
                if (precision >= 0 && spec != 'c') {
                    pos += (size_t)snprintf(conv + pos, sizeof(conv) - pos, ".%d", precision);
                }

                switch (spec) {
                    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                        long long s = 0;
                        if (isIntlikeType(VALUE_TYPE(v))) {
                            s = AS_INTEGER(v);
                        }
                        int bits;
                        if (mod_len == 0) {
                            bits = (VALUE_TYPE(v) == TYPE_INT64 || VALUE_TYPE(v) == TYPE_UINT64) ? 64 : 32;
                        } else if (mods_all_h) {
                            bits = (mod_len >= 2) ? 8 : 16;
                        } else {
                            bits = 64;
                        }
                        conv[pos++] = 'l';
                        conv[pos++] = 'l';
                        conv[pos++] = spec;
                        conv[pos] = '\0';
                        if (spec == 'd' || spec == 'i') {
                            if (bits == 8) s = (signed char)s;
                            else if (bits == 16) s = (short)s;
                            else if (bits == 32) s = (int32_t)s;
                            fprintf(stdout, conv, s);
                        } else {
                            unsigned long long u = (unsigned long long)s;
                            if (bits == 8) u &= 0xFFULL;
                            else if (bits == 16) u &= 0xFFFFULL;
                            else if (bits == 32) u &= 0xFFFFFFFFULL;
                            fprintf(stdout, conv, u);
                        }
                        break;
                    }
                    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                        long double rv = 0.0L;
                        if (isRealType(VALUE_TYPE(v))) {
                            rv = AS_REAL(v);
                        } else if (isIntlikeType(VALUE_TYPE(v))) {
                            rv = asLd(v);
                        }
                        if (mod_long_double) {
                            conv[pos++] = 'L';
                            conv[pos++] = spec;
                            conv[pos] = '\0';
                            fprintf(stdout, conv, rv);
                        } else {
                            conv[pos++] = spec;
                            conv[pos] = '\0';
                            fprintf(stdout, conv, (double)rv);
                        }
                        break;
                    }
                    case 'c': {
                        char ch = (VALUE_TYPE(v) == TYPE_CHAR) ? (char)AS_CHAR(v) : (char)AS_INTEGER(v);
                        conv[pos++] = 'c';
                        conv[pos] = '\0';
                        fprintf(stdout, conv, (int)(unsigned char)ch);
                        break;
                    }
                    case 's': {
                        char char_text[5] = {0};
                        const char* sv = "";
                        if (isPascalStringType(VALUE_TYPE(v)) && AS_STRING(v)) {
                            sv = AS_STRING(v);
                        } else if (VALUE_TYPE(v) == TYPE_CHAR) {
                            char_text[0] = (char)AS_CHAR(v);
                            sv = char_text;
                        } else if (VALUE_TYPE(v) == TYPE_WIDECHAR) {
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(v), char_text);
                            sv = char_text;
                        }
                        conv[pos++] = 's';
                        conv[pos] = '\0';
                        fprintf(stdout, conv, sv);
                        break;
                    }
                    default:
                        printValueToStream(v, stdout);
                        break;
                }
                freeValue(&v);
                i = j;
            } else {
                fputc('%', stdout);
            }
        } else {
            fputc(fmt[i], stdout);
        }
    }

    for (int k = arg_index; k < arg_count; ++k) {
        freeValue(&args[k]);
    }
    freeValue(&args[0]);
    free(args);
    fflush(stdout);
    return makeInt(0);
}

static Value vmHostShellLastStatusHost(VM* vm) {
    return vmHostShellLastStatus(vm);
}

static Value vmHostShellLoopCheckConditionHost(VM* vm) {
    return vmHostShellLoopCheckCondition(vm);
}

static Value vmHostShellLoopCheckBodyHost(VM* vm) {
    return vmHostShellLoopCheckBody(vm);
}

static Value vmHostShellLoopExecuteBodyHost(VM* vm) {
    return vmHostShellLoopExecuteBody(vm);
}

static Value vmHostShellLoopAdvanceHost(VM* vm) {
    return vmHostShellLoopAdvance(vm);
}

static Value vmHostShellPollJobsHost(VM* vm) {
    return vmHostShellPollJobs(vm);
}

static Value vmHostShellLoopIsReadyHost(VM* vm) {
    return vmHostShellLoopIsReady(vm);
}

static void vmReleaseExecutionResources(VM* vm) {
    if (!vm) return;

    for (Value* slot = vm->stack; slot < vm->stackTop; ++slot) {
        freeValue(slot);
    }
    resetStack(vm);

    for (int i = 0; i < vm->frameCount; ++i) {
        CallFrame* frame = &vm->frames[i];
        if (frame->closureEnv) {
            releaseClosureEnv(frame->closureEnv);
            frame->closureEnv = NULL;
        } else if (frame->owns_upvalues && frame->upvalues) {
            free(frame->upvalues);
        }
        frame->upvalues = NULL;
        frame->owns_upvalues = false;
        frame->return_address = NULL;
        frame->slots = NULL;
        frame->function_symbol = NULL;
        frame->slotCount = 0;
        frame->locals_count = 0;
        frame->upvalue_count = 0;
        frame->discard_result_on_return = false;
        frame->vtable = NULL;
    }
    vm->frameCount = 0;
}

// --- Host Function Registration ---
bool registerHostFunction(VM* vm, HostFunctionID id, HostFn fn) {
    if (!vm) return false;
    if (id >= HOST_FN_COUNT || id < 0) {
        fprintf(stderr, "VM Error: HostFunctionID %d out of bounds during registration.\n", id);
        return false;
    }
    vm->host_functions[id] = fn;
    return true;
}

// --- VM Initialization and Cleanup ---
void vmResetExecutionState(VM* vm) {
    if (!vm) return;

    vmReleaseExecutionResources(vm);

    vm->chunk = NULL;
    vm->ip = NULL;
    vm->lastInstruction = NULL;
    vm->vmGlobalSymbols = NULL;
    vm->vmConstGlobalSymbols = NULL;
    vm->procedureTable = NULL;
    if (vm->procedureByAddress) {
        free(vm->procedureByAddress);
        vm->procedureByAddress = NULL;
    }
    vm->procedureByAddressSize = 0;
    vmFreeUserProcSiteCache(vm);

    vm->exit_requested = false;
    vm->abort_requested = false;
    vm->runtime_error_raised = false;
    vm->suspend_unwind_requested = false;
    vm->current_builtin_name = NULL;
    vm->trace_executed = 0;
    freeValue(&vm->threadMyself);
    vm->threadMyself = makeNil();

    atomic_store(&vm->shuttingDownWorkers, true);
    if (vm->jobQueue) {
        pthread_mutex_lock(&vm->jobQueue->mutex);
        vm->jobQueue->shuttingDown = true;
        pthread_cond_broadcast(&vm->jobQueue->cond);
        pthread_mutex_unlock(&vm->jobQueue->mutex);
    }
    for (size_t i = 1; i < vm->threadsCommittedCount; ++i) {
        Thread* thread = &vm->threads[i];
        if (atomic_load(&thread->inPool)) {
            atomic_store(&thread->killRequested, true);
            vmThreadWakeStateWaiters(thread);
        }
        // Gate on needsJoin, not inPool/active: the global shutdown flags
        // just set above already let any idle worker wake and fully exit
        // (clearing inPool/active) before this loop reaches its slot, so
        // reading those here would race and skip the join, leaking the
        // already-finished OS thread (see needsJoin's vm.h comment).
        if (atomic_load(&thread->needsJoin)) {
            pthread_join(thread->handle, NULL);
            atomic_store(&thread->active, false);
            atomic_store(&thread->needsJoin, false);
        }
        if (thread->ownsVm && thread->vm) {
            freeVM(thread->vm);
            free(thread->vm);
            thread->vm = NULL;
            thread->ownsVm = false;
        }
        vmThreadDestroySlot(thread);
        memset(thread, 0, sizeof(*thread));
        vmThreadInitSlot(thread);
    }
    if (vm->jobQueue) {
        vmThreadJobQueueDestroy(vm->jobQueue);
    }
    vm->jobQueue = vmThreadJobQueueCreate();
    vm->workerCount = 0;
    vm->availableWorkers = 0;
    atomic_store(&vm->shuttingDownWorkers, false);
    vm->threadCount = 1;
    vm->threadOwner = vm;
    atomic_store(&vm->threads[0].active, false);
    vm->threads[0].vm = NULL;
    vm->threads[0].vm = vm;

    // Reset mutex registry state so a reused VM behaves like a fresh instance.
    if (vm->mutexOwner == vm) {
        pthread_mutex_lock(&vm->mutexRegistryLock);
        for (int i = 0; i < vm->mutexCount; ++i) {
            if (vm->mutexes[i].active) {
                pthread_mutex_destroy(&vm->mutexes[i].handle);
                vm->mutexes[i].active = false;
            }
        }
        vm->mutexCount = 0;
        pthread_mutex_unlock(&vm->mutexRegistryLock);
    } else {
        vm->mutexCount = 0;
    }
    vm->mutexOwner = vm;
}

void initVM(VM* vm) { // As in all.txt, with frameCount
    if (!vm) return;
    // VM 2.0 Phase 4a (Docs/pscal_vm2_plan.md §5.10.1): one-time check that
    // this process's heap pointers fit the 46-bit payload budget the
    // upcoming NaN-boxed Value representation reserves. Idempotent and
    // cheap (one malloc+free); run first so a violated assumption aborts
    // before any other VM state is built, not partway through.
    pscalObjRunPointerWidthCanary();

    vm->stack = NULL;
    vm->stackTop = NULL;
    vm->stackMappedBytes = 0;
    vm->stackReservedValues = 0;
    vm->stackCommittedValues = 0;
    if (!vmAllocStackStorage(vm)) {
        fprintf(stderr, "Fatal VM Error: Could not reserve the operand stack "
                         "(%zu Values).\n", pscalVmStackCeilingValues());
        EXIT_FAILURE_HANDLER();
    }

    vm->frames = NULL;
    vm->frameCapacity = 0;
    vm->frameCount = 0; // <--- INITIALIZE frameCount (before vmGrowFrames() reads it below)
    if (!vmGrowFrames(vm)) {
        fprintf(stderr, "Fatal VM Error: Could not allocate the call-frame array.\n");
        EXIT_FAILURE_HANDLER();
    }

    vm->chunk = NULL;
    vm->ip = NULL;
    vm->lastInstruction = NULL;
    vm->vmGlobalSymbols = NULL;              // Will be set by interpretBytecode
    vm->vmConstGlobalSymbols = NULL;
    vm->procedureTable = NULL;
    vm->procedureByAddress = NULL;
    vm->procedureByAddressSize = 0;
    vm->userProcBySite = NULL;
    vm->userProcBySiteSize = 0;
    vm->userProcBySiteChunk = NULL;

    vm->exit_requested = false;
    vm->abort_requested = false;
    vm->runtime_error_raised = false;
    vm->suspend_unwind_requested = false;
    vm->current_builtin_name = NULL;
    vm->threadMyself = makeNil();

    vm->threadCount = 1; // main thread occupies index 0
    vm->threadOwner = vm;
    vm->threads = NULL;
    vm->threadsMappedBytes = 0;
    vm->threadsReservedCount = 0;
    vm->threadsCommittedCount = 0;
    if (!vmAllocThreadStorage(vm)) {
        fprintf(stderr, "Fatal VM Error: Could not reserve the thread pool "
                         "(%zu slots).\n", pscalVmThreadsCeiling());
        EXIT_FAILURE_HANDLER();
    }
    vm->threads[0].vm = vm;
    pthread_mutex_init(&vm->threadRegistryLock, NULL);
    vm->jobQueue = vmThreadJobQueueCreate();
    vm->workerCount = 0;
    vm->availableWorkers = 0;
    atomic_store(&vm->shuttingDownWorkers, false);

    vm->mutexCount = 0;
    pthread_mutex_init(&vm->mutexRegistryLock, NULL);
    vm->mutexOwner = vm;
    vm->mutexes = NULL;
    vm->mutexesMappedBytes = 0;
    vm->mutexesReservedCount = 0;
    vm->mutexesCommittedCount = 0;
    if (!vmAllocMutexStorage(vm)) {
        fprintf(stderr, "Fatal VM Error: Could not reserve the mutex pool "
                         "(%zu slots).\n", pscalVmMutexesCeiling());
        EXIT_FAILURE_HANDLER();
    }

    vm->owningThread = NULL;
    vm->threadId = 0;
    vm->frontendContext = NULL;
    vm->zeroBasedIndexing = frontendIsZeroBasedStrings();

    for (int i = 0; i < MAX_HOST_FUNCTIONS; i++) {
        vm->host_functions[i] = NULL;
    }
    if (!registerHostFunction(vm, HOST_FN_QUIT_REQUESTED, vmHostQuitRequested)) { // from all.txt
        fprintf(stderr, "Fatal VM Error: Could not register HOST_FN_QUIT_REQUESTED.\n");
        EXIT_FAILURE_HANDLER();
    }
    registerHostFunction(vm, HOST_FN_CREATE_THREAD_ADDR, vmHostCreateThreadAddr);
    registerHostFunction(vm, HOST_FN_WAIT_THREAD, vmHostWaitThread);
    registerHostFunction(vm, HOST_FN_PRINTF, vmHostPrintf);
    registerHostFunction(vm, HOST_FN_SHELL_LAST_STATUS, vmHostShellLastStatusHost);
    registerHostFunction(vm, HOST_FN_SHELL_LOOP_CHECK_CONDITION, vmHostShellLoopCheckConditionHost);
    registerHostFunction(vm, HOST_FN_SHELL_LOOP_CHECK_BODY, vmHostShellLoopCheckBodyHost);
    registerHostFunction(vm, HOST_FN_SHELL_LOOP_EXEC_BODY, vmHostShellLoopExecuteBodyHost);
    registerHostFunction(vm, HOST_FN_SHELL_LOOP_ADVANCE, vmHostShellLoopAdvanceHost);
    registerHostFunction(vm, HOST_FN_SHELL_POLL_JOBS, vmHostShellPollJobsHost);
    registerHostFunction(vm, HOST_FN_SHELL_LOOP_IS_READY, vmHostShellLoopIsReadyHost);
    registerHostFunction(vm, HOST_FN_CREATE_CLOSURE, vmHostCreateClosure);
    registerHostFunction(vm, HOST_FN_BOX_INTERFACE, vmHostBoxInterface);
    registerHostFunction(vm, HOST_FN_INTERFACE_LOOKUP, vmHostInterfaceLookup);
    registerHostFunction(vm, HOST_FN_INTERFACE_IS, vmHostInterfaceIs);
    registerHostFunction(vm, HOST_FN_INTERFACE_ASSERT, vmHostInterfaceAssert);

    // Default: tracing disabled
    vm->trace_head_instructions = 0;
    vm->trace_executed = 0;

    vmProcRegister(vm);
}

void freeVM(VM* vm) {
    if (!vm) return;
    vmProcUnregister(vm);
    vmReleaseExecutionResources(vm);
    freeValue(&vm->threadMyself);
    vm->threadMyself = makeNil();
    // The VM holds references to global symbol tables that are owned and
    // managed by the caller (e.g. vm_main.c). Freeing them here would lead to
    // double-free errors when the caller performs its own cleanup. Simply
    // clear the pointer to signal that the VM no longer uses it.
    if (vm->vmGlobalSymbols) {
        vm->vmGlobalSymbols = NULL;
    }
    if (vm->vmConstGlobalSymbols) {
        vm->vmConstGlobalSymbols = NULL;
    }

    if (vm->procedureByAddress) {
        free(vm->procedureByAddress);
        vm->procedureByAddress = NULL;
    }
    vm->procedureByAddressSize = 0;
    vmFreeUserProcSiteCache(vm);

    atomic_store(&vm->shuttingDownWorkers, true);
    if (vm->jobQueue) {
        pthread_mutex_lock(&vm->jobQueue->mutex);
        vm->jobQueue->shuttingDown = true;
        pthread_cond_broadcast(&vm->jobQueue->cond);
        pthread_mutex_unlock(&vm->jobQueue->mutex);
    }
    for (size_t i = 1; i < vm->threadsCommittedCount; i++) {
        Thread* thread = &vm->threads[i];
        if (atomic_load(&thread->inPool)) {
            atomic_store(&thread->killRequested, true);
            vmThreadWakeStateWaiters(thread);
        }
        // Gate on needsJoin, not inPool/active: the global shutdown flags
        // just set above already let any idle worker wake and fully exit
        // (clearing inPool/active) before this loop reaches its slot, so
        // reading those here would race and skip the join, leaking the
        // already-finished OS thread (see needsJoin's vm.h comment).
        if (atomic_load(&thread->needsJoin)) {
            pthread_join(thread->handle, NULL);
            atomic_store(&thread->active, false);
            atomic_store(&thread->needsJoin, false);
        }
        if (thread->ownsVm && thread->vm) {
            freeVM(thread->vm);
            free(thread->vm);
            thread->vm = NULL;
            thread->ownsVm = false;
        }
        atomic_store(&thread->inPool, false);
    }
    if (vm->jobQueue) {
        vmThreadJobQueueDestroy(vm->jobQueue);
        vm->jobQueue = NULL;
    }
    pthread_mutex_destroy(&vm->threadRegistryLock);

    vm->frontendContext = NULL;

    if (vm->mutexOwner == vm) {
        for (int i = 0; i < vm->mutexCount; i++) {
            if (vm->mutexes[i].active) {
                pthread_mutex_destroy(&vm->mutexes[i].handle);
                vm->mutexes[i].active = false;
            }
        }
    }
    // vmFreeThreadStorage below already vmThreadDestroySlot's every
    // committed slot before unmapping -- no separate loop needed here
    // (VM 2.0 Phase 5a checkpoint 5a-ii).
    pthread_mutex_destroy(&vm->mutexRegistryLock);
    vmCleanupGlobalCachesIfIdle();
    // No explicit freeing of vm->host_functions array itself as it's part of
    // the VM struct. If HostFn entries allocated memory, that would require
    // additional handling.

    // vmReleaseExecutionResources() (called above) already freed every Value
    // still on the stack and every frame's upvalues/closureEnv; only the
    // underlying storage itself (the mmap reservation and the frames[]
    // heap array) remains to be released now that the VM is being torn down.
    vmFreeStackStorage(vm);
    if (vm->frames) {
        free(vm->frames);
        vm->frames = NULL;
    }
    vm->frameCapacity = 0;
    // VM 2.0 Phase 5a checkpoint 5a-ii: threads[]/mutexes[] are now mmap
    // reservations too, same lifecycle as the stack above.
    vmFreeThreadStorage(vm);
    vmFreeMutexStorage(vm);
}

// Unwind the current call frame. If there are no more frames, the VM should halt.
// The 'halted' flag is set to true when the VM has returned from the top-level frame.
static InterpretResult returnFromCall(VM* vm, bool* halted) {
    if (vm->frameCount == 0) {
        if (vm->stackTop > vm->stack) {
            Value final_return_val = pop(vm);
            freeValue(&final_return_val);
        }
        if (halted) *halted = true;
        return INTERPRET_OK;
    }

    CallFrame* currentFrame = &vm->frames[vm->frameCount - 1];
    bool has_result = (currentFrame->function_symbol != NULL) &&
                      (currentFrame->function_symbol->type != TYPE_VOID);

    Value safeReturnValue = makeVoid();
    if (has_result) {
        if (vm->stackTop <= currentFrame->slots) {
            runtimeError(vm, "Stack underflow on function return.");
            if (halted) *halted = true;
            return INTERPRET_RUNTIME_ERROR;
        }
        // pop() hands over the stack's reference, so the result moves to
        // the caller as is: copying it and freeing the original cost a
        // locked copy per return and changed nothing.
        safeReturnValue = pop(vm);

        // The declared return type is a coercion sink just like parameter
        // slots (CALL_USER_PROC argument setup) and SET_LOCAL/SET_GLOBAL
        // targets: a function declared to return an int-like type can end
        // its body with a Real-producing expression (Int/Int division boxes
        // a Real), and vice versa. Align the value with the declaration
        // here so `ret expr` behaves like assigning expr to a typed result
        // variable; otherwise the raw Real escapes to the caller (e.g. an
        // Int-declared mean prints "24.000000").
        VarType declared_ret = currentFrame->function_symbol->type;
        if (isRealType(declared_ret) && isIntlikeType(VALUE_TYPE(safeReturnValue))) {
            long double tmp = asLd(safeReturnValue);
            setTypeValue(&safeReturnValue, declared_ret);
            SET_REAL_VALUE(&safeReturnValue, tmp);
        } else if (isIntlikeType(declared_ret) && isRealType(VALUE_TYPE(safeReturnValue))) {
            long double tmp = asLd(safeReturnValue);
            setTypeValue(&safeReturnValue, declared_ret);
            assignRealToIntChecked(vm, &safeReturnValue, tmp);
        }
    }

    for (Value* slot = currentFrame->slots; slot < vm->stackTop; slot++) {
        freeValue(slot);
    }

    vm->ip = currentFrame->return_address;
    vm->stackTop = currentFrame->slots;
    currentFrame->slotCount = 0;

    if (currentFrame->closureEnv) {
        releaseClosureEnv(currentFrame->closureEnv);
        currentFrame->closureEnv = NULL;
        currentFrame->upvalues = NULL;
    } else if (currentFrame->owns_upvalues && currentFrame->upvalues) {
        free(currentFrame->upvalues);
        currentFrame->upvalues = NULL;
    }
    currentFrame->owns_upvalues = false;
    vm->frameCount--;

    if (has_result && !currentFrame->discard_result_on_return) {
        push(vm, safeReturnValue);
    } else {
        freeValue(&safeReturnValue);
    }

    // Signal halt when we've popped the last call frame so the caller can
    // terminate execution gracefully.
    if (halted) {
        *halted = (vm->frameCount == 0);
    }
    return INTERPRET_OK;
}

// --- Bytecode Reading Macros ---
// Your existing READ_BYTE and READ_CONSTANT macros are fine as they implicitly use 'vm' from runVM scope
#define READ_BYTE() (*vm->ip++)
#define READ_CONSTANT() (vm->chunk->constants[READ_BYTE()])
#define READ_CONSTANT16() (vm->chunk->constants[READ_SHORT(vm)])

// Helper function to read a 16-bit short. It will use the READ_BYTE() macro.
static inline uint16_t READ_SHORT(VM* vm_param) { // Pass vm explicitly here
    uint8_t msb = (*vm_param->ip++); // Explicitly use vm_param
    uint8_t lsb = (*vm_param->ip++); // Explicitly use vm_param
    return (uint16_t)(msb << 8) | lsb;
}

static inline uint32_t READ_UINT32(VM* vm_param) {
    uint32_t b1 = (uint32_t)(*vm_param->ip++);
    uint32_t b2 = (uint32_t)(*vm_param->ip++);
    uint32_t b3 = (uint32_t)(*vm_param->ip++);
    uint32_t b4 = (uint32_t)(*vm_param->ip++);
    return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
}

#define READ_HOST_ID() ((HostFunctionID)READ_BYTE())

// --- Fast Stack Helpers -------------------------------------------------
// These still do the minimum bounds check (a comparison + branch, not a
// function call into push()/pop()'s fuller error-path setup) so that the
// load-time verifier's "unknown" abstract-depth regions (see
// bytecode_verify.c: CALL_INDIRECT/CALL_METHOD/CALL_HOST taint depth
// tracking to unknown and stop asserting concrete bounds there) have a real
// runtime backstop, exactly like push()/pop()/peek() already provide. A
// crafted/adversarial .bc file can make these run with a shallower stack
// than the fast-path opcodes (ADD/SUBTRACT/MULTIPLY/DIVIDE/NEGATE/NOT/
// TO_BOOL) expect; without this check that walks vm->stackTop out of the
// VM_s.stack array bounds -- real memory corruption, not just a wrong
// result.
static inline void vmFastPushUnchecked(VM* vm, Value value) {
    size_t used = (size_t)(vm->stackTop - vm->stack);
    if (used >= vm->stackCommittedValues && !vmGrowStackStorage(vm, used + 1)) {
        runtimeError(vm, "VM Error: Stack overflow.");
        return;
    }
    *vm->stackTop = value;
    vm->stackTop++;
}

static inline Value vmFastPopUnchecked(VM* vm) {
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow.");
        return makeNil();
    }
    vm->stackTop--;
    return *vm->stackTop;
}

static inline __attribute__((unused)) Value vmFastPeekChecked(VM* vm, int distance) {
    if (vm->stackTop - vm->stack < distance + 1) {
        runtimeError(vm, "VM Error: Stack underflow (peek too deep).");
        return makeNil();
    }
    return vm->stackTop[-(distance + 1)];
}

#define FAST_PUSH(v) vmFastPushUnchecked(vm, (v))
#define FAST_POP() vmFastPopUnchecked(vm)
#define FAST_PEEK(dist) vmFastPeekChecked(vm, (dist))

// For handlers that use their operands in place through vm->stackTop
// instead of popping them. After a CALL_INDIRECT/CALL_METHOD/CALL_HOST the
// verifier no longer knows the depth, so the stack can be shorter than the
// opcode needs, and vm->stackTop - 1 on an empty stack reads whatever is
// mapped below vm->stack.
static bool vmStackHasOperands(VM* vm, uint8_t opcode, int depth) {
    long have = (long)(vm->stackTop - vm->stack);
    if (have >= depth) {
        return true;
    }
    runtimeError(vm, "VM Error: Stack underflow (%s requires stack depth >= %d but have %ld).",
                 kOpcodeNames[opcode], depth, have);
    return false;
}

static bool vmSizeForVarType(VarType type, long long* out_bytes) {
    if (!out_bytes) {
        return false;
    }
    switch (type) {
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
            *out_bytes = 1;
            return true;
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_WORD:
            *out_bytes = 2;
            return true;
        case TYPE_INT32:
        case TYPE_UINT32:
            *out_bytes = 4;
            return true;
        case TYPE_INT64:
        case TYPE_UINT64:
            *out_bytes = 8;
            return true;
        case TYPE_FLOAT:
            *out_bytes = (long long)sizeof(float);
            return true;
        case TYPE_DOUBLE:
            *out_bytes = (long long)sizeof(double);
            return true;
        case TYPE_LONG_DOUBLE:
            *out_bytes = (long long)sizeof(long double);
            return true;
        case TYPE_POINTER:
        case TYPE_FILE:
        case TYPE_MEMORYSTREAM:
        case TYPE_INTERFACE:
        case TYPE_CLOSURE:
        case TYPE_THREAD:
        case TYPE_TASK:
        case TYPE_CHANNEL:
        case TYPE_MAP:
            *out_bytes = (long long)sizeof(void*);
            return true;
        case TYPE_ENUM:
            *out_bytes = (long long)sizeof(int);
            return true;
        default:
            return false;
    }
}

// --- Symbol Management (VM specific) ---
static Symbol* createSymbolForVM(const char* name, VarType type, AST* type_def_for_value_init) {
    if (!name || name[0] == '\0') { /* ... */ return NULL; }
    Symbol *sym = (Symbol*)malloc(sizeof(Symbol));
    if (!sym) { /* ... */ return NULL; }
    sym->name = strdup(name);
    if (!sym->name) { /* ... */ free(sym); return NULL; }
    toLowerString(sym->name);

    sym->type = type;
    sym->type_def = type_def_for_value_init; // Store the provided type definition AST
    sym->value = (Value*)malloc(sizeof(Value));
    if (!sym->value) { /* ... */ free(sym->name); free(sym); return NULL; }

    // Call makeValueForType with the (now potentially non-NULL) type_def_for_value_init
    *(sym->value) = makeValueForType(type, type_def_for_value_init, sym);
    if (sym->name && strcmp(sym->name, "textattr") == 0) {
        SET_INT_VALUE(sym->value, 7);
    }
    // (debug logging removed)

    sym->is_alias = false;
    sym->is_const = false; // Constants handled at compile time won't use DEFINE_GLOBAL
                           // If VM needs to know about them, another mechanism or flag is needed.
    sym->is_local_var = false;
    sym->is_inline = false;
    sym->closure_captures = false;
    sym->closure_escapes = false;
    sym->next = NULL;
    sym->enclosing = NULL;
    sym->upvalue_count = 0;
    return sym;
}

// Guards every chunk's type_defaults table: THREAD_CREATE and TaskSpawn run
// the same chunk on several VMs at once.
static pthread_mutex_t type_defaults_mutex = PTHREAD_MUTEX_INITIALIZER;

// The chunk's prototype of `type` for type-name constant `name_idx`, or NULL
// if none has been built. Caller holds type_defaults_mutex.
static TypeDefaultProto* vmFindTypeDefault(const BytecodeChunk* chunk, VarType type, uint16_t name_idx) {
    if (name_idx >= chunk->type_defaults_count) {
        return NULL;
    }
    TypeDefaultProto* proto = chunk->type_defaults[name_idx];
    while (proto && proto->type != (uint8_t)type) {
        proto = proto->next;
    }
    return proto;
}

// PUSH_TYPE_DEFAULT's value: the default of `type` for the type registered
// under constant `name_idx`, built from the type table as DEFINE_GLOBAL_SLOT
// builds a global. It is built once per chunk and then shared, so a record
// default costs what the pooled constant it replaces did: createEmptyRecord
// scans the whole procedure table, far too slow to repeat on every call of a
// routine with a record local. Returns NULL after reporting a runtime error.
static const Value* vmTypeDefaultPrototype(VM* vm, VarType type, uint16_t name_idx) {
    BytecodeChunk* chunk = vm->chunk;
    pthread_mutex_lock(&type_defaults_mutex);
    TypeDefaultProto* proto = vmFindTypeDefault(chunk, type, name_idx);
    pthread_mutex_unlock(&type_defaults_mutex);
    if (proto) {
        return &proto->value;
    }

    const char* type_name = NULL;
    if (name_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[name_idx]) == TYPE_STRING) {
        type_name = AS_STRING(chunk->constants[name_idx]);
    }
    AST* type_def = (type_name && type_name[0]) ? lookupType(type_name) : NULL;
    if (!type_def) {
        runtimeError(vm, "VM Error: Type '%s' not found for PUSH_TYPE_DEFAULT.", type_name ? type_name : "?");
        return NULL;
    }
    TypeDefaultProto* built = (TypeDefaultProto*)malloc(sizeof(TypeDefaultProto));
    if (!built) {
        runtimeError(vm, "VM Error: Malloc failed for PUSH_TYPE_DEFAULT.");
        return NULL;
    }
    built->type = (uint8_t)type;
    built->value = makeValueForType(type, type_def, NULL);

    pthread_mutex_lock(&type_defaults_mutex);
    // Another VM may have built the same default meanwhile; keep the first.
    proto = vmFindTypeDefault(chunk, type, name_idx);
    if (!proto && name_idx >= chunk->type_defaults_count) {
        int count = chunk->constants_count > name_idx ? chunk->constants_count : name_idx + 1;
        TypeDefaultProto** grown = (TypeDefaultProto**)realloc(chunk->type_defaults, sizeof(TypeDefaultProto*) * (size_t)count);
        if (grown) {
            memset(grown + chunk->type_defaults_count, 0,
                   sizeof(TypeDefaultProto*) * (size_t)(count - chunk->type_defaults_count));
            chunk->type_defaults = grown;
            chunk->type_defaults_count = count;
        }
    }
    if (!proto && name_idx < chunk->type_defaults_count) {
        built->next = chunk->type_defaults[name_idx];
        chunk->type_defaults[name_idx] = built;
        proto = built;
        built = NULL;
    }
    pthread_mutex_unlock(&type_defaults_mutex);
    if (built) {
        freeValue(&built->value);
        free(built);
    }
    if (!proto) {
        runtimeError(vm, "VM Error: Malloc failed for PUSH_TYPE_DEFAULT.");
        return NULL;
    }
    return &proto->value;
}

// Shared logic for DEFINE_GLOBAL and DEFINE_GLOBAL16.
// Assumes the name has already been read (as Value) and the IP is positioned
// at the declared type byte.
// VM 2.0 Phase 2b (plan §5.7): shared logic for DEFINE_GLOBAL_SLOT, the
// slot-addressed successor to DEFINE_GLOBAL/DEFINE_GLOBAL16 (retired,
// opcodes.def 0x20/0x21). `slot` was resolved from a compile-time name
// index to a link-time slot index by the load-time link step
// (compiler/bytecode_link.c) before this chunk was ever handed to
// interpretBytecode(); the verifier validates slot < global_slot_count for
// every 's'-spec operand, so by the time this runs on a *verified* chunk
// (the cache/file-load path) the index is already known good. Fresh
// compiler output (the other path into this function) is trusted by the
// same pre-existing model that lets it skip verification entirely, and the
// calling frontend's main.c links it (once, after saving to cache) before
// ever reaching interpretBytecode() -- but this still
// defensively re-checks, matching the security posture the Phase 1e
// follow-up established for this exact function (a previous version of
// this code read constant-pool indices with no runtime bounds check).
//
// Symbol allocation/ownership is UNCHANGED from the pre-2b design: the
// Symbol this creates or finds is still owned by vm->vmGlobalSymbols (the
// same hash table other consumers -- stdin/stdout/input/output setup,
// nullifyPointerAliasesByAddrValue, debug dumps -- still look up by name).
// The only new step is the last line: mirroring that same Symbol* into
// chunk->global_slots[slot] as a non-owning fast-path index, so
// GET_GSLOT/SET_GSLOT never need to consult the hash table at all.
//
// `define_pc` is the code offset of the DEFINE_GLOBAL_SLOT instruction being
// executed. Finding the slot already populated is only worth a "redefined"
// warning when some *other* instruction populated it -- a second declaration
// of the same global name. A declaration in the body of a top-level loop
// compiles to a DEFINE inside the loop (there is no function frame to hold a
// local), so the very same instruction legitimately runs once per iteration;
// re-initializing the value is the intended behaviour there and the warning
// is noise, so it is suppressed when define_pc matches what the slot already
// recorded.
static InterpretResult handleDefineGlobalSlot(VM* vm, uint16_t slot, int define_pc) {
    if (slot >= (uint16_t)vm->chunk->global_slot_count || !vm->chunk->global_slot_names) {
        runtimeError(vm, "VM Error: DEFINE_GLOBAL_SLOT slot %u out of range.", (unsigned)slot);
        return INTERPRET_RUNTIME_ERROR;
    }
    const char* varName = vm->chunk->global_slot_names[slot];
    Symbol* sym = NULL;
    // True only when a *different* instruction already defined this slot; a
    // re-run of this same DEFINE (a top-level loop body) is not a redeclaration.
    const bool warn_on_redefine = (vm->chunk->global_slots[slot].define_pc != define_pc);
    // A declaration in a block of the main program lives in a slot named
    // "<name>@<n>" (see TopLevelBlockVar in compiler.c); name the variable in
    // the warning as the source spells it.
    const int sourceNameLen = (int)strcspn(varName, "@");
    VarType declaredType = (VarType)READ_BYTE();

    if (declaredType == TYPE_ARRAY) {
        uint8_t dimension_count = READ_BYTE();
        int* lower_bounds = NULL;
        int* upper_bounds = NULL;

        if (dimension_count > 0) {
            lower_bounds = malloc(sizeof(int) * dimension_count);
            upper_bounds = malloc(sizeof(int) * dimension_count);
            if (!lower_bounds || !upper_bounds) {
                runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                if (lower_bounds) free(lower_bounds);
                if (upper_bounds) free(upper_bounds);
                return INTERPRET_RUNTIME_ERROR;
            }

            for (int i = 0; i < dimension_count; i++) {
                uint16_t lower_idx = READ_SHORT(vm);
                uint16_t upper_idx = READ_SHORT(vm);
                if (lower_idx >= vm->chunk->constants_count || upper_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Array bound constant index out of range for '%s'.", varName);
                    free(lower_bounds); free(upper_bounds);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value lower_val = vm->chunk->constants[lower_idx];
                Value upper_val = vm->chunk->constants[upper_idx];
                if (!isIntlikeType(VALUE_TYPE(lower_val)) || !isIntlikeType(VALUE_TYPE(upper_val))) {
                    runtimeError(vm, "VM Error: Invalid constant types for array bounds of '%s'.", varName);
                    free(lower_bounds); free(upper_bounds);
                    return INTERPRET_RUNTIME_ERROR;
                }
                lower_bounds[i] = (int)VAL_INT(lower_val);
                upper_bounds[i] = (int)VAL_INT(upper_val);
            }
        }

        VarType elem_var_type = (VarType)READ_BYTE();
        uint16_t elem_name_idx = READ_SHORT(vm);
        if (elem_name_idx >= vm->chunk->constants_count) {
            runtimeError(vm, "VM Error: Array element type constant index out of range for '%s'.", varName);
            if (lower_bounds) free(lower_bounds);
            if (upper_bounds) free(upper_bounds);
            return INTERPRET_RUNTIME_ERROR;
        }
        Value elem_name_val = vm->chunk->constants[elem_name_idx];
        AST* elem_type_def = NULL;
        if (VALUE_TYPE(elem_name_val) == TYPE_STRING && AS_STRING(elem_name_val) && AS_STRING(elem_name_val)[0] != '\0') {
            elem_type_def = lookupType(AS_STRING(elem_name_val));
        }

        Value array_value;
        if (dimension_count > 0) {
            array_value = makeArrayND(dimension_count, lower_bounds, upper_bounds,
                                    elem_var_type, elem_type_def);
        } else {
            array_value = makeEmptyArray(elem_var_type, elem_type_def);
        }
        if (lower_bounds) free(lower_bounds);
        if (upper_bounds) free(upper_bounds);
        if (dimension_count > 0 && ARRAY_DIMENSIONS(array_value) == 0) {
            runtimeError(vm, "VM Error: Failed to allocate array for global '%s'.", varName);
            freeValue(&array_value);
            return INTERPRET_RUNTIME_ERROR;
        }

        sym = hashTableLookup(vm->vmGlobalSymbols, varName);
        if (sym == NULL) {
            sym = (Symbol*)malloc(sizeof(Symbol));
            if (!sym) {
                runtimeError(vm, "VM Error: Malloc failed for Symbol struct for global array '%s'.", varName);
                freeValue(&array_value);
                return INTERPRET_RUNTIME_ERROR;
            }
            sym->name = strdup(varName);
            if (!sym->name) {
                runtimeError(vm, "VM Error: Malloc failed for symbol name for global array '%s'.", varName);
                free(sym); freeValue(&array_value);
                return INTERPRET_RUNTIME_ERROR;
            }
            toLowerString(sym->name);
            sym->type = declaredType;
            sym->type_def = NULL;
            sym->value = (Value*)malloc(sizeof(Value));
            if (!sym->value) {
                runtimeError(vm, "VM Error: Malloc failed for Value struct for global array '%s'.", varName);
                free(sym->name); free(sym); freeValue(&array_value);
                return INTERPRET_RUNTIME_ERROR;
            }
            *(sym->value) = array_value;
            sym->is_alias = false;
            sym->is_const = false;
            sym->is_local_var = false;
            sym->is_inline = false;
            sym->next = NULL;
            sym->enclosing = NULL;
            sym->upvalue_count = 0;
            hashTableInsert(vm->vmGlobalSymbols, sym);
        } else {
            if (warn_on_redefine) {
                runtimeWarning(vm, "VM Warning: Global variable '%.*s' redefined.", sourceNameLen, varName);
            }
            freeValue(sym->value);
            *(sym->value) = array_value;
        }
    } else {
        uint16_t type_name_idx = READ_SHORT(vm);
        VarType file_element_type = TYPE_VOID;
        uint16_t file_element_name_idx = 0xFFFF;
        if (declaredType == TYPE_FILE) {
            file_element_type = (VarType)READ_BYTE();
            file_element_name_idx = READ_SHORT(vm);
        }
        int str_len = 0;
        uint16_t len_idx = 0;
        if (declaredType == TYPE_STRING) {
            len_idx = READ_SHORT(vm);
            if (len_idx >= vm->chunk->constants_count) {
                runtimeError(vm, "VM Error: String length constant index out of range for '%s'.", varName);
                return INTERPRET_RUNTIME_ERROR;
            }
            Value len_val = vm->chunk->constants[len_idx];
            if (VALUE_TYPE(len_val) == TYPE_INTEGER) {
                str_len = (int)VAL_INT(len_val);
            }
        }
        if (type_name_idx >= vm->chunk->constants_count) {
            runtimeError(vm, "VM Error: Type name constant index out of range for '%s'.", varName);
            return INTERPRET_RUNTIME_ERROR;
        }
        Value typeNameVal = vm->chunk->constants[type_name_idx];
        // (debug logging removed)
        AST* type_def_node = NULL;
        // True when type_def_node was built here rather than looked up: the
        // symbol takes it over when created, otherwise it is freed below.
        bool type_def_built = false;
        if (declaredType == TYPE_STRING && str_len > 0) {
            Token* strTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, strTok);
            type_def_built = true;
            setTypeAST(type_def_node, TYPE_STRING);
            freeToken(strTok);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", str_len);
            Token* lenTok = newToken(TOKEN_INTEGER_CONST, buf, 0, 0);
            AST* lenNode = newASTNode(AST_NUMBER, lenTok);
            setTypeAST(lenNode, TYPE_INTEGER);
            freeToken(lenTok);
            setRight(type_def_node, lenNode);
        } else if (VALUE_TYPE(typeNameVal) == TYPE_STRING && AS_STRING(typeNameVal)) {
            // Prefer user-defined type resolution if available
            AST* looked = lookupType(AS_STRING(typeNameVal));
            if (declaredType == TYPE_POINTER && looked) {
                type_def_node = looked; // Will be a POINTER_TYPE or TYPE_REFERENCE
            } else if (declaredType == TYPE_POINTER) {
                // Fall back to simple base types mapping
                Token* baseTok = newToken(TOKEN_IDENTIFIER, AS_STRING(typeNameVal), 0, 0);
                type_def_node = newASTNode(AST_VARIABLE, baseTok);
                type_def_built = true;
                const char* tn = AS_STRING(typeNameVal);
                if      (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) setTypeAST(type_def_node, TYPE_INT32);
                else if (strcasecmp(tn, "real")    == 0 || strcasecmp(tn, "double") == 0) setTypeAST(type_def_node, TYPE_DOUBLE);
                else if (strcasecmp(tn, "single")  == 0 || strcasecmp(tn, "float")  == 0) setTypeAST(type_def_node, TYPE_FLOAT);
                else if (strcasecmp(tn, "char")    == 0) setTypeAST(type_def_node, TYPE_CHAR);
                else if (strcasecmp(tn, "widechar")== 0) setTypeAST(type_def_node, TYPE_WIDECHAR);
                else if (strcasecmp(tn, "unicodestring")== 0) setTypeAST(type_def_node, TYPE_UNICODE_STRING);
                else if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) setTypeAST(type_def_node, TYPE_BOOLEAN);
                else if (strcasecmp(tn, "byte")    == 0) setTypeAST(type_def_node, TYPE_BYTE);
                else if (strcasecmp(tn, "word")    == 0) setTypeAST(type_def_node, TYPE_WORD);
                else if (strcasecmp(tn, "int64")   == 0 || strcasecmp(tn, "longint") == 0) setTypeAST(type_def_node, TYPE_INT64);
                else if (strcasecmp(tn, "cardinal")== 0) setTypeAST(type_def_node, TYPE_UINT32);
                else setTypeAST(type_def_node, TYPE_VOID);
                freeToken(baseTok);
            } else {
                type_def_node = looked;
                if (declaredType == TYPE_ENUM && type_def_node == NULL) {
                    runtimeError(vm, "VM Error: Enum type '%s' not found for global '%s'.", AS_STRING(typeNameVal), varName);
                    return INTERPRET_RUNTIME_ERROR;
                }
            }
        }

        if (declaredType == TYPE_POINTER && type_def_node == NULL) {
            // Final safety: default pointer base to integer if not provided
            Token* baseTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, baseTok);
            type_def_built = true;
            setTypeAST(type_def_node, TYPE_INT32);
            freeToken(baseTok);
        }

        sym = hashTableLookup(vm->vmGlobalSymbols, varName);
        if (sym == NULL) {
            sym = createSymbolForVM(varName, declaredType, type_def_node);
            if (!sym) {
                runtimeError(vm, "VM Error: Failed to create symbol for global '%s'.", varName);
                return INTERPRET_RUNTIME_ERROR;
            }
            hashTableInsert(vm->vmGlobalSymbols, sym);
        } else {
            if (warn_on_redefine) {
                runtimeWarning(vm, "VM Warning: Global variable '%.*s' redefined.", sourceNameLen, varName);
            } else if (sym->value) {
                // This same DEFINE running again (a declaration in a
                // top-level loop body): start the variable over from its
                // type's default each iteration, as the array branch above
                // does and as a function local would, instead of carrying
                // the previous iteration's value forward.
                replaceValueCell(sym->value, makeValueForType(declaredType, sym->type_def, sym), NULL);
            }
            if (type_def_built) {
                freeAST(type_def_node);
                type_def_node = NULL;
            }
        }

        if (declaredType == TYPE_FILE && sym && sym->value) {
            if (file_element_type != TYPE_VOID && file_element_type != TYPE_UNKNOWN) {
                FILE_ELEMENT_TYPE(*sym->value) = file_element_type;
                long long bytes = 0;
                if (vmSizeForVarType(file_element_type, &bytes) && bytes > 0 && bytes <= INT_MAX) {
                    FILE_RECORD_SIZE(*sym->value) = (int)bytes;
                    FILE_RECORD_SIZE_EXPLICIT(*sym->value) = true;
                }
            }
            if (file_element_name_idx != 0xFFFF && file_element_name_idx < vm->chunk->constants_count) {
                Value elem_name_val = vm->chunk->constants[file_element_name_idx];
                if (VALUE_TYPE(elem_name_val) == TYPE_STRING && AS_STRING(elem_name_val) && AS_STRING(elem_name_val)[0] != '\0') {
                    AST* elem_def = lookupType(AS_STRING(elem_name_val));
                    if (elem_def) {
                        FILE_ELEMENT_TYPE_DEF(*sym->value) = elem_def;
                    }
                }
            }
        }
    }

    // Mirror into the per-chunk slot table (non-owning: vm->vmGlobalSymbols
    // remains the sole owner of `sym`). Caller holds globals_mutex.
    vm->chunk->global_slots[slot].symbol = sym;
    vm->chunk->global_slots[slot].define_pc = define_pc;
    return INTERPRET_OK;
}

// Determine if a core VM builtin requires access to global interpreter
// structures protected by globals_mutex. Builtins that do not touch such
// structures can execute without acquiring the global lock.
static bool builtinUsesGlobalStructures(const char* name) {
    if (!name) return false;

    /*
     * Builtins listed here read or modify interpreter globals declared in
     * Pascal/globals.c (symbol tables, IO state, CRT state, etc.). They must
     * execute while holding globals_mutex to avoid races with other
     * interpreter threads touching the same shared state.
     */
    static const char* const needs_lock[] = {
        "append",         "assign",        "biblinktext",   "biboldtext",
        "biclrscr",       "bilowvideo",    "binormvideo",   "biunderlinetext",
        "biwherex",       "biwherey",      "blinktext",     "boldtext",
        "close",          "clreol",        "clrscr",        "cursoroff",
        "cursoron",       "deline",        "dispose",       "eof",
        "erase",          "gotoxy",        "hidecursor",    "highvideo",
        "ioresult",       "insline",       "invertcolors",  "lowvideo",
        "normvideo",      "normalcolors",  "paramcount",    "paramstr",
        "rename",         "reset",
        "rewrite",        "screenrows",    "screencols",    "showcursor",
        "textbackground", "textbackgrounde","textcolor",     "textcolore",
        "underlinetext",  "window",        "wherex",        "wherey",
    };

    for (size_t i = 0; i < sizeof(needs_lock)/sizeof(needs_lock[0]); i++) {
        if (strcasecmp(name, needs_lock[i]) == 0) return true;
    }
    return false;
}

typedef enum {
    VM_BUILTIN_CACHE_UNKNOWN = 0,
    VM_BUILTIN_CACHE_FALSE = 1,
    VM_BUILTIN_CACHE_TRUE = 2
} VmBuiltinBoolCacheState;

typedef enum {
    VM_BUILTIN_TYPE_CACHE_UNKNOWN = 0,
    VM_BUILTIN_TYPE_CACHE_NONE = 1,
    VM_BUILTIN_TYPE_CACHE_PROCEDURE = 2,
    VM_BUILTIN_TYPE_CACHE_FUNCTION = 3
} VmBuiltinTypeCacheState;

static pthread_mutex_t gVmBuiltinMetadataCacheMutex = PTHREAD_MUTEX_INITIALIZER;
// _Atomic: read unlocked on the fast path (below) from any TaskSpawn'd
// worker VM sharing this process's builtin metadata; gVmBuiltinMetadataCacheMutex
// still serializes the compute-and-populate slow path (harmless if raced --
// every writer computes the same value for a given builtin_id -- but the
// mutex avoids redundant concurrent recomputation on a cache miss).
static _Atomic uint8_t gVmBuiltinNeedsLockCache[UINT16_MAX + 1];
static _Atomic uint8_t gVmBuiltinTypeCache[UINT16_MAX + 1];

static bool vmBuiltinNeedsGlobalLockCached(int builtin_id, const char* fallback_name) {
    if (builtin_id >= 0 && builtin_id <= UINT16_MAX) {
        uint8_t cached = atomic_load(&gVmBuiltinNeedsLockCache[builtin_id]);
        if (cached == VM_BUILTIN_CACHE_TRUE) {
            return true;
        }
        if (cached == VM_BUILTIN_CACHE_FALSE) {
            return false;
        }

        const char* name = getVmBuiltinNameById(builtin_id);
        if (!name || !*name) {
            name = fallback_name;
        }
        bool needs_lock = builtinUsesGlobalStructures(name);

        pthread_mutex_lock(&gVmBuiltinMetadataCacheMutex);
        if (atomic_load(&gVmBuiltinNeedsLockCache[builtin_id]) == VM_BUILTIN_CACHE_UNKNOWN) {
            atomic_store(&gVmBuiltinNeedsLockCache[builtin_id], needs_lock ? VM_BUILTIN_CACHE_TRUE : VM_BUILTIN_CACHE_FALSE);
        }
        pthread_mutex_unlock(&gVmBuiltinMetadataCacheMutex);
        return needs_lock;
    }

    return builtinUsesGlobalStructures(fallback_name);
}

static BuiltinRoutineType vmBuiltinTypeCached(int builtin_id, const char* fallback_name) {
    if (builtin_id >= 0 && builtin_id <= UINT16_MAX) {
        uint8_t cached = atomic_load(&gVmBuiltinTypeCache[builtin_id]);
        switch (cached) {
            case VM_BUILTIN_TYPE_CACHE_NONE:
                return BUILTIN_TYPE_NONE;
            case VM_BUILTIN_TYPE_CACHE_PROCEDURE:
                return BUILTIN_TYPE_PROCEDURE;
            case VM_BUILTIN_TYPE_CACHE_FUNCTION:
                return BUILTIN_TYPE_FUNCTION;
            default:
                break;
        }

        const char* name = getVmBuiltinNameById(builtin_id);
        if (!name || !*name) {
            name = fallback_name;
        }
        BuiltinRoutineType builtin_type = name ? getBuiltinType(name) : BUILTIN_TYPE_NONE;
        uint8_t encoded = VM_BUILTIN_TYPE_CACHE_NONE;
        if (builtin_type == BUILTIN_TYPE_PROCEDURE) {
            encoded = VM_BUILTIN_TYPE_CACHE_PROCEDURE;
        } else if (builtin_type == BUILTIN_TYPE_FUNCTION) {
            encoded = VM_BUILTIN_TYPE_CACHE_FUNCTION;
        }

        pthread_mutex_lock(&gVmBuiltinMetadataCacheMutex);
        if (atomic_load(&gVmBuiltinTypeCache[builtin_id]) == VM_BUILTIN_TYPE_CACHE_UNKNOWN) {
            atomic_store(&gVmBuiltinTypeCache[builtin_id], encoded);
        }
        pthread_mutex_unlock(&gVmBuiltinMetadataCacheMutex);
        return builtin_type;
    }

    return fallback_name ? getBuiltinType(fallback_name) : BUILTIN_TYPE_NONE;
}

/* VM 2.0 Phase 6 (Docs/pscal_vm2_plan.md §6.3): shared enforce/replay gate for
 * both builtin-call opcodes (CALL_BUILTIN_PROC and CALL_BUILTIN), called
 * right before the handler would otherwise run. pscalFxPolicyActive() is a
 * cheap check (no mutex, no per-id lookup) that short-circuits to INTERPRET_OK
 * with *out_mask left at FX_PURE when neither a deny-list nor record/replay
 * is active -- this is the "zero cost when no policy is set" path.
 *
 * On a denied builtin or a replay journal mismatch, calls runtimeError()
 * itself and returns INTERPRET_RUNTIME_ERROR; the caller still owns stack/arg
 * cleanup (identical to the existing "unknown builtin" error paths) since the
 * cleanup shape differs slightly between the two opcodes. On a successful
 * replay substitution, fills *out_result and sets *out_replayed so the caller
 * skips invoking the real handler. *out_mask is filled either way so the
 * caller can decide whether to journal the result after a live call. */
static InterpretResult vmApplyFxPolicy(VM* vm, int effective_id, const char* effective_name,
                                        int arg_count, Value* args,
                                        Value* out_result, bool* out_replayed, EffectMask* out_mask) {
    *out_replayed = false;
    *out_mask = FX_PURE;
    if (!pscalFxPolicyActive()) return INTERPRET_OK;

    EffectMask fx_mask = getVmBuiltinEffectMaskById(effective_id);
    *out_mask = fx_mask;
    if (fx_mask == FX_PURE) return INTERPRET_OK;

    EffectMask denied = pscalFxEffectiveDeniedMask();
    if (denied & fx_mask) {
        runtimeError(vm, "VM Error: builtin '%s' denied by --deny/PSCAL_VM_DENY policy (effect mask 0x%x intersects denied 0x%x).",
                     effective_name ? effective_name : "?", (unsigned)fx_mask, (unsigned)denied);
        return INTERPRET_RUNTIME_ERROR;
    }

    /* Denied above if the sandbox says so; otherwise a journal-exempt
     * builtin runs live and is not recorded (FX_PURE stops the caller's
     * pscalFxRecordCall). */
    if (pscalFxBuiltinIsJournalExempt(getVmBuiltinNameById(effective_id))) {
        *out_mask = FX_PURE;
        return INTERPRET_OK;
    }

    if (pscalFxReplayActive()) {
        char mismatch[256];
        PscalFxReplayOutcome outcome = pscalFxReplayCall(vm, effective_name ? effective_name : "?", arg_count, args,
                                                         out_result, mismatch, sizeof(mismatch));
        if (outcome == PSCAL_FX_REPLAY_MISMATCH) {
            runtimeError(vm, "VM Error: fx replay journal mismatch calling '%s': %s",
                         effective_name ? effective_name : "?", mismatch);
            return INTERPRET_RUNTIME_ERROR;
        }
        /* PSCAL_FX_REPLAY_RUN_LIVE: this call wasn't substitutable when
         * recorded (e.g. a file/handle writeback) -- leave *out_replayed
         * false so the caller invokes the real handler, exactly as happened
         * during recording. */
        *out_replayed = (outcome == PSCAL_FX_REPLAY_SUBSTITUTED);
    }
    return INTERPRET_OK;
}

// --- Main Interpretation Loop ---
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* const_globals, HashTable* procedures, uint32_t entry) {
    if (!vm || !chunk) return INTERPRET_RUNTIME_ERROR;

    /* Defensive: ensure symbol tables exist so DEFINE_GLOBAL and similar ops
     * never dereference an uninitialized pointer (can happen after frontend
     * state swaps on iOS). */
    if (!globals) {
        globals = createHashTable();
    }
    if (!const_globals) {
        const_globals = createHashTable();
    }

    vm->chunk = chunk;

    // VM 2.0 Phase 2a (plan §5.6): one-time-per-chunk prep before any
    // instruction runs. `chunk` can be shared across multiple interpretBytecode()
    // calls (THREAD_CREATE spawns a new VM against the *same* chunk pointer;
    // an embedded shell closure's own chunk can likewise be invoked more than
    // once, potentially from different threads), so this is guarded by
    // globals_mutex -- the same lock already used for other per-chunk shared
    // state. By this point the VM 2.0 Phase 2b link step (compiler/
    // bytecode_link.c) has already run -- either in cache.c's
    // loadBytecodeFromCache()/loadBytecodeFromFile() (loaded chunk, before
    // verification) or, for a fresh compile, in the calling frontend's
    // main.c (called once, immediately after saveBytecodeToCache() and
    // before interpretBytecode() -- deliberately NOT inside
    // compileASTToBytecode() itself, since a chunk it returns may still be
    // written to the .bc cache in its pre-link form; see that function's
    // comment) -- both of which happen strictly before any chunk reaches
    // interpretBytecode(), so chunk->global_slots is already populated and
    // chunk->code already has its GET_GSLOT/SET_GSLOT/DEFINE_GLOBAL_SLOT
    // operands rewritten from
    // name indices to slot indices. That ordering matters here specifically
    // because pscalProtectChunkCode() below makes `code` read-only -- the
    // link step's in-place rewrite would not be safe to run after this.
    // Double-checked: the unlocked read must acquire, pairing with the
    // release store below, so a thread that sees `true` also sees the
    // caches table and the protected code it guards.
    if (!atomic_load_explicit(&chunk->prepared_for_execution, memory_order_acquire)) {
        pthread_mutex_lock(&globals_mutex);
        if (!atomic_load_explicit(&chunk->prepared_for_execution, memory_order_relaxed)) {
            if (chunk->cache_count > 0 && !chunk->caches) {
                chunk->caches = (CacheSlot*)calloc((size_t)chunk->cache_count, sizeof(CacheSlot));
            }
            bool caches_ok = (chunk->cache_count == 0) || (chunk->caches != NULL);
            bool protect_ok = caches_ok && pscalProtectChunkCode(chunk);
            if (caches_ok && protect_ok) {
                atomic_store_explicit(&chunk->prepared_for_execution, true, memory_order_release);
            }
            pthread_mutex_unlock(&globals_mutex);
            if (!caches_ok) {
                runtimeError(vm, "VM Error: out of memory allocating global-access cache table.");
                return INTERPRET_RUNTIME_ERROR;
            }
            if (!protect_ok) {
                runtimeError(vm, "VM Error: failed to prepare bytecode for execution.");
                return INTERPRET_RUNTIME_ERROR;
            }
        } else {
            pthread_mutex_unlock(&globals_mutex);
        }
    }

    vm->ip = vm->chunk->code + entry;
    vm->lastInstruction = vm->ip;
    vm->abort_requested = false;
    vm->runtime_error_raised = false;
    vm->suspend_unwind_requested = false;
    vm->zeroBasedIndexing = frontendIsZeroBasedStrings();

    vm->vmGlobalSymbols = globals;    // Store globals table (ensure this is the intended one)
    vm->vmConstGlobalSymbols = const_globals; // Table of constant globals (no locking)
    vm->procedureTable = procedures; // <--- STORED procedureTable
    vmPopulateProcedureAddressCache(vm);

#if VM_USE_COMPUTED_GOTO
    // VM 2.0 Phase 1a: full 256-entry dispatch table (reserved-range dispatch,
    // plan Docs/pscal_vm2_plan.md §5.1). Unused opcode values (holes in the
    // 0x00-0x63 published page, and the whole 0x64-0xFF reserved space) point
    // at LABEL_INVALID so `goto *dispatch_table[instruction_val]` is always
    // safe for any uint8_t opcode byte with no bounds check on the hot path.
    static void *dispatch_table[256];
    static bool dispatch_table_ready = false;
    if (!dispatch_table_ready) {
        for (int dt_i = 0; dt_i < 256; ++dt_i) {
            dispatch_table[dt_i] = &&LABEL_INVALID;
        }
#define OP(name, value, operands, stack_in, stack_out) dispatch_table[value] = &&LABEL_##name;
#include "compiler/opcodes.def"
#undef OP
        dispatch_table_ready = true;
    }
#endif

    bool opcode_profile_enabled = vmOpcodeProfileIsEnabled();
    const volatile bool *pending_exit_flag = shellRuntimePendingExitFlag();

    // Initialize stream globals lazily. These are shared symbols, so avoid
    // per-run/per-thread rebinding that can race across concurrent VMs.
    if (vm->vmGlobalSymbols) {
        FILE* runtime_stdin = pscalRuntimeVmStdin();
        FILE* runtime_stdout = pscalRuntimeVmStdout();
        FILE* runtime_stderr = pscalRuntimeVmStderr();
        pthread_mutex_lock(&globals_mutex);
        Symbol* stdinSym = hashTableLookup(vm->vmGlobalSymbols, "stdin");
        if (stdinSym && stdinSym->value &&
            VALUE_TYPE(*stdinSym->value) == TYPE_FILE &&
            AS_FILE(*stdinSym->value) == NULL) {
            AS_FILE(*stdinSym->value) = runtime_stdin;
        }

        Symbol* stdoutSym = hashTableLookup(vm->vmGlobalSymbols, "stdout");
        if (stdoutSym && stdoutSym->value &&
            VALUE_TYPE(*stdoutSym->value) == TYPE_FILE &&
            AS_FILE(*stdoutSym->value) == NULL) {
            AS_FILE(*stdoutSym->value) = runtime_stdout;
        }

        Symbol* stderrSym = hashTableLookup(vm->vmGlobalSymbols, "stderr");
        if (stderrSym && stderrSym->value &&
            VALUE_TYPE(*stderrSym->value) == TYPE_FILE &&
            AS_FILE(*stderrSym->value) == NULL) {
            AS_FILE(*stderrSym->value) = runtime_stderr;
        }

        Symbol* inputSym = hashTableLookup(vm->vmGlobalSymbols, "input");
        if (inputSym && inputSym->value &&
            VALUE_TYPE(*inputSym->value) == TYPE_FILE &&
            AS_FILE(*inputSym->value) == NULL) {
            AS_FILE(*inputSym->value) = runtime_stdin;
        }

        Symbol* outputSym = hashTableLookup(vm->vmGlobalSymbols, "output");
        if (outputSym && outputSym->value &&
            VALUE_TYPE(*outputSym->value) == TYPE_FILE &&
            AS_FILE(*outputSym->value) == NULL) {
            AS_FILE(*outputSym->value) = runtime_stdout;
        }
        pthread_mutex_unlock(&globals_mutex);
    }

    // Establish a base call frame for the main program if none has been
    // installed yet. Threads that set up their own initial frame prior to
    // invoking the interpreter can skip this.
    if (vm->frameCount == 0) {
        CallFrame* baseFrame = &vm->frames[vm->frameCount++];
        baseFrame->return_address = NULL;
        baseFrame->slots = vm->stack;
        baseFrame->function_symbol = NULL;
        baseFrame->slotCount = 0;
        baseFrame->locals_count = 0;
        baseFrame->upvalue_count = 0;
        baseFrame->upvalues = NULL;
        baseFrame->vtable = NULL;
    }

    #ifdef DEBUG
    if (dumpExec) { // from all.txt
        printf("\n--- VM Initial State ---\n");
        printf("IP: %p (offset 0)\n", (void*)vm->ip);
        printf("Stack top: %p (empty)\n", (void*)vm->stackTop);
        printf("Chunk code: %p, Chunk constants: %p\n", (void*)vm->chunk->code, (void*)vm->chunk->constants);
        printf("Global symbol table (for VM): %p\n", (void*)vm->vmGlobalSymbols);
        printf("Const global symbol table: %p\n", (void*)vm->vmConstGlobalSymbols);
        printf("Procedure table (for disassembly): %p\n", (void*)vm->procedureTable); // Debug print
        printf("------------------------\n");
    }
    #endif

#define BINARY_OP(op_char_for_error_msg, current_instruction_code) \
    do { \
        Value b_val_popped = FAST_POP(); \
        Value a_val_popped = FAST_POP(); \
        Value result_val; \
        bool op_is_handled = false; \
        \
        /* String/char concatenation for ADD */ \
        if (current_instruction_code == ADD) { \
            /* Optimization: Fast path for common integer arithmetic */ \
            if (VALUE_TYPE(a_val_popped) == TYPE_INT32 && VALUE_TYPE(b_val_popped) == TYPE_INT32) { \
                 long long iresult; \
                 if (__builtin_add_overflow(VAL_INT(a_val_popped), VAL_INT(b_val_popped), &iresult)) { \
                     runtimeError(vm, "Runtime Error: Integer overflow."); \
                     freeValue(&a_val_popped); freeValue(&b_val_popped); \
                     return INTERPRET_RUNTIME_ERROR; \
                 } \
                 result_val = pscalIntResultNarrow(iresult); \
                 op_is_handled = true; \
            } else { \
                /* Optimization: Resolve pointers without deep copying for string/char operands */ \
                Value* final_a = &a_val_popped; \
                while (VALUE_TYPE(*final_a) == TYPE_POINTER && AS_POINTER(*final_a)) { \
                    final_a = (Value*)AS_POINTER(*final_a); \
                } \
                Value* final_b = &b_val_popped; \
                while (VALUE_TYPE(*final_b) == TYPE_POINTER && AS_POINTER(*final_b)) { \
                    final_b = (Value*)AS_POINTER(*final_b); \
                } \
                /* A string + anything concatenates (the other side is \
                 * stringified). A char + a non-text value does so only \
                 * under Aether, whose `s[i]` is a one-character Text and \
                 * whose Text + number stringifies (D7); everywhere else a \
                 * char is an ordinal and char +/- int is arithmetic. */ \
                bool concat_text_any = frontendIsAether(); \
                if (IS_STRING(*final_a) || \
                    (IS_CHAR(*final_a) && \
                     (IS_STRING(*final_b) || IS_CHAR(*final_b) || concat_text_any))) { \
                    char a_buffer[5] = {0}; \
                    char b_buffer[5] = {0}; \
                    char* coerced_b = NULL; \
                    const char* s_a = NULL; \
                    const char* s_b = NULL; \
                    VarType result_type = \
                        (VALUE_TYPE(*final_a) == TYPE_UNICODE_STRING || VALUE_TYPE(*final_b) == TYPE_UNICODE_STRING || \
                         VALUE_TYPE(*final_a) == TYPE_WIDECHAR || VALUE_TYPE(*final_b) == TYPE_WIDECHAR) \
                        ? TYPE_UNICODE_STRING \
                        : TYPE_STRING; \
                    if (IS_STRING(*final_a)) { \
                        s_a = AS_STRING(*final_a) ? AS_STRING(*final_a) : ""; \
                    } else { \
                        if (VALUE_TYPE(*final_a) == TYPE_WIDECHAR) { \
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(*final_a), a_buffer); \
                        } else { \
                            a_buffer[0] = (char)AS_CHAR(*final_a); \
                        } \
                        s_a = a_buffer; \
                    } \
                    if (IS_STRING(*final_b)) { \
                        s_b = AS_STRING(*final_b) ? AS_STRING(*final_b) : ""; \
                    } else if (IS_CHAR(*final_b)) { \
                        if (VALUE_TYPE(*final_b) == TYPE_WIDECHAR) { \
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(*final_b), b_buffer); \
                        } else { \
                            b_buffer[0] = (char)AS_CHAR(*final_b); \
                        } \
                        s_b = b_buffer; \
                    } else { \
                        coerced_b = vmStringifyValueForConcat(final_b); \
                        if (!coerced_b) { \
                            runtimeError(vm, "Runtime Error: Failed to stringify right operand for concatenation."); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                        } \
                        s_b = coerced_b; \
                    } \
                    size_t len_a = strlen(s_a); \
                    size_t len_b = strlen(s_b); \
                    if (len_b > SIZE_MAX - len_a - 1) { \
                        free(coerced_b); \
                        runtimeError(vm, "Runtime Error: String concatenation overflow."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    size_t total_len = len_a + len_b; \
                    char* temp_concat_buffer = NULL; \
                    StringObj *a_concat_obj = (final_a == &a_val_popped && VALUE_TYPE(a_val_popped) == TYPE_STRING) \
                        ? PSCAL_VALUE_PTR(a_val_popped, StringObj) : NULL; \
                    /* VM 2.0 Phase 4j (Stage B): this realloc-in-place fast \
                     * path mutates a_val_popped's buffer directly -- only \
                     * safe when nothing else shares this StringObj (e.g. a \
                     * retain-shared string constant/variable copyValueForStack \
                     * now hands out): reallocating/nulling a shared buffer \
                     * here corrupted every other alias, including the \
                     * bytecode constant pool's own permanent copy of a \
                     * string literal used in more than one concatenation. \
                     * Falls through to the always-safe fresh-malloc path \
                     * whenever the object is still shared. */ \
                    if (a_concat_obj && a_concat_obj->buffer && \
                        atomic_load_explicit(&a_concat_obj->header.refcount, memory_order_acquire) == 1) { \
                        temp_concat_buffer = (char*)realloc(a_concat_obj->buffer, total_len + 1); \
                        if (!temp_concat_buffer) { \
                            free(coerced_b); \
                            runtimeError(vm, "Runtime Error: Realloc failed for string concatenation buffer."); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                        } \
                        memcpy(temp_concat_buffer + len_a, s_b, len_b); \
                        temp_concat_buffer[total_len] = '\0'; \
                        a_concat_obj->buffer = NULL; \
                    } else { \
                        temp_concat_buffer = (char*)malloc(total_len + 1); \
                        if (!temp_concat_buffer) { \
                            free(coerced_b); \
                            runtimeError(vm, "Runtime Error: Malloc failed for string concatenation buffer."); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                        } \
                        memcpy(temp_concat_buffer, s_a, len_a); \
                        memcpy(temp_concat_buffer + len_a, s_b, len_b); \
                        temp_concat_buffer[total_len] = '\0'; \
                    } \
                    result_val = makeOwnedString(temp_concat_buffer, total_len); \
                    { \
                        /* Re-tag after the retype below stomps .bits back to \
                         * a nil-pointer placeholder -- reset_rs is the real \
                         * StringObj makeOwnedString already allocated. */ \
                        StringObj *reset_rs = PSCAL_VALUE_PTR(result_val, StringObj); \
                        SET_VALUE_TYPE(&result_val, result_type); \
                        pscalValueSetHeapPtrBits(&result_val, reset_rs); \
                    } \
                    free(coerced_b); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    op_is_handled = true; \
                } else if (IS_STRING(*final_b) || (IS_CHAR(*final_b) && concat_text_any)) { \
                    char* coerced_a = vmStringifyValueForConcat(final_a); \
                    char b_buffer[5] = {0}; \
                    const char* s_a = NULL; \
                    const char* s_b = NULL; \
                    VarType result_type = \
                        (VALUE_TYPE(*final_b) == TYPE_UNICODE_STRING || VALUE_TYPE(*final_b) == TYPE_WIDECHAR) \
                        ? TYPE_UNICODE_STRING \
                        : TYPE_STRING; \
                    if (!coerced_a) { \
                        runtimeError(vm, "Runtime Error: Failed to stringify left operand for concatenation."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    s_a = coerced_a; \
                    if (IS_STRING(*final_b)) { \
                        s_b = AS_STRING(*final_b) ? AS_STRING(*final_b) : ""; \
                    } else { \
                        if (VALUE_TYPE(*final_b) == TYPE_WIDECHAR) { \
                            encodeUtf8Codepoint((uint32_t)AS_CHAR(*final_b), b_buffer); \
                        } else { \
                            b_buffer[0] = (char)AS_CHAR(*final_b); \
                        } \
                        s_b = b_buffer; \
                    } \
                    size_t len_a = strlen(s_a); \
                    size_t len_b = strlen(s_b); \
                    if (len_b > SIZE_MAX - len_a - 1) { \
                        free(coerced_a); \
                        runtimeError(vm, "Runtime Error: String concatenation overflow."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    size_t total_len = len_a + len_b; \
                    char* temp_concat_buffer = (char*)malloc(total_len + 1); \
                    if (!temp_concat_buffer) { \
                        free(coerced_a); \
                        runtimeError(vm, "Runtime Error: Malloc failed for string concatenation buffer."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    memcpy(temp_concat_buffer, s_a, len_a); \
                    memcpy(temp_concat_buffer + len_a, s_b, len_b); \
                    temp_concat_buffer[total_len] = '\0'; \
                    result_val = makeOwnedString(temp_concat_buffer, total_len); \
                    { \
                        /* Re-tag after the retype below stomps .bits back to \
                         * a nil-pointer placeholder -- reset_rs is the real \
                         * StringObj makeOwnedString already allocated. */ \
                        StringObj *reset_rs = PSCAL_VALUE_PTR(result_val, StringObj); \
                        SET_VALUE_TYPE(&result_val, result_type); \
                        pscalValueSetHeapPtrBits(&result_val, reset_rs); \
                    } \
                    free(coerced_a); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    op_is_handled = true; \
                } else { \
                    /* Fallback for non-string types: preserve original pointer resolution behavior */ \
                    while (VALUE_TYPE(a_val_popped) == TYPE_POINTER && AS_POINTER(a_val_popped)) { \
                        Value tmp = copyValueForStack(AS_POINTER(a_val_popped)); \
                        freeValue(&a_val_popped); \
                        a_val_popped = tmp; \
                    } \
                    while (VALUE_TYPE(b_val_popped) == TYPE_POINTER && AS_POINTER(b_val_popped)) { \
                        Value tmp = copyValueForStack(AS_POINTER(b_val_popped)); \
                        freeValue(&b_val_popped); \
                        b_val_popped = tmp; \
                    } \
                } \
            } \
        } \
        \
        /* Char +/- intlike handled as numeric ordinal operations */ \
\
        /* Enum +/- intlike */ \
        if (!op_is_handled) { \
            if (current_instruction_code == ADD || current_instruction_code == SUBTRACT) { \
                bool a_enum_b_int = (VALUE_TYPE(a_val_popped) == TYPE_ENUM && IS_INTLIKE(b_val_popped)); \
                bool a_int_b_enum = (IS_INTLIKE(a_val_popped) && VALUE_TYPE(b_val_popped) == TYPE_ENUM); \
                if (a_enum_b_int || a_int_b_enum) { \
                    Value enum_value = a_enum_b_int ? a_val_popped : b_val_popped; \
                    Value int_val  = a_enum_b_int ? b_val_popped : a_val_popped; \
                    long long delta = asI64(int_val); \
                    int new_ord = AS_ENUM(enum_value).ordinal + \
                        ((current_instruction_code == ADD) ? (int)delta : -(int)delta); \
                    if (ENUM_META(enum_value) && \
                        (new_ord < 0 || new_ord >= ENUM_META(enum_value)->member_count)) { \
                        runtimeError(vm, "Runtime Error: Enum '%s' out of range.", \
                                     AS_ENUM(enum_value).enum_name ? AS_ENUM(enum_value).enum_name : "<anon>"); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    result_val = makeEnum(AS_ENUM(enum_value).enum_name, new_ord); \
                    ENUM_META(result_val) = ENUM_META(enum_value); \
                    ENUM_TYPE_DEF(result_val) = ENUM_TYPE_DEF(enum_value); \
                    op_is_handled = true; \
                } \
            } \
        } \
        \
        /* Set union/difference/intersection */ \
        if (!op_is_handled) { \
            if (VALUE_TYPE(a_val_popped) == TYPE_SET && VALUE_TYPE(b_val_popped) == TYPE_SET) { \
                switch (current_instruction_code) { \
                    case ADD: \
                        result_val = setUnion(a_val_popped, b_val_popped); \
                        break; \
                    case SUBTRACT: \
                        result_val = setDifference(a_val_popped, b_val_popped); \
                        break; \
                    case MULTIPLY: \
                        result_val = setIntersection(a_val_popped, b_val_popped); \
                        break; \
                    default: \
                        runtimeError(vm, "Runtime Error: Unsupported set operation '%s'.", op_char_for_error_msg); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                } \
                op_is_handled = true; \
            } \
        } \
        \
        /* Numeric arithmetic (INTEGER/BYTE/WORD/REAL) */ \
        if (!op_is_handled) { \
            if (IS_NUMERIC(a_val_popped) && IS_NUMERIC(b_val_popped)) { \
                bool a_real = IS_REAL(a_val_popped); \
                bool b_real = IS_REAL(b_val_popped); \
                if ((a_real || b_real) && \
                    vmIsDoubleOrFloat(VALUE_TYPE(a_val_popped)) && \
                    vmIsDoubleOrFloat(VALUE_TYPE(b_val_popped))) { \
                    double da = VAL_REAL64(a_val_popped); \
                    double db = VAL_REAL64(b_val_popped); \
                    if (current_instruction_code == DIVIDE && db == 0.0) { \
                        runtimeError(vm, "Runtime Error: Division by zero."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    switch (current_instruction_code) { \
                        case ADD:      result_val = vmMakeDoubleValue(da + db); break; \
                        case SUBTRACT: result_val = vmMakeDoubleValue(da - db); break; \
                        case MULTIPLY: result_val = vmMakeDoubleValue(da * db); break; \
                        case DIVIDE:   result_val = vmMakeDoubleValue(da / db); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for real numbers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } else if (a_real || b_real) { \
                    /* asLd reads its operands; it never retypes them. */ \
                    long double fa = asLd(a_val_popped); \
                    long double fb = asLd(b_val_popped); \
                    if (current_instruction_code == DIVIDE && fb == 0.0L) { \
                        runtimeError(vm, "Runtime Error: Division by zero."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    int useLong = (VALUE_TYPE(a_val_popped) == TYPE_LONG_DOUBLE || VALUE_TYPE(b_val_popped) == TYPE_LONG_DOUBLE); \
                    switch (current_instruction_code) { \
                        case ADD:      result_val = useLong ? makeLongDouble(fa + fb) : makeReal(fa + fb); break; \
                        case SUBTRACT: result_val = useLong ? makeLongDouble(fa - fb) : makeReal(fa - fb); break; \
                        case MULTIPLY: result_val = useLong ? makeLongDouble(fa * fb) : makeReal(fa * fb); break; \
                        case DIVIDE:   result_val = useLong ? makeLongDouble(fa / fb) : makeReal(fa / fb); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for real numbers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } else { \
                    long long ia = asI64(a_val_popped); \
                    long long ib = asI64(b_val_popped); \
                    if (current_instruction_code == DIVIDE && ib == 0) { \
                        runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    long long iresult = 0; \
                    bool overflow = false; \
                    /* A UINT64 operand: exact unsigned arithmetic, not the \
                     * signed reading of its bits (2^63 - 1 was an overflow). */ \
                    bool uint64_op = (VALUE_TYPE(a_val_popped) == TYPE_UINT64 || \
                                      VALUE_TYPE(b_val_popped) == TYPE_UINT64); \
                    unsigned long long uresult = 0; \
                    switch (current_instruction_code) { \
                        case ADD: \
                        case SUBTRACT: \
                        case MULTIPLY: \
                            if (uint64_op) { \
                                overflow = !vmUInt64Arith(current_instruction_code, a_val_popped, b_val_popped, &uresult); \
                            } else if (current_instruction_code == ADD) { \
                                overflow = __builtin_add_overflow(ia, ib, &iresult); \
                            } else if (current_instruction_code == SUBTRACT) { \
                                overflow = __builtin_sub_overflow(ia, ib, &iresult); \
                            } else { \
                                overflow = __builtin_mul_overflow(ia, ib, &iresult); \
                            } \
                            break; \
                        case DIVIDE: \
                            result_val = makeReal(asLd(a_val_popped) / asLd(b_val_popped)); \
                            break; \
                        case MOD: \
                            iresult = (ib == 0 || ib == -1) ? 0 : ia % ib; \
                            break; \
        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for integers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                    if (current_instruction_code == DIVIDE) { \
                        /* result_val already set for division */ \
                    } else if (uint64_op && !overflow && current_instruction_code != MOD) { \
                        result_val = makeUInt64(uresult); \
                    } else if (overflow) { \
                        runtimeError(vm, "Runtime Error: Integer overflow."); \
                        freeValue(&a_val_popped); \
                        freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } else { \
                        result_val = pscalIntResultLike2(a_val_popped, b_val_popped, iresult); \
                    } \
                } \
                op_is_handled = true; \
            } \
        } \
        \
        if (!op_is_handled) { \
            runtimeError(vm, "Runtime Error: Operands must be numbers for arithmetic operation '%s' (or strings/chars for '+'). Got %s and %s", \
                         op_char_for_error_msg, varTypeToString(VALUE_TYPE(a_val_popped)), varTypeToString(VALUE_TYPE(b_val_popped))); \
            freeValue(&a_val_popped); \
            freeValue(&b_val_popped); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        FAST_PUSH(result_val); \
        freeValue(&a_val_popped); \
        freeValue(&b_val_popped); \
    } while (false)

    uint8_t instruction_val;
    for (;;) {
#if defined(PSCAL_TARGET_IOS)
        if (vmIsRootExecutor(vm)) {
            if (vprocWaitIfStopped(vprocCurrent())) {
                if (vmIosDebugEnabled()) {
                    long ip_offset = -1;
                    if (vm->chunk && vm->chunk->code && vm->ip) {
                        ip_offset = (long)(vm->ip - vm->chunk->code);
                    }
                    vmIosDebugLogf("[vm-loop] root stop-wait returned vm=%p ip=%ld abort=%d exit=%d suspend=%d",
                                   (void*)vm,
                                   ip_offset,
                                   vm->abort_requested ? 1 : 0,
                                   vm->exit_requested ? 1 : 0,
                                   vm->suspend_unwind_requested ? 1 : 0);
                }
                continue;
            }
        }
#endif
#if !defined(PSCAL_TARGET_IOS)
        // Both consumers below are no-ops unless a signal flag or one of
        // these VM flags is set, so test those inline first: this check runs
        // before every instruction.
        if (pscalRuntimeSignalAttention() ||
            vm->abort_requested || vm->exit_requested ||
            (vm->threadOwner && (vm->threadOwner->abort_requested ||
                                 vm->threadOwner->exit_requested)))
#endif
        {
            if (vmConsumeSuspendRequest(vm)) {
                /* Marked as a cooperative stop request; unwind below via exit_requested. */
            }
            if (vmConsumeInterruptRequest(vm)) {
                /* VM abort flag is set; let the normal exit/abort handling run. */
            }
        }
        if (pending_exit_flag && *pending_exit_flag) {
            shellRuntimeMaybeRequestPendingExit(vm);
        }
        // The previous instruction reported an error but did not return one
        // (a push()/pop()/peek() failure, or a handler that reports and
        // falls through). Stop here. The unwind below is for exits and
        // interrupts: it leaves one frame and carries on, and at top level
        // it halts with INTERPRET_OK, so a failed program would exit 0.
        // Code that clears abort_requested after reporting has handled the
        // error itself (the shell recovers from some builtin failures that
        // way), so that case carries on as before.
        if (vm->runtime_error_raised) {
            if (vm->abort_requested) {
                return INTERPRET_RUNTIME_ERROR;
            }
            vm->runtime_error_raised = false;
        }
        if (vm->exit_requested || vm->abort_requested) {
            if (shellRuntimeShouldDeferExit(vm)) {
                continue;
            }
            bool suspend_unwind = vm->suspend_unwind_requested && !vm->abort_requested;
            bool halted = false;
            InterpretResult res = returnFromCall(vm, &halted);
            if (res != INTERPRET_OK) {
                return res;
            }
            if (halted) {
                vm->exit_requested = false;
                vm->abort_requested = false;
                vm->suspend_unwind_requested = false;
                return INTERPRET_OK;
            }
            vm->exit_requested = suspend_unwind;
            vm->abort_requested = false;
            continue;
        }
        vm->lastInstruction = vm->ip;
/* #ifdef DEBUG
        if (dumpExec) {
            fprintf(stderr,"VM Stack: ");
            for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
                fprintf(stderr,"[ ");
                printValueToStream(*slot, stderr);
                fprintf(stderr," ]");
            }
            fprintf(stderr,"\n");
            disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code), vm->procedureTable);
        }
        #endif */
        //vmDumpStackInfo(vm); // Call new helper at the start of each instruction

        instruction_val = READ_BYTE();
        if (opcode_profile_enabled) {
            vmOpcodeProfileRecord(instruction_val);
        }
        if (vm->trace_head_instructions > 0 && vm->trace_executed < vm->trace_head_instructions) {
            int offset = (int)(vm->ip - vm->chunk->code) - 1;
            long stacksz = (long)(vm->stackTop - vm->stack);
            fprintf(stderr, "[VM-TRACE] IP=%04d OPC=%u STACK=%ld\n", offset, (unsigned)instruction_val, stacksz);
            vm->trace_executed++;
        }
#if VM_USE_COMPUTED_GOTO
        goto *dispatch_table[instruction_val];

#define OP(name, value, operands, stack_in, stack_out) \
LABEL_##name: \
        instruction_val = name; \
        goto dispatch_switch;
#include "compiler/opcodes.def"
#undef OP
LABEL_INVALID:
        instruction_val = OPCODE_COUNT;
        goto dispatch_switch;
#endif

#if VM_USE_COMPUTED_GOTO
dispatch_switch:
#endif
        switch (instruction_val) {
            case RETURN: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case EXIT: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case CONSTANT: {
                Value constant = READ_CONSTANT();
                push(vm, copyValueForStackUnlocked(&constant));
                break;
            }
                
            case CONSTANT16: {
                uint16_t idx = READ_SHORT(vm);
                if (idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Constant index %u out of bounds for CONSTANT16.", idx);
                   return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStackUnlocked(&vm->chunk->constants[idx]));
                break;
            }
            case CONST_0:
                FAST_PUSH(makeInt(0));
                break;
            case CONST_1:
                FAST_PUSH(makeInt(1));
                break;
            case CONST_TRUE:
                FAST_PUSH(makeBoolean(true));
                break;
            case CONST_FALSE:
                FAST_PUSH(makeBoolean(false));
                break;
            case PUSH_IMMEDIATE_INT8: {
                uint8_t raw = READ_BYTE();
                long long imm = (raw <= 0x7F) ? (long long)raw : ((long long)raw - 0x100LL);
                FAST_PUSH(makeInt(imm));
                break;
            }

            case GET_CHAR_ADDRESS: {
                if (!vmStackHasOperands(vm, GET_CHAR_ADDRESS, 2)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value index_val = pop(vm);
                Value* string_ptr_val = vm->stackTop - 1; // Peek at the string pointer

                if (VALUE_TYPE(*string_ptr_val) != TYPE_POINTER || !AS_POINTER(*string_ptr_val) ||
                    !isPascalStringType(VALUE_TYPE(*(Value*)AS_POINTER(*string_ptr_val)))) {
                    runtimeError(vm, "VM Error: Base for character index is not a pointer to a string.");
                    freeValue(&index_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!isIntlikeType(VALUE_TYPE(index_val))) {
                    runtimeError(vm, "VM Error: String index must be an integer.");
                    freeValue(&index_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                long long pscal_index = VAL_INT(index_val);
                freeValue(&index_val);

                Value* string_val = (Value*)AS_POINTER(*string_ptr_val);

                // VM 2.0 Phase 4j (Stage B): see the matching comment in
                // GET_ELEMENT_ADDRESS.
                valueEnsureUnique(string_val);

                // Pin the character before releasing the base pointer.
                Value popped_string_ptr = pop(vm);
                bool pushed = vmPushStringCharAddress(vm, string_val, pscal_index, false);
                freeValue(&popped_string_ptr);
                if (!pushed) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            // GET_GLOBAL_ADDRESS/GET_GLOBAL_ADDRESS16 (0x24/0x27) are
            // retired as of VM 2.0 Phase 2b -- superseded by
            // GET_GSLOT_ADDRESS below.
            case GET_GSLOT_ADDRESS: {
                uint16_t slot = READ_SHORT(vm);
                if (slot >= (uint16_t)vm->chunk->global_slot_count) {
                    runtimeError(vm, "VM Error: Global slot %u out of bounds for GET_GSLOT_ADDRESS.", (unsigned)slot);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->chunk->global_myself_slot >= 0 && (int)slot == vm->chunk->global_myself_slot) {
                    push(vm, makePointer(&vm->threadMyself, NULL));
                    break;
                }

                Symbol* sym = vm->chunk->global_slots[slot].symbol;
                if (!sym || !sym->value) {
                    const char* name = vm->chunk->global_slot_names ? vm->chunk->global_slot_names[slot] : "?";
                    runtimeError(vm, "Runtime Error: Global '%s' not found in symbol table.", name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(sym->value, NULL));
                break;
            }
            case GET_LOCAL_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(&frame->slots[slot], NULL));
                break;
            }
            case ADD:      BINARY_OP("+", instruction_val); break;
            case SUBTRACT: BINARY_OP("-", instruction_val); break;
            case MULTIPLY: BINARY_OP("*", instruction_val); break;
            case DIVIDE:   BINARY_OP("/", instruction_val); break;

            case NEGATE: {
                Value val_popped = FAST_POP();
                Value result_val;
                if (IS_INTEGER(val_popped)) {
                    long long iv = AS_INTEGER(val_popped);
                    if (iv == LLONG_MIN && VALUE_TYPE(val_popped) != TYPE_UINT64) {
                        runtimeError(vm, "Runtime Error: Integer overflow.");
                        freeValue(&val_popped);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    result_val = pscalIntResultLike1(val_popped, (long long)(0ULL - (unsigned long long)iv));
                }
                else if (IS_REAL(val_popped)) {
                    if (VALUE_TYPE(val_popped) == TYPE_LONG_DOUBLE) result_val = makeLongDouble(-AS_REAL(val_popped));
                    else result_val = makeReal(-AS_REAL(val_popped));
                }
                else {
                    runtimeError(vm, "Runtime Error: Operand for negate must be a number.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                FAST_PUSH(result_val);
                freeValue(&val_popped);
                break;
            }
            case NOT: {
                Value val_popped = FAST_POP();
                bool condition_truth = false;
                if (!coerceValueToBoolean(&val_popped, &condition_truth)) {
                    runtimeError(vm, "Runtime Error: Operand for boolean conversion must be boolean or numeric.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                FAST_PUSH(makeBoolean(!condition_truth));
                freeValue(&val_popped);
                break;
            }
            case TO_BOOL: {
                Value val_popped = FAST_POP();
                bool condition_truth = false;
                if (!coerceValueToBoolean(&val_popped, &condition_truth)) {
                    runtimeError(vm, "Runtime Error: Operand for boolean conversion must be boolean or numeric.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                FAST_PUSH(makeBoolean(condition_truth));
                freeValue(&val_popped);
                break;
            }
            case SWAP: {
                if (vm->stackTop - vm->stack < 2) {
                    runtimeError(vm, "VM Error: Not enough values on stack to swap.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Perform an in-place swap of the top two Value structs on the stack.
                Value temp = vm->stackTop[-1];
                vm->stackTop[-1] = vm->stackTop[-2];
                vm->stackTop[-2] = temp;
                break;
            }
            case DUP: {
                if (vm->stackTop == vm->stack) {
                    runtimeError(vm, "VM Error: Stack underflow (dup from empty stack).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStackUnlocked(&vm->stackTop[-1]));
                break;
            }
            case AND:
            case OR:
            case XOR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;

                if (VALUE_TYPE(a_val) == TYPE_INT32 && VALUE_TYPE(b_val) == TYPE_INT32) {
                    long long ia = VAL_INT(a_val);
                    long long ib = VAL_INT(b_val);
                    if (instruction_val == AND) {
                        result_val = makeInt(ia & ib);
                    } else if (instruction_val == OR) {
                        result_val = makeInt(ia | ib);
                    } else {
                        result_val = makeInt(ia ^ ib);
                    }
                } else if (IS_BOOLEAN(a_val) && IS_BOOLEAN(b_val)) {
                    bool ba = AS_BOOLEAN(a_val);
                    bool bb = AS_BOOLEAN(b_val);
                    if (instruction_val == AND) {
                        result_val = makeBoolean(ba && bb);
                    } else if (instruction_val == OR) {
                        result_val = makeBoolean(ba || bb);
                    } else {
                        result_val = makeBoolean(ba ^ bb);
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val))  {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (instruction_val == AND) {
                        result_val = pscalIntResultLike2(a_val, b_val, ia & ib);
                    } else if (instruction_val == OR) {
                        result_val = pscalIntResultLike2(a_val, b_val, ia | ib);
                    } else {
                        result_val = pscalIntResultLike2(a_val, b_val, ia ^ ib);
                    }
                } else {
                    runtimeError(vm, "Runtime Error: Operands for AND/OR/XOR must be both Boolean or both Integer. Got %s and %s.",
                                 varTypeToString(VALUE_TYPE(a_val)), varTypeToString(VALUE_TYPE(b_val)));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case INT_DIV: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (VALUE_TYPE(a_val) == TYPE_INT32 && VALUE_TYPE(b_val) == TYPE_INT32) {
                    int32_t ia = (int32_t)VAL_INT(a_val);
                    int32_t ib = (int32_t)VAL_INT(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Integer division by zero.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (ia == INT32_MIN && ib == -1) {
                        // Prevent x86 hardware trap and preserve 64-bit promotion semantics
                        push(vm, makeInt64(2147483648LL));
                    } else {
                        push(vm, makeInt((long long)(ia / ib)));
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Integer division by zero.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(a_val) == TYPE_UINT64 || VALUE_TYPE(b_val) == TYPE_UINT64) {
                        unsigned long long uq = 0;
                        if (!vmUInt64DivMod(a_val, b_val, false, &uq)) {
                            runtimeError(vm, "Runtime Error: Integer overflow.");
                            freeValue(&a_val); freeValue(&b_val);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        push(vm, makeUInt64(uq));
                        freeValue(&a_val);
                        freeValue(&b_val);
                        break;
                    }
                    if (ia == LLONG_MIN && ib == -1) {
                        runtimeError(vm, "Runtime Error: Integer overflow.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, pscalIntResultLike2(a_val, b_val, ia / ib));
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'int_div' must be integers. Got %s and %s.",
                                 varTypeToString(VALUE_TYPE(a_val)), varTypeToString(VALUE_TYPE(b_val)));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case MOD: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (VALUE_TYPE(a_val) == TYPE_INT32 && VALUE_TYPE(b_val) == TYPE_INT32) {
                    int32_t ia = (int32_t)VAL_INT(a_val);
                    int32_t ib = (int32_t)VAL_INT(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Modulo by zero.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (ia == INT32_MIN && ib == -1) {
                        // For mod, division by -1 is always exactly representable, modulo is 0
                        push(vm, makeInt(0));
                    } else {
                        push(vm, makeInt((long long)(ia % ib)));
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Modulo by zero.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(a_val) == TYPE_UINT64 || VALUE_TYPE(b_val) == TYPE_UINT64) {
                        unsigned long long ur = 0;
                        vmUInt64DivMod(a_val, b_val, true, &ur); // |a mod b| <= |a|: cannot overflow
                        push(vm, makeUInt64(ur));
                        freeValue(&a_val);
                        freeValue(&b_val);
                        break;
                    }
                    // LLONG_MIN % -1 overflows (it traps on x86-64); any
                    // x mod -1 is 0.
                    push(vm, pscalIntResultLike2(a_val, b_val, ib == -1 ? 0 : ia % ib));
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'mod' must be integers. Got %s and %s.",
                                 varTypeToString(VALUE_TYPE(a_val)), varTypeToString(VALUE_TYPE(b_val)));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case SHL:
            case SHR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (VALUE_TYPE(a_val) == TYPE_INT32 && VALUE_TYPE(b_val) == TYPE_INT32) {
                    long long ia = VAL_INT(a_val);
                    long long ib = VAL_INT(b_val);
                    if (ib < 0) {
                        runtimeError(vm, "Runtime Error: Shift amount cannot be negative.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    // Widen like the slow path: makeInt() tagged INT32 and
                    // dropped the high bits (`1 << 32` was 0).
                    push(vm, pscalIntResultNarrow(vmShiftInt64(ia, ib, instruction_val == SHL, false)));
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib < 0) {
                        runtimeError(vm, "Runtime Error: Shift amount cannot be negative.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, pscalIntResultLike2(a_val, b_val,
                                                 vmShiftInt64(ia, ib, instruction_val == SHL,
                                                              VALUE_TYPE(a_val) == TYPE_UINT64)));
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'shl' and 'shr' must be integers. Got %s and %s.",
                                 varTypeToString(VALUE_TYPE(a_val)), varTypeToString(VALUE_TYPE(b_val)));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case EQUAL:
            case NOT_EQUAL:
            case GREATER:
            case GREATER_EQUAL:
            case LESS:
            case LESS_EQUAL: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;
                bool comparison_succeeded = false;

                // Handle explicit NIL-to-NIL comparisons first.  Pointer/NIL
                // comparisons are handled in the pointer block below.

                // Optimization: Fast path for TYPE_INT32 comparisons.
                // Bypasses the overhead of IS_NUMERIC and asI64 type resolution switches
                // which provides significant performance gains in hot loops.
                if (VALUE_TYPE(a_val) == TYPE_INT32 && VALUE_TYPE(b_val) == TYPE_INT32) {
                    long long ia = VAL_INT(a_val);
                    long long ib = VAL_INT(b_val);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(ia == ib); break;
                        case NOT_EQUAL:     result_val = makeBoolean(ia != ib); break;
                        case GREATER:       result_val = makeBoolean(ia >  ib); break;
                        case GREATER_EQUAL: result_val = makeBoolean(ia >= ib); break;
                        case LESS:          result_val = makeBoolean(ia <  ib); break;
                        case LESS_EQUAL:    result_val = makeBoolean(ia <= ib); break;
                        default: goto comparison_error_label;
                    }
                    comparison_succeeded = true;
                } else
                if (VALUE_TYPE(a_val) == TYPE_NIL && VALUE_TYPE(b_val) == TYPE_NIL) {
                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(true);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(false);
                    } else {
                        goto comparison_error_label;
                    }
                    comparison_succeeded = true;
                }
                // Numeric comparison (Integers and Reals)
                else if (IS_NUMERIC(a_val) && IS_NUMERIC(b_val)) {
                    bool a_real = isRealType(VALUE_TYPE(a_val));
                    bool b_real = isRealType(VALUE_TYPE(b_val));

                    if (vmIsDoubleOrFloat(VALUE_TYPE(a_val)) && vmIsDoubleOrFloat(VALUE_TYPE(b_val))) {
                        double da = VAL_REAL64(a_val);
                        double db = VAL_REAL64(b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(da == db); break;
                            case NOT_EQUAL:     result_val = makeBoolean(da != db); break;
                            case GREATER:       result_val = makeBoolean(da >  db); break;
                            case GREATER_EQUAL: result_val = makeBoolean(da >= db); break;
                            case LESS:          result_val = makeBoolean(da <  db); break;
                            case LESS_EQUAL:    result_val = makeBoolean(da <= db); break;
                            default: goto comparison_error_label;
                        }
                    } else if (a_real || b_real) {
                        long double fa = asLd(a_val);
                        long double fb = asLd(b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(fa == fb); break;
                            case NOT_EQUAL:     result_val = makeBoolean(fa != fb); break;
                            case GREATER:       result_val = makeBoolean(fa >  fb); break;
                            case GREATER_EQUAL: result_val = makeBoolean(fa >= fb); break;
                            case LESS:          result_val = makeBoolean(fa <  fb); break;
                            case LESS_EQUAL:    result_val = makeBoolean(fa <= fb); break;
                            default: goto comparison_error_label;
                        }
                    } else {
                        int cmp = vmCompareIntlike(a_val, b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(cmp == 0); break;
                            case NOT_EQUAL:     result_val = makeBoolean(cmp != 0); break;
                            case GREATER:       result_val = makeBoolean(cmp >  0); break;
                            case GREATER_EQUAL: result_val = makeBoolean(cmp >= 0); break;
                            case LESS:          result_val = makeBoolean(cmp <  0); break;
                            case LESS_EQUAL:    result_val = makeBoolean(cmp <= 0); break;
                            default: goto comparison_error_label;
                        }
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_STRING(b_val)) || (IS_STRING(a_val) && IS_CHAR(b_val))) {
                    char a_text[5] = {0};
                    char b_text[5] = {0};
                    const char *sa = NULL;
                    const char *sb = NULL;
                    size_t la = 0;
                    size_t lb = 0;

                    // Explicit lengths: a char is always one character, so
                    // chr(0) is not the empty string (as a C string it was).
                    if (IS_STRING(a_val)) {
                        sa = AS_STRING(a_val) ? AS_STRING(a_val) : "";
                        la = strlen(sa);
                    } else if (VALUE_TYPE(a_val) == TYPE_WIDECHAR) {
                        la = encodeUtf8Codepoint((uint32_t)AS_CHAR(a_val), a_text);
                        sa = a_text;
                    } else {
                        a_text[0] = (char)AS_CHAR(a_val);
                        la = 1;
                        sa = a_text;
                    }

                    if (IS_STRING(b_val)) {
                        sb = AS_STRING(b_val) ? AS_STRING(b_val) : "";
                        lb = strlen(sb);
                    } else if (VALUE_TYPE(b_val) == TYPE_WIDECHAR) {
                        lb = encodeUtf8Codepoint((uint32_t)AS_CHAR(b_val), b_text);
                        sb = b_text;
                    } else {
                        b_text[0] = (char)AS_CHAR(b_val);
                        lb = 1;
                        sb = b_text;
                    }

                    int cmp = memcmp(sa, sb, la < lb ? la : lb);
                    if (cmp == 0) {
                        cmp = (la > lb) - (la < lb);
                    }
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(cmp == 0); break;
                        case NOT_EQUAL:     result_val = makeBoolean(cmp != 0); break;
                        case GREATER:       result_val = makeBoolean(cmp > 0);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(cmp >= 0); break;
                        case LESS:          result_val = makeBoolean(cmp < 0);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(cmp <= 0); break;
                        default:
                            goto comparison_error_label;
                    }
                    comparison_succeeded = true;
                }
                else if (IS_STRING(a_val) && IS_STRING(b_val)) {
                    const char* sa = AS_STRING(a_val) ? AS_STRING(a_val) : "";
                    const char* sb = AS_STRING(b_val) ? AS_STRING(b_val) : "";
                    int cmp = strcmp(sa, sb);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(cmp == 0); break;
                        case NOT_EQUAL:     result_val = makeBoolean(cmp != 0); break;
                        case GREATER:       result_val = makeBoolean(cmp > 0);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(cmp >= 0); break;
                        case LESS:          result_val = makeBoolean(cmp < 0);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(cmp <= 0); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected string comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_INTEGER(b_val)) || (IS_INTEGER(a_val) && IS_CHAR(b_val))) {
                    int char_val = IS_CHAR(a_val) ? AS_CHAR(a_val) : AS_CHAR(b_val);
                    long long int_val = IS_INTEGER(a_val) ? AS_INTEGER(a_val) : AS_INTEGER(b_val);

                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean((long long)char_val == int_val); break;
                        case NOT_EQUAL:     result_val = makeBoolean((long long)char_val != int_val); break;
                        case GREATER:       result_val = makeBoolean((long long)char_val > int_val);  break;
                        case GREATER_EQUAL: result_val = makeBoolean((long long)char_val >= int_val); break;
                        case LESS:          result_val = makeBoolean((long long)char_val < int_val);  break;
                        case LESS_EQUAL:    result_val = makeBoolean((long long)char_val <= int_val); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char/integer comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Char comparison
                else if (IS_CHAR(a_val) && IS_CHAR(b_val)) {
                    int ca = AS_CHAR(a_val);
                    int cb = AS_CHAR(b_val);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(ca == cb); break;
                        case NOT_EQUAL:     result_val = makeBoolean(ca != cb); break;
                        case GREATER:       result_val = makeBoolean(ca > cb);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(ca >= cb); break;
                        case LESS:          result_val = makeBoolean(ca < cb);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(ca <= cb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Boolean comparison
                else if ((IS_BOOLEAN(a_val) && (IS_BOOLEAN(b_val) || IS_INTEGER(b_val))) ||
                         (IS_INTEGER(a_val) && IS_BOOLEAN(b_val))) {
                          bool ba = IS_BOOLEAN(a_val) ? AS_BOOLEAN(a_val) : (AS_INTEGER(a_val) != 0);
                          bool bb = IS_BOOLEAN(b_val) ? AS_BOOLEAN(b_val) : (AS_INTEGER(b_val) != 0);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(ba == bb); break;
                        case NOT_EQUAL:     result_val = makeBoolean(ba != bb); break;
                        case GREATER:       result_val = makeBoolean(ba > bb);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(ba >= bb); break;
                        case LESS:          result_val = makeBoolean(ba < bb);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(ba <= bb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected boolean comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // ENUM comparison
                else if (VALUE_TYPE(a_val) == TYPE_ENUM && VALUE_TYPE(b_val) == TYPE_ENUM) {
                    const char *name_a = AS_ENUM(a_val).enum_name;
                    const char *name_b = AS_ENUM(b_val).enum_name;
                    bool types_match = (name_a && name_b && strcmp(name_a, name_b) == 0);
                    int ord_a = AS_ENUM(a_val).ordinal;
                    int ord_b = AS_ENUM(b_val).ordinal;

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(types_match && (ord_a == ord_b));
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!types_match || (ord_a != ord_b));
                    } else {
                        if (!types_match) {
                            runtimeError(vm,
                                         "Runtime Error: Cannot compare different ENUM types ('%s' vs '%s') with relational operator.",
                                         name_a ? name_a : "<anon>",
                                         name_b ? name_b : "<anon>");
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                        switch (instruction_val) {
                            case GREATER:       result_val = makeBoolean(ord_a > ord_b);  break;
                            case GREATER_EQUAL: result_val = makeBoolean(ord_a >= ord_b); break;
                            case LESS:          result_val = makeBoolean(ord_a < ord_b);  break;
                            case LESS_EQUAL:    result_val = makeBoolean(ord_a <= ord_b); break;
                            default:
                                runtimeError(vm, "VM Error: Unexpected enum comparison opcode %d.", instruction_val);
                                freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    comparison_succeeded = true;
                }
                // Memory stream and NIL comparison
                else if ((VALUE_TYPE(a_val) == TYPE_MEMORYSTREAM || VALUE_TYPE(a_val) == TYPE_NIL) &&
                         (VALUE_TYPE(b_val) == TYPE_MEMORYSTREAM || VALUE_TYPE(b_val) == TYPE_NIL)) {
                    MStream* ms_a = (VALUE_TYPE(a_val) == TYPE_MEMORYSTREAM) ? AS_MSTREAM(a_val) : NULL;
                    MStream* ms_b = (VALUE_TYPE(b_val) == TYPE_MEMORYSTREAM) ? AS_MSTREAM(b_val) : NULL;
                    bool streams_equal = false;
                    if (VALUE_TYPE(a_val) == TYPE_NIL && VALUE_TYPE(b_val) == TYPE_NIL) {
                        streams_equal = true;
                    } else if (VALUE_TYPE(a_val) == TYPE_NIL) {
                        streams_equal = (ms_b == NULL);
                    } else if (VALUE_TYPE(b_val) == TYPE_NIL) {
                        streams_equal = (ms_a == NULL);
                    } else {
                        streams_equal = (ms_a == ms_b);
                    }

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(streams_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!streams_equal);
                    } else {
                        runtimeError(vm,
                                     "Runtime Error: Invalid operator for memory stream comparison. Only '=' and '<>' are allowed. Got opcode %d.",
                                     instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Interface and NIL comparison
                else if ((VALUE_TYPE(a_val) == TYPE_INTERFACE || VALUE_TYPE(a_val) == TYPE_NIL) &&
                         (VALUE_TYPE(b_val) == TYPE_INTERFACE || VALUE_TYPE(b_val) == TYPE_NIL)) {
                    ClosureEnvPayload* payload_a = (VALUE_TYPE(a_val) == TYPE_INTERFACE) ? AS_INTERFACE(a_val).payload : NULL;
                    ClosureEnvPayload* payload_b = (VALUE_TYPE(b_val) == TYPE_INTERFACE) ? AS_INTERFACE(b_val).payload : NULL;
                    bool interfaces_equal = (payload_a == payload_b);

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(interfaces_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!interfaces_equal);
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for interface comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Closure and NIL comparison
                else if ((VALUE_TYPE(a_val) == TYPE_CLOSURE || VALUE_TYPE(a_val) == TYPE_NIL) &&
                         (VALUE_TYPE(b_val) == TYPE_CLOSURE || VALUE_TYPE(b_val) == TYPE_NIL)) {
                    bool closures_equal = false;
                    if (VALUE_TYPE(a_val) == TYPE_NIL && VALUE_TYPE(b_val) == TYPE_NIL) {
                        closures_equal = true;
                    } else if (VALUE_TYPE(a_val) == TYPE_CLOSURE && VALUE_TYPE(b_val) == TYPE_CLOSURE) {
                        closures_equal = (AS_CLOSURE(a_val).entry_offset == AS_CLOSURE(b_val).entry_offset) &&
                                         (AS_CLOSURE(a_val).symbol == AS_CLOSURE(b_val).symbol) &&
                                         (AS_CLOSURE(a_val).env == AS_CLOSURE(b_val).env);
                    } else {
                        closures_equal = false;
                    }

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(closures_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!closures_equal);
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for closure comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Pointer and NIL comparison
                else if ((VALUE_TYPE(a_val) == TYPE_POINTER || VALUE_TYPE(a_val) == TYPE_NIL) &&
                         (VALUE_TYPE(b_val) == TYPE_POINTER || VALUE_TYPE(b_val) == TYPE_NIL)) {
                    bool ptrs_equal = false;
                    if (VALUE_TYPE(a_val) == TYPE_NIL && VALUE_TYPE(b_val) == TYPE_NIL) {
                        ptrs_equal = true;
                    } else if (VALUE_TYPE(a_val) == TYPE_NIL && VALUE_TYPE(b_val) == TYPE_POINTER) {
                        ptrs_equal = (AS_POINTER(b_val) == NULL);
                    } else if (VALUE_TYPE(a_val) == TYPE_POINTER && VALUE_TYPE(b_val) == TYPE_NIL) {
                        ptrs_equal = (AS_POINTER(a_val) == NULL);
                    } else if (frontendIsAether()) {
                        /* D24: records compare field by field under Aether. */
                        int eq = pscalRecordPointersDeepEqual(&a_val, &b_val);
                        if (eq < 0) goto comparison_error_label;
                        ptrs_equal = (eq == 1);
                    } else {
                        ptrs_equal = (AS_POINTER(a_val) == AS_POINTER(b_val));
                    }

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(ptrs_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!ptrs_equal);
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for pointer comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Arrays: deep structural equality, '=' and '<>' only (see
                // pscalArraysDeepEqual above). Ordering has no meaning for arrays
                // and stays an error.
                else if (VALUE_TYPE(a_val) == TYPE_ARRAY && VALUE_TYPE(b_val) == TYPE_ARRAY) {
                    if (instruction_val == EQUAL || instruction_val == NOT_EQUAL) {
                        int eq = pscalArraysDeepEqual(&a_val, &b_val);
                        if (eq < 0) goto comparison_error_label;
                        result_val = makeBoolean(instruction_val == EQUAL ? (eq == 1) : (eq == 0));
                        comparison_succeeded = true;
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for array comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                }
                // Aether maps (D68): structural equality, the same key set and
                // equal values, order ignored. '=' and '<>' only.
                else if (VALUE_TYPE(a_val) == TYPE_MAP && VALUE_TYPE(b_val) == TYPE_MAP) {
                    if (instruction_val == EQUAL || instruction_val == NOT_EQUAL) {
                        int eq = pscalMapsDeepEqual(&a_val, &b_val);
                        if (eq < 0) goto comparison_error_label;
                        result_val = makeBoolean(instruction_val == EQUAL ? (eq == 1) : (eq == 0));
                        comparison_succeeded = true;
                    } else {
                        runtimeError(vm, "Runtime Error: maps can only be compared with == and !=.");
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                }
                // Pascal sets: '=' and '<>' compare membership (the order
                // the members were added in does not matter), '<=' is
                // "is a subset of" and '>=' "is a superset of". '<' and
                // '>' are not set operators and stay an error.
                else if (VALUE_TYPE(a_val) == TYPE_SET && VALUE_TYPE(b_val) == TYPE_SET) {
                    int sub = 1;
                    int super = 1;
                    switch (instruction_val) {
                        case EQUAL:
                        case NOT_EQUAL:
                            sub = vmSetIsSubset(&a_val, &b_val);
                            if (sub == 1) {
                                super = vmSetIsSubset(&b_val, &a_val);
                            }
                            break;
                        case LESS_EQUAL:
                            sub = vmSetIsSubset(&a_val, &b_val);
                            break;
                        case GREATER_EQUAL:
                            super = vmSetIsSubset(&b_val, &a_val);
                            break;
                        default:
                            goto comparison_error_label;
                    }
                    if (sub < 0 || super < 0) {
                        runtimeError(vm, "VM Error: Out of memory comparing sets.");
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    bool holds = (sub == 1 && super == 1);
                    result_val = makeBoolean(instruction_val == NOT_EQUAL ? !holds : holds);
                    comparison_succeeded = true;
                }
                // Any other concrete value compared against NIL: builtins
                // like ChannelReceive/TaskAwait return TYPE_NIL to signal a
                // degenerate/closed/canceled result even when the receiving
                // variable is declared as an ordinary type (e.g. `integer`),
                // and callers are documented to check that result against
                // nil (see vmBuiltinChannelReceive's comment) -- so a
                // concrete non-nil value of ANY type must compare as simply
                // "not nil", not as a type error. Only '=' and '<>' make
                // sense here; ordering against nil is still rejected. (Both
                // sides NIL is already handled above, so reaching here with
                // either side NIL means the other side is some standalone
                // type -- ordinal, real, etc. -- not already covered by the
                // string/char/memorystream/interface/closure/pointer blocks.)
                else if (VALUE_TYPE(a_val) == TYPE_NIL || VALUE_TYPE(b_val) == TYPE_NIL) {
                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(false);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(true);
                    } else {
                        goto comparison_error_label;
                    }
                    comparison_succeeded = true;
                }

comparison_error_label:
                if (comparison_succeeded) {
                    push(vm, result_val);
                } else {
                    const char* op_str = "unknown_comparison_op";
                    switch (instruction_val) {
                        case EQUAL:         op_str = "="; break;
                        case NOT_EQUAL:     op_str = "<>"; break;
                        case GREATER:       op_str = ">";  break;
                        case GREATER_EQUAL: op_str = ">="; break;
                        case LESS:          op_str = "<";  break;
                        case LESS_EQUAL:    op_str = "<="; break;
                        default: op_str = "unknown_comparison_op_code"; break;
                    }

                    runtimeError(vm, "Runtime Error: Operands not comparable for operator '%s'. Left operand: %s, Right operand: %s.",
                                                 op_str,
                                                 varTypeToString(VALUE_TYPE(a_val)),
                                                 varTypeToString(VALUE_TYPE(b_val)));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case ALLOC_OBJECT: {
                uint8_t field_count = READ_BYTE();
                FieldValue* fields_head = NULL;
                FieldValue** next_ptr = &fields_head;
                for (uint16_t i = 0; i < field_count; i++) {
                    FieldValue* field = calloc(1, sizeof(FieldValue));
                    if (!field) {
                        freeFieldValue(fields_head);
                        runtimeError(vm, "VM Error: Out of memory allocating object field.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    field->name = NULL;
                    field->value = makeNil();
                    field->storage = &field->value;
                    field->slot_index = (int)i;
                    field->owns_storage = true;
                    field->declared_type = TYPE_VOID;
                    field->type_def = NULL;
                    field->next = NULL;
                    *next_ptr = field;
                    next_ptr = &field->next;
                }
                Value* obj = malloc(sizeof(Value));
                if (!obj) {
                    freeFieldValue(fields_head);
                    runtimeError(vm, "VM Error: Out of memory allocating object value.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                *obj = makeRecord(fields_head);
                push(vm, makePointer(obj, NULL));
                break;
            }
            case ALLOC_OBJECT16: {
                uint16_t field_count = READ_SHORT(vm);
                FieldValue* fields_head = NULL;
                FieldValue** next_ptr = &fields_head;
                for (uint16_t i = 0; i < field_count; i++) {
                    FieldValue* field = calloc(1, sizeof(FieldValue));
                    if (!field) {
                        freeFieldValue(fields_head);
                        runtimeError(vm, "VM Error: Out of memory allocating object field.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    field->name = NULL;
                    field->value = makeNil();
                    field->storage = &field->value;
                    field->slot_index = (int)i;
                    field->owns_storage = true;
                    field->declared_type = TYPE_VOID;
                    field->type_def = NULL;
                    field->next = NULL;
                    *next_ptr = field;
                    next_ptr = &field->next;
                }
                Value* obj = malloc(sizeof(Value));
                if (!obj) {
                    freeFieldValue(fields_head);
                    runtimeError(vm, "VM Error: Out of memory allocating object value.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                *obj = makeRecord(fields_head);
                push(vm, makePointer(obj, NULL));
                break;
            }
            case GET_FIELD_OFFSET: {
                uint8_t field_index = READ_BYTE();
                if (!vmStackHasOperands(vm, GET_FIELD_OFFSET, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // The field address handed out here is the l-value of a
                // store (SET_INDIRECT) or a VAR argument, exactly like
                // GET_FIELD_ADDRESS's -- clone a still-shared RecordObj
                // first, or the write lands in every copy that shares it
                // (a CLike struct passed by value was mutated in the
                // caller). Only when the record was reached through a
                // pointer: a record held directly by this stack slot is a
                // temporary popped just below, and cloning it would leave
                // the pushed address pointing into a freed clone.
                if (record_struct_ptr != base_val_ptr) {
                    valueEnsureUnique(record_struct_ptr);
                }

                FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                hydrateFieldMetadataFromBaseValue(base_val_ptr, current, field_index);
                // A field of a dynamic array's element: the new address keeps
                // that array alive as the element pointer did (retained_array).
                ArrayObj* owner = vmRetainPointerOwner(base_val_ptr);
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def, owner));
                break;
            }
            case GET_FIELD_OFFSET16: {
                uint16_t field_index = READ_SHORT(vm);
                if (!vmStackHasOperands(vm, GET_FIELD_OFFSET16, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Same clone-on-write as GET_FIELD_OFFSET above.
                if (record_struct_ptr != base_val_ptr) {
                    valueEnsureUnique(record_struct_ptr);
                }

                FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");

                    return INTERPRET_RUNTIME_ERROR;
                }
                hydrateFieldMetadataFromBaseValue(base_val_ptr, current, field_index);
                // A field of a dynamic array's element: the new address keeps
                // that array alive as the element pointer did (retained_array).
                ArrayObj* owner = vmRetainPointerOwner(base_val_ptr);
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def, owner));
                break;
            }
            case LOAD_FIELD_VALUE: {
                uint8_t field_index = READ_BYTE();
                Value base_val = pop(vm);
                bool ok = pushFieldValueByOffset(vm, &base_val, field_index);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case LOAD_FIELD_VALUE16: {
                uint16_t field_index = READ_SHORT(vm);
                Value base_val = pop(vm);
                bool ok = pushFieldValueByOffset(vm, &base_val, field_index);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case GET_FIELD_ADDRESS: {
                uint8_t field_name_idx = READ_BYTE();
                if (!vmStackHasOperands(vm, GET_FIELD_ADDRESS, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): this address is about to be
                // handed out for a potential in-place mutation (SET_INDIRECT,
                // further field/element chaining, or VAR-param aliasing) --
                // clone the RecordObj here if it's still shared, before
                // resolving the field's storage address.
                valueEnsureUnique(record_struct_ptr);

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    // A field of a dynamic array's element: the new address keeps
                    // that array alive as the element pointer did (retained_array).
                    ArrayObj* owner = vmRetainPointerOwner(base_val_ptr);
                    Value popped_base_val = pop(vm);
                    freeValue(&popped_base_val);
                    push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def, owner));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                if (!vmStackHasOperands(vm, GET_FIELD_ADDRESS16, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): this address is about to be
                // handed out for a potential in-place mutation (SET_INDIRECT,
                // further field/element chaining, or VAR-param aliasing) --
                // clone the RecordObj here if it's still shared, before
                // resolving the field's storage address.
                valueEnsureUnique(record_struct_ptr);

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    // A field of a dynamic array's element: the new address keeps
                    // that array alive as the element pointer did (retained_array).
                    ArrayObj* owner = vmRetainPointerOwner(base_val_ptr);
                    Value popped_base_val = pop(vm);
                    freeValue(&popped_base_val);
                    push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def, owner));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS_KEEP: {
                uint8_t field_name_idx = READ_BYTE();
                if (!vmStackHasOperands(vm, GET_FIELD_ADDRESS_KEEP, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): this address is about to be
                // handed out for a potential in-place mutation (SET_INDIRECT,
                // further field/element chaining, or VAR-param aliasing) --
                // clone the RecordObj here if it's still shared, before
                // resolving the field's storage address.
                valueEnsureUnique(record_struct_ptr);

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def,
                                                              vmRetainPointerOwner(base_val_ptr)));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS_KEEP16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                if (!vmStackHasOperands(vm, GET_FIELD_ADDRESS_KEEP16, 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    vmReportFieldBaseNotRecord(vm, record_struct_ptr);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): this address is about to be
                // handed out for a potential in-place mutation (SET_INDIRECT,
                // further field/element chaining, or VAR-param aliasing) --
                // clone the RecordObj here if it's still shared, before
                // resolving the field's storage address.
                valueEnsureUnique(record_struct_ptr);

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    push(vm, makeRetainedArrayElementPointer(fieldValueStorage(current), current->type_def,
                                                              vmRetainPointerOwner(base_val_ptr)));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case LOAD_FIELD_VALUE_BY_NAME: {
                uint8_t field_name_idx = READ_BYTE();
                Value base_val = pop(vm);
                const Value* name_val = (field_name_idx < vm->chunk->constants_count)
                                        ? &vm->chunk->constants[field_name_idx]
                                        : NULL;
                const char* field_name = (name_val && VALUE_TYPE(*name_val) == TYPE_STRING)
                                          ? AS_STRING(*name_val)
                                          : NULL;
                bool ok = pushFieldValueByName(vm, &base_val, field_name);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case LOAD_FIELD_VALUE_BY_NAME16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                Value base_val = pop(vm);
                const Value* name_val = (field_name_idx < vm->chunk->constants_count)
                                        ? &vm->chunk->constants[field_name_idx]
                                        : NULL;
                const char* field_name = (name_val && VALUE_TYPE(*name_val) == TYPE_STRING)
                                          ? AS_STRING(*name_val)
                                          : NULL;
                bool ok = pushFieldValueByName(vm, &base_val, field_name);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case GET_ELEMENT_ADDRESS: {
                uint8_t dimension_count = READ_BYTE();

                // Pop the base operand first so type checking does not
                // depend on stack offsets.
                Value operand = pop(vm);

                // A character/byte/opaque address is not a Value: reading
                // it as one (below) would run off its buffer (`s[2][1]`).
                if (VALUE_TYPE(operand) == TYPE_POINTER && vmPointerTargetIsNotValue(&operand)) {
                    runtimeError(vm, "VM Error: Cannot index through a character, byte or opaque address.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Special handling for strings when there is exactly one
                // index.  We avoid referencing stackTop-2 by working with the
                // popped operand directly and reusing it if it represents a
                // string.
                if (dimension_count == 1 && VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* base_val = (Value*)AS_POINTER(operand);
                    if (base_val && isPascalStringType(VALUE_TYPE(*base_val))) {
                        Value index_val = pop(vm);
                        if (!isIntlikeType(VALUE_TYPE(index_val))) {
                            runtimeError(vm, "VM Error: String index must be an integer.");
                            freeValue(&index_val);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        long long pscal_index = VAL_INT(index_val);
                        freeValue(&index_val);

                        // VM 2.0 Phase 4j (Stage B): this address is about to
                        // be handed out for a potential in-place mutation
                        // (SET_INDIRECT, or aliased into a VAR parameter) --
                        // clone the StringObj here if it's still shared,
                        // BEFORE computing an address into it, so a write
                        // through this pointer can never leak into another
                        // Value that shares the same buffer via 4j's cheap
                        // copyValueForStack retain-share.
                        valueEnsureUnique(base_val);

                        // A pinned char pointer (makeStringCharPointer),
                        // indexed by code point for a UnicodeString.
                        bool pushed = vmPushStringCharAddress(vm, base_val, pscal_index, true);
                        freeValue(&operand);
                        if (!pushed) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        break;
                    }
                }

                // Up to 8 dimensions index from the stack: this ran a malloc/free
                // pair on every element access.
                int indices_inline[8];
                int* indices = dimension_count <= 8 ? indices_inline
                                                    : malloc(sizeof(int) * dimension_count);
                if (!indices) { runtimeError(vm, "VM Error: Malloc failed for array indices."); freeValue(&operand); return INTERPRET_RUNTIME_ERROR; }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (!vmIndexValueToInt(vm, &index_val, &indices[dimension_count - 1 - i])) {
                        if (indices != indices_inline) free(indices);
                        freeValue(&index_val);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value shared_snapshot = makeNil();
                // Initialised lazily, only on the pointer-to-array-type path
                // that actually uses it: an eager init callocs an ArrayObj on
                // every element access and only that path ever frees it.
                Value temp_wrapper;
                memset(&temp_wrapper, 0, sizeof(temp_wrapper));
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (!candidate || vmValueIsDisposed(candidate)) {
                        runtimeError(vm, candidate ? "VM Error: Array access through a dangling pointer (its target was disposed)."
                                                   : "VM Error: Array access through a nil pointer.");
                        if (indices != indices_inline) free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        // VM 2.0 follow-up to dynamic_array_fresh_publish_race
                        // (pscal-core f65432e): `candidate` here can alias a
                        // shared global's live Value cell. This opcode always
                        // hands out an address that MAY be used to mutate the
                        // array in place or alias it as a VAR parameter, so
                        // (unlike LOAD_ELEMENT_VALUE's read-only snapshot,
                        // which is freed before that opcode returns) the
                        // retained reference this snapshot takes on a
                        // dynamic array must OUTLIVE this opcode -- it gets
                        // transferred into the returned pointer's
                        // PointerObj.retained_array below (core/types.h)
                        // instead of being freed here. Checking
                        // ARRAY_IS_DYNAMIC(*candidate) before any lock would
                        // itself race a concurrent SetLength publish the
                        // same way LOAD_ELEMENT_VALUE's did, so call
                        // copyDynamicArraySnapshotValue() unconditionally
                        // first and branch on the result.
                        shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                        if (ARRAY_IS_DYNAMIC(shared_snapshot)) {
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        vmInitArrayIndexWrapper(&temp_wrapper);
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        {
                            // SET_VALUE_TYPE resets .bits to a nil-pointer
                            // placeholder (VM 2.0 Phase 4i checkpoint 3b), but
                            // temp_wrapper already has a real ArrayObj* from
                            // vmInitArrayIndexWrapper's pscalArrayEnsureObj
                            // call just above -- capture it first and re-tag with
                            // the wrapper that's actually there, same
                            // "re-tag after retype" fix checkpoint 2 needed
                            // for TYPE_THREAD.
                            ArrayObj *reset_a = PSCAL_VALUE_PTR(temp_wrapper, ArrayObj);
                            SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
                            pscalValueSetHeapPtrBits(&temp_wrapper, reset_a);
                        }
                        ARRAY_DIMENSIONS(temp_wrapper) = dims;
                        ARRAY_ELEMENT_TYPE(temp_wrapper) = vmResolveArrayElementType(arrayType);
                        ARRAY_IS_PACKED(temp_wrapper) = isPackedByteElementType(ARRAY_ELEMENT_TYPE(temp_wrapper));
                        if (ARRAY_IS_PACKED(temp_wrapper)) {
                            AS_ARRAY_RAW(temp_wrapper) = (uint8_t*)AS_POINTER(operand);
                            AS_ARRAY(temp_wrapper) = NULL;
                        } else {
                            AS_ARRAY(temp_wrapper) = (Value*)AS_POINTER(operand);
                            AS_ARRAY_RAW(temp_wrapper) = NULL;
                        }
                        ARRAY_LOWER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        ARRAY_UPPER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        if (!ARRAY_LOWER_BOUNDS(temp_wrapper) || !ARRAY_UPPER_BOUNDS(temp_wrapper)) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                            if (indices != indices_inline) free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                vmFreeArrayIndexWrapper(&temp_wrapper);
                                if (indices != indices_inline) free(indices);
                                freeValue(&operand);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            ARRAY_LOWER_BOUNDS(temp_wrapper)[i] = lb;
                            ARRAY_UPPER_BOUNDS(temp_wrapper)[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        if (indices != indices_inline) free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "[AETH-RUNTIME-INDEX-NOT-ARRAY] cannot index this value: it is not an array. hint: indexing a 1-D array two-dimensionally (dp[i][j]) does this -- a `let dp: Int[]` is flat, so use a single index dp[i * width + j], or build an array of arrays.");
                    if (indices != indices_inline) free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): this opcode always returns an
                // address that MAY be used to mutate the array in place
                // (SET_INDIRECT, VAR-param aliasing, or Inc/Dec-style
                // read-modify-write) -- clone here if still shared, before
                // computing any offset into it. A no-op when array_val_ptr
                // is the temp_wrapper/operand-literal case (always
                // freshly-built, refcount 1) or already uniquely owned;
                // dynamic arrays are permanently exempt (valueEnsureUnique
                // itself branches on ArrayObj.is_dynamic).
                valueEnsureUnique(array_val_ptr);

                // See the matching comment in LOAD_ELEMENT_VALUE: a dynamic
                // array's true dimensionality can exceed dimension_count
                // here (e.g. `g[i][j]` against a value from
                // SetLength(g, 6, 6), dimensions==2). When that happens,
                // produce a sub-array VIEW instead of a final address so the
                // next opcode in the chain can finish the indexing. Since
                // this opcode must return a stable pointer (the caller may
                // write through it via SET_INDIRECT, or index further into
                // it), the view is heap-allocated and tagged
                // OWNED_POINTER_SENTINEL so freeValue() on the pointer Value
                // (already called unconditionally on the TYPE_POINTER path
                // below and by any consumer that later frees its own popped
                // operand) releases both the wrapper allocation and this
                // view's share of the underlying array's refcount.
                if (dimension_count < (uint8_t)ARRAY_DIMENSIONS(*array_val_ptr)) {
                    Value* slice = malloc(sizeof(Value));
                    bool slice_oob = false;
                    bool slice_ok = slice && makeDynamicArraySliceValue(array_val_ptr, dimension_count, indices, slice, &slice_oob);
                    if (indices != indices_inline) free(indices);
                    if (!slice_ok) {
                        free(slice);
                        runtimeError(vm, "VM Error: Array element index out of bounds.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makePointer(slice, OWNED_POINTER_SENTINEL));
                    if (VALUE_TYPE(operand) == TYPE_POINTER) {
                        freeValue(&operand);
                    }
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    // makeDynamicArraySliceValue() above already took its
                    // own retained reference (view_of) on the underlying
                    // array -- this snapshot's own retain is no longer
                    // needed once the slice exists independently.
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    break;
                }

                /* Bounds-check before computeFlatOffset(), which would otherwise
                 * print a location-less line and exit. See
                 * vmCheckArrayIndexBounds(). */
                if (!vmCheckArrayIndexBounds(vm, array_val_ptr, indices)) {
                    if (indices != indices_inline) free(indices);
                    freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                if (indices != indices_inline) free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                AST* element_base_type = NULL;
                if (ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr)) {
                    element_base_type = ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr);
                } else if (using_wrapper && PTR_BASE_TYPE_NODE(operand) &&
                           PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                    // Only trust PTR_BASE_TYPE_NODE(operand) as a real AST*
                    // here when using_wrapper is true -- that's the same
                    // condition under which this opcode already validated
                    // it as a genuine AST_ARRAY_TYPE node (see the
                    // temp_wrapper construction above). Without this guard,
                    // a slice/view Value's element_type_def can be NULL
                    // (inherited from an array whose own element type has
                    // no AST def, e.g. a plain scalar) while `operand` is a
                    // pointer whose base_type_node is one of the *_SENTINEL
                    // fake-AST markers (OWNED_POINTER_SENTINEL etc, see
                    // string_sentinels.h) -- dereferencing those crashes.
                    element_base_type = PTR_BASE_TYPE_NODE(operand)->right;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    // VM 2.0 follow-up to dynamic_array_fresh_publish_race
                    // (pscal-core f65432e): when using_snapshot, this
                    // address points into shared_snapshot's own retained
                    // ArrayObj -- hand that retained reference off to the
                    // returned pointer (PointerObj.retained_array,
                    // core/types.h) instead of freeing shared_snapshot
                    // below, so the buffer stays alive for exactly as long
                    // as the pointer does, even across a concurrent
                    // SetLength() on the live global this snapshot was
                    // taken from.
                    if (using_snapshot) {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY_RAW(*array_val_ptr)[offset],
                                                                  BYTE_ARRAY_PTR_SENTINEL,
                                                                  PSCAL_VALUE_PTR(shared_snapshot, ArrayObj)));
                    } else {
                        // A static array inside a dynamic array's element
                        // lives in that array's storage: keep it alive.
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY_RAW(*array_val_ptr)[offset],
                                                                  BYTE_ARRAY_PTR_SENTINEL,
                                                                  vmRetainPointerOwner(&operand)));
                    }
                } else {
                    if (using_snapshot) {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY(*array_val_ptr)[offset],
                                                                  element_base_type,
                                                                  PSCAL_VALUE_PTR(shared_snapshot, ArrayObj)));
                    } else {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY(*array_val_ptr)[offset],
                                                                  element_base_type,
                                                                  vmRetainPointerOwner(&operand)));
                    }
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    vmFreeArrayIndexWrapper(&temp_wrapper);
                }
                // shared_snapshot's retain was transferred into the pointer
                // above (using_snapshot case) -- do not also free it here.

                break;
            }
            case GET_ELEMENT_ADDRESS_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);
                Value operand = pop(vm);

                // See GET_ELEMENT_ADDRESS: such an address is not a Value.
                if (VALUE_TYPE(operand) == TYPE_POINTER && vmPointerTargetIsNotValue(&operand)) {
                    runtimeError(vm, "VM Error: Cannot index through a character, byte or opaque address.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* base_val = (Value*)AS_POINTER(operand);
                    if (base_val && isPascalStringType(VALUE_TYPE(*base_val))) {
                        const char* str = AS_STRING(*base_val) ? AS_STRING(*base_val) : "";
                        size_t byte_len = strlen(str);
                        bool unicode = VALUE_TYPE(*base_val) == TYPE_UNICODE_STRING;
                        size_t len = unicode ? utf8CodepointCount(str, byte_len) : byte_len;

                        if ((size_t)flat_offset >= len) {
                            unsigned long long display_index = vmDisplayIndexFromOffset(flat_offset);
                            runtimeError(vm,
                                         "Runtime Error: String index (%llu) out of bounds for string of length %zu.",
                                         display_index, len);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        // VM 2.0 Phase 4j (Stage B): see the matching comment
                        // in GET_ELEMENT_ADDRESS.
                        valueEnsureUnique(base_val);

                        // A pinned char pointer, as in GET_ELEMENT_ADDRESS.
                        size_t byte_offset = (unicode && len != byte_len)
                                                 ? utf8ByteOffsetForCodepointIndex(AS_STRING(*base_val), byte_len, flat_offset)
                                                 : (size_t)flat_offset;
                        push(vm, makeStringCharPointer(base_val, flat_offset, unicode, &AS_STRING(*base_val)[byte_offset]));
                        freeValue(&operand);
                        break;
                    }
                }

                Value* array_val_ptr = NULL;
                Value shared_snapshot = makeNil();
                // Initialised lazily, only on the pointer-to-array-type path
                // that actually uses it: an eager init callocs an ArrayObj on
                // every element access and only that path ever frees it.
                Value temp_wrapper;
                memset(&temp_wrapper, 0, sizeof(temp_wrapper));
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (!candidate || vmValueIsDisposed(candidate)) {
                        runtimeError(vm, candidate ? "VM Error: Array access through a dangling pointer (its target was disposed)."
                                                   : "VM Error: Array access through a nil pointer.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        // VM 2.0 follow-up to dynamic_array_fresh_publish_race
                        // (pscal-core f65432e): see the matching comment in
                        // GET_ELEMENT_ADDRESS -- same hazard, same fix.
                        shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                        if (ARRAY_IS_DYNAMIC(shared_snapshot)) {
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        vmInitArrayIndexWrapper(&temp_wrapper);
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        {
                            // SET_VALUE_TYPE resets .bits to a nil-pointer
                            // placeholder (VM 2.0 Phase 4i checkpoint 3b), but
                            // temp_wrapper already has a real ArrayObj* from
                            // vmInitArrayIndexWrapper's pscalArrayEnsureObj
                            // call just above -- capture it first and re-tag with
                            // the wrapper that's actually there, same
                            // "re-tag after retype" fix checkpoint 2 needed
                            // for TYPE_THREAD.
                            ArrayObj *reset_a = PSCAL_VALUE_PTR(temp_wrapper, ArrayObj);
                            SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
                            pscalValueSetHeapPtrBits(&temp_wrapper, reset_a);
                        }
                        ARRAY_DIMENSIONS(temp_wrapper) = dims;
                        ARRAY_ELEMENT_TYPE(temp_wrapper) = vmResolveArrayElementType(arrayType);
                        ARRAY_IS_PACKED(temp_wrapper) = isPackedByteElementType(ARRAY_ELEMENT_TYPE(temp_wrapper));
                        if (ARRAY_IS_PACKED(temp_wrapper)) {
                            AS_ARRAY_RAW(temp_wrapper) = (uint8_t*)AS_POINTER(operand);
                            AS_ARRAY(temp_wrapper) = NULL;
                        } else {
                            AS_ARRAY(temp_wrapper) = (Value*)AS_POINTER(operand);
                            AS_ARRAY_RAW(temp_wrapper) = NULL;
                        }
                        ARRAY_LOWER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        ARRAY_UPPER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        if (!ARRAY_LOWER_BOUNDS(temp_wrapper) || !ARRAY_UPPER_BOUNDS(temp_wrapper)) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                vmFreeArrayIndexWrapper(&temp_wrapper);
                                freeValue(&operand);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            ARRAY_LOWER_BOUNDS(temp_wrapper)[i] = lb;
                            ARRAY_UPPER_BOUNDS(temp_wrapper)[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "[AETH-RUNTIME-INDEX-NOT-ARRAY] cannot index this value: it is not an array. hint: indexing a 1-D array two-dimensionally (dp[i][j]) does this -- a `let dp: Int[]` is flat, so use a single index dp[i * width + j], or build an array of arrays.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 4j (Stage B): see the matching comment in
                // GET_ELEMENT_ADDRESS.
                valueEnsureUnique(array_val_ptr);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (VALUE_TYPE(operand) == TYPE_POINTER) {
                        freeValue(&operand);
                    }
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                AST* element_base_type = NULL;
                if (ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr)) {
                    element_base_type = ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr);
                } else if (using_wrapper && PTR_BASE_TYPE_NODE(operand) &&
                           PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                    // Only trust PTR_BASE_TYPE_NODE(operand) as a real AST*
                    // here when using_wrapper is true -- that's the same
                    // condition under which this opcode already validated
                    // it as a genuine AST_ARRAY_TYPE node (see the
                    // temp_wrapper construction above). Without this guard,
                    // a slice/view Value's element_type_def can be NULL
                    // (inherited from an array whose own element type has
                    // no AST def, e.g. a plain scalar) while `operand` is a
                    // pointer whose base_type_node is one of the *_SENTINEL
                    // fake-AST markers (OWNED_POINTER_SENTINEL etc, see
                    // string_sentinels.h) -- dereferencing those crashes.
                    element_base_type = PTR_BASE_TYPE_NODE(operand)->right;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (using_snapshot) {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY_RAW(*array_val_ptr)[flat_offset],
                                                                  BYTE_ARRAY_PTR_SENTINEL,
                                                                  PSCAL_VALUE_PTR(shared_snapshot, ArrayObj)));
                    } else {
                        // See GET_ELEMENT_ADDRESS: keep an enclosing dynamic
                        // array alive.
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY_RAW(*array_val_ptr)[flat_offset],
                                                                  BYTE_ARRAY_PTR_SENTINEL,
                                                                  vmRetainPointerOwner(&operand)));
                    }
                } else {
                    if (using_snapshot) {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY(*array_val_ptr)[flat_offset],
                                                                  element_base_type,
                                                                  PSCAL_VALUE_PTR(shared_snapshot, ArrayObj)));
                    } else {
                        push(vm, makeRetainedArrayElementPointer(&AS_ARRAY(*array_val_ptr)[flat_offset],
                                                                  element_base_type,
                                                                  vmRetainPointerOwner(&operand)));
                    }
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    vmFreeArrayIndexWrapper(&temp_wrapper);
                }
                // shared_snapshot's retain was transferred into the pointer
                // above (using_snapshot case) -- do not also free it here.

                break;
            }
            case LOAD_ELEMENT_VALUE: {
                uint8_t dimension_count = READ_BYTE();

                Value operand = pop(vm);

                // See GET_ELEMENT_ADDRESS: such an address is not a Value.
                if (VALUE_TYPE(operand) == TYPE_POINTER && vmPointerTargetIsNotValue(&operand)) {
                    runtimeError(vm, "VM Error: Cannot index through a character, byte or opaque address.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (dimension_count == 1 && VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* base_val = (Value*)AS_POINTER(operand);
                    if (base_val && isPascalStringType(VALUE_TYPE(*base_val))) {
                        Value index_val = pop(vm);
                        if (!isIntlikeType(VALUE_TYPE(index_val))) {
                            runtimeError(vm, "VM Error: String index must be an integer.");
                            freeValue(&index_val);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        long long pscal_index = VAL_INT(index_val);
                        freeValue(&index_val);

                        size_t len = AS_STRING(*base_val) ? strlen(AS_STRING(*base_val)) : 0;
                        size_t char_offset = 0;
                        bool wants_length = false;
                        if (!vmResolveStringIndex(vm,
                                                  pscal_index,
                                                  len,
                                                  &char_offset,
                                                  true,
                                                  &wants_length)) {
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        if (!frontendIsZeroBasedStrings() && wants_length) {
                            push(vm, makeInt((long long)len));
                            freeValue(&operand);
                            break;
                        }

                        char ch = AS_STRING(*base_val) ? AS_STRING(*base_val)[char_offset] : '\0';
                        push(vm, makeChar(ch));
                        freeValue(&operand);
                        break;
                    }
                }

                // Up to 8 dimensions index from the stack: this ran a malloc/free
                // pair on every element access.
                int indices_inline[8];
                int* indices = dimension_count <= 8 ? indices_inline
                                                    : malloc(sizeof(int) * dimension_count);
                if (!indices) {
                    runtimeError(vm, "VM Error: Malloc failed for array indices.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (!vmIndexValueToInt(vm, &index_val, &indices[dimension_count - 1 - i])) {
                        if (indices != indices_inline) free(indices);
                        freeValue(&index_val);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value shared_snapshot = makeNil();
                // Initialised lazily, only on the pointer-to-array-type path
                // that actually uses it: an eager init callocs an ArrayObj on
                // every element access and only that path ever frees it.
                Value temp_wrapper;
                memset(&temp_wrapper, 0, sizeof(temp_wrapper));
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (!candidate || vmValueIsDisposed(candidate)) {
                        runtimeError(vm, candidate ? "VM Error: Array access through a dangling pointer (its target was disposed)."
                                                   : "VM Error: Array access through a nil pointer.");
                        if (indices != indices_inline) free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        // Checking ARRAY_IS_DYNAMIC(*candidate) here first,
                        // before any lock, would itself race a concurrent
                        // SetLength publish (dynamic_array_fresh_publish_race
                        // follow-up, pscal-core builtin.c's
                        // resizeDynamicArrayValue) -- it dereferences
                        // candidate's live bits pointer the same way the
                        // element read below does. copyDynamicArraySnapshotValue()
                        // takes the whole-struct copy, the is_dynamic check,
                        // and the conditional retain all under one critical
                        // section, so call it unconditionally and only
                        // branch on dynamic-vs-static from the now-safe
                        // local copy.
                        shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                        if (ARRAY_IS_DYNAMIC(shared_snapshot)) {
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        vmInitArrayIndexWrapper(&temp_wrapper);
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        {
                            // SET_VALUE_TYPE resets .bits to a nil-pointer
                            // placeholder (VM 2.0 Phase 4i checkpoint 3b), but
                            // temp_wrapper already has a real ArrayObj* from
                            // vmInitArrayIndexWrapper's pscalArrayEnsureObj
                            // call just above -- capture it first and re-tag with
                            // the wrapper that's actually there, same
                            // "re-tag after retype" fix checkpoint 2 needed
                            // for TYPE_THREAD.
                            ArrayObj *reset_a = PSCAL_VALUE_PTR(temp_wrapper, ArrayObj);
                            SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
                            pscalValueSetHeapPtrBits(&temp_wrapper, reset_a);
                        }
                        ARRAY_DIMENSIONS(temp_wrapper) = dims;
                        ARRAY_ELEMENT_TYPE(temp_wrapper) = vmResolveArrayElementType(arrayType);
                        ARRAY_IS_PACKED(temp_wrapper) = isPackedByteElementType(ARRAY_ELEMENT_TYPE(temp_wrapper));
                        if (ARRAY_IS_PACKED(temp_wrapper)) {
                            AS_ARRAY_RAW(temp_wrapper) = (uint8_t*)AS_POINTER(operand);
                            AS_ARRAY(temp_wrapper) = NULL;
                        } else {
                            AS_ARRAY(temp_wrapper) = (Value*)AS_POINTER(operand);
                            AS_ARRAY_RAW(temp_wrapper) = NULL;
                        }
                        ARRAY_LOWER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        ARRAY_UPPER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        if (!ARRAY_LOWER_BOUNDS(temp_wrapper) || !ARRAY_UPPER_BOUNDS(temp_wrapper)) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                            if (indices != indices_inline) free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                vmFreeArrayIndexWrapper(&temp_wrapper);
                                if (indices != indices_inline) free(indices);
                                freeValue(&operand);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            ARRAY_LOWER_BOUNDS(temp_wrapper)[i] = lb;
                            ARRAY_UPPER_BOUNDS(temp_wrapper)[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        if (indices != indices_inline) free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "[AETH-RUNTIME-INDEX-NOT-ARRAY] cannot index this value: it is not an array. hint: indexing a 1-D array two-dimensionally (dp[i][j]) does this -- a `let dp: Int[]` is flat, so use a single index dp[i * width + j], or build an array of arrays.");
                    if (indices != indices_inline) free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // A dynamic array's true dimensionality (e.g. 2 for
                // SetLength(g, 6, 6)) can exceed dimension_count here: the
                // parser always splits `g[i][j]`/`g[i,j]` into one index per
                // opcode, since it cannot know at compile time whether the
                // runtime value will be jagged (each level dimensions==1) or
                // a single flat multi-dim value. When fewer indices were
                // supplied than the array actually has, treat them as
                // selecting a sub-array rather than a final scalar --
                // computeFlatOffset()/calculateArrayTotalSize() below assume
                // dimension_count == array->dimensions and would otherwise
                // read indices[] past its allocated length.
                if (dimension_count < (uint8_t)ARRAY_DIMENSIONS(*array_val_ptr)) {
                    Value sliceVal;
                    bool slice_oob = false;
                    bool slice_ok = makeDynamicArraySliceValue(array_val_ptr, dimension_count, indices, &sliceVal, &slice_oob);
                    if (indices != indices_inline) free(indices);
                    if (!slice_ok) {
                        runtimeError(vm, "VM Error: Array element index out of bounds.");
                        if (VALUE_TYPE(operand) == TYPE_POINTER) freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, sliceVal);
                    freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    break;
                }

                /* Bounds-check before computeFlatOffset(), which would otherwise
                 * print a location-less line and exit. Note this site's operand
                 * cleanup is conditional, unlike the other call site's. */
                if (!vmCheckArrayIndexBounds(vm, array_val_ptr, indices)) {
                    if (indices != indices_inline) free(indices);
                    if (VALUE_TYPE(operand) == TYPE_POINTER) freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                if (indices != indices_inline) free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (VALUE_TYPE(operand) == TYPE_POINTER) freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeByte(AS_ARRAY_RAW(*array_val_ptr)[offset]));
                } else {
                    push(vm, copyValueForStack(&AS_ARRAY(*array_val_ptr)[offset]));
                }

                freeValue(&operand);
                if (using_wrapper) {
                    vmFreeArrayIndexWrapper(&temp_wrapper);
                }
                if (using_snapshot) {
                    freeValue(&shared_snapshot);
                }

                break;
            }
            case LOAD_ELEMENT_VALUE_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);

                Value operand = pop(vm);

                // See GET_ELEMENT_ADDRESS: such an address is not a Value.
                if (VALUE_TYPE(operand) == TYPE_POINTER && vmPointerTargetIsNotValue(&operand)) {
                    runtimeError(vm, "VM Error: Cannot index through a character, byte or opaque address.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* base_val = (Value*)AS_POINTER(operand);
                    if (base_val && isPascalStringType(VALUE_TYPE(*base_val))) {
                        const char* str = AS_STRING(*base_val) ? AS_STRING(*base_val) : "";
                        size_t len = strlen(str);

                        if ((size_t)flat_offset >= len) {
                            unsigned long long display_index = vmDisplayIndexFromOffset(flat_offset);
                            runtimeError(vm,
                                         "Runtime Error: String index (%llu) out of bounds for string of length %zu.",
                                         display_index, len);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        push(vm, makeChar(str[flat_offset]));
                        freeValue(&operand);
                        break;
                    }
                }

                Value* array_val_ptr = NULL;
                Value shared_snapshot = makeNil();
                // Initialised lazily, only on the pointer-to-array-type path
                // that actually uses it: an eager init callocs an ArrayObj on
                // every element access and only that path ever frees it.
                Value temp_wrapper;
                memset(&temp_wrapper, 0, sizeof(temp_wrapper));
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (!candidate || vmValueIsDisposed(candidate)) {
                        runtimeError(vm, candidate ? "VM Error: Array access through a dangling pointer (its target was disposed)."
                                                   : "VM Error: Array access through a nil pointer.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        // Checking ARRAY_IS_DYNAMIC(*candidate) here first,
                        // before any lock, would itself race a concurrent
                        // SetLength publish (dynamic_array_fresh_publish_race
                        // follow-up, pscal-core builtin.c's
                        // resizeDynamicArrayValue) -- it dereferences
                        // candidate's live bits pointer the same way the
                        // element read below does. copyDynamicArraySnapshotValue()
                        // takes the whole-struct copy, the is_dynamic check,
                        // and the conditional retain all under one critical
                        // section, so call it unconditionally and only
                        // branch on dynamic-vs-static from the now-safe
                        // local copy.
                        shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                        if (ARRAY_IS_DYNAMIC(shared_snapshot)) {
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        vmInitArrayIndexWrapper(&temp_wrapper);
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        {
                            // SET_VALUE_TYPE resets .bits to a nil-pointer
                            // placeholder (VM 2.0 Phase 4i checkpoint 3b), but
                            // temp_wrapper already has a real ArrayObj* from
                            // vmInitArrayIndexWrapper's pscalArrayEnsureObj
                            // call just above -- capture it first and re-tag with
                            // the wrapper that's actually there, same
                            // "re-tag after retype" fix checkpoint 2 needed
                            // for TYPE_THREAD.
                            ArrayObj *reset_a = PSCAL_VALUE_PTR(temp_wrapper, ArrayObj);
                            SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
                            pscalValueSetHeapPtrBits(&temp_wrapper, reset_a);
                        }
                        ARRAY_DIMENSIONS(temp_wrapper) = dims;
                        ARRAY_ELEMENT_TYPE(temp_wrapper) = vmResolveArrayElementType(arrayType);
                        ARRAY_IS_PACKED(temp_wrapper) = isPackedByteElementType(ARRAY_ELEMENT_TYPE(temp_wrapper));
                        if (ARRAY_IS_PACKED(temp_wrapper)) {
                            AS_ARRAY_RAW(temp_wrapper) = (uint8_t*)AS_POINTER(operand);
                            AS_ARRAY(temp_wrapper) = NULL;
                        } else {
                            AS_ARRAY(temp_wrapper) = (Value*)AS_POINTER(operand);
                            AS_ARRAY_RAW(temp_wrapper) = NULL;
                        }
                        ARRAY_LOWER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        ARRAY_UPPER_BOUNDS(temp_wrapper) = malloc(sizeof(int) * dims);
                        if (!ARRAY_LOWER_BOUNDS(temp_wrapper) || !ARRAY_UPPER_BOUNDS(temp_wrapper)) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                vmFreeArrayIndexWrapper(&temp_wrapper);
                                freeValue(&operand);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            ARRAY_LOWER_BOUNDS(temp_wrapper)[i] = lb;
                            ARRAY_UPPER_BOUNDS(temp_wrapper)[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "[AETH-RUNTIME-INDEX-NOT-ARRAY] cannot index this value: it is not an array. hint: indexing a 1-D array two-dimensionally (dp[i][j]) does this -- a `let dp: Int[]` is flat, so use a single index dp[i * width + j], or build an array of arrays.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        vmFreeArrayIndexWrapper(&temp_wrapper);
                    }
                    if (using_snapshot) {
                        freeValue(&shared_snapshot);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            vmFreeArrayIndexWrapper(&temp_wrapper);
                        }
                        if (using_snapshot) {
                            freeValue(&shared_snapshot);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeByte(AS_ARRAY_RAW(*array_val_ptr)[flat_offset]));
                } else {
                    push(vm, copyValueForStack(&AS_ARRAY(*array_val_ptr)[flat_offset]));
                }

                freeValue(&operand);
                if (using_wrapper) {
                    vmFreeArrayIndexWrapper(&temp_wrapper);
                }
                if (using_snapshot) {
                    freeValue(&shared_snapshot);
                }

                break;
            }
            case SET_INDIRECT: {
                Value value_to_set = pop(vm);
                Value pointer_to_lvalue = pop(vm);
                if (vmPasExceptionPending(vm)) {
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    break;
                }
                if (VALUE_TYPE(pointer_to_lvalue) != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: SET_INDIRECT requires an address on the stack.");
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (PTR_BASE_TYPE_NODE(pointer_to_lvalue) == STRING_CHAR_PTR_SENTINEL ||
                    PTR_BASE_TYPE_NODE(pointer_to_lvalue) == SERIALIZED_CHAR_PTR_SENTINEL) {
                    PointerObj* char_po = PSCAL_VALUE_PTR(pointer_to_lvalue, PointerObj);
                    if (char_po && char_po->retained_string && char_po->string_index_is_codepoint) {
                        // A UnicodeString character: re-encoded and spliced
                        // in, since its UTF-8 length can change.
                        bool stored = vmStoreUnicodeChar(vm, char_po, &value_to_set);
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        if (!stored) {
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        break;
                    }
                    char* char_target_addr = vmStringCharTarget(vm, &pointer_to_lvalue,
                                                                "VM Error: Attempting to assign to a NULL character address.");
                    if (char_target_addr == NULL) {
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                        *char_target_addr = AS_CHAR(value_to_set);
                    } else if (VALUE_TYPE(value_to_set) == TYPE_WIDECHAR) {
                        /* Rea's `str` type (and other Unicode-aware string
                         * contexts) is TYPE_UNICODE_STRING, so indexing any
                         * such string legitimately yields a WIDECHAR codepoint
                         * even when every character in it happens to be ASCII.
                         * Narrow it into the single raw byte here, the same
                         * way a single-char TYPE_STRING is narrowed above;
                         * only reject codepoints that can't fit in one byte. */
                        long long codepoint = AS_CHAR(value_to_set);
                        if (codepoint < 0 || codepoint > PASCAL_CHAR_MAX) {
                            runtimeError(vm, "VM Error: Cannot assign multi-byte character (codepoint %lld) to a single-byte character location.", codepoint);
                            freeValue(&value_to_set);
                            freeValue(&pointer_to_lvalue);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        *char_target_addr = (char)codepoint;
                    } else if (VALUE_TYPE(value_to_set) == TYPE_STRING) {
                        if (AS_STRING(value_to_set) && strlen(AS_STRING(value_to_set)) == 1) {
                            *char_target_addr = AS_STRING(value_to_set)[0];
                        } else {
                            runtimeError(vm, "VM Error: Cannot assign multi-character or empty string to a single character location.");
                            freeValue(&value_to_set);
                            freeValue(&pointer_to_lvalue);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        runtimeError(vm, "VM Error: Type mismatch for character assignment. Expected CHAR or single-char STRING, got %s.", varTypeToString(VALUE_TYPE(value_to_set)));
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (PTR_BASE_TYPE_NODE(pointer_to_lvalue) == BYTE_ARRAY_PTR_SENTINEL) {
                    uint8_t* byte_target_addr = (uint8_t*)AS_POINTER(pointer_to_lvalue);
                    if (byte_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to assign to a NULL byte address.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    unsigned long long stored = 0;
                    bool range_error = false;
                    if (isRealType(VALUE_TYPE(value_to_set))) {
                        long double real_val = AS_REAL(value_to_set);
                        if (real_val < 0.0L) {
                            range_error = true;
                            stored = 0;
                        } else if (real_val > (long double)UINT8_MAX) {
                            range_error = true;
                            stored = UINT8_MAX;
                        } else {
                            stored = (unsigned long long)real_val;
                        }
                    } else if (isIntlikeType(VALUE_TYPE(value_to_set))) {
                        long long val = AS_INTEGER(value_to_set);
                        if (val < 0 || val > 255) {
                            range_error = true;
                        }
                        stored = (unsigned long long)(val & 0xFF);
                    } else {
                        runtimeError(vm, "VM Error: Type mismatch for byte assignment. Expected numeric type, got %s.",
                                     varTypeToString(VALUE_TYPE(value_to_set)));
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (range_error) {
                        runtimeWarning(vm, "Warning: Range check error assigning to BYTE.");
                    }
                    *byte_target_addr = (uint8_t)stored;
                } else if (PTR_BASE_TYPE_NODE(pointer_to_lvalue) == STRING_LENGTH_SENTINEL) {
                    if (!frontendIsZeroBasedStrings()) {
                        runtimeError(vm, "VM Error: Cannot assign to string length.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (PTR_BASE_TYPE_NODE(pointer_to_lvalue) == SHELL_FUNCTION_PTR_SENTINEL ||
                           PTR_BASE_TYPE_NODE(pointer_to_lvalue) == OPAQUE_POINTER_SENTINEL) {
                    runtimeError(vm, "VM Error: Cannot assign through opaque/function pointer constants.");
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                } else {
                    // This is the start of your existing logic for other types
                    Value* target_lvalue_ptr = (Value*)AS_POINTER(pointer_to_lvalue);
                    if (!target_lvalue_ptr) {
                        runtimeError(vm, "VM Error: SET_INDIRECT called with a nil LValue pointer.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (vmValueIsDisposed(target_lvalue_ptr)) {
                        runtimeError(vm, "VM Error: Assignment through a dangling pointer (its target was disposed).");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    AST* declaredTypeNode = PTR_BASE_TYPE_NODE(pointer_to_lvalue);
                    if (declaredTypeNode == OWNED_POINTER_SENTINEL) {
                        // GET_ELEMENT_ADDRESS's row of a multi-dimensional
                        // array (`m[1] := row`): a view whose storage is m's
                        // (or, for an array of arrays, the row array itself).
                        // It carries no declared type, and replacing the view
                        // Value would drop the write, so copy the elements
                        // through it.
                        if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_ARRAY) {
                            bool stored = VALUE_TYPE(value_to_set) == TYPE_ARRAY &&
                                          valueAssignArrayElementsInPlace(target_lvalue_ptr, &value_to_set);
                            if (!stored) {
                                runtimeError(vm, "Runtime Error: Cannot assign %s to an array row of a different shape.",
                                             VALUE_TYPE(value_to_set) == TYPE_ARRAY ? "an array" : varTypeToString(VALUE_TYPE(value_to_set)));
                            }
                            freeValue(&value_to_set);
                            freeValue(&pointer_to_lvalue);
                            if (!stored) {
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            break;
                        }
                        declaredTypeNode = NULL;
                    }
                    VarType declaredType = vmDeclaredVarType(declaredTypeNode);
                    AST* resolvedDeclaredType = vmResolveTypeAlias(declaredTypeNode);
                    if (declaredType != TYPE_VOID) {
                        bool needs_reset = false;
                        if (declaredType == TYPE_POINTER) {
                            if (VALUE_TYPE(*target_lvalue_ptr) != TYPE_POINTER) {
                                needs_reset = true;
                            }
                        } else if (declaredType == TYPE_RECORD || declaredType == TYPE_ARRAY ||
                                   declaredType == TYPE_STRING || declaredType == TYPE_ENUM ||
                                   declaredType == TYPE_INTERFACE) {
                            if (VALUE_TYPE(*target_lvalue_ptr) != declaredType) {
                                needs_reset = true;
                            }
                        } else if (VALUE_TYPE(*target_lvalue_ptr) != declaredType) {
                            needs_reset = true;
                        }

                        if (needs_reset) {
                            freeValue(target_lvalue_ptr);
                            *target_lvalue_ptr = makeValueForType(declaredType, resolvedDeclaredType, NULL);
                        } else if (declaredType == TYPE_POINTER && resolvedDeclaredType &&
                                   resolvedDeclaredType->type == AST_POINTER_TYPE &&
                                   resolvedDeclaredType->right) {
                            PTR_BASE_TYPE_NODE(*target_lvalue_ptr) = resolvedDeclaredType->right;
                        }
                    }

                    // (Your existing logic for handling fixed-length strings, pointers, reals, etc. goes here)
                    // Dynamic-string targets get a whole new Value installed
                    // through replaceValueCell, never a free-then-rebuild of
                    // the cell in place: another thread copying this cell
                    // (copyValueForStack, under value_cell_mutex) could
                    // otherwise retain the StringObj just freed here.
                    if (isPascalStringType(VALUE_TYPE(*target_lvalue_ptr)) && STRING_MAX_LENGTH(*target_lvalue_ptr) <= 0) {
                        if (VALUE_TYPE(value_to_set) == TYPE_CHAR || VALUE_TYPE(value_to_set) == TYPE_WIDECHAR) {
                            VarType destType = VALUE_TYPE(*target_lvalue_ptr) == TYPE_UNICODE_STRING
                                                   ? TYPE_UNICODE_STRING
                                                   : TYPE_STRING;
                            char encoded[5] = {0};
                            size_t encoded_len = 0;
                            if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                                encoded[0] = (char)AS_CHAR(value_to_set);
                                encoded_len = 1;
                            } else {
                                encoded_len = encodeUtf8Codepoint((uint32_t)AS_CHAR(value_to_set), encoded);
                            }
                            Value replacement = (destType == TYPE_UNICODE_STRING)
                                                    ? makeUnicodeStringLen(encoded, encoded_len)
                                                    : makeStringLen(encoded, encoded_len);
                            replaceValueCell(target_lvalue_ptr, replacement, NULL);
                        } else if (isPascalStringType(VALUE_TYPE(value_to_set)) && AS_STRING(value_to_set)) {
                            VarType destType = VALUE_TYPE(*target_lvalue_ptr);
                            StringObj *src_s = PSCAL_VALUE_PTR(value_to_set, StringObj);
                            Value replacement;
                            if (src_s->max_length > 0) {
                                /* A string[N] source's StringObj is still that
                                 * variable's own fixed buffer (GET_* shares it
                                 * by reference count). Taking it over would
                                 * share the buffer with the target and strip
                                 * its length cap, so copy the text into a
                                 * fresh dynamic string instead. */
                                replacement = (destType == TYPE_UNICODE_STRING)
                                                  ? makeUnicodeString(AS_STRING(value_to_set))
                                                  : makeString(AS_STRING(value_to_set));
                            } else {
                                /* Optimization: take over the temporary's
                                 * whole (dynamic) StringObj reference rather
                                 * than copying its buffer. */
                                pscalValueSetHeapPtrBits(&value_to_set, NULL); /* Prevent double-free */
                                replacement = makeVoid();
                                SET_VALUE_TYPE(&replacement, destType);
                                pscalValueSetHeapPtrBits(&replacement, src_s);
                            }
                            replaceValueCell(target_lvalue_ptr, replacement, NULL);
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign this type to a dynamic string.");
                        }
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_STRING && STRING_MAX_LENGTH(*target_lvalue_ptr) > 0) {
                        // The fixed buffer is written in place (char
                        // addresses into it stay valid), so first make sure
                        // this variable is its only holder: GET_* shares a
                        // StringObj by reference count, and a by-value
                        // parameter or a copy would otherwise change too.
                        valueEnsureUnique(target_lvalue_ptr);
                        if (isPascalStringType(VALUE_TYPE(value_to_set)) && AS_STRING(value_to_set)) {
                            strncpy(AS_STRING(*target_lvalue_ptr), AS_STRING(value_to_set), STRING_MAX_LENGTH(*target_lvalue_ptr));
                            AS_STRING(*target_lvalue_ptr)[STRING_MAX_LENGTH(*target_lvalue_ptr)] = '\0'; // Ensure null termination
                        } else if (VALUE_TYPE(value_to_set) == TYPE_CHAR ||
                                   VALUE_TYPE(value_to_set) == TYPE_WIDECHAR) {
                            char encoded[5] = {0};
                            size_t encoded_len;
                            if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                                encoded[0] = (char)AS_CHAR(value_to_set);
                                encoded_len = 1;
                            } else {
                                encoded_len = encodeUtf8Codepoint((uint32_t)AS_CHAR(value_to_set), encoded);
                            }
                            size_t cap = (size_t)STRING_MAX_LENGTH(*target_lvalue_ptr);
                            if (encoded_len > cap) encoded_len = cap;
                            memcpy(AS_STRING(*target_lvalue_ptr), encoded, encoded_len);
                            AS_STRING(*target_lvalue_ptr)[encoded_len] = '\0';
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign this type to a fixed-length string.");
                        }
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_POINTER && (VALUE_TYPE(value_to_set) == TYPE_POINTER || VALUE_TYPE(value_to_set) == TYPE_NIL)) {
                        if (VALUE_TYPE(value_to_set) == TYPE_NIL) {
                            // Preserve base type when assigning nil
                            AS_POINTER(*target_lvalue_ptr) = NULL;
                        } else {
                            AS_POINTER(*target_lvalue_ptr) = AS_POINTER(value_to_set);
                            if (PTR_BASE_TYPE_NODE(value_to_set)) {
                                PTR_BASE_TYPE_NODE(*target_lvalue_ptr) = PTR_BASE_TYPE_NODE(value_to_set);
                            }
                        }
                    }
                    else if (isRealType(VALUE_TYPE(*target_lvalue_ptr)) && isRealType(VALUE_TYPE(value_to_set))) {
                        long double tmp = AS_REAL(value_to_set);
                        SET_REAL_VALUE(target_lvalue_ptr, tmp);
                    }
                    else if (isRealType(VALUE_TYPE(*target_lvalue_ptr)) && isIntlikeType(VALUE_TYPE(value_to_set))) {
                        long double tmp = asLd(value_to_set);
                        SET_REAL_VALUE(target_lvalue_ptr, tmp);
                    }
                    else if (isIntlikeType(VALUE_TYPE(*target_lvalue_ptr)) && isRealType(VALUE_TYPE(value_to_set))) {
                        assignRealToIntChecked(vm, target_lvalue_ptr, AS_REAL(value_to_set));
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_BYTE && VALUE_TYPE(value_to_set) == TYPE_INTEGER) {
                        if (VAL_INT(value_to_set) < 0 || VAL_INT(value_to_set) > 255) {
                            runtimeWarning(vm, "Warning: Range check error assigning INTEGER %lld to BYTE.", VAL_INT(value_to_set));
                        }
                        SET_INT_VALUE(target_lvalue_ptr, VAL_INT(value_to_set) & 0xFF);
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_WORD && VALUE_TYPE(value_to_set) == TYPE_INTEGER) {
                        if (VAL_INT(value_to_set) < 0 || VAL_INT(value_to_set) > 65535) {
                            runtimeWarning(vm, "Warning: Range check error assigning INTEGER %lld to WORD.", VAL_INT(value_to_set));
                        }
                        SET_INT_VALUE(target_lvalue_ptr, VAL_INT(value_to_set) & 0xFFFF);
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_INTEGER &&
                             (VALUE_TYPE(value_to_set) == TYPE_BYTE || VALUE_TYPE(value_to_set) == TYPE_WORD || VALUE_TYPE(value_to_set) == TYPE_BOOLEAN)) {
                        SET_INT_VALUE(target_lvalue_ptr, VAL_INT(value_to_set));
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_INTEGER && VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                        SET_INT_VALUE(target_lvalue_ptr, (long long)AS_CHAR(value_to_set));
                    }
                    else if (isIntlikeType(VALUE_TYPE(*target_lvalue_ptr)) && isIntlikeType(VALUE_TYPE(value_to_set)) &&
                             VALUE_TYPE(*target_lvalue_ptr) != VALUE_TYPE(value_to_set)) {
                        /* Catch-all for int-like-to-int-like assignments not
                         * handled by a more specific branch above (notably
                         * TYPE_INT64/TYPE_UINT64 targets, which have no
                         * dedicated branch here). Without this, execution
                         * falls through to the generic "replace the whole
                         * cell" branch at the bottom of this if-chain, which
                         * blindly copies value_to_set's OWN type onto the
                         * target -- clobbering a declared int64/uint64
                         * variable down to whatever narrower type the RHS
                         * literal/expression happened to be (e.g. `sum := 0`
                         * on an int64 `sum` silently downgraded it to
                         * TYPE_INT32). SET_INT_VALUE preserves the target's
                         * existing declared width instead. */
                        SET_INT_VALUE(target_lvalue_ptr, AS_INTEGER(value_to_set));
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_CHAR) {
                        if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                            SET_CHAR_VALUE(target_lvalue_ptr, AS_CHAR(value_to_set));
                        } else if (isPascalStringType(VALUE_TYPE(value_to_set)) && AS_STRING(value_to_set)) {
                            size_t len = strlen(AS_STRING(value_to_set));
                            if (len == 1) {
                                SET_CHAR_VALUE(target_lvalue_ptr, (unsigned char)AS_STRING(value_to_set)[0]);
                            } else if (len == 0) {
                                SET_CHAR_VALUE(target_lvalue_ptr, '\0');
                            } else {
                                runtimeError(vm, "Type mismatch: Cannot assign multi-character string to CHAR.");
                            }
                        } else if (VALUE_TYPE(value_to_set) == TYPE_INTEGER) {
                            SET_CHAR_VALUE(target_lvalue_ptr, (int)VAL_INT(value_to_set));
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign %s to CHAR.", varTypeToString(VALUE_TYPE(value_to_set)));
                        }
                        SET_INT_VALUE(target_lvalue_ptr, AS_CHAR(*target_lvalue_ptr));
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_WIDECHAR) {
                        if (VALUE_TYPE(value_to_set) == TYPE_WIDECHAR || VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                            SET_CHAR_VALUE(target_lvalue_ptr, AS_CHAR(value_to_set));
                        } else if (isPascalStringType(VALUE_TYPE(value_to_set)) && AS_STRING(value_to_set)) {
                            size_t len = strlen(AS_STRING(value_to_set));
                            uint32_t codepoint = 0;
                            size_t advance = 0;
                            if (len == 0) {
                                SET_CHAR_VALUE(target_lvalue_ptr, 0);
                            } else if (decodeUtf8Codepoint(AS_STRING(value_to_set), len, &codepoint, &advance) &&
                                       advance == len) {
                                SET_CHAR_VALUE(target_lvalue_ptr, (int)codepoint);
                            } else {
                                runtimeError(vm, "Type mismatch: Cannot assign multi-character string to WIDECHAR.");
                            }
                        } else if (VALUE_TYPE(value_to_set) == TYPE_INTEGER) {
                            SET_CHAR_VALUE(target_lvalue_ptr, (int)VAL_INT(value_to_set));
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign %s to WIDECHAR.", varTypeToString(VALUE_TYPE(value_to_set)));
                        }
                        SET_INT_VALUE(target_lvalue_ptr, AS_CHAR(*target_lvalue_ptr));
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_ENUM &&
                             isIntegerFamilyType(VALUE_TYPE(value_to_set))) {
                        /* An integer stored into an enum variable: what a
                         * Pascal cast `e := T(k)` compiles to, since the cast
                         * itself emits no conversion. Replacing the whole
                         * cell made `e` an INTEGER that no longer compared
                         * with T's members ("Operands not comparable");
                         * keep the enum type and store the ordinal. */
                        long long ordinal = asI64(value_to_set);
                        Type* meta = ENUM_META(*target_lvalue_ptr);
                        const char* enum_name = AS_ENUM(*target_lvalue_ptr).enum_name;
                        if (meta ? (ordinal < 0 || ordinal >= meta->member_count)
                                 : (ordinal < INT32_MIN || ordinal > INT32_MAX)) {
                            runtimeError(vm, "Runtime Error: Enum '%s' out of range (ordinal %lld).",
                                         enum_name ? enum_name : "<anon>", ordinal);
                            freeValue(&value_to_set);
                            freeValue(&pointer_to_lvalue);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Value replacement = makeEnum(enum_name, (int)ordinal);
                        ENUM_META(replacement) = meta;
                        ENUM_TYPE_DEF(replacement) = ENUM_TYPE_DEF(*target_lvalue_ptr);
                        replaceValueCell(target_lvalue_ptr, replacement, NULL);
                    }
                    else {
                        // VM 2.0 Phase 4i: target_lvalue_ptr's type is never
                        // TYPE_POINTER at this point (that case is handled
                        // by the branch above) -- guard anyway rather than
                        // rely on that invariant holding forever, since
                        // PTR_BASE_TYPE_NODE now requires it.
                        AST* preserved_base = (VALUE_TYPE(*target_lvalue_ptr) == TYPE_POINTER)
                                                   ? PTR_BASE_TYPE_NODE(*target_lvalue_ptr)
                                                   : NULL;
                        if (VALUE_TYPE(value_to_set) == TYPE_MEMORYSTREAM) {
                            /* Transfer ownership of the MStream pointer without freeing it
                             * when the temporary value is cleaned up below. */
                            Value replacement = value_to_set;
                            replaceValueCell(target_lvalue_ptr, replacement, preserved_base);
                            pscalValueSetHeapPtrBits(&value_to_set, NULL);
                            // Detached the alias above without updating .bits
                            // (VM 2.0 Phase 4i checkpoint 3b) -- value_to_set
                            // is freeValue'd a few lines down and would
                            // otherwise abort on a stale non-NULL pointer tag.
                            pscalValueSetHeapPtrBits(&value_to_set, NULL);
                        } else if (!valueAssignAggregateInPlace(target_lvalue_ptr, &value_to_set)) {
                            // (A record or static array assigned over one of
                            // the same shape was instead copied into the
                            // target's own storage just now: Pascal keeps a
                            // variable's storage across `r := r2`, and VAR
                            // parameters may point into it.)
                            Value replacement = makeCopyOfValue(&value_to_set);
                            replaceValueCell(target_lvalue_ptr, replacement, preserved_base);
                        }
                    }
                }

                /*
                 * In Pascal, assignments are statements and do not yield a
                 * value.  The previous implementation pushed a copy of the
                 * assigned value onto the stack, mirroring C-like semantics.
                 * This resulted in stray values accumulating on the VM stack
                 * for every indirect assignment (e.g. array or record field
                 * writes).  Large programs such as the SDL multi bouncing balls
                 * demo perform many such assignments each frame, eventually
                 * exhausting the stack and triggering a runtime stack overflow.
                 *
                 * To restore correct Pascal semantics and prevent the leak,
                 * simply discard the temporary value after storing it.
                 */
                freeValue(&value_to_set);
                freeValue(&pointer_to_lvalue);
                break;
            }
            case IN: {
                Value set_value = pop(vm);
                Value item_val = pop(vm);

                if (VALUE_TYPE(set_value) != TYPE_SET) {
                    runtimeError(vm, "Right operand of IN must be a set.");
                    freeValue(&item_val);
                    freeValue(&set_value);
                    return INTERPRET_RUNTIME_ERROR;
                }

                bool result = vmSetContains(&set_value, &item_val);

                freeValue(&item_val);
                freeValue(&set_value);

                push(vm, makeBoolean(result));
                break;
            }

            case MAKE_SET_SINGLETON: {
                Value item_val = pop(vm);
                long long ordinal = 0;
                if (!tryValueToOrdinal(&item_val, &ordinal)) {
                    runtimeError(vm, "Set constructor element must be an ordinal value.");
                    freeValue(&item_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value set_value;
                if (!vmBuildSetFromOrdinal(&set_value, ordinal)) {
                    runtimeError(vm, "VM Error: Failed to allocate set singleton.");
                    freeValue(&set_value);
                    freeValue(&item_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&item_val);
                push(vm, set_value);
                break;
            }

            case MAKE_SET_RANGE: {
                Value end_val = pop(vm);
                Value start_val = pop(vm);
                long long start_ord = 0;
                long long end_ord = 0;
                if (!tryValueToOrdinal(&start_val, &start_ord) ||
                    !tryValueToOrdinal(&end_val, &end_ord)) {
                    runtimeError(vm, "Set constructor range bounds must be ordinal values.");
                    freeValue(&start_val);
                    freeValue(&end_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value set_value;
                VmSetRangeStatus range_status = vmBuildSetFromRange(&set_value, start_ord, end_ord);
                if (range_status != VM_SET_RANGE_OK) {
                    if (range_status == VM_SET_RANGE_TOO_LARGE) {
                        runtimeError(vm, "Runtime Error: Set range [%lld..%lld] is too large (at most %u elements).",
                                     start_ord, end_ord, (unsigned)VM_SET_RANGE_MAX_ELEMENTS);
                    } else {
                        runtimeError(vm, "VM Error: Failed to allocate set range.");
                    }
                    freeValue(&start_val);
                    freeValue(&end_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&start_val);
                freeValue(&end_val);
                push(vm, set_value);
                break;
            }

            case GET_INDIRECT: {
                Value pointer_val = pop(vm);
                if (VALUE_TYPE(pointer_val) != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: GET_INDIRECT requires an address on the stack.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (PTR_BASE_TYPE_NODE(pointer_val) == STRING_CHAR_PTR_SENTINEL ||
                    PTR_BASE_TYPE_NODE(pointer_val) == SERIALIZED_CHAR_PTR_SENTINEL) {
                    // Special case: pointer into a string's character buffer.
                    PointerObj* char_po = PSCAL_VALUE_PTR(pointer_val, PointerObj);
                    if (char_po && char_po->retained_string && char_po->string_index_is_codepoint) {
                        Value ch;
                        if (!vmLoadUnicodeChar(vm, char_po, &ch)) {
                            freeValue(&pointer_val);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        push(vm, ch);
                    } else {
                        char* char_target_addr = vmStringCharTarget(vm, &pointer_val,
                                                                    "VM Error: Attempting to dereference a NULL character address.");
                        if (char_target_addr == NULL) {
                            freeValue(&pointer_val);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        push(vm, makeChar(*char_target_addr));
                    }
                } else if (PTR_BASE_TYPE_NODE(pointer_val) == BYTE_ARRAY_PTR_SENTINEL) {
                    uint8_t* byte_target_addr = (uint8_t*)AS_POINTER(pointer_val);
                    if (byte_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to dereference a NULL byte address.");
                        freeValue(&pointer_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeByte(*byte_target_addr));
                } else if (PTR_BASE_TYPE_NODE(pointer_val) == STRING_LENGTH_SENTINEL && !frontendIsZeroBasedStrings()) {
                    // Special case: request for string length via element 0.
                    Value* str_val = (Value*)AS_POINTER(pointer_val);
                    size_t len = 0;
                    if (str_val && AS_STRING(*str_val)) {
                        size_t byte_len = strlen(AS_STRING(*str_val));
                        len = (VALUE_TYPE(*str_val) == TYPE_UNICODE_STRING)
                            ? utf8CodepointCount(AS_STRING(*str_val), byte_len)
                            : byte_len;
                    }
                    push(vm, makeInt((long long)len));
                } else if (PTR_BASE_TYPE_NODE(pointer_val) == SHELL_FUNCTION_PTR_SENTINEL ||
                           PTR_BASE_TYPE_NODE(pointer_val) == OPAQUE_POINTER_SENTINEL) {
                    runtimeError(vm, "VM Error: Cannot dereference opaque/function pointer constants.");
                    freeValue(&pointer_val);
                    return INTERPRET_RUNTIME_ERROR;
                } else {
                    Value* target_lvalue_ptr = (Value*)AS_POINTER(pointer_val);
                    if (target_lvalue_ptr == NULL) {
                        runtimeError(vm, "VM Error: GET_INDIRECT on a nil pointer.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (vmValueIsDisposed(target_lvalue_ptr)) {
                        runtimeError(vm, "VM Error: Dereference of a dangling pointer (its target was disposed).");
                        freeValue(&pointer_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, copyValueForStack(target_lvalue_ptr));
                }
                freeValue(&pointer_val);
                break;
            }

            case GET_CHAR_FROM_STRING: {
                 Value index_val = pop(vm);
                 Value base_val = pop(vm); // Can be string or char

                 if (!isIntlikeType(VALUE_TYPE(index_val))) {
                     runtimeError(vm, "VM Error: String/Char index must be an integer.");
                     freeValue(&index_val); freeValue(&base_val);
                     return INTERPRET_RUNTIME_ERROR;
                 }

                 long long pscal_index = VAL_INT(index_val);

                 if (VALUE_TYPE(base_val) == TYPE_UNICODE_STRING) {
                     const char* str = AS_STRING(base_val) ? AS_STRING(base_val) : "";
                     size_t len = strlen(str);
                     size_t cp_count = utf8CodepointCount(str, len);
                     long long expected_index = frontendIsZeroBasedStrings() ? 0 : 1;
                     if (pscal_index < expected_index ||
                         (size_t)(pscal_index - expected_index) >= cp_count) {
                         runtimeError(vm, "Runtime Error: String index %lld out of bounds for UnicodeString length %zu.",
                                      pscal_index, cp_count);
                         freeValue(&index_val); freeValue(&base_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     size_t cp_index = (size_t)(pscal_index - expected_index);
                     size_t byte_offset = utf8ByteOffsetForCodepointIndex(str, len, cp_index);
                     uint32_t codepoint = 0;
                     size_t advance = 0;
                     if (!decodeUtf8Codepoint(str + byte_offset, len - byte_offset, &codepoint, &advance)) {
                         runtimeError(vm, "VM Error: Failed to decode UnicodeString character.");
                         freeValue(&index_val); freeValue(&base_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     push(vm, makeWideChar((int)codepoint));
                 } else {
                     char result_char;
                     if (VALUE_TYPE(base_val) == TYPE_STRING) {
                     const char* str = AS_STRING(base_val) ? AS_STRING(base_val) : "";
                     size_t len = strlen(str);
                     size_t char_offset = 0;
                     if (!vmResolveStringIndex(vm, pscal_index, len, &char_offset, false, NULL)) {
                         freeValue(&index_val); freeValue(&base_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     result_char = str[char_offset];
                     } else if (VALUE_TYPE(base_val) == TYPE_CHAR) {
                    long long expected_index = frontendIsZeroBasedStrings() ? 0 : 1;
                    if (pscal_index != expected_index) {
                        runtimeError(vm,
                                     "Runtime Error: Index for a CHAR type must be %lld, got %lld.",
                                     expected_index,
                                     pscal_index);
                        freeValue(&index_val);
                        freeValue(&base_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    result_char = AS_CHAR(base_val);
                     } else if (VALUE_TYPE(base_val) == TYPE_WIDECHAR) {
                         long long expected_index = frontendIsZeroBasedStrings() ? 0 : 1;
                         if (pscal_index != expected_index) {
                             runtimeError(vm,
                                          "Runtime Error: Index for a WIDECHAR type must be %lld, got %lld.",
                                          expected_index,
                                          pscal_index);
                             freeValue(&index_val);
                             freeValue(&base_val);
                             return INTERPRET_RUNTIME_ERROR;
                         }
                         push(vm, makeWideChar(AS_CHAR(base_val)));
                         freeValue(&index_val);
                         freeValue(&base_val);
                         break;
                     } else {
                     runtimeError(vm, "VM Error: Base for character index is not a string or char. Got %s", varTypeToString(VALUE_TYPE(base_val)));
                     freeValue(&index_val); freeValue(&base_val);
                     return INTERPRET_RUNTIME_ERROR;
                     }

                     push(vm, makeChar(result_char));
                 }

                 freeValue(&index_val);
                 freeValue(&base_val);
                 break;
             }
            // DEFINE_GLOBAL/DEFINE_GLOBAL16 (0x20/0x21) are retired as of VM
            // 2.0 Phase 2b -- superseded by DEFINE_GLOBAL_SLOT below. No case
            // for them here means an adversarial chunk containing one falls
            // through to default: and gets a clean "unknown opcode" error,
            // same as the Phase 2a GET/SET_GLOBAL[16]_CACHED retirement.
            case DEFINE_GLOBAL_SLOT: {
                // vm->ip is one byte past the opcode here, so this is the
                // offset of the instruction itself -- its identity, used to
                // tell a loop body's re-run apart from a second declaration.
                int define_pc = (int)(vm->ip - vm->chunk->code) - 1;
                uint16_t slot = READ_SHORT(vm);
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobalSlot(vm, slot, define_pc);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            // GET_GLOBAL/SET_GLOBAL/GET_GLOBAL16/SET_GLOBAL16 (0x22/0x23/
            // 0x25/0x26) are retired as of VM 2.0 Phase 2b -- superseded by
            // GET_GSLOT/SET_GSLOT below. No per-call-site cache, no
            // "populated?" branch: `chunk->global_slots[slot]` is resolved
            // once, either by the link step (const globals, at load time)
            // or by DEFINE_GLOBAL_SLOT (mutable globals, the first and only
            // time it executes for that slot), and every GET_GSLOT/SET_GSLOT
            // after that is a direct array index.
            case GET_GSLOT: {
                uint16_t slot = READ_SHORT(vm);
                if (slot >= (uint16_t)vm->chunk->global_slot_count) {
                    runtimeError(vm, "VM Error: Global slot %u out of bounds for GET_GSLOT.", (unsigned)slot);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->chunk->global_myself_slot >= 0 && (int)slot == vm->chunk->global_myself_slot) {
                    push(vm, vmLoadThreadMyselfCopy(vm));
                    break;
                }

                Symbol* sym = vm->chunk->global_slots[slot].symbol;
                if (!sym || !sym->value) {
                    const char* name = vm->chunk->global_slot_names ? vm->chunk->global_slot_names[slot] : "?";
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Cheap first-letter test before the four compares: until
                // TextAttr is first touched (for most programs, never) they
                // ran on every global read.
                if (!gTextAttrInitialized && sym->name &&
                    ((sym->name[0] | 0x20) == 't' || (sym->name[0] | 0x20) == 'c') &&
                    (strcasecmp(sym->name, "CRT.TextAttr") == 0 ||
                     strcasecmp(sym->name, "TextAttr") == 0 ||
                     strcasecmp(sym->name, "crt.textattr") == 0 ||
                     strcasecmp(sym->name, "textattr") == 0)) {
                    gTextAttrInitialized = true;
                    SET_INT_VALUE(sym->value, 7);
                }

                push(vm, copyValueForStack(sym->value));
                break;
            }
            case SET_GSLOT: {
                uint16_t slot = READ_SHORT(vm);
                if (slot >= (uint16_t)vm->chunk->global_slot_count) {
                    runtimeError(vm, "VM Error: Global slot %u out of bounds for SET_GSLOT.", (unsigned)slot);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // VM 2.0 Phase 2b: const-ness is a bitmap check here, not a
                // separate table (vmConstGlobalSymbols is still populated
                // for slot-resolution purposes -- see bytecode_link.c -- but
                // is no longer what enforces immutability). Checked ahead of
                // the exception-unwind skip below since a write to a const
                // slot is a structural violation that should always surface
                // as an error, not be silently absorbed the way an ordinary
                // assignment is during unwind.
                if (vm->chunk->global_slot_is_const && vm->chunk->global_slot_is_const[slot]) {
                    Value skipped_value = pop(vm);
                    freeValue(&skipped_value);
                    const char* name = vm->chunk->global_slot_names ? vm->chunk->global_slot_names[slot] : "?";
                    runtimeError(vm, "Runtime Error: Cannot assign to const global '%s'.", name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                bool is_myself = (vm->chunk->global_myself_slot >= 0 && (int)slot == vm->chunk->global_myself_slot);
                bool is_pas_exc_global =
                    (vm->chunk->global_pas_exc_pending_slot >= 0 && (int)slot == vm->chunk->global_pas_exc_pending_slot) ||
                    (vm->chunk->global_pas_exc_message_slot >= 0 && (int)slot == vm->chunk->global_pas_exc_message_slot);

                if (vmPasExceptionPending(vm) && !is_pas_exc_global && !is_myself) {
                    Value skipped_value = pop(vm);
                    freeValue(&skipped_value);
                    break;
                }
                if (is_myself) {
                    Value value_from_stack = pop(vm);
                    vmStoreThreadMyself(vm, value_from_stack);
                    break;
                }

                pthread_mutex_lock(&globals_mutex);
                Symbol* sym = vm->chunk->global_slots[slot].symbol;
                if (!sym) {
                    pthread_mutex_unlock(&globals_mutex);
                    const char* name = vm->chunk->global_slot_names ? vm->chunk->global_slot_names[slot] : "?";
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        pthread_mutex_unlock(&globals_mutex);
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GSLOT.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }

                Value value_from_stack = pop(vm);
                updateSymbolDirect(sym, sym->name, value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                break;
            }
            case GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(&frame->slots[slot]));
                break;
            }
            case RESET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = makeNil();
                break;
            }
            case SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                Value value_from_stack = pop(vm);
                if (vmPasExceptionPending(vm)) {
                    freeValue(&value_from_stack);
                    break;
                }

                // --- START CORRECTED LOGIC ---
                if (VALUE_TYPE(*target_slot) == TYPE_POINTER && VALUE_TYPE(value_from_stack) == TYPE_NIL) {
                    // Assigning nil to a pointer variable preserves its base type and type
                    AS_POINTER(*target_slot) = NULL;
                    // type and base_type_node remain unchanged
                } else if (isPascalStringType(VALUE_TYPE(*target_slot)) && STRING_MAX_LENGTH(*target_slot) <= 0) {
                    VarType destType = VALUE_TYPE(*target_slot);
                    char encoded[5] = {0};
                    const char* source_str = NULL;

                    if (VALUE_TYPE(value_from_stack) == TYPE_CHAR) {
                        encoded[0] = (char)AS_CHAR(value_from_stack);
                        encoded[1] = '\0';
                        source_str = encoded;
                    } else if (VALUE_TYPE(value_from_stack) == TYPE_WIDECHAR) {
                        encodeUtf8Codepoint((uint32_t)AS_CHAR(value_from_stack), encoded);
                        source_str = encoded;
                    } else if (isPascalStringType(VALUE_TYPE(value_from_stack)) && AS_STRING(value_from_stack)) {
                        source_str = AS_STRING(value_from_stack);
                    }

                    if (!source_str) {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to string.",
                                     varTypeToString(VALUE_TYPE(value_from_stack)));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    freeValue(target_slot);
                    pscalStringEnsureObj(target_slot); // freeValue left target_slot's StringObj empty
                    AS_STRING(*target_slot) = strdup(source_str);
                    if (!AS_STRING(*target_slot)) {
                        runtimeError(vm, "VM Error: strdup failed for dynamic string assignment.");
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    {
                        // Re-tag after the retype below stomps .bits back to a
                        // nil-pointer placeholder -- reset_s is the real
                        // StringObj pscalStringEnsureObj already allocated.
                        StringObj *reset_s = PSCAL_VALUE_PTR(*target_slot, StringObj);
                        SET_VALUE_TYPE(target_slot, destType);
                        pscalValueSetHeapPtrBits(target_slot, reset_s);
                    }
                    STRING_MAX_LENGTH(*target_slot) = -1;
                } else if (VALUE_TYPE(*target_slot) == TYPE_STRING && STRING_MAX_LENGTH(*target_slot) > 0) {
                    // Special case: Assignment to a fixed-length string. The
                    // buffer is written in place, so unshare it first (see
                    // SET_INDIRECT's fixed-length branch).
                    valueEnsureUnique(target_slot);
                    const char* source_str = "";
                    char char_buf[2] = {0};

                    if (isPascalStringType(VALUE_TYPE(value_from_stack)) && AS_STRING(value_from_stack)) {
                        source_str = AS_STRING(value_from_stack);
                    } else if (VALUE_TYPE(value_from_stack) == TYPE_CHAR) {
                        char_buf[0] = AS_CHAR(value_from_stack);
                        source_str = char_buf;
                    }
                    
                    strncpy(AS_STRING(*target_slot), source_str, STRING_MAX_LENGTH(*target_slot));
                    AS_STRING(*target_slot)[STRING_MAX_LENGTH(*target_slot)] = '\0';

                } else if (isRealType(VALUE_TYPE(*target_slot))) {
                    if (isRealType(VALUE_TYPE(value_from_stack))) {
                        long double tmp = AS_REAL(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else if (isIntlikeType(VALUE_TYPE(value_from_stack))) {
                        long double tmp = asLd(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to real.", varTypeToString(VALUE_TYPE(value_from_stack)));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (isIntlikeType(VALUE_TYPE(*target_slot))) {
                    if (IS_NUMERIC(value_from_stack)) {
                        if (isRealType(VALUE_TYPE(value_from_stack))) {
                            assignRealToIntChecked(vm, target_slot, AS_REAL(value_from_stack));
                        } else {
                            long long tmp = asI64(value_from_stack);
                            if (VALUE_TYPE(*target_slot) == TYPE_BOOLEAN) tmp = (tmp != 0) ? 1 : 0;
                            SET_INT_VALUE(target_slot, tmp);
                            if (VALUE_TYPE(*target_slot) == TYPE_CHAR || VALUE_TYPE(*target_slot) == TYPE_WIDECHAR) {
                                SET_CHAR_VALUE(target_slot, (int)tmp);
                            }
                        }
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to integer.",
                                     varTypeToString(VALUE_TYPE(value_from_stack)));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (valueAssignAggregateInPlace(target_slot, &value_from_stack)) {
                    // A whole record or static array, copied into the slot's
                    // own storage so addresses taken into it stay valid (see
                    // SET_INDIRECT).
                } else {
                    // This is the logic for all other types, including dynamic strings,
                    // numbers, records, etc., which requires a deep copy.
                    // VM 2.0 Phase 4i: target_slot can genuinely be
                    // TYPE_POINTER here (unlike SET_INDIRECT's fallback
                    // branch above) -- guard since PTR_BASE_TYPE_NODE now
                    // requires it.
                    AST* preserved_base = (VALUE_TYPE(*target_slot) == TYPE_POINTER)
                                               ? PTR_BASE_TYPE_NODE(*target_slot)
                                               : NULL;
                    Value replacement = makeCopyOfValue(&value_from_stack);
                    replaceValueCell(target_slot, replacement, preserved_base);
                }
                // --- END CORRECTED LOGIC ---
                #ifdef DEBUG
                if (VALUE_TYPE(*target_slot) == TYPE_POINTER) {
                    fprintf(stderr,
                            "[DEBUG set_local] slot %u ptr=%p base=%p (%s) val=%p\n",
                            (unsigned)slot,
                            (void*)target_slot,
                            (void*)PTR_BASE_TYPE_NODE(*target_slot),
                            PTR_BASE_TYPE_NODE(*target_slot) ? astTypeToString(VALUE_TYPE(*PTR_BASE_TYPE_NODE(*target_slot))) : "NULL",
                            AS_POINTER(*target_slot));
                }
                #endif

                // Free the temporary value that was popped from the stack.
                freeValue(&value_from_stack);
                break;
            }
            case INC_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                if (!adjustLocalByDelta(vm, target_slot, 1, "INC_LOCAL")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case DEC_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                if (!adjustLocalByDelta(vm, target_slot, -1, "DEC_LOCAL")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(frame->upvalues[slot]));
                break;
            }
            case SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = frame->upvalues[slot];
                Value value_from_stack = pop(vm);
                if (vmPasExceptionPending(vm)) {
                    freeValue(&value_from_stack);
                    break;
                }

                if (VALUE_TYPE(*target_slot) == TYPE_POINTER && VALUE_TYPE(value_from_stack) == TYPE_NIL) {
                    // Preserve pointer metadata when assigning NIL.
                    AS_POINTER(*target_slot) = NULL;
                } else if (VALUE_TYPE(*target_slot) == TYPE_STRING && STRING_MAX_LENGTH(*target_slot) > 0) {
                    valueEnsureUnique(target_slot); // in-place write: unshare first
                    const char* source_str = "";
                    char char_buf[2] = {0};
                    if (isPascalStringType(VALUE_TYPE(value_from_stack)) && AS_STRING(value_from_stack)) {
                        source_str = AS_STRING(value_from_stack);
                    } else if (VALUE_TYPE(value_from_stack) == TYPE_CHAR) {
                        char_buf[0] = AS_CHAR(value_from_stack);
                        source_str = char_buf;
                    }
                    strncpy(AS_STRING(*target_slot), source_str, STRING_MAX_LENGTH(*target_slot));
                    AS_STRING(*target_slot)[STRING_MAX_LENGTH(*target_slot)] = '\0';
                } else if (isRealType(VALUE_TYPE(*target_slot))) {
                    if (IS_NUMERIC(value_from_stack)) {
                        long double tmp = asLd(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to real.", varTypeToString(VALUE_TYPE(value_from_stack)));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (isIntlikeType(VALUE_TYPE(*target_slot))) {
                    if (IS_NUMERIC(value_from_stack)) {
                        if (isRealType(VALUE_TYPE(value_from_stack))) {
                            assignRealToIntChecked(vm, target_slot, AS_REAL(value_from_stack));
                        } else {
                            long long tmp = asI64(value_from_stack);
                            if (VALUE_TYPE(*target_slot) == TYPE_BOOLEAN) tmp = (tmp != 0) ? 1 : 0;
                            SET_INT_VALUE(target_slot, tmp);
                            if (VALUE_TYPE(*target_slot) == TYPE_CHAR) SET_CHAR_VALUE(target_slot, (int)tmp);
                        }
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to integer.",
                                     varTypeToString(VALUE_TYPE(value_from_stack)));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (valueAssignAggregateInPlace(target_slot, &value_from_stack)) {
                    // See SET_LOCAL: a whole record or static array keeps its
                    // storage.
                } else {
                    // VM 2.0 Phase 4i: target_slot can genuinely be
                    // TYPE_POINTER here (unlike SET_INDIRECT's fallback
                    // branch above) -- guard since PTR_BASE_TYPE_NODE now
                    // requires it.
                    AST* preserved_base = (VALUE_TYPE(*target_slot) == TYPE_POINTER)
                                               ? PTR_BASE_TYPE_NODE(*target_slot)
                                               : NULL;
                    Value replacement = makeCopyOfValue(&value_from_stack);
                    replaceValueCell(target_slot, replacement, preserved_base);
                }
                freeValue(&value_from_stack);
                break;
            }
            case GET_UPVALUE_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(frame->upvalues[slot], NULL));
                break;
            }
            case INIT_LOCAL_ARRAY: {
                uint8_t slot = READ_BYTE();
                uint8_t dimension_count = READ_BYTE();
                uint16_t *lower_idx = NULL;
                uint16_t *upper_idx = NULL;
                if (dimension_count > 0) {
                    lower_idx = malloc(sizeof(uint16_t) * dimension_count);
                    upper_idx = malloc(sizeof(uint16_t) * dimension_count);
                    if (!lower_idx || !upper_idx) {
                        runtimeError(vm, "VM Error: Malloc failed for array bound indices.");
                        if (lower_idx) free(lower_idx);
                        if (upper_idx) free(upper_idx);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < dimension_count; i++) {
                        lower_idx[i] = READ_SHORT(vm);
                        upper_idx[i] = READ_SHORT(vm);
                    }
                }

                VarType elem_var_type = (VarType)READ_BYTE();
                uint16_t elem_name_idx = READ_SHORT(vm);
                if (elem_name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Array element type constant index out of range.");
                    free(lower_idx);
                    free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value elem_name_val = vm->chunk->constants[elem_name_idx];
                AST* elem_type_def = NULL;
                if (VALUE_TYPE(elem_name_val) == TYPE_STRING && AS_STRING(elem_name_val) && AS_STRING(elem_name_val)[0] != '\0') {
                    elem_type_def = lookupType(AS_STRING(elem_name_val));
                }

                Value array_value;
                if (dimension_count > 0) {
                    int* lower_bounds = malloc(sizeof(int) * dimension_count);
                    int* upper_bounds = malloc(sizeof(int) * dimension_count);
                    if (!lower_bounds || !upper_bounds) {
                        runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                        if (lower_bounds) free(lower_bounds);
                        if (upper_bounds) free(upper_bounds);
                        free(lower_idx);
                        free(upper_idx);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = dimension_count - 1; i >= 0; i--) {
                        if (lower_idx[i] == 0xFFFF && upper_idx[i] == 0xFFFF) {
                            Value size_val = pop(vm);
                            if (!isIntlikeType(VALUE_TYPE(size_val))) {
                                runtimeError(vm, "VM Error: Array size expression did not evaluate to an integer.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            long long size = asI64(size_val);
                            freeValue(&size_val);
                            if (size < 1 || !vmCheckArrayDimension(vm, 0, size - 1, &lower_bounds[i], &upper_bounds[i])) {
                                if (size < 1) {
                                    runtimeError(vm, "Runtime Error: Array size %lld must be at least 1.", size);
                                }
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                        } else {
                            if (lower_idx[i] >= vm->chunk->constants_count || upper_idx[i] >= vm->chunk->constants_count) {
                                runtimeError(vm, "VM Error: Array bound constant index out of range.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            Value lower_val = vm->chunk->constants[lower_idx[i]];
                            Value upper_val = vm->chunk->constants[upper_idx[i]];
                            if (!isIntlikeType(VALUE_TYPE(lower_val)) || !isIntlikeType(VALUE_TYPE(upper_val))) {
                                runtimeError(vm, "VM Error: Invalid constant types for array bounds.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            if (!vmCheckArrayDimension(vm, asI64(lower_val), asI64(upper_val),
                                                       &lower_bounds[i], &upper_bounds[i])) {
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                        }
                    }

                    array_value = makeArrayND(dimension_count, lower_bounds, upper_bounds, elem_var_type, elem_type_def);
                    free(lower_bounds);
                    free(upper_bounds);
                } else {
                    array_value = makeEmptyArray(elem_var_type, elem_type_def);
                }

                if (dimension_count > 0 && ARRAY_DIMENSIONS(array_value) == 0) {
                    runtimeError(vm, "VM Error: Failed to allocate array for local slot %u.", slot);
                    freeValue(&array_value);
                    free(lower_idx);
                    free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                free(lower_idx);
                free(upper_idx);

                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = array_value;
                break;
            }
            case INIT_FIELD_ARRAY: {
                uint8_t field_index = READ_BYTE();
                uint8_t dimension_count = READ_BYTE();
                uint16_t *lower_idx = NULL;
                uint16_t *upper_idx = NULL;
                if (dimension_count > 0) {
                    lower_idx = malloc(sizeof(uint16_t) * dimension_count);
                    upper_idx = malloc(sizeof(uint16_t) * dimension_count);
                    if (!lower_idx || !upper_idx) {
                        runtimeError(vm, "VM Error: Malloc failed for array bound indices.");
                        if (lower_idx) free(lower_idx);
                        if (upper_idx) free(upper_idx);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < dimension_count; i++) {
                        lower_idx[i] = READ_SHORT(vm);
                        upper_idx[i] = READ_SHORT(vm);
                    }
                }

                VarType elem_var_type = (VarType)READ_BYTE();
                uint16_t elem_name_idx = READ_SHORT(vm);
                if (elem_name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Array element type constant index out of range for INIT_FIELD_ARRAY.");
                    free(lower_idx);
                    free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value elem_name_val = vm->chunk->constants[elem_name_idx];
                AST* elem_type_def = NULL;
                if (VALUE_TYPE(elem_name_val) == TYPE_STRING && AS_STRING(elem_name_val) && AS_STRING(elem_name_val)[0] != '\0') {
                    elem_type_def = lookupType(AS_STRING(elem_name_val));
                }

                Value array_value;
                if (dimension_count > 0) {
                    int* lower_bounds = malloc(sizeof(int) * dimension_count);
                    int* upper_bounds = malloc(sizeof(int) * dimension_count);
                    if (!lower_bounds || !upper_bounds) {
                        runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                        if (lower_bounds) free(lower_bounds);
                        if (upper_bounds) free(upper_bounds);
                        free(lower_idx);
                        free(upper_idx);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = dimension_count - 1; i >= 0; i--) {
                        if (lower_idx[i] == 0xFFFF && upper_idx[i] == 0xFFFF) {
                            Value size_val = pop(vm);
                            if (!isIntlikeType(VALUE_TYPE(size_val))) {
                                runtimeError(vm, "VM Error: Array size expression did not evaluate to an integer.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            long long size = asI64(size_val);
                            freeValue(&size_val);
                            if (size < 1 || !vmCheckArrayDimension(vm, 0, size - 1, &lower_bounds[i], &upper_bounds[i])) {
                                if (size < 1) {
                                    runtimeError(vm, "Runtime Error: Array size %lld must be at least 1.", size);
                                }
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                        } else {
                            if (lower_idx[i] >= vm->chunk->constants_count || upper_idx[i] >= vm->chunk->constants_count) {
                                runtimeError(vm, "VM Error: Array bound constant index out of range.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            Value lower_val = vm->chunk->constants[lower_idx[i]];
                            Value upper_val = vm->chunk->constants[upper_idx[i]];
                            if (!isIntlikeType(VALUE_TYPE(lower_val)) || !isIntlikeType(VALUE_TYPE(upper_val))) {
                                runtimeError(vm, "VM Error: Invalid constant types for array bounds.");
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                            if (!vmCheckArrayDimension(vm, asI64(lower_val), asI64(upper_val),
                                                       &lower_bounds[i], &upper_bounds[i])) {
                                free(lower_bounds); free(upper_bounds);
                                free(lower_idx); free(upper_idx);
                                return INTERPRET_RUNTIME_ERROR;
                            }
                        }
                    }

                    array_value = makeArrayND(dimension_count, lower_bounds, upper_bounds, elem_var_type, elem_type_def);
                    free(lower_bounds);
                    free(upper_bounds);
                } else {
                    array_value = makeEmptyArray(elem_var_type, elem_type_def);
                }

                if (dimension_count > 0 && ARRAY_DIMENSIONS(array_value) == 0) {
                    runtimeError(vm, "VM Error: Failed to allocate array for field %u.", field_index);
                    freeValue(&array_value);
                    free(lower_idx);
                    free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                free(lower_idx);
                free(upper_idx);

                if (vm->stackTop == vm->stack) {
                    runtimeError(vm, "VM Error: Stack underflow (INIT_FIELD_ARRAY on empty stack).");
                    freeValue(&array_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    freeValue(&array_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL || VALUE_TYPE(*record_struct_ptr) != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer or non-record value.");
                    freeValue(&array_value);
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range for INIT_FIELD_ARRAY.");
                    freeValue(&array_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* fieldCell = fieldValueStorage(current);
                freeValue(fieldCell);
                *fieldCell = array_value;
                break;
            }
            case INIT_LOCAL_FILE: {
                uint8_t slot = READ_BYTE();
                VarType element_type = (VarType)READ_BYTE();
                uint16_t type_name_index = READ_SHORT(vm);
                AST* element_type_def = NULL;
                if (type_name_index != 0xFFFF && type_name_index < vm->chunk->constants_count) {
                    Value type_name_val = vm->chunk->constants[type_name_index];
                    if (VALUE_TYPE(type_name_val) == TYPE_STRING && AS_STRING(type_name_val) && AS_STRING(type_name_val)[0] != '\0') {
                        element_type_def = lookupType(AS_STRING(type_name_val));
                    }
                }

                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                Value file_val = makeValueForType(TYPE_FILE, NULL, NULL);
                if (element_type != TYPE_VOID && element_type != TYPE_UNKNOWN) {
                    FILE_ELEMENT_TYPE(file_val) = element_type;
                    FILE_ELEMENT_TYPE_DEF(file_val) = element_type_def;
                    long long bytes = 0;
                    if (vmSizeForVarType(element_type, &bytes) && bytes > 0 && bytes <= INT_MAX) {
                        FILE_RECORD_SIZE(file_val) = (int)bytes;
                        FILE_RECORD_SIZE_EXPLICIT(file_val) = true;
                    }
                }
                *target_slot = file_val;
                break;
            }
            case INIT_LOCAL_POINTER: {
                uint8_t slot = READ_BYTE();
                uint16_t type_name_idx = READ_SHORT(vm);
                AST* type_def = NULL;
                Value type_name_val = vm->chunk->constants[type_name_idx];
                if (VALUE_TYPE(type_name_val) == TYPE_STRING && AS_STRING(type_name_val) && AS_STRING(type_name_val)[0] != '\0') {
                    // Prefer a named type if available (e.g., pointer to record type)
                    AST* looked = lookupType(AS_STRING(type_name_val));
                    if (looked) {
                        type_def = looked;
                    } else {
                        // A builtin base type (`^integer`): one shared node per name.
                        type_def = vmBuiltinPointerBaseNode(AS_STRING(type_name_val));
                    }
                }
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                // Initialize pointer with a sensible base_type_node. If type_def is a pointer type
                // use its right child; for basic types, use the type_def directly.
                Value ptr = makeValueForType(TYPE_POINTER, NULL, NULL);
                if (type_def) {
                    AST* resolved = type_def;
                    if (resolved->type == AST_TYPE_REFERENCE && resolved->right) {
                        resolved = resolved->right;
                    }
                    if (resolved->type == AST_POINTER_TYPE && resolved->right) {
                        PTR_BASE_TYPE_NODE(ptr) = resolved->right;
                    } else if (resolved->type == AST_VARIABLE || resolved->type == AST_TYPE_IDENTIFIER) {
                        PTR_BASE_TYPE_NODE(ptr) = resolved;
                    } else {
                        PTR_BASE_TYPE_NODE(ptr) = resolved;
                    }
                }
                *target_slot = ptr;
                break;
            }
            case INIT_LOCAL_STRING: {
                uint8_t slot = READ_BYTE();
                uint8_t length = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                SET_VALUE_TYPE(target_slot, TYPE_STRING);
                pscalStringEnsureObj(target_slot); // freeValue left target_slot->s_val NULL
                STRING_MAX_LENGTH(*target_slot) = length;
                AS_STRING(*target_slot) = (char*)calloc(length + 1, 1);
                if (!AS_STRING(*target_slot)) {
                    runtimeError(vm, "VM Error: Malloc failed for fixed-length string initialization.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case PUSH_TYPE_DEFAULT: {
                VarType type = (VarType)READ_BYTE();
                uint16_t type_name_idx = READ_SHORT(vm);
                const Value* prototype = vmTypeDefaultPrototype(vm, type, type_name_idx);
                if (!prototype) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(prototype));
                break;
            }
            case JUMP_IF_FALSE: {
                uint32_t offset_val = READ_UINT32(vm);
                Value condition_value = pop(vm);
                bool condition_truth = false;
                bool value_valid = true;

                if (IS_BOOLEAN(condition_value)) {
                    condition_truth = AS_BOOLEAN(condition_value);
                } else if (IS_INTLIKE(condition_value)) {
                    condition_truth = AS_INTEGER(condition_value) != 0;
                } else if (IS_REAL(condition_value)) {
                    condition_truth = AS_REAL(condition_value) != 0.0;
                } else if (IS_CHAR(condition_value)) {
                    condition_truth = AS_CHAR(condition_value) != '\0';
                } else if (VALUE_TYPE(condition_value) == TYPE_NIL) {
                    condition_truth = false;
                } else if (VALUE_TYPE(condition_value) == TYPE_POINTER) {
                    // A pointer is truthy when non-nil, falsy when nil, so a
                    // safe-cast result (e.g. `iface is T`) works as a condition.
                    condition_truth = (AS_POINTER(condition_value) != NULL);
                } else {
                    value_valid = false;
                }

                if (!value_valid) {
                    runtimeError(vm, "VM Error: IF condition must be a Boolean or numeric value.");
                    freeValue(&condition_value);
                    return INTERPRET_RUNTIME_ERROR;
                }

                freeValue(&condition_value);

                if (!condition_truth) {
                    vm->ip += (int32_t)offset_val;
                }
                break;
            }
            case JUMP: {
                uint32_t offset = READ_UINT32(vm);
                vm->ip += (int32_t)offset;
                break;
            }
            case POP: {
                Value popped_val = pop(vm);
                freeValue(&popped_val);
                break;
            }
            case CALL_BUILTIN_PROC: {
                uint16_t builtin_id = READ_SHORT(vm);
                uint16_t name_const_idx = READ_SHORT(vm);
                uint8_t arg_count = READ_BYTE();

                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Error: Stack underflow for built-in arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* encoded_name = NULL;
                if (name_const_idx < vm->chunk->constants_count) {
                    Value* name_val = &vm->chunk->constants[name_const_idx];
                    if (VALUE_TYPE(*name_val) == TYPE_STRING && AS_STRING(*name_val)) {
                        encoded_name = AS_STRING(*name_val);
                    }
                }

                Value* args = vm->stackTop - arg_count;
                const char* builtin_name = getVmBuiltinNameById((int)builtin_id);
                VmBuiltinFn handler = getVmBuiltinHandlerById((int)builtin_id);
                const char* canonical_name = builtin_name;
                int resolved_id = -1;
                int effective_id = (int)builtin_id;
                VmBuiltinMapping mapping;
                bool have_mapping = false;

                /* The registry id baked into the bytecode is only trusted when
                 * it agrees with the builtin name compiled alongside it; a
                 * mismatch means the bytecode was produced against a different
                 * registry layout (stale cache, older binary), and dispatching
                 * by that id would run the WRONG builtin. The name is the
                 * stable contract — re-resolve by it. */
                bool id_name_mismatch = handler && builtin_name &&
                                        encoded_name && *encoded_name &&
                                        strcasecmp(builtin_name, encoded_name) != 0;
                if ((!handler || !canonical_name || id_name_mismatch) &&
                    encoded_name && *encoded_name) {
                    have_mapping = getVmBuiltinMapping(encoded_name, &mapping, &resolved_id);
                    if (id_name_mismatch && !have_mapping) {
                        /* Name unknown to this binary: the id cannot be right
                         * either (it names something else), so fail loudly
                         * rather than silently running a different builtin. */
                        handler = NULL;
                        canonical_name = encoded_name;
                    }
                } else if (!handler && canonical_name) {
                    have_mapping = getVmBuiltinMapping(canonical_name, &mapping, &resolved_id);
                }

                if (have_mapping) {
                    handler = mapping.handler;
                    canonical_name = mapping.name;
                    if (resolved_id >= 0) {
                        effective_id = resolved_id;
                    }
                }

                const char* effective_name = canonical_name;
                if (!effective_name && encoded_name && *encoded_name) {
                    effective_name = encoded_name;
                }

                if (!handler) {
                    if (effective_name && *effective_name) {
                        runtimeError(vm, "VM Error: Unimplemented or unknown built-in '%s' (id %u) called.",
                                     effective_name, builtin_id);
                    } else {
                        runtimeError(vm, "VM Error: Unknown built-in id %u called.", builtin_id);
                    }
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        freeValue(&args[i]);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (gVmBuiltinProfileEnabled && effective_id >= 0 && effective_id <= UINT16_MAX) {
                    gVmBuiltinCallCounts[effective_id]++;
                }

                bool needs_lock = vmBuiltinNeedsGlobalLockCached(effective_id, effective_name);
                if (needs_lock) pthread_mutex_lock(&globals_mutex);

                const char* context_name = encoded_name && *encoded_name ? encoded_name : effective_name;
                if (!context_name) {
                    context_name = builtin_name;
                }
                const char* previous_builtin_name = vm->current_builtin_name;
                vm->current_builtin_name = context_name;

                Value result;
                bool fx_replayed = false;
                EffectMask fx_mask = FX_PURE;
                InterpretResult fx_status = vmApplyFxPolicy(vm, effective_id, effective_name, arg_count, args,
                                                            &result, &fx_replayed, &fx_mask);
                if (fx_status != INTERPRET_OK) {
                    if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                    vm->current_builtin_name = previous_builtin_name;
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        freeValue(&args[i]);
                    }
                    return fx_status;
                }
                if (!fx_replayed) {
                    result = handler(vm, arg_count, args);
                    if (fx_mask != FX_PURE && pscalFxRecordActive()) {
                        pscalFxRecordCall(effective_name, arg_count, args, &result);
                    }
                }

                if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                vm->current_builtin_name = previous_builtin_name;

                vm->stackTop -= arg_count;
                for (int i = 0; i < arg_count; i++) {
                    freeValue(&args[i]);
                }

                if (vm->abort_requested) {
                    freeValue(&result);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&result);

                if (vm->exit_requested) {
                    bool suspend_unwind = vm->suspend_unwind_requested && !vm->abort_requested;
                    bool halted = false;
                    InterpretResult res = returnFromCall(vm, &halted);
                    if (res != INTERPRET_OK) return res;
                    if (halted) {
                        vm->exit_requested = false;
                        vm->suspend_unwind_requested = false;
                        return INTERPRET_OK;
                    }
                    vm->exit_requested = suspend_unwind;
                }
                break;
            }
            case CALL_BUILTIN: {
                uint16_t name_const_idx = READ_SHORT(vm);
                uint8_t arg_count = READ_BYTE();

                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Stack underflow for built-in arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* args = vm->stackTop - arg_count;
                if (!vm->chunk || name_const_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Invalid built-in name index %u.", name_const_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_const_idx];
                const char* builtin_name_original_case = NULL;
                if (VALUE_TYPE(*name_val) == TYPE_STRING) {
                    builtin_name_original_case = AS_STRING(*name_val);
                }

                const char* builtin_name_lower = NULL;
                int lower_idx = getBuiltinLowercaseIndex(vm->chunk, (int)name_const_idx);
                if (lower_idx >= 0 && lower_idx < vm->chunk->constants_count) {
                    Value* lower_val = &vm->chunk->constants[lower_idx];
                    if (VALUE_TYPE(*lower_val) == TYPE_STRING) {
                        builtin_name_lower = AS_STRING(*lower_val);
                    }
                }

                int mapped_builtin_id = -1;
                VmBuiltinFn handler = NULL;
                const char* canonical_name = NULL;
                bool have_mapping = false;

                if (vm->chunk && vm->chunk->builtin_resolved_ids &&
                    name_const_idx < vm->chunk->constants_count) {
                    mapped_builtin_id = atomic_load(&vm->chunk->builtin_resolved_ids[name_const_idx]);
                    if (mapped_builtin_id >= 0) {
                        handler = getVmBuiltinHandlerById(mapped_builtin_id);
                        canonical_name = getVmBuiltinNameById(mapped_builtin_id);
                        have_mapping = handler != NULL;
                    } else if (mapped_builtin_id == -1) {
                        have_mapping = false;
                    } else {
                        VmBuiltinMapping mapping;
                        if (builtin_name_lower && builtin_name_lower[0]) {
                            have_mapping = getVmBuiltinMappingCanonical(builtin_name_lower, &mapping, &mapped_builtin_id);
                        }
                        if (!have_mapping) {
                            have_mapping = getVmBuiltinMapping(builtin_name_original_case, &mapping, &mapped_builtin_id);
                        }
                        if (have_mapping) {
                            handler = mapping.handler;
                            canonical_name = mapping.name;
                        } else {
                            mapped_builtin_id = -1;
                        }
                        atomic_store(&vm->chunk->builtin_resolved_ids[name_const_idx], mapped_builtin_id);
                    }
                } else {
                    VmBuiltinMapping mapping;
                    if (builtin_name_lower && builtin_name_lower[0]) {
                        have_mapping = getVmBuiltinMappingCanonical(builtin_name_lower, &mapping, &mapped_builtin_id);
                    }
                    if (!have_mapping) {
                        have_mapping = getVmBuiltinMapping(builtin_name_original_case, &mapping, &mapped_builtin_id);
                    }
                    if (have_mapping) {
                        handler = mapping.handler;
                        canonical_name = mapping.name;
                    }
                }

                if (handler) {
                    bool needs_lock = vmBuiltinNeedsGlobalLockCached(mapped_builtin_id,
                        canonical_name ? canonical_name : builtin_name_original_case);
                    if (needs_lock) pthread_mutex_lock(&globals_mutex);

                    const char* context_name = builtin_name_original_case && *builtin_name_original_case
                        ? builtin_name_original_case
                        : canonical_name;
                    const char* previous_builtin_name = vm->current_builtin_name;
                    vm->current_builtin_name = context_name;

                    Value result;
                    bool fx_replayed = false;
                    EffectMask fx_mask = FX_PURE;
                    InterpretResult fx_status = vmApplyFxPolicy(vm, mapped_builtin_id, context_name, arg_count, args,
                                                                &result, &fx_replayed, &fx_mask);
                    if (fx_status != INTERPRET_OK) {
                        if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                        vm->current_builtin_name = previous_builtin_name;
                        vm->stackTop -= arg_count;
                        for (int i = 0; i < arg_count; i++) {
                            freeValue(&args[i]);
                        }
                        return fx_status;
                    }
                    if (!fx_replayed) {
                        result = handler(vm, arg_count, args);
                        if (fx_mask != FX_PURE && pscalFxRecordActive()) {
                            pscalFxRecordCall(context_name, arg_count, args, &result);
                        }
                    }

                    if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                    vm->current_builtin_name = previous_builtin_name;

                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        freeValue(&args[i]);
                    }

                    if (vm->abort_requested) {
                        freeValue(&result);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    const char* type_name = canonical_name ? canonical_name : context_name;
                    if (vmBuiltinTypeCached(mapped_builtin_id, type_name) == BUILTIN_TYPE_FUNCTION) {
                        push(vm, result);
                    } else {
                        freeValue(&result);
                    }
                } else {
                    runtimeError(vm, "VM Error: Unimplemented or unknown built-in '%s' called.", builtin_name_original_case);
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        freeValue(&args[i]);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->exit_requested) {
                    bool suspend_unwind = vm->suspend_unwind_requested && !vm->abort_requested;
                    bool halted = false;
                    InterpretResult res = returnFromCall(vm, &halted);
                    if (res != INTERPRET_OK) return res;
                    if (halted) {
                        vm->exit_requested = false;
                        vm->suspend_unwind_requested = false;
                        return INTERPRET_OK;
                    }
                    vm->exit_requested = suspend_unwind;
                }
                break;
            }
            case CALL_USER_PROC: {
                if (!vmEnsureFrameCapacity(vm)) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                size_t call_site = (size_t)(vm->lastInstruction - vm->chunk->code);
                uint16_t name_index = READ_SHORT(vm);
                uint8_t declared_arity = READ_BYTE();

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %u, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* proc_symbol = vmUserProcSiteLookup(vm, call_site);
                const char* proc_name = proc_symbol ? proc_symbol->name : NULL;
                if (!proc_symbol) {
                    if (!vm->chunk || name_index >= vm->chunk->constants_count) {
                        runtimeError(vm, "VM Error: Invalid procedure name index %u for CALL_USER_PROC.", name_index);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    Value* name_val = &vm->chunk->constants[name_index];
                    if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                        runtimeError(vm, "VM Error: CALL_USER_PROC requires string constant for callee name (index %u).", name_index);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    proc_name = AS_STRING(*name_val);
                    char lookup_name[MAX_SYMBOL_LENGTH + 1];
                    strncpy(lookup_name, proc_name, MAX_SYMBOL_LENGTH);
                    lookup_name[MAX_SYMBOL_LENGTH] = '\0';
                    toLowerString(lookup_name);

                    if (!vm->procedureTable) {
                        runtimeError(vm, "VM Error: Procedure table not initialized when calling '%s'.", proc_name);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    proc_symbol = vmResolveUserProc(vm, lookup_name);
                    if (!proc_symbol) {
                        runtimeError(vm, "VM Error: Procedure '%s' not found for CALL_USER_PROC.", proc_name);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (!proc_symbol->is_defined || proc_symbol->bytecode_address < 0) {
                        runtimeError(vm, "VM Error: Procedure '%s' has no compiled body.",
                                     proc_symbol->name ? proc_symbol->name : proc_name);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vmUserProcSiteStore(vm, call_site, proc_symbol);
                }
                if (!vmCheckCallArity(vm, proc_symbol, declared_arity)) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                size_t target_address = (size_t)proc_symbol->bytecode_address;
                if (!vm->chunk || target_address >= (size_t)vm->chunk->count) {
                    runtimeError(vm, "VM Error: Procedure '%s' bytecode address %d out of range.",
                                 proc_symbol->name ? proc_symbol->name : proc_name,
                                 proc_symbol->bytecode_address);
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        } else if (isIntlikeType(param_ast->var_type) && isRealType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            assignRealToIntChecked(vm, arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->owns_upvalues = false;
                frame->closureEnv = NULL;
                frame->discard_result_on_return = false;
                frame->vtable = NULL;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    frame->owns_upvalues = true;
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        if (frame->owns_upvalues && frame->upvalues) {
                            free(frame->upvalues);
                            frame->upvalues = NULL;
                            frame->owns_upvalues = false;
                        }
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL: {
                if (!vmEnsureFrameCapacity(vm)) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Operands: name_idx (2 bytes), target_address (4 bytes), declared_arity (1 byte)
                uint16_t name_idx_ignored = READ_SHORT(vm); // Read and discard the name index
                (void)name_idx_ignored; // Suppress unused variable warning
                uint32_t target_address = READ_UINT32(vm);
                uint8_t declared_arity = READ_BYTE();

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                Symbol* proc_symbol = vmGetProcedureByAddress(vm, target_address);
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: Could not retrieve procedure symbol for called address %04u.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!vmCheckCallArity(vm, proc_symbol, declared_arity)) {
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        } else if (isIntlikeType(param_ast->var_type) && isRealType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            assignRealToIntChecked(vm, arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->owns_upvalues = false;
                frame->closureEnv = NULL;
                frame->discard_result_on_return = false;
                frame->vtable = NULL;
                frame->vtable = NULL;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        if (frame->owns_upvalues && frame->upvalues) {
                            free(frame->upvalues);
                            frame->upvalues = NULL;
                            frame->owns_upvalues = false;
                        }
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL_INDIRECT: {
                uint8_t declared_arity = READ_BYTE();
                // Stack layout expected: [... args] [addr]
                Value addrVal = pop(vm);
                ClosureEnvPayload* captured_env = NULL;
                Symbol* proc_symbol = NULL;
                uint32_t target_address = 0;

                if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
                    target_address = (uint32_t)AS_CLOSURE(addrVal).entry_offset;
                    captured_env = AS_CLOSURE(addrVal).env;
                    if (captured_env) {
                        retainClosureEnv(captured_env);
                    }
                    proc_symbol = AS_CLOSURE(addrVal).symbol;
                } else if (IS_INTLIKE(addrVal)) {
                    target_address = (uint32_t)AS_INTEGER(addrVal);
                } else {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires procedure pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&addrVal);

                if (!vmEnsureFrameCapacity(vm)) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (vm->stackTop - vm->stack < declared_arity) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: Stack underflow for indirect call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                if (!proc_symbol) {
                    proc_symbol = vmGetProcedureByAddress(vm, target_address);
                }
                if (!proc_symbol) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: No procedure found at address %04u for indirect call.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!vmCheckCallArity(vm, proc_symbol, declared_arity)) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Coerce numeric argument types to match formal parameter real/integer expectations
                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        } else if (isIntlikeType(param_ast->var_type) && isRealType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            assignRealToIntChecked(vm, arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->owns_upvalues = false;
                frame->closureEnv = NULL;
                frame->discard_result_on_return = false;

                if (captured_env) {
                    if (captured_env->slot_count != proc_symbol->upvalue_count) {
                        releaseClosureEnv(captured_env);
                        runtimeError(vm, "VM Error: Closure environment mismatch for '%s'.",
                                     proc_symbol->name ? proc_symbol->name : "<anonymous>");
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    frame->closureEnv = captured_env;
                    frame->upvalues = captured_env->slots;
                } else if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    frame->owns_upvalues = true;
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        if (frame->owns_upvalues && frame->upvalues) {
                            free(frame->upvalues);
                            frame->upvalues = NULL;
                            frame->owns_upvalues = false;
                        }
                        if (captured_env) {
                            releaseClosureEnv(captured_env);
                        }
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                } else if (captured_env) {
                    releaseClosureEnv(captured_env);
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL_METHOD: {
                uint8_t method_index = READ_BYTE();
                uint8_t declared_arity = READ_BYTE();
                if (vm->stackTop - vm->stack < declared_arity + 1) {
                    runtimeError(vm, "VM Error: Stack underflow for method call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value receiverVal = vm->stackTop[-declared_arity - 1];
                if (VALUE_TYPE(receiverVal) != TYPE_POINTER || AS_POINTER(receiverVal) == NULL) {
                    runtimeError(vm, "VM Error: Method call receiver must be an object pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* objVal = AS_POINTER(receiverVal);
                if (VALUE_TYPE(*objVal) != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Method call receiver must be an object record.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = AS_RECORD(*objVal);
                const Value* vtable_val = NULL;
                while (current) {
                    // ALLOC_OBJECT's fields have no name.
                    if (current->name && strcmp(current->name, "__vtable") == 0) {
                        const Value *fieldValue = fieldValueStorageConst(current);
                        if (VALUE_TYPE(*fieldValue) == TYPE_ARRAY && !ARRAY_IS_PACKED(*fieldValue) &&
                            AS_ARRAY(*fieldValue)) {
                            vtable_val = fieldValue;
                        }
                        break;
                    }
                    current = current->next;
                }

                if (!vtable_val) {
                    runtimeError(vm, "VM Error: Object missing V-table.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* vtable_arr = AS_ARRAY(*vtable_val);
                int vtable_len = calculateArrayTotalSize(vtable_val);
                if (method_index >= vtable_len) {
                    runtimeError(vm, "VM Error: Method index %d is outside the %d-entry V-table.",
                                 method_index, vtable_len);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // The V-table is data the program can change, so the
                // verifier never saw this target. Like CALL and
                // CALL_INDIRECT, only go there if it is a procedure's entry.
                Value vtable_slot = vtable_arr[method_index];
                long long slot_address = IS_INTLIKE(vtable_slot) ? AS_INTEGER(vtable_slot) : -1;
                if (slot_address < 0 || slot_address >= vm->chunk->count) {
                    runtimeError(vm, "VM Error: V-table entry %d is not a code address.", method_index);
                    return INTERPRET_RUNTIME_ERROR;
                }
                uint32_t target_address = (uint32_t)slot_address;

                Symbol* method_symbol = NULL;
                const char* className = NULL;
                // VM 2.0 Phase 4i: objVal is the DEREFERENCED record (see
                // AS_POINTER(receiverVal) above, confirmed TYPE_RECORD by
                // the check preceding this block), never TYPE_POINTER --
                // PTR_BASE_TYPE_NODE now requires TYPE_POINTER, and a
                // dedicated audit confirmed records never populate
                // base_type_node in practice, so this guard preserves the
                // pre-4i always-NULL-className behavior of this path
                // exactly (method dispatch here relies on the vtable
                // lookup above, not this className fallback).
                if (VALUE_TYPE(*objVal) == TYPE_POINTER && PTR_BASE_TYPE_NODE(*objVal) && PTR_BASE_TYPE_NODE(*objVal)->token) {
                    className = PTR_BASE_TYPE_NODE(*objVal)->token->value;
                }
                if (className) {
                    method_symbol = vmFindClassMethod(vm, className, method_index);
                }
                if (!method_symbol) {
                    method_symbol = vmGetProcedureByAddress(vm, target_address);
                }
                // A method found by class name must be the one the V-table
                // points at, since control goes to target_address.
                if (!method_symbol || !method_symbol->is_defined ||
                    method_symbol->bytecode_address < 0 ||
                    (uint32_t)method_symbol->bytecode_address != target_address) {
                    runtimeError(vm, "VM Error: V-table entry %d (address %u) is not a procedure entry.",
                                 method_index, target_address);
                    return INTERPRET_RUNTIME_ERROR;
                }
                // The receiver is the method's first parameter.
                if (!vmCheckCallArity(vm, method_symbol, declared_arity + 1)) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!vmEnsureFrameCapacity(vm)) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity - 1;
                frame->vtable = vtable_arr;
                frame->slotCount = 0;

                if (method_symbol->type_def && method_symbol->type_def->child_count >= declared_arity + 1) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = method_symbol->type_def->children[i + 1];
                        Value* arg_val = frame->slots + 1 + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        } else if (isIntlikeType(param_ast->var_type) && isRealType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            assignRealToIntChecked(vm, arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = method_symbol;
                frame->locals_count = method_symbol->locals_count;
                frame->upvalue_count = method_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->owns_upvalues = false;
                frame->closureEnv = NULL;
                frame->discard_result_on_return = false;

                if (method_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * method_symbol->upvalue_count);
                    frame->owns_upvalues = true;
                    CallFrame* parent_frame = NULL;
                    if (method_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == method_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }
                    if (!parent_frame) {
                        free(frame->upvalues);
                        frame->upvalues = NULL;
                        frame->owns_upvalues = false;
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", method_symbol->name);
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    for (int i = 0; i < method_symbol->upvalue_count; i++) {
                        if (method_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + method_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[method_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < method_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + 1 + method_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case PROC_CALL_INDIRECT: {
                uint8_t declared_arity = READ_BYTE();
                // Reuse CALL_INDIRECT machinery by rewinding ip to interpret the common path,
                // but we need to know when to discard a return value. Implement inline duplication instead.

                Value addrVal = pop(vm);
                ClosureEnvPayload* captured_env = NULL;
                Symbol* proc_symbol = NULL;
                uint32_t target_address = 0;

                if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
                    target_address = (uint32_t)AS_CLOSURE(addrVal).entry_offset;
                    captured_env = AS_CLOSURE(addrVal).env;
                    if (captured_env) {
                        retainClosureEnv(captured_env);
                    }
                    proc_symbol = AS_CLOSURE(addrVal).symbol;
                } else if (IS_INTLIKE(addrVal)) {
                    target_address = (uint32_t)AS_INTEGER(addrVal);
                } else {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires procedure pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&addrVal);

                if (!vmEnsureFrameCapacity(vm)) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->stackTop - vm->stack < declared_arity) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: Stack underflow for indirect call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                if (!proc_symbol) {
                    proc_symbol = vmGetProcedureByAddress(vm, target_address);
                }
                if (!proc_symbol) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    runtimeError(vm, "VM Error: No procedure found at address %04u for indirect call.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!vmCheckCallArity(vm, proc_symbol, declared_arity)) {
                    if (captured_env) {
                        releaseClosureEnv(captured_env);
                    }
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        } else if (isIntlikeType(param_ast->var_type) && isRealType(VALUE_TYPE(*arg_val))) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            assignRealToIntChecked(vm, arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->owns_upvalues = false;
                frame->closureEnv = NULL;
                frame->discard_result_on_return = true;
                frame->vtable = NULL;

                if (captured_env) {
                    if (captured_env->slot_count != proc_symbol->upvalue_count) {
                        releaseClosureEnv(captured_env);
                        runtimeError(vm, "VM Error: Closure environment mismatch for '%s'.",
                                     proc_symbol->name ? proc_symbol->name : "<anonymous>");
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    frame->closureEnv = captured_env;
                    frame->upvalues = captured_env->slots;
                } else if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    frame->owns_upvalues = true;
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }
                    if (!parent_frame) {
                        if (frame->owns_upvalues && frame->upvalues) {
                            free(frame->upvalues);
                            frame->upvalues = NULL;
                            frame->owns_upvalues = false;
                        }
                        if (captured_env) {
                            releaseClosureEnv(captured_env);
                        }
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        vm->frameCount--;
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                } else if (captured_env) {
                    releaseClosureEnv(captured_env);
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;

                // After the callee returns, if it is a function, its result will be on the stack.
                // Since this opcode is for statement context, discard it to keep the stack balanced.
                // This block will run when the frame unwinds back here.
                // Note: actual popping occurs after the callee returns to this frame.
                // The main interpreter loop continues; no action needed here now.
                break;
            }

            case HALT:
                return INTERPRET_OK;
            case CALL_HOST: {
                HostFunctionID host_id = READ_HOST_ID();
                if (host_id >= HOST_FN_COUNT || vm->host_functions[host_id] == NULL) {
                    runtimeError(vm, "Invalid host function ID %d or function not registered.", host_id);
                    return INTERPRET_RUNTIME_ERROR;
                }
                HostFn func = vm->host_functions[host_id];
                // Do not hold globals_mutex around host calls that may block (e.g., thread waits),
                // or that start threads which immediately need to access globals during VM init.
                // Individual host functions should lock as needed.
                Value result = func(vm);
                if (vm->abort_requested) {
                    freeValue(&result);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result);
                break;
            }
            case THREAD_CREATE: {
                uint32_t entry = READ_UINT32(vm);
                int id = createThread(vm, entry);
                if (id < 0) {
                    if (!vm->abort_requested) {
                        runtimeError(vm, "Thread limit exceeded.");
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case THREAD_JOIN: {
                Value tidVal = peek(vm, 0);
                int tid_ok = 0;
                int tid = 0;
                if (VALUE_TYPE(tidVal) == TYPE_THREAD) {
                    tid = (int)AS_INTEGER(tidVal);
                    tid_ok = 1;
                } else if (IS_INTLIKE(tidVal)) {
                    tid = (int)AS_INTEGER(tidVal);
                    tid_ok = 1;
                }
                if (!tid_ok) {
                    runtimeError(vm, "Thread id must be integer.");
                    Value popped_tid = pop(vm);
                    freeValue(&popped_tid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                bool joined_ok = true;
                bool have_status = vmThreadTakeResult(vm, tid, NULL, true, &joined_ok, true);
                if (!have_status) {
                    joinThreadInternal(vm, tid);
                }
                Value popped_tid = pop(vm);
                freeValue(&popped_tid);
                // Aether lowers `par` to spawn + join and has no way to ask a
                // thread's status, so a branch that failed (its runtime error
                // already printed) must fail the program here, as the same
                // call would have run sequentially. Other front ends keep
                // join's discard-the-status contract and ask ThreadGetStatus.
                if (have_status && !joined_ok && frontendIsAether()) {
                    vm->abort_requested = true;
                    vm->runtime_error_raised = true;
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case MUTEX_CREATE: {
                int id = createMutex(vm, false);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case RCMUTEX_CREATE: {
                int id = createMutex(vm, true);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case MUTEX_LOCK: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)VAL_INT(midVal);
                if (!lockMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case MUTEX_UNLOCK: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)VAL_INT(midVal);
                if (!unlockMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case MUTEX_DESTROY: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)VAL_INT(midVal);
                if (!destroyMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case FORMAT_VALUE: {
                uint8_t width = READ_BYTE();
                uint8_t precision_raw = READ_BYTE();
                int precision = (precision_raw == 0xFF) ? -1 : precision_raw;

                Value raw_val = pop(vm);

                // Sized for the whole field: `1e300:1:2` is over 300
                // characters, and a string may be any length.
                char* buf = NULL;

                if (isRealType(VALUE_TYPE(raw_val))) {
                    long double rv = AS_REAL(raw_val);
                    int width_int = width;
                    if (precision >= 0) {
                        buf = vmFormatAlloc("%*.*Lf", width_int, precision, rv);
                    } else {
                        buf = vmFormatAlloc("%*.*LE", width_int,
                                 (int)PASCAL_DEFAULT_FLOAT_PRECISION, rv);
                    }
                } else if (VALUE_TYPE(raw_val) == TYPE_CHAR) {
                    buf = vmFormatAlloc("%*c", width, AS_CHAR(raw_val));
                } else if (VALUE_TYPE(raw_val) == TYPE_BOOLEAN) {
                    const char* bool_str = VAL_INT(raw_val) ? "TRUE" : "FALSE";
                    buf = vmFormatAlloc("%*s", width, bool_str);
                } else if (isIntlikeType(VALUE_TYPE(raw_val))) {
                    if (VALUE_TYPE(raw_val) == TYPE_UINT64 || VALUE_TYPE(raw_val) == TYPE_UINT32 ||
                        VALUE_TYPE(raw_val) == TYPE_UINT16 || VALUE_TYPE(raw_val) == TYPE_UINT8 ||
                        VALUE_TYPE(raw_val) == TYPE_WORD   || VALUE_TYPE(raw_val) == TYPE_BYTE) {
                        unsigned long long u = VAL_UINT(raw_val);
                        if (VALUE_TYPE(raw_val) == TYPE_BYTE || VALUE_TYPE(raw_val) == TYPE_UINT8)   u &= 0xFFULL;
                        if (VALUE_TYPE(raw_val) == TYPE_WORD || VALUE_TYPE(raw_val) == TYPE_UINT16) u &= 0xFFFFULL;
                        if (VALUE_TYPE(raw_val) == TYPE_UINT32) u &= 0xFFFFFFFFULL;
                        buf = vmFormatAlloc("%*llu", width, u);
                    } else {
                        long long s = VAL_INT(raw_val);
                        if (VALUE_TYPE(raw_val) == TYPE_INT8)  s = (int8_t)s;
                        if (VALUE_TYPE(raw_val) == TYPE_INT16) s = (int16_t)s;
                        buf = vmFormatAlloc("%*lld", width, s);
                    }
                } else if (VALUE_TYPE(raw_val) == TYPE_STRING) {
                    const char* source_str = AS_STRING(raw_val) ? AS_STRING(raw_val) : "";
                    size_t len = strlen(source_str);
                    int prec = (width > 0 && (size_t)width < len) ? width : (int)len;
                    buf = vmFormatAlloc("%*.*s", width, prec, source_str);
                } else {
                    buf = vmFormatAlloc("%*s", width, "?");
                }

                freeValue(&raw_val);
                if (!buf) {
                    runtimeError(vm, "VM Error: Out of memory formatting a value for output.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makeOwnedString(buf, strlen(buf)));
                break;
            }

            case OPCODE_COUNT:
            default:
                runtimeError(vm, "VM Error: Unknown opcode %d.", instruction_val);
                return INTERPRET_RUNTIME_ERROR;
        }
        next_instruction:;
    }
    return INTERPRET_OK;
}
__attribute__((weak)) const volatile bool *shellRuntimePendingExitFlag(void) {
    return NULL;
}

__attribute__((weak)) bool shellRuntimeShouldDeferExit(VM* vm) {
    (void)vm;
    return false;
}

__attribute__((weak)) bool shellRuntimeMaybeRequestPendingExit(VM* vm) {
    (void)vm;
    return false;
}
bool vmOpcodeProfileIsEnabled(void) {
    pthread_once(&gVmOpcodeProfileOnce, vmOpcodeProfileInitOnce);
    return gVmOpcodeProfileEnabled;
}

static void vmShellBuiltinProfileIncrement(const char *name) {
    if (!name || !*name) {
        return;
    }
    for (size_t i = 0; i < gVmShellBuiltinProfileCount; ++i) {
        if (strcmp(gVmShellBuiltinProfiles[i].name, name) == 0) {
            gVmShellBuiltinProfiles[i].count++;
            return;
        }
    }
    if (gVmShellBuiltinProfileCount == gVmShellBuiltinProfileCapacity) {
        size_t new_cap = gVmShellBuiltinProfileCapacity ? gVmShellBuiltinProfileCapacity * 2 : 16;
        VmShellBuiltinProfileEntry *resized = realloc(gVmShellBuiltinProfiles, new_cap * sizeof(*resized));
        if (!resized) {
            return;
        }
        gVmShellBuiltinProfiles = resized;
        gVmShellBuiltinProfileCapacity = new_cap;
    }
    char *copy = strdup(name);
    if (!copy) {
        return;
    }
    gVmShellBuiltinProfiles[gVmShellBuiltinProfileCount].name = copy;
    gVmShellBuiltinProfiles[gVmShellBuiltinProfileCount].count = 1;
    gVmShellBuiltinProfileCount++;
}

void vmProfileShellBuiltin(const char *name) {
    pthread_once(&gVmOpcodeProfileOnce, vmOpcodeProfileInitOnce);
    if (!gVmOpcodeProfileEnabled) {
        return;
    }
    vmShellBuiltinProfileIncrement(name);
}
