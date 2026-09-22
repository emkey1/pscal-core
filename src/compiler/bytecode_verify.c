// src/compiler/bytecode_verify.c
//
// Load-time bytecode verifier (VM 2.0 plan, Docs/pscal_vm2_plan.md §5.5).
// See bytecode_verify.h for the contract and opcodes.def's Phase 1e audit
// comment for the per-opcode stack-effect rationale this file implements.

#include "compiler/bytecode_verify.h"
#include "compiler/bytecode.h"
#include "backend_ast/builtin.h"
#include "vm/vm.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/globals.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- Small byte-decode helpers (big-endian, matching emitShort/emitInt32 in
// bytecode.c and READ_SHORT/READ_UINT32 in vm.c). Callers only invoke these
// on ranges already proven in-bounds by the pass-1 instruction-stream walk. */
static uint16_t verifyReadU16BE(const uint8_t* code, int pos) {
    return (uint16_t)(((uint16_t)code[pos] << 8) | (uint16_t)code[pos + 1]);
}
static uint32_t verifyReadU32BE(const uint8_t* code, int pos) {
    return ((uint32_t)code[pos] << 24) | ((uint32_t)code[pos + 1] << 16) |
           ((uint32_t)code[pos + 2] << 8) | (uint32_t)code[pos + 3];
}

typedef struct {
    const BytecodeChunk* chunk;
    HashTable* procedures;
    bool* boundary;   // size chunk->count; true at valid instruction-start offsets
    char* err_buf;
    size_t err_buf_size;
} VCtx;

static bool vfail(VCtx* ctx, const char* fmt, ...) {
    if (ctx->err_buf && ctx->err_buf_size) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(ctx->err_buf, ctx->err_buf_size, fmt, ap);
        va_end(ap);
    }
    return false;
}

static bool checkConstIndex(VCtx* ctx, uint32_t idx, int pc, const char* what) {
    if (idx >= (uint32_t)ctx->chunk->constants_count) {
        return vfail(ctx, "pc %d: %s constant index %u out of range (pool size %d)",
                     pc, what, idx, ctx->chunk->constants_count);
    }
    return true;
}

// VM 2.0 Phase 2a (plan §5.6): validates a GET/SET_GLOBAL[16] cache_id
// operand against chunk->cache_count, same pattern as checkConstIndex above.
// The opcodes that carried a 'c' operand are retired as of Phase 2b, so
// this is now unreachable for any chunk emitted post-2b; kept because the
// 'c' spec letter itself is kept (opcodes.def) for legacy .bc verification.
static bool checkCacheIndex(VCtx* ctx, uint32_t idx, int pc, const char* what) {
    if (idx >= (uint32_t)ctx->chunk->cache_count) {
        return vfail(ctx, "pc %d: %s cache index %u out of range (cache_count %d)",
                     pc, what, idx, ctx->chunk->cache_count);
    }
    return true;
}

// VM 2.0 Phase 2b (plan §5.7): validates a GET_GSLOT/SET_GSLOT/
// GET_GSLOT_ADDRESS/DEFINE_GLOBAL_SLOT slot operand against
// chunk->global_slot_count. Meaningful only because the load-time link step
// (compiler/bytecode_link.c) has already run by the time this verifier
// executes -- see cache.c's load-then-verify ordering and bytecode_link.c's
// module comment for why that ordering, and not the reverse, is correct.
static bool checkSlotIndex(VCtx* ctx, uint32_t idx, int pc, const char* what) {
    if (idx >= (uint32_t)ctx->chunk->global_slot_count) {
        return vfail(ctx, "pc %d: %s slot index %u out of range (global_slot_count %d)",
                     pc, what, idx, ctx->chunk->global_slot_count);
    }
    return true;
}

// A file declaration's element-type-name operand (DEFINE_GLOBAL*'s TYPE_FILE
// payload, INIT_LOCAL_FILE's K) holds this instead of a constant index for a
// text or untyped file; the VM tests for it before indexing the pool.
#define NO_ELEMENT_TYPE_NAME 0xFFFFu

static bool checkCodeTarget(VCtx* ctx, uint32_t target, int pc, const char* what) {
    if (target >= (uint32_t)ctx->chunk->count || !ctx->boundary[target]) {
        return vfail(ctx, "pc %d: %s target %u is not a valid instruction boundary",
                     pc, what, target);
    }
    return true;
}

// ===================== Pass 1: instruction stream walk =====================

