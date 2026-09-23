# pscalasm — Assemble PSCAL Bytecode

## Name

`pscalasm` — assemble a `.pbc` bytecode file from textual PSCAL assembly

## Synopsis

```sh
pscalasm <assembly.txt|-> <output.pbc>
pscalasm --help
```

## Description

`pscalasm` supports two input modes:

1. `PSCALASM2` textual assembly (primary mode).
2. Legacy `PSCALASM` hex block from `pscald --asm` (fallback compatibility mode).

Use `pscald --emit-asm` to export canonical text assembly that round-trips via
`pscalasm`.

## Canonical Workflow

1. Emit canonical assembly:

```sh
pscald --emit-asm input.pbc > dump.asm
```

2. Assemble back to bytecode:

```sh
pscalasm dump.asm rebuilt.pbc
```

3. Optional verification:

```sh
pscald input.pbc 2> a.disasm
pscald rebuilt.pbc 2> b.disasm
diff -u a.disasm b.disasm
```

## Notes On `PSCALASM2` Code Sections

- `code <byte_count>` declares the final byte size of the instruction stream.
- `inst <line> <OPCODE> ...` emits one instruction.
- `label <name>` marks the current instruction boundary.
- `JUMP` and `JUMP_IF_FALSE` take a 4-byte (`int32`) relative displacement and
  accept either:
  - four raw operand bytes, big-endian (`inst 12 JUMP 0 0 255 244`)
  - one signed relative offset (`inst 12 JUMP -12`)
  - one symbolic label (`inst 12 JUMP @loop_start`)

## Notes On `PSCALASM2` Constants

Each constant is `const <idx> <var_type> <payload>`, where `<var_type>` is the
`VarType` number from `core/var_type.h`. `pscald --emit-asm` writes one payload
form for every type the bytecode cache can store, so a chunk any frontend
compiled round-trips through `pscalasm`:

- Integers (`INTEGER` 2, `BYTE` 8, `WORD` 9, `BOOLEAN` 12, `INT8` 18, `INT16` 20,
  `INT64` 23, `THREAD` 28): `const <idx> <type> <n>`
- Unsigned integers (`UINT8` 19, `UINT16` 21, `UINT32` 22, `UINT64` 24): the
  same, over the full unsigned 64-bit range.
- Reals (`REAL` 3, `FLOAT` 25, `LONG_DOUBLE` 26): `const <idx> <type> <x>`
- Strings (`STRING` 4, `UNICODESTRING` 30): `const <idx> <type> "<text>"`, or
  `null` for a string with no buffer at all, which is not the same as `""`.
- `CHAR` 5: `const <idx> 5 <code>` (-128..255). `WIDECHAR` 29: the code point.
- No payload: `NIL` 27, `VOID` 1, and the unset handles `TASK` 31 and
  `CHANNEL` 32 (a live task or channel cannot be stored).
- `MEMORY_STREAM` 13: `null`, or `bytes <n> <b0> <b1> ...` in decimal.
- `ENUM` 10: `const <idx> 10 "<enum_name>" <ordinal>`
- `SET` 14: `const <idx> 14 <count> <ord0> <ord1> ...`
- `POINTER` 15:
  - null pointer: `const <idx> 15 null`
  - C-string pointer payload: `const <idx> 15 charptr "<string_data>"`
  - shell compiled function pointer:
    `const <idx> 15 shellfn_asm "<escaped_nested_pscalasm2>"`
  - nil pointer that keeps its base type (e.g. an Aether array-of-records
    literal's elements): `const <idx> 15 typed_nil "<json_ast>"`, or
    `typed_nil none` for an untyped one
  - opaque raw pointer address: `const <idx> 15 opaque_addr <address>`
- `ARRAY` 11:
  `const <idx> 11 dims <n> elem <var_type> bounds <lb0> <ub0> ... values <total> <v0> <v1> ...`
  - `values` lists scalar or string elements untyped, each read as `elem`.
  - `typed_values <total>` instead gives each element as `<var_type> <payload>`,
    so an element can be any constant above, including another array. pscald
    uses it whenever an element's own type differs from `elem` or has no
    untyped spelling.
  - `dims 0 elem <var_type> bounds values 0` is an empty dynamic array
    (Aether's `[]`).
- Serialized constant globals are preserved with:
  - `const_symbols <count>`
  - `const_symbol "<name>" <var_type> ...payload...`
- Serialized type-table entries are preserved with:
  - `types <count>`
  - `type "<name>" "<json_ast>"`
- Procedures are `proc <idx> "<name>" <address> <locals> <upvalues> <var_type>
  <arity> <enclosing_idx>`, followed by `captures` and/or `escapes` for a
  routine the compiler marked as capturing or escaping closure state.

The `<json_ast>` pscald writes is compact JSON that carries every field the
bytecode cache stores for an AST node (`var_type_id`, `i_val`, `by_ref`,
`is_inline`, `is_virtual`, `is_global_scope`, token, children), and pscalasm
reads it exactly as written. The indented `--dump-ast-json` form also loads,
but it omits some of those fields.

## Stdin Mode

Use `-` to read from stdin:

```sh
pscald --emit-asm input.pbc | pscalasm - rebuilt.pbc
```

## Exit Status

- `0` on success.
- Non-zero on parse/assembly errors or file I/O failures.

## Legacy Compatibility

Legacy block mode still works:

```sh
pscald --asm input.pbc 2> dump.txt
pscalasm dump.txt rebuilt.pbc
```

## See Also

- `pscald` — disassembler and asm exporter (`--emit-asm`, `--asm`)
- `pscalvm` — bytecode execution runtime
