#include "core/utils.h"
#include "backend_ast/builtin.h"

static Value vmBuiltinFactorial(struct VM_s* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Factorial expects exactly 1 argument.");
        return makeInt(-1);
    }
    if (!IS_INTLIKE(args[0])) {
        runtimeError(vm, "Factorial argument must be an integer.");
        return makeInt(-1);
    }
    long long n = asI64(args[0]);
    if (n < 0) {
        runtimeError(vm, "Factorial argument must be non-negative.");
        return makeInt(-1);
    }
    long long result = 1;
    for (long long i = 2; i <= n; ++i) {
        if (__builtin_mul_overflow(result, i, &result)) { // from 21!
            runtimeError(vm, "Runtime Error: Integer overflow.");
            return makeInt(-1);
        }
    }
    return pscalIntResultLike1(args[0], result);
}

void registerFactorialBuiltin(void) {
    registerVmBuiltin("factorial", vmBuiltinFactorial,
                      BUILTIN_TYPE_FUNCTION, "Factorial", FX_PURE);
}