static bool verifyInstructionStream(VCtx* ctx) {
    const BytecodeChunk* chunk = ctx->chunk;
    int pc = 0;
    while (pc < chunk->count) {
        uint8_t opcode = chunk->code[pc];
        const OpcodeInfo* info = pscalOpcodeInfo(opcode);
        if (!info) {
            return vfail(ctx, "pc %d: undefined opcode 0x%02X", pc, opcode);
        }
        int len = 0;
        bool decoded_ok = pscalDecodeInstructionLength(chunk, pc, &len);
        if (!decoded_ok) {
            return vfail(ctx, "pc %d: truncated %s instruction", pc, info->name);
        }
        if (len <= 0 || pc + len > chunk->count) {
            return vfail(ctx, "pc %d: %s instruction runs past end of code section", pc, info->name);
        }
        ctx->boundary[pc] = true;
        pc += len;
    }
    return true;
}

// ================== Pass 2: operand / payload validation ===================

// Re-walks a DEFINE_GLOBAL/DEFINE_GLOBAL16/DEFINE_GLOBAL_SLOT/
// INIT_LOCAL_ARRAY/INIT_FIELD_ARRAY payload (whose bytes are already known
// in-bounds from pass 1) to validate every embedded constant-pool index
// and, for the array-shaped opcodes, count dimensions using the
// runtime-computed-bound sentinel (lo==hi==0xFFFF) -- each such dimension
// pops one size value at runtime (see opcodes.def's Phase 1e audit
// comment). out_dynamic_dims may be NULL when the caller only needs
// validation (DEFINE_GLOBAL/16/_SLOT).
static bool walkVariablePayload(VCtx* ctx, int pc, int* out_dynamic_dims) {
    const BytecodeChunk* chunk = ctx->chunk;
    const uint8_t* code = chunk->code;
    uint8_t opcode = code[pc];
    if (out_dynamic_dims) *out_dynamic_dims = 0;

    if (opcode == DEFINE_GLOBAL || opcode == DEFINE_GLOBAL16 || opcode == DEFINE_GLOBAL_SLOT) {
        // DEFINE_GLOBAL is the narrow (u8 name) legacy form, DEFINE_GLOBAL16
        // the wide (u16 name) legacy form, and DEFINE_GLOBAL_SLOT (VM 2.0
        // Phase 2b) the current, always-wide (u16) form whose leading field
        // is a slot index rather than a name index -- see checkSlotIndex vs
        // checkConstIndex below for the only difference in this branch.
        bool wide = (opcode == DEFINE_GLOBAL16 || opcode == DEFINE_GLOBAL_SLOT);
        int name_pos = pc + 1;
        uint32_t name_idx = wide ? verifyReadU16BE(code, name_pos) : code[name_pos];
        if (opcode == DEFINE_GLOBAL_SLOT) {
            if (!checkSlotIndex(ctx, name_idx, pc, "DEFINE_GLOBAL_SLOT")) return false;
        } else {
            if (!checkConstIndex(ctx, name_idx, pc, "DEFINE_GLOBAL name")) return false;
        }
        int type_pos = wide ? pc + 3 : pc + 2;
        VarType declared = (VarType)code[type_pos];
        int cursor = type_pos + 1;
        if (declared == TYPE_ARRAY) {
            uint8_t dims = code[cursor++];
            for (uint8_t d = 0; d < dims; d++) {
                uint32_t lo = verifyReadU16BE(code, cursor); cursor += 2;
                uint32_t hi = verifyReadU16BE(code, cursor); cursor += 2;
                if (!(lo == 0xFFFF && hi == 0xFFFF)) {
                    if (!checkConstIndex(ctx, lo, pc, "DEFINE_GLOBAL array bound")) return false;
                    if (!checkConstIndex(ctx, hi, pc, "DEFINE_GLOBAL array bound")) return false;
                }
            }
            cursor++; // element VarType byte
            uint32_t elem_name = verifyReadU16BE(code, cursor); cursor += 2;
            if (!checkConstIndex(ctx, elem_name, pc, "DEFINE_GLOBAL element-name")) return false;
        } else {
            uint32_t type_name = verifyReadU16BE(code, cursor); cursor += 2;
            if (!checkConstIndex(ctx, type_name, pc, "DEFINE_GLOBAL type-name")) return false;
            if (declared == TYPE_STRING) {
                uint32_t len_idx = verifyReadU16BE(code, cursor); cursor += 2;
                if (!checkConstIndex(ctx, len_idx, pc, "DEFINE_GLOBAL length")) return false;
            } else if (declared == TYPE_FILE) {
                cursor++; // element VarType byte
                uint32_t elem_name = verifyReadU16BE(code, cursor); cursor += 2;
                // 0xFFFF: no element type (text and untyped files); the VM skips it.
                if (elem_name != NO_ELEMENT_TYPE_NAME &&
                    !checkConstIndex(ctx, elem_name, pc, "DEFINE_GLOBAL element-name")) return false;
            }
        }
        return true;
    }

    // INIT_LOCAL_ARRAY / INIT_FIELD_ARRAY: [slot-or-field:b][dims:b]
    // { [lo:K][hi:K] }*dims [elem_type:b][elem_name:K]
    int cursor = pc + 2; // past opcode + slot/field byte
    uint8_t dims = code[cursor++];
    int dynamic = 0;
    for (uint8_t d = 0; d < dims; d++) {
        uint32_t lo = verifyReadU16BE(code, cursor); cursor += 2;
        uint32_t hi = verifyReadU16BE(code, cursor); cursor += 2;
        if (lo == 0xFFFF && hi == 0xFFFF) {
            dynamic++;
        } else {
            if (!checkConstIndex(ctx, lo, pc, "array bound")) return false;
            if (!checkConstIndex(ctx, hi, pc, "array bound")) return false;
        }
    }
    cursor++; // element VarType byte
    uint32_t elem_name = verifyReadU16BE(code, cursor); cursor += 2;
    if (!checkConstIndex(ctx, elem_name, pc, "element-name")) return false;
    if (out_dynamic_dims) *out_dynamic_dims = dynamic;
    return true;
}

