//
// Mid-level intermediate representation (MIR).
//

/**
 * @file MIR.ixx
 * @brief MIR v1: a control-flow graph of basic blocks over typed locals, built from HIR.
 *
 * What MIR fixes that HIR leaves open:
 *   - control flow is explicit: `if`, `while`, `for`, `break`, `continue`, `&&` and `||` are blocks and jumps
 *   - evaluation order is explicit: every compound expression is a sequence of simple statements writing temporaries
 *   - every statement does one thing: `place = rvalue`, or a call whose result is discarded
 *   - memory access is explicit: a place is a local (or global) plus a chain of projections (`*p`, `.field`, `[i]`)
 *
 * A function is a vector of blocks; block 0 is the entry. A block is straight-line statements followed by exactly one
 * terminator (goto, branch, return, unreachable). There are no phi nodes: locals are mutable slots, and a later SSA
 * or register-allocation stage can build on that. Calls are ordinary rvalues (there is no unwinding yet).
 *
 * MIR shares the HIR type table, so a type id means the same thing in both. Functions HIR could not lower, and generic
 * templates (only their instantiations exist), are not lowered again: MIR lists them as not lowered with the reason.
 */

module;

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

export module fool.mir;

import fool.common;
import fool.lexer;
import fool.ast;
import fool.hir;

export namespace fool::mir {

using hir::LocalId;
using hir::NO_LOCAL;
using hir::NO_TYPE;
using hir::TypeId;

using BlockId = uint32_t;
inline constexpr BlockId NO_BLOCK = static_cast<BlockId>(-1);

enum class ProjectionKind : uint8_t {
    Deref,   ///< `*place`: the place holds a pointer or reference; the result is what it points to
    Field,   ///< `place.name`: m_index in declaration order; the place holds a class value
    Index    ///< `place[local]`: the place holds a pointer; the result is the element m_local steps away
};

struct Projection {
    // 1. Member variables
    ProjectionKind m_kind{ProjectionKind::Deref};
    TypeId m_type{NO_TYPE};                  ///< the type after applying this projection
    size_t m_index{0};                       ///< Field
    SymbolID m_name{INVALID_SYMBOL_ID};      ///< Field
    LocalId m_local{NO_LOCAL};               ///< Index: a local holding the index
};

/// Somewhere a value can be read from or written to: a local or a global, and a chain of projections.
struct Place {
    // 1. Member variables
    LocalId m_local{NO_LOCAL};               ///< the base, unless it is a global
    SymbolID m_global{INVALID_SYMBOL_ID};    ///< the base when it is a global (m_local is NO_LOCAL)
    TypeId m_base_type{NO_TYPE};
    std::vector<Projection> m_projections;

