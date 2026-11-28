# `foolc` Source Structure

How the compiler's source tree is organised, what each module owns, and how a Fool file travels from text to HIR.
For the language itself see [`language.md`](language.md); for usage and flags see the [README](../README.md).

Contents: [1. Overview](#1-overview) · [2. Directory map](#2-directory-map) · [3. The pipeline](#3-the-pipeline) ·
[4. Modules](#4-modules) · [5. Conventions](#5-conventions) · [6. Adding a language feature](#6-adding-a-language-feature) ·
[7. Tests](#7-tests) · [8. Build](#8-build) · [9. Not built yet](#9-not-built-yet)

---

## 1. Overview

`foolc` is C++23, built from C++ modules (`.ixx` interface files, `.cpp` implementation units) with CMake + Ninja and
clang. It is a front end: it reads Fool source, checks it, lowers function bodies to a typed high-level IR (HIR) and
builds a control-flow graph (MIR) from that. There is no code generator yet.

```text
 .fo / .fl / .fex
        │  Lexer ──► Parser ──► AST (arena)          [or: .flbmi cache hit skips these two]
        ▼
 ModuleGraphPass1   index files by module, resolve #import, topological order
        ▼
 SymbolTablePass0   per module group: target pruning, @if, constx values, declarations, imports
        ▼
 TypeCheckerPass2   names, types, immutability, optionals, lambdas, casts, serialization
        ▼
 BorrowCheckerPass  (opt-in, --check-borrow)
        ▼
 hir::lower_module  typed, resolved, desugared bodies; generics monomorphized   (--dump-hir)
        ▼
 mir::lower         basic blocks, explicit evaluation order, places; mir::verify    (--dump-mir)
```

---

## 2. Directory map

```text
src/
├── main.cpp                  driver: option parsing, file mode, project mode, dump flags
├── Common/
│   ├── Common.ixx            module fool.common: SourceLocation/Span, FileRole, Arena, StringInterner/SymbolID
│   ├── Diagnostics.ixx       module fool.diagnostics: Severity, Diagnostic, DiagnosticEngine
│   └── StringInterner.cpp
├── Lexer/                    module fool.lexer: TokenKind, Token, Lexer (zero-copy)
├── AST/                      module fool.ast: every node type; arena-allocated, never mutated after parsing
├── Parser/
│   ├── Parser.ixx            module fool.parser: the Parser class
│   ├── Parser.cpp            token helpers, error recovery, expressions (Pratt loop)
│   └── ParserDecl.cpp        declarations, directives, statements (if/while/for/...)
├── Sema/                     module fool.sema (+ fool.borrow)
│   ├── SymbolTable.ixx       TargetConfig, ConstEvaluator, SymbolTablePass0, ModuleGraphPass1,
│   │                         TypeCheckerPass2 and the structures they share (SymbolBinding, ResolvedCall, ...)
│   ├── SymbolTable.cpp       Pass 0: target pruning, @if, declaration collection, imports
│   ├── ConstEval.cpp         `constx` evaluation with range/overflow checks
│   ├── ModuleGraph.cpp       Pass 1: file indexing, import resolution, cycles, topological order
│   ├── TypeChecker.cpp       Pass 2 core: declarations, statements, scopes, return paths
│   ├── TypeCheckerExpr.cpp   expression typing, implicit conversions, calls, struct literals
│   ├── TypeCheckerCasts.cpp  `as` and the named casts, type sizes
│   ├── TypeCheckerOptional.cpp  optional narrowing (and `kill_assigned_in`)
│   ├── TypeCheckerLambda.cpp lambdas, function types, capture rules
│   ├── TypeCheckerSerialize.cpp  `$serialize`, `serialize(x)`, `&` reflection
│   ├── BorrowChecker.ixx / .cpp  opt-in intra-procedural borrow checker
├── HIR/
│   ├── HIR.ixx               module fool.hir: Type/TypeTable, Expr, Stmt, Function, Module, lower_module, dump
│   ├── HIR.cpp               TypeTable, Module helpers, the `--dump-hir` printer
│   └── Lower.cpp             the Lowerer: typing, conversions, calls, generics instantiation, statements
├── MIR/
│   ├── MIR.ixx               module fool.mir: Place, Operand, Rvalue, Statement, Terminator, Block, Function, Module
│   ├── MIR.cpp               the `--dump-mir` printer, the verifier, `--mir-selftest`
│   └── Build.cpp             the Builder: HIR -> blocks (control flow, evaluation order, places)
├── Module/                   modules fool.bmi (the `.flbmi` cache), fool.toml, fool.manifest
│   ├── BMICache.ixx / .cpp   Sha256, BMICacheManager, key and file layout, BMI_FORMAT_VERSION
│   ├── BMICodec.cpp          lossless binary encoding of the AST (one case per node type)
│   ├── BMISelfTest.cpp       `--bmi-selftest`
│   ├── Sha256.cpp
│   ├── Toml.ixx / .cpp       module fool.toml: the TOML reader (tables, strings, numbers, arrays, inline tables), `merge`
│   ├── TomlSelfTest.cpp      `--toml-selftest`
│   └── Manifest.ixx / .cpp   module fool.manifest: `fool.toml` + `overwrite.toml` -> Manifest, `locate_package`
└── Codegen/ …                placeholders (see §9)

stdlib/       the standard library in Fool: core, io, sync, async, net, math, sys (.fo/.fl/.fex triads)
examples/     small programs used as documentation and as part of the project-wide check
tests/        regression tests (see §7)
editors/      VS Code extension and TextMate bundle (syntax highlighting)
docs/         language.md, source-structure.md
.agent/       rules and skills for AI agents working on the language and the compiler
```

---

## 3. The pipeline

**Driver (`main.cpp`).** File mode checks the named files as one workspace; project mode (the default with no files)
indexes `stdlib/` and `examples/` plus any `-I` roots. Both build a `ModuleGraphPass1`, then, for each module in
topological order, call `check_module_group`: build a `SymbolTablePass0`, add the imports that were already built, run
`TypeCheckerPass2`, optionally the borrow checker, and then whichever dump the flags ask for. Each distinct set of
import-template arguments is checked again as its own instantiation.

**Lexer and parser.** The lexer returns tokens that point into the source text (no copies; identifiers are interned).
The parser is recursive descent with a Pratt loop for binary operators (`Parser::get_precedence`) and error recovery:
`synchronize_statement` skips to the next `;`, `}` or statement keyword, and every recovery path must consume at least
one token. Nodes are allocated in an `Arena`.

**`.flbmi` cache.** When enabled (`--cache`), a file whose content, target and compiler front end are unchanged is
decoded from `BMICodec` instead of being lexed and parsed. The key is `SHA-256(format version ‖ front-end fingerprint ‖
source ‖ template args ‖ target)`. `--bmi-roundtrip` forces every parse through encode and decode, which is how the
codec is tested against every other fixture.

**Pass 0 (`SymbolTablePass0`).** Evaluates `@target`, top-level and statement-level `@if`, module parameters and
`constx` values, collects declarations from all files of a module and records imports. It never mutates the AST, so one
tree can be checked under several configurations. (`active_branch` exposes which `@if` branch was selected.) A
`namespace` is flattened here: its functions and constants are registered as `ns::name` (`find_namespace_function`,
`find_function_from` walk outward from the namespace being checked), and the table also answers visibility
(`is_accessible`: the declaration is `pub`, a twin declaration of the same function is, or both live in one privacy unit,
`unit_key` = directory + file stem without a platform suffix).

**Pass 1 (`ModuleGraphPass1`).** Indexes files by their declared module name. An `#import` is a *path* (`import_base`): it
becomes a file-system location from the root (`@std`, `@src`, `@lib`, `@sup`, `@pack`, a drive, or the importing file's
folder), which names a same-stem unit or else a folder (`match_files`). Files not indexed yet are loaded from disk
(`load_from_disk`) and their imports followed. The files' module sections are the dependency edges, and the result
(`ResolvedImport`: module plus the privacy units the path covered) is what Pass 0 filters its views with. It also validates
template arguments, reports unresolved, self and cyclic imports, and produces the compilation order.
The project root of a file is the nearest folder with a `fool.toml` (`find_manifest_dir`); `manifest_for` loads that
folder's `fool.toml` + `overwrite.toml` once (`Manifest`), which renames `@src`/`@lib` and drives `@pack:` through
`find_package` (the importing file's project first, then the project of every `-I` root; see `locate_package`). The
recursive indexing of a search root skips each project's `packages` folder, so installed packages load only when imported.
Pass 0 turns each resolved target into an `ImportedModule` view (files covered, `{names}` selected, `pub` re-export);
every `find_*` lookup walks the views, so a name that was not imported is simply not found.

**Pass 2 (`TypeCheckerPass2`).** Checks one module group: declaration consistency between `.fo` and `.fl`, scopes and
shadowing, immutability, receivers, argument binding, expression types and implicit conversions, optionals, lambdas
and captures, casts, serialization, return paths, and visibility (`check_access` / `check_type_access` /
`check_field_access` at each use of a function, constant, type, field or method; the `find_*` lookups themselves do not
filter, so one error is reported at the use instead of a misleading "unknown name"). It records for every call which declaration it bound
(`find_resolved_call`) and for every expression its inferred type (`expression_types()`); HIR lowering reads both.

**Borrow checker.** Walks each function body twice (collect uses, then check) tracking which locals are borrowed while a
borrow is live. Opt-in with `--check-borrow`.

**HIR lowering (`hir::lower_module`).** Per function with a body: type every expression, resolve every name to a local
index, global or callee, make every conversion explicit, fill in default arguments, desugar `@if`, and lower loops.
Generics are monomorphized through a queue of instantiations. A function that uses something HIR cannot express yet is
kept as "not lowerable yet" with the reason and line, never silently dropped. Details are in the README section "HIR v1".

**MIR (`mir::lower`).** Reads only the HIR module. Each lowered function becomes a list of basic blocks: `if`, loops,
`break`, `continue`, `return`, `&&` and `||` turn into blocks and jumps, every compound expression into statements that write
temporaries (copying a read out first when a later operand calls something), every memory access into a place with
projections. Blocks that nothing reaches (the code after a `return`) are removed. `mir::verify` then checks the graph;
`--dump-mir` runs it on every dump and reports a failure as an internal error. Details are in the README section "MIR v1".

---

## 4. Modules

| C++ module | Files | Exports |
| :--- | :--- | :--- |
| `fool.common` | `Common/Common.ixx` | `SourceLocation`, `SourceSpan`, `FileRole`, `Arena`, `StringInterner`, `SymbolID` |
| `fool.diagnostics` | `Common/Diagnostics.ixx` | `Severity`, `Diagnostic`, `DiagnosticEngine` |
| `fool.lexer` | `Lexer/` | `TokenKind`, `Token`, `Lexer` |
| `fool.ast` | `AST/AST.ixx` | the node classes (`ExprNode`, `StmtNode`, `DeclNode`, `TypeNode` hierarchies) |
| `fool.parser` | `Parser/` | `Parser` |
| `fool.sema` | `Sema/SymbolTable.ixx` + the `.cpp` files | `SymbolTablePass0`, `ModuleGraphPass1`, `TypeCheckerPass2`, `ConstEvaluator`, `TargetConfig` |
| `fool.borrow` | `Sema/BorrowChecker.ixx` | `BorrowCheckerPass` |
| `fool.hir` | `HIR/` | `Type`, `TypeTable`, `Expr`, `Stmt`, `Function`, `Module`, `lower_module`, `dump` |
| `fool.mir` | `MIR/` | `Place`, `Operand`, `Rvalue`, `Statement`, `Terminator`, `Block`, `Function`, `Module`, `lower`, `verify`, `dump` |
| `fool.bmi` | `Module/` | `Sha256`, `BMICacheManager`, the codec |
| `fool.toml` | `Module/Toml.*` | `toml::Value`, `parse`, `merge` |
| `fool.manifest` | `Module/Manifest.*` | `Manifest`, `load_manifest`, `locate_package`, `version_matches` |

Dependency order, each importing only what is above it: `common` → `diagnostics` → `lexer` → `ast` → `parser`;
`sema` uses `ast` and `parser`; `borrow` and `hir` use `sema`; `mir` uses `hir` (it never looks at the AST or the checker
again); `bmi` uses `parser` and `ast`; `manifest` uses `toml` and `diagnostics`, and `sema` uses `manifest`; `main.cpp` uses all.

---

## 5. Conventions

These are enforced in review; `.agent/rules/foolc-compiler-rules.md` is the authority.

- **Class layout.** Member variables first (`m_` prefix), then constructors and destructors, then member functions.
  Plain structs: fields, constructors, helpers.
- **AST nodes and compilation data live in the `Arena`.** Never `new` or `make_unique` for AST nodes. Strings and
  identifiers are interned to 32-bit `SymbolID`s and compared as integers. (HIR data is ordinary C++ and owns itself.)
- **Includes.** In module units, standard headers go in the global module fragment; in `main.cpp`, standard
  `#include`s precede every `import fool.*`.
- **Parser progress.** Error recovery must always advance the cursor; `consume` never loops on a missing token.
- **Argument evaluation order is unspecified** (right to left on the Windows ABI). Never write `x->field` and
  `std::move(x)` in one call; compute values into locals first. Likewise do not keep a `const Type&` into the HIR type
  table across an `intern` call, because the table is a `std::vector` that can reallocate.
- **The AST is immutable after parsing.** Passes keep their own side tables.
- **Do not build inside the repository tree.** Use a scratch copy.

---

## 6. Adding a language feature

A change that adds or alters syntax touches the same places in the same order:

1. **Lexer** (`Lexer.ixx/.cpp`): new tokens or keywords.
2. **AST** (`AST.ixx`): the node and its fields.
3. **Parser** (`Parser.cpp` / `ParserDecl.cpp`): parse it, with recovery that consumes tokens and a clear message for
   the old spelling if one is being replaced.
4. **BMI codec** (`BMICodec.cpp`): write and read every new field, then **bump `BMI_FORMAT_VERSION`**
   (`BMICache.ixx`). `--bmi-roundtrip` runs the whole suite through the codec, so a forgotten field shows up as a diff.
5. **Pass 0** if the feature affects pruning or constants; **Pass 2** for the checks (scopes, types, narrowing). Every
   walker that switches over statement or expression kinds needs the new case: the type checker, `kill_assigned_in`
   in `TypeCheckerOptional.cpp`, the borrow checker.
6. **HIR** (`HIR.ixx`, `Lower.cpp`, `HIR.cpp` printer): lower it, or make the function "not lowerable yet" with a reason.
   A new HIR expression or statement kind also needs a case in the MIR builder (`MIR/Build.cpp`), and a new MIR
   construct a case in the printer and the verifier (`MIR/MIR.cpp`).
7. **Tests** (below) and the **README / `docs/`**. Editor grammars live in `editors/`.

Example: `pub` and `namespace` touched the lexer (two keywords), `DeclNode::m_is_pub` / `m_namespace` and `NamespaceDeclNode`
(AST), `parse_declaration` and `parse_namespace`, the codec (a new declaration tag and fields; version 4 → 5), Pass 0
(qualified keys, the privacy unit), every checker use site, and HIR's names (`math::square`). Migrating the standard
library meant adding `pub` to every `.fo`; running the project check and following the "declared here" notes finds the rest.

Example: the `for (init; condition; step)` loop touched `ForStmt` (AST), `parse_for`, `StmtFor` in the codec (version 3 → 4),
the checker's scope handling, `kill_assigned_in`, the borrow checker's `walk_loop`, and HIR's `For` statement.

Example: `match` touched `KwMatch` / `FatArrow` (lexer), `MatchStmt` / `MatchArm` (AST), `parse_match`, `StmtMatch` in the codec
(version 6 → 7), `check_match` (arms, payload bindings, exhaustiveness), `stmt_always_returns` / `stmt_always_exits` /
`kill_assigned_in`, the borrow checker's `walk_statement`, HIR's `Match` statement (`MatchCase`), and in MIR the `Payload`
projection and the `Switch` terminator (builder, block pruning and renumbering, printer, verifier and its selftest).

Example: generic enums in HIR touched `enum_type` (instances named like class instances, payload types lowered under the enum's
own type arguments), `lower_enum_value` (type arguments from the path, the expected type or the payload values) and
`EnumInfo::m_args`; MIR needed nothing, since it finds an enum by its instance name.

Example: explicit enum values touched `EnumDeclNode::m_underlying` and `EnumVariant::m_value` (AST), `parse_enum`, the codec
(version 8 → 9), `evaluate_enum_values` (`Sema/ConstEval.cpp`, used by both the checker and HIR lowering, so they cannot
disagree), `check_enum_cast`, and HIR's `EnumInfo::m_underlying` / `EnumVariantInfo::m_value` with the dump.

Example: `match` as an expression touched `MatchExpr` and `MatchArm::m_value` (AST), `parse_match_expression` / `parse_match_pattern`
(parser; a primary expression starting with `match`), `ExprMatch` in the codec (version 7 → 8), `check_match_arms` (shared with
the statement, plus the common arm type, recorded in `m_expr_types`), `kill_assigned_in` and the borrow checker's `walk_expr`, HIR's
`Match` expression (`MatchCase::m_value`), and in MIR `lower_match_value` (the result temporary and the join block).

---

## 7. Tests

Tests that expect foolc to reject something (`negative` project fixtures, `foolc_warn_werror_fails`) run through
`tests/expect_failure.cmake`: it requires a plain non-zero exit and a matching message, then exits 0 itself, so they read as
passes in ctest, CLion and CI (not as a failing command rescued by `PASS_REGULAR_EXPRESSION`).

`ctest --test-dir <build> -j` runs everything; `-L <label>` selects a group. All fixtures are `.fl` files under `tests/`.

| Directory | What it checks | Label |
| :--- | :--- | :--- |
| `tests/positive/` | must produce no errors | `positive` |
| `tests/negative/` | each `// expect-error: <text>` must be reported (matched by text) | `negative` |
| `tests/borrow/` | the borrow checker (`--check-borrow`) with the same markers | `borrow` |
| `tests/warn/` | `--warn-shadow` with `// expect-warning:`; a clean file must also pass `-Werror` | `warn` |
| `tests/calls/` | `--dump-calls` golden files (`.expected`) | `calls` |
| `tests/types/` | `--dump-types` golden files | `types` |
| `tests/reflection/` | `--dump-reflection` golden files | `reflection` |
| `tests/hir/` | `--dump-hir` golden files | `hir` |
| `tests/mir/` | `--dump-mir` golden files; the verifier runs on each dump. Also `foolc_mir_selftest` and `foolc_mir_project` (every function of `stdlib/` + `examples/`) | `mir` |
| `tests/project/` | multi-file module graphs (ok, cycle, unresolved, bad template, interface-only, ...) and visibility (`vis_ok`, `vis_private`: one run per expected message) | `project` |
| `tests/project/pkg_*` | `fool.toml` / `overwrite.toml`: local and shared packages, overrides, renamed folders, version and git-source errors, broken manifests; run with `--show-packages`. Also `foolc_toml_selftest` (the TOML reader and version matching) | `packages` |
| `tests/cache/` | the `.flbmi` cache cycle | `bmi` |

Golden tests run through `tests/calls/run_golden.cmake`, which runs `foolc -q <flag> <file>` and compares the output with
the `.expected` file. The `calls`, `types`, `reflection`, `hir` and `mir` groups run a second time with `--bmi-roundtrip`
(`*_roundtrip`, label `bmi`). To refresh a golden file after an intended change, regenerate it with the same command
and review the diff. The whole `stdlib/` + `examples/` workspace is also checked in project mode, with and without the
borrow checker and for several `--target`s.

---

## 8. Build

Requirements: CMake 3.28+, Ninja, clang 18 or newer (C++23 modules need `clang-scan-deps`; pass
`-DCMAKE_CXX_COMPILER_CLANG_SCAN_DEPS=` if CMake picks the wrong one). The only target is the `foolc` executable;
the front-end fingerprint used in the cache key is computed by CMake from the lexer, parser and AST sources.

```text
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=clang++
ninja -C build
ctest --test-dir build -j
```

---

## 9. Not built yet

- `src/Codegen/` (`BackendInterface`, `LLVM/LLVMCodeGen`, `Native/FastEmitter`), `src/Module/ModuleGraph.ixx` and
  `src/Module/PatchOverlay.ixx` are empty placeholders for the planned back ends and are not part of the build.
- Optimization passes, SSA, linking, and everything after MIR (code generation).
- A language server; the editor extensions are syntax highlighting only.
