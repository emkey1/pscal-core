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

#define FX_ALL_KNOWN (FX_IO | FX_NET | FX_PROC | FX_CLOCK | FX_RANDOM)

#endif /* PSCAL_EFFECT_MASK_H */
