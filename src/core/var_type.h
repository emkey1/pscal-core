// src/core/var_type.h
//
// VarType lives in its own header, separate from core/types.h, so that
// core/obj_header.h can reference it (ObjHeader.type is a VarType) without
// creating a circular include: types.h embeds ObjHeader inside
// ClosureEnvPayload/MStream (VM 2.0 Phase 4b), so types.h now depends on
// obj_header.h, and obj_header.h in turn only needs this file, not all of
// types.h.
#ifndef PSCAL_VAR_TYPE_H
#define PSCAL_VAR_TYPE_H

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_INT32,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_RECORD,
    TYPE_FILE,
    TYPE_BYTE,
    TYPE_WORD,
    TYPE_ENUM,
    TYPE_ARRAY,
    TYPE_BOOLEAN,
    TYPE_MEMORYSTREAM,
    TYPE_SET,
    TYPE_POINTER,
    TYPE_INTERFACE,
    TYPE_CLOSURE,
    /* Extended integer and floating-point types */
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT,
    TYPE_LONG_DOUBLE,
    TYPE_NIL,
    TYPE_THREAD,
    TYPE_WIDECHAR,
    TYPE_UNICODE_STRING,
    // VM 2.0 Phase 5a (Docs/pscal_vm2_plan.md Sec 6.1, checkpoint 5a-i):
    // ObjHeader-boxed handle to a spawned unit of work (TaskObj, core/
    // types.h), formalizing today's opaque TYPE_THREAD int handle as a
    // first-class refcounted value. Appended at the end, not grouped near
    // TYPE_THREAD/TYPE_CLOSURE -- opcode/VarType numbering is append-only
    // post-PSB3 (Sec 2) and .bc files carry no compatibility promise
    // (recompile is the story), so there is no correctness reason to
    // renumber, only a cosmetic one.
    TYPE_TASK,
    // VM 2.0 Phase 5b (Docs/pscal_vm2_plan.md Sec 6.2, checkpoint 5b-i):
    // ObjHeader-boxed bounded MPMC queue of Values (ChannelObj, core/
    // types.h). Appended after TYPE_TASK for the identical append-only
    // reason above -- MUST stay the last member until the next VarType is
    // added, or core/obj_header.c's PSCAL_OBJ_DESTRUCTOR_TABLE_SIZE (sized
    // off "the enum's last member") needs updating too. TYPE_TASK's own
    // addition is exactly the bug this note exists to prevent: the table
    // was sized off the previous last member and aborted registering
    // TYPE_TASK's destructor as "out of range" until an actual TaskSpawn
    // smoke test caught it.
    TYPE_CHANNEL
} VarType;

/*
 * Backwards compatibility aliases.
 *
 * Pascal traditionally exposes INTEGER and REAL as its fundamental numeric
 * types.  The VM has been moving toward a more explicit naming scheme where
 * the underlying sizes are part of the type name (e.g. INT32 and DOUBLE).
 *
 * To avoid a massive churn throughout the existing front‑ends we simply map
 * the old identifiers to the new ones via macros.  This allows legacy code
 * that still uses TYPE_INTEGER/TYPE_REAL to compile unchanged while the rest
 * of the system can reason about the new INT32/DOUBLE symbols.
 */
#define TYPE_INTEGER TYPE_INT32
#define TYPE_REAL    TYPE_DOUBLE

#endif // PSCAL_VAR_TYPE_H
