// src/core/obj_header.h
//
// VM 2.0 Phase 4 Stage A foundation (Docs/pscal_vm2_plan.md, sub-phase 4a
// of section 5.10.4). Purely additive: as of this sub-phase nothing in the
// VM constructs an ObjHeader-based object yet, and no Value call site
// changes. Every concrete heap shape later sub-phases introduce (StringObj
// in 4c, ArrayObj in 4e/4f, RecordObj in 4d, ...) embeds one of these as
// its first struct member and registers a destructor via
// pscalObjRegisterDestructor from its own init code -- this file only owns
// the generic refcount/dispatch mechanism, never a concrete shape, so that
// each sub-phase's type-family conversion stays isolated to its own files.
#ifndef PSCAL_OBJ_HEADER_H
#define PSCAL_OBJ_HEADER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/types.h" // VarType

typedef struct ObjHeader {
    VarType type;
    _Atomic uint32_t refcount;
} ObjHeader;

typedef void (*PscalObjDestroyFn)(ObjHeader *obj);

// Registers the destructor invoked when an object of this VarType's
// refcount reaches zero. One destructor per VarType; registering a second
// one for the same type is a usage error (aborts), since it almost
// certainly means two sub-phases' init paths raced or double-registered.
void pscalObjRegisterDestructor(VarType type, PscalObjDestroyFn destroy);

// Initializes an embedded ObjHeader with refcount 1. Callers allocate the
// containing struct themselves; this only sets up the header fields.
void pscalObjHeaderInit(ObjHeader *header, VarType type);

// Atomically increments the refcount. Returns `header` unchanged (NULL is
// accepted and passed through) so callers can write
// `x = pscalObjRetain(y);` unconditionally without a separate NULL check.
ObjHeader *pscalObjRetain(ObjHeader *header);

// Atomically decrements the refcount. When it reaches zero, looks up the
// destructor registered for header->type and invokes it -- the destructor
// owns freeing the whole containing struct, including `header` itself.
// NULL-safe (no-op on NULL). Aborts with a diagnostic if no destructor was
// registered for the type: a real bug (a later sub-phase forgot to
// register one, or a tagged word decoded to a bogus type), not a case to
// paper over silently.
void pscalObjRelease(ObjHeader *header);

// --- NaN-boxing double canonicalization (plan §5.10.1) ---
//
// Bits 63-51 of the reserved tag header: sign=0, exponent=0x7FF (11 bits,
// all set), quiet-NaN indicator (bit 51)=1. Any 64-bit word matching this
// 13-bit pattern is a boxed PSCAL value once Stage A wires Value onto this
// encoding -- never a legitimate finite double (finite doubles never have
// exponent==0x7FF) and never +/-Infinity (mantissa all-zero, which this
// pattern excludes via the qnan bit). A genuine IEEE NaN produced by real
// Pascal REAL arithmetic (0.0/0.0, Sqrt(-1), ...) can collide with this
// exact pattern -- the canonical quiet-NaN bit pattern real hardware
// produces typically has sign=0 and bit 51 set -- so it must be pushed out
// of the reserved region before ever being stored in a tagged word.
#define PSCAL_NANBOX_HEADER_MASK (((uint64_t)0x1FFFu) << 51)
#define PSCAL_NANBOX_HEADER      (((uint64_t)0x0FFFu) << 51)

// True if `bits`, read as a 64-bit word, falls in PSCAL's reserved
// NaN-boxing tag region (bits 63-51 match PSCAL_NANBOX_HEADER). Exposed as
// the raw bit-pattern test later sub-phases will use to distinguish a
// boxed tagged value from a live double; defined here rather than 4h
// because it's pure bit arithmetic with no dependency on ObjHeader/Value.
static inline bool pscalWordIsNanBoxTag(uint64_t bits) {
    return (bits & PSCAL_NANBOX_HEADER_MASK) == PSCAL_NANBOX_HEADER;
}

// Canonicalizes `d` into a 64-bit word safe to store in a NaN-boxed slot:
// if `d`'s bit pattern would collide with PSCAL_NANBOX_HEADER, forces the
// sign bit to 1, moving it unambiguously out of the reserved region --
// a NaN's sign bit carries no IEEE-754 semantic meaning, so this is a
// safe, one-way canonicalization. Every other double, including every
// other NaN payload, passes through with its bit pattern unchanged.
static inline uint64_t pscalBoxDouble(double d) {
    uint64_t bits;
    memcpy(&bits, &d, sizeof(bits));
    if (pscalWordIsNanBoxTag(bits)) {
        bits |= (UINT64_C(1) << 63);
    }
    return bits;
}

// Reverses pscalBoxDouble: reinterprets a 64-bit word as a double. No
// correction is needed on read -- the write-side canonicalization already
// guarantees no ambiguity. Callers are expected to have already confirmed
// !pscalWordIsNanBoxTag(bits) (this function does not assert it itself,
// since 4a has no Value-level caller yet to enforce that discipline on).
static inline double pscalUnboxDouble(uint64_t bits) {
    double d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

// --- Pointer-width startup canary (plan §5.10.1) ---
//
// Phase 4's tagged-word design reserves 46 bits for a boxed pointer
// payload -- comfortably above what malloc/mmap-without-a-high-hint return
// in practice on every targeted platform, but that is an empirical
// assumption about allocator behavior, not a language guarantee.

// True if `ptr` fits in the 46-bit payload budget. Exposed separately from
// the canary so later sub-phases (and this sub-phase's own unit tests) can
// check arbitrary pointers, not just the canary's own throwaway
// allocation.
static inline bool pscalObjPointerFitsPayload(const void *ptr) {
    return ((uintptr_t)ptr >> 46) == 0;
}

// Runs a one-time canary allocation and aborts cleanly ("PSCAL VM 2.0:
// tagged-pointer address-space assumption violated") if it doesn't fit,
// rather than silently truncating and corrupting a reference later.
// Idempotent -- safe to call more than once (e.g. from multiple VM
// instances in the same process).
void pscalObjRunPointerWidthCanary(void);

#endif // PSCAL_OBJ_HEADER_H
