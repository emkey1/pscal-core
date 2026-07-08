//
//  vm.h
//  Pscal
//
//  Created by Michael Miller on 5/19/25.
//
#ifndef PSCAL_VM_H
#define PSCAL_VM_H

#include "compiler/bytecode.h" // For BytecodeChunk and Value (via its include of types.h)
#include "core/types.h"        // For Value explicitly, though bytecode.h should bring it in
#include "symbol/symbol.h"     // For HashTable, if VM manages globals using it directly
#include <stdbool.h>
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/resource.h>
#include <stdatomic.h>

// --- VM Configuration ---
// VM_STACK_MAX / VM_CALL_STACK_MAX are *default ceilings* (VM 2.0 Phase 3,
// plan Docs/pscal_vm2_plan.md §5.9), not fixed inline array sizes: the
// operand stack and call-frame array each start small and grow on demand,
// up to these ceilings. Override at runtime via PSCAL_VM_MAX_STACK_VALUES /
// PSCAL_VM_MAX_CALL_FRAMES (read once via pscalVmStackCeilingValues() /
// pscalVmCallFrameCeiling(), below) so the verifier and the runtime always
// agree on the same bound.
#define VM_STACK_MAX 1048576         // default ceiling: max Values on the operand stack
#define VM_STACK_INITIAL_VALUES 4096 // initial committed operand-stack segment
#define VM_GLOBALS_MAX 4096     // Maximum number of global variables (for simple array storage)

#define MAX_HOST_FUNCTIONS 4096

#define VM_CALL_STACK_MAX 131072            // default ceiling: max active call frames
#define VM_CALL_FRAME_INITIAL_CAPACITY 256  // initial frames[] capacity

// VM 2.0 Phase 5a checkpoint 5a-ii (Docs/pscal_vm2_plan.md Sec 6.1):
// VM_MAX_THREADS / VM_MAX_MUTEXES are *default ceilings* now, matching
// VM_STACK_MAX/VM_CALL_STACK_MAX's convention above -- threads[]/mutexes[]
// each start committed at the INITIAL count (matching this codebase's exact
// pre-5a-ii behavior, 16/64) and grow toward these ceilings on demand, via
// mmap-reservation + mprotect-extend (the operand stack's technique, NOT
// frames[]'s realloc technique -- confirmed necessary, not assumed: several
// call sites here (lockMutex, joinThreadInternal, vmThreadTakeResult) take a
// raw Thread*/Mutex* pointer, release the owning registry lock, and then
// BLOCK on it for an arbitrary duration (pthread_mutex_lock/
// pthread_cond_timedwait) -- a realloc that relocated the array mid-wait
// would leave that pointer dangling). Override at runtime via
// PSCAL_VM_MAX_THREADS / PSCAL_VM_MAX_MUTEXES (read once via
// pscalVmThreadsCeiling() / pscalVmMutexesCeiling(), below).
#define VM_MAX_THREADS 4096            // default ceiling: max concurrent thread-pool slots
#define VM_THREADS_INITIAL_COUNT 16    // initial committed thread-pool slots
#define VM_MAX_MUTEXES 65536           // default ceiling: max concurrent mutex slots
#define VM_MUTEXES_INITIAL_COUNT 64    // initial committed mutex slots

// Effective ceilings (VM_STACK_MAX / VM_CALL_STACK_MAX / VM_MAX_THREADS /
// VM_MAX_MUTEXES unless overridden by PSCAL_VM_MAX_STACK_VALUES /
// PSCAL_VM_MAX_CALL_FRAMES / PSCAL_VM_MAX_THREADS / PSCAL_VM_MAX_MUTEXES);
// read once per process.
size_t pscalVmStackCeilingValues(void);
size_t pscalVmCallFrameCeiling(void);
size_t pscalVmThreadsCeiling(void);
size_t pscalVmMutexesCeiling(void);

#ifndef THREAD_NAME_MAX
#define THREAD_NAME_MAX 64
#endif

// Flags for the VM write/writeln builtin.
#define VM_WRITE_FLAG_NEWLINE           0x1
#define VM_WRITE_FLAG_SUPPRESS_SPACING  0x2

// Forward declaration
struct VM_s;

// Host function pointer type
typedef Value (*HostFn)(struct VM_s* vm);

