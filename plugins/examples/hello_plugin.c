// components/pscal-core/plugins/examples/hello_plugin.c
//
// Minimal reference plugin for the VM 2.0 Phase 7 dlopen ABI
// (backend_ast/pscal_ext_api.h). Deliberately smaller and simpler than
// plugins/sqlite/sqlite_ext_plugin.c -- no handle table, no native
// dependency, just the bare pattern every plugin follows: check the ABI,
// register some builtins, use the host API for everything. See
// Docs/pscal_ext_plugin_guide.md for the full walkthrough (write, build,
// load, gate) and Docs/pscal_vm2_plan.md §7.1 / the VM manual's ch4 §4.0b
// for the design rationale.
//
// Build (no CMake needed -- this is the whole point):
//   cc -shared -fPIC -I <pscal-core>/src -o hello_plugin.dylib hello_plugin.c   # macOS
//   cc -shared -fPIC -I <pscal-core>/src -o hello_plugin.so    hello_plugin.c   # Linux
//
// Load:
//   pascal --ext ./hello_plugin.dylib --no-cache prog.pas
//   (or drop it in a directory and set PSCAL_EXT_DIR to that directory --
//   every frontend picks it up the same way)
#include "backend_ast/pscal_ext_api.h"

#include <stdio.h>
#include <string.h>

// The host API pointer arrives exactly once, as pscal_ext_register()'s
// argument -- stash it so the builtin handlers below (called later, once
// per program call to PluginPing/PluginGreet) can reach it. Every plugin
// needs some place to keep this; a single file-static pointer is enough
// for a plugin this small.
static const PscalExtHostApi *g_host = NULL;

// The simplest possible builtin: no arguments, no Value inspection, just
// construct a return value with the host API and hand it back. Good first
// smoke test after loading a new plugin -- if this doesn't return 42,
// nothing else will work either.
static Value pluginPing(struct VM_s *vm, int arg_count, Value *args) {
    (void)args;
    if (arg_count != 0) {
        // runtime_error() mirrors runtimeError(VM*, const char*, ...) --
        // reports a normal PSCAL runtime error (same path a builtin like
        // WriteLn's own argument checks use) and does NOT stop execution
        // by itself; the handler must still return a well-formed Value.
        g_host->runtime_error(vm, "PluginPing takes no arguments.");
        return g_host->make_int(-1);
    }
    return g_host->make_int(42);
}

// A builtin that actually reads its argument: this is the shape most real
// plugins need. Shows the three things every argument-handling builtin
// does -- validate arg_count, validate the argument's type before reading
// it, then read it through a host accessor (never through `args[0].bits`
// directly; only `.type`, the plain discriminant tag, is safe to touch
// without going through the host API).
static Value pluginGreet(struct VM_s *vm, int arg_count, Value *args) {
    if (arg_count != 1) {
        g_host->runtime_error(vm, "PluginGreet expects exactly 1 argument (a name).");
        return g_host->make_string("");
    }
    // is_string_type() takes the raw VarType tag (args[0].type is always
    // safe to read directly -- it's the discriminant, not a boxed
    // payload) and answers the same question isPascalStringType() does
    // in-tree, without exposing what a "string Value" actually looks like
    // internally.
    if (!g_host->is_string_type(args[0].type)) {
        g_host->runtime_error(vm, "PluginGreet argument must be a string.");
        return g_host->make_string("");
    }
    // as_cstring() returns the live buffer, not a copy -- valid exactly as
    // long as the Value itself is, which for an argument Value is exactly
    // this call. Copy it out immediately if you need it to outlive the
    // call (this example does, into `buf`, via snprintf).
    const char *name = g_host->as_cstring(args[0]);
    if (!name || !*name) {
        name = "world";
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "Hello, %s!", name);
    // make_string() copies `buf` into a new, host-owned Value -- safe to
    // return even though `buf` itself is about to go out of scope.
    return g_host->make_string(buf);
}

// Every plugin's one required export. Called exactly once, right after a
// successful dlopen, with the live host API and the host's ABI version.
int pscal_ext_register(const PscalExtHostApi *host, uint32_t host_abi) {
    // Check the ABI major version BEFORE touching `host` at all -- an
    // incompatible host may have reordered or removed vtable fields this
    // plugin was built against. Returning nonzero here is a normal,
    // expected outcome the loader treats as a clean rejection, not an
    // error; it is NOT the same as crashing.
    if (PSCAL_EXT_ABI_MAJOR_OF(host_abi) != PSCAL_EXT_ABI_MAJOR) {
        return 1;
    }
    g_host = host;

    // register_category()/register_function_entry() are optional --
    // they only feed `--dump-ext-builtins`'s introspection catalog, not
    // the builtins' actual callability. Skipping them is fine for a
    // throwaway plugin; included here since it costs nothing and this
    // file is meant to be copied as a starting point.
    host->register_category("hello");
    host->register_function_entry("hello", "greeting", "PluginPing");
    host->register_function_entry("hello", "greeting", "PluginGreet");

    // register_builtin() is the one call that actually matters: it binds
    // the name for the CALL_BUILTIN/CALL_BUILTIN_PROC opcodes AND
    // synthesizes the compile-time declaration every frontend's compiler
    // resolves identifiers against (Pascal, Rea, CLike, Aether, and exsh
    // all see "PluginPing"/"PluginGreet" as ordinary functions the moment
    // this returns). PSCAL_EXT_FX_PURE is correct for both -- neither
    // touches the outside world, so neither is affected by --deny.
    host->register_builtin("pluginping", pluginPing, PSCAL_EXT_BUILTIN_FUNCTION,
                            "PluginPing", PSCAL_EXT_FX_PURE);
    host->register_builtin("plugingreet", pluginGreet, PSCAL_EXT_BUILTIN_FUNCTION,
                            "PluginGreet", PSCAL_EXT_FX_PURE);
    return 0;
}