    // 2. Member functions
    [[nodiscard]] TypeId type() const { return m_projections.empty() ? m_base_type : m_projections.back().m_type; }
    [[nodiscard]] bool is_global() const { return m_local == NO_LOCAL; }
};

enum class OperandKind : uint8_t {
    Unit,    ///< the value of a call to a function that returns nothing; never stored
    Int,     ///< m_int
    Float,   ///< m_float
    Bool,    ///< m_int (0 / 1)
    Str,     ///< m_text: the literal including its quotes
    Null,    ///< the empty optional of type m_type
    Read     ///< the current value of m_place
};

struct Operand {
    // 1. Member variables
    OperandKind m_kind{OperandKind::Unit};
    TypeId m_type{NO_TYPE};
    int64_t m_int{0};
    double m_float{0.0};
    std::string m_text;
    Place m_place;
};

enum class RvalueKind : uint8_t {
    Use,        ///< operands[0]
    Unary,      ///< m_op (Minus, Bang) ; operands[0]
    Binary,     ///< m_op ; operands[0], operands[1] (never `&&` / `||`: those became branches)
    Convert,    ///< implicit conversion of operands[0] to the destination type
    Cast,       ///< explicit cast, m_cast ; operands[0]
    AddrOf,     ///< `&m_place`
    Unwrap,     ///< `T?` -> `T`
    Wrap,       ///< `T` -> `T?`
    Aggregate,  ///< a class value; operands in field declaration order
    Variant,    ///< an enum value: m_index is the variant, m_text `Enum::Variant`; operands are the payload
    Call        ///< m_text callee display name, m_callee when known; operands in parameter order, receiver first
};

struct Rvalue {
    // 1. Member variables
    RvalueKind m_kind{RvalueKind::Use};
    TypeId m_type{NO_TYPE};
    TokenKind m_op{TokenKind::Invalid};
    CastKind m_cast{CastKind::Unchecked};
    size_t m_index{0};
    std::string m_text;
    const FunctionDeclNode* m_callee{nullptr};
    Place m_place;                       ///< AddrOf
    std::vector<Operand> m_operands;
};

enum class StatementKind : uint8_t {
    Assign,   ///< m_place = m_value
    Eval      ///< m_value evaluated and its result discarded (a call that returns nothing, or whose result is unused)
};

struct Statement {
    // 1. Member variables
    StatementKind m_kind{StatementKind::Assign};
    SourceSpan m_span;
    Place m_place;
    Rvalue m_value;
};

enum class TerminatorKind : uint8_t {
    Goto,          ///< m_target
    Branch,        ///< m_value is a bool: true -> m_target, false -> m_else
    Return,        ///< m_value when m_has_value
    Unreachable    ///< control cannot get here (the end of a function whose every path returned)
};

struct Terminator {
    // 1. Member variables
    TerminatorKind m_kind{TerminatorKind::Unreachable};
    SourceSpan m_span;
    Operand m_value;
    bool m_has_value{false};
    BlockId m_target{NO_BLOCK};
    BlockId m_else{NO_BLOCK};
};

struct Block {
    // 1. Member variables
    std::vector<Statement> m_statements;
    Terminator m_terminator;
    bool m_terminated{false};
};

struct Local {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};   ///< INVALID for a temporary
    TypeId m_type{NO_TYPE};
    bool m_mutable{true};
    bool m_is_param{false};
};

struct Function {
    // 1. Member variables
    std::string m_name;
    const FunctionDeclNode* m_decl{nullptr};
    std::vector<Local> m_locals;          ///< the HIR locals with the same indices (parameters first), then temporaries
    size_t m_param_count{0};
    TypeId m_return{NO_TYPE};
    std::vector<Block> m_blocks;          ///< block 0 is the entry
    bool m_lowered{false};
    std::string m_reason;                 ///< why it is not lowered (when !m_lowered)
    SourceSpan m_reason_span;
};

struct Module {
    // 1. Member variables
    hir::TypeTable m_types;               ///< a copy of the HIR table: type ids are the same
    std::vector<hir::EnumInfo> m_enums;
    std::vector<Function> m_functions;    ///< the lowered and not-lowerable functions of the HIR module; generic templates are omitted
    size_t m_templates{0};                ///< how many generic templates the HIR module had

    // 2. Member functions
    [[nodiscard]] size_t lowered_count() const;
};

/// Builds the control-flow graph of every function HIR lowered.
[[nodiscard]] Module lower(const hir::Module& module);

/// Structural and type checks on a built module (every block ends in a terminator, jumps and locals exist, branch
/// conditions are bool, assigned values have their place's type, returns match the function). Empty when it is sound.
[[nodiscard]] std::vector<std::string> verify(const Module& module);

/// Feeds the verifier hand-built broken graphs and checks that each defect is reported (and a sound graph is not).
/// Returns a description of every expectation that failed; empty when the verifier behaves.
[[nodiscard]] std::vector<std::string> selftest();

/// Text form (what `--dump-mir` prints).
[[nodiscard]] std::string dump(const Module& module);

} // namespace fool::mir
