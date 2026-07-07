// src/core/obj_header.h
//
// VM 2.0 Phase 4 Stage A foundation (Docs/pscal_vm2_plan.md, sub-phase 4a
// of section 5.10.4). As of 4b, ClosureEnvPayload and MStream (both
// declared in core/types.h) embed an ObjHeader as their first member --
// which is why this file takes VarType from the standalone core/var_type.h
// rather than core/types.h itself: types.h includes this header (to embed
// ObjHeader), so this header including types.h back would be circular.
// Every concrete heap shape later sub-phases introduce (StringObj in 4c,
// ArrayObj in 4e/4f, RecordObj in 4d, ...) embeds one of these as its
// first struct member and registers a destructor via
// pscalObjRegisterDestructor from its own init code -- this file only owns
// the generic refcount/dispatch mechanism, never a concrete shape, so that
// each sub-phase's type-family conversion stays isolated to its own files.
#ifndef PSCAL_OBJ_HEADER_H
#define PSCAL_OBJ_HEADER_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "core/var_type.h" // VarType

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
// Phase 4's tagged-word design reserves a 50-bit pointer payload (a
// single discriminant bit at bit 50 of the reserved region separates
// "pointer, all 50 remaining bits are address" from "other inline
// immediate, needs its own sub-kind+payload split" -- see plan §5.10.1
// for the corrected encoding). This budget was revised from an initial
// 46-bit design the moment the canary caught a real violation on first
// deploy: Linux/ARM64 (the claw fleet's GB10 Sparks) returns heap and
// stack addresses using close to the full 48-bit address space (observed:
// a malloc'd pointer needing 48 bits, a stack address at 0xfffffc...,
// both comfortably fitting 50 bits with margin to spare) -- 46 bits was
// an assumption grounded in this developer's own machine (macOS/ARM64,
// which does stay under 46 bits) generalized to "every targeted platform"
// without actually checking Linux/ARM64. 50 bits is still an empirical
// assumption, not a language guarantee, which is exactly why this canary
// exists rather than trusting the assumption silently.

// True if `ptr` fits in the 50-bit pointer payload budget. Exposed
// separately from the canary so later sub-phases (and this sub-phase's
// own unit tests) can check arbitrary pointers, not just the canary's own
// throwaway allocation.
static inline bool pscalObjPointerFitsPayload(const void *ptr) {
    return ((uintptr_t)ptr >> 50) == 0;
}

// Runs a one-time canary allocation and aborts cleanly ("PSCAL VM 2.0:
// tagged-pointer address-space assumption violated") if it doesn't fit,
// rather than silently truncating and corrupting a reference later.
// Idempotent -- safe to call more than once (e.g. from multiple VM
// instances in the same process).
void pscalObjRunPointerWidthCanary(void);

// --- Immediate/pointer tagged-word packing (plan §5.10.1, sub-phase 4h) ---
//
// VM 2.0 Phase 4h (Docs/pscal_vm2_plan.md §5.10.4): builds and unit-tests
// the actual bit-packing this design has specified since 4a, in isolation
// -- exactly like the NaN-box canonicalization and pointer-width canary
// above, nothing in the live VM constructs one of these tagged words yet.
// `Value`'s physical layout does not change until 4i (the flag day); this
// section only proves the encode/decode round-trips correctly.
//
// Layout, once a boxed word's top 13 bits (PSCAL_NANBOX_HEADER) mark it
// as tagged rather than a live IEEE-754 double:
//
//   bit 50 = 0: bits 49-0 (50 bits) are a raw pointer payload, in full.
//   bit 50 = 1: bits 49-45 (5 bits) are a `kind` tag, bits 44-0 (45 bits)
//               are that kind's immediate payload.
//
// TYPE_DOUBLE itself is never tagged at all -- a double's own IEEE-754
// bit pattern (after pscalBoxDouble's NaN canonicalization) simply IS its
// tagged word, distinguished purely by NOT matching the reserved header.
// TYPE_FLOAT gets its own inline `kind` slot instead, since a 32-bit
// float's bit pattern does not stand alone the way a 64-bit double's
// does.

typedef enum PscalTaggedKind {
    PSCAL_TAG_VOID = 0,
    PSCAL_TAG_NIL = 1,
    PSCAL_TAG_BOOLEAN = 2,
    PSCAL_TAG_CHAR = 3,
    PSCAL_TAG_WIDECHAR = 4,
    PSCAL_TAG_BYTE = 5,
    PSCAL_TAG_WORD = 6,
    PSCAL_TAG_INT8 = 7,
    PSCAL_TAG_UINT8 = 8,
    PSCAL_TAG_INT16 = 9,
    PSCAL_TAG_UINT16 = 10,
    PSCAL_TAG_INT32 = 11,
    PSCAL_TAG_UINT32 = 12,
    PSCAL_TAG_FLOAT = 13,
    // 14-31 reserved (plan §5.10.1): headroom for future inline kinds
    // without another Stage-A-shaped migration.
} PscalTaggedKind;

