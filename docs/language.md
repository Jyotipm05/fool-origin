# The Fool Language

This is the reference for the language as `foolc` accepts it today. It describes what the compiler parses and checks;
where something is only parsed or only type-checked (there is no code generator yet), the text says so. The
[README](../README.md) has the design rationale and the command-line usage, and
[`source-structure.md`](source-structure.md) explains how the compiler is built.

Contents: [1. Source files](#1-source-files) · [2. Lexical structure](#2-lexical-structure) · [3. Modules and imports](#3-modules-and-imports) ·
[4. Types](#4-types) · [5. Declarations](#5-declarations) · [6. Statements](#6-statements) · [7. Expressions](#7-expressions) ·
[8. Directives](#8-directives) · [9. Implicit conversions](#9-implicit-conversions) · [10. What does not exist yet](#10-what-does-not-exist-yet)

---

## 1. Source files

A module is spread over up to three kinds of file, each with a fixed job:

| Extension | Role | May contain |
| :--- | :--- | :--- |
| `.fo` | interface | declarations only: constants, `class` / `struct` / `enum` / `ruleset` / `blueprint` definitions, function and method signatures. No function bodies. |
| `.fl` | implementation | function and method bodies. Declares `module <name>;` and never imports its own `.fo`. |
| `.fex` | overlay | foreign and platform-specific bindings, gated by `@target(...)`. |

A `.fo` does not need a `.fl` (a pure interface module is valid) and a `.fl` does not need a `.fo`. Every file starts with
`module a::b::c;`, and all files that name the same module form one module. Platform variants of a file are picked by
the stem suffix: `memory_posix.fex`, `mutex_linux.fex`, `file_windows.fl`, `..._wasm`, `..._wasi`, `..._darwin`.

---

## 2. Lexical structure

- **Comments:** `// to end of line` and `/* block */`.
- **Identifiers:** letters, digits and `_`, not starting with a digit. Names are case-sensitive.
- **Integer literals:** decimal (`4096`), hexadecimal (`0xFF`), binary (`0b1010`); `_` may separate digits (`5_000_000`).
- **Float literals:** `2.5`, `1.0`.
- **Strings:** `"text"`. A string literal has type `const* u8`.
- **Keywords:** `import module pub namespace class struct enum ruleset blueprint fn async await this var const constx ref
  return if else for in while break continue true false null as`. `in` and `ref` are reserved and not used by any construct yet.
- **Marker characters:** `#import` (import directive), `@` (compile-time directive, never an operator),
  `$` (the `$serialize` attribute), `&` (address-of, reference types, and reflection when followed by
  `field_count(`, `type_name(` or `is_serializable(`).

Statements end with `;`. Blocks are `{ ... }`.

---

## 3. Modules and imports

```fool
module net::http<TLS = 0, PORT = 80>;      // optional template parameters with default values

#import @std:core;                         // everything public in the standard library's core folder
#import @src:net:http<TLS = 1>;            // override compile-time parameters at the import site
#import {Counter, make} from store;        // only these names, from the file `store` next to this one
pub #import @lib:shared:clock;             // import and re-export: importers of this file see it too
```

An import names **files by path**. Segments are separated by single colons (`a:b:c` is the file `a/b/c`); `::` is for
names inside a module and is not accepted in an import path.

| Path starts with | Meaning |
| :--- | :--- |
| `name:...` | next to the importing file |
| `@std:...` | the standard library (`--std-root`, default `stdlib`) |
| `@src:...` | the project's source folder (the nearest enclosing `src`, else `<project root>/src`; `[package] src` renames it) |
| `@lib:...` | the `lib` folder of the library or package (found the same way; `[package] lib` renames it) |
| `@sup:...` | the folder above the importing file's; `@sup:@sup:x` goes up two |
| `@pack:name:...` | the package `name` (its `lib` folder if it has one): from the project's `packages` folder or a shared collection, see below |
| `:c:...` | a drive path (`C:/...`) |

The project root is the nearest folder above the file that holds a `fool.toml`, else the `-I` search root.

- **What a path names.** A file name stands for the whole *unit*: `store.fo`, `store.fl` and `store_linux.fex` together. If no
  such file exists but a folder does, the import covers every file directly inside that folder (not sub-folders): `@std:core`
  is all of `stdlib/core/*`. Files named by an import that were not indexed from a search root are loaded from disk.
- **What it brings in.** The `pub` declarations of exactly those files (plus the private ones, which then fail with a
  "private" error when used). Names are used unqualified, or with the last path segment in front (`store::make()`).
- **`{a, b} from path`** brings in only those names. Each must exist in the named files and be usable from here
  (`'x' is not declared in 'path'`, `'x' is private to 'file'`). A namespace can be selected by its name.
- **`pub #import`** re-exports: whoever imports this file also gets what it imported, narrowed by `{...}` if given.
- **Modules.** `module a::b;` still names the module a file belongs to and owns its template parameters; a path can reach files
  of several modules. Template arguments (`<A = 1>`) need a path that names exactly one module.
- A module template parameter is a compile-time integer. Code can test it with `@if def(NAME) && (NAME == 1)`.
  Every distinct set of arguments an importer uses is checked as its own instantiation.
- Imports must resolve, may not import the module itself, and may not form a cycle. The compiler orders modules
  topologically.
- A name from another module is reached with a path: `sys_mem::os_alloc_pages(n)`, `Shape::Circle`.
- Only `pub` declarations of an imported module can be used (next section).

### Projects and packages: `fool.toml` and `overwrite.toml`

A folder with a `fool.toml` is a project (or a package: the same file describes both). The file is optional; without
one every default below applies.

```toml
# fool.toml: shared with everyone who gets the project
[package]
name    = "geometry-app"
version = "0.3.0"
src     = "src"           # what @src: means (default "src")
lib     = "lib"           # what @lib: means, and the folder other projects import from (default "lib")

[packages]
dir = "packages"          # where this project installs packages (default "packages")

[dependencies]
shapes = "1"                                      # any 1.x, found in packages/ or a shared collection
mathx  = { version = "2", path = "../mathx" }     # use this folder (relative to fool.toml)
remote = { git = "https://example.com/r.git" }    # recorded for the package manager; not fetched yet
```

```toml
# overwrite.toml: next to fool.toml, personal to this machine, left out of what you distribute
[packages]
collections = ["~/.fool/packages", "D:/fool/packages"]    # shared package collections (or: collection = "...")

[dependencies]
shapes = { path = "../shapes-fork" }                      # work on a dependency without touching fool.toml
```

`overwrite.toml` is laid over `fool.toml` key by key (tables merge, anything else is replaced), so it can name any setting;
the usual uses are the machine's package collection and local copies of dependencies. Keep it out of version control.
Paths are relative to the project folder; `~` is the home folder. Problems are reported against the file they are in, with
line and column; unknown keys and tables are warnings.

**Where `@pack:name:...` looks**, first match wins:

1. the project's own `path` for the dependency (`[dependencies]`, possibly from `overwrite.toml`);
2. `<project>/packages/name`;
3. `<collection>/name` for each shared collection, in the order listed.

A package is a folder `name` containing its source (and optionally its own `fool.toml`). Its importable files are in its
`lib` folder when it has one, else the folder itself. A dependency's `version` is a prefix of the package's version
(`1` accepts `1.4.2`, `1.4` does not accept `1.5.0`; `*` or no version accepts anything); the version comes from the
package's own `fool.toml` and is only checked when both sides state one. A project can use local and shared packages
side by side, so one shared copy serves every project that depends on it.

A package that has its own `fool.toml` resolves its own `@pack:` imports first through that file, then through the
project being compiled (its `packages` folder and collections). Installed packages are not scanned up front: a package
is read only when something imports it. `foolc --show-packages` prints each project's settings and where every
dependency resolves.

### Visibility: `pub`

A function, variable, constant, class, struct, enum, ruleset or blueprint is **private to its file** unless it is
written `pub`:

```fool
pub fn area(w: i32, h: i32) -> i32 { return w * h; }   // other files may call it
fn helper() -> i32 { return 3; }                       // this file only
pub constx LIMIT: i32 = 10;
pub class Counter { ... }
```

- **Privacy unit.** Files with the same name stem form one unit and see each other's private declarations:
  `store.fo`, `store.fl` and `store_linux.fex` (a platform suffix does not change the stem). A different stem is a
  different file, even inside the same module.
- **Interface and definition.** `pub` on a declaration covers its definition: `pub fn make() -> Counter;` in `store.fo`
  makes the `fn make() -> Counter { ... }` in `store.fl` public; you do not repeat it.
- **Members.** Fields and methods of a class or struct are private to the file unless each is written `pub` *inside* the
  class body (`pub n: i32;`, `pub fn bump(this &self);`). A type being `pub` does not make its members public.
  Enum variants follow their enum; the functions of a ruleset or blueprint follow it.
- A use of a private name from another unit is an error that names the file and points at the declaration
  (`function 'helper' is private to 'store.fo'; declare it `pub` to use it from another file`).

### Namespaces

A `namespace` groups functions and constants under a name. Its members are reached as `name::item`:

```fool
pub namespace geo {
    constx UNIT: i32 = 1;

    fn area(w: i32, h: i32) -> i32 { return w * h * UNIT; }   // siblings are used by their plain name

    namespace inner {                                          // namespaces nest: geo::inner::...
        fn perimeter(w: i32, h: i32) -> i32 { return 2 * (w + h) * UNIT; }
    }
}

fn run() -> i32 { return geo::area(2, 3) + geo::inner::perimeter(1, 2); }
```

- `pub namespace` makes **every** member public (including those of nested namespaces); a plain `namespace` keeps its
  members private to the file unless a member is itself written `pub`.
- A namespace can hold functions, constants and variables, and nested namespaces. Types (classes, enums, ...) cannot be
  declared inside one yet.
- The functions are defined inside the block: `fn geo::area(...)` outside it is an error. Bodies live in `.fl` files, as for
  any function.
- A bare name inside a namespace finds the namespace's own members first, then those of the enclosing namespaces, then the
  file's top level.
- After `#import shapes;`, a namespace of that file is used as `geo::area(...)` or `shapes::geo::area(...)`.

---

## 4. Types

### Primitive types

| Type | Meaning |
| :--- | :--- |
| `i8 i16 i32 i64` | signed integers |
| `u8 u16 u32 u64` | unsigned integers |
| `isize usize` | pointer-sized integers (width from `--target`) |
| `f32 f64` | IEEE floats |
| `bool` | `true` / `false` |
| `void` | no value (a function that returns nothing) |

An unannotated integer literal is `i32` (or `i64` when it does not fit), a float literal is `f64`; a literal adapts to
the type its context asks for (`var byte: u8 = 7;`, `scale: f32 = 3`).

### Views: pointers and references

| Spelling | Meaning |
| :--- | :--- |
| `*T` / `const* T` | pointer to `T`, writable / read-only |
| `&T` / `const& T` | reference to `T`, writable / read-only |

`const` is transitive: nothing reachable through a `const&` can be modified, and a `this &self` method cannot be
called on a `const&`. `.` reaches a member through a value, a reference or a pointer alike (`p.x`), so changing a
variable from `T` to `&T` does not change the code using it. `*p` dereferences, `&x` takes an address. `&T` and `*T`
convert to each other, and `const` can be added but never removed implicitly.

### Optionals: `T?`

A value that may be absent. `null` is the empty value, and only an optional can hold it. `T` converts to `T?`, never
back. The compiler narrows an optional to its value after `if (a)`, `a != null`, an early `return` in
`if (a == null)`, inside `&&` / `||`, in a loop condition's body, and after assigning a plain value; see the README
section on optionals for the full list. `*T?` is a nullable pointer. `T??` is rejected.

### Function types

`fn(i32, f32) -> bool` is the type of a function value; `fn()` takes and returns nothing.

### Classes, structs and enums

```fool
struct Vec2 {                         // plain data aggregate
    x: f32;
    y: f32;
}

class Counter {                       // fields, then method declarations (bodies live out of the class)
    value: i32;
    step: Vec2?;

    fn bump(this &self, by: i32 = 1) -> i32;
    fn make(start: i32) -> Counter;   // no receiver: a static function
}

enum Shape {
    Circle(f64),                      // a variant may carry a payload
    Empty,
}
```

- Fields are written `name: Type;` and laid out in declaration order with natural C alignment.
- A class declares its methods without bodies (in the `.fo`, or at the top of the `.fl`); the definition is outside:
  `fn Counter::bump(this &self, by: i32) -> i32 { ... }`. The two must agree (checked).
- There is **no inheritance**. Shared behaviour is expressed with rulesets.
- Struct literal: `Vec2 { x: 1.0, y: 2.0 }`; an optional field that the literal leaves out starts as `null`.
- Duplicate fields, variants and definitions are errors.

### Generics

```fool
fn identity<T>(x: T) -> T { return x; }

class Box<T> {
    item: T;
    fn get(this const& self) -> T;
}
fn Box<T>::get(this const& self) -> T { return self.item; }

var a = identity(3);                  // identity<i32>
var b: Box<i64> = Box<i64> { item: 6 };
```

Generic functions, classes and their methods are monomorphized: each distinct set of type arguments is compiled
separately. Type arguments are inferred from call arguments and receivers or written explicitly (`Box<i64>::make(6)`).
Today the checker treats a generic parameter as an unknown type (never an error) and the instantiation happens in HIR
lowering. Constant generic arguments, generic enums and instantiating a generic from another module are not done yet.

### Rulesets and blueprints

```fool
ruleset Equatable<T> {                // nominal capability: what a type must offer
    fn equals(this const& self, other: const& T) -> bool;
}
ruleset Printable<T: Equatable> {     // a bound on a parameter
    fn to_string(this const& self) -> StringView;
}

blueprint VectorMath<SIMD: bool = false> {   // a compile-time template of functions
    fn dot_product(a: const* f32, b: const* f32, len: usize) -> f32;
}
```

Rulesets replace inheritance and C++ concepts. Their conformance is parsed and name-checked but not modelled yet
(a class does not declare which rulesets it satisfies).

---

## 5. Declarations

### Constants and globals

```fool
constx BUFFER_CAPACITY: usize = 4096;     // compile-time constant, evaluated by the compiler
const LIMIT: i32 = 10;                    // immutable
var counter: i32 = 0;                     // mutable global
```

`constx` initializers are folded at compile time, with range and overflow checks (`constx SMALL: u8 = 300;` is an
error); arithmetic, comparisons, casts between scalars and other `constx` names are allowed.
A `const` / `constx` binding must have an initializer. A variable needs a type or an initializer.

### Functions

```fool
fn add(a: i32, b: i32) -> i32 { return a + b; }
fn log(message: const* u8);                         // signature only, in a .fo
async fn fetch(url: const* u8) -> i32;              // async functions are parsed and checked, not lowered yet
```

Functions and lambdas share one syntax, `(params) -> result { body }`; giving it a name makes a function:
`double_it = (x: i32) -> i32 { return x * 2; };`.

### Receivers

Methods have no hidden `this`; the receiver is the first parameter and says how the object is used:

| Receiver | Meaning |
| :--- | :--- |
| `this const& self` | read-only borrow |
| `this &self` | mutable borrow |
| `this self` | consumes the object; it cannot be used afterwards (checked) |

### Defaults and named arguments

Any parameter may have a default, including one in the middle:

```fool
fn render_box(x: f32, y: f32, scale: f32 = 1.0, width: f32, height: f32) -> i32;

render_box(10.0, 20.0, 1.0, 100.0, 200.0);                 // positional
render_box(10.0, 20.0, width = 100.0, height = 200.0);     // labels; `width: 100.0` means the same
```

Inside a call, `name = value` is always a label, never an assignment. Unknown or repeated labels, a positional argument
after a labelled one, missing arguments and type mismatches are errors. Evaluation order of arguments is unspecified.

### Lambdas and captures

```fool
fn scale(factor: i32) -> i32 {
    var total: i32 = 0;
    add = (n: i32) -> i32 capture=[total, factor] { total = total + n * factor; return total; };
    run_task(capture=[total]) { /* a block after a call is a lambda passed as the last argument */ }
}
```

A lambda sees only its parameters, its own locals, the variables in `capture=[...]`, and program-wide functions,
types and constants. Anything else from the enclosing function, and any mutable global, is an error that suggests the
capture list. Captures are shared with the enclosing code (by reference).

---

## 6. Statements

### Variables

```fool
var count: i32 = 0;       // mutable, explicit type
var total = 1.5;          // mutable, type from the initializer (f64)
const name = 42;          // immutable: assignment is an error
limit = 100;              // `x = value;` with no `x` in scope declares a mutable variable
```

An initializer is evaluated before the new name enters scope.

### Scopes

Scoping is C++'s: a block, an `if` / `while` body, a `for` loop (its variable and body) and each branch of a
compile-time `@if` open a scope. An inner declaration shadows an outer one until the end of its block; redeclaring a
name in the *same* scope (a parameter included) is an error. `foolc --warn-shadow` reports shadowing.

### `if`

```fool
if (x < 0) { return 0; } else if (x == 0) { return 1; } else { return 2; }
```

The parentheses are optional. The condition must be `bool` or an optional (true when it holds a value).

### `while`

```fool
while (i < n) { i = i + 1; }
```

### `for`

The loop is written the way C++ writes it, so the type of the variable, where it starts and how it moves are all
in front of you:

```fool
for (var i: usize = 0; i < n; i = i + 1) { ... }          // up by one
for (var i: usize = n; i > 0; i = i - 1) { ... }          // down by one, stops before wrapping below zero
for (var i = 0; i < limit; i = i + 3) { ... }             // up by three; `var i = 0` is an i32 as everywhere
for (var x: f32 = 0.0; x < 1.0; x = x + 0.25) { ... }     // non-integer steps are explicit too
for (; i < limit; ) { ... }                               // every part is optional
for (;;) { ... }                                          // until `break` or `return`
```

- Header: `(init; condition; step)`. `init` is a declaration or an expression statement; `condition` is a `bool` (or an
  optional); `step` is an expression run after every iteration, including after `continue`.
- The variable declared in `init` is visible in the condition, the step and the body, and ends with the loop.
  A `const` loop variable cannot be stepped.
- There is no `for x in collection` / `a..b` form and no iteration protocol yet. `in` is reserved for that design.

### `match`

`match` takes an enum value apart: it runs the first arm whose variant the value holds and gives that arm the payload.

```fool
enum Shape {
    Circle(f64),
    Rect(f64, f64),
    Empty,
}

fn area(shape: Shape) -> f64 {
    match shape {
        Shape::Circle(r) => { return r * r * 3.14; }
        Shape::Rect(w, h) => { return w * h; }
        Shape::Empty => { return 0.0; }
    }
}

match shape {
    Shape::Rect(w, _) => { width = w; }     // `_` in the parentheses skips a payload value
    Shape::Circle => { width = 1.0; }       // no parentheses: the payload is not named
    _ => { }                                // everything else
}
```

- An arm is `Enum::Variant` (optionally `(a, b, ...)`) or `_`, then `=>` and a **block**. Arms are tried in order; a
  comma between arms is allowed and optional.
- The names in `(...)` are new read-only locals, one per payload value, typed as the variant declares, and visible only in
  that arm. With parentheses the count must equal the variant's payload; without them the payload is simply not named.
- Every variant must be covered, or the last arm must be `_`: `match is not exhaustive: missing Shape::Rect (or add a '_'
  arm)`. A variant handled twice, an arm after `_`, a variant the enum does not have, and arms naming different enums are
  errors. The value must be the enum the arms name.
- `match` is a statement, not an expression: it has no value, and an arm's block is not a value either. A function whose
  every arm returns counts as returning on all paths.
- Not yet: nested patterns, matching on integers or strings, guards (`if`), and generic enums (they check, but are not
  lowered to HIR yet).

### `break`, `continue`, `return`

`break` and `continue` apply to the innermost loop and are errors outside one. `return value;` / `return;` must match
the function's result type, and the checker verifies that every path returns.

### Compile-time `@if`

```fool
@if (def(DEBUG)) { log_it(); } @elif (LEVEL == 2) { other(); } @else { fallback(); }
```

The condition is a compile-time constant. Only the selected branch is compiled and checked; the others are pruned.

---

## 7. Expressions

### Operators, highest precedence first

| Level | Operators |
| :--- | :--- |
| 1 | postfix: call `f(x)`, index `a[i]`, member `a.b`, path `a::b` |
| 2 | prefix: `-x`, `!x`, `*p`, `&x`, `await x` |
| 3 | `as` (explicit unchecked cast) |
| 4 | `*  /  %` |
| 5 | `+  -` |
| 6 | `&` |
| 7 | `^` |
| 8 | `\|` |
| 9 | `==  !=  <  <=  >  >=` |
| 10 | `&&` |
| 11 | `\|\|` |
| 12 | `..` (parsed; has no meaning now that `for` does not use it) |
| 13 | `=` assignment, right associative |

`!` is logical not on a `bool` and bitwise complement on an integer. There are no compound assignments (`+=`) and no
`++` / `--`: write `i = i + 1`.

### Casts

| Spelling | Use |
| :--- | :--- |
| `x as T` | unchecked C-style conversion for FFI and low-level code; wraps instead of failing |
| `@static_cast<T>(x)` | numeric conversions, `*void` ⇄ `*U`, `&T` → `*T`, adding `const`; constants that do not fit are errors |
| `@reinterpret_cast<T>(x)` | pointer ⇄ pointer, pointer ⇄ pointer-sized integer |
| `@bit_cast<T>(x)` | reinterpret the bytes of a value of exactly the same size |
| `@dynamic_cast<*T>(x)` | checked downcast of a ruleset view; `null` when the dynamic type differs (type-checked only) |

### Calls

Positional, named (`width = 100.0`), with defaults filled in. A call through a variable that still holds its lambda uses
the lambda's parameter names; through any other function value it is positional.

### Struct literals

`Type { field: value, ... }` or `Type<Args> { ... }`. A struct literal is not allowed where it would be read as a block,
for instance directly in an `if` / `while` condition.

---

## 8. Directives

| Directive | Meaning |
| :--- | :--- |
| `@target(os = "windows", arch = "x86_64", pointer_width = 64)` | include the following declaration only on matching targets; several `@target`s are OR'd, keys inside one are AND'd; `os = "posix"` matches linux, darwin and the BSDs |
| `@extern("C", link_name = "VirtualAlloc")` | declare a foreign function |
| `@inline(...)` | inlining hint |
| `@align_as(N)` | alignment of a type |
| `@if` / `@elif` / `@else` | compile-time conditional, at top level or inside a function |
| `$serialize(format = "json")` | make a class, struct or enum serializable; `format` is `json` (default), `binary` or `text` |

`@` is only ever a directive marker, never an operator. `serialize(x)` converts any serializable value to a
`StringView`; reflection is `&field_count(T)`, `&type_name(x)` and `&is_serializable(T)`.
`foolc --dump-reflection` prints what the checker derived.

---

## 9. Implicit conversions

The compiler converts implicitly only when no information can be lost:

| Allowed implicitly | Needs an explicit cast |
| :--- | :--- |
| integer to a wider integer of the same signedness; unsigned to a strictly wider signed integer | narrowing, or changing signedness |
| `f32` to `f64` | `f64` to `f32`; integer ⇄ float; `bool` ⇄ number |
| `&T` ⇄ `*T`; adding `const` to a view | removing `const`; changing a primitive pointee; integer → pointer |
| `T` to `T?` | `T?` to `T` (narrow it first) |
| class `A` to class `A` | class `A` to an unrelated class `B` |

This applies to initializers, assignments, `return`, call arguments, struct-literal fields, operands (one side must
convert to the other), `&&` / `||` operands and conditions. A type the checker cannot determine is never reported, so a
missed error is possible but a wrong one should not be.

---

## 10. What does not exist yet

- **A package manager.** Packages are found where they were put (`packages/`, a shared collection, a `path`); nothing
  downloads them yet. A `git = "..."` dependency is recorded and reported as needing the package manager. Version
  requirements are a plain prefix match, with no ranges, resolution or lock file.
- **Types inside namespaces.**
- **Code generation.** The compiler parses, checks, lowers to HIR and builds a control-flow graph (MIR); nothing executes yet.
- **Optimization, linking.**
- **More pattern matching.** `match` covers enum variants and their payload (see above). There are no nested patterns,
  no matching on integers or strings, no guards, and no `match` as an expression.
- **Ruleset conformance** (declaring and checking that a class satisfies a ruleset) and the runtime test behind
  `@dynamic_cast`.
- **Iteration over collections** and any use of `in`; compound assignment and `++` / `--`.
- **`deserialize`**, compile-time evaluation of reflection, serialization code.
- **Generics:** constant generic arguments, generic enums, instantiation across modules, constraints enforced on
  arguments.
- **HIR for** lambdas, async functions, reflection intrinsics and generic enums (those functions are reported by
  `--dump-hir` as "not lowerable yet" with the reason).