static bool verifyOperands(VCtx* ctx) {
    const BytecodeChunk* chunk = ctx->chunk;
    const uint8_t* code = chunk->code;
    for (int pc = 0; pc < chunk->count; pc++) {
        if (!ctx->boundary[pc]) continue;
        uint8_t opcode = code[pc];
        const OpcodeInfo* info = pscalOpcodeInfo(opcode);
        int len = 0;
        pscalDecodeInstructionLength(chunk, pc, &len); // already known-good (pass 1)

        if (strcmp(info->operands, "?") == 0) {
            if (!walkVariablePayload(ctx, pc, NULL)) return false;
            continue;
        }

        if (opcode == CALL_HOST) {
            uint8_t host_id = code[pc + 1];
            if (host_id >= HOST_FN_COUNT) {
                return vfail(ctx, "pc %d: CALL_HOST host id %u out of range (max %d)",
                             pc, host_id, HOST_FN_COUNT);
            }
            continue;
        }

        if (opcode == INIT_LOCAL_FILE) { // "bbK": slot, element VarType, element-type name
            uint32_t elem_name = verifyReadU16BE(code, pc + 3);
            if (elem_name != NO_ELEMENT_TYPE_NAME &&
                !checkConstIndex(ctx, elem_name, pc, info->name)) return false;
            continue;
        }

        int cursor = pc + 1;
        for (const char* p = info->operands; *p; ++p) {
            switch (*p) {
                case 'k': {
                    if (!checkConstIndex(ctx, code[cursor], pc, info->name)) return false;
                    cursor += 1;
                    break;
                }
                case 'K': {
                    uint32_t idx = verifyReadU16BE(code, cursor);
                    if (!checkConstIndex(ctx, idx, pc, info->name)) return false;
                    cursor += 2;
                    break;
                }
                case 'b':
                case 'i':
                    cursor += 1;
                    break;
                case 'w':
                    cursor += 2;
                    break;
                case 'W': {
                    uint32_t target = verifyReadU32BE(code, cursor);
                    if (!checkCodeTarget(ctx, target, pc, info->name)) return false;
                    cursor += 4;
                    break;
                }
                case 'j': {
                    int32_t disp = (int32_t)verifyReadU32BE(code, cursor);
                    long target = (long)(pc + len) + disp;
                    if (target < 0 || target >= chunk->count || !ctx->boundary[(int)target]) {
                        return vfail(ctx, "pc %d: %s jump target %ld is not a valid instruction boundary",
                                     pc, info->name, target);
                    }
                    cursor += 4;
                    break;
                }
                case 'f':
                    cursor += 4;
                    break;
                case 'c': {
                    uint32_t idx = verifyReadU16BE(code, cursor);
                    if (!checkCacheIndex(ctx, idx, pc, info->name)) return false;
                    cursor += 2;
                    break;
                }
                case 's': {
                    uint32_t idx = verifyReadU16BE(code, cursor);
                    if (!checkSlotIndex(ctx, idx, pc, info->name)) return false;
                    cursor += 2;
                    break;
                }
                case 'C':
                    // Legacy operand spec, only ever seen on the retired
                    // GET/SET_GLOBAL[16]_CACHED holes (opcodes.def 0x28-0x2B).
                    // Never emitted post-Phase-2a; no cache_id-style bounds
                    // check applies since it's not a table index at all (it
                    // was a raw in-stream Symbol* slot).
                    cursor += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                    break;
                default:
                    break;
            }
        }
    }
    return true;
}

