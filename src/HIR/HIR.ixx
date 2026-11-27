//
// High-level intermediate representation (HIR).
//

/**
 * @file HIR.ixx
 * @brief HIR v1: typed, name-resolved, desugared function bodies lowered from the checked AST.
 *
 * What the HIR fixes that the AST leaves open:
 *   - every expression has one concrete type (literals take the type of their context, `i32` / `f64` otherwise)
 *   - every name is resolved: a local (by index), a global, or a callee
 *   - every conversion is explicit: widening, `T` -> `T?`, `T?` -> `T` after narrowing, `&T` <-> `*T`, casts
 *   - every call lists its arguments in the callee's parameter order, defaults filled in, receiver first
 *   - member access goes through explicit dereferences and field indices
 *   - compile-time `@if` statements are gone: only the selected branch remains
 *   - an enum value is an EnumLit (`Mode::Read`, `Shape::Circle(2.0)`); equality is only defined on enums without payloads
 *   - `for (init; condition; step)` is a Block holding the initializer and a For, so the loop variable has a scope of its own
 *
 * Generics are monomorphized: a generic function or class is lowered once per set of type arguments that a call or
 * a type mentions (`identity<i32>`, `Box<i64>::make`), starting from the non-generic functions; the generic
 * declaration itself is kept as a template entry. A function that uses something HIR cannot express (lambdas,
 * async, reflection, generic enums, ...) is kept as "not lowerable yet" with the reason and the
 * line, so a later stage knows exactly what is missing instead of silently skipping it.
 */

module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

export module fool.hir;

import fool.common;
import fool.lexer;
import fool.ast;
import fool.sema;

export namespace fool::hir {

using TypeId = uint32_t;
using LocalId = uint32_t;
inline constexpr TypeId NO_TYPE = static_cast<TypeId>(-1);
inline constexpr LocalId NO_LOCAL = static_cast<LocalId>(-1);

enum class TypeKind : uint8_t { Void, Never, Bool, Int, Float, Pointer, Reference, Named, Optional, Function };

/// One structural type. Types are interned in a TypeTable, so equal types have equal ids.
struct Type {
    // 1. Member variables
    TypeKind m_kind{TypeKind::Void};
    unsigned m_bits{0};                  ///< Int / Float width
    bool m_signed{false};                ///< Int
    bool m_const{false};                 ///< Pointer / Reference: the pointee is read-only
    SymbolID m_name{INVALID_SYMBOL_ID};  ///< Int / Float / Named: the spelled name (`usize` and `u64` stay distinct)
    TypeId m_inner{NO_TYPE};             ///< pointee, referent, wrapped type, or the result of a Function
    std::vector<TypeId> m_params;        ///< Function parameters