// Enum to identify specific host functions
typedef enum {
    HOST_FN_QUIT_REQUESTED,
    HOST_FN_CREATE_THREAD_ADDR,
    HOST_FN_WAIT_THREAD,
    HOST_FN_PRINTF,
    HOST_FN_SHELL_LAST_STATUS,
    HOST_FN_SHELL_LOOP_CHECK_CONDITION,
    HOST_FN_SHELL_LOOP_CHECK_BODY,
    HOST_FN_SHELL_LOOP_EXEC_BODY,
    HOST_FN_SHELL_LOOP_ADVANCE,
    HOST_FN_SHELL_POLL_JOBS,
    HOST_FN_SHELL_LOOP_IS_READY,
    HOST_FN_CREATE_CLOSURE,
    HOST_FN_BOX_INTERFACE,
    HOST_FN_INTERFACE_LOOKUP,
    HOST_FN_INTERFACE_IS,
    HOST_FN_INTERFACE_ASSERT,
    // ... add other host function IDs here ...
    HOST_FN_COUNT
} HostFunctionID;

// --- Interpret Result ---
typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR, // Should be caught before VM runs
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

typedef struct {
    uint8_t* return_address;    // IP in the caller to return to
    Value* slots;               // Pointer to this frame's window on the VM value stack
    Symbol* function_symbol;    // Pointer to the Symbol of the function being called (for arity/locals_count)
                                // Note: Storing Symbol* is one way; alternatively, CALL could carry locals_count,
                                // or RETURN could be generic if stack is always reset to frame->slots.
    uint16_t slotCount;         // Total slots (arguments + locals) reserved for this frame
    uint8_t locals_count;       // Number of local variables (excluding params)
    uint8_t upvalue_count;
    Value** upvalues;
    bool owns_upvalues;
    ClosureEnvPayload* closureEnv;
    bool discard_result_on_return; // If true, drop any function result on return
    Value* vtable;               // Reference to class V-table when executing a method
} CallFrame;

typedef struct ThreadJob ThreadJob;

typedef struct {
    bool valid;
    struct timespec cpuTime;    // CPU time reading for the worker
    struct rusage usage;        // Resource usage snapshot
    size_t rssBytes;            // Resident set size snapshot (bytes)
} ThreadMetricsSample;

typedef struct {
    ThreadMetricsSample start;
    ThreadMetricsSample end;
} ThreadMetrics;

typedef struct {
    uintptr_t vm_address;
    uintptr_t thread_owner_address;
    uintptr_t frontend_context_address;
    uintptr_t chunk_address;
    uintptr_t globals_address;
    uintptr_t const_globals_address;
    uintptr_t procedures_address;
    uintptr_t mutex_owner_address;
    int thread_id;
    int thread_count;
    int worker_count;
    int available_workers;
    int mutex_count;
    int frame_count;
    int trace_head_instructions;
    int trace_executed;
    int chunk_bytecode_count;
    size_t stack_depth;
    size_t global_symbol_count;
    size_t const_symbol_count;
    size_t procedure_symbol_count;
    bool is_root_vm;
    bool has_job_queue;
    bool shell_indexing;
    bool exit_requested;
    bool abort_requested;
    bool suspend_unwind_requested;
} VMProcSnapshot;

typedef struct {
    int slot_id;
    uintptr_t vm_address;
    bool in_pool;
    bool active;
    bool idle;
    bool owns_vm;
    bool pool_worker;
    bool awaiting_reuse;
    bool ready_for_reuse;
    bool status_ready;
    bool result_ready;
    bool paused;
    bool cancel_requested;
    bool kill_requested;
    int pool_generation;
    char name[THREAD_NAME_MAX];
    struct timespec queued_at;
    struct timespec started_at;
    struct timespec finished_at;
    ThreadMetrics metrics;
} VMProcWorkerSnapshot;