// ============== Pass 3: per-entry abstract stack-depth walk ================
//
// Each walk starts at depth 0 from an entry point where the VM begins a
// frame: pc 0 (the top-level program), every procedure's bytecode_address,
// and every THREAD_CREATE target. It follows control flow wherever it leads
// in the chunk. Procedure addresses are not extents: Pascal-family compilers
// lay a program out as
//     prologue; JUMP L1; proc A; L1: JUMP L2; proc B; L2: main block; HALT
// and a routine with nested routines as JUMP-over-nested + body, so the code
// a JUMP lands on usually lies after some other routine's start address while
// still belonging to the routine that jumped. Treating "next procedure's
// address" as the end of a routine (and dropping edges that crossed it) left
// every Pascal main block, and the body of every routine with nested
// routines, unwalked.
//
// Every instruction a walk reaches belongs to that walk alone. Reaching
// another walk's entry or instructions, or falling off the end of the code,
// is rejected: routines are entered only by calls, and no compiler emits a
// jump or fall-through from one routine's code into another's. That keeps
// each instruction's depth tied to one frame, and keeps the pass linear,
// since no instruction is walked by more than one entry.

typedef struct {
    uint32_t addr;
    Symbol* sym; // NULL for pc 0's top-level program, or an unnamed thread entry
} AddrSym;

// Appends one entry; false only on allocation failure.
static bool appendAddrSym(AddrSym** arr, int* count, int* cap, uint32_t addr, Symbol* sym) {
    if (*count == *cap) {
        int new_cap = (*cap == 0) ? 16 : (*cap * 2);
        AddrSym* grown = (AddrSym*)realloc(*arr, sizeof(AddrSym) * (size_t)new_cap);
        if (!grown) return false;
        *arr = grown;
        *cap = new_cap;
    }
    (*arr)[*count].addr = addr;
    (*arr)[*count].sym = sym;
    (*count)++;
    return true;
}

static bool collectProcedures(HashTable* table, AddrSym** arr, int* count, int* cap) {
    if (!table) return true;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (!s->is_alias && s->is_defined && s->bytecode_address >= 0) {
                if (!appendAddrSym(arr, count, cap, (uint32_t)s->bytecode_address, s)) return false;
            }
            if (s->type_def && s->type_def->symbol_table) {
                if (!collectProcedures((HashTable*)s->type_def->symbol_table, arr, count, cap)) return false;
            }
        }
    }
    return true;
}

// Orders by address, and a named entry ahead of an unnamed one at the same
// address, so deduplication keeps the procedure's symbol.
static int cmpAddrSym(const void* a, const void* b) {
    const AddrSym* x = (const AddrSym*)a;
    const AddrSym* y = (const AddrSym*)b;
    if (x->addr != y->addr) return (x->addr < y->addr) ? -1 : 1;
    if ((x->sym != NULL) != (y->sym != NULL)) return x->sym ? -1 : 1;
    return 0;
}

static const char* entryName(const AddrSym* entry) {
    if (entry->sym && entry->sym->name) return entry->sym->name;
    return (entry->addr == 0) ? "the top-level program" : "an unnamed thread entry";
}

// Finds the defined, non-alias procedure whose bytecode_address == address,
// including nested (class/unit) symbol tables. Mirrors vm.c's (static, not
// exported) findProcedureByAddress()+resolveProcedureAlias().
static Symbol* findProcByAddress(HashTable* table, uint32_t address) {
    if (!table) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (s->is_defined && (uint32_t)s->bytecode_address == address) {
                return (s->is_alias && s->real_symbol) ? s->real_symbol : s;
            }
            if (s->type_def && s->type_def->symbol_table) {
                Symbol* nested = findProcByAddress((HashTable*)s->type_def->symbol_table, address);
                if (nested) return nested;
            }
        }
    }
    return NULL;
}