    // 2. Member functions
    [[nodiscard]] bool operator==(const Type&) const = default;
};

class TypeTable {
    // 1. Member variables
    std::vector<Type> m_types;

public:
    // 2. Member functions
    [[nodiscard]] TypeId intern(Type type);
    [[nodiscard]] const Type& get(TypeId id) const { return m_types[id]; }
    [[nodiscard]] size_t size() const { return m_types.size(); }
    /// Source-like spelling: `i32`, `*u8`, `const& Buffer`, `Player?`, `fn(i32) -> bool`.
    [[nodiscard]] std::string text(TypeId id) const;
};

enum class ExprKind : uint8_t {
    IntLit,     ///< m_int
    FloatLit,   ///< m_float
    BoolLit,    ///< m_int (0 / 1)
    StrLit,     ///< m_text: the literal including its quotes
    NullLit,    ///< the empty optional; m_type is the optional type
    Local,      ///< m_local
    Global,     ///< m_name
    Unary,      ///< m_op (Minus, Bang) ; operands[0]
    Binary,     ///< m_op ; operands[0], operands[1]; arithmetic keeps the operand type, comparisons and `&&` `||` are bool
    Assign,     ///< operands[0] = place, operands[1] = value; type void
    Convert,    ///< implicit conversion of operands[0] to m_type (widening, retagging, `&T` <-> `*T`, const added)
    Cast,       ///< explicit cast, m_cast ; operands[0]
    Field,      ///< operands[0] is a value of the class (never a view); m_index in declaration order, m_name
    Index,      ///< operands[0][operands[1]]
    Call,       ///< m_callee / m_text; operands in parameter order, the receiver first for methods
    AddrOf,     ///< `&operands[0]`
    Deref,      ///< `*operands[0]`
    Unwrap,     ///< `T?` -> `T`; the checker proved the optional holds a value
    Wrap,       ///< `T` -> `T?`
    StructLit,  ///< operands in field declaration order
    EnumLit     ///< a variant of an enum: m_type is the enum, m_index the variant, m_text `Enum::Variant`; operands are the payload in order
};

struct Expr {
    // 1. Member variables
    ExprKind m_kind{ExprKind::IntLit};
    TypeId m_type{NO_TYPE};
    SourceSpan m_span;
    int64_t m_int{0};
    double m_float{0.0};
    std::string m_text;                         ///< StrLit text, Call display name (`f`, `Type::method`)
    LocalId m_local{NO_LOCAL};
    SymbolID m_name{INVALID_SYMBOL_ID};         ///< Global / Field name
    TokenKind m_op{TokenKind::Invalid};
    CastKind m_cast{CastKind::Unchecked};
    size_t m_index{0};
    const FunctionDeclNode* m_callee{nullptr};
    std::vector<std::unique_ptr<Expr>> m_operands;
};

enum class StmtKind : uint8_t {
    Let,        ///< declares m_local, initialized by m_expr when present
    Expr,       ///< m_expr evaluated for its effects
    Return,     ///< m_expr, or none for `return;`
    If,         ///< condition m_expr, m_body, m_else (an `else if` chain is one nested If in m_else)
    While,      ///< condition m_expr, m_body
    For,        ///< condition m_expr (none: loop until `break`), m_step runs after every iteration, also after `continue`; m_body
    Break,
    Continue,
    Block,      ///< m_body, a scope of its own
    Match       ///< subject m_expr (an enum value); m_arms, each with its own scope
};

struct Stmt;

/// One arm of a Match (HIR): the variant it handles (or everything else, for `_`) and the locals bound to the payload.
struct MatchCase {
    // 1. Member variables
    bool m_wildcard{false};
    size_t m_variant{0};                 ///< index into the enum's variants
    std::vector<LocalId> m_bindings;     ///< one per payload value (NO_LOCAL for `_`); empty when the pattern named no payload
    std::vector<Stmt> m_body;
    SourceSpan m_span;
};

struct Stmt {
    // 1. Member variables
    StmtKind m_kind{StmtKind::Expr};
    SourceSpan m_span;
    LocalId m_local{NO_LOCAL};
    std::unique_ptr<Expr> m_expr;
    std::unique_ptr<Expr> m_step;     ///< For: evaluated after the body and after `continue`; may be absent
    std::vector<Stmt> m_body;
    std::vector<Stmt> m_else;
    std::vector<MatchCase> m_arms;    ///< Match
};

struct Local {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    TypeId m_type{NO_TYPE};
    bool m_mutable{true};     ///< false for `const` / `constx` bindings
    bool m_is_param{false};   ///< the receiver and the parameters come first, in order
};

/// A generic class instantiated with type arguments (`Box<i32>`): a Named type whose name is the instance's spelling.
struct ClassInstance {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};          ///< `Box<i32>`
    const ClassDeclNode* m_decl{nullptr};
    std::vector<TypeId> m_args;                  ///< one per type parameter of m_decl, in order
};

/// One variant of an enum and the types of its payload (empty for `Mode::Read`).
struct EnumVariantInfo {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<TypeId> m_payload;
};

/// A (non-generic) enum that some lowered code mentions: a Named type whose variants are listed in declaration order.
struct EnumInfo {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    const EnumDeclNode* m_decl{nullptr};
    std::vector<EnumVariantInfo> m_variants;

    // 2. Member functions
    /// True when no variant carries a payload: the values are plain tags and can be compared with `==` / `!=`.
    [[nodiscard]] bool is_plain() const {
        for (const EnumVariantInfo& variant : m_variants) {
            if (!variant.m_payload.empty()) return false;
        }
        return true;
    }
};

struct Function {
    // 1. Member variables
    std::string m_name;                          ///< `f`, `Type::method`, or an instance such as `identity<i32>` / `Box<i32>::get`
    bool m_is_template{false};                   ///< a generic function (or method of a generic type): lowered once per instantiation, not itself
    const FunctionDeclNode* m_decl{nullptr};
    std::vector<Local> m_locals;                 ///< parameters first (receiver, then parameters), then every `let`
    size_t m_param_count{0};
    TypeId m_return{NO_TYPE};
    std::vector<Stmt> m_body;
    bool m_lowered{false};
    std::string m_reason;                        ///< why it is not lowered (when !m_lowered)
    SourceSpan m_reason_span;
};

struct Module {
    // 1. Member variables
    TypeTable m_types;
    std::vector<Function> m_functions;         ///< the non-generic functions in declaration order, then the instantiations in the order they were requested
    std::vector<ClassInstance> m_instances;
    std::vector<EnumInfo> m_enums;             ///< the enums that the lowered code mentions, in order of first use

    // 2. Member functions
    [[nodiscard]] size_t lowered_count() const;
    [[nodiscard]] size_t template_count() const;
    [[nodiscard]] const ClassInstance* find_instance(SymbolID name) const;
    [[nodiscard]] const EnumInfo* find_enum(SymbolID name) const;
};

/// Lowers every function with a body that the checker accepted. Call only after `checker.check_module()` succeeded.
[[nodiscard]] Module lower_module(const TypeCheckerPass2& checker, const SymbolTablePass0& pass0);

/// Text form of the HIR, one function after another (what `--dump-hir` prints).
[[nodiscard]] std::string dump(const Module& module);

} // namespace fool::hir
