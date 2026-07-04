#include <stdlib.h>
#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinAtoi(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "atoi expects exactly 1 argument.");
        return makeInt(0);
    }
    if (VALUE_TYPE(args[0]) != TYPE_STRING && VALUE_TYPE(args[0]) != TYPE_UNICODE_STRING) {
        runtimeError(vm, "atoi argument must be a string.");
        return makeInt(0);
    }
    const char* s = AS_STRING(args[0]);
    if (!s) {
        runtimeError(vm, "atoi received NIL string.");
        return makeInt(0);
    }
    return makeInt(atoi(s));
}

void registerAtoiBuiltin(void) {
    registerVmBuiltin("atoi", vmBuiltinAtoi,
                      BUILTIN_TYPE_FUNCTION, "Atoi");
}