static Symbol* findProcByName(HashTable* table, const char* lowered_name) {
    Symbol* sym = hashTableLookup(table, lowered_name);
    if (!sym) return NULL;
    return (sym->is_alias && sym->real_symbol) ? sym->real_symbol : sym;
}

// Tri-state abstract depth: a call through an unresolvable target (closure,
// vtable dispatch) or CALL_HOST (opaque per-host-id convention) makes the
// exact depth unknowable; from that point on this walk stops asserting
// bounds along that path (the VM's own checked push()/pop() remain the
// runtime backstop for that region -- see bytecode_verify.h).
typedef struct {
    bool known;
    int value;
} Depth;

typedef struct {
    int pc;
    Depth depth;
} WorkItem;

// Per-instruction classification: minimum depth required before it executes
// (`req`), and the net depth delta if the incoming depth is known. `unknown`
// is set when the post-state must be tainted regardless of req/delta.
typedef struct {
    int req;
    int delta;
    bool unknown_after;
} Effect;

static bool classifyInstruction(VCtx* ctx, int pc, int len, const OpcodeInfo* info,
                                 Symbol* entry_symbol, Effect* eff) {
    const uint8_t* code = ctx->chunk->code;
    uint8_t opcode = code[pc];
    eff->req = 0;
    eff->delta = 0;
    eff->unknown_after = false;

    switch (opcode) {
        case RETURN:
        case EXIT: {
            bool is_function = entry_symbol && entry_symbol->type != TYPE_VOID;
            eff->req = is_function ? 1 : 0;
            eff->delta = 0; // terminal: no successor consumes the post-state
            return true;
        }
        case GET_ELEMENT_ADDRESS:
        case LOAD_ELEMENT_VALUE: {
            uint8_t dims = code[pc + 1];
            eff->req = 1 + dims;
            eff->delta = 1 - eff->req;
            return true;
        }
        case INIT_LOCAL_ARRAY: {
            int dynamic = 0;
            if (!walkVariablePayload(ctx, pc, &dynamic)) return false;
            eff->req = dynamic;
            eff->delta = -dynamic;
            return true;
        }
        case INIT_FIELD_ARRAY: {
            int dynamic = 0;
            if (!walkVariablePayload(ctx, pc, &dynamic)) return false;
            eff->req = 1 + dynamic; // base pointer (peeked, kept) + dynamic sizes
            eff->delta = -dynamic;
            return true;
        }
        case CALL_BUILTIN_PROC: {
            uint8_t arity = code[pc + len - 1]; // "wKb": arity is the trailing byte
            eff->req = arity;
            eff->delta = -arity;
            return true;
        }
        case CALL_BUILTIN: {
            uint8_t arity = code[pc + len - 1]; // "Kb": arity is the trailing byte
            uint32_t name_idx = verifyReadU16BE(code, pc + 1);
            int pushed = 1; // fallback: assume function-shaped if unresolvable
            if (name_idx < (uint32_t)ctx->chunk->constants_count) {
                Value* v = &ctx->chunk->constants[name_idx];
                if (VALUE_TYPE(*v) == TYPE_STRING && AS_STRING(*v)) {
                    BuiltinRoutineType t = getBuiltinType(AS_STRING(*v));
                    if (t == BUILTIN_TYPE_PROCEDURE) pushed = 0;
                }
            }
            eff->req = arity;
            eff->delta = pushed - arity;
            return true;
        }
        case CALL_USER_PROC:
        case CALL: {
            uint8_t arity = code[pc + len - 1]; // trailing byte in both "Kb" and "KWb"
            Symbol* target = NULL;
            if (opcode == CALL) {
                uint32_t addr = verifyReadU32BE(code, pc + 3);
                target = findProcByAddress(ctx->procedures, addr);
            } else {
                uint32_t name_idx = verifyReadU16BE(code, pc + 1);
                if (name_idx < (uint32_t)ctx->chunk->constants_count) {
                    Value* v = &ctx->chunk->constants[name_idx];
                    if (VALUE_TYPE(*v) == TYPE_STRING && AS_STRING(*v)) {
                        char lowered[MAX_SYMBOL_LENGTH + 1];
                        strncpy(lowered, AS_STRING(*v), MAX_SYMBOL_LENGTH);
                        lowered[MAX_SYMBOL_LENGTH] = '\0';
                        toLowerString(lowered);
                        target = findProcByName(ctx->procedures, lowered);
                    }
                }
            }
            eff->req = arity;
            if (target) {
                // The caller's view once the call returns: returnFromCall()
                // collapses the frame -- args and callee locals alike -- back
                // to frame->slots, then pushes a result only for a function.
                // (locals_count is what the callee's own walk starts with,
                // not a net effect here; that walk runs separately.)
                eff->delta = -(int)arity + (target->type != TYPE_VOID ? 1 : 0);
            } else {
                // Unresolvable (stale/foreign cache entry): don't guess.
                eff->unknown_after = true;
            }
            return true;
        }
        case CALL_INDIRECT:
        case CALL_METHOD: {
            uint8_t arity = code[pc + 1 + (opcode == CALL_METHOD ? 1 : 0)];
            eff->req = arity + 1; // args (+receiver for CALL_METHOD) plus the address/vtable slot
            eff->unknown_after = true; // target resolved at runtime; not statically knowable
            return true;
        }
        case PROC_CALL_INDIRECT: {
            // Unlike CALL_INDIRECT/CALL_METHOD, the callee's result push is
            // unconditionally suppressed (discard_result_on_return) and its
            // RETURN unconditionally collapses stackTop to the pre-call
            // frame base, so the net round-trip effect is exactly
            // -(arity+1) regardless of which target gets called.
            uint8_t arity = code[pc + 1];
            eff->req = arity + 1;
            eff->delta = -(arity + 1);
            return true;
        }
        case CALL_HOST: {
            // host_fn_id has no arg-count operand; each host function pops
            // its own ad hoc convention (see opcodes.def audit comment).
            eff->req = 0;
            eff->unknown_after = true;
            return true;
        }
        default: {
            if (info->stack_in < 0 || info->stack_out < 0) {
                // Any other -1-flagged opcode we haven't special-cased above
                // is a table-authoring bug, not a verifiable chunk.
                return vfail(ctx, "pc %d: opcode %s has no verifier rule for its variable stack effect",
                             pc, info->name);
            }
            eff->req = info->stack_in;
            eff->delta = info->stack_out - info->stack_in;
            return true;
        }
    }
}

