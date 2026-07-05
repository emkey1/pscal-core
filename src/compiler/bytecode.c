// src/compiler/bytecode.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memcpy
#include <stdint.h>

#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For freeValue, varTypeToString
#include "symbol/symbol.h" // For Symbol struct, HashTable, lookupSymbolIn
#include "vm/vm.h"         // For HostFunctionID type (used in CALL_HOST cast)
#include "core/globals.h"
#include "backend_ast/builtin.h"
#include "core/version.h"
#include "core/diagmap.h"

// initBytecodeChunk, freeBytecodeChunk, reallocate, writeBytecodeChunk,
// addConstantToChunk, emitShort, patchShort from your provided file.

// --- Opcode metadata (generated from compiler/opcodes.def) ---
static const OpcodeInfo kOpcodeInfoTable[OPCODE_COUNT] = {
#define OP(name, value, operands, stack_in, stack_out) \
    [value] = { #name, operands, stack_in, stack_out },
#include "compiler/opcodes.def"
#undef OP
};

const OpcodeInfo* pscalOpcodeInfo(uint8_t opcode) {
    if (opcode >= OPCODE_COUNT) {
        return NULL;
    }
    return &kOpcodeInfoTable[opcode];
}

int pscalOpcodeOperandSpecLength(const char* operands) {
    if (!operands) {
        return -1;
    }
    int length = 0;
    for (const char* p = operands; *p; ++p) {
        switch (*p) {
            case 'b':
            case 'i':
            case 'k':
                length += 1;
                break;
            case 'w':
            case 'j':
            case 'K':
                length += 2;
                break;
            case 'f':
                length += 4;
                break;
            case 'C':
                length += GLOBAL_INLINE_CACHE_SLOT_SIZE;
                break;
            default: // '?' (or anything unrecognized): variable-length payload
                return -1;
        }
    }
    return length;
}

void initBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    chunk->version = pscal_vm_version();
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->constants = NULL;
    chunk->builtin_lowercase_indices = NULL;
    chunk->builtin_resolved_ids = NULL;
    chunk->global_symbol_cache = NULL;
    chunk->source_path = NULL;
  //  chunk->lines = 0;
}

void freeBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    free(chunk->code);
    free(chunk->lines);
    free(chunk->source_path);
    for (int i = 0; i < chunk->constants_count; i++) {
        freeValue(&chunk->constants[i]);
    }
    free(chunk->constants);
    free(chunk->builtin_lowercase_indices);
    free(chunk->builtin_resolved_ids);
    free(chunk->global_symbol_cache);
    initBytecodeChunk(chunk);
}

const char* bytecodeDisplayNameForPath(const char* path) {
    if (!path) {
        return NULL;
    }

    const char* trimmed = path;
    const char* tests_sub = strstr(path, "/Tests/");
    if (tests_sub) {
        trimmed = tests_sub + 7; // skip "/Tests/"
    } else {
        const char* tests_back_sub = strstr(path, "\\Tests\\");
        if (tests_back_sub) {
            trimmed = tests_back_sub + 7; // skip "\\Tests\\"
        } else if (strncmp(path, "Tests/", 6) == 0) {
            trimmed = path + 6;
        } else if (strncmp(path, "Tests\\", 6) == 0) {
            trimmed = path + 6;
        }
    }

    while (*trimmed == '/' || *trimmed == '\\') {
        ++trimmed;
    }

    if (*trimmed == '\0') {
        return path;
    }
    return trimmed;
}

static void* reallocate(void* pointer, size_t oldSize, size_t newSize) { // From all.txt
    (void)oldSize;
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }
    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "Memory allocation error (realloc failed)\n");
        EXIT_FAILURE_HANDLER();
        return NULL;
    }
    return result;
}

static const char* formatInlineCachePointer(uintptr_t cached, char* buffer, size_t bufferSize) {
    if (cached == (uintptr_t)0) {
        return "0x0";
    }

    if (bufferSize == 0) {
        return "";
    }

    snprintf(buffer, bufferSize, "%p", (void*)cached);
    buffer[bufferSize - 1] = '\0';
    return buffer;
}

void setBytecodeChunkSourcePath(BytecodeChunk* chunk, const char* path) {
    char* copy = NULL;
    size_t len;

    if (!chunk) {
        return;
    }
    free(chunk->source_path);
    chunk->source_path = NULL;
    if (!path) {
        return;
    }
    len = strlen(path);
    copy = (char*)malloc(len + 1);
    if (!copy) {
        fprintf(stderr, "Memory allocation error (source path copy failed)\n");
        EXIT_FAILURE_HANDLER();
        return;
    }
    memcpy(copy, path, len + 1);
    chunk->source_path = copy;
}

