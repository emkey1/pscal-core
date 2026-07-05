// src/compiler/bytecode_link.c
//
// VM 2.0 Phase 2b (plan Docs/pscal_vm2_plan.md §5.7): the load-time link
// step for slot-addressed globals. See bytecode_link.h for the contract.
//
// Ordering vs the Phase 1e verifier (bytecode_verify.c): THIS RUNS FIRST.
// The compiler emits DEFINE_GLOBAL_SLOT/GET_GSLOT/SET_GSLOT/GET_GSLOT_ADDRESS
// with a raw constant-pool NAME index in the operand position the 's' spec
// letter documents (opcodes.def); this file rewrites that same 2-byte
// field, in place, into a resolved SLOT index before anything else touches
// the chunk. The verifier's 's'-spec check (checkSlotIndex, bytecode_verify.c)
// validates slot < chunk->global_slot_count -- a check that is meaningless
// until this has run and populated global_slot_count. There is no way to
// order this the other way around: the verifier cannot validate "is this a
// valid slot" before slots exist, and linking cannot be skipped for
// untrusted input either (GET_GSLOT is simply not executable without it).
// So every name index this file reads is read BEFORE verification and must
// be defensively bounds-checked here, not assumed valid -- the same
// posture cache.c's section-directory validation and PROCS-section reader
// already take for data consumed ahead of the verifier.
//
// This does NOT reintroduce Phase 2a's "self-modifying code" problem
// (goal G2): the rewrite happens once, at load time, on a plain malloc'd
// buffer, strictly before pscalProtectChunkCode() ever mprotects it
// PROT_READ (that call happens later, in interpretBytecode()'s prologue).
// It is a linker relocation pass, not runtime self-modification -- the
// same distinction the plan draws by calling this a "link step" rather
// than caching.
//
// Called from two places, both before first execution: cache.c's
// loadBytecodeFromCache()/loadBytecodeFromFile() (link then verify), and
// compiler.c's compileASTToBytecode() (link only -- fresh compiler output
// is trusted and already skips verification entirely, matching the
// pre-existing trust model). Both paths converge on identical linked
// bytecode before interpretBytecode() ever sees it.
//
// exsh is entirely untouched by any of this: its independent codegen
// (components/exsh/src/shell/codegen.c) never emits GET_GLOBAL/SET_GLOBAL/
// DEFINE_GLOBAL (or their slot-addressed successors) at all -- shell
// variables, being creatable at runtime, go through CALL_HOST/CALL_BUILTIN
// dispatch instead. The dual-path design the plan anticipated ("exsh keeps
// a name-addressed escape hatch") turned out to be unnecessary: exsh never
// used the name-addressed opcode family to begin with.

#include "compiler/bytecode_link.h"
#include "core/globals.h"
#include "core/types.h"
#include "core/utils.h"
#include "symbol/symbol.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void linkFail(char* err_buf, size_t err_buf_size, const char* fmt, ...) {
    if (!err_buf || !err_buf_size) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err_buf, err_buf_size, fmt, ap);
    va_end(ap);
}

typedef struct {
    char* name; // owned, lowercased
    bool is_const;
} SlotEntry;

typedef struct {
    SlotEntry* items;
    int count;
    int capacity;
} SlotList;

static void slotListFree(SlotList* list) {
    for (int i = 0; i < list->count; i++) free(list->items[i].name);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int slotListFind(SlotList* list, const char* lname) {
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->items[i].name, lname) == 0) return i;
    }
    return -1;
}

// Returns the slot index for `lname` (already lowercased), creating one if
// this is the first reference. Returns -1 on allocation failure.
static int slotListIntern(SlotList* list, const char* lname, bool is_const) {
    int idx = slotListFind(list, lname);
    if (idx >= 0) {
        if (is_const) list->items[idx].is_const = true;
        return idx;
    }
    if (list->count == list->capacity) {
        int new_cap = list->capacity == 0 ? 16 : list->capacity * 2;
        SlotEntry* grown = (SlotEntry*)realloc(list->items, sizeof(SlotEntry) * (size_t)new_cap);
        if (!grown) return -1;
        list->items = grown;
        list->capacity = new_cap;
    }
    char* copy = strdup(lname);
    if (!copy) return -1;
    list->items[list->count].name = copy;
    list->items[list->count].is_const = is_const;
    return list->count++;
}

static uint16_t linkReadU16BE(const uint8_t* code, int pos) {
    return (uint16_t)(((uint16_t)code[pos] << 8) | (uint16_t)code[pos + 1]);
}

static void linkWriteU16BE(uint8_t* code, int pos, uint16_t value) {
    code[pos] = (uint8_t)(value >> 8);
    code[pos + 1] = (uint8_t)(value & 0xFF);
}