// Per-walk state shared across entries. owner[pc] is 1 + the index (into
// entries) of the walk that reached pc, or 0 if none has; entry pcs are
// claimed by their own walk before any walk runs, so reaching one from
// elsewhere trips the same ownership check as reaching another walk's code.
typedef struct {
    const AddrSym* entries;
    int* owner;
    uint8_t* visited;
    Depth* depths;
    WorkItem* worklist;
    int worklist_cap;
} WalkState;

// Accepts `target` as a successor of `pc` in walk `walk_id`, or fails the
// chunk if control would leave the code section or enter another walk's
// code (see the Pass 3 comment above).
static bool addSuccessor(VCtx* ctx, const WalkState* ws, int walk_id, int pc,
                         const OpcodeInfo* info, int target, int* out, int* out_count) {
    if (target < 0 || target >= ctx->chunk->count || !ctx->boundary[target]) {
        return vfail(ctx, "pc %d: %s continues to pc %d, past the end of the code section",
                     pc, info->name, target);
    }
    int other = ws->owner[target];
    if (other != 0 && other != walk_id) {
        const AddrSym* here = &ws->entries[walk_id - 1];
        const AddrSym* there = &ws->entries[other - 1];
        if ((int)there->addr == target) {
            return vfail(ctx, "pc %d: %s in %s enters %s at pc %d without a call",
                         pc, info->name, entryName(here), entryName(there), target);
        }
        return vfail(ctx, "pc %d: %s in %s reaches pc %d, which belongs to %s (entry pc %u)",
                     pc, info->name, entryName(here), target, entryName(there), there->addr);
    }
    out[(*out_count)++] = target;
    return true;
}

