# Object Layout

The runtime represents an object as a record structure where fields are
stored sequentially in memory.  Each field occupies one slot in an array
of `FieldValue` entries and can therefore be addressed by a zero‑based
index.

```
+----+----------------+-----------------+-----+
| 0  | 1              | 2               | ... |
+----+----------------+-----------------+-----+
|__vtable| field1     | field2          |     |
+----+----------------+-----------------+-----+
```

* **Slot 0** – reserved for the hidden `__vtable` pointer.
* **Inherited fields** – occupy the lowest indices after `__vtable`.
* **Declared fields** – appended in the order they appear in the class
  declaration.

Code generation uses these offsets directly which enables tooling to
predict the in‑memory layout without performing name based lookups at
runtime.

## Representation

A record value is one `RecordObj` allocation: the header followed by its
`FieldValue` entries (`field_block`), each `next`‑linked to the one after it.
Copying a record (copy‑on‑write clone, by‑value argument, return) is one
allocation plus a copy of each field's value. Field names are interned
(`pscalInternName`) and shared by every record carrying them. Fields that
overlay one storage cell (variant parts, a redeclared slot) point at the
first such field's value, within the same block, and a copy keeps that
sharing inside the copy.

What building a record from its type (`pscalRecordObjCreateForType`)
derives — each field's name, declared type and slot, the vtable slot,
inherited fields — is cached on the type's AST node (`RecordTypeLayout`) and
reused until a symbol or type table changes or a compile starts or ends, so
`new`, `SetLength` of a record array and field metadata lookups on
`ALLOC_OBJECT` objects no longer rescan the procedure table each time.