// Resolves the constant-pool string at `name_idx`, defensively. Returns
// NULL (and sets *ok = false) if out of range or not a string -- the
// caller treats this as a link failure, matching how the verifier would
// have rejected the equivalent GET_GLOBAL/16 name index pre-Phase-2b.
static const char* linkResolveName(BytecodeChunk* chunk, uint32_t name_idx, bool* ok) {
    if (name_idx >= (uint32_t)chunk->constants_count) {
        *ok = false;
        return NULL;
    }
    Value* v = &chunk->constants[(int)name_idx];
    if (VALUE_TYPE(*v) != TYPE_STRING || !AS_STRING(*v)) {
        *ok = false;
        return NULL;
    }
    *ok = true;
    return AS_STRING(*v);
}

// Length of a DEFINE_GLOBAL_SLOT instruction at `pc` (opcode already known).
// Identical shape to the retired DEFINE_GLOBAL16's payload with the name
// field repurposed as a slot field -- duplicated from (rather than shared
// with) pscalDecodeInstructionLength()'s DEFINE_GLOBAL16 case because that
// generic decoder is not safe to invoke mid-link for other opcodes yet (see
// module comment: this function itself performs the bounds-checking the
// verifier would normally have done first). Keep the two in sync if the
// DEFINE_GLOBAL_SLOT payload shape ever changes.
static bool defineGlobalSlotPayloadLength(const BytecodeChunk* chunk, int pc, int* out_len) {
    const uint8_t* code = chunk->code;
    int count = chunk->count;
    if (pc + 3 >= count) return false; // need slot(2) + type(1) at minimum
    VarType declared = (VarType)code[pc + 3];
    int cursor = pc + 4;
    if (declared == TYPE_ARRAY) {
        if (cursor >= count) return false;
        uint8_t dims = code[cursor++];
        cursor += (int)dims * 4;
        cursor += 3; // elem type byte + 2-byte elem type name index
    } else {
        cursor += 2; // type name index
        if (declared == TYPE_STRING) {
            cursor += 2; // length constant index
        } else if (declared == TYPE_FILE) {
            cursor += 3; // elem type byte + 2-byte elem type name index
        }
    }
    if (cursor > count) return false;
    *out_len = cursor - pc;
    return true;
}

// Interns the name at the 2-byte field `pc+1` (reading it as a raw
// constant-pool name index) and rewrites that field in place with the
// resolved slot index. Shared by the DEFINE_GLOBAL_SLOT and
// GET_GSLOT/SET_GSLOT/GET_GSLOT_ADDRESS cases below.
static bool linkNameFieldToSlot(BytecodeChunk* chunk, SlotList* list, int pc,
                                 const char* opcode_name, char* err_buf, size_t err_buf_size) {
    bool name_ok = false;
    const char* name = linkResolveName(chunk, linkReadU16BE(chunk->code, pc + 1), &name_ok);
    if (!name_ok) {
        linkFail(err_buf, err_buf_size, "pc %d: %s name index out of range", pc, opcode_name);
        return false;
    }
    char lname[MAX_SYMBOL_LENGTH + 1];
    strncpy(lname, name, MAX_SYMBOL_LENGTH);
    lname[MAX_SYMBOL_LENGTH] = '\0';
    toLowerString(lname);
    int slot = slotListIntern(list, lname, false);
    if (slot < 0) {
        linkFail(err_buf, err_buf_size, "pc %d: out of memory linking global slots", pc);
        return false;
    }
    linkWriteU16BE(chunk->code, pc + 1, (uint16_t)slot);
    return true;
}

