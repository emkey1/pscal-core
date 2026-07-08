#ifndef PSCAL_EFFECT_MASK_H
#define PSCAL_EFFECT_MASK_H

/* VM 2.0 Phase 6: effect classes for builtins (Docs/pscal_vm2_plan.md §6.3).
 * A bitmask so a builtin can combine categories (e.g. FX_IO | FX_NET). */

#include <stdint.h>

typedef uint32_t EffectMask;

#define FX_PURE   ((EffectMask)0)
#define FX_IO     ((EffectMask)(1u << 0))
#define FX_NET    ((EffectMask)(1u << 1))
#define FX_PROC   ((EffectMask)(1u << 2))
#define FX_CLOCK  ((EffectMask)(1u << 3))
#define FX_RANDOM ((EffectMask)(1u << 4))
/* VM 2.0 Phase 7 follow-up: not a per-builtin classification (no
 * registerVmBuiltin() call site should ever pass this) -- a distinct
 * capability gate checked once, at plugin-load time
 * (ext_builtins/plugin_loader.c), for whether --ext/PSCAL_EXT_DIR dlopen
 * loading is permitted at all. Reuses the --deny/PSCAL_VM_DENY parsing and
 * storage (vm_fx_policy.c) purely so "no plugins" composes with "no
 * network"/"no process spawn" as one flag/mental model
 * (`--deny net,proc,ext`) -- loading a plugin is a capability escalation
 * (arbitrary native code in-process, not sandboxed by any other FX_* bit),
 * so it gets denied by `--deny all` too. */
#define FX_EXT    ((EffectMask)(1u << 5))

#define FX_ALL_KNOWN (FX_IO | FX_NET | FX_PROC | FX_CLOCK | FX_RANDOM | FX_EXT)

#endif /* PSCAL_EFFECT_MASK_H */
