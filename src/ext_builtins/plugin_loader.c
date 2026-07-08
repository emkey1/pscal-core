// src/ext_builtins/plugin_loader.c
//
// VM 2.0 Phase 7 (Docs/pscal_vm2_plan.md §7.1). See plugin_loader.h for the
// public surface and ordering contract (must run after initSymbolSystem()).
#include "ext_builtins/plugin_loader.h"

#include "backend_ast/pscal_ext_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// pscal_ext_api.c's accessor -- deliberately not exposed in pscal_ext_api.h
// (a plugin receives the host pointer as pscal_ext_register()'s argument;
// it has no business calling this itself).
const PscalExtHostApi *pscalExtGetHostApi(void);

#if defined(__APPLE__)
#define PSCAL_EXT_PLUGIN_SUFFIX ".dylib"
#else
#define PSCAL_EXT_PLUGIN_SUFFIX ".so"
#endif

// ---------------------------------------------------------------------------
// Growable list of plugin paths collected during CLI parsing (--ext) and/or
// a PSCAL_EXT_DIR directory scan, consumed once by
// pscalExtLoadRegisteredPlugins(). Single-threaded by construction: both
// CLI parsing and the pthread_once-guarded load happen on the frontend's
// main thread before any user program (and its threads/tasks) starts
// running, so no lock is needed here.
static char **g_ext_plugin_paths = NULL;
static size_t g_ext_plugin_count = 0;
static size_t g_ext_plugin_capacity = 0;

void pscalExtAddPluginPath(const char *path) {
    if (!path || !*path) {
        return;
    }
    if (g_ext_plugin_count == g_ext_plugin_capacity) {
        size_t new_capacity = g_ext_plugin_capacity ? g_ext_plugin_capacity * 2 : 4;
        char **grown = (char **)realloc(g_ext_plugin_paths, new_capacity * sizeof(char *));
        if (!grown) {
            fprintf(stderr, "Error: out of memory registering plugin path '%s'.\n", path);
            return;
        }
        g_ext_plugin_paths = grown;
        g_ext_plugin_capacity = new_capacity;
    }
    g_ext_plugin_paths[g_ext_plugin_count++] = strdup(path);
}

bool pscalExtIsCliFlag(const char *arg) {
    return arg && strcmp(arg, "--ext") == 0;
}

bool pscalExtHandleCliFlag(const char *flag, const char *value) {
    if (!flag || !value) {
        fprintf(stderr, "Error: %s requires an argument.\n", flag ? flag : "(null)");
        return false;
    }
    if (strcmp(flag, "--ext") != 0) {
        fprintf(stderr, "Error: unrecognized plugin flag '%s'.\n", flag);
        return false;
    }
    // Fail fast on an obvious typo instead of deferring to the later load
    // phase, matching --fx-record/--fx-replay's early-open-failure
    // behavior (vm_fx_policy.c) -- the file must exist and be readable now,
    // even though the real dlopen happens later.
    FILE *probe = fopen(value, "rb");
    if (!probe) {
        fprintf(stderr, "Error: --ext: cannot open '%s'.\n", value);
        return false;
    }
    fclose(probe);
    pscalExtAddPluginPath(value);
    return true;
}

#if defined(PSCAL_TARGET_IOS)

// iOS: no dlopen culture, statically linked only (Docs/pscal_vm2_plan.md §9
// risk register). --ext/PSCAL_EXT_DIR are recognized (so a shared script
// invoking pscalvm/pascal/etc. with --ext doesn't get "unknown option" on
// iOS) but produce a clean, immediate, unambiguous rejection rather than a
// silently-compiled-but-nonfunctional dlopen path.
void pscalExtLoadRegisteredPlugins(void) {
    if (g_ext_plugin_count == 0 && !getenv("PSCAL_EXT_DIR")) {
        return;
    }
    fprintf(stderr,
            "Error: dlopen plugin loading (--ext/PSCAL_EXT_DIR) is not supported on this "
            "platform (iOS is static-registration-only). Remove --ext / unset PSCAL_EXT_DIR.\n");
    exit(EXIT_FAILURE);
}

#else /* !PSCAL_TARGET_IOS: real dlopen-backed loading */

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool pscalExtPathHasSuffix(const char *path, const char *suffix) {
    size_t plen = strlen(path);
    size_t slen = strlen(suffix);
    return plen >= slen && strcmp(path + (plen - slen), suffix) == 0;
}

// Scans PSCAL_EXT_DIR (if set) for platform shared-library files and adds
// each as a plugin path, exactly as if it had been passed via --ext. Not
// recursive; unreadable/non-library entries are silently skipped (a
// directory of mixed content, e.g. README files alongside .dylibs, is a
// normal thing to point PSCAL_EXT_DIR at).
static void pscalExtScanConfiguredDirectory(void) {
    const char *dir_path = getenv("PSCAL_EXT_DIR");
    if (!dir_path || !*dir_path) {
        return;
    }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Error: PSCAL_EXT_DIR '%s': %s\n", dir_path, strerror(errno));
        exit(EXIT_FAILURE);
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (!pscalExtPathHasSuffix(entry->d_name, PSCAL_EXT_PLUGIN_SUFFIX)) {
            continue;
        }
        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        pscalExtAddPluginPath(full_path);
    }
    closedir(dir);
}

