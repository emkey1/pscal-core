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
    TYPE_UNICODE_STRING
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
