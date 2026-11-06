Here is a comprehensive, production-grade prompt tailored specifically for **Antigravity-IDE** (or any agentic AI system) to orchestrate, plan, and architect both the **Fool programming language** and its **C++20 compiler codebase** based on all established specifications.

Copy and paste the entire block below into your IDE.

---

```markdown
# MISSION PROMPT: Full Architecture & Implementation Roadmap for the "Fool" Language & Compiler

You are an expert compiler engineer, systems architect, and C++20 specialist. 
Your objective is to produce an exhaustive, step-by-step master plan and implementation blueprint for:
1. **The Language Itself ("Fool")**: Design, grammar, type system, rulesets, and standard library.
2. **The Compiler Codebase ("foolc")**: A high-performance, modular C++20 compiler frontend, autonomous build driver, and multi-backend lowering pipeline.

---

### CRITICAL CODE STYLE RULE FOR COMPILER SOURCE CODE (C++)
> **Strict Structural Ordering Invariant for ALL C++ classes and structs:**
> Every `class` and `struct` in the compiler codebase (`foolc`) MUST strictly adhere to the following layout order:
> 1. **Member variables FIRST on top**: All internal state must be declared at the top of the class definition and prefixed with the letter `m_` (or `m` followed by PascalCase, e.g., `m_arena`, `m_cursor`, `mTokens`).
> 2. **Constructors and Destructors in the MIDDLE**: Placement after member variables, before any member functions.
> 3. **Member functions AT THE LAST / BOTTOM**: All helper methods, accessors, visitors, and interface implementations must reside at the bottom.
> 
> *Note: This layout rule applies strictly to the compiler implementation in C++, NOT to user-written code in the Fool language.*

Example of mandatory C++ layout:
```cpp
class Parser {
    // 1. Member variables FIRST with 'm_' prefix
    std::vector<Token> m_tokens;
    size_t m_cursor{0};
    Arena& m_arena;
    DiagnosticEngine& m_diag;

public:
    // 2. Constructors and Destructors in the MIDDLE
    Parser(std::vector<Token> tokens, Arena& arena, DiagnosticEngine& diag)
        : m_tokens(std::move(tokens)), m_cursor(0), m_arena(arena), m_diag(diag) {}
    ~Parser() = default;

    // 3. Member functions LAST
    ModuleAST* parse_module();
    ExprNode* parse_expression(int min_precedence);
    DeclNode* parse_declaration();
};

```

---

### SECTION 1: LANGUAGE SPECIFICATION ("Fool")

Provide a detailed specification covering the semantics, grammar, and runtime characteristics of Fool:

1. **Headerless Syntax & Dependency Architecture**:

* Total elimination of `#include` and the C preprocessor.

* Multi-pass, out-of-order symbol resolution without forward declarations.

* Parameterized, hermetic module blueprints: `import Math<SIMD = true>;` and `import IO<Buffering = 4096>;`.

* Cryptographically hashed `.flbmi` (Binary Module Interface) cache system.

1. **The 3-Tier File Triad (`.fo`, `.fl`, `.fex`)**:

* `.fo`: Interface blueprints, public classes, `ruleset` capability bounds, and signatures (no private bodies).
* `.fl`: Core logic, method implementations, algorithmic routines, and async state machine steps.
* `.fex`: External bindings (`@extern("C")`, `@extern("C++")`), assembly shims, and target syscalls.

1. **Core Language Features**:

* **Explicit Object Parameters (`this`)**: Eliminates the hidden `this` pointer. Supports `this self` (by-value/move), `this const& self` (immutable borrow), and `this mut& self` (mutable borrow).

* **Named & Default Arguments Everywhere**: Eliminate the need for signature permutation aliases by supporting labels and defaults in the middle or end of signatures: `fn render(x: f32, y: f32, scale: f32 = 1.0, width: f32, height: f32)`.
* **Compile-Time Directives (`@`)**: Strictly non-operator. Reserved for `@target(...)`, `@inline(...)`, `@align_as(...)`, and `@extern(...)`.

