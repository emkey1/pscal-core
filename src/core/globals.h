// src/globals.h
#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdio.h>  // For fprintf, stderr
#include <stdlib.h> // For exit, EXIT_FAILURE
#include <pthread.h> // For mutex guarding of global tables
#include <stdatomic.h> // For atomic flags shared across threads

#include "core/types.h" // Provides TypeEntry, Value, List, AST forward decl etc.
#ifdef SDL
#include "backend_ast/pscal_sdl_runtime.h"   // For SDL related externs or types if any directly in globals.h
                   // (It's better if specific SDL globals are in sdl.h and sdl.c)
#endif

// --- EXIT_FAILURE_HANDLER Macro ---
// iOS builds should not call exit(3) from library code; force suppression there.
#if defined(PSCAL_TARGET_IOS) && !defined(SUPPRESS_EXIT)
#define SUPPRESS_EXIT
#endif

#ifdef SUPPRESS_EXIT
    #define EXIT_FAILURE_HANDLER() fprintf(stderr, "Suppressed exit call from %s:%d\n", __FILE__, __LINE__)
#else
    #define EXIT_FAILURE_HANDLER() exit(EXIT_FAILURE)
#endif

#define MAX_SYMBOL_LENGTH 255
#define MAX_ID_LENGTH 256

// Per-run state. On iOS each command runs on a thread of one process (a pipeline's
// stages, a terminal's tabs), so a frontend's compile state and anything else that
// belongs to one run needs this. A mutable static in the compile path without it
// crashes `clike a | clike b`: Tests/ios_host_statics.sh fails on a new one. It is
// empty on hosts, and a VM worker has its own copy (see ThreadJob in vm.c).
#ifndef PSCAL_THREAD_LOCAL
#if defined(PSCAL_TARGET_IOS)
#if defined(__cplusplus)
#define PSCAL_THREAD_LOCAL thread_local
#else
#define PSCAL_THREAD_LOCAL _Thread_local
#endif
#else
#define PSCAL_THREAD_LOCAL
#endif
#endif

// --- Forward Declarations and Typedefs needed by this file ---
// These types are defined in symbol.h
struct Symbol_s;
typedef struct Symbol_s Symbol;

struct SymbolTable_s;
typedef struct SymbolTable_s HashTable;

// TypeEntry is now fully defined in "types.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Global Variable EXTERN Declarations ---
extern PSCAL_THREAD_LOCAL HashTable *globalSymbols;
extern PSCAL_THREAD_LOCAL HashTable *constGlobalSymbols;
extern PSCAL_THREAD_LOCAL HashTable *localSymbols;
extern PSCAL_THREAD_LOCAL Symbol *current_function_symbol;

extern PSCAL_THREAD_LOCAL HashTable *procedure_table; // Procedure table is now a HashTable
extern PSCAL_THREAD_LOCAL HashTable *current_procedure_table; // Pointer to current procedure scope
extern PSCAL_THREAD_LOCAL TypeEntry *type_table;      // TypeEntry definition comes from types.h

// --- CRT State Variables ---
extern PSCAL_THREAD_LOCAL int gCurrentTextColor;
extern PSCAL_THREAD_LOCAL int gCurrentTextBackground;
extern PSCAL_THREAD_LOCAL bool gCurrentTextBold;
extern PSCAL_THREAD_LOCAL bool gCurrentColorIsExt;
extern PSCAL_THREAD_LOCAL bool gCurrentBgIsExt;
extern PSCAL_THREAD_LOCAL bool gCurrentTextUnderline;
extern PSCAL_THREAD_LOCAL bool gCurrentTextBlink;
extern PSCAL_THREAD_LOCAL bool gConsoleAttrDirty;
extern PSCAL_THREAD_LOCAL bool gConsoleAttrDirtyFromReset;
extern PSCAL_THREAD_LOCAL bool gTextAttrInitialized;
extern PSCAL_THREAD_LOCAL int gWindowLeft;
extern PSCAL_THREAD_LOCAL int gWindowTop;
extern PSCAL_THREAD_LOCAL int gWindowRight;
extern PSCAL_THREAD_LOCAL int gWindowBottom;

// The state above, for a thread that starts under another's: a VM worker
// begins with what its spawner had, and from then on each has its own.
typedef struct CrtAttributes {
    int textColor;
    int textBackground;
    bool textBold;
    bool colorIsExt;
    bool bgIsExt;
    bool textUnderline;
    bool textBlink;
    bool attrDirty;
    bool attrDirtyFromReset;
    bool textAttrInitialized;
    int windowLeft;
    int windowTop;
    int windowRight;
    int windowBottom;
} CrtAttributes;
void crtAttributesCapture(CrtAttributes *out);
void crtAttributesApply(const CrtAttributes *in);

// --- Other Globals ---
#define MAX_RECURSION_DEPTH 10
extern PSCAL_THREAD_LOCAL int gParamCount;
extern PSCAL_THREAD_LOCAL char **gParamValues;
extern PSCAL_THREAD_LOCAL bool gParamValuesOwned;
// extern AST *globalRoot; // Defined in main.c typically

// Per thread, like Free Pascal's InOutRes: Write/BlockRead/BlockWrite set
// it without a lock, so as a process global one thread's I/O clobbered
// another thread's IOResult.
extern PSCAL_THREAD_LOCAL int last_io_error;
extern PSCAL_THREAD_LOCAL int typeWarn;

#ifdef DEBUG
extern PSCAL_THREAD_LOCAL int dumpExec;
// Assuming List is defined in types.h or list.h (which types.h might include)
extern PSCAL_THREAD_LOCAL List *inserted_global_names;
#endif

extern atomic_int break_requested;
extern PSCAL_THREAD_LOCAL int exit_requested; // Flag set by builtin 'exit' to unwind the current routine
// These belong to one frontend run, and on iOS two runs share the process
// (`pascal a | clike b`, two tabs). A VM worker takes its run's values from
// its ThreadJob.
extern PSCAL_THREAD_LOCAL int gSuppressWriteSpacing; // Non-zero disables automatic spacing between write arguments
extern PSCAL_THREAD_LOCAL int gUppercaseBooleans;    // Non-zero prints TRUE/FALSE instead of true/false
extern PSCAL_THREAD_LOCAL int pascal_semantic_pass_active; // Non-zero only during the main Pascal semantic-analysis pass
extern PSCAL_THREAD_LOCAL int pascal_semantic_error_count; // Count of semantic/type errors during analysis
extern PSCAL_THREAD_LOCAL int pascal_parser_error_count;   // Count of parser (syntax) errors

// Mutex protecting shared global tables
extern pthread_mutex_t globals_mutex;

#define DEFAULT_STRING_CAPACITY 255

// Snapshot structure for saving/restoring local symbol environments
typedef struct SymbolEnvSnapshot {
    HashTable *head; // Uses HashTable typedef
} SymbolEnvSnapshot;

#ifdef __cplusplus
}
#endif

#endif /* GLOBALS_H */
