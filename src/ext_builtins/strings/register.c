#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"

void registerAtoiBuiltin(void);
void registerParseBuiltins(void);

void registerStringBuiltins(void) {
    const char *category = "strings";
    extBuiltinRegisterCategory(category);
    extBuiltinRegisterGroup(category, "conversion");
    extBuiltinRegisterFunction(category, "conversion", "Atoi");
    extBuiltinRegisterFunction(category, "conversion", "parse_int");
    extBuiltinRegisterFunction(category, "conversion", "parse_float");
    extBuiltinRegisterFunction(category, "conversion", "parse_bool");
    extBuiltinRegisterFunction(category, "conversion", "split");
    extBuiltinRegisterFunction(category, "conversion", "itoa");
    registerAtoiBuiltin();
    registerParseBuiltins();
}
