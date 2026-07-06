#ifndef PSCAL_CACHE_H
#define PSCAL_CACHE_H

#include <stdbool.h>
#include <stdio.h>
#include "compiler/bytecode.h"
#include "symbol/symbol.h"

bool loadBytecodeFromCache(const char* source_path,
                           const char* compiler_id,
                           const char* frontend_path,
                           const char** dependencies,
                           int dep_count,
                           BytecodeChunk* chunk);
void saveBytecodeToCache(const char* source_path, const char* compiler_id, const BytecodeChunk* chunk);
bool saveBytecodeToFile(const char* file_path, const char* source_path, const BytecodeChunk* chunk);
bool loadBytecodeFromFile(const char* file_path, BytecodeChunk* chunk);

// Reads a chunk straight off disk without running the Phase 2b global-slot
// link step or the Phase 1e verifier -- the raw, pre-link encoding a fresh
// compile would have produced (DEFINE_GLOBAL_SLOT/GET_GSLOT/SET_GSLOT/
// GET_GSLOT_ADDRESS operands are still constant-pool NAME indices, not
// slots). For tools that need to round-trip bytecode byte-for-byte, e.g.
// pscald --emit-asm: linking is an in-memory rewrite of those operand
// fields, so disassembling post-link output and reassembling it verbatim
// would bake resolved slot indices back in as if they were name indices.
bool loadBytecodeFromFileUnlinked(const char* file_path, BytecodeChunk* chunk);

// Build the canonical path for the cache file corresponding to a source path.
// Caller is responsible for freeing the returned string.
char* buildCachePath(const char* source_path, const char* compiler_id);

// Rebuild constructor aliases in procedure tables so fresh-compile and
// cache-loaded execution paths share identical callable symbol aliases.
void restoreProcedureConstructorAliases(HashTable* table);

// VM 2.0 Phase 6 (Docs/pscal_vm2_plan.md §6.3): expose the PSB3 value codec
// (writeValue/readValue) as a self-framed (length-prefixed) record for the
// record/replay journal, so the journal reuses the same Value serialization
// as the bytecode cache instead of inventing a second scheme. Returns false
// on an unsupported Value variant (write) or a truncated/corrupt frame
// (read); the caller decides how to degrade (record: skip journaling this
// call's result; replay: abort cleanly).
bool pscalCacheWriteValueFramed(FILE* out, const Value* v);
bool pscalCacheReadValueFramed(FILE* in, Value* out);

#endif // PSCAL_CACHE_H