* **Rulesets**: Nominal capability bounds (equivalent to C++20 concepts / Rust traits) for generic constraints: `ruleset Numeric<T>`, `ruleset Equatable<T>`, `ruleset Lockable<T>`.
* **Async/Await State Machines**: Stackless poll-based futures lowered into structural state-machine enums/structs without mandatory heap allocation.

* **Opt-In Borrow Checker**: Destructive moves by default, with an optional static lifetime and aliasing verification pass.

1. **Standard Library Design (`stdlib/`)**:

* Plan out `stdlib/core/`, `stdlib/io/`, `stdlib/sync/`, `stdlib/async/`, `stdlib/net/`, `stdlib/math/`, and `stdlib/sys/` using the `.fo` / `.fl` / `.fex` file triad.
* Platform switching via `ModuleGraph` file routing (`io_linux.fl` vs `io_windows.fl` vs `io_wasm.fl`) and Pass 0 `@target` pruning.

---

### SECTION 2: COMPILER ARCHITECTURE ("foolc")

Architect the compiler frontend and driver targeting Jai-class compilation speeds:

1. **Frontend Infrastructure (DOD & Arena Allocation)**:

* Contiguous linear bump `Arena` allocators for $O(1)$ AST allocation and instant teardown.
* 32-bit `SymbolID` string interning for single-cycle integer identifier comparison.
* Pratt top-down operator precedence expression parsing with named/default argument slot binding.

1. **Multi-Pass Semantic Analysis Pipeline**:

* **Pass 0 (Symbol Collection & Target Pruning)**: Discovers symbols out-of-order and completely prunes non-matching `@target(os=..., arch=...)` declarations before type checking.

* **Pass 1 (Type Checking & Named Argument Resolution)**: Evaluates expressions, validates types, orders named arguments into canonical ABI slots, and fills defaults.
* **Pass 2 (Borrow Checking - Opt-in)**: Verifies region lifetimes and Aliasing XOR Mutability.

1. **Autonomous Driver & Build Tool**:

* In-process dependency DAG resolution without requiring Make or CMake for Fool projects.

* Deterministic cache salting: $\text{Hash} = \text{SHA256}(\text{BaseAST} \parallel \text{TemplateArgs} \parallel \text{TargetTriple})$.

* Dependency Placeholder / Patch Overlay system (`.fool/patches/<dep>`) via a layered Virtual Filesystem (VFS) to modify dependencies non-destructively.

1. **Backend Lowering Strategy**:

* Decoupled High-Level Intermediate Representation (HIR).

* Fast Native Machine Emitter for millisecond debug iteration.
* Production LLVM backend (supporting native $x86\_64$, AArch64, and WebAssembly `wasm32-wasi` / `wasm32-unknown`).

* Future expansion hooks for GCC (`libgccjit`) and MLIR dialects.

---

### DELIVERABLES EXPECTED

1. **Directory Tree**: A comprehensive file structure for the repository (including CMake 4.0+, C++20 module interfaces `.ixx` in `src/`, stdlib layout, and test suite).
2. **Phase-by-Phase Execution Plan**: A chronological roadmap detailing milestones from bootstrap lexer/parser up to self-hosting.
3. **Core C++ Interfaces**: Skeleton C++ class definitions for `Arena`, `StringInterner`, `Lexer`, `Parser`, `SymbolTablePass0`, and `ModuleGraph` strictly adhering to the **member variables on top (`m_` prefix) -> constructor/destructor in middle -> member functions at bottom** invariant.
4. **Sample Fool Source Files**: Demonstrations of `.fo`, `.fl`, and `.fex` files interoperating seamlessly.

```

***

### Key Highlights Built Into This Prompt:
* **Enforces the Layout Invariant:** Specifically mandates that all C++ classes place `m_` variables at the top, constructors/destructors in the middle, and member methods at the bottom.
* **Integrates All Language Features:** Covers the `.fo` / `.fl` / `.fex` file triad, parameterized modules[cite: 1], explicit `this`[cite: 1], `@target` pruning[cite: 1, 2], `ruleset` contracts, named/default argument resolution, and dependency patch overlays.
* **Specifies Compilation Performance:** Directs the agent to adopt memory arenas, string interning, and decoupled HIR to rival Jai's compilation throughput.

```
