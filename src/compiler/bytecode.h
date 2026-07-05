// src/compiler/bytecode.h
//
//  bytecode.h
//  Pscal
//
//  Created by Michael Miller on 5/18/25.
//

#ifndef PSCAL_BYTECODE_H
#define PSCAL_BYTECODE_H

#include "core/types.h" // For Value struct, as constants will be Values
#include "symbol/symbol.h" // For HashTable definition

// VM 2.0 Phase 1e legacy width: GET_GLOBAL_CACHED/SET_GLOBAL_CACHED/
// GET_GLOBAL16_CACHED/SET_GLOBAL16_CACHED (opcodes.def 0x28-0x2B) reserved
// this many in-stream bytes for a self-patched Symbol* before VM 2.0 Phase
// 2a moved the cache out to a per-chunk side table (CacheSlot, below). Those
// four opcodes are retired holes now -- never emitted, never executed -- but
// their operand-spec strings ("kC"/"KC") keep this macro alive so a legacy
// standalone .bc disassembly still reports their original 10/11-byte width
// instead of misreading it.
#define GLOBAL_INLINE_CACHE_SLOT_SIZE 8
_Static_assert(sizeof(Symbol*) <= GLOBAL_INLINE_CACHE_SLOT_SIZE,
               "GLOBAL_INLINE_CACHE_SLOT_SIZE is too small for Symbol* pointers");

// VM 2.0 Phase 2a: per-chunk global-access inline-cache side table (plan
// Docs/pscal_vm2_plan.md §5.6). GET_GLOBAL/SET_GLOBAL/GET_GLOBAL16/
// SET_GLOBAL16 each carry a cache_id:u16 operand (opcodes.def's 'c' spec
// letter) indexing this table instead of embedding a patchable Symbol* in
// the instruction stream. `symbol` is NULL until the first successful
// resolution of that call site's global name.
typedef struct {
    struct Symbol_s* symbol;
} CacheSlot;

// --- Opcode Definitions ---
// The opcode page is generated from compiler/opcodes.def — the single source
// of truth for opcode names, ordinals, operand encodings and stack effects.
// Add or change opcodes THERE, never here.  Per-instruction semantics and
// payload layouts are documented in opcodes.def and in
// Docs/pscal_vm_manual/pscal_vm_manual_ch3.md.
typedef enum {
#define OP(name, value, operands, stack_in, stack_out) name = value,
#include "compiler/opcodes.def"
#undef OP
    OPCODE_COUNT        // Total number of opcodes (must remain last)
} OpCode;

