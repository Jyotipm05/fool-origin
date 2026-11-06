# Fool Language Support for CLion & JetBrains IDEs

This directory contains the official **TextMate Bundle** providing complete syntax highlighting, file association, and code comment support for the Fool programming language (`.fo`, `.fl`, `.fex`) in **CLion**, **IntelliJ IDEA**, **Rider**, and other JetBrains IDEs.

---

## Features

- **File Associations**: Automatic detection of `.fo` (Fool interface modules), `.fl` (Fool implementation modules), and `.fex` (Fool executable units).
- **Directives & Module Templates**: Highlighting for `#import`, `@if`, `@elif`, `@else`, `@target`, `@inline`, `@align_as`, and `def(...)`.
- **Receiver Method Syntax**: Specialized highlighting for `this const& self`, `this &self`, and `this self`.
- **Transitive Immutability**: Full highlighting for `const` (strict transitive immutability) and `constx` (compile-time constant expressions).
- **Comments & Toggle**: Automatic line comment (`//`) and block comment (`/* ... */`) support with CLion's `Ctrl+/` / `Cmd+/` shortcut.
- **Types & Operators**: Full coverage of integer (`i8`..`u64`), floating-point (`f32`, `f64`), standard types (`StringView`, `Span`, `Atomic`, etc.), custom structs/classes, and operators.

---

## Installation in CLion (1-Minute Setup)

CLion has native support for TextMate bundles built-in via the bundled TextMate plugin (enabled by default).

### Method 1: Import the Bundle Folder (Recommended)

1. Open CLion.
2. Open Settings:
   - **Windows / Linux**: `File` -> `Settings` (or press `Ctrl+Alt+S`)
   - **macOS**: `CLion` -> `Settings...` (or press `Cmd+,`)
3. In the left navigation pane, navigate to:
   **Editor** -> **TextMate Bundles**
4. Click the **`+` (Add)** icon at the top of the TextMate Bundles list.
5. Navigate to and select the `Fool.tmbundle` folder:
   ```
   <repo-root>/editors/clion/Fool.tmbundle
   ```
6. Click **Apply**, then **OK**.

All `.fo`, `.fl`, and `.fex` files will immediately display syntax highlighting!

---

### Method 2: Import the Zip Archive

If you prefer a single archive:
1. Open **Settings** -> **Editor** -> **TextMate Bundles**.
2. Click **`+` (Add)**.
3. Select `Fool.tmbundle.zip` (located in the repository root or in `editors/clion/`).
4. Click **Apply** -> **OK**.

---

## Verifying File Association

Once imported, CLion will list:
- **Bundle**: `Fool`
- **Grammar**: `Fool` (`source.fool`)
- **Extensions**: `*.fo`, `*.fl`, `*.fex`

If a file was previously opened as plain text before installing the bundle:
1. Right-click the file in the editor or project tree.
2. If needed, go to **Settings** -> **Editor** -> **File Types**.
3. Under **Recognized File Types**, ensure `TextMate` has registered `*.fo`, `*.fl`, and `*.fex` (or remove them from "Text" if previously registered as plain text).

---

## Rebuilding the Bundle Archive

To repackage the archive after modifying grammar rules:
```bash
python editors/clion/package_clion.py
```
This updates `editors/clion/Fool.tmbundle.zip` and the root `Fool.tmbundle.zip`.