bool pscalLinkGlobalSlots(BytecodeChunk* chunk, char* err_buf, size_t err_buf_size) {
    if (err_buf && err_buf_size) err_buf[0] = '\0';
    if (!chunk) return true;
    if (chunk->globals_linked) return true; // idempotent

    if (chunk->count == 0 || !chunk->code) {
        chunk->global_slot_count = 0;
        chunk->global_slots = NULL;
        chunk->global_slot_is_const = NULL;
        chunk->global_slot_names = NULL;
        chunk->global_myself_slot = -1;
        chunk->global_pas_exc_pending_slot = -1;
        chunk->global_pas_exc_message_slot = -1;
        chunk->globals_linked = true;
        return true;
    }

    SlotList list = {0};
    bool ok = true;

    int pc = 0;
    while (ok && pc < chunk->count) {
        uint8_t opcode = chunk->code[pc];
        int len = 1;

        if (opcode == DEFINE_GLOBAL_SLOT) {
            if (!defineGlobalSlotPayloadLength(chunk, pc, &len)) {
                linkFail(err_buf, err_buf_size, "pc %d: truncated DEFINE_GLOBAL_SLOT instruction", pc);
                ok = false;
                break;
            }
            if (!linkNameFieldToSlot(chunk, &list, pc, "DEFINE_GLOBAL_SLOT", err_buf, err_buf_size)) {
                ok = false;
                break;
            }
        } else if (opcode == GET_GSLOT || opcode == SET_GSLOT || opcode == GET_GSLOT_ADDRESS) {
            len = 3; // opcode + u16 ('s' spec), fixed width
            if (pc + len > chunk->count) {
                linkFail(err_buf, err_buf_size, "pc %d: truncated global-slot instruction", pc);
                ok = false;
                break;
            }
            const char* opname = (opcode == GET_GSLOT) ? "GET_GSLOT"
                                : (opcode == SET_GSLOT) ? "SET_GSLOT"
                                                         : "GET_GSLOT_ADDRESS";
            if (!linkNameFieldToSlot(chunk, &list, pc, opname, err_buf, err_buf_size)) {
                ok = false;
                break;
            }
        } else {
            const OpcodeInfo* info = pscalOpcodeInfo(opcode);
            if (!info) {
                linkFail(err_buf, err_buf_size, "pc %d: undefined opcode 0x%02X", pc, opcode);
                ok = false;
                break;
            }
            if (!pscalDecodeInstructionLength(chunk, pc, &len) || len <= 0 || pc + len > chunk->count) {
                linkFail(err_buf, err_buf_size, "pc %d: truncated %s instruction", pc, info->name);
                ok = false;
                break;
            }
        }
        pc += len;
    }

    if (!ok) {
        slotListFree(&list);
        return false;
    }

    int count = list.count;
    GlobalSlot* slots = (GlobalSlot*)calloc((size_t)(count > 0 ? count : 1), sizeof(GlobalSlot));
    bool* is_const = (bool*)calloc((size_t)(count > 0 ? count : 1), sizeof(bool));
    char** names = (char**)calloc((size_t)(count > 0 ? count : 1), sizeof(char*));
    if (!slots || !is_const || !names) {
        free(slots);
        free(is_const);
        free(names);
        slotListFree(&list);
        linkFail(err_buf, err_buf_size, "out of memory allocating global slot table");
        return false;
    }

    // Eagerly resolve any slot whose Symbol already exists at link time.
    // This is NOT limited to genuine constants: enum members and
    // unit-interface symbols (exportEnumMembersFromNode()/linkUnit(),
    // core/utils.c) are registered directly into `globalSymbols` at
    // COMPILE TIME -- with no corresponding DEFINE_GLOBAL_SLOT instruction
    // ever emitted for them -- so a slot resolved only against
    // DEFINE_GLOBAL_SLOT occurrences would stay permanently unpopulated for
    // those names. `constGlobalSymbols` is checked first, matching the
    // pre-2b GET_GLOBAL/GET_GLOBAL16 lookup order (const table first, plain
    // global table second); the two can legitimately hold two *different*
    // Symbol objects for the same const name on the cache-load path
    // (readProcsSection calls both insertGlobalSymbol() and
    // insertConstGlobalSymbol(), the latter always allocating its own
    // Symbol -- see symbol.c), so which one wins matters. A name found in
    // neither table (the common case: an ordinary `var` declaration) is
    // left with symbol=NULL here, to be populated by DEFINE_GLOBAL_SLOT's
    // handler the first (and only) time it executes for that slot.
    int myself_slot = -1, pending_slot = -1, message_slot = -1;
    for (int i = 0; i < count; i++) {
        names[i] = list.items[i].name; // ownership transferred, do not free via slotListFree below

        Symbol* existing = constGlobalSymbols ? hashTableLookup(constGlobalSymbols, names[i]) : NULL;
        if (!existing && globalSymbols) {
            existing = hashTableLookup(globalSymbols, names[i]);
        }
        if (existing) {
            slots[i].symbol = existing;
            is_const[i] = existing->is_const;
        }

        if (strcmp(names[i], "myself") == 0) myself_slot = i;
        else if (strcmp(names[i], "__pas_exc_pending") == 0) pending_slot = i;
        else if (strcmp(names[i], "__pas_exc_message") == 0) message_slot = i;
    }
    free(list.items); // names[] now owns the strings; only the array itself is freed here

    chunk->global_slot_count = count;
    chunk->global_slots = slots;
    chunk->global_slot_is_const = is_const;
    chunk->global_slot_names = names;
    chunk->global_myself_slot = myself_slot;
    chunk->global_pas_exc_pending_slot = pending_slot;
    chunk->global_pas_exc_message_slot = message_slot;
    chunk->globals_linked = true;
    return true;
}
