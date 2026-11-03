// globals.c
// This file defines and initializes the global variables used throughout Pascal 

// Include the globals header file, which declares the global variables.
#include "core/globals.h"
#include <pthread.h>
#include <stdatomic.h>


// --- Global Variable Definitions and Initialization ---
// These variables are declared as 'extern' in globals.h.

// I/O and type conversion globals
PSCAL_THREAD_LOCAL int last_io_error = 0; // Error code of this thread's last I/O operation.
PSCAL_THREAD_LOCAL int typeWarn = 1; // Flag to control type warning messages (e.g., 1 for enabled, 0 for disabled).

#ifdef DEBUG
PSCAL_THREAD_LOCAL List *inserted_global_names = NULL; // Tracks globals inserted during debugging sessions.
#endif

// Symbol table globals - NOW POINTERS TO HASHTABLES.
// These will be initialized by calling createHashTable() in initSymbolSystem().
PSCAL_THREAD_LOCAL HashTable *globalSymbols = NULL; // Global symbol table (initialized to NULL, will point to a HashTable).
PSCAL_THREAD_LOCAL HashTable *localSymbols = NULL;  // Current local symbol table (initialized to NULL, will point to a HashTable).
PSCAL_THREAD_LOCAL HashTable *constGlobalSymbols = NULL; // Table of global constants (read-only at runtime)

// Pointer to the Symbol representing the currently executing function (for 'result' variable).

// Procedure table for storing information about declared procedures and functions.
// This remains a linked list of Procedure structs.
PSCAL_THREAD_LOCAL HashTable *procedure_table = NULL; // Initialized to NULL.
PSCAL_THREAD_LOCAL HashTable *current_procedure_table = NULL; // Pointer to current procedure scope.

// User-defined type table for storing information about declared types (records, enums, etc.).
// This remains a linked list of TypeEntry structs.
PSCAL_THREAD_LOCAL TypeEntry *type_table = NULL; // Initialized to NULL.


// --- CRT State Variable Definitions & Defaults ---
// These variables hold the current state for console/text rendering (colors, bold).
PSCAL_THREAD_LOCAL int gCurrentTextColor       = 7;       // Default foreground color (LightGray in 16-color palette).
PSCAL_THREAD_LOCAL int gCurrentTextBackground = 0;       // Default background color (Black in 16-color palette).
PSCAL_THREAD_LOCAL bool gCurrentTextBold      = false;   // Default text boldness state.
PSCAL_THREAD_LOCAL bool gCurrentColorIsExt    = false;   // Flag for extended 256-color foreground mode.
PSCAL_THREAD_LOCAL bool gCurrentBgIsExt       = false;   // Flag for extended 256-color background mode.
PSCAL_THREAD_LOCAL bool gCurrentTextUnderline = false;   // Default underline state.
PSCAL_THREAD_LOCAL bool gCurrentTextBlink     = false;   // Default blink state.
PSCAL_THREAD_LOCAL bool gConsoleAttrDirty     = false;   // Start with host terminal colors.
PSCAL_THREAD_LOCAL bool gConsoleAttrDirtyFromReset = false; // Track resets that need a reapply of custom colors.
PSCAL_THREAD_LOCAL bool gTextAttrInitialized  = false;   // Tracks if CRT.TextAttr has been explicitly set.
PSCAL_THREAD_LOCAL int gWindowLeft            = 1;
PSCAL_THREAD_LOCAL int gWindowTop             = 1;
PSCAL_THREAD_LOCAL int gWindowRight           = 80;
PSCAL_THREAD_LOCAL int gWindowBottom          = 24;
// --- End CRT State Variables ---

void crtAttributesCapture(CrtAttributes *out) {
    out->textColor = gCurrentTextColor;
    out->textBackground = gCurrentTextBackground;
    out->textBold = gCurrentTextBold;
    out->colorIsExt = gCurrentColorIsExt;
    out->bgIsExt = gCurrentBgIsExt;
    out->textUnderline = gCurrentTextUnderline;
    out->textBlink = gCurrentTextBlink;
    out->attrDirty = gConsoleAttrDirty;
    out->attrDirtyFromReset = gConsoleAttrDirtyFromReset;
    out->textAttrInitialized = gTextAttrInitialized;
    out->windowLeft = gWindowLeft;
    out->windowTop = gWindowTop;
    out->windowRight = gWindowRight;
    out->windowBottom = gWindowBottom;
}

void crtAttributesApply(const CrtAttributes *in) {
    gCurrentTextColor = in->textColor;
    gCurrentTextBackground = in->textBackground;
    gCurrentTextBold = in->textBold;
    gCurrentColorIsExt = in->colorIsExt;
    gCurrentBgIsExt = in->bgIsExt;
    gCurrentTextUnderline = in->textUnderline;
    gCurrentTextBlink = in->textBlink;
    gConsoleAttrDirty = in->attrDirty;
    gConsoleAttrDirtyFromReset = in->attrDirtyFromReset;
    gTextAttrInitialized = in->textAttrInitialized;
    gWindowLeft = in->windowLeft;
    gWindowTop = in->windowTop;
    gWindowRight = in->windowRight;
    gWindowBottom = in->windowBottom;
}

// Flag used by builtins like GraphLoop to signal a quit request from the user.
atomic_int break_requested = 0;
// Flag used by builtin 'exit' to request unwinding the current routine (not program termination).
PSCAL_THREAD_LOCAL int exit_requested = 0;
PSCAL_THREAD_LOCAL int gSuppressWriteSpacing = 0;
PSCAL_THREAD_LOCAL int gUppercaseBooleans = 0;
PSCAL_THREAD_LOCAL int pascal_semantic_pass_active = 0;
// Semantic/type error counter for the Pascal front end
PSCAL_THREAD_LOCAL int pascal_semantic_error_count = 0;
PSCAL_THREAD_LOCAL int pascal_parser_error_count = 0;


#ifdef DEBUG
// In DEBUG mode, this flag controls whether execution debugging output is enabled.
PSCAL_THREAD_LOCAL int dumpExec = 1;  // Set to 1 by default in debug mode.
#endif

PSCAL_THREAD_LOCAL Symbol *current_function_symbol = NULL;

// Note: Other global SDL/Audio variables declared in globals.h are typically
// defined and initialized in their respective .c files (sdl.c, audio.c).

// Mutex definition guarding shared global tables.  Use a recursive mutex so
// a thread may reacquire the lock when builtins invoke helpers that also
// touch global interpreter state.
#if defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER)
pthread_mutex_t globals_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
#elif defined(PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
pthread_mutex_t globals_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
pthread_mutex_t globals_mutex;
static void initGlobalsMutex(void) __attribute__((constructor));
static void initGlobalsMutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
#ifdef PTHREAD_MUTEX_RECURSIVE
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#else
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE_NP);
#endif
    pthread_mutex_init(&globals_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
}
#endif
