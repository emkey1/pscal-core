// src/compiler/bytecode_verify.h
//
// Load-time bytecode verifier (VM 2.0 plan, Docs/pscal_vm2_plan.md §5.5).
// Runs once per chunk load (cache hits and standalone .bc files, see
// core/cache.c) before any instruction executes. On success the chunk is
// safe to run without the interpreter's own defensive checks needing to be
// the only line of defense; on failure the caller must treat the chunk as
// unusable (fall back to recompiling from source, or refuse to run a
// standalone .bc), never partially trust it.

#ifndef PSCAL_BYTECODE_VERIFY_H
#define PSCAL_BYTECODE_VERIFY_H

#include "compiler/bytecode.h"
#include "symbol/symbol.h"

// Verifies `chunk`:
//   1. Every byte in the code section belongs to a fully in-bounds
//      instruction with a defined opcode; every jump/call/thread-entry
//      target lands on an instruction boundary.
//   2. Every constant-pool index, host-function id, and code address
//      operand (including those inside the "?" variable-length payloads)
//      is in range.
//   3. Per procedure (as delimited by `procedures`' bytecode_address
//      entries, plus the implicit top-level entry at pc 0), an abstract
//      walk of the operand stack never goes negative, never exceeds
//      VM_STACK_MAX, and agrees at control-flow join points. Call targets
//      that cannot be resolved statically (closures, virtual dispatch,
//      CALL_HOST) are tracked as "unknown" rather than guessed at -- the
//      runtime's own checked push()/pop() remain the backstop there (see
//      opcodes.def's Phase 1e audit comment for the full per-opcode
//      rationale).
//
// `procedures` is the global procedure table (already populated from the
// PROC section by the time this is called). On failure, a human-readable
// reason is written to err_buf (if non-NULL/non-zero-size) and false is
// returned; err_buf is always NUL-terminated on failure.
bool pscalVerifyBytecodeChunk(const BytecodeChunk* chunk, HashTable* procedures,
                               char* err_buf, size_t err_buf_size);

#endif // PSCAL_BYTECODE_VERIFY_H
