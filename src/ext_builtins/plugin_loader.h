#ifndef PSCAL_EXT_PLUGIN_LOADER_H
#define PSCAL_EXT_PLUGIN_LOADER_H

// VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1): dlopen-based plugin loading
// on top of the frozen pscal_ext_api.h ABI. Desktop/server only -- iOS has
// no dlopen culture and is statically linked (already special-cased in
// ext_builtins/register.c for the in-tree categories); every function here
// is still callable on iOS but the .c file behind it compiles a no-op stub
// there instead of the real dlopen path, so iOS builds neither drag in
// dlopen nor silently misbehave (Docs/pscal_vm2_plan.md §9 risk register).

#include <stdbool.h>

/* Record a plugin path supplied via --ext <path> (repeatable) or the
 * PSCAL_EXT_DIR environment variable's directory scan. Safe to call before
 * any VM/symbol-table init has run -- this only appends a string to an
 * internal list; the actual dlopen happens later, from
 * pscalExtLoadRegisteredPlugins(). */
void pscalExtAddPluginPath(const char *path);

/* Shared CLI wiring, mirroring vm_fx_policy.h's pscalFxIsCliFlag/
 * pscalFxHandleCliFlag pattern exactly, so the frontend main()s that already
 * recognize --deny/--fx-record/--fx-replay this way gain --ext the same
 * way -- one more `else if` block, no new parsing infrastructure. */
bool pscalExtIsCliFlag(const char *arg);
bool pscalExtHandleCliFlag(const char *flag, const char *value);

/* Loads every plugin path registered so far (via --ext and/or PSCAL_EXT_DIR)
 * and calls each one's pscal_ext_register() entry point against the host
 * API. Must be called AFTER initSymbolSystem() (registerVmBuiltin's
 * compile-time half touches the global symbol/procedure hash tables) --
 * called from registerExtendedBuiltinsOnce() (ext_builtins/register.c),
 * the same pthread_once-guarded, exactly-once seam the static in-tree
 * categories already use, right after their registration. A process with
 * no --ext flag and no PSCAL_EXT_DIR set pays one static-bool check and
 * returns; this is a true no-op for every program that doesn't opt in.
 *
 * Load/registration failures (unopenable file, missing entry point, ABI
 * major mismatch, or the entry point itself crashing/returning nonzero --
 * see plugin_loader.c's fork-based crash isolation) print a diagnostic to
 * stderr and terminate the process (exit(EXIT_FAILURE)): a program that
 * explicitly asked for a plugin via --ext/PSCAL_EXT_DIR and didn't get it
 * should fail loudly at startup, not run degraded with mysterious
 * "unknown identifier" errors deep in its own source later. */
void pscalExtLoadRegisteredPlugins(void);

#endif /* PSCAL_EXT_PLUGIN_LOADER_H */