#define PSCAL_TAG_DISCRIMINANT_BIT        (UINT64_C(1) << 50)
#define PSCAL_TAG_POINTER_PAYLOAD_MASK    ((UINT64_C(1) << 50) - 1)
#define PSCAL_TAG_KIND_SHIFT              45
#define PSCAL_TAG_KIND_MASK               (UINT64_C(0x1F) << PSCAL_TAG_KIND_SHIFT)
#define PSCAL_TAG_IMMEDIATE_PAYLOAD_MASK  ((UINT64_C(1) << 45) - 1)

// True if `word` is a tagged pointer word (bit 50 clear). Caller must
// have already confirmed pscalWordIsNanBoxTag(word) -- this only
// distinguishes the pointer/immediate sub-cases *within* the tagged
// region, it does not itself check the region header.
static inline bool pscalTaggedWordIsPointer(uint64_t word) {
    return (word & PSCAL_TAG_DISCRIMINANT_BIT) == 0;
}

// Packs `ptr` into a tagged pointer word. Caller must have already
// confirmed pscalObjPointerFitsPayload(ptr) (the canary's own contract,
// reused here rather than re-asserted, matching pscalUnboxDouble's
// "write side already guaranteed it" precedent).
static inline uint64_t pscalTagPointer(const void *ptr) {
    uint64_t addr = (uint64_t)(uintptr_t)ptr;
    return PSCAL_NANBOX_HEADER | (addr & PSCAL_TAG_POINTER_PAYLOAD_MASK);
}

// Reverses pscalTagPointer. Caller must have already confirmed
// pscalTaggedWordIsPointer(word).
static inline void *pscalUntagPointer(uint64_t word) {
    return (void *)(uintptr_t)(word & PSCAL_TAG_POINTER_PAYLOAD_MASK);
}

// Packs an immediate `kind`+`raw_payload` into a tagged word. `raw_payload`
// is masked to 45 bits -- callers are expected to have already narrowed
// their value to the kind's own width (this function does not itself
// validate that a caller didn't hand it, say, a 64-bit value for a
// kind whose table entry only claims 8 bits; the typed pscalTagX
// wrappers below are the enforcement point for that).
static inline uint64_t pscalTagImmediate(PscalTaggedKind kind, uint64_t raw_payload) {
    uint64_t kind_bits = ((uint64_t)kind << PSCAL_TAG_KIND_SHIFT) & PSCAL_TAG_KIND_MASK;
    uint64_t payload = raw_payload & PSCAL_TAG_IMMEDIATE_PAYLOAD_MASK;
    return PSCAL_NANBOX_HEADER | PSCAL_TAG_DISCRIMINANT_BIT | kind_bits | payload;
}

// Caller must have already confirmed pscalWordIsNanBoxTag(word) &&
// !pscalTaggedWordIsPointer(word).
static inline PscalTaggedKind pscalTaggedWordKind(uint64_t word) {
    return (PscalTaggedKind)((word & PSCAL_TAG_KIND_MASK) >> PSCAL_TAG_KIND_SHIFT);
}

static inline uint64_t pscalTaggedWordPayload(uint64_t word) {
    return word & PSCAL_TAG_IMMEDIATE_PAYLOAD_MASK;
}

// --- Typed convenience wrappers, one pair per plan §5.10.1 `kind` row ---
//
// Each pair packs/unpacks a scalar in its own natural C width, with the
// correct sign/zero-extension for that width baked in at the unpack side
// -- getting this wrong (e.g. zero-extending an INT8) would silently
// corrupt every negative value that width can hold, so each unpack
// function's cast sequence is the load-bearing part, not the pack side.

static inline uint64_t pscalTagVoid(void) { return pscalTagImmediate(PSCAL_TAG_VOID, 0); }
static inline uint64_t pscalTagNil(void) { return pscalTagImmediate(PSCAL_TAG_NIL, 0); }

static inline uint64_t pscalTagBoolean(bool b) {
    return pscalTagImmediate(PSCAL_TAG_BOOLEAN, b ? 1u : 0u);
}
static inline bool pscalUntagBoolean(uint64_t word) {
    return (pscalTaggedWordPayload(word) & 0x1u) != 0;
}

