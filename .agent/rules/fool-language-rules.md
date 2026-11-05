# Fool Programming Language Rules

This rule defines the core language syntax, semantics, and invariants of the Fool systems programming language.

## 1. File Triad Architecture
Every module in Fool is organized across a compiler-enforced file triad:
- **`.fo` (Interface Blueprints)**: Pure declarations, nominal `ruleset` capability bounds, class memory layouts, and public function/method signatures. No executable function bodies are permitted.
- **`.fl` (Implementation Units)**: Pure Fool algorithmic logic, method bodies, and state machine step transitions.
- **`.fex` (Foreign ABI & Platform Overlays)**: Foreign ABI bindings (`@extern("C")`, `@extern("llvm")`), kernel syscalls, and target-specific overrides gated by `@target(...)`.

## 2. Mutability & Reference Model
- **No `mut` keyword**: The `mut` keyword does not exist in Fool.
- **Mutable by Default**:
  - Variables: `counter = 0;` (or `var count: i32 = 0;`).
  - References: `&T` (e.g. `fn write(buffer: &Span<u8>)`).
  - Pointers: `*T` (e.g. `data: *u8`).
  - Receivers: `this &self` (e.g. `fn append(this &self, slice: StringView)`).
- **Rust-like Strict Immutability with `const`**:
  - Immutable Bindings: `const val = 42;` (or `const val: i32 = 42;`). Re-assignment is a compile-time error.
  - Immutable References: `const& T` (e.g. `fn read(data: const& Span<u8>)`).
  - Immutable Pointers: `const* T` (e.g. `bytes: const* u8`).
  - Immutable Receivers: `this const& self` (e.g. `fn len(this const& self) -> usize`).
  - *Semantics*: Strict transitive read-only protection. Mutating fields, reassigning, calling `this &self` methods on a `const&` instance, or casting away `const` is prohibited.
- **Compile-Time Constants with `constx`**:
  - C++ `constexpr` / Rust `const` equivalent: `constx BUFFER_SIZE: usize = 4096;`. Evaluated strictly at compile time.

## 3. Explicit `this` Receivers
Method declarations do not use implicit hidden `this` pointers. The receiver is explicitly bound as the first parameter:
- **Value Receiver (Move / Consuming)**: `fn destroy(this self)`
- **Immutable Borrow Receiver**: `fn len(this const& self) -> usize`
- **Mutable Borrow Receiver**: `fn append(this &self, slice: StringView) -> bool`

## 4. Parameter Defaults
- Defaults are supported in **any** parameter slot, including middle positions:
  `async fn render_box(x: f32, y: f32, scale: f32 = 1.0, width: f32, height: f32) -> i32;`
- Call sites can disambiguate via labeled arguments (`width: 100.0`).

## 5. Nominal Capability Rulesets
- Fool rejects class inheritance in favor of nominal `ruleset` capability bounds (equivalent to C++20 concepts and Rust traits):
  ```fool
  ruleset Equatable<T> {
      fn operator==(this const& self, other: const& T) -> bool;
      fn operator!=(this const& self, other: const& T) -> bool;
  }
  ```

## 6. Monomorphized Blueprints
- Parameterized compile-time templates use `blueprint`:
  ```fool
  blueprint Math<SIMD: bool = false> {
      fn dot_product(a: const* f32, b: const* f32, length: usize) -> f32;
  }
  ```

## 7. Directive-Only `@`
- `@` is strictly a compile-time directive/attribute marker (`@target`, `@inline`, `@align_as`, `@extern`), never an expression operator.

## 8. Module Import Directives
- Module imports use the `#import` directive:
  ```fool
  #import std::core;
  #import std::io::posix_ffi;
  ```
- **No Self-Import in Implementation Units**: An `.fl` (implementation) file implicitly implements the interface defined in its companion `.fo` file under the same `module <name>;` declaration. An `.fl` file must **never** contain an `#import` for its own module/`.fo` file. Imports from *other* modules are allowed.



