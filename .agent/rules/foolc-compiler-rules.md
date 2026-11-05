# `foolc` Compiler Implementation Rules

This rule defines engineering constraints, structural invariants, and safety guidelines for the C++ implementation of the `foolc` compiler.

## 1. Strict C++ Class & Struct Layout Invariant
Every `class` and `struct` in `src/` must adhere to this exact structural ordering:
```cpp
class ExampleSubsystem {
    // 1. Member variables FIRST on top with 'm_' prefix
    Arena& m_arena;
    DiagnosticEngine& m_diag;
    size_t m_state{0};

public:
    // 2. Constructors and Destructors in the MIDDLE
    ExampleSubsystem(Arena& arena, DiagnosticEngine& diag)
        : m_arena(arena), m_diag(diag) {}
    ~ExampleSubsystem() = default;

    // 3. Member functions LAST at the bottom
    void execute();
    [[nodiscard]] size_t get_state() const;
};
```
- In plain `struct` data types (e.g. AST nodes), fields are declared first, followed by constructors, followed by helper methods.

## 2. C++20 / C++23 Modules Conventions
- All subsystems are modularized: `export module fool.<subsystem>;`.
- Global module fragments are used for headers:
  ```cpp
  module;
  #include <cstdint>
  #include <vector>
  export module fool.ast;
  ```
- **Include Order Invariant**: In non-module translation units (`src/main.cpp`), standard `#include <...>` headers **must strictly precede** all `import fool.<subsystem>;` declarations to prevent MSVC IFC redefinition collisions.

## 3. Memory & Performance Discipline
- AST nodes and intermediate compilation structures must be allocated via the linear bump `fool::Arena`. Never allocate AST nodes with raw `new` or `std::make_unique`.
- Strings, tokens, and identifiers must be interned into 32-bit `fool::SymbolID` via `fool::StringInterner::get()` for $O(1)$ equality checks.

## 4. Parser Progress Guarantee
- Pratt and recursive descent parsers must guarantee monotonic cursor advancement during error recovery.
- In `Parser::consume(TokenKind k, std::string_view err_msg)`, never enter an infinite loop when an expected token is missing. Always synchronize or advance.

## 5. Automated Build Tool Constraint
- **NEVER** run `cmake`, `ctest`, or any build commands without the user's explicit permission.