void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line) { // From all.txt
    if (aetherHasRewriteLineMap()) {
        line = aetherMapRewrittenLineToSource(line);
    }
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->code = (uint8_t*)reallocate(chunk->code,
                                         sizeof(uint8_t) * oldCapacity,
                                         sizeof(uint8_t) * chunk->capacity);
        chunk->lines = (int*)reallocate(chunk->lines,
                                      sizeof(int) * oldCapacity,
                                      sizeof(int) * chunk->capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int addConstantToChunk(BytecodeChunk* chunk, const Value* value) {
#ifdef DEBUG
    fprintf(stderr, "[DEBUG addConstantToChunk] ENTER. Adding value type %s. chunk ptr: %p\n", varTypeToString(value->type), (void*)chunk);
    if (VALUE_TYPE(*value) == TYPE_STRING) {
        fprintf(stderr, "[DEBUG addConstantToChunk] String value to add: '%s'\n", AS_STRING(*value) ? AS_STRING(*value) : "NULL_SVAL");
    }
    fflush(stderr);
#endif

    // First, check if an identical constant already exists to avoid duplicates.
    for (int i = 0; i < chunk->constants_count; i++) {
        Value* existing = &chunk->constants[i];
        if (existing->type == value->type) {
            if (VALUE_TYPE(*existing) == TYPE_INTEGER && VAL_INT(*existing) == VAL_INT(*value)) return i;
            if (isRealType(existing->type) && AS_REAL(*existing) == AS_REAL(*value)) return i;
            if (VALUE_TYPE(*existing) == TYPE_STRING && AS_STRING(*existing) && AS_STRING(*value) && strcmp(AS_STRING(*existing), AS_STRING(*value)) == 0) return i;
            if (VALUE_TYPE(*existing) == TYPE_CHAR && AS_CHAR(*existing) == AS_CHAR(*value)) return i;
        }
    }

    if (chunk->constants_capacity < chunk->constants_count + 1) {
        int oldCapacity = chunk->constants_capacity;
        chunk->constants_capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->constants = (Value*)reallocate(chunk->constants,
                                             sizeof(Value) * oldCapacity,
                                             sizeof(Value) * chunk->constants_capacity);
        chunk->builtin_lowercase_indices = (int*)reallocate(chunk->builtin_lowercase_indices,
                                                           sizeof(int) * oldCapacity,
                                                           sizeof(int) * chunk->constants_capacity);
        chunk->builtin_resolved_ids = (int*)reallocate(chunk->builtin_resolved_ids,
                                                       sizeof(int) * oldCapacity,
                                                       sizeof(int) * chunk->constants_capacity);
        chunk->global_symbol_cache = (Symbol**)reallocate(chunk->global_symbol_cache,
                                                          sizeof(Symbol*) * oldCapacity,
                                                          sizeof(Symbol*) * chunk->constants_capacity);
        for (int i = oldCapacity; i < chunk->constants_capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
            chunk->builtin_resolved_ids[i] = -2;
            chunk->global_symbol_cache[i] = NULL;
        }
    } else if (!chunk->builtin_lowercase_indices && chunk->constants_capacity > 0) {
        chunk->builtin_lowercase_indices = (int*)reallocate(NULL,
                                                           0,
                                                           sizeof(int) * chunk->constants_capacity);
        for (int i = 0; i < chunk->constants_capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
        }
    }
    if (!chunk->builtin_resolved_ids && chunk->constants_capacity > 0) {
        chunk->builtin_resolved_ids = (int*)reallocate(NULL,
                                                       0,
                                                       sizeof(int) * chunk->constants_capacity);
        for (int i = 0; i < chunk->constants_capacity; ++i) {
            chunk->builtin_resolved_ids[i] = -2;
        }
    }
    if (!chunk->global_symbol_cache && chunk->constants_capacity > 0) {
        chunk->global_symbol_cache = (Symbol**)reallocate(NULL,
                                                          0,
                                                          sizeof(Symbol*) * chunk->constants_capacity);
        for (int i = 0; i < chunk->constants_capacity; ++i) {
            chunk->global_symbol_cache[i] = NULL;
        }
    }

    // Perform a deep copy from the provided pointer.
    int index = chunk->constants_count;
    chunk->constants[index] = makeCopyOfValue(value);
    if (chunk->builtin_lowercase_indices) {
        chunk->builtin_lowercase_indices[index] = -1;
    }
    if (chunk->builtin_resolved_ids) {
        chunk->builtin_resolved_ids[index] = -2;
    }
    if (chunk->global_symbol_cache) {
        chunk->global_symbol_cache[index] = NULL;
    }

    // The function NO LONGER frees the incoming value. The caller is responsible.

    return chunk->constants_count++;
}

void setBuiltinLowercaseIndex(BytecodeChunk* chunk, int original_idx, int lowercase_idx) {
    if (!chunk || original_idx < 0) {
        return;
    }
    if (!chunk->builtin_lowercase_indices) {
        int capacity = chunk->constants_capacity > 0 ? chunk->constants_capacity : chunk->constants_count;
        if (capacity <= 0) {
            return;
        }
        chunk->builtin_lowercase_indices = (int*)reallocate(NULL, 0, sizeof(int) * capacity);
        for (int i = 0; i < capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
        }
    }
    if (original_idx >= chunk->constants_capacity) {
        return;
    }
    chunk->builtin_lowercase_indices[original_idx] = lowercase_idx;
}

int getBuiltinLowercaseIndex(const BytecodeChunk* chunk, int original_idx) {
    if (!chunk || !chunk->builtin_lowercase_indices) {
        return -1;
    }
    if (original_idx < 0 || original_idx >= chunk->constants_count) {
        return -1;
    }
    return chunk->builtin_lowercase_indices[original_idx];
}

void emitShort(BytecodeChunk* chunk, uint16_t value, int line) { // From all.txt
    writeBytecodeChunk(chunk, (uint8_t)((value >> 8) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)(value & 0xFF), line);
}

void emitInt32(BytecodeChunk* chunk, uint32_t value, int line) {
    writeBytecodeChunk(chunk, (uint8_t)((value >> 24) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)((value >> 16) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)((value >> 8) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)(value & 0xFF), line);
}

void patchShort(BytecodeChunk* chunk, int offset_in_code, uint16_t value) {
    if (offset_in_code < 0 || (offset_in_code + 1) >= chunk->count) {
        fprintf(stderr, "Error: patchShort out of bounds. Offset: %d, Chunk count: %d.\n",
                offset_in_code, chunk->count);
        return;
    }
    chunk->code[offset_in_code]     = (uint8_t)((value >> 8) & 0xFF);
    chunk->code[offset_in_code + 1] = (uint8_t)(value & 0xFF);
}

void writeInlineCacheSlot(BytecodeChunk* chunk, int line) {
    if (!chunk) {
        return;
    }
    for (int i = 0; i < GLOBAL_INLINE_CACHE_SLOT_SIZE; ++i) {
        writeBytecodeChunk(chunk, 0, line);
    }
}

// Corrected helper function to find procedure/function name by its bytecode address
static const char* findProcedureNameByAddress(HashTable* procedureTable, uint16_t address) {
    if (!procedureTable) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol* symbol = procedureTable->buckets[i];
        while (symbol) {
            if (symbol->is_defined && symbol->bytecode_address == address) {
                return symbol->name;
            }
            symbol = symbol->next;
        }
    }
    return NULL;
}
// Length of the instruction at `offset`.  Fixed-length opcodes are driven by
// the operand-spec table generated from compiler/opcodes.def; the four
// variable-length opcodes ("?" specs) keep bespoke decode logic below.
int getInstructionLength(BytecodeChunk* chunk, int offset) {
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case INIT_FIELD_ARRAY:
        case INIT_LOCAL_ARRAY: {
            int current_pos = offset + 1; // after opcode
            current_pos++; // slot (INIT_LOCAL_ARRAY) / field index (INIT_FIELD_ARRAY)
            if (current_pos >= chunk->count) return 1;
            uint8_t dimension_count = chunk->code[current_pos++];
            current_pos += dimension_count * 4; // bounds indices (two 16-bit indices per dimension)
            current_pos += 3; // elem type and 2-byte elem type name index
            return current_pos - offset;
        }
        case DEFINE_GLOBAL: {
            // This instruction has a variable length.
            int current_pos = offset + 1; // Position after the opcode
            if (current_pos + 1 >= chunk->count) return 1; // Safeguard for incomplete instruction

            VarType declaredType = (VarType)chunk->code[offset + 2];
            current_pos = offset + 3; // Position after the type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // Skip over all the bounds indices
                    current_pos += 3; // Skip element VarType and 2-byte element type name index
                }
            } else {
                // Simple types store a 16-bit type name index. Strings add an
                // extra 16-bit length constant and files carry element metadata.
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                } else if (declaredType == TYPE_FILE) {
                    current_pos += 3; // element VarType byte + 2-byte element type name index
                }
            }
            return (current_pos - offset); // Return the total calculated length
        }
        case DEFINE_GLOBAL16: {
            // Similar to DEFINE_GLOBAL but with a 16-bit name index.
            int current_pos = offset + 1; // after opcode
            if (current_pos + 2 >= chunk->count) return 1; // ensure enough bytes for name and type
            VarType declaredType = (VarType)chunk->code[offset + 3];
            current_pos = offset + 4; // position after type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // bounds indices
                    current_pos += 3; // element var type and 2-byte element type name index
                }
            } else {
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                } else if (declaredType == TYPE_FILE) {
                    current_pos += 3; // element VarType byte + 2-byte element type name index
                }
            }
            return (current_pos - offset);
        }
        default: {
            const OpcodeInfo* info = pscalOpcodeInfo(instruction);
            if (!info) return 1; // unknown opcodes advance one byte
            int operand_len = pscalOpcodeOperandSpecLength(info->operands);
            return (operand_len < 0) ? 1 : 1 + operand_len;
        }
    }
}