static inline uint64_t pscalTagChar(char c) {
    return pscalTagImmediate(PSCAL_TAG_CHAR, (uint8_t)c);
}
static inline char pscalUntagChar(uint64_t word) {
    return (char)(uint8_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagWideChar(uint32_t codepoint) {
    return pscalTagImmediate(PSCAL_TAG_WIDECHAR, codepoint);
}
static inline uint32_t pscalUntagWideChar(uint64_t word) {
    return (uint32_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagByte(uint8_t b) {
    return pscalTagImmediate(PSCAL_TAG_BYTE, b);
}
static inline uint8_t pscalUntagByte(uint64_t word) {
    return (uint8_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagWord(uint16_t w) {
    return pscalTagImmediate(PSCAL_TAG_WORD, w);
}
static inline uint16_t pscalUntagWord(uint64_t word) {
    return (uint16_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagInt8(int8_t v) {
    return pscalTagImmediate(PSCAL_TAG_INT8, (uint8_t)v);
}
static inline int8_t pscalUntagInt8(uint64_t word) {
    return (int8_t)(uint8_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagUInt8(uint8_t v) {
    return pscalTagImmediate(PSCAL_TAG_UINT8, v);
}
static inline uint8_t pscalUntagUInt8(uint64_t word) {
    return (uint8_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagInt16(int16_t v) {
    return pscalTagImmediate(PSCAL_TAG_INT16, (uint16_t)v);
}
static inline int16_t pscalUntagInt16(uint64_t word) {
    return (int16_t)(uint16_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagUInt16(uint16_t v) {
    return pscalTagImmediate(PSCAL_TAG_UINT16, v);
}
static inline uint16_t pscalUntagUInt16(uint64_t word) {
    return (uint16_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagInt32(int32_t v) {
    return pscalTagImmediate(PSCAL_TAG_INT32, (uint32_t)v);
}
static inline int32_t pscalUntagInt32(uint64_t word) {
    return (int32_t)(uint32_t)pscalTaggedWordPayload(word);
}

static inline uint64_t pscalTagUInt32(uint32_t v) {
    return pscalTagImmediate(PSCAL_TAG_UINT32, v);
}
static inline uint32_t pscalUntagUInt32(uint64_t word) {
    return (uint32_t)pscalTaggedWordPayload(word);
}

// FLOAT carries its raw IEEE-754 single-precision bit pattern as the
// 45-bit payload -- no NaN canonicalization needed here (unlike
// pscalBoxDouble), because these 32 bits are payload *inside* an already-
// tagged word, never reinterpreted as a standalone 64-bit double that
// could collide with the reserved header.
static inline uint64_t pscalTagFloat(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return pscalTagImmediate(PSCAL_TAG_FLOAT, bits);
}
static inline float pscalUntagFloat(uint64_t word) {
    uint32_t bits = (uint32_t)pscalTaggedWordPayload(word);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

// --- Int64Box / LongDoubleBox (plan §5.10.1, sub-phase 4h) ---
//
// The two numeric types that must stay heap-boxed under this tagged-word
// scheme: TYPE_INT64/TYPE_UINT64 (full 64-bit range doesn't fit the
// 45-bit immediate payload) and TYPE_LONG_DOUBLE (extended precision a
// 64-bit word cannot carry inline alongside a type tag). Defined here
// rather than core/types.h because nothing wires these onto ValueStruct's
// union until 4i (the flag day) -- keeping them here lets this whole
// sub-phase build and unit-test standalone against just
// obj_header.{h,c}, the same isolation 4a established for the NaN-box
// and canary. Expect these two structs (not their logic) to migrate to
// core/types.h alongside the other ObjHeader-based shapes once 4i wires
// them into Value's union, the same way every prior sub-phase's shape
// has lived there.
//
// One physical field serves both TYPE_INT64 and TYPE_UINT64 -- a signed
// and unsigned 64-bit value share the same bit pattern, exactly like the
// live VM's Value.i_val already stores unsigned 64-bit values in a
// signed field today (see utils.h's asI64 family) -- not a new risk this
// phase introduces, just carried into the boxed shape unchanged. The
// caller selects which of the two VarTypes a given instance is by
// setting header.type after pscalInt64BoxCreate returns (default
// TYPE_INT64); both destructors are pre-registered so either is safe.
typedef struct Int64Box {
    ObjHeader header;
    int64_t value;
} Int64Box;

typedef struct LongDoubleBox {
    ObjHeader header; // header.type is always TYPE_LONG_DOUBLE
    long double value;
} LongDoubleBox;

// Returns a fresh, refcount=1 Int64Box holding `value`, tagged
// TYPE_INT64 by default -- set `box->header.type = TYPE_UINT64` after
// the call for an unsigned instance (reinterpret the bit pattern via
// memcpy at the point of use, not a cast, to avoid signed-overflow UB).
Int64Box *pscalInt64BoxCreate(int64_t value);

// Returns a fresh, refcount=1 LongDoubleBox holding `value`.
LongDoubleBox *pscalLongDoubleBoxCreate(long double value);

#endif // PSCAL_OBJ_HEADER_H