// Exit codes the forked probe child reports via _exit(); the parent
// interprets these (plus WIFSIGNALED) to produce a clean diagnostic. Kept
// distinct from a plugin's own registration-failure convention (any
// nonzero pscal_ext_register return) which is folded into
// PSCAL_EXT_PROBE_ENTRY_REJECTED.
typedef enum {
    PSCAL_EXT_PROBE_OK = 0,
    PSCAL_EXT_PROBE_DLOPEN_FAILED = 2,
    PSCAL_EXT_PROBE_NO_ENTRY_POINT = 3,
    PSCAL_EXT_PROBE_ENTRY_REJECTED = 4
} PscalExtProbeResult;

// ---------------------------------------------------------------------------
// The PscalExtHostApi handed to the entry point ONLY inside the forked
// probe child (below): identical to the real host API except for five
// fields (register_builtin/register_category/register_group/
// register_function_entry/runtime_error), which are inert no-op stubs.
//
// Why those five specifically: pscalExtLoadRegisteredPlugins() runs nested
// deep inside populateBuiltinRegistry() (backend_ast/builtin.c), which
// holds its own recursive `builtin_registry_mutex` for that entire call.
// Recursive pthread mutexes are NOT forkable while held -- the mutex's
// owner bookkeeping is thread/process-identity based, so a child that
// tries to re-enter it (as register_builtin -> registerVmBuiltin ->
// registerBuiltinFunction does) deadlocks forever on a lock whose "owner"
// no longer resolves in the child process. Found empirically (a hung
// `pascal --ext goodplugin.dylib ...` smoke test; `sample` showed the
// child stuck in _pthread_mutex_firstfit_lock_wait inside
// registerBuiltinFunction). Those five calls are the only ones that touch
// that shared, already-locked global state.
//
// Everything else on the vtable -- make_*/as_*/is_*_type (pure value
// construction/inspection, no shared state) and the handle-table family
// (operates only on the PscalExtHandleTable the plugin itself creates,
// backed by a mutex initialized fresh in this call, never inherited
// pre-locked from the parent) -- is copied from the REAL host API and
// used for real inside the probe child. This matters in practice: a
// plugin that checks `handle_table_create()`'s result for NULL (as the
// sqlite-as-plugin proof does) would otherwise fail the probe every time
// even though the real, unforked load would have succeeded -- an inert
// stub there produces a false negative, not a safety margin.
static void pscalExtStubRegisterBuiltin(const char *n, PscalExtBuiltinFn h, PscalExtBuiltinKind k,
                                         const char *d, PscalExtEffectMask m) {
    (void)n; (void)h; (void)k; (void)d; (void)m;
}
static void pscalExtStubRegisterCategory(const char *n) { (void)n; }
static void pscalExtStubRegisterGroup(const char *c, const char *g) { (void)c; (void)g; }
static void pscalExtStubRegisterFunctionEntry(const char *c, const char *g, const char *f) {
    (void)c; (void)g; (void)f;
}
static void pscalExtStubRuntimeError(struct VM_s *vm, const char *format, ...) {
    (void)vm; (void)format;
}

static const PscalExtHostApi *pscalExtGetProbeHostApi(void) {
    static PscalExtHostApi api;
    static bool initialized = false;
    if (!initialized) {
        api = *pscalExtGetHostApi(); // Start from the real vtable...
        api.register_builtin = pscalExtStubRegisterBuiltin; // ...override only the
        api.register_category = pscalExtStubRegisterCategory; // shared-global-touching
        api.register_group = pscalExtStubRegisterGroup; // entries.
        api.register_function_entry = pscalExtStubRegisterFunctionEntry;
        api.runtime_error = pscalExtStubRuntimeError;
        initialized = true;
    }
    return &api;
}