// Every published ordinal is pinned: editing a value in opcodes.def (or
// hand-editing this header) breaks the build instead of silently renumbering
// the ISA that on-disk bytecode caches depend on.
#define OP(name, value, operands, stack_in, stack_out) \
    _Static_assert((int)name == (value), \
                   "opcode " #name " drifted from its published ordinal " #value);
#include "compiler/opcodes.def"
#undef OP

enum {
    PSCAL_OPCODE_DEF_ENTRY_COUNT = 0
#define OP(name, value, operands, stack_in, stack_out) + 1
#include "compiler/opcodes.def"
#undef OP
};
_Static_assert(PSCAL_OPCODE_DEF_ENTRY_COUNT == OPCODE_COUNT,
               "opcodes.def must define every ordinal exactly once (no holes, no duplicates)");
_Static_assert(OPCODE_COUNT == 0x64,
               "published opcode page is 0x00-0x63; append new opcodes after RESET_LOCAL "
               "and update this assert deliberately");

// Per-opcode metadata generated from compiler/opcodes.def.  `operands` is the
// encoding-spec string documented at the top of opcodes.def ("?" = variable
// length with bespoke decode logic); stack_in/stack_out use -1 for
// operand-dependent effects.
typedef struct {
    const char* name;
    const char* operands;
    int8_t stack_in;
    int8_t stack_out;
} OpcodeInfo;

// Returns the metadata row for an opcode, or NULL if it is not a valid opcode.
const OpcodeInfo* pscalOpcodeInfo(uint8_t opcode);
// Total operand bytes implied by an encoding-spec string, or -1 for
// variable-length ("?") specs.
int pscalOpcodeOperandSpecLength(const char* operands);

// --- Bytecode Chunk Structure ---
// A "chunk" represents a compiled piece of code (e.g., a procedure, function, or the main program block)
typedef struct {
    uint32_t version;   // VM bytecode version this chunk targets
    int count;          // Number of bytes currently in use in 'code'
    int capacity;       // Allocated capacity for 'code'
    uint8_t* code;      // The array of bytecode instructions and operands

    int constants_count; // Number of constants currently in use
    int constants_capacity; // Allocated capacity for 'constants'
    Value* constants;   // Array of constants (Value structs: numbers, strings)
    int* builtin_lowercase_indices; // Maps string constant indices to their lowercase copies (-1 if not a builtin)
    int* builtin_resolved_ids; // Maps string constant indices to resolved builtin ids (-2 unknown, -1 unresolved)

    int cache_count;    // Number of GET/SET_GLOBAL[16] cache sites the compiler emitted (compile-time constant)
    CacheSlot* caches;  // Per-chunk runtime side table, sized cache_count; allocated lazily on first execution
    bool prepared_for_execution; // One-time-init guard for `caches` allocation (and, in PSCAL_VM_CODE_PROTECT
                                 // builds, for mprotect'ing `code` read-only) -- see interpretBytecode()
    bool code_is_mapped;   // true if `code` is an mmap'd buffer (PSCAL_VM_CODE_PROTECT) rather than malloc'd
    size_t code_map_size;  // valid only when code_is_mapped

    // Optional: For debugging runtime errors
    char* source_path;     // Owning source path label for human-facing diagnostics
    int* lines;         // Array storing the source line number for each byte of code
} BytecodeChunk;

// --- Function Prototypes for BytecodeChunk ---
void initBytecodeChunk(BytecodeChunk* chunk);
void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line); // Add byte to chunk
void freeBytecodeChunk(BytecodeChunk* chunk);
int addConstantToChunk(BytecodeChunk* chunk, const Value* value); // Add a value to constant pool, return index
void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable);
const char* bytecodeDisplayNameForPath(const char* path);
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable);
void emitShort(BytecodeChunk* chunk, uint16_t value, int line);
void emitInt32(BytecodeChunk* chunk, uint32_t value, int line);
void patchShort(BytecodeChunk* chunk, int offset_in_code, uint16_t value);
void patchInt32(BytecodeChunk* chunk, int offset_in_code, uint32_t value);
int getInstructionLength(BytecodeChunk* chunk, int offset);
// Shared decode logic behind getInstructionLength(); see bytecode.c for the
// exact contract. Used by the Phase 1e verifier (bytecode_verify.c) to tell
// truncated variable-length instructions apart from well-formed ones.
bool pscalDecodeInstructionLength(const BytecodeChunk* chunk, int offset, int* out_length);
void setBuiltinLowercaseIndex(BytecodeChunk* chunk, int original_idx, int lowercase_idx);
int getBuiltinLowercaseIndex(const BytecodeChunk* chunk, int original_idx);
void setBytecodeChunkSourcePath(BytecodeChunk* chunk, const char* path);
// Releases chunk->code, whether it's a plain malloc/realloc'd buffer or an
// mmap'd one (see pscalProtectChunkCode). Every teardown path that used to
// free(chunk->code) directly must go through this instead.
void pscalReleaseChunkCode(BytecodeChunk* chunk);
// VM 2.0 Phase 2a (plan §5.6): mprotect(PROT_READ) chunk->code in
// PSCAL_VM_CODE_PROTECT builds; a no-op returning true otherwise. See
// bytecode.c for the full contract.
bool pscalProtectChunkCode(BytecodeChunk* chunk);

#endif // PSCAL_BYTECODE_H