typedef struct {
    pthread_t handle;           // OS-level thread handle
    struct VM_s* vm;            // Pointer to the VM executing on this thread
    atomic_bool active;         // Whether this thread is running -- read
                                 // cross-thread without a consistent lock
                                 // domain (freeVM vs. threadStart vs.
                                 // joinThreadInternal all disagreed on which
                                 // mutex, if any, guarded it; TSan-confirmed
                                 // race, Docs/pscal_vm2_plan.md Phase 5a
                                 // prerequisite fix), so it joins paused/
                                 // cancelRequested/killRequested as an atomic
                                 // flag rather than gaining a fourth lock.

    // Result hand-off state for builtin jobs.
    bool statusReady;           // True when a worker has published a status/result
    bool statusFlag;            // Worker-reported success flag
    bool statusConsumed;        // Tracks whether readers consumed the status
    bool resultReady;           // True when resultValue holds a pending result
    bool resultConsumed;        // Tracks whether the pending result has been taken
    Value resultValue;          // Stored builtin result value

    pthread_mutex_t resultMutex; // Protects result/status hand-off
    pthread_cond_t resultCond;   // Notifies waiters when hand-off is ready
    bool syncInitialized;        // True once mutex/cond initialised

    // Cooperative scheduling controls & identity
    char name[THREAD_NAME_MAX];
    atomic_bool paused;             // Worker paused flag
    atomic_bool cancelRequested;    // Cancellation requested flag
    atomic_bool killRequested;      // Hard termination requested

    // Pool ownership bookkeeping
    atomic_bool inPool;              // True when thread participates in worker pool --
                                     // TSan-confirmed reachable race (VM 2.0 Phase 5a
                                     // checkpoint 5a-i): createThreadJob's slot-scan
                                     // reads this unlocked while a worker retiring due
                                     // to job-queue idle timeout writes it unlocked, in
                                     // ordinary (non-shutdown) operation -- not the
                                     // shutdown-only case it was first assumed to be.
                                     // Joins active/paused/cancelRequested/killRequested
                                     // as an atomic flag.
    bool idle;                      // True when waiting for a job
    bool shouldExit;                // Signals worker loop shutdown
    bool ownsVm;                    // Tracks whether vm pointer should be destroyed
    int poolGeneration;             // Bumps when recycled so handles stay unique
    bool poolWorker;                // True when thread should be reported as part of the shared pool
    ThreadJob* currentJob;          // Currently executing job (if any)
    bool awaitingReuse;             // True once job finished but not yet released
    bool readyForReuse;             // Set by consumers to allow thread to return to pool

    struct timespec queuedAt;       // Timestamp when job was queued
    struct timespec startedAt;      // Timestamp when job began execution
    struct timespec finishedAt;     // Timestamp when job finished execution

    ThreadMetrics metrics;        // Metrics captured for last executed job

    pthread_mutex_t stateMutex;    // Protects paused/cancel flags and state transitions
    pthread_cond_t stateCond;      // Signals state changes (pause/resume/kill)
    bool stateSyncInitialized;     // True once state mutex/cond initialised

    // VM 2.0 Phase 5a checkpoint 5a-iii (Docs/pscal_vm2_plan.md Sec 6.1):
    // native-task progress counters, generic enough for any future
    // vmTaskCreateNative caller (HTTP async today; SQLite busy queries, DNS
    // lookups later), not HTTP-specific. Cancellation deliberately has NO
    // separate native-task hook here: an earlier draft added `nativeCancelFn`
    // (a per-slot function pointer set by vmTaskCreateNative right after
    // spawning, for vmThreadCancel to invoke), but that write raced against
    // this exact worker's OWN threadStart job-pickup code calling
    // vmThreadResetResult on its first loop iteration -- confirmed by an
    // actual failing cancel-demo run, not assumed -- since both can run
    // concurrently on different threads with no ordering between them.
    // Native work has no interpreter safe points to cooperatively check
    // cancelRequested at (unlike bytecode, which the interpreter loop
    // already polls), but it CAN poll cancelRequested directly itself, at
    // its own natural safe points (e.g. HTTP async's curl progress
    // callback does) -- which needs no new plumbing at all, just a plain
    // atomic_load of the flag every native work_fn already has access to
    // via its own Thread*.
    atomic_llong nativeProgressNow;   // vmTaskReportProgress / vmTaskGetProgress
    atomic_llong nativeProgressTotal; // (0 total means "unknown", matching
                                       // HTTP's existing dl_total convention)
} Thread;

typedef void (*VMThreadCallback)(struct VM_s* threadVm, void* user_data);
typedef void (*VMThreadCleanup)(void* user_data);

typedef struct {
    pthread_mutex_t handle;
    bool active;
} Mutex;

