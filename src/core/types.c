#include "types.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>

void setTypeValue(Value *val, VarType type) {
    if (!val) return;
#ifdef DEBUG
    if (val->type != type) {
        fprintf(stderr, "DEBUG: Changing Value type from %s to %s\n",
                varTypeToString(val->type), varTypeToString(type));
    }
#endif
    // VM 2.0 Phase 4i checkpoint 3c: unlike SET_VALUE_TYPE (used at fresh-
    // construction sites, or sites already audited to freeValue first),
    // setTypeValue's own callers (vm.c's real/int parameter-coercion
    // sites) retype an ALREADY-LIVE Value across the real/int family
    // boundary with no freeValue call in between. Release an old
    // Int64Box/LongDoubleBox here before it becomes unreachable, or every
    // such coercion leaks one box.
    if (val->type == TYPE_INT64 || val->type == TYPE_UINT64) {
        if (val->int64_box) {
            pscalObjRelease(&val->int64_box->header);
            val->int64_box = NULL;
        }
    } else if (val->type == TYPE_LONG_DOUBLE) {
        if (val->long_double_box) {
            pscalObjRelease(&val->long_double_box->header);
            val->long_double_box = NULL;
        }
    }
    val->type = type;
}

// Infer the type of a binary operation based on operand types
VarType inferBinaryOpType(VarType left, VarType right) {
    if (left == TYPE_UNICODE_STRING || right == TYPE_UNICODE_STRING) return TYPE_UNICODE_STRING;
    if (left == TYPE_STRING || right == TYPE_STRING) return TYPE_STRING;

    bool left_real = isRealType(left);
    bool right_real = isRealType(right);
    bool left_int = isIntlikeType(left);
    bool right_int = isIntlikeType(right);

    if (left_real && right_int) return left;
    if (right_real && left_int) return right;

    if (left_real && right_real) {
        if (left == TYPE_LONG_DOUBLE || right == TYPE_LONG_DOUBLE) return TYPE_LONG_DOUBLE;
        if (left == TYPE_DOUBLE || right == TYPE_DOUBLE) return TYPE_DOUBLE;
        return TYPE_FLOAT;
    }
    if (left == TYPE_BOOLEAN && right == TYPE_BOOLEAN) return TYPE_BOOLEAN;
    if (left == TYPE_CHAR && right == TYPE_CHAR) return TYPE_STRING; // for '+'
    if (left == TYPE_WIDECHAR && right == TYPE_WIDECHAR) return TYPE_UNICODE_STRING;
    if ((left == TYPE_WIDECHAR && right == TYPE_CHAR) || (left == TYPE_CHAR && right == TYPE_WIDECHAR)) {
        return TYPE_UNICODE_STRING;
    }
    if (left_int && right_int) return TYPE_INT32;
    return TYPE_VOID; // fallback
}