// Per-pc visit state. A pc is re-checked at most once for a known depth and
// once for an unknown depth: two visits, not one, because a pc can be
// reached by both a normal (known-depth) edge and an edge downstream of an
// unresolvable call (unknown-depth) -- see the Depth/Effect comment above.
// Whichever arrives first must not suppress checking the other: an earlier
// arrival with an unknown depth used to mark the pc "seen" and cause a
// later, checkable, known-depth arrival to be skipped via the seen[pc] gate
// below, silently accepting bytecode that would underflow/overflow on that
// second edge (found in review: a join-point dataflow bug independent of
// finding 1's FAST_PUSH/POP issue).
enum { VISITED_KNOWN = 1u << 0, VISITED_UNKNOWN = 1u << 1 };

static bool verifyWalk(VCtx* ctx, const WalkState* ws, int walk_id) {
    const AddrSym* entry = &ws->entries[walk_id - 1];
    uint8_t* visited = ws->visited;
    Depth* depths = ws->depths;
    WorkItem* worklist = ws->worklist;
    int worklist_cap = ws->worklist_cap;

    int wl_count = 0;
    worklist[wl_count++] = (WorkItem){ (int)entry->addr, (Depth){ true, 0 } };

    while (wl_count > 0) {
        WorkItem item = worklist[--wl_count];
        int pc = item.pc;
        ws->owner[pc] = walk_id; // addSuccessor admitted pc only if unowned or already ours

        if (item.depth.known) {
            if (visited[pc] & VISITED_KNOWN) {
                if (depths[pc].value != item.depth.value) {
                    return vfail(ctx, "pc %d: stack depth mismatch at control-flow join (%d vs %d)",
                                 pc, depths[pc].value, item.depth.value);
                }
                continue; // this exact known depth was already validated here
            }
            visited[pc] |= VISITED_KNOWN;
            depths[pc] = item.depth;
        } else {
            if (visited[pc] & VISITED_UNKNOWN) continue; // no new information
            visited[pc] |= VISITED_UNKNOWN;
        }

        uint8_t opcode = ctx->chunk->code[pc];
        const OpcodeInfo* info = pscalOpcodeInfo(opcode);
        int len = 0;
        pscalDecodeInstructionLength(ctx->chunk, pc, &len); // already known-good (pass 1)

        Effect eff;
        if (!classifyInstruction(ctx, pc, len, info, entry->sym, &eff)) return false;

        Depth next_depth = item.depth;
        if (item.depth.known) {
            if (item.depth.value < eff.req) {
                return vfail(ctx, "pc %d: %s requires stack depth >= %d but have %d",
                             pc, info->name, eff.req, item.depth.value);
            }
            if (eff.unknown_after) {
                next_depth.known = false;
            } else {
                int nv = item.depth.value + eff.delta;
                if (nv < 0) {
                    return vfail(ctx, "pc %d: %s would underflow the stack (depth %d, delta %d)",
                                 pc, info->name, item.depth.value, eff.delta);
                }
                size_t stack_ceiling = pscalVmStackCeilingValues();
                if ((size_t)nv > stack_ceiling) {
                    return vfail(ctx, "pc %d: %s exceeds the maximum stack depth (%d > %zu)",
                                 pc, info->name, nv, stack_ceiling);
                }
                next_depth.value = nv;
            }
        }
        // else: depth already unknown; stays unknown, no checks performed
        // (the runtime's checked push()/pop() remain the backstop here).

        if (opcode == RETURN || opcode == EXIT || opcode == HALT) {
            continue; // terminal: no successors
        }

        int succ[2];
        int succ_count = 0;
        if (opcode == JUMP || opcode == JUMP_IF_FALSE) {
            int32_t disp = (int32_t)verifyReadU32BE(ctx->chunk->code, pc + 1);
            if (!addSuccessor(ctx, ws, walk_id, pc, info, (int)((long)(pc + len) + disp), succ, &succ_count)) {
                return false;
            }
        }
        if (opcode != JUMP) {
            if (!addSuccessor(ctx, ws, walk_id, pc, info, pc + len, succ, &succ_count)) return false;
        }

        for (int i = 0; i < succ_count; i++) {
            // wl_count is bounded by 2 * (instructions processed so far) + 1,
            // which is <= worklist_cap (4 * chunk->count + 8, see caller)
            // for any walk; this check is a defensive backstop, not expected
            // to ever trip.
            if (wl_count < worklist_cap) {
                worklist[wl_count++] = (WorkItem){ succ[i], next_depth };
            }
        }
    }
    return true;
}