// --- Virtual Machine Structure ---
typedef struct VM_s {
    BytecodeChunk* chunk;     // The chunk of bytecode to execute
    uint8_t* ip;              // Instruction Pointer: points to the *next* byte to be read
    uint8_t* lastInstruction; // Start of the last executed instruction

    // The operand stack (VM 2.0 Phase 3, plan §5.9). `stack` is the base of a
    // single virtual-memory reservation made once in initVM() via mmap(PROT_NONE)
    // and never relocated or freed until freeVM() — GET_LOCAL_ADDRESS/
    // GET_GSLOT_ADDRESS and VAR-parameter passing push real `Value*` pointers
    // into this region that must stay valid for the VM's entire lifetime (see
    // manual ch1 §1.2). Growth extends the accessible (mprotect'd PROT_READ|
    // PROT_WRITE) prefix in place rather than reallocating, so the base address
    // never changes and every existing `vm->stackTop - vm->stack`-style
    // computation throughout vm.c keeps working unmodified.
    Value* stack;
    Value* stackTop;          // Pointer to the element just above the top of the stack
                              // (i.e., where the next pushed item will go)
    size_t stackCommittedValues; // currently accessible (mprotect'd RW) prefix, in Values
    size_t stackReservedValues;  // total virtual reservation size, in Values (the hard ceiling)
    size_t stackMappedBytes;     // exact byte length passed to mmap (for munmap in freeVM)

    HashTable* vmGlobalSymbols;      // VM's own symbol table for runtime global variable storage
    HashTable* vmConstGlobalSymbols; // Separate table for constant globals (read-only, no mutex)
    HashTable* procedureTable;      // store procedure table for disassembly
    Symbol** procedureByAddress;    // Cache mapping bytecode offsets to procedure symbols
    size_t procedureByAddressSize;  // Number of cached entries
    
    HostFn host_functions[MAX_HOST_FUNCTIONS];

    // Call-frame array (VM 2.0 Phase 3). Grown via ordinary realloc() — unlike
    // the operand stack, nothing takes the address of a CallFrame and holds it
    // beyond the immediate scope of the opcode handler that read it (verified
    // across every CALL/CALL_INDIRECT/CALL_METHOD/PROC_CALL_INDIRECT/RETURN/
    // THREAD_CREATE site), so relocation on growth is safe.
    CallFrame* frames;
    size_t frameCapacity;
    int frameCount;

    bool exit_requested;      // Indicates a builtin requested early exit from the current frame
    bool abort_requested;     // Raised when a builtin requests an immediate interpreter abort
    bool suspend_unwind_requested; // Keep unwinding frames for cooperative Ctrl-Z requests
    const char* current_builtin_name; // Tracks the name of the builtin currently executing (for diagnostics)
    Value threadMyself;       // Per-VM receiver context for Pascal record methods

    // Threading support (VM 2.0 Phase 5a checkpoint 5a-ii: mmap-reserved,
    // growable storage -- see vmAllocThreadStorage's comment. `threads` is
    // fixed for the life of the VM once allocated, exactly like `stack`
    // above: VM_s.owningThread and threadStart's own persistent `Thread*`
    // (TSan/audit-confirmed long-lived pointers, Docs/pscal_vm2_plan.md
    // Sec 6.1) depend on this.
    Thread* threads;
    size_t threadsReservedCount;
    size_t threadsCommittedCount;
    size_t threadsMappedBytes;
    int threadCount;
    struct VM_s* threadOwner;

    pthread_mutex_t threadRegistryLock; // Protects worker pool state
    struct ThreadJobQueue* jobQueue;    // Shared job queue for worker reuse
    int workerCount;                    // Number of worker threads allocated
    int availableWorkers;               // Number of idle workers in pool
    atomic_bool shuttingDownWorkers;    // Signals pool shutdown

    // Mutex support (VM 2.0 Phase 5a checkpoint 5a-ii: mmap-reserved,
    // growable storage, same rationale as `threads` above -- lockMutex/
    // unlockMutex hold a raw Mutex* across a potentially long blocking
    // pthread_mutex_lock/unlock call, after releasing mutexRegistryLock).
    Mutex* mutexes;
    size_t mutexesReservedCount;
    size_t mutexesCommittedCount;
    size_t mutexesMappedBytes;
    int mutexCount;
    pthread_mutex_t mutexRegistryLock; // Protects mutex registry updates
    struct VM_s* mutexOwner; // VM that owns the mutex registry

    Thread* owningThread;        // Non-NULL when running inside a worker slot
    int threadId;                // Slot index for owningThread (0 for main VM)

    /* Frontend-specific context; e.g., exsh per-VM shell state. */
    void* frontendContext;
    /* String indexing mode: true for shell-style (0-based), false for Pascal/REA (1-based). */
    bool shellIndexing;

    // Optional tracing: when >0, print execution of first N instructions
    int trace_head_instructions;
    int trace_executed;

} VM;

// --- Public VM Interface ---
void initVM(VM* vm);    // Initialize a new VM instance
void freeVM(VM* vm);    // Free resources associated with a VM instance
void vmResetExecutionState(VM* vm); // Reset stack/frames so a VM can be reused

