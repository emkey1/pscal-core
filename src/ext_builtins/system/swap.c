#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinSwap(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "Swap expects exactly 2 arguments.");
        return makeVoid();
    }
    if (VALUE_TYPE(args[0]) != TYPE_POINTER || VALUE_TYPE(args[1]) != TYPE_POINTER) {
        runtimeError(vm, "Arguments to Swap must be variables (VAR parameters).");
        return makeVoid();
    }
    Value* varA = (Value*)AS_POINTER(args[0]);
    Value* varB = (Value*)AS_POINTER(args[1]);
    if (!varA || !varB) {
        runtimeError(vm, "Swap received a NIL pointer for a VAR parameter.");
        return makeVoid();
    }
    if (VALUE_TYPE(*varA) != VALUE_TYPE(*varB)) {
        runtimeError(vm, "Cannot swap variables of different types (%s and %s).",
                     varTypeToString(VALUE_TYPE(*varA)), varTypeToString(VALUE_TYPE(*varB)));
        return makeVoid();
    }
    Value temp = *varA;
    *varA = *varB;
    *varB = temp;
    return makeVoid();
}

void registerSwapBuiltin(void) {
    registerVmBuiltin("swap", vmBuiltinSwap,
                      BUILTIN_TYPE_PROCEDURE, "Swap", FX_PROC);
}

