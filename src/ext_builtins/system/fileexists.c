#include <stdio.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinFileExists(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "FileExists expects exactly 1 argument.");
        return makeBoolean(0);
    }
    if (!isPascalStringType(VALUE_TYPE(args[0]))) {
        runtimeError(vm, "FileExists argument must be a string.");
        return makeBoolean(0);
    }
    const char* path = AS_STRING(args[0]);
    if (!path) {
        runtimeError(vm, "FileExists received NIL string.");
        return makeBoolean(0);
    }
    FILE* f = fopen(path, "r");
    if (f) {
        fclose(f);
        return makeBoolean(1);
    }
    return makeBoolean(0);
}

void registerFileExistsBuiltin(void) {
    registerVmBuiltin("fileexists", vmBuiltinFileExists,
                      BUILTIN_TYPE_FUNCTION, "FileExists", FX_IO);
}