// Main function to interpret a chunk of bytecode
// Takes a BytecodeChunk that was successfully compiled.
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* const_globals, HashTable* procedures, uint32_t entry);
void vmNullifyAliases(VM* vm, uintptr_t disposedAddrValue);
int vmSpawnCallbackThread(VM* vm, VMThreadCallback callback, void* user_data, VMThreadCleanup cleanup);
// VM 2.0 Phase 5a checkpoint 5a-i (Docs/pscal_vm2_plan.md Sec 6.1): task
// primitives backing TaskSpawn/TaskDone (builtin.c). TaskAwait/TaskCancel
// reuse the existing vmJoinThreadById/vmThreadTakeResult/vmThreadCancel
// above directly -- no new entry point needed for those.
int vmHostCreateTaskEntry(VM* vm, Value fnVal, int argc, const Value* argv);
bool vmTaskIsDone(VM* vm, int threadId);
// VM 2.0 Phase 5a checkpoint 5a-iii: the "any future builtin needing
// awaitable async work" entry point the plan calls for (HTTP async is the
// first caller, retiring its own bespoke 32-slot pool; SQLite busy queries
// or DNS lookups are natural future callers). A thin wrapper over
// vmSpawnCallbackThread (same underlying growable pool, same job-queue
// machinery). The interpreter's own cooperative-cancel safe points don't
// exist inside native C code, so work_fn is responsible for polling
// Thread.cancelRequested itself (via threadVm->owningThread) at its own
// natural safe points (e.g. a curl progress callback) -- deliberately NOT
// a separate cancel_fn hook set post-spawn: an earlier draft tried that and
// found it races against this exact worker's own first-job-pickup
// vmThreadResetResult call on a different thread (confirmed by an actual
// failing cancel test, not assumed). Returns the new thread id (wrap in
// makeTask(id, vm) to hand a Value back to script code), or -1 with
// *cleanup* already invoked on failure -- unlike vmSpawnCallbackThread,
// whose failure contract leaves that to the caller only for the narrow
// (!vm || !callback) case; callers of THIS function may assume cleanup
// always runs exactly once, success or failure.
int vmTaskCreateNative(VM* vm, VMThreadCallback work_fn, void* user_data, VMThreadCleanup cleanup);
// Called by a native work_fn (e.g. HTTP async's curl progress callback) to
// publish progress; called by anything holding a task Value (e.g.
// HttpGetAsyncProgress/Total) to read it back. total==0 conventionally
// means "unknown," matching HTTP's pre-existing dl_total contract.
void vmTaskReportProgress(VM* threadVm, long long now, long long total);
bool vmTaskGetProgress(VM* owner, int threadId, long long* outNow, long long* outTotal);
int vmSpawnBuiltinThread(VM* vm, int builtinId, const char* builtinName, int argCount,
                         const Value* args, bool submitOnly, const char* threadName);
void vmThreadStoreResult(VM* vm, const Value* result, bool success);
bool vmThreadTakeResult(VM* vm, int threadId, Value* outResult, bool takeValue, bool* outStatus, bool takeStatus);
bool vmJoinThreadById(struct VM_s* vm, int id);
bool vmThreadAssignName(struct VM_s* vm, int threadId, const char* name);
int vmThreadFindIdByName(struct VM_s* vm, const char* name);
bool vmThreadPause(struct VM_s* vm, int threadId);
bool vmThreadResume(struct VM_s* vm, int threadId);
bool vmThreadCancel(struct VM_s* vm, int threadId);
bool vmThreadKill(struct VM_s* vm, int threadId);
size_t vmSnapshotWorkerUsage(struct VM_s* vm, ThreadMetrics* outMetrics, size_t capacity);
size_t vmSnapshotProcState(VMProcSnapshot* out, size_t capacity);
size_t vmSnapshotProcWorkers(uintptr_t vm_address, VMProcWorkerSnapshot* out, size_t capacity);

// Register and lookup class methods in the VM's procedure table
void vmRegisterClassMethod(VM* vm, const char* className, uint16_t methodIndex, Symbol* methodSymbol);
Symbol* vmFindClassMethod(VM* vm, const char* className, uint16_t methodIndex);

void runtimeError(VM* vm, const char* format, ...);
void runtimeWarning(VM* vm, const char* format, ...);
void vmDumpStackInfo(VM* vm);
void vmDumpStackInfoDetailed(VM* vm, const char* context_message);
void vmSetSuppressStateDump(bool suppress);
void vmSetVerboseErrors(bool enabled);
void vmOpcodeProfileDump(void);
bool vmOpcodeProfileIsEnabled(void);
void vmProfileShellBuiltin(const char *name);

#endif // PSCAL_VM_H
