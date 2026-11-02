#include "common/frontend_kind.h"
#include "common/runtime_tty.h"
#include "vm/vm.h"

/* In the shell, a builtin that fails reports it and fails that command, and the
 * script goes on, as under bash. runtimeError stops the VM, which is what the
 * other frontends that reach these builtins want. */
static void shellBuiltinError(VM *vm, const char *format, ...) __attribute__((format(printf, 2, 3)));
#define runtimeError(vm, ...) shellBuiltinError((vm), __VA_ARGS__)

#include "shell/shell_runtime_state.inc"
#include "shell/shell_command_utils.inc"
#include "shell/shell_word_expansion.inc"
#include "shell/shell_execution.inc"
#include "shell/shell_builtins.inc"

#undef runtimeError

static void shellBuiltinError(VM *vm, const char *format, ...) {
    char message[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (!frontendIsShell()) {
        runtimeError(vm, "%s", message);
        return;
    }
    shellReportRecoverableError(vm, true, "%s", message);
    shellUpdateStatus(1);
}
