---
name: fool-authoring
description: Guidelines and procedures for authoring Fool standard library modules, user libraries, and applications following the .fo/.fl/.fex file triad and language invariants.
---

# Fool Module Authoring Skill

This skill guides the authoring of idiomatic Fool code.

## 1. Triad Structure Checklist
When creating or modifying a Fool module named `<name>` in a subsystem directory (e.g. `stdlib/core/`):
1. **`<name>.fo`**: Define the public blueprint. Include:
   - Module declaration: `module <full::module::name>;`
   - Ruleset definitions with nominal bounds.
   - Class declarations (fields and layout).
   - Method signatures with explicit receivers (`this const& self` for query/read, `this &self` for mutation, `this self` for consuming).
2. **`<name>.fl`**: Implement algorithmic logic. Include:
   - Method bodies: `fn ClassName::method(this &self, ...) -> Ret { ... }`
3. **`<name>_<target>.fex`** (if platform/ABI specific):
   - Add `@target(os = "...")` or `@target(arch = "...")`.
   - Foreign bindings via `@extern("C")` or `@extern("llvm")`.

## 2. Mutability Syntax Reference
- Use `const` for immutable bindings: `const max_limit = 1024;`
- Use `const` for immutable types: `const& T`, `const* T`, `this const& self`
- Use `constx` for compile-time constants: `constx BUFFER_SIZE: usize = 4096;`
- Use `=` for mutable state: `count = 0;` (or `var count: i32 = 0;`)
- Use `&T` for mutable references (no `mut` keyword!).
- Use `*T` for mutable pointers.

## 3. Module Imports
- Always import external modules using `#import <subsystem::module>;`:
  ```fool
  #import std::core;
  #import std::io::sys_file;
  ```
- **Never import a module's own `.fo` in its companion `.fl`**: An implementation unit implicitly inherits its interface blueprint declarations under the shared `module <name>;` line. Only import *other* external modules.