// Utility helpers to print strings and characters with escape sequences so
// they don't introduce newlines in the disassembly output.
static void printEscapedString(const char* str) {
    if (!str) {
        fprintf(stderr, "NULL_STR");
        return;
    }
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '\n': fprintf(stderr, "\\n"); break;
            case '\r': fprintf(stderr, "\\r"); break;
            case '\t': fprintf(stderr, "\\t"); break;
            case '\\': fprintf(stderr, "\\\\"); break;
            case '\'': fputc('\'', stderr); break;
            default: fputc(*p, stderr); break;
        }
    }
}

static void printEscapedChar(char c) {
    switch (c) {
        case '\n': fprintf(stderr, "\\n"); break;
        case '\r': fprintf(stderr, "\\r"); break;
        case '\t': fprintf(stderr, "\\t"); break;
        case '\\': fprintf(stderr, "\\\\"); break;
        case '\'': fputc('\'', stderr); break;
        default: fprintf(stderr, "%c", c); break;
    }
}

static void printConstantValue(const Value* value) {
    if (!value) {
        fprintf(stderr, "<NULL>");
        return;
    }

    switch (value->type) {
        case TYPE_INTEGER:
            fprintf(stderr, "%lld", VAL_INT(*value));
            break;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            fprintf(stderr, "%Lf", AS_REAL(*value));
            break;
        case TYPE_STRING:
        case TYPE_UNICODE_STRING:
            if (AS_STRING(*value)) {
                printEscapedString(AS_STRING(*value));
            } else {
                fprintf(stderr, "NULL_STR");
            }
            break;
        case TYPE_CHAR:
            printEscapedChar(AS_CHAR(*value));
            break;
        case TYPE_BOOLEAN:
            fprintf(stderr, "%s", VAL_INT(*value) ? "true" : "false");
            break;
        case TYPE_NIL:
            fprintf(stderr, "nil");
            break;
        case TYPE_CLOSURE: {
            fprintf(stderr, "closure(entry=%u", AS_CLOSURE(*value).entry_offset);
            if (AS_CLOSURE(*value).symbol && AS_CLOSURE(*value).symbol->name) {
                fprintf(stderr, ", symbol=%s", AS_CLOSURE(*value).symbol->name);
            }
            if (AS_CLOSURE(*value).env) {
                fprintf(stderr, ", env=%p, slots=%u, ref=%u)",
                        (void*)AS_CLOSURE(*value).env,
                        (unsigned)AS_CLOSURE(*value).env->slot_count,
                        (unsigned)AS_CLOSURE(*value).env->refcount);
            } else {
                fprintf(stderr, ", env=NULL)");
            }
            break;
        }
        default:
            fprintf(stderr, "Value type %s", varTypeToString(value->type));
            break;
    }
}

