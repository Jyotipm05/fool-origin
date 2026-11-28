//
// LLVM backend: MIR -> textual LLVM IR.
//

/**
 * @file LLVMCodeGen.ixx
 * @brief MIR v1 to LLVM IR text (`.ll`), which `clang` turns into an object file or an executable.
 * @details The backend writes IR as text on purpose. It needs no LLVM libraries to build `foolc` (only a `clang` to run the
 *          result), it works the same on every host, its output can be diffed in golden tests, and a compiler written in Fool
 *          can produce it with plain string building. A native emitter can later be a second backend over the same MIR.
 *
 *          Every MIR local becomes an `alloca` in the entry block and every read and write a `load` / `store`; `clang -O1`
 *          (`mem2reg`) turns those back into registers. Integer arithmetic wraps and division by zero is undefined, as in C.
 *
 *          What is emitted today: functions over integers, floats, bools and pointers/references to them; locals, arithmetic,
 *          comparisons, conversions and casts between those; `if` / `while` / `for` control flow; calls to Fool functions of
 *          the module and to `@extern("C")` functions; `*p`, `p[i]` and `&x`. A function that uses anything else (classes,
 *          enums, optionals, strings, globals, function values, calls into other modules) is *not emitted*: it is listed with
 *          the reason, and a build refuses to link a module that has any.
 */

module;

#include <string>
#include <vector>

export module fool.llvm;

import fool.common;
import fool.mir;

export namespace fool::llvm {

struct EmitOptions {
    // 1. Member variables FIRST
    std::string m_module_name{"main"};    ///< prefix of every Fool symbol (`<module>.<function>`) when a declaration names no module
    std::string m_target_triple;          ///< written as `target triple` when not empty; otherwise clang uses the host's
};

/// A function the backend could not emit, and why.
struct SkippedFunction {
    // 1. Member variables FIRST
    std::string m_name;
    std::string m_reason;
};

struct EmitResult {
    // 1. Member variables FIRST
    std::string m_ir;                         ///< the whole `.ll` file
    std::vector<SkippedFunction> m_skipped;   ///< functions MIR did not lower, or the backend does not support yet
    bool m_has_main{false};                   ///< a `fn main` was emitted, together with the C entry point that calls it
    size_t m_emitted{0};                      ///< how many Fool functions are in the IR
    std::vector<std::string> m_problems;      ///< inconsistencies (an extern declared twice with different types, an unsupported entry type)
};

/// Emits `module` as LLVM IR text.
[[nodiscard]] EmitResult emit(const mir::Module& module, const EmitOptions& options);

}  // namespace fool::llvm