static bool verifyStackDepths(VCtx* ctx) {
    const BytecodeChunk* chunk = ctx->chunk;
    AddrSym* addrs = NULL;
    int addr_count = 0, addr_cap = 0;
    int* owner = NULL;
    uint8_t* visited = NULL;
    Depth* depths = NULL;
    WorkItem* worklist = NULL;
    bool ok = true;

    // Entries: the top-level program at pc 0, every procedure, and every
    // THREAD_CREATE target (a thread starts a fresh frame at its operand,
    // whether or not a procedure is registered there; pass 2 has already
    // proven the operand is an instruction boundary).
    bool collected = appendAddrSym(&addrs, &addr_count, &addr_cap, 0, NULL) &&
                     collectProcedures(ctx->procedures, &addrs, &addr_count, &addr_cap);
    for (int pc = 0; collected && pc < chunk->count; pc++) {
        if (ctx->boundary[pc] && chunk->code[pc] == THREAD_CREATE) {
            uint32_t target = verifyReadU32BE(chunk->code, pc + 1);
            collected = appendAddrSym(&addrs, &addr_count, &addr_cap, target,
                                      findProcByAddress(ctx->procedures, target));
        }
    }
    if (!collected) {
        ok = vfail(ctx, "out of memory collecting verifier entry points");
        goto done;
    }
    qsort(addrs, (size_t)addr_count, sizeof(AddrSym), cmpAddrSym);

    // Dedupe identical addresses (aliases resolving to the same target, a
    // procedure at pc 0, a thread entry at a procedure) and drop any that are
    // not an instruction start of this chunk (stale cross-chunk metadata).
    int n = 0;
    for (int i = 0; i < addr_count; i++) {
        if (addrs[i].addr >= (uint32_t)chunk->count || !ctx->boundary[addrs[i].addr]) continue;
        if (n > 0 && addrs[n - 1].addr == addrs[i].addr) continue;
        addrs[n++] = addrs[i];
    }
    addr_count = n;

    owner = (int*)calloc((size_t)chunk->count, sizeof(int));
    visited = (uint8_t*)calloc((size_t)chunk->count, sizeof(uint8_t));
    depths = (Depth*)calloc((size_t)chunk->count, sizeof(Depth));
    // A walk can span up to the whole chunk. Each pc can be *processed*
    // (i.e. reach classifyInstruction and push successors) up to twice --
    // once for a known depth, once for an unknown one (see
    // VISITED_KNOWN/VISITED_UNKNOWN above) -- and each processed pc pushes
    // at most two successors (JUMP_IF_FALSE), so 4*count+8 is a proven upper
    // bound reusable across every walk (see verifyWalk()).
    int worklist_cap = chunk->count * 4 + 8;
    worklist = (WorkItem*)calloc((size_t)worklist_cap, sizeof(WorkItem));
    if (!owner || !visited || !depths || !worklist) {
        ok = vfail(ctx, "out of memory building verifier walk state");
        goto done;
    }

    for (int i = 0; i < addr_count; i++) {
        owner[addrs[i].addr] = i + 1;
    }
    WalkState ws = { addrs, owner, visited, depths, worklist, worklist_cap };
    for (int i = 0; i < addr_count; i++) {
        if (!verifyWalk(ctx, &ws, i + 1)) {
            ok = false;
            goto done;
        }
    }

done:
    free(addrs);
    free(owner);
    free(visited);
    free(depths);
    free(worklist);
    return ok;
}

// ============================ Public entry point ============================

bool pscalVerifyBytecodeChunk(const BytecodeChunk* chunk, HashTable* procedures,
                               char* err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size) err_buf[0] = '\0';
    if (!chunk) return true; // nothing to verify
    if (chunk->count == 0) return true; // empty chunk is trivially valid

    if (!chunk->code) {
        if (err_buf && err_buf_size) snprintf(err_buf, err_buf_size, "chunk has a nonzero count but no code buffer");
        return false;
    }

    VCtx ctx;
    ctx.chunk = chunk;
    ctx.procedures = procedures;
    ctx.err_buf = err_buf;
    ctx.err_buf_size = err_buf_size;
    ctx.boundary = (bool*)calloc((size_t)chunk->count, sizeof(bool));
    if (!ctx.boundary) {
        if (err_buf && err_buf_size) snprintf(err_buf, err_buf_size, "out of memory in verifier");
        return false;
    }

    bool ok = verifyInstructionStream(&ctx) &&
              verifyOperands(&ctx) &&
              verifyStackDepths(&ctx);

    free(ctx.boundary);
    return ok;
}