static uintptr_t readInlineCachePtr(const BytecodeChunk* chunk, int offset) {
    uintptr_t value = 0;
    size_t to_copy = sizeof(value) < (size_t)GLOBAL_INLINE_CACHE_SLOT_SIZE
                         ? sizeof(value)
                         : (size_t)GLOBAL_INLINE_CACHE_SLOT_SIZE;
    memcpy(&value, chunk->code + offset, to_copy);
    return value;
}

static uint16_t readU16BE(const BytecodeChunk* chunk, int offset) {
    return (uint16_t)((chunk->code[offset] << 8) | chunk->code[offset + 1]);
}

// This is the function declared in bytecode.h and called by disassembleBytecodeChunk.
// Mnemonics and operand widths come from the compiler/opcodes.def metadata
// table; formatting that needs constant-pool context stays hand-written per
// opcode group below.
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable) {
    fprintf(stderr, "%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        fprintf(stderr, "   | ");
    } else {
        fprintf(stderr, "%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    const OpcodeInfo* info = pscalOpcodeInfo(instruction);

    // Every opcode without operands prints as its bare mnemonic.
    if (info && info->operands[0] == '\0') {
        fprintf(stderr, "%s\n", info->name);
        return offset + 1;
    }

    switch (instruction) {
        case CONSTANT:
        case CONSTANT16: {
            int wide = (instruction == CONSTANT16);
            unsigned constant_index = wide ? readU16BE(chunk, offset + 1)
                                           : chunk->code[offset + 1];
            int next = offset + (wide ? 3 : 2);
            fprintf(stderr, "%-16s %4u ", info->name, constant_index);
            if (constant_index >= (unsigned)chunk->constants_count) {
                fprintf(stderr, "<INVALID CONST IDX %u>\n", constant_index);
                return next;
            }
            fprintf(stderr, "'");
            Value constantValue = chunk->constants[constant_index];
            printConstantValue(&constantValue);
            fprintf(stderr, "'\n");
            return next;
        }
        case PUSH_IMMEDIATE_INT8: {
            uint8_t raw = chunk->code[offset + 1];
            int imm = (raw <= 0x7F) ? (int)raw : ((int)raw - 0x100);
            fprintf(stderr, "%-16s %4d\n", "PUSH_IMM_I8", imm);
            return offset + 2;
        }

        case JUMP_IF_FALSE:
        case JUMP: {
            int16_t jump_operand = (int16_t)readU16BE(chunk, offset + 1);
            int target_addr = offset + 3 + jump_operand;
            const char* targetName = findProcedureNameByAddress(procedureTable, target_addr);
            fprintf(stderr, "%-16s %4d (to %04d)", info->name, jump_operand, target_addr);
            if (targetName) {
                fprintf(stderr, " -> %s", targetName);
            }
            fprintf(stderr, "\n");
            return offset + 3;
        }

        case DEFINE_GLOBAL: {
            uint8_t name_idx = chunk->code[offset + 1];
            VarType declaredType = (VarType)chunk->code[offset + 2];
            fprintf(stderr, "%-16s NameIdx:%-3d ", "DEFINE_GLOBAL", name_idx);
            if (name_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[name_idx]) == TYPE_STRING) {
                fprintf(stderr, "'%s' ", AS_STRING(chunk->constants[name_idx]));
            } else {
                fprintf(stderr, "INVALID_NAME_IDX ");
            }
            fprintf(stderr, "Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 3;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    fprintf(stderr, "Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            uint16_t upper_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            fprintf(stderr, "%lld..%lld%s", VAL_INT(chunk->constants[lower_idx]), VAL_INT(chunk->constants[upper_idx]),
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    fprintf(stderr, "] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        fprintf(stderr, "%s ", varTypeToString(elem_var_type));
                        if (current_offset + 1 < chunk->count) {
                            uint16_t elem_name_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            if (elem_name_idx < chunk->constants_count &&
                                VALUE_TYPE(chunk->constants[elem_name_idx]) == TYPE_STRING) {
                                fprintf(stderr, "('%s')", AS_STRING(chunk->constants[elem_name_idx]));
                            }
                        }
                    }
                }
            } else {
                if (current_offset + 1 < chunk->count) {
                    uint16_t type_name_idx = readU16BE(chunk, current_offset);
                    current_offset += 2;
                    if (type_name_idx > 0 && type_name_idx < chunk->constants_count &&
                        VALUE_TYPE(chunk->constants[type_name_idx]) == TYPE_STRING) {
                        fprintf(stderr, "('%s')", AS_STRING(chunk->constants[type_name_idx]));
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx = readU16BE(chunk, current_offset);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[len_idx]) == TYPE_INTEGER) {
                            fprintf(stderr, " len=%lld", VAL_INT(chunk->constants[len_idx]));
                        }
                    } else if (declaredType == TYPE_FILE && current_offset + 2 < chunk->count) {
                        VarType elem_type = (VarType)chunk->code[current_offset++];
                        uint16_t elem_name_idx = readU16BE(chunk, current_offset);
                        current_offset += 2;
                        fprintf(stderr, " elem=%s", varTypeToString(elem_type));
                        if (elem_name_idx != 0xFFFF && elem_name_idx < chunk->constants_count &&
                            VALUE_TYPE(chunk->constants[elem_name_idx]) == TYPE_STRING) {
                            fprintf(stderr, " ('%s')", AS_STRING(chunk->constants[elem_name_idx]));
                        }
                    }
                }
            }
            fprintf(stderr, "\n");
            return current_offset;
        }
        case DEFINE_GLOBAL16: {
            // Variable or constant definition using a 16-bit name index.
            // NOTE: intentionally does not decode the TYPE_FILE element tail
            // (getInstructionLength does); preserved as-is for output parity.
            uint16_t name_idx = readU16BE(chunk, offset + 1);
            VarType declaredType = (VarType)chunk->code[offset + 3];
            fprintf(stderr, "%-16s NameIdx:%-3d ", "DEFINE_GLOBAL16", name_idx);
            if (name_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[name_idx]) == TYPE_STRING) {
                fprintf(stderr, "'%s' ", AS_STRING(chunk->constants[name_idx]));
            } else {
                fprintf(stderr, "INVALID_NAME_IDX ");
            }
            fprintf(stderr, "Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 4;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    fprintf(stderr, "Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            uint16_t upper_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            fprintf(stderr, "%lld..%lld%s", VAL_INT(chunk->constants[lower_idx]), VAL_INT(chunk->constants[upper_idx]),
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    fprintf(stderr, "] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        fprintf(stderr, "%s ", varTypeToString(elem_var_type));
                        if (current_offset + 1 < chunk->count) {
                            uint16_t elem_name_idx = readU16BE(chunk, current_offset);
                            current_offset += 2;
                            if (elem_name_idx < chunk->constants_count &&
                                VALUE_TYPE(chunk->constants[elem_name_idx]) == TYPE_STRING) {
                                fprintf(stderr, "('%s')", AS_STRING(chunk->constants[elem_name_idx]));
                            }
                        }
                    }
                }
            } else {
                if (current_offset + 1 < chunk->count) {
                    uint16_t type_name_idx = readU16BE(chunk, current_offset);
                    current_offset += 2;
                    if (type_name_idx > 0 && type_name_idx < chunk->constants_count &&
                        VALUE_TYPE(chunk->constants[type_name_idx]) == TYPE_STRING) {
                        fprintf(stderr, "('%s')", AS_STRING(chunk->constants[type_name_idx]));
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx = readU16BE(chunk, current_offset);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[len_idx]) == TYPE_INTEGER) {
                            fprintf(stderr, " len=%lld", VAL_INT(chunk->constants[len_idx]));
                        }
                    }
                }
            }
            fprintf(stderr, "\n");
            return current_offset;
        }

        case GET_GLOBAL:
        case SET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            const char* name = (name_index < chunk->constants_count &&
                                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                                AS_STRING(chunk->constants[name_index]))
                                   ? AS_STRING(chunk->constants[name_index])
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", info->name,
                    (unsigned)name_index, name, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL_CACHED:
        case SET_GLOBAL_CACHED: {
            uint8_t name_index = chunk->code[offset + 1];
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", info->name,
                    (unsigned)name_index, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL16:
        case SET_GLOBAL16: {
            uint16_t name_index = readU16BE(chunk, offset + 1);
            const char* name = (name_index < chunk->constants_count &&
                                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                                AS_STRING(chunk->constants[name_index]))
                                   ? AS_STRING(chunk->constants[name_index])
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", info->name,
                    (unsigned)name_index, name, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL16_CACHED:
        case SET_GLOBAL16_CACHED: {
            uint16_t name_index = readU16BE(chunk, offset + 1);
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", info->name,
                    (unsigned)name_index, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL_ADDRESS:
        case GET_GLOBAL_ADDRESS16: {
            int wide = (instruction == GET_GLOBAL_ADDRESS16);
            unsigned name_index = wide ? readU16BE(chunk, offset + 1)
                                       : chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d '%s'\n", info->name, (int)name_index,
                    AS_STRING(chunk->constants[name_index]));
            return offset + (wide ? 3 : 2);
        }

        case GET_LOCAL:
        case SET_LOCAL:
        case INC_LOCAL:
        case DEC_LOCAL:
        case GET_UPVALUE:
        case SET_UPVALUE:
        case GET_UPVALUE_ADDRESS:
        case RESET_LOCAL:
        case GET_LOCAL_ADDRESS: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", info->name, slot);
            return offset + 2;
        }

        case INIT_FIELD_ARRAY:
        case INIT_LOCAL_ARRAY: {
            uint8_t target = chunk->code[offset + 1];
            uint8_t dim_count = chunk->code[offset + 2];
            fprintf(stderr, "%-16s %s:%d Dims:%d", info->name,
                    (instruction == INIT_FIELD_ARRAY) ? "Field" : "Slot",
                    target, dim_count);
            int current_offset = offset + 3 + dim_count * 4;
            int next_offset = offset + 6 + dim_count * 4;
            if (current_offset < chunk->count) {
                VarType elem_type = (VarType)chunk->code[current_offset++];
                fprintf(stderr, " Elem:%s", varTypeToString(elem_type));
                if (current_offset + 1 < chunk->count) {
                    uint16_t elem_name_idx = readU16BE(chunk, current_offset);
                    current_offset += 2;
                    if (elem_name_idx != 0xFFFF &&
                        elem_name_idx < chunk->constants_count &&
                        VALUE_TYPE(chunk->constants[elem_name_idx]) == TYPE_STRING &&
                        AS_STRING(chunk->constants[elem_name_idx])) {
                        fprintf(stderr, " ('%s')", AS_STRING(chunk->constants[elem_name_idx]));
                    } else if (elem_name_idx != 0xFFFF) {
                        fprintf(stderr, " idx=%u", elem_name_idx);
                    }
                }
            }
            fprintf(stderr, "\n");
            return next_offset;
        }
        case INIT_LOCAL_FILE: {
            uint8_t slot = chunk->code[offset + 1];
            VarType elem_type = (VarType)chunk->code[offset + 2];
            uint16_t name_idx = readU16BE(chunk, offset + 3);
            fprintf(stderr, "%-16s %4d (slot) %-8s", "INIT_LOCAL_FILE", slot, varTypeToString(elem_type));
            if (name_idx != 0xFFFF) {
                fprintf(stderr, " idx=%u", name_idx);
                if (name_idx < chunk->constants_count && VALUE_TYPE(chunk->constants[name_idx]) == TYPE_STRING &&
                    AS_STRING(chunk->constants[name_idx])) {
                    fprintf(stderr, " '%s'", AS_STRING(chunk->constants[name_idx]));
                }
            }
            fprintf(stderr, "\n");
            return offset + 5;
        }
        case INIT_LOCAL_STRING: {
            uint8_t slot = chunk->code[offset + 1];
            uint8_t length = chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d (slot) %4d (len)\n", "INIT_LOCAL_STRING", slot, length);
            return offset + 3;
        }
        case INIT_LOCAL_POINTER: {
            uint8_t slot = chunk->code[offset + 1];
            uint16_t name_idx = readU16BE(chunk, offset + 2);
            fprintf(stderr, "%-16s %4d (slot) %4d", "INIT_LOCAL_POINTER", slot, name_idx);
            if (name_idx < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[name_idx]) == TYPE_STRING) {
                fprintf(stderr, " '%s'", AS_STRING(chunk->constants[name_idx]));
            }
            fprintf(stderr, "\n");
            return offset + 4;
        }

        case GET_FIELD_ADDRESS:
        case GET_FIELD_ADDRESS_KEEP:
        case LOAD_FIELD_VALUE_BY_NAME:
        case GET_FIELD_ADDRESS16:
        case GET_FIELD_ADDRESS_KEEP16:
        case LOAD_FIELD_VALUE_BY_NAME16: {
            int wide = (info->operands[0] == 'K');
            unsigned const_idx = wide ? readU16BE(chunk, offset + 1)
                                      : chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d ", info->name, (int)const_idx);
            if (const_idx < (unsigned)chunk->constants_count &&
                VALUE_TYPE(chunk->constants[const_idx]) == TYPE_STRING) {
                fprintf(stderr, "'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                fprintf(stderr, "<INVALID FIELD CONST>\n");
            }
            return offset + (wide ? 3 : 2);
        }

        case ALLOC_OBJECT:
        case ALLOC_OBJECT16: {
            int wide = (instruction == ALLOC_OBJECT16);
            unsigned fields = wide ? readU16BE(chunk, offset + 1)
                                   : chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (fields)\n", info->name, (int)fields);
            return offset + (wide ? 3 : 2);
        }
        case GET_FIELD_OFFSET:
        case GET_FIELD_OFFSET16:
        case LOAD_FIELD_VALUE:
        case LOAD_FIELD_VALUE16: {
            int wide = (info->operands[0] == 'w');
            unsigned idx = wide ? readU16BE(chunk, offset + 1)
                                : chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (index)\n", info->name, (int)idx);
            return offset + (wide ? 3 : 2);
        }
        case GET_ELEMENT_ADDRESS:
        case LOAD_ELEMENT_VALUE: {
            uint8_t dims = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (dims)\n", info->name, dims);
            return offset + 2;
        }
        case GET_ELEMENT_ADDRESS_CONST:
        case LOAD_ELEMENT_VALUE_CONST: {
            uint32_t flat = ((uint32_t)chunk->code[offset + 1] << 24) |
                            ((uint32_t)chunk->code[offset + 2] << 16) |
                            ((uint32_t)chunk->code[offset + 3] << 8) |
                            (uint32_t)chunk->code[offset + 4];
            fprintf(stderr, "%-16s %10u (flat offset)\n", info->name, flat);
            return offset + 5;
        }

        case CALL_BUILTIN: {
            uint16_t name_index = readU16BE(chunk, offset + 1);
            uint8_t arg_count = chunk->code[offset + 3];
            const char* name = "<INVALID>";
            if (name_index < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }
            const char* lower_name = NULL;
            int lower_idx = getBuiltinLowercaseIndex(chunk, (int)name_index);
            if (lower_idx >= 0 && lower_idx < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[lower_idx]) == TYPE_STRING &&
                AS_STRING(chunk->constants[lower_idx])) {
                lower_name = AS_STRING(chunk->constants[lower_idx]);
            }
            if (lower_name && name && strcmp(name, lower_name) != 0) {
                fprintf(stderr, "%-16s %5d '%s' (lower='%s') (%d args)\n",
                        "CALL_BUILTIN", name_index, name, lower_name, arg_count);
            } else {
                fprintf(stderr, "%-16s %5d '%s' (%d args)\n",
                        "CALL_BUILTIN", name_index, name, arg_count);
            }
            return offset + 4;
        }
        case CALL_BUILTIN_PROC: {
            uint16_t builtin_id = readU16BE(chunk, offset + 1);
            uint16_t name_index = readU16BE(chunk, offset + 3);
            uint8_t arg_count = chunk->code[offset + 5];
            const char* name = NULL;
            if (name_index < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }
            if (!name) {
                name = getVmBuiltinNameById((int)builtin_id);
            }
            if (!name) name = "<UNKNOWN>";
            fprintf(stderr, "%-16s %5u '%s' (%u args)\n",
                    "CALL_BUILTIN_PROC", builtin_id, name, arg_count);
            return offset + 6;
        }
        case CALL_USER_PROC: {
            uint16_t name_index = readU16BE(chunk, offset + 1);
            uint8_t arg_count = chunk->code[offset + 3];
            const char* name = NULL;
            if (name_index < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }

            const char* display_name = name ? name : "<INVALID>";
            int entry_address = -1;
            if (procedureTable && name && *name) {
                char lookup_name[MAX_SYMBOL_LENGTH + 1];
                strncpy(lookup_name, name, MAX_SYMBOL_LENGTH);
                lookup_name[MAX_SYMBOL_LENGTH] = '\0';
                toLowerString(lookup_name);
                Symbol* sym = hashTableLookup(procedureTable, lookup_name);
                sym = resolveSymbolAlias(sym);
                if (sym && sym->is_defined) {
                    entry_address = sym->bytecode_address;
                }
            }

            if (entry_address >= 0) {
                fprintf(stderr, "%-16s %5u '%s' @%04d (%u args)\n",
                        "CALL_USER_PROC", name_index, display_name,
                        (uint16_t)entry_address, arg_count);
            } else {
                fprintf(stderr, "%-16s %5u '%s' (%u args)\n",
                        "CALL_USER_PROC", name_index, display_name, arg_count);
            }
            return offset + 4;
        }
        case CALL_HOST: {
            uint8_t host_fn_id_val = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (ID: %d)\n", "CALL_HOST", host_fn_id_val, host_fn_id_val);
            return offset + 2;
        }
        case CALL: {
            uint16_t name_index = readU16BE(chunk, offset + 1);
            uint16_t address = readU16BE(chunk, offset + 3);
            uint8_t declared_arity = chunk->code[offset + 5];
            const char* targetProcName = "<INVALID>";
            if (name_index < chunk->constants_count &&
                VALUE_TYPE(chunk->constants[name_index]) == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                targetProcName = AS_STRING(chunk->constants[name_index]);
            }
            fprintf(stderr, "%-16s %04d (%s) (%d args)\n",
                    "CALL", address, targetProcName, declared_arity);
            return offset + 6;
        }
        case CALL_INDIRECT:
        case PROC_CALL_INDIRECT: {
            uint8_t arg_count = chunk->code[offset + 1];
            fprintf(stderr, "%-16s (args=%d)\n", info->name, arg_count);
            return offset + 2;
        }
        case FORMAT_VALUE: {
            uint8_t width = chunk->code[offset+1];
            uint8_t precision = chunk->code[offset+2];
            fprintf(stderr, "%-16s width:%d prec:%d\n", "FORMAT_VALUE", width, (int8_t)precision);
            return offset + 3;
        }
        case THREAD_CREATE: {
            uint16_t entry = readU16BE(chunk, offset + 1);
            fprintf(stderr, "%-16s %04d\n", "THREAD_CREATE", entry);
            return offset + 3;
        }

        // CALL_METHOD has never had a disassembly case; it intentionally
        // still falls through to the default (output parity with the
        // hand-written switch this replaced).
        default:
            fprintf(stderr, "Unknown opcode %d\n", instruction);
            return offset + 1;
    }
}

