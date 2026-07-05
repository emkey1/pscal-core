// src/compiler/bytecode_link.h
#ifndef PSCAL_BYTECODE_LINK_H
#define PSCAL_BYTECODE_LINK_H

#include "compiler/bytecode.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// VM 2.0 Phase 2b (plan Docs/pscal_vm2_plan.md §5.7): the load-time link
// step. Walks `chunk`'s CODE section once, resolving every
// DEFINE_GLOBAL_SLOT/GET_GSLOT/SET_GSLOT/GET_GSLOT_ADDRESS instruction's
// name-index operand (as emitted by compiler.c, into the constant pool) to
// a slot index, rewriting that operand in place (same byte width, so no
// other offset in the chunk moves), and populating chunk->global_slots and
// friends. Idempotent (a chunk that already has globals_linked=true is left
// untouched and returns true immediately) and safe to call on untrusted
// input: every name index it reads is defensively bounds-checked here,
// since this runs BEFORE the Phase 1e verifier on the cache/file-load path
// (see the .c file's module comment for why that ordering, not the
// reverse, is correct).
//
// Must be called exactly once per chunk before the chunk is verified
// (loadBytecodeFromCache()/loadBytecodeFromFile(), cache.c) or executed
// (compileASTToBytecode(), compiler.c, for the fresh-compile path that
// never goes through cache.c at all). Requires the process's
// `globalSymbols`/`constGlobalSymbols` tables (core/globals.h) to already
// contain every const-global this chunk references -- true by construction
// on both paths: compileASTToBytecode()'s semantic pass and cache.c's
// PROCS-section reader both populate them before this runs.
//
// On failure (malformed chunk only -- never expected for trusted compiler
// output), returns false and writes a message to err_buf/err_buf_size (may
// be NULL/0 to suppress); the chunk must not be executed or verified.
bool pscalLinkGlobalSlots(BytecodeChunk* chunk, char* err_buf, size_t err_buf_size);

#ifdef __cplusplus
}
#endif

#endif // PSCAL_BYTECODE_LINK_H
