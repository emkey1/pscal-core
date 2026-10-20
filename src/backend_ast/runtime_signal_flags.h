#ifndef PSCAL_RUNTIME_SIGNAL_FLAGS_H
#define PSCAL_RUNTIME_SIGNAL_FLAGS_H

/* The runtime's process-wide signal flags (defined in builtin.c), exposed so
 * the VM's instruction loop can test them inline instead of making several
 * out-of-line calls per instruction. C only: builtin.h is also included from
 * Objective-C++, where <stdatomic.h> is unavailable. Only read these here;
 * set and clear them through builtin.c's pscalRuntime* functions. */

#include <stdatomic.h>
#include <stdbool.h>

extern atomic_int g_vm_sigint_seen;
extern atomic_bool g_vm_interrupt_broadcast;
extern atomic_bool g_vm_suspend_requested;

/* True when a SIGINT, an interrupt broadcast or a suspend request may be
 * pending. Relaxed loads: a hit sends the caller down the slow path, which
 * re-reads each flag with the usual ordering. */
static inline bool pscalRuntimeSignalAttention(void) {
    return atomic_load_explicit(&g_vm_sigint_seen, memory_order_relaxed) != 0 ||
           atomic_load_explicit(&g_vm_interrupt_broadcast, memory_order_relaxed) ||
           atomic_load_explicit(&g_vm_suspend_requested, memory_order_relaxed);
}

#endif
