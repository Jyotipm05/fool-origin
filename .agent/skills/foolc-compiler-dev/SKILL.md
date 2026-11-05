---
name: foolc-compiler-dev
description: Runbook for developing, extending, and refactoring compiler frontend and semantic passes in the foolc C++ codebase.
---

# `foolc` Compiler Development Skill

This skill guides development inside the `src/` directory of the `foolc` compiler.

## 1. Class & Struct Layout Checklist
Before committing any C++ class or struct in `src/`, verify the layout invariant:
1. **Top**: Member variables prefixed with `m_`.
2. **Middle**: Constructors and destructors.
3. **Bottom**: Member methods and accessors.

Example:
```cpp
class TokenStream {
    // 1. Members
    std::vector<Token> m_tokens;
    size_t m_cursor{0};

public:
    // 2. Constructors
    explicit TokenStream(std::vector<Token> tokens) : m_tokens(std::move(tokens)) {}
    ~TokenStream() = default;

    // 3. Methods
    const Token& current() const { return m_tokens[m_cursor]; }
};
```

## 2. Adding / Modifying Language Features
1. **Lexer (`src/Lexer/`)**: Update `TokenKind` in `Lexer.ixx` and token matching in `Lexer.cpp`.
2. **AST (`src/AST/`)**: Define arena-allocated node in `AST.ixx`.
3. **Parser (`src/Parser/`)**: Add parsing method in `Parser.cpp`. Always guarantee progress (avoid infinite loops on unexpected tokens).
4. **Sema (`src/Sema/`)**:
   - Pass 0 (`SymbolTable.cpp`): Declaration discovery & `@target` pruning.
   - Pass 1: Symbol resolution & scoping.
   - Pass 2 (`TypeChecker.cpp`): Type checking, receiver validation, and `const` enforcement.
   - Opt-in Borrow Checker (`BorrowChecker.cpp`): Invoked only when `--check-borrow` is enabled.

## 3. Command Execution Policy
- Never run `cmake` or `ctest` without explicit user permission.

