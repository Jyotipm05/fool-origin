# Fool Language Support for Visual Studio Code

Official syntax highlighting, code snippets, and IntelliSense configuration for the **Fool** systems programming language (`.fo`, `.fl`, `.fex`).

## Features

- **File Triad Recognition**:
  - `.fo`: Interface Blueprints
  - `.fl`: Implementation Units
  - `.fex`: Platform Overlays & Foreign ABI Bindings
- **Accurate Syntax Highlighting**:
  - Module imports: `#import <module_path>;`
  - Compile-time constants: `constx`
  - Strict immutability bindings: `const`
  - Mutable state: `=` and `var`
  - Explicit receivers: `this &self` (mutable borrow), `this const& self` (immutable borrow), `this self` (consuming move)
  - Raw pointers: `*T`, `const* T`
  - References: `&T`, `const& T`
  - Nominal capabilities: `ruleset`
  - Monomorphized templates: `blueprint`
  - Directives: `@target(...)`, `@extern(...)`, `@inline(...)`, `@align_as(...)`
- **Productivity Snippets**:
  - `fn`, `asyncfn`, `method_mut`, `method_const`, `method_move`
  - `ruleset`, `blueprint`, `class`
  - `target`, `extern`, `constx`

## Installation

Install directly from the generated `.vsix` file:
```bash
code --install-extension fool-vscode-0.1.0.vsix
```
Or via the Extensions view: **... (Views and More Actions) -> Install from VSIX...**