// Attempts dlopen + dlsym + calling pscal_ext_register for `path` against
// the inert probe host above, writing a short diagnostic to `msg_fd` (if
// the outcome isn't success) before exiting with the matching
// PscalExtProbeResult code. Runs ONLY inside the forked child (see
// pscalExtLoadOnePlugin) -- a crash here (segfault, abort(), an illegal
// instruction from corrupt/adversarial plugin code) takes down the child,
// not the host process; the parent observes it via waitpid()'s
// WIFSIGNALED and reports a clean "plugin crashed" error instead of dying
// itself. This is the load-bearing mechanism behind "a malformed plugin
// must fail cleanly, never crash the host VM" (Docs/pscal_vm2_plan.md
// §7.1's rigor bar).
static void pscalExtProbeChild(const char *path, int msg_fd) {
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        dprintf(msg_fd, "%s", err ? err : "dlopen failed (no further detail)");
        _exit(PSCAL_EXT_PROBE_DLOPEN_FAILED);
    }
    dlerror(); // Clear any pending error before dlsym, per dlsym(3)'s own convention.
    void *sym = dlsym(handle, PSCAL_EXT_ENTRY_POINT_NAME);
    if (!sym) {
        dprintf(msg_fd, "missing '%s' entry point", PSCAL_EXT_ENTRY_POINT_NAME);
        _exit(PSCAL_EXT_PROBE_NO_ENTRY_POINT);
    }
    PscalExtRegisterFn entry = (PscalExtRegisterFn)sym;
    int rc = entry(pscalExtGetProbeHostApi(), PSCAL_EXT_ABI_CURRENT);
    if (rc != 0) {
        dprintf(msg_fd, "entry point rejected registration (returned %d) -- likely an ABI "
                        "version this plugin doesn't support",
                rc);
        _exit(PSCAL_EXT_PROBE_ENTRY_REJECTED);
    }
    _exit(PSCAL_EXT_PROBE_OK);
}

// Loads a single plugin: a crash-isolated dry-run probe in a forked child
// first, then -- only if the probe reports success -- the real, in-process
// dlopen + pscal_ext_register() call that actually registers the plugin's
// builtins into this process's live registry. The probe's own
// registrations never reach the parent (separate address space); it exists
// purely to answer "does loading this survive" before the real attempt
// touches this process. A plugin whose pscal_ext_register() has
// non-idempotent side effects beyond registerVmBuiltin calls would see
// those side effects run twice (once in the probe, once for real) -- an
// inherent, documented limitation of fork-based crash isolation without a
// long-lived out-of-process plugin host, which this phase does not build.
static bool pscalExtLoadOnePlugin(const char *path) {
    int msg_pipe[2];
    if (pipe(msg_pipe) != 0) {
        fprintf(stderr, "Error: --ext '%s': pipe() failed: %s\n", path, strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: --ext '%s': fork() failed: %s\n", path, strerror(errno));
        close(msg_pipe[0]);
        close(msg_pipe[1]);
        return false;
    }
    if (pid == 0) {
        close(msg_pipe[0]);
        pscalExtProbeChild(path, msg_pipe[1]);
        _exit(127); // Unreachable: pscalExtProbeChild always calls _exit().
    }

    close(msg_pipe[1]);
    char msg_buf[512];
    ssize_t msg_len = read(msg_pipe[0], msg_buf, sizeof(msg_buf) - 1);
    msg_buf[msg_len > 0 ? msg_len : 0] = '\0';
    close(msg_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        fprintf(stderr, "Error: --ext '%s': waitpid() failed: %s\n", path, strerror(errno));
        return false;
    }

    if (WIFSIGNALED(status)) {
        fprintf(stderr, "Error: --ext '%s': plugin crashed while loading (signal %d: %s).\n",
                path, WTERMSIG(status), strsignal(WTERMSIG(status)));
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != PSCAL_EXT_PROBE_OK) {
        fprintf(stderr, "Error: --ext '%s': %s\n", path,
                msg_buf[0] ? msg_buf : "plugin failed to load (no further detail)");
        return false;
    }

    // Probe succeeded: repeat the same dlopen + entry-point call for real,
    // in this process, so its builtins actually land in the live registry.
    void *handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        const char *err = dlerror();
        fprintf(stderr, "Error: --ext '%s': dlopen failed on the real load after a successful "
                        "probe: %s\n",
                path, err ? err : "(no further detail)");
        return false;
    }
    dlerror();
    void *sym = dlsym(handle, PSCAL_EXT_ENTRY_POINT_NAME);
    if (!sym) {
        fprintf(stderr, "Error: --ext '%s': entry point vanished between probe and real load.\n", path);
        return false;
    }
    PscalExtRegisterFn entry = (PscalExtRegisterFn)sym;
    int rc = entry(pscalExtGetHostApi(), PSCAL_EXT_ABI_CURRENT);
    if (rc != 0) {
        fprintf(stderr,
                "Error: --ext '%s': entry point returned %d on the real load after a "
                "successful probe (non-deterministic plugin behavior).\n",
                path, rc);
        return false;
    }
    return true;
}

void pscalExtLoadRegisteredPlugins(void) {
    pscalExtScanConfiguredDirectory();
    if (g_ext_plugin_count == 0) {
        return;
    }
    for (size_t i = 0; i < g_ext_plugin_count; ++i) {
        if (!pscalExtLoadOnePlugin(g_ext_plugin_paths[i])) {
            exit(EXIT_FAILURE);
        }
    }
}

#endif /* PSCAL_TARGET_IOS */
