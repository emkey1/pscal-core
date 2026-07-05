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
#include "backend_ast/builtin.h"
#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"
#endif
#if defined(__APPLE__)
#include <dlfcn.h>
#endif
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
static pthread_mutex_t value_cell_mutex = PTHREAD_MUTEX_INITIALIZER;
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

static bool vmProcRegistryIsEmpty(void) {
    bool empty = false;
    pthread_mutex_lock(&gVmProcRegistryLock);
    empty = (gVmProcRegistryCount == 0);
    pthread_mutex_unlock(&gVmProcRegistryLock);
    return empty;
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
    out->shell_indexing = vm->shellIndexing;
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

// Resolve a value to its underlying record by chasing pointer chains.
// Returns NULL if a nil pointer is encountered.  If the original value is
// neither a pointer nor a record, *invalid_type is set to true.
static void push(VM* vm, Value value);
static Value copyValueForStack(const Value* src);

static Value* resolveRecord(Value* base, bool* invalid_type) {
    if (invalid_type) *invalid_type = false;
    if (VALUE_TYPE(*base) != TYPE_POINTER && VALUE_TYPE(*base) != TYPE_RECORD) {
        if (invalid_type) *invalid_type = true;
        return NULL;
    }
    Value* current = base;
    while (current && VALUE_TYPE(*current) == TYPE_POINTER) {
        current = AS_POINTER(*current);
    }
    return current;
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
        runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
        return NULL;
    }
    return record_struct_ptr;
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

    type_node = PTR_BASE_TYPE_NODE(*base_val_ptr);
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

    freeFieldValue(prototype_fields);
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
        if (fieldStorage && VALUE_TYPE(*fieldStorage) == TYPE_ARRAY && ARRAY_IS_DYNAMIC(*fieldStorage)) {
            push(vm, copyDynamicArraySnapshotValue(fieldStorage));
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
    bool is_shell_frontend = vm ? vm->shellIndexing : frontendIsShell();

    if (!is_shell_frontend)
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

static Value makeOwnedString(char* data, size_t len) {
    Value v;
    memset(&v, 0, sizeof(Value));
    SET_VALUE_TYPE(&v, TYPE_STRING);
    AS_STRING(v) = data;
    STRING_MAX_LENGTH(v) = -1;
    if (data) {
        data[len] = '\0';
    }
    return v;
}

static unsigned long long vmDisplayIndexFromOffset(size_t offset) {
    if (frontendIsShell()) {
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

static bool adjustLocalByDelta(VM* vm, Value* slot, long long delta, const char* opcode_name) {
    if (!slot) {
        runtimeError(vm, "VM Error: %s encountered a null local slot pointer.", opcode_name);
        return false;
    }

    if (VALUE_TYPE(*slot) == TYPE_ENUM) {
        long long new_ord = (long long)AS_ENUM(*slot).ordinal + delta;
        AS_ENUM(*slot).ordinal = (int)new_ord;
        SET_INT_VALUE(slot, AS_ENUM(*slot).ordinal);
        return true;
    }

    if (isIntlikeType(VALUE_TYPE(*slot))) {
        long long new_val = AS_INTEGER(*slot) + delta;
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

static void replaceValueCell(Value* target, Value replacement, AST* preserved_base_type) {
    if (!target) {
        return;
    }

    pthread_mutex_lock(&value_cell_mutex);
    Value old_value = *target;
    *target = replacement;
    if (VALUE_TYPE(*target) == TYPE_POINTER && PTR_BASE_TYPE_NODE(*target) == NULL && preserved_base_type) {
        PTR_BASE_TYPE_NODE(*target) = preserved_base_type;
    }
    pthread_mutex_unlock(&value_cell_mutex);
    freeValue(&old_value);
}

static bool vmNameIsMyself(const char* name) {
    return name && strcasecmp(name, "myself") == 0;
}

static bool vmNameIsPasExceptionGlobal(const char* name) {
    return name &&
           (strcasecmp(name, "__pas_exc_pending") == 0 ||
            strcasecmp(name, "__pas_exc_message") == 0);
}

static bool vmPasExceptionPending(VM* vm) {
    if (!vm || !vm->vmGlobalSymbols) {
        return false;
    }

    bool pending = false;
    pthread_mutex_lock(&globals_mutex);
    Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, "__pas_exc_pending");
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
    AST* preserved_base = PTR_BASE_TYPE_NODE(vm->threadMyself);
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
    uint16_t entry;
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

static Symbol* vmGetCachedGlobalSymbol(BytecodeChunk* chunk, int index) {
    if (!chunk || !chunk->global_symbol_cache) return NULL;
    if (index < 0 || index >= chunk->constants_capacity) return NULL;
    return chunk->global_symbol_cache[index];
}

static void vmCacheGlobalSymbol(BytecodeChunk* chunk, int index, Symbol* sym) {
    if (!chunk || !chunk->global_symbol_cache) return;
    if (index < 0 || index >= chunk->constants_capacity) return;
    chunk->global_symbol_cache[index] = sym;
}

static ThreadJob* vmThreadJobCreate(VM* vm,
                                    ThreadJobKind kind,
                                    BytecodeChunk* chunk,
                                    uint16_t entry,
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
static Symbol* findProcedureByAddress(HashTable* table, uint16_t address);
static void vmPopulateProcedureAddressCache(VM* vm);
static Symbol* vmGetProcedureByAddress(VM* vm, uint16_t address);

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
    if (thread->resultReady) {
        freeValue(&thread->resultValue);
    }
    thread->resultValue = makeNil();
    thread->resultReady = false;
    thread->resultConsumed = false;
    thread->statusReady = false;
    thread->statusFlag = false;
    thread->statusConsumed = false;
    thread->currentJob = NULL;
    thread->awaitingReuse = false;
    thread->readyForReuse = false;
    thread->queuedAt = (struct timespec){0, 0};
    thread->startedAt = (struct timespec){0, 0};
    thread->finishedAt = (struct timespec){0, 0};
    vmThreadMetricsReset(&thread->metrics);
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
    thread->active = false;
    thread->vm = NULL;
    thread->ownsVm = false;
    thread->inPool = false;
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
            owner->availableWorkers++;
            thread->idle = true;
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
        thread->active = true;
        atomic_store(&thread->cancelRequested, false);
        atomic_store(&thread->paused, false);
        pthread_mutex_unlock(&thread->stateMutex);
        vmThreadJobSignalAssignment(job, thread, threadId);

        if (!vmThreadPrepareWorkerVm(thread, owner, job, threadId)) {
            vmThreadStoreResultDirect(thread, NULL, false);
            vmThreadJobDestroy(job);
            job = NULL;
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
                        Value v = job->args ? job->args[i] : makeNil();
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
                        interpretBytecode(workerVm, workerVm->chunk, workerVm->vmGlobalSymbols,
                                          workerVm->vmConstGlobalSymbols, workerVm->procedureTable, job->entry);
                    }
                } else {
                    vmThreadStoreResultDirect(thread, NULL, false);
                }
                break;
            }
            default:
                break;
        }

        if (!thread->statusReady) {
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

        vmThreadJobDestroy(job);
        job = NULL;

        pthread_mutex_lock(&thread->stateMutex);
        thread->awaitingReuse = true;
        pthread_cond_broadcast(&thread->stateCond);
        while (!thread->readyForReuse && !atomic_load(&thread->killRequested) && !atomic_load(&owner->shuttingDownWorkers)) {
            pthread_cond_wait(&thread->stateCond, &thread->stateMutex);
        }
        bool exitLoop = atomic_load(&thread->killRequested) || atomic_load(&owner->shuttingDownWorkers);
        thread->awaitingReuse = false;
        thread->readyForReuse = false;
        pthread_mutex_unlock(&thread->stateMutex);

        if (exitLoop) {
            break;
        }

        pthread_mutex_lock(&thread->stateMutex);
        vmThreadResetResult(thread);
        thread->active = false;
        pthread_mutex_unlock(&thread->stateMutex);
        workerVm = thread->vm;
    }

    pthread_mutex_lock(&owner->threadRegistryLock);
    if (owner->availableWorkers > 0 && thread->idle) {
        owner->availableWorkers--;
    }
    pthread_mutex_lock(&thread->stateMutex);
    thread->idle = false;
    thread->active = false;
    pthread_mutex_unlock(&thread->stateMutex);
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
    thread->inPool = false;
    thread->currentJob = NULL;

    return NULL;
}

static int createThreadJob(VM* vm,
                           ThreadJobKind kind,
                           BytecodeChunk* chunk,
                           uint16_t entry,
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

    if (vm->workerCount < VM_MAX_WORKERS) {
        for (int i = 1; i < VM_MAX_THREADS; ++i) {
            Thread* candidate = &vm->threads[i];
            if (!candidate->inPool) {
                assignedThread = candidate;
                assignedId = i;
                spawnNewWorker = true;
                break;
            }
        }
    }

    if (spawnNewWorker && assignedThread) {
        int originalGeneration = assignedThread->poolGeneration;
        assignedThread->inPool = true;
        assignedThread->active = true;
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
            assignedThread->inPool = false;
            assignedThread->poolGeneration = originalGeneration;
            assignedThread->active = false;
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
            assignedThread->inPool = false;
            assignedThread->poolGeneration = originalGeneration;
            assignedThread->active = false;
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

        vmThreadJobSignalAssignment(job, assignedThread, assignedId);
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
        pthread_mutex_unlock(&job->assignmentMutex);
    }

    return assignedThread ? assignedId : -1;
}

static int createThreadWithArgs(VM* vm,
                                uint16_t entry,
                                ClosureEnvPayload* closureEnv,
                                Symbol* closureSymbol,
                                int argc,
                                const Value* argv) {
    return createThreadJob(vm, THREAD_JOB_BYTECODE, vm ? vm->chunk : NULL, entry, closureEnv, closureSymbol, argc, argv, NULL,
                           NULL, NULL, NULL, -1, NULL, false, NULL);
}

// Backward-compatible helper: no argument provided, pass NIL
static int createThread(VM* vm, uint16_t entry) {
    return createThreadWithArgs(vm, entry, NULL, NULL, 0, NULL);
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
        pending = vm->abort_requested || vm->exit_requested;
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
        for (int i = 1; i < VM_MAX_THREADS; ++i) {
            Thread* thread = &root->threads[i];
            if (!thread->inPool) {
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

bool vmThreadTakeResult(VM* vm, int threadId, Value* outResult, bool takeValue, bool* outStatus, bool takeStatus) {
    if (!vm) {
        return false;
    }
    if (threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->syncInitialized) {
        return false;
    }

    pthread_mutex_lock(&thread->resultMutex);
    while (!thread->statusReady) {
        if (!thread->active && !thread->awaitingReuse) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
        if (vmConsumeInterruptRequest(vm)) {
            pthread_mutex_unlock(&thread->resultMutex);
            vmThreadCancel(vm, threadId);
            return false;
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
        pthread_mutex_lock(&thread->stateMutex);
        thread->readyForReuse = true;
        pthread_cond_broadcast(&thread->stateCond);
        pthread_mutex_unlock(&thread->stateMutex);
    }
    return true;
}

static bool joinThreadInternal(VM* vm, int id) {
    if (!vm) {
        return false;
    }
    if (id <= 0 || id >= VM_MAX_THREADS) {
        return false;
    }

    Thread* thread = &vm->threads[id];
    if (!thread->inPool) {
        return false;
    }
    pthread_mutex_lock(&thread->resultMutex);
    while (!thread->statusReady) {
        if (!thread->active && !thread->awaitingReuse) {
            pthread_mutex_unlock(&thread->resultMutex);
            return false;
        }
        if (vmConsumeInterruptRequest(vm)) {
            pthread_mutex_unlock(&thread->resultMutex);
            vmThreadCancel(vm, id);
            return false;
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

static void joinThread(VM* vm, int id) {
    if (!vm) {
        return;
    }
    joinThreadInternal(vm, id);
}

bool vmJoinThreadById(VM* vm, int id) {
    joinThread(vm, id);

    /* Ensure any pending status/result is consumed so the worker can be reused. */
    if (vm && id > 0 && id < VM_MAX_THREADS) {
        vmThreadTakeResult(vm, id, NULL, false, NULL, true);
        Thread *thread = &vm->threads[id];
        if (thread && thread->inPool && thread->syncInitialized) {
            bool mark_ready = false;
            pthread_mutex_lock(&thread->resultMutex);
            if (!thread->statusReady) {
                thread->statusFlag = true;
                thread->statusReady = false;
                thread->statusConsumed = true;
                thread->resultConsumed = true;
                mark_ready = true;
            } else if (thread->statusConsumed && (!thread->resultReady || thread->resultConsumed)) {
                mark_ready = true;
            }
            pthread_mutex_unlock(&thread->resultMutex);
            if (mark_ready) {
                pthread_mutex_lock(&thread->stateMutex);
                thread->readyForReuse = true;
                pthread_cond_broadcast(&thread->stateCond);
                pthread_mutex_unlock(&thread->stateMutex);
            }
        }
    }
    return true;
}

bool vmThreadAssignName(VM* vm, int threadId, const char* name) {
    if (!vm || threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    pthread_mutex_lock(&vm->threadRegistryLock);
    Thread* thread = &vm->threads[threadId];
    if (!thread->inPool) {
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
    for (int i = 1; i < VM_MAX_THREADS; ++i) {
        Thread* thread = &vm->threads[i];
        if (!thread->inPool) {
            continue;
        }
        if (strncmp(thread->name, name, THREAD_NAME_MAX) == 0) {
            return i;
        }
    }
    return -1;
}

bool vmThreadPause(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->inPool) {
        return false;
    }
    atomic_store(&thread->paused, true);
    vmThreadWakeStateWaiters(thread);
    return true;
}

bool vmThreadResume(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->inPool) {
        return false;
    }
    atomic_store(&thread->paused, false);
    vmThreadWakeStateWaiters(thread);
    return true;
}

bool vmThreadCancel(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->inPool) {
        return false;
    }
    atomic_store(&thread->cancelRequested, true);
    vmThreadWakeStateWaiters(thread);
    pthread_mutex_lock(&thread->stateMutex);
    thread->readyForReuse = true;
    pthread_cond_broadcast(&thread->stateCond);
    pthread_mutex_unlock(&thread->stateMutex);
    vmThreadJobQueueWake(vm->jobQueue);
    return true;
}

bool vmThreadKill(VM* vm, int threadId) {
    if (!vm || threadId <= 0 || threadId >= VM_MAX_THREADS) {
        return false;
    }
    Thread* thread = &vm->threads[threadId];
    if (!thread->inPool) {
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
    for (int i = 1; i < VM_MAX_THREADS && count < capacity; ++i) {
        Thread* thread = &vm->threads[i];
        if (!thread->inPool) {
            continue;
        }
        if (pthread_mutex_trylock(&thread->stateMutex) != 0) {
            continue;
        }
        ThreadMetrics snapshot = thread->metrics;
        if (thread->active) {
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
    for (int i = 1; i < VM_MAX_THREADS && count < capacity; ++i) {
        Thread* thread = &root->threads[i];
        bool include = thread->inPool || thread->active || thread->vm != NULL;
        if (!include) {
            continue;
        }

        VMProcWorkerSnapshot snapshot;
        memset(&snapshot, 0, sizeof(snapshot));
        snapshot.slot_id = i;
        snapshot.vm_address = (uintptr_t)thread->vm;

        bool locked = (pthread_mutex_trylock(&thread->stateMutex) == 0);
        snapshot.in_pool = thread->inPool;
        snapshot.active = thread->active;
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
        if (thread->active && snapshot.metrics.start.valid) {
            ThreadMetricsSample currentSample = snapshot.metrics.start;
            vmThreadMetricsCapture(&currentSample);
            snapshot.metrics.end = currentSample;
        }
        if (thread->name[0]) {
            strncpy(snapshot.name, thread->name, sizeof(snapshot.name) - 1);
            snapshot.name[sizeof(snapshot.name) - 1] = '\0';
        } else {
            snprintf(snapshot.name, sizeof(snapshot.name), "worker-%d", i);
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
    // If none found, append a new mutex if capacity allows.
    if (id == -1) {
        if (owner->mutexCount >= VM_MAX_MUTEXES) {
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
static void vmDumpStackInternal(VM* vm, bool detailed) {
    if (!vm) return;

    if (detailed) {
        for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
            fprintf(stderr, "  [ ");
            printValueToStream(*slot, stderr);
            fprintf(stderr, " ]\n");
        }
    } else {
        for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
            fprintf(stderr, "[");
            printValueToStream(*slot, stderr);
            fprintf(stderr, "] ");
        }
        fprintf(stderr, "\n");
    }
}

static void assignRealToIntChecked(VM* vm, Value* dest, long double real_val) {
    bool range_error = false;
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
            } else if (real_val > (long double)UINT64_MAX) {
                range_error = true;
                tmp = UINT64_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            VAL_UINT(*dest) = tmp;
            VAL_INT(*dest) = (tmp <= (unsigned long long)LLONG_MAX) ? (long long)tmp : LLONG_MAX;
            break;
        }
        case TYPE_INT64: {
            long long tmp;
            if (real_val < (long double)LLONG_MIN) {
                range_error = true;
                tmp = LLONG_MIN;
            } else if (real_val > (long double)LLONG_MAX) {
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
            } else if (real_val > (long double)LLONG_MAX) {
                range_error = true;
                tmp = LLONG_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
    }
    if (range_error) {
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

    long long item_ord;
    bool item_is_ordinal = false;

    // Get ordinal value of the item
    switch (VALUE_TYPE(*itemVal)) {
        case TYPE_INTEGER:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_BOOLEAN:
            item_ord = VAL_INT(*itemVal);
            item_is_ordinal = true;
            break;
        case TYPE_CHAR:
            item_ord = (long long)AS_CHAR(*itemVal);
            item_is_ordinal = true;
            break;
        case TYPE_ENUM:
            item_ord = (long long)AS_ENUM(*itemVal).ordinal;
            item_is_ordinal = true;
            break;
        default:
            item_is_ordinal = false;
            break;
    }

    if (!item_is_ordinal) return false; // Item is not of a type that can be in a set

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
    if (AS_SET(*setVal).set_size >= STRING_MAX_LENGTH(*setVal)) {
        int new_capacity = (STRING_MAX_LENGTH(*setVal) == 0) ? 8 : STRING_MAX_LENGTH(*setVal) * 2;
        long long* new_values = realloc(AS_SET(*setVal).set_values, sizeof(long long) * new_capacity);
        if (!new_values) {
            return false;
        }
        AS_SET(*setVal).set_values = new_values;
        STRING_MAX_LENGTH(*setVal) = new_capacity;
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

static bool vmBuildSetFromRange(Value* outSet, long long start_ord, long long end_ord) {
    if (!outSet) {
        return false;
    }
    *outSet = makeValueForType(TYPE_SET, NULL, NULL);
    if (start_ord <= end_ord) {
        for (long long current = start_ord; current <= end_ord; ++current) {
            if (!vmAddOrdinalToSet(outSet, current)) {
                return false;
            }
            if (current == LLONG_MAX) {
                break;
            }
        }
        return true;
    }
    for (long long current = start_ord;; --current) {
        if (!vmAddOrdinalToSet(outSet, current)) {
            return false;
        }
        if (current == end_ord) {
            return true;
        }
        if (current == LLONG_MIN) {
            return false;
        }
    }
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

// runtimeError - Assuming your existing one is fine.
void runtimeError(VM* vm, const char* format, ...) {
    if (vm) {
        vm->abort_requested = true;
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

        int start_dump_offset = (int)instruction_offset - 10;
        if (!have_runtime_location || start_dump_offset < 0) start_dump_offset = 0;

        fprintf(stderr, "\nLast Instructions executed (leading to crash, up to %d bytes before error point):\n", (int)instruction_offset - start_dump_offset);
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

static Value copyValueForStack(const Value* src) {
    if (!src) {
        return makeNil();
    }

    pthread_mutex_lock(&value_cell_mutex);

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
        case TYPE_LONG_DOUBLE:
        case TYPE_NIL:
        {
            Value copy = *src;
            pthread_mutex_unlock(&value_cell_mutex);
            return copy;
        }
        default:
            break;
    }

    if (VALUE_TYPE(*src) == TYPE_MEMORYSTREAM) {
        Value alias = *src;
        if (AS_MSTREAM(alias)) {
            retainMStream(AS_MSTREAM(alias));
        }
        pthread_mutex_unlock(&value_cell_mutex);
        return alias;
    }

    if (VALUE_TYPE(*src) == TYPE_CLOSURE) {
        Value alias = *src;
        if (AS_CLOSURE(alias).env) {
            retainClosureEnv(AS_CLOSURE(alias).env);
        }
        pthread_mutex_unlock(&value_cell_mutex);
        return alias;
    }

    Value copy = makeCopyOfValue(src);
    pthread_mutex_unlock(&value_cell_mutex);
    return copy;
}

static void push(VM* vm, Value value) { // Using your original name 'push'
    if (vm->stackTop - vm->stack >= VM_STACK_MAX) {
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

static Symbol* findProcedureByAddress(HashTable* table, uint16_t address) {
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

static Symbol* vmGetProcedureByAddress(VM* vm, uint16_t address) {
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

static Value pop(VM* vm) {
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow (pop from empty stack).");
        return makeNil();
    }
    vm->stackTop--;
    Value result = *vm->stackTop; // Make a copy of the value to return.

    // Mark the slot as vacant without incurring the cost of makeNil().
    SET_VALUE_TYPE(vm->stackTop, TYPE_VOID);
    AS_POINTER(*vm->stackTop) = NULL;

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
        uint16_t entry = 0;
        bool validEntry = false;
        ClosureEnvPayload* closureEnv = NULL;
        Symbol* closureSymbol = NULL;
        if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
            entry = (uint16_t)AS_CLOSURE(addrVal).entry_offset;
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
                entry = (uint16_t)proc_symbol->bytecode_address;
                closureSymbol = proc_symbol;
                validEntry = true;
            }
        } else if (IS_INTLIKE(addrVal)) {
            entry = (uint16_t)AS_INTEGER(addrVal);
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
        uint16_t entry = 0;
        bool validEntry = false;
        ClosureEnvPayload* closureEnv = NULL;
        Symbol* closureSymbol = NULL;
        if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
            entry = (uint16_t)AS_CLOSURE(addrVal).entry_offset;
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
                entry = (uint16_t)proc_symbol->bytecode_address;
                closureSymbol = proc_symbol;
                validEntry = true;
            }
        } else if (IS_INTLIKE(addrVal)) {
            entry = (uint16_t)AS_INTEGER(addrVal);
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

static Value vmHostCreateClosure(VM* vm) {
    Value entryVal = pop(vm);
    if (!IS_INTLIKE(entryVal)) {
        freeValue(&entryVal);
        runtimeError(vm, "VM Error: Closure creation requires integer entry address.");
        return makeNil();
    }
    uint16_t entry = (uint16_t)AS_INTEGER(entryVal);
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

    ClosureEnvPayload* env = createClosureEnv((uint16_t)capture_count);
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

static RuntimeVTableEntry* runtimeVTables = NULL;

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
    RuntimeVTableEntry* entry = runtimeVTables;
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
    runtimeVTables = NULL;
}

static void vmCleanupGlobalCachesIfIdle(void) {
    if (!vmProcRegistryIsEmpty()) {
        return;
    }
    vmFreeRuntimeVTables();
    vmFreeShellBuiltinProfiles();
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

static bool ensureRuntimeInterfaceVTable(VM* vm,
                                         const char* className,
                                         AST* interfaceType,
                                         Value* tableValue) {
    if (!vm || !className || !interfaceType || !tableValue) {
        return false;
    }

    if (VALUE_TYPE(*tableValue) == TYPE_ARRAY && AS_ARRAY(*tableValue)) {
        return true;
    }

    if (!vm->procedureTable) {
        return false;
    }

    char class_lower[MAX_SYMBOL_LENGTH];
    strncpy(class_lower, className, sizeof(class_lower) - 1);
    class_lower[sizeof(class_lower) - 1] = '\0';
    toLowerString(class_lower);

    char interface_lower[MAX_SYMBOL_LENGTH];
    const char* interface_name = (interfaceType->token && interfaceType->token->value)
                                     ? interfaceType->token->value
                                     : "<anonymous_interface>";
    strncpy(interface_lower, interface_name, sizeof(interface_lower) - 1);
    interface_lower[sizeof(interface_lower) - 1] = '\0';
    toLowerString(interface_lower);

    char cache_key[(MAX_SYMBOL_LENGTH * 2) + 4];
    snprintf(cache_key, sizeof(cache_key), "%s|%s", class_lower, interface_lower);

    RuntimeVTableEntry* entry = findRuntimeVTableEntry(cache_key);
    bool need_build = true;
    if (entry && entry->table && VALUE_TYPE(*entry->table) == TYPE_ARRAY && AS_ARRAY(*entry->table)) {
        need_build = false;
    }

    int method_capacity = 0;
    int method_count = 0;
    int* addrs = NULL;

    if (need_build) {
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

        if (!entry) {
            entry = createRuntimeVTableEntry(cache_key);
            if (!entry) {
                freeValue(&arr);
                return false;
            }
        }

        if (!entry->table) {
            entry->table = (Value*)malloc(sizeof(Value));
            if (!entry->table) {
                freeValue(&arr);
                return false;
            }
            *(entry->table) = makeNil();
        } else {
            freeValue(entry->table);
        }
        *(entry->table) = arr;
    }

    if (!entry) {
        entry = findRuntimeVTableEntry(class_lower);
    }
    if (!entry || !entry->table) {
        return false;
    }

    if (tableValue == entry->table) {
        return true;
    }

    if (VALUE_TYPE(*tableValue) == TYPE_POINTER && AS_POINTER(*tableValue) == entry->table) {
        return true;
    }

    freeValue(tableValue);
    *tableValue = makePointer(entry->table, NULL);
    return true;
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
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        runtimeError(vm, "VM Error: Unknown interface type '%s'.", AS_STRING(typeNameVal));
        return makeNil();
    }

    Value* tableSlotPtr = NULL;
    Value* tableValuePtr = NULL;
    if (VALUE_TYPE(tablePtrVal) == TYPE_POINTER && AS_POINTER(tablePtrVal)) {
        tableSlotPtr = (Value*)AS_POINTER(tablePtrVal);
        tableValuePtr = tableSlotPtr;
        if (tableValuePtr && VALUE_TYPE(*tableValuePtr) == TYPE_POINTER && AS_POINTER(*tableValuePtr)) {
            tableValuePtr = (Value*)AS_POINTER(*tableValuePtr);
        }
    }

    if (!tableSlotPtr && VALUE_TYPE(receiverVal) == TYPE_POINTER && AS_POINTER(receiverVal)) {
        bool invalid_type = false;
        Value* existingRecord = resolveRecord(&receiverVal, &invalid_type);
        if (!invalid_type && existingRecord && VALUE_TYPE(*existingRecord) == TYPE_RECORD) {
            FieldValue* hiddenField = AS_RECORD(*existingRecord);
            if (hiddenField) {
                tableSlotPtr = fieldValueStorage(hiddenField);
                tableValuePtr = tableSlotPtr;
                if (tableValuePtr && VALUE_TYPE(*tableValuePtr) == TYPE_POINTER && AS_POINTER(*tableValuePtr)) {
                    tableValuePtr = (Value*)AS_POINTER(*tableValuePtr);
                }
            }
        }
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
        FieldValue* hiddenField = AS_RECORD(*clonedRecord);
        if (!hiddenField) {
            freeValue(&classNameVal);
            freeValue(&typeNameVal);
            freeValue(&receiverVal);
            freeValue(&tablePtrVal);
            runtimeError(vm, "VM Error: Cloned receiver lacks vtable storage.");
            return makeNil();
        }
        tableSlotPtr = fieldValueStorage(hiddenField);
        tableValuePtr = tableSlotPtr;
        if (tableValuePtr && VALUE_TYPE(*tableValuePtr) == TYPE_POINTER && AS_POINTER(*tableValuePtr)) {
            tableValuePtr = (Value*)AS_POINTER(*tableValuePtr);
        }
    }

    const char* class_name_str = (VALUE_TYPE(classNameVal) == TYPE_STRING && AS_STRING(classNameVal))
                                     ? AS_STRING(classNameVal)
                                     : NULL;
    if (!tableValuePtr || !ensureRuntimeInterfaceVTable(vm, class_name_str, interfaceType, tableValuePtr)) {
        runtimeError(vm, "VM Error: Unable to initialise interface table for class '%s'.",
                     class_name_str ? class_name_str : "<unknown>");
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        return makeNil();
    }
    Value* resolvedTablePtr = tableSlotPtr;
    while (resolvedTablePtr && VALUE_TYPE(*resolvedTablePtr) == TYPE_POINTER) {
        resolvedTablePtr = (Value*)AS_POINTER(*resolvedTablePtr);
    }
    if (!resolvedTablePtr || VALUE_TYPE(*resolvedTablePtr) != TYPE_ARRAY || !AS_ARRAY(*resolvedTablePtr)) {
        runtimeError(vm, "VM Error: Resolved vtable storage for class '%s' is invalid.",
                     class_name_str ? class_name_str : "<unknown>");
        freeValue(&classNameVal);
        freeValue(&typeNameVal);
        freeValue(&receiverVal);
        freeValue(&tablePtrVal);
        return makeNil();
    }
    tableValuePtr = resolvedTablePtr;

    ClosureEnvPayload* payload = createClosureEnv(3);
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

    uint16_t target_address = (uint16_t)AS_INTEGER(entry);

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
    if (VALUE_TYPE(tidVal) == TYPE_THREAD) {
        int id = (int)AS_INTEGER(tidVal);
        joinThread(vm, id);
    } else if (IS_INTLIKE(tidVal)) {
        int id = (int)AS_INTEGER(tidVal);
        joinThread(vm, id);
    }
    freeValue(&tidVal);
    return makeInt(0);
}

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
                char flags[8];
                size_t flag_len = 0;
                const char* flag_chars = "-+ #0'";
                while (j < flen && strchr(flag_chars, fmt[j]) != NULL) {
                    if (flag_len + 1 < sizeof(flags)) {
                        flags[flag_len++] = fmt[j];
                    }
                    j++;
                }
                flags[flag_len] = '\0';
                bool width_specified = false;
                int width = 0;
                while (j < flen && isdigit((unsigned char)fmt[j])) {
                    width_specified = true;
                    width = width * 10 + (fmt[j]-'0');
                    j++;
                }
                int precision = -1;
                if (j < flen && fmt[j] == '.') {
                    j++;
                    precision = 0;
                    while (j < flen && isdigit((unsigned char)fmt[j])) { precision = precision * 10 + (fmt[j]-'0'); j++; }
                }
                const char* length_mods = "hlLjzt";
                size_t mod_start = j;
                while (j < flen && strchr(length_mods, fmt[j]) != NULL) j++;
                char lenmod[4] = {0};
                size_t mod_len = (j > mod_start) ? (j - mod_start) : 0;
                if (mod_len > 0) {
                    if (mod_len > sizeof(lenmod) - 1) mod_len = sizeof(lenmod) - 1;
                    memcpy(lenmod, fmt + mod_start, mod_len);
                    lenmod[mod_len] = '\0';
                }
                char spec = (j < flen) ? fmt[j] : '\0';

                char fmtbuf[64];
                char buf[DEFAULT_STRING_CAPACITY];
                Value v = args[arg_index++];
                size_t copy_len = (spec && spec != '\0') ? (j - i + 1) : 0;
                if (copy_len >= sizeof(fmtbuf)) copy_len = sizeof(fmtbuf) - 1;
                if (copy_len > 0) {
                    memcpy(fmtbuf, fmt + i, copy_len);
                    fmtbuf[copy_len] = '\0';
                } else {
                    fmtbuf[0] = '%';
                    size_t pos = 1;
                    if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                        memcpy(&fmtbuf[pos], flags, flag_len);
                        pos += flag_len;
                    }
                    if (width_specified) {
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) pos = sizeof(fmtbuf) - 1;
                            else pos += (size_t)written;
                        }
                    }
                    if (precision >= 0 && pos < sizeof(fmtbuf)) {
                        fmtbuf[pos++] = '.';
                        int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                        if (written > 0) {
                            if ((size_t)written >= sizeof(fmtbuf) - pos) pos = sizeof(fmtbuf) - 1;
                            else pos += (size_t)written;
                        }
                    }
                    size_t lenmod_len = strlen(lenmod);
                    if (lenmod_len > 0 && pos + lenmod_len < sizeof(fmtbuf)) {
                        memcpy(&fmtbuf[pos], lenmod, lenmod_len);
                        pos += lenmod_len;
                    }
                    if (pos < sizeof(fmtbuf)) {
                        fmtbuf[pos++] = spec ? spec : ' ';
                    }
                    fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';
                }
                bool expects_wide_char = (lenmod[0] != '\0' && strpbrk(lenmod, "lL") != NULL);
                switch (spec) {
                    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                        unsigned long long u = 0ULL; long long s = 0LL;
                        if (isIntlikeType(VALUE_TYPE(v)) || VALUE_TYPE(v) == TYPE_BOOLEAN || VALUE_TYPE(v) == TYPE_CHAR) {
                            s = AS_INTEGER(v);
                            u = (unsigned long long)AS_INTEGER(v);
                        }
                        bool is_unsigned = (spec=='u'||spec=='o'||spec=='x'||spec=='X');
                        // Cast to the correct type expected by the format length modifier
                        if (is_unsigned) {
                            if (strcmp(lenmod, "ll") == 0) {
                                unsigned long long val = (unsigned long long)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "l") == 0) {
                                unsigned long val = (unsigned long)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "j") == 0) {
                                uintmax_t val = (uintmax_t)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "z") == 0) {
                                size_t val = (size_t)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else {
                                unsigned int val = (unsigned int)u; // includes h, hh, and default
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            }
                        } else {
                            if (strcmp(lenmod, "ll") == 0) {
                                long long val = (long long)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "l") == 0) {
                                long val = (long)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "j") == 0) {
                                intmax_t val = (intmax_t)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "t") == 0) {
                                ptrdiff_t val = (ptrdiff_t)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else {
                                int val = (int)s; // includes h, hh, and default
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            }
                        }
                        fputs(buf, stdout);
                        break;
                    }
                    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                        long double rv = isRealType(VALUE_TYPE(v)) ? AS_REAL(v) : (long double)AS_INTEGER(v);
                        // If 'L' modifier is present, pass a long double; otherwise pass double
                        if (strcmp(lenmod, "L") == 0) {
                            // Print directly with long double argument
                            // Use a temporary buffer via vsnprintf-like call by delegating to snprintf
                            // Note: snprintf supports %Lf on platforms where long double is distinct.
                            // We still format into buf to keep existing behavior (collect into string before fputs)
                            // Casting to long double explicitly for clarity.
                            snprintf(buf, sizeof(buf), fmtbuf, (long double)rv);
                        } else {
                            snprintf(buf, sizeof(buf), fmtbuf, (double)rv);
                        }
                        fputs(buf, stdout);
                        break;
                    }
                    case 'c': {
                        char ch = (VALUE_TYPE(v) == TYPE_CHAR) ? AS_CHAR(v) : (char)AS_INTEGER(v);
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (expects_wide_char) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, lenmod);
                            if (mod_pos) {
                                size_t remove_len = strlen(lenmod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, (unsigned int)ch);
                        fputs(buf, stdout);
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
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (expects_wide_char) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, lenmod);
                            if (mod_pos) {
                                size_t remove_len = strlen(lenmod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, sv);
                        fputs(buf, stdout);
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

    vm->exit_requested = false;
    vm->abort_requested = false;
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
    for (int i = 1; i < VM_MAX_THREADS; ++i) {
        Thread* thread = &vm->threads[i];
        if (thread->inPool) {
            atomic_store(&thread->killRequested, true);
            vmThreadWakeStateWaiters(thread);
        }
        if (thread->active) {
            pthread_join(thread->handle, NULL);
            thread->active = false;
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
    vm->threads[0].active = false;
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
    resetStack(vm);
    vm->chunk = NULL;
    vm->ip = NULL;
    vm->lastInstruction = NULL;
    vm->vmGlobalSymbols = NULL;              // Will be set by interpretBytecode
    vm->vmConstGlobalSymbols = NULL;
    vm->procedureTable = NULL;
    vm->procedureByAddress = NULL;
    vm->procedureByAddressSize = 0;

    vm->frameCount = 0; // <--- INITIALIZE frameCount

    vm->exit_requested = false;
    vm->abort_requested = false;
    vm->suspend_unwind_requested = false;
    vm->current_builtin_name = NULL;
    vm->threadMyself = makeNil();

    vm->threadCount = 1; // main thread occupies index 0
    vm->threadOwner = vm;
    memset(vm->threads, 0, sizeof(vm->threads));
    for (int i = 0; i < VM_MAX_THREADS; i++) {
        vmThreadInitSlot(&vm->threads[i]);
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
    for (int i = 0; i < VM_MAX_MUTEXES; i++) {
        vm->mutexes[i].active = false;
    }

    vm->owningThread = NULL;
    vm->threadId = 0;
    vm->frontendContext = NULL;
    vm->shellIndexing = frontendIsShell();

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

    atomic_store(&vm->shuttingDownWorkers, true);
    if (vm->jobQueue) {
        pthread_mutex_lock(&vm->jobQueue->mutex);
        vm->jobQueue->shuttingDown = true;
        pthread_cond_broadcast(&vm->jobQueue->cond);
        pthread_mutex_unlock(&vm->jobQueue->mutex);
    }
    for (int i = 1; i < VM_MAX_THREADS; i++) {
        Thread* thread = &vm->threads[i];
        bool shouldJoin = thread->inPool || thread->active;
        if (thread->inPool) {
            atomic_store(&thread->killRequested, true);
            vmThreadWakeStateWaiters(thread);
        }
        if (shouldJoin) {
            pthread_join(thread->handle, NULL);
            thread->active = false;
        }
        if (thread->ownsVm && thread->vm) {
            freeVM(thread->vm);
            free(thread->vm);
            thread->vm = NULL;
            thread->ownsVm = false;
        }
        thread->inPool = false;
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
    for (int i = 0; i < VM_MAX_THREADS; i++) {
        vmThreadDestroySlot(&vm->threads[i]);
    }
    pthread_mutex_destroy(&vm->mutexRegistryLock);
    vmCleanupGlobalCachesIfIdle();
    // No explicit freeing of vm->host_functions array itself as it's part of
    // the VM struct. If HostFn entries allocated memory, that would require
    // additional handling.
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
        Value returnValue = pop(vm);
        safeReturnValue = copyValueForStack(&returnValue);
        freeValue(&returnValue);
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
// These helpers intentionally skip overflow/underflow checks and should only
// be used in opcode handlers where stack depth has already been validated by
// preceding logic.
static inline void vmFastPushUnchecked(VM* vm, Value value) {
    *vm->stackTop = value;
    vm->stackTop++;
}

static inline Value vmFastPopUnchecked(VM* vm) {
    vm->stackTop--;
    return *vm->stackTop;
}

#define FAST_PUSH(v) vmFastPushUnchecked(vm, (v))
#define FAST_POP() vmFastPopUnchecked(vm)
#define FAST_PEEK(dist) (vm->stackTop[-((dist) + 1)])

static inline Symbol* vmInlineCacheReadSymbol(uint8_t* slot) {
    Symbol* sym = NULL;
    memcpy(&sym, slot, sizeof(Symbol*));
    return sym;
}

static inline void vmInlineCacheWriteSymbol(uint8_t* slot, Symbol* sym) {
    memcpy(slot, &sym, sizeof(Symbol*));
}

static inline void vmPatchGlobalOpcode(uint8_t* instruction, bool isSet, bool isWide) {
    uint8_t newOpcode = isSet
        ? (isWide ? SET_GLOBAL16_CACHED : SET_GLOBAL_CACHED)
        : (isWide ? GET_GLOBAL16_CACHED : GET_GLOBAL_CACHED);
    *instruction = newOpcode;
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

// Shared logic for DEFINE_GLOBAL and DEFINE_GLOBAL16.
// Assumes the name has already been read (as Value) and the IP is positioned
// at the declared type byte.
static InterpretResult handleDefineGlobal(VM* vm, Value varNameVal) {
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
                    runtimeError(vm, "VM Error: Array bound constant index out of range for '%s'.", AS_STRING(varNameVal));
                    free(lower_bounds); free(upper_bounds);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value lower_val = vm->chunk->constants[lower_idx];
                Value upper_val = vm->chunk->constants[upper_idx];
                if (!isIntlikeType(VALUE_TYPE(lower_val)) || !isIntlikeType(VALUE_TYPE(upper_val))) {
                    runtimeError(vm, "VM Error: Invalid constant types for array bounds of '%s'.", AS_STRING(varNameVal));
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
            runtimeError(vm, "VM Error: Array element type constant index out of range for '%s'.", AS_STRING(varNameVal));
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
            runtimeError(vm, "VM Error: Failed to allocate array for global '%s'.", AS_STRING(varNameVal));
            freeValue(&array_value);
            return INTERPRET_RUNTIME_ERROR;
        }

        Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(varNameVal));
        if (sym == NULL) {
            sym = (Symbol*)malloc(sizeof(Symbol));
            if (!sym) {
                runtimeError(vm, "VM Error: Malloc failed for Symbol struct for global array '%s'.", AS_STRING(varNameVal));
                freeValue(&array_value);
                return INTERPRET_RUNTIME_ERROR;
            }
            sym->name = strdup(AS_STRING(varNameVal));
            if (!sym->name) {
                runtimeError(vm, "VM Error: Malloc failed for symbol name for global array '%s'.", AS_STRING(varNameVal));
                free(sym); freeValue(&array_value);
                return INTERPRET_RUNTIME_ERROR;
            }
            toLowerString(sym->name);
            sym->type = declaredType;
            sym->type_def = NULL;
            sym->value = (Value*)malloc(sizeof(Value));
            if (!sym->value) {
                runtimeError(vm, "VM Error: Malloc failed for Value struct for global array '%s'.", AS_STRING(varNameVal));
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
            runtimeWarning(vm, "VM Warning: Global variable '%s' redefined.", AS_STRING(varNameVal));
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
            Value len_val = vm->chunk->constants[len_idx];
            if (VALUE_TYPE(len_val) == TYPE_INTEGER) {
                str_len = (int)VAL_INT(len_val);
            }
        }
        Value typeNameVal = vm->chunk->constants[type_name_idx];
        // (debug logging removed)
        AST* type_def_node = NULL;
        if (declaredType == TYPE_STRING && str_len > 0) {
            Token* strTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, strTok);
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
                    runtimeError(vm, "VM Error: Enum type '%s' not found for global '%s'.", AS_STRING(typeNameVal), AS_STRING(varNameVal));
                    return INTERPRET_RUNTIME_ERROR;
                }
            }
        }

        if (declaredType == TYPE_POINTER && type_def_node == NULL) {
            // Final safety: default pointer base to integer if not provided
            Token* baseTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, baseTok);
            setTypeAST(type_def_node, TYPE_INT32);
            freeToken(baseTok);
        }

        if (VALUE_TYPE(varNameVal) != TYPE_STRING || !AS_STRING(varNameVal)) {
            runtimeError(vm, "VM Error: Invalid variable name for DEFINE_GLOBAL.");
            return INTERPRET_RUNTIME_ERROR;
        }

        Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(varNameVal));
        if (sym == NULL) {
            sym = createSymbolForVM(AS_STRING(varNameVal), declaredType, type_def_node);
            if (!sym) {
                runtimeError(vm, "VM Error: Failed to create symbol for global '%s'.", AS_STRING(varNameVal));
                return INTERPRET_RUNTIME_ERROR;
            }
            hashTableInsert(vm->vmGlobalSymbols, sym);
        } else {
            runtimeWarning(vm, "VM Warning: Global variable '%s' redefined.", AS_STRING(varNameVal));
        }

        if (declaredType == TYPE_FILE && sym && sym->value) {
            if (file_element_type != TYPE_VOID && file_element_type != TYPE_UNKNOWN) {
                ARRAY_ELEMENT_TYPE(*sym->value) = file_element_type;
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
                        ARRAY_ELEMENT_TYPE_DEF(*sym->value) = elem_def;
                    }
                }
            }
        }
    }

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
static uint8_t gVmBuiltinNeedsLockCache[UINT16_MAX + 1];
static uint8_t gVmBuiltinTypeCache[UINT16_MAX + 1];

static bool vmBuiltinNeedsGlobalLockCached(int builtin_id, const char* fallback_name) {
    if (builtin_id >= 0 && builtin_id <= UINT16_MAX) {
        uint8_t cached = gVmBuiltinNeedsLockCache[builtin_id];
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
        if (gVmBuiltinNeedsLockCache[builtin_id] == VM_BUILTIN_CACHE_UNKNOWN) {
            gVmBuiltinNeedsLockCache[builtin_id] = needs_lock ? VM_BUILTIN_CACHE_TRUE : VM_BUILTIN_CACHE_FALSE;
        }
        pthread_mutex_unlock(&gVmBuiltinMetadataCacheMutex);
        return needs_lock;
    }

    return builtinUsesGlobalStructures(fallback_name);
}

static BuiltinRoutineType vmBuiltinTypeCached(int builtin_id, const char* fallback_name) {
    if (builtin_id >= 0 && builtin_id <= UINT16_MAX) {
        uint8_t cached = gVmBuiltinTypeCache[builtin_id];
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
        if (gVmBuiltinTypeCache[builtin_id] == VM_BUILTIN_TYPE_CACHE_UNKNOWN) {
            gVmBuiltinTypeCache[builtin_id] = encoded;
        }
        pthread_mutex_unlock(&gVmBuiltinMetadataCacheMutex);
        return builtin_type;
    }

    return fallback_name ? getBuiltinType(fallback_name) : BUILTIN_TYPE_NONE;
}

// --- Main Interpretation Loop ---
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* const_globals, HashTable* procedures, uint16_t entry) {
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
    vm->ip = vm->chunk->code + entry;
    vm->lastInstruction = vm->ip;
    vm->abort_requested = false;
    vm->suspend_unwind_requested = false;
    vm->shellIndexing = frontendIsShell();

    vm->vmGlobalSymbols = globals;    // Store globals table (ensure this is the intended one)
    vm->vmConstGlobalSymbols = const_globals; // Table of constant globals (no locking)
    vm->procedureTable = procedures; // <--- STORED procedureTable
    vmPopulateProcedureAddressCache(vm);

#if VM_USE_COMPUTED_GOTO
    static void *dispatch_table[OPCODE_COUNT] = {
#define OP(name, value, operands, stack_in, stack_out) [value] = &&LABEL_##name,
#include "compiler/opcodes.def"
#undef OP
    };
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
                 result_val = makeInt(iresult); \
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
                if ((IS_STRING(*final_a) || IS_CHAR(*final_a)) && \
                    (IS_STRING(*final_b) || IS_CHAR(*final_b) || current_instruction_code == ADD)) { \
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
                    if (final_a == &a_val_popped && VALUE_TYPE(a_val_popped) == TYPE_STRING && AS_STRING(a_val_popped)) { \
                        temp_concat_buffer = (char*)realloc(AS_STRING(a_val_popped), total_len + 1); \
                        if (!temp_concat_buffer) { \
                            free(coerced_b); \
                            runtimeError(vm, "Runtime Error: Realloc failed for string concatenation buffer."); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                        } \
                        memcpy(temp_concat_buffer + len_a, s_b, len_b); \
                        temp_concat_buffer[total_len] = '\0'; \
                        AS_STRING(a_val_popped) = NULL; \
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
                    SET_VALUE_TYPE(&result_val, result_type); \
                    free(coerced_b); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    op_is_handled = true; \
                } else if ((IS_STRING(*final_b) || IS_CHAR(*final_b)) && current_instruction_code == ADD) { \
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
                    SET_VALUE_TYPE(&result_val, result_type); \
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
                    PTR_BASE_TYPE_NODE(result_val) = PTR_BASE_TYPE_NODE(enum_value); \
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
                if (a_real || b_real) { \
                    /*
                     * When an integer participates in real arithmetic, operate on
                     * temporary copies so the original integer Value retains its
                     * type.  This prevents implicit widening of integer operands.
                     */ \
                    Value a_tmp = makeCopyOfValue(&a_val_popped); \
                    Value b_tmp = makeCopyOfValue(&b_val_popped); \
                    long double fa = asLd(a_tmp); \
                    long double fb = asLd(b_tmp); \
                    freeValue(&a_tmp); \
                    freeValue(&b_tmp); \
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
                    switch (current_instruction_code) { \
                        case ADD: \
                            overflow = __builtin_add_overflow(ia, ib, &iresult); \
                            break; \
                        case SUBTRACT: \
                            overflow = __builtin_sub_overflow(ia, ib, &iresult); \
                            break; \
                        case MULTIPLY: \
                            overflow = __builtin_mul_overflow(ia, ib, &iresult); \
                            break; \
                        case DIVIDE: \
                            result_val = makeReal((long double)ia / (long double)ib); \
                            break; \
                        case MOD: \
                            iresult = ib == 0 ? 0 : ia % ib; \
                            break; \
        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for integers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                    if (current_instruction_code == DIVIDE) { \
                        /* result_val already set for division */ \
                    } else if (overflow) { \
                        runtimeError(vm, "Runtime Error: Integer overflow."); \
                        freeValue(&a_val_popped); \
                        freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } else { \
                        result_val = makeInt(iresult); \
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
        if (vmConsumeSuspendRequest(vm)) {
            /* Marked as a cooperative stop request; unwind below via exit_requested. */
        }
        if (vmConsumeInterruptRequest(vm)) {
            /* VM abort flag is set; let the normal exit/abort handling run. */
        }
        if (pending_exit_flag && *pending_exit_flag) {
            shellRuntimeMaybeRequestPendingExit(vm);
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
        if (instruction_val >= OPCODE_COUNT) {
            goto LABEL_INVALID;
        }
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
                push(vm, copyValueForStack(&constant));
                break;
            }
                
            case CONSTANT16: {
                uint16_t idx = READ_SHORT(vm);
                if (idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Constant index %u out of bounds for CONSTANT16.", idx);
                   return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(&vm->chunk->constants[idx]));
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
                const char* str = AS_STRING(*string_val) ? AS_STRING(*string_val) : "";
                size_t len = strlen(str);

                size_t char_offset = 0;
                if (!vmResolveStringIndex(vm, pscal_index, len, &char_offset, false, NULL)) {
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value popped_string_ptr = pop(vm);
                freeValue(&popped_string_ptr);

                push(vm, makePointer(&AS_STRING(*string_val)[char_offset], STRING_CHAR_PTR_SENTINEL));
                break;
            }
            case GET_GLOBAL_ADDRESS: {
                uint8_t name_idx = READ_BYTE();
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL_ADDRESS.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global name for address lookup.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    push(vm, makePointer(&vm->threadMyself, NULL));
                    break;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, AS_STRING(*name_val));
                    if (sym && sym->value) {
                        push(vm, makePointer(sym->value, NULL));
                        break;
                    }
                }
                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Global '%s' not found in symbol table.", AS_STRING(*name_val));
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(sym->value, NULL));
                break;
            }
            case GET_GLOBAL_ADDRESS16: {
                uint16_t name_idx = READ_SHORT(vm);
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL_ADDRESS16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global name for address lookup.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    push(vm, makePointer(&vm->threadMyself, NULL));
                    break;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, AS_STRING(*name_val));
                    if (sym && sym->value) {
                        push(vm, makePointer(sym->value, NULL));
                        break;
                    }
                }
                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Global '%s' not found in symbol table.", AS_STRING(*name_val));
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
                if (IS_INTEGER(val_popped)) result_val = makeInt(-AS_INTEGER(val_popped));
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
                push(vm, copyValueForStack(&vm->stackTop[-1]));
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
                        result_val = makeInt(ia & ib);
                    } else if (instruction_val == OR) {
                        result_val = makeInt(ia | ib);
                    } else {
                        result_val = makeInt(ia ^ ib);
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
                        push(vm, makeInt(2147483648LL));
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
                    if (ia == LLONG_MIN && ib == -1) {
                        runtimeError(vm, "Runtime Error: Integer overflow.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeInt(ia / ib));
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
                    push(vm, makeInt(ia % ib));
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
                    if (instruction_val == SHL) {
                        push(vm, makeInt(ia << ib));
                    } else {
                        push(vm, makeInt(ia >> ib));
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib < 0) {
                        runtimeError(vm, "Runtime Error: Shift amount cannot be negative.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (instruction_val == SHL) {
                        push(vm, makeInt(ia << ib));
                    } else {
                        push(vm, makeInt(ia >> ib));
                    }
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

                    if (a_real || b_real) {
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
                        long long ia = asI64(a_val);
                        long long ib = asI64(b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(ia == ib); break;
                            case NOT_EQUAL:     result_val = makeBoolean(ia != ib); break;
                            case GREATER:       result_val = makeBoolean(ia >  ib); break;
                            case GREATER_EQUAL: result_val = makeBoolean(ia >= ib); break;
                            case LESS:          result_val = makeBoolean(ia <  ib); break;
                            case LESS_EQUAL:    result_val = makeBoolean(ia <= ib); break;
                            default: goto comparison_error_label;
                        }
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_STRING(b_val)) || (IS_STRING(a_val) && IS_CHAR(b_val))) {
                    char a_text[5] = {0};
                    char b_text[5] = {0};
                    const char *sa = NULL;
                    const char *sb = NULL;

                    if (IS_STRING(a_val)) {
                        sa = AS_STRING(a_val) ? AS_STRING(a_val) : "";
                    } else if (VALUE_TYPE(a_val) == TYPE_WIDECHAR) {
                        encodeUtf8Codepoint((uint32_t)AS_CHAR(a_val), a_text);
                        sa = a_text;
                    } else {
                        a_text[0] = (char)AS_CHAR(a_val);
                        sa = a_text;
                    }

                    if (IS_STRING(b_val)) {
                        sb = AS_STRING(b_val) ? AS_STRING(b_val) : "";
                    } else if (VALUE_TYPE(b_val) == TYPE_WIDECHAR) {
                        encodeUtf8Codepoint((uint32_t)AS_CHAR(b_val), b_text);
                        sb = b_text;
                    } else {
                        b_text[0] = (char)AS_CHAR(b_val);
                        sb = b_text;
                    }

                    int cmp = strcmp(sa, sb);
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }


                FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                hydrateFieldMetadataFromBaseValue(base_val_ptr, current, field_index);
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makePointer(fieldValueStorage(current), current->type_def));
                break;
            }
            case GET_FIELD_OFFSET16: {
                uint16_t field_index = READ_SHORT(vm);
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = findRecordFieldBySlot(record_struct_ptr, field_index);
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");

                    return INTERPRET_RUNTIME_ERROR;
                }
                hydrateFieldMetadataFromBaseValue(base_val_ptr, current, field_index);
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makePointer(fieldValueStorage(current), current->type_def));
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    Value popped_base_val = pop(vm);
                    freeValue(&popped_base_val);
                    push(vm, makePointer(fieldValueStorage(current), current->type_def));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS16: {
                uint16_t field_name_idx = READ_SHORT(vm);
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    Value popped_base_val = pop(vm);
                    freeValue(&popped_base_val);
                    push(vm, makePointer(fieldValueStorage(current), current->type_def));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS_KEEP: {
                uint8_t field_name_idx = READ_BYTE();
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    push(vm, makePointer(fieldValueStorage(current), current->type_def));
                    goto next_instruction;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS_KEEP16: {
                uint16_t field_name_idx = READ_SHORT(vm);
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
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = findRecordFieldByName(record_struct_ptr, field_name);
                if (current) {
                    push(vm, makePointer(fieldValueStorage(current), current->type_def));
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

                        if (!frontendIsShell() && wants_length) {
                            push(vm, makePointer(base_val, STRING_LENGTH_SENTINEL));
                            freeValue(&operand);
                            break;
                        }

                        push(vm, makePointer(&AS_STRING(*base_val)[char_offset], STRING_CHAR_PTR_SENTINEL));
                        freeValue(&operand);
                        break;
                    }
                }

                int* indices = malloc(sizeof(int) * dimension_count);
                if (!indices) { runtimeError(vm, "VM Error: Malloc failed for array indices."); freeValue(&operand); return INTERPRET_RUNTIME_ERROR; }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (!vmIndexValueToInt(vm, &index_val, &indices[dimension_count - 1 - i])) {
                        free(indices);
                        freeValue(&index_val);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                bool using_wrapper = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (candidate && VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
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
                            if (ARRAY_LOWER_BOUNDS(temp_wrapper)) free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            if (ARRAY_UPPER_BOUNDS(temp_wrapper)) free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                            free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                                free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                                free(indices);
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
                        free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                        free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                AST* element_base_type = NULL;
                if (ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr)) {
                    element_base_type = ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr);
                } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                    element_base_type = PTR_BASE_TYPE_NODE(operand)->right;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makePointer(&AS_ARRAY_RAW(*array_val_ptr)[offset], BYTE_ARRAY_PTR_SENTINEL));
                } else {
                    push(vm, makePointer(&AS_ARRAY(*array_val_ptr)[offset], element_base_type));
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                    free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                }

                break;
            }
            case GET_ELEMENT_ADDRESS_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);
                Value operand = pop(vm);

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

                        push(vm, makePointer(&AS_STRING(*base_val)[flat_offset], STRING_CHAR_PTR_SENTINEL));
                        freeValue(&operand);
                        break;
                    }
                }

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                ARRAY_LOWER_BOUNDS(temp_wrapper) = NULL;
                ARRAY_UPPER_BOUNDS(temp_wrapper) = NULL;
                bool using_wrapper = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (candidate && VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
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
                            if (ARRAY_LOWER_BOUNDS(temp_wrapper)) free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            if (ARRAY_UPPER_BOUNDS(temp_wrapper)) free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                                free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (VALUE_TYPE(operand) == TYPE_POINTER) {
                        freeValue(&operand);
                    }
                    if (using_wrapper) {
                        free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                        free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                AST* element_base_type = NULL;
                if (ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr)) {
                    element_base_type = ARRAY_ELEMENT_TYPE_DEF(*array_val_ptr);
                } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                    element_base_type = PTR_BASE_TYPE_NODE(operand)->right;
                }

                if (ARRAY_IS_PACKED(*array_val_ptr)) {
                    if (!AS_ARRAY_RAW(*array_val_ptr)) {
                        runtimeError(vm, "VM Error: Packed array storage missing.");
                        freeValue(&operand);
                        if (using_wrapper) {
                            free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makePointer(&AS_ARRAY_RAW(*array_val_ptr)[flat_offset], BYTE_ARRAY_PTR_SENTINEL));
                } else {
                    push(vm, makePointer(&AS_ARRAY(*array_val_ptr)[flat_offset], element_base_type));
                }

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                    free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                }

                break;
            }
            case LOAD_ELEMENT_VALUE: {
                uint8_t dimension_count = READ_BYTE();

                Value operand = pop(vm);

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

                        if (!frontendIsShell() && wants_length) {
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

                int* indices = malloc(sizeof(int) * dimension_count);
                if (!indices) {
                    runtimeError(vm, "VM Error: Malloc failed for array indices.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (!vmIndexValueToInt(vm, &index_val, &indices[dimension_count - 1 - i])) {
                        free(indices);
                        freeValue(&index_val);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value shared_snapshot = makeNil();
                Value temp_wrapper;
                ARRAY_LOWER_BOUNDS(temp_wrapper) = NULL;
                ARRAY_UPPER_BOUNDS(temp_wrapper) = NULL;
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (candidate && VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        if (ARRAY_IS_DYNAMIC(*candidate)) {
                            shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
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
                            if (ARRAY_LOWER_BOUNDS(temp_wrapper)) free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            if (ARRAY_UPPER_BOUNDS(temp_wrapper)) free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                            free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                                free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                                free(indices);
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
                        free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (VALUE_TYPE(operand) == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (VALUE_TYPE(operand) == TYPE_POINTER) freeValue(&operand);
                    if (using_wrapper) {
                        free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                        free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                            free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                    free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                    free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                }
                if (using_snapshot) {
                    freeValue(&shared_snapshot);
                }

                break;
            }
            case LOAD_ELEMENT_VALUE_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);

                Value operand = pop(vm);

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
                Value temp_wrapper;
                ARRAY_LOWER_BOUNDS(temp_wrapper) = NULL;
                ARRAY_UPPER_BOUNDS(temp_wrapper) = NULL;
                bool using_wrapper = false;
                bool using_snapshot = false;

                if (VALUE_TYPE(operand) == TYPE_POINTER) {
                    Value* candidate = (Value*)AS_POINTER(operand);
                    if (candidate && VALUE_TYPE(*candidate) == TYPE_ARRAY) {
                        if (ARRAY_IS_DYNAMIC(*candidate)) {
                            shared_snapshot = copyDynamicArraySnapshotValue(candidate);
                            array_val_ptr = &shared_snapshot;
                            using_snapshot = true;
                        } else {
                            array_val_ptr = candidate;
                        }
                    } else if (PTR_BASE_TYPE_NODE(operand) && PTR_BASE_TYPE_NODE(operand)->type == AST_ARRAY_TYPE) {
                        AST* arrayType = PTR_BASE_TYPE_NODE(operand);
                        int dims = arrayType->child_count;
                        SET_VALUE_TYPE(&temp_wrapper, TYPE_ARRAY);
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
                            if (ARRAY_LOWER_BOUNDS(temp_wrapper)) free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            if (ARRAY_UPPER_BOUNDS(temp_wrapper)) free(ARRAY_UPPER_BOUNDS(temp_wrapper));
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && !vmResolveArraySubrangeBounds(vm, sub, &lb, &ub)) {
                                free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                                free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                        free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                            free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                            free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                    free(ARRAY_LOWER_BOUNDS(temp_wrapper));
                    free(ARRAY_UPPER_BOUNDS(temp_wrapper));
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
                    char* char_target_addr = (char*)AS_POINTER(pointer_to_lvalue);
                    if (char_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to assign to a NULL character address.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                        *char_target_addr = AS_CHAR(value_to_set);
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
                    if (!frontendIsShell()) {
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

                    AST* declaredTypeNode = PTR_BASE_TYPE_NODE(pointer_to_lvalue);
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
                    if (isPascalStringType(VALUE_TYPE(*target_lvalue_ptr)) && STRING_MAX_LENGTH(*target_lvalue_ptr) <= 0) {
                        if (VALUE_TYPE(value_to_set) == TYPE_CHAR || VALUE_TYPE(value_to_set) == TYPE_WIDECHAR) {
                            freeValue(target_lvalue_ptr);
                            char encoded[5] = {0};
                            size_t encoded_len = 0;
                            if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                                encoded[0] = (char)AS_CHAR(value_to_set);
                                encoded_len = 1;
                            } else {
                                encoded_len = encodeUtf8Codepoint((uint32_t)AS_CHAR(value_to_set), encoded);
                            }
                            AS_STRING(*target_lvalue_ptr) = (char*)malloc(encoded_len + 1);
                            if (!AS_STRING(*target_lvalue_ptr)) {
                                runtimeError(vm, "VM Error: Malloc failed for CHAR/WIDECHAR to string assignment.");
                            } else {
                                memcpy(AS_STRING(*target_lvalue_ptr), encoded, encoded_len);
                                AS_STRING(*target_lvalue_ptr)[encoded_len] = '\0';
                            }
                            SET_VALUE_TYPE(target_lvalue_ptr,
                                           VALUE_TYPE(*target_lvalue_ptr) == TYPE_UNICODE_STRING
                                               ? TYPE_UNICODE_STRING
                                               : TYPE_STRING);
                            STRING_MAX_LENGTH(*target_lvalue_ptr) = -1;
                        } else if (isPascalStringType(VALUE_TYPE(value_to_set)) && AS_STRING(value_to_set)) {
                            VarType destType = VALUE_TYPE(*target_lvalue_ptr);
                            freeValue(target_lvalue_ptr);
                            /* Optimization: Steal buffer from temporary value on stack */
                            AS_STRING(*target_lvalue_ptr) = AS_STRING(value_to_set);
                            AS_STRING(value_to_set) = NULL; /* Prevent double-free */
                            SET_VALUE_TYPE(target_lvalue_ptr, destType);
                            STRING_MAX_LENGTH(*target_lvalue_ptr) = -1;
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign this type to a dynamic string.");
                        }
                    }
                    else if (VALUE_TYPE(*target_lvalue_ptr) == TYPE_STRING && STRING_MAX_LENGTH(*target_lvalue_ptr) > 0) {
                        if (VALUE_TYPE(value_to_set) == TYPE_STRING && AS_STRING(value_to_set)) {
                            strncpy(AS_STRING(*target_lvalue_ptr), AS_STRING(value_to_set), STRING_MAX_LENGTH(*target_lvalue_ptr));
                            AS_STRING(*target_lvalue_ptr)[STRING_MAX_LENGTH(*target_lvalue_ptr)] = '\0'; // Ensure null termination
                        } else if (VALUE_TYPE(value_to_set) == TYPE_CHAR) {
                            AS_STRING(*target_lvalue_ptr)[0] = AS_CHAR(value_to_set);
                            AS_STRING(*target_lvalue_ptr)[1] = '\0';
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
                    else {
                        AST* preserved_base = PTR_BASE_TYPE_NODE(*target_lvalue_ptr);
                        if (VALUE_TYPE(value_to_set) == TYPE_MEMORYSTREAM) {
                            /* Transfer ownership of the MStream pointer without freeing it
                             * when the temporary value is cleaned up below. */
                            Value replacement = value_to_set;
                            replaceValueCell(target_lvalue_ptr, replacement, preserved_base);
                            AS_MSTREAM(value_to_set) = NULL;
                        } else {
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
                if (!vmBuildSetFromRange(&set_value, start_ord, end_ord)) {
                    runtimeError(vm, "VM Error: Failed to allocate set range.");
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
                    char* char_target_addr = (char*)AS_POINTER(pointer_val);
                    if (char_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to dereference a NULL character address.");
                        freeValue(&pointer_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeChar(*char_target_addr));
                } else if (PTR_BASE_TYPE_NODE(pointer_val) == BYTE_ARRAY_PTR_SENTINEL) {
                    uint8_t* byte_target_addr = (uint8_t*)AS_POINTER(pointer_val);
                    if (byte_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to dereference a NULL byte address.");
                        freeValue(&pointer_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeByte(*byte_target_addr));
                } else if (PTR_BASE_TYPE_NODE(pointer_val) == STRING_LENGTH_SENTINEL && !frontendIsShell()) {
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
                     long long expected_index = frontendIsShell() ? 0 : 1;
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
                    long long expected_index = frontendIsShell() ? 0 : 1;
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
                         long long expected_index = frontendIsShell() ? 0 : 1;
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
            case DEFINE_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case DEFINE_GLOBAL16: {
                Value varNameVal = READ_CONSTANT16();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case GET_GLOBAL: {
                uint8_t* instruction_start = vm->lastInstruction;
                uint8_t name_idx = READ_BYTE();
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    push(vm, vmLoadThreadMyselfCopy(vm));
                    break;
                }

                Symbol* sym = vmGetCachedGlobalSymbol(vm->chunk, name_idx);
                if (!sym || !sym->value) {
                    Symbol* resolved = NULL;
                    bool locked = false;
                    if (vm->vmConstGlobalSymbols) {
                        resolved = hashTableLookup(vm->vmConstGlobalSymbols, AS_STRING(*name_val));
                    }
                    if (!resolved) {
                        pthread_mutex_lock(&globals_mutex);
                        locked = true;
                        resolved = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                        if (!resolved || !resolved->value) {
                            pthread_mutex_unlock(&globals_mutex);
                            runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", AS_STRING(*name_val));
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    if (!locked) pthread_mutex_lock(&globals_mutex);
                    vmCacheGlobalSymbol(vm->chunk, name_idx, resolved);
                    pthread_mutex_unlock(&globals_mutex);
                    sym = resolved;
                }

                if (!gTextAttrInitialized && AS_STRING(*name_val) &&
                    (strcasecmp(AS_STRING(*name_val), "CRT.TextAttr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "TextAttr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "crt.textattr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "textattr") == 0)) {
                    gTextAttrInitialized = true;
                    SET_INT_VALUE(sym->value, 7);
                }

                push(vm, copyValueForStack(sym->value));
                vmInlineCacheWriteSymbol(cache_slot, sym);
                vmPatchGlobalOpcode(instruction_start, false, false);
                break;
            }
            case GET_GLOBAL16: {
                uint8_t* instruction_start = vm->lastInstruction;
                uint16_t name_idx = READ_SHORT(vm);
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    push(vm, vmLoadThreadMyselfCopy(vm));
                    break;
                }

                Symbol* sym = vmGetCachedGlobalSymbol(vm->chunk, name_idx);
                if (!sym || !sym->value) {
                    Symbol* resolved = NULL;
                    bool locked = false;
                    if (vm->vmConstGlobalSymbols) {
                        resolved = hashTableLookup(vm->vmConstGlobalSymbols, AS_STRING(*name_val));
                    }
                    if (!resolved) {
                        pthread_mutex_lock(&globals_mutex);
                        locked = true;
                        resolved = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                        if (!resolved || !resolved->value) {
                            pthread_mutex_unlock(&globals_mutex);
                            runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", AS_STRING(*name_val));
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    if (!locked) pthread_mutex_lock(&globals_mutex);
                    vmCacheGlobalSymbol(vm->chunk, name_idx, resolved);
                    pthread_mutex_unlock(&globals_mutex);
                    sym = resolved;
                }

                if (!gTextAttrInitialized && AS_STRING(*name_val) &&
                    (strcasecmp(AS_STRING(*name_val), "CRT.TextAttr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "TextAttr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "crt.textattr") == 0 ||
                     strcasecmp(AS_STRING(*name_val), "textattr") == 0)) {
                    gTextAttrInitialized = true;
                    SET_INT_VALUE(sym->value, 7);
                }

                push(vm, copyValueForStack(sym->value));
                vmInlineCacheWriteSymbol(cache_slot, sym);
                vmPatchGlobalOpcode(instruction_start, false, true);
                break;
            }
            case GET_GLOBAL_CACHED: {
                READ_BYTE();
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                Symbol* sym = vmInlineCacheReadSymbol(cache_slot);
                if (!sym || !sym->value) {
                    runtimeError(vm, "VM Error: Cached global unavailable in GET_GLOBAL_CACHED.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(sym->name)) {
                    push(vm, vmLoadThreadMyselfCopy(vm));
                } else {
                    push(vm, copyValueForStack(sym->value));
                }
                break;
            }
            case GET_GLOBAL16_CACHED: {
                READ_SHORT(vm);
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                Symbol* sym = vmInlineCacheReadSymbol(cache_slot);
                if (!sym || !sym->value) {
                    runtimeError(vm, "VM Error: Cached global unavailable in GET_GLOBAL16_CACHED.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(sym->name)) {
                    push(vm, vmLoadThreadMyselfCopy(vm));
                } else {
                    push(vm, copyValueForStack(sym->value));
                }
                break;
            }
            case SET_GLOBAL: {
                uint8_t* instruction_start = vm->lastInstruction;
                uint8_t name_idx = READ_BYTE();
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for SET_GLOBAL.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global variable name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmPasExceptionPending(vm) &&
                    !vmNameIsPasExceptionGlobal(AS_STRING(*name_val)) &&
                    !vmNameIsMyself(AS_STRING(*name_val))) {
                    Value skipped_value = pop(vm);
                    freeValue(&skipped_value);
                    break;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    Value value_from_stack = pop(vm);
                    vmStoreThreadMyself(vm, value_from_stack);
                    break;
                }

                pthread_mutex_lock(&globals_mutex);
                Symbol* sym = vmGetCachedGlobalSymbol(vm->chunk, name_idx);
                if (!sym) {
                    sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                    if (!sym) {
                        pthread_mutex_unlock(&globals_mutex);
                        runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", AS_STRING(*name_val));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vmCacheGlobalSymbol(vm->chunk, name_idx, sym);
                }

                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GLOBAL.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }

                Value value_from_stack = pop(vm);
                updateSymbolDirect(sym, AS_STRING(*name_val), value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                vmInlineCacheWriteSymbol(cache_slot, sym);
                vmPatchGlobalOpcode(instruction_start, true, false);
                break;
            }
            case SET_GLOBAL16: {
                uint8_t* instruction_start = vm->lastInstruction;
                uint16_t name_idx = READ_SHORT(vm);
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for SET_GLOBAL16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "Runtime Error: Invalid global variable name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmPasExceptionPending(vm) &&
                    !vmNameIsPasExceptionGlobal(AS_STRING(*name_val)) &&
                    !vmNameIsMyself(AS_STRING(*name_val))) {
                    Value skipped_value = pop(vm);
                    freeValue(&skipped_value);
                    break;
                }
                if (vmNameIsMyself(AS_STRING(*name_val))) {
                    Value value_from_stack = pop(vm);
                    vmStoreThreadMyself(vm, value_from_stack);
                    break;
                }

                pthread_mutex_lock(&globals_mutex);
                Symbol* sym = vmGetCachedGlobalSymbol(vm->chunk, name_idx);
                if (!sym) {
                    sym = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(*name_val));
                    if (!sym) {
                        pthread_mutex_unlock(&globals_mutex);
                        runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", AS_STRING(*name_val));
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    vmCacheGlobalSymbol(vm->chunk, name_idx, sym);
                }

                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GLOBAL16.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }

                Value value_from_stack = pop(vm);
                updateSymbolDirect(sym, AS_STRING(*name_val), value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                vmInlineCacheWriteSymbol(cache_slot, sym);
                vmPatchGlobalOpcode(instruction_start, true, true);
                break;
            }
            case SET_GLOBAL_CACHED: {
                READ_BYTE();
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                Symbol* sym = vmInlineCacheReadSymbol(cache_slot);
                if (!sym) {
                    runtimeError(vm, "VM Error: Cached symbol missing for SET_GLOBAL_CACHED.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(sym->name)) {
                    Value value_from_stack = pop(vm);
                    vmStoreThreadMyself(vm, value_from_stack);
                    break;
                }
                pthread_mutex_lock(&globals_mutex);
                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        pthread_mutex_unlock(&globals_mutex);
                        runtimeError(vm, "VM Error: Malloc failed for cached symbol value in SET_GLOBAL.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }
                Value value_from_stack = pop(vm);
                updateSymbolDirect(sym, sym->name, value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                break;
            }
            case SET_GLOBAL16_CACHED: {
                READ_SHORT(vm);
                uint8_t* cache_slot = vm->ip;
                vm->ip += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                Symbol* sym = vmInlineCacheReadSymbol(cache_slot);
                if (!sym) {
                    runtimeError(vm, "VM Error: Cached symbol missing for SET_GLOBAL16_CACHED.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vmNameIsMyself(sym->name)) {
                    Value value_from_stack = pop(vm);
                    vmStoreThreadMyself(vm, value_from_stack);
                    break;
                }
                pthread_mutex_lock(&globals_mutex);
                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        pthread_mutex_unlock(&globals_mutex);
                        runtimeError(vm, "VM Error: Malloc failed for cached symbol value in SET_GLOBAL16.");
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
                    AS_STRING(*target_slot) = strdup(source_str);
                    if (!AS_STRING(*target_slot)) {
                        runtimeError(vm, "VM Error: strdup failed for dynamic string assignment.");
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    SET_VALUE_TYPE(target_slot, destType);
                    STRING_MAX_LENGTH(*target_slot) = -1;
                } else if (VALUE_TYPE(*target_slot) == TYPE_STRING && STRING_MAX_LENGTH(*target_slot) > 0) {
                    // Special case: Assignment to a fixed-length string.
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
                } else {
                    // This is the logic for all other types, including dynamic strings,
                    // numbers, records, etc., which requires a deep copy.
                    AST* preserved_base = PTR_BASE_TYPE_NODE(*target_slot);
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
                } else {
                    AST* preserved_base = PTR_BASE_TYPE_NODE(*target_slot);
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
                            lower_bounds[i] = 0;
                            upper_bounds[i] = (int)VAL_INT(size_val) - 1;
                            freeValue(&size_val);
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
                            lower_bounds[i] = (int)VAL_INT(lower_val);
                            upper_bounds[i] = (int)VAL_INT(upper_val);
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
                            lower_bounds[i] = 0;
                            upper_bounds[i] = (int)VAL_INT(size_val) - 1;
                            freeValue(&size_val);
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
                            lower_bounds[i] = (int)VAL_INT(lower_val);
                            upper_bounds[i] = (int)VAL_INT(upper_val);
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
                    ARRAY_ELEMENT_TYPE(file_val) = element_type;
                    ARRAY_ELEMENT_TYPE_DEF(file_val) = element_type_def;
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
                        // Build a simple base type node from the provided name
                        Token* baseTok = newToken(TOKEN_IDENTIFIER, AS_STRING(type_name_val), 0, 0);
                        type_def = newASTNode(AST_VARIABLE, baseTok);
                        const char* tn = AS_STRING(type_name_val);
                        if      (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) setTypeAST(type_def, TYPE_INT32);
                        else if (strcasecmp(tn, "real")    == 0 || strcasecmp(tn, "double") == 0) setTypeAST(type_def, TYPE_DOUBLE);
                        else if (strcasecmp(tn, "single")  == 0 || strcasecmp(tn, "float")  == 0) setTypeAST(type_def, TYPE_FLOAT);
                        else if (strcasecmp(tn, "char")    == 0) setTypeAST(type_def, TYPE_CHAR);
                        else if (strcasecmp(tn, "widechar")== 0) setTypeAST(type_def, TYPE_WIDECHAR);
                        else if (strcasecmp(tn, "unicodestring")== 0) setTypeAST(type_def, TYPE_UNICODE_STRING);
                        else if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) setTypeAST(type_def, TYPE_BOOLEAN);
                        else if (strcasecmp(tn, "byte")    == 0) setTypeAST(type_def, TYPE_BYTE);
                        else if (strcasecmp(tn, "word")    == 0) setTypeAST(type_def, TYPE_WORD);
                        else if (strcasecmp(tn, "int64")   == 0 || strcasecmp(tn, "longint") == 0) setTypeAST(type_def, TYPE_INT64);
                        else if (strcasecmp(tn, "cardinal")== 0) setTypeAST(type_def, TYPE_UINT32);
                        else setTypeAST(type_def, TYPE_VOID);
                        freeToken(baseTok);
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
                STRING_MAX_LENGTH(*target_slot) = length;
                AS_STRING(*target_slot) = (char*)calloc(length + 1, 1);
                if (!AS_STRING(*target_slot)) {
                    runtimeError(vm, "VM Error: Malloc failed for fixed-length string initialization.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case JUMP_IF_FALSE: {
                uint16_t offset_val = READ_SHORT(vm);
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
                    vm->ip += (int16_t)offset_val;
                }
                break;
            }
            case JUMP: {
                uint16_t offset = READ_SHORT(vm);
                vm->ip += (int16_t)offset;
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
                        if (VALUE_TYPE(args[i]) == TYPE_POINTER || VALUE_TYPE(args[i]) == TYPE_FILE) {
                            continue;
                        }
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

                Value result = handler(vm, arg_count, args);

                if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                vm->current_builtin_name = previous_builtin_name;

                vm->stackTop -= arg_count;
                for (int i = 0; i < arg_count; i++) {
                    if (VALUE_TYPE(args[i]) == TYPE_POINTER || VALUE_TYPE(args[i]) == TYPE_FILE) {
                        continue;
                    }
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
                    mapped_builtin_id = vm->chunk->builtin_resolved_ids[name_const_idx];
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
                        vm->chunk->builtin_resolved_ids[name_const_idx] = mapped_builtin_id;
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

                    Value result = handler(vm, arg_count, args);

                    if (needs_lock) pthread_mutex_unlock(&globals_mutex);
                    vm->current_builtin_name = previous_builtin_name;

                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        if (VALUE_TYPE(args[i]) == TYPE_POINTER || VALUE_TYPE(args[i]) == TYPE_FILE) {
                            continue;
                        }
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
                        if (VALUE_TYPE(args[i]) == TYPE_POINTER || VALUE_TYPE(args[i]) == TYPE_FILE) {
                            continue;
                        }
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
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t name_index = READ_SHORT(vm);
                uint8_t declared_arity = READ_BYTE();

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %u, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!vm->chunk || name_index >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Invalid procedure name index %u for CALL_USER_PROC.", name_index);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_index];
                if (VALUE_TYPE(*name_val) != TYPE_STRING || !AS_STRING(*name_val)) {
                    runtimeError(vm, "VM Error: CALL_USER_PROC requires string constant for callee name (index %u).", name_index);
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* proc_name = AS_STRING(*name_val);
                char lookup_name[MAX_SYMBOL_LENGTH + 1];
                strncpy(lookup_name, proc_name, MAX_SYMBOL_LENGTH);
                lookup_name[MAX_SYMBOL_LENGTH] = '\0';
                toLowerString(lookup_name);

                if (!vm->procedureTable) {
                    runtimeError(vm, "VM Error: Procedure table not initialized when calling '%s'.", proc_name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* proc_symbol = findProcedureByName(vm->procedureTable, lookup_name, vm);
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: Procedure '%s' not found for CALL_USER_PROC.", proc_name);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!proc_symbol->is_defined || proc_symbol->bytecode_address < 0) {
                    runtimeError(vm, "VM Error: Procedure '%s' has no compiled body.",
                                 proc_symbol->name ? proc_symbol->name : proc_name);
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
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Operands: name_idx (2 bytes), target_address (2 bytes), declared_arity (1 byte)
                uint16_t name_idx_ignored = READ_SHORT(vm); // Read and discard the name index
                (void)name_idx_ignored; // Suppress unused variable warning
                uint16_t target_address = READ_SHORT(vm);
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
                    runtimeError(vm, "VM Error: Could not retrieve procedure symbol for called address %04d.", target_address);
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
                uint16_t target_address = 0;

                if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
                    target_address = (uint16_t)AS_CLOSURE(addrVal).entry_offset;
                    captured_env = AS_CLOSURE(addrVal).env;
                    if (captured_env) {
                        retainClosureEnv(captured_env);
                    }
                    proc_symbol = AS_CLOSURE(addrVal).symbol;
                } else if (IS_INTLIKE(addrVal)) {
                    target_address = (uint16_t)AS_INTEGER(addrVal);
                } else {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires procedure pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&addrVal);

                if (vm->frameCount >= VM_CALL_STACK_MAX) {
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
                    runtimeError(vm, "VM Error: No procedure found at address %04d for indirect call.", target_address);
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
                Value* vtable_arr = NULL;
                while (current) {
                    if (strcmp(current->name, "__vtable") == 0) {
                        const Value *fieldValue = fieldValueStorageConst(current);
                        if (VALUE_TYPE(*fieldValue) == TYPE_ARRAY) {
                            vtable_arr = AS_ARRAY(*fieldValue);
                        }
                        break;
                    }
                    current = current->next;
                }

                if (!vtable_arr) {
                    runtimeError(vm, "VM Error: Object missing V-table.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t target_address = (uint16_t)VAL_UINT(vtable_arr[method_index]);
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity - 1;
                frame->vtable = vtable_arr;
                frame->slotCount = 0;

                Symbol* method_symbol = NULL;
                const char* className = NULL;
                if (PTR_BASE_TYPE_NODE(*objVal) && PTR_BASE_TYPE_NODE(*objVal)->token) {
                    className = PTR_BASE_TYPE_NODE(*objVal)->token->value;
                }
                if (className) {
                    method_symbol = vmFindClassMethod(vm, className, method_index);
                }
                if (!method_symbol) {
                    method_symbol = vmGetProcedureByAddress(vm, target_address);
                }
                if (!method_symbol) {
                    runtimeError(vm, "VM Error: Method not found for index %d.", method_index);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

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
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", method_symbol->name);
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
                uint16_t target_address = 0;

                if (VALUE_TYPE(addrVal) == TYPE_CLOSURE) {
                    target_address = (uint16_t)AS_CLOSURE(addrVal).entry_offset;
                    captured_env = AS_CLOSURE(addrVal).env;
                    if (captured_env) {
                        retainClosureEnv(captured_env);
                    }
                    proc_symbol = AS_CLOSURE(addrVal).symbol;
                } else if (IS_INTLIKE(addrVal)) {
                    target_address = (uint16_t)AS_INTEGER(addrVal);
                } else {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires procedure pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&addrVal);

                if (vm->frameCount >= VM_CALL_STACK_MAX) {
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
                    runtimeError(vm, "VM Error: No procedure found at address %04d for indirect call.", target_address);
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
                uint16_t entry = READ_SHORT(vm);
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
                if (!vmThreadTakeResult(vm, tid, NULL, true, NULL, true)) {
                    joinThreadInternal(vm, tid);
                }
                Value popped_tid = pop(vm);
                freeValue(&popped_tid);
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

                char buf[DEFAULT_STRING_CAPACITY];
                buf[0] = '\0';

                if (isRealType(VALUE_TYPE(raw_val))) {
                    long double rv = AS_REAL(raw_val);
                    int width_int = width;
                    if (precision >= 0) {
                        snprintf(buf, sizeof(buf), "%*.*Lf", width_int, precision, rv);
                    } else {
                        snprintf(buf, sizeof(buf), "%*.*LE", width_int,
                                 (int)PASCAL_DEFAULT_FLOAT_PRECISION, rv);
                    }
                } else if (VALUE_TYPE(raw_val) == TYPE_CHAR) {
                    snprintf(buf, sizeof(buf), "%*c", width, AS_CHAR(raw_val));
                } else if (VALUE_TYPE(raw_val) == TYPE_BOOLEAN) {
                    const char* bool_str = VAL_INT(raw_val) ? "TRUE" : "FALSE";
                    snprintf(buf, sizeof(buf), "%*s", width, bool_str);
                } else if (isIntlikeType(VALUE_TYPE(raw_val))) {
                    if (VALUE_TYPE(raw_val) == TYPE_UINT64 || VALUE_TYPE(raw_val) == TYPE_UINT32 ||
                        VALUE_TYPE(raw_val) == TYPE_UINT16 || VALUE_TYPE(raw_val) == TYPE_UINT8 ||
                        VALUE_TYPE(raw_val) == TYPE_WORD   || VALUE_TYPE(raw_val) == TYPE_BYTE) {
                        unsigned long long u = VAL_UINT(raw_val);
                        if (VALUE_TYPE(raw_val) == TYPE_BYTE || VALUE_TYPE(raw_val) == TYPE_UINT8)   u &= 0xFFULL;
                        if (VALUE_TYPE(raw_val) == TYPE_WORD || VALUE_TYPE(raw_val) == TYPE_UINT16) u &= 0xFFFFULL;
                        if (VALUE_TYPE(raw_val) == TYPE_UINT32) u &= 0xFFFFFFFFULL;
                        snprintf(buf, sizeof(buf), "%*llu", width, u);
                    } else {
                        long long s = VAL_INT(raw_val);
                        if (VALUE_TYPE(raw_val) == TYPE_INT8)  s = (int8_t)s;
                        if (VALUE_TYPE(raw_val) == TYPE_INT16) s = (int16_t)s;
                        snprintf(buf, sizeof(buf), "%*lld", width, s);
                    }
                } else if (VALUE_TYPE(raw_val) == TYPE_STRING) {
                    const char* source_str = AS_STRING(raw_val) ? AS_STRING(raw_val) : "";
                    size_t len = strlen(source_str);
                    int prec = (width > 0 && (size_t)width < len) ? width : (int)len;
                    snprintf(buf, sizeof(buf), "%*.*s", width, prec, source_str);
                } else {
                    snprintf(buf, sizeof(buf), "%*s", width, "?");
                }

                freeValue(&raw_val);

                push(vm, makeString(buf));
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