void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable) {
    fprintf(stderr, "== Disassembly: %s ==\n", name);
    fprintf(stderr, "Offset Line Opcode           Operand  Value / Target (Args)\n");
    fprintf(stderr, "------ ---- ---------------- -------- --------------------------\n");

    for (int offset = 0; offset < chunk->count; ) {
        const char* procNameAtOffset = findProcedureNameByAddress(procedureTable, offset);
        if (procNameAtOffset) {
            const char* routineTypeStr = "Routine"; // Default
            if (procedureTable) {
                Symbol* sym = lookupSymbolIn(procedureTable, procNameAtOffset); // Assumes lookupSymbolIn is robust
                if (sym && sym->type_def) {
                    if (sym->type_def->type == AST_FUNCTION_DECL) {
                        routineTypeStr = "Function";
                    } else if (sym->type_def->type == AST_PROCEDURE_DECL) {
                        routineTypeStr = "Procedure";
                    }
                }
            }
            fprintf(stderr, "\n");
            fprintf(stderr, "--- %s %s (at %04d) ---\n", routineTypeStr, procNameAtOffset, offset);
        }
        // Call the public, enhanced disassembleInstruction
        offset = disassembleInstruction(chunk, offset, procedureTable);
    }
    fprintf(stderr, "== End Disassembly: %s ==\n\n", name);

    if (chunk->constants_count > 0) {
        fprintf(stderr, "Constants (%d):\\n", chunk->constants_count);
        for (int i = 0; i < chunk->constants_count; i++) {
            fprintf(stderr, "  %04d: ", i);
            Value constantValue = chunk->constants[i];
            switch(constantValue.type) {
                case TYPE_INTEGER:
                    fprintf(stderr, "INT   %lld\n", VAL_INT(constantValue));
                    break;
                case TYPE_FLOAT:
                case TYPE_DOUBLE:
                case TYPE_LONG_DOUBLE:
                    fprintf(stderr, "REAL  %Lf\n", AS_REAL(constantValue));
                    break;
                case TYPE_STRING:
                case TYPE_UNICODE_STRING:
                    fprintf(stderr, "STR   \"");
                    if (AS_STRING(constantValue)) {
                        printEscapedString(AS_STRING(constantValue));
                    } else {
                    fprintf(stderr, "NULL_STR");
                    }
                    fprintf(stderr, "\"");
                    int lower_idx = getBuiltinLowercaseIndex(chunk, i);
                    if (lower_idx >= 0 && lower_idx < chunk->constants_count &&
                        VALUE_TYPE(chunk->constants[lower_idx]) == TYPE_STRING &&
                        AS_STRING(chunk->constants[lower_idx]) &&
                        (!AS_STRING(constantValue) || strcmp(AS_STRING(constantValue), AS_STRING(chunk->constants[lower_idx])) != 0)) {
                        fprintf(stderr, " (lower -> %04d: \"", lower_idx);
                        printEscapedString(AS_STRING(chunk->constants[lower_idx]));
                        fprintf(stderr, "\"");
                    }
                    fprintf(stderr, "\n");
                    break;
                case TYPE_CHAR:
                case TYPE_WIDECHAR:
                    fprintf(stderr, "CHAR  '");
                    printEscapedChar(AS_CHAR(constantValue));
                    fprintf(stderr, "'\n");
                    break;
                case TYPE_BOOLEAN:
                    fprintf(stderr, "BOOL  %s\n", VAL_INT(constantValue) ? "true" : "false");
                    break;
                case TYPE_CLOSURE:
                    fprintf(stderr, "CLOS  ");
                    printConstantValue(&constantValue);
                    fprintf(stderr, "\n");
                    break;
                case TYPE_NIL:
                    fprintf(stderr, "NIL\n");
                    break;
                default:
                    fprintf(stderr, "Value type %s\n", varTypeToString(constantValue.type));
                    break;
            }
        }
        fprintf(stderr, "\n");
    }
}
