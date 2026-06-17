# pscal-core

The shared **PSCAL** runtime and toolkit: the bytecode VM, the AST -> bytecode
compiler, the shared AST, lexer, symbol table, type registry, the builtin
backend, the extended-builtin library, and the `vproc` platform layer.

PSCAL language front ends — Pascal, CLike, Rea, Aether, and the exsh shell —
parse their own syntax into this shared AST and run it on this one VM. They each
build against `pscal-core` rather than carrying their own VM or codegen. This
repository is the foundation they share.

## Build

```sh
cmake -S . -B build
cmake --build build -j
```

Produces `libpscal_core_static.a`. The current build is the minimal,
dependency-light configuration (links only `libm` and threads). Optional
features (SDL graphics/audio, libcurl networking, SQLite) are gated and added as
the build matures.

## Layout

- `src/vm`, `src/compiler` — the bytecode VM and AST -> bytecode compiler (the ISA).
- `src/ast`, `src/symbol`, `src/lexer`, `src/core` — shared AST, symbol/type tables, lexer, runtime core.
- `src/backend_ast`, `src/ext_builtins` — the builtin dispatch backend and the extended builtin library.
- `src/runtime` — terrain/shader data generators and the `vproc` virtual-process platform layer.
- `src/third_party/yyjson`, `third-party/nextvi`, `lib/noise` — vendored support.

## History

This repository was extracted from the PSCAL monorepo (emkey1/pscal) with full
per-file history preserved.
