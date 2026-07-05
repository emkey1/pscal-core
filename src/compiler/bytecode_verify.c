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
                if (!checkConstIndex(ctx, elem_name, pc, "DEFINE_GLOBAL element-name")) return false;
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

// ============== Pass 3: per-procedure abstract stack-depth walk ============

typedef struct {
    int start;
    int end; // exclusive
    Symbol* symbol; // NULL for the implicit top-level/main segment
} ProcSegment;

typedef struct {
    uint32_t addr;
    Symbol* sym;
} AddrSym;

static void collectProcedures(HashTable* table, AddrSym** arr, int* count, int* cap) {
    if (!table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (!s->is_alias && s->is_defined && s->bytecode_address >= 0) {
                if (*count == *cap) {
                    int new_cap = (*cap == 0) ? 16 : (*cap * 2);
                    AddrSym* grown = (AddrSym*)realloc(*arr, sizeof(AddrSym) * (size_t)new_cap);
                    if (!grown) continue; // best-effort; skip growth on OOM
                    *arr = grown;
                    *cap = new_cap;
                }
                (*arr)[*count].addr = (uint32_t)s->bytecode_address;
                (*arr)[*count].sym = s;
                (*count)++;
            }
            if (s->type_def && s->type_def->symbol_table) {
                collectProcedures((HashTable*)s->type_def->symbol_table, arr, count, cap);
            }
        }
    }
}

static int cmpAddrSym(const void* a, const void* b) {
    uint32_t aa = ((const AddrSym*)a)->addr;
    uint32_t bb = ((const AddrSym*)b)->addr;
    if (aa < bb) return -1;
    if (aa > bb) return 1;
    return 0;
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
// bounds until the next segment (the VM's own checked push()/pop() remain
// the runtime backstop for that region -- see bytecode_verify.h).
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
                                 Symbol* segment_symbol, Effect* eff) {
    const uint8_t* code = ctx->chunk->code;
    uint8_t opcode = code[pc];
    eff->req = 0;
    eff->delta = 0;
    eff->unknown_after = false;

    switch (opcode) {
        case RETURN:
        case EXIT: {
            bool is_function = segment_symbol && segment_symbol->type != TYPE_VOID;
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
                eff->delta = target->locals_count; // args stay in place, only new locals are pushed
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

// Successor PCs within the *current segment only*; a computed successor
// landing outside [start,end) is dropped rather than followed (segment
// boundaries are inferred from procedure_table addresses, not verified to
// be jump-tight, so this is a conservative safety valve, not a hard error).
static void addSuccessor(int target, const ProcSegment* seg, int* out, int* out_count) {
    if (target >= seg->start && target < seg->end) {
        out[(*out_count)++] = target;
    }
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

static bool verifySegment(VCtx* ctx, const ProcSegment* seg, uint8_t* visited, Depth* depths,
                           WorkItem* worklist, int worklist_cap) {
    for (int pc = seg->start; pc < seg->end; pc++) {
        visited[pc] = 0;
    }

    int wl_count = 0;
    worklist[wl_count++] = (WorkItem){ seg->start, (Depth){ true, 0 } };

    while (wl_count > 0) {
        WorkItem item = worklist[--wl_count];
        int pc = item.pc;
        if (pc < seg->start || pc >= seg->end || !ctx->boundary[pc]) continue;

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
        if (!classifyInstruction(ctx, pc, len, info, seg->symbol, &eff)) return false;

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
                if (nv > VM_STACK_MAX) {
                    return vfail(ctx, "pc %d: %s exceeds the maximum stack depth (%d > %d)",
                                 pc, info->name, nv, VM_STACK_MAX);
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
        if (opcode == JUMP) {
            int32_t disp = (int32_t)verifyReadU32BE(ctx->chunk->code, pc + 1);
            addSuccessor((int)((long)(pc + len) + disp), seg, succ, &succ_count);
        } else if (opcode == JUMP_IF_FALSE) {
            int32_t disp = (int32_t)verifyReadU32BE(ctx->chunk->code, pc + 1);
            addSuccessor((int)((long)(pc + len) + disp), seg, succ, &succ_count);
            addSuccessor(pc + len, seg, succ, &succ_count);
        } else {
            addSuccessor(pc + len, seg, succ, &succ_count);
        }

        for (int i = 0; i < succ_count; i++) {
            // wl_count is bounded by 2 * (instructions processed so far) + 1,
            // which is <= worklist_cap (2 * chunk->count + 8, see caller) for
            // any segment; this check is a defensive backstop, not expected
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
    collectProcedures(ctx->procedures, &addrs, &addr_count, &addr_cap);
    qsort(addrs, (size_t)addr_count, sizeof(AddrSym), cmpAddrSym);

    // Dedupe identical addresses (aliases resolving to the same target) and
    // drop any that fall outside the chunk (stale cross-chunk metadata).
    int n = 0;
    for (int i = 0; i < addr_count; i++) {
        if (addrs[i].addr >= (uint32_t)chunk->count) continue;
        if (n > 0 && addrs[n - 1].addr == addrs[i].addr) continue;
        addrs[n++] = addrs[i];
    }
    addr_count = n;

    int segment_count = (addrs != NULL && addr_count > 0 && addrs[0].addr == 0) ? addr_count : addr_count + 1;
    ProcSegment* segments = (ProcSegment*)calloc((size_t)segment_count, sizeof(ProcSegment));
    uint8_t* visited = (uint8_t*)calloc((size_t)(chunk->count > 0 ? chunk->count : 1), sizeof(uint8_t));
    Depth* depths = (Depth*)calloc((size_t)(chunk->count > 0 ? chunk->count : 1), sizeof(Depth));
    // Any single segment can span up to the whole chunk. Each pc can now be
    // *processed* (i.e. reach classifyInstruction and push successors) up to
    // twice -- once for a known depth, once for an unknown one (see
    // VISITED_KNOWN/VISITED_UNKNOWN above) -- and each processed pc pushes at
    // most two successors (JUMP_IF_FALSE), so 4*count+8 is a proven upper
    // bound reusable across every segment (see verifySegment()).
    int worklist_cap = chunk->count * 4 + 8;
    WorkItem* worklist = (WorkItem*)calloc((size_t)worklist_cap, sizeof(WorkItem));
    bool ok = true;

    if (!segments || !visited || !depths || !worklist) {
        ok = vfail(ctx, "out of memory building verifier segments");
        goto done;
    }

    int seg_idx = 0;
    int prev_addr = 0;
    bool have_zero = (addr_count > 0 && addrs[0].addr == 0);
    if (!have_zero) {
        segments[seg_idx].start = 0;
        segments[seg_idx].symbol = NULL;
        segments[seg_idx].end = (addr_count > 0) ? (int)addrs[0].addr : chunk->count;
        seg_idx++;
        prev_addr = segments[0].end;
    }
    for (int i = 0; i < addr_count; i++) {
        segments[seg_idx].start = (int)addrs[i].addr;
        segments[seg_idx].symbol = addrs[i].sym;
        segments[seg_idx].end = (i + 1 < addr_count) ? (int)addrs[i + 1].addr : chunk->count;
        seg_idx++;
    }
    (void)prev_addr;

    for (int i = 0; i < seg_idx; i++) {
        if (segments[i].start >= segments[i].end) continue; // empty segment, nothing to walk
        if (!verifySegment(ctx, &segments[i], visited, depths, worklist, worklist_cap)) {
            ok = false;
            goto done;
        }
    }

done:
    free(addrs);
    free(segments);
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
