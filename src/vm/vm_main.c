#include "core/cache.h"
#include "core/utils.h"
#include "core/list.h"
#include "vm/vm.h"
#include "core/globals.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "vm/vm_fx_policy.h"
#include "ext_builtins/plugin_loader.h"
#include "common/frontend_kind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Local copy of initSymbolSystem from main.c to set up tables. */
static void initSymbolSystem(void) {
#ifdef DEBUG
    inserted_global_names = createList();
#endif
    globalSymbols = createHashTable();
    if (!globalSymbols) {
        fprintf(stderr, "FATAL: Failed to create global symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    insertStandardStreamSymbols();

    constGlobalSymbols = createHashTable();
    if (!constGlobalSymbols) {
        fprintf(stderr, "FATAL: Failed to create constant symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }

    procedure_table = createHashTable();
    if (!procedure_table) {
        fprintf(stderr, "FATAL: Failed to create procedure hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    current_procedure_table = procedure_table;
#ifdef SDL
    initializeTextureSystem();
#endif
}

static const char *PSCALVM_USAGE = "Usage: pscalvm <bytecode_file> [program_parameters...]\n";

int pscalvm_main(int argc, char* argv[]) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define PSCALVM_RETURN(value)           \
    do {                                \
        int __vm_rc = (value);          \
        frontendPopKind(previousKind);  \
        return __vm_rc;                 \
    } while (0)
    vmInitTerminalState();
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("%s", PSCALVM_USAGE);
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }

    int argi = 1;
    while (argi < argc && (pscalFxIsCliFlag(argv[argi]) || pscalExtIsCliFlag(argv[argi]))) {
        const char *value = (argi + 1 < argc) ? argv[argi + 1] : NULL;
        bool ok = pscalFxIsCliFlag(argv[argi]) ? pscalFxHandleCliFlag(argv[argi], value)
                                                : pscalExtHandleCliFlag(argv[argi], value);
        if (!ok) {
            PSCALVM_RETURN(vmExitWithCleanup(EXIT_FAILURE));
        }
        argi += 2;
    }

    if (argi >= argc) {
        fprintf(stderr, "%s", PSCALVM_USAGE);
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    const char* bytecode_path = argv[argi];
    gParamCount = argc - (argi + 1);
    gParamValues = (gParamCount > 0) ? &argv[argi + 1] : NULL;

    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    if (!loadBytecodeFromFile(bytecode_path, &chunk)) {  // reports why itself
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    /* Adopt the conventions of the frontend that compiled this chunk, which
     * the PSB3 header records (cache.c). pscalvm is the one host with no
     * frontend of its own, so until this existed it ran everything as Pascal
     * and silently applied Pascal's rules to bytecode compiled under someone
     * else's: an Aether chunk indexes Text from 0, so `loop ch in s` died on
     * "String index 0 out of bounds", and copy()/pos() were off by one. The
     * string base is the loudest of these, but not the only one -- array
     * out-of-bounds diagnostics and the thread-introspection builtins are
     * frontend-specific too. FRONTEND_KIND_UNKNOWN means the producer
     * didn't record one (a hand-assembled .asm, tools/tiny); leaving Pascal
     * pushed for those keeps their long-standing behaviour exactly. */
    if (chunk.frontend_kind != FRONTEND_KIND_UNKNOWN) {
        frontendPushKind(chunk.frontend_kind);
    }

    VM vm;
    initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);

    PSCALVM_RETURN(vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE));
}
#undef PSCALVM_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char* argv[]) {
    return pscalvm_main(argc, argv);
}
#endif
