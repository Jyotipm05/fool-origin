//
// Created by jmbab on 05-11-2026.
//

/**
 * @file AST.ixx
 * @brief Arena-allocated Abstract Syntax Tree (AST) definitions for Fool.
 */

module;

#include <vector>
#include <string>
#include <string_view>
#include <cstdint>
#include <optional>
#include <utility>

export module fool.ast;

import fool.common;
import fool.lexer;

export namespace fool {

/**
 * @enum ReceiverKind
 * @brief Specifies the parameter qualification of an explicit 'this' receiver method.
 */
enum class ReceiverKind {
    None,       ///< Free function
    ByValue,    ///< Consuming move receiver: 'this self'
    ByConstRef, ///< Read-only borrowed receiver: 'this const& self'
    ByMutRef    ///< Mutable borrowed receiver: 'this &self' (default)
};

/**
 * @enum BindingKind
 * @brief Variable declaration and binding semantics in Fool.
 */
enum class BindingKind {
    Mutable,   ///< Declared with '=' (and 'var'): mutable variable
    Immutable, ///< Declared with 'const': Rust-like immutable binding
    Constx     ///< Declared with 'constx': C++ constexpr / Rust const compile-time constant
};

/**
 * @struct AttributeArg
 * @brief Key-value pair in an @target, @inline, or @extern directive.
 *        Positional arguments (e.g. the "C" in @extern("C", ...)) carry an invalid key.
 */
struct AttributeArg {
    // 1. Member variables
    SymbolID m_key{INVALID_SYMBOL_ID};
    std::string_view m_value; ///< Unquoted value text (string quotes are stripped)

    // 2. Constructors
    AttributeArg() = default;
    AttributeArg(SymbolID key, std::string_view value)
        : m_key(key), m_value(value) {}
};

/**
 * @class AttributeNode
 * @brief Compile-time directive node attached to declarations.
 */
class AttributeNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<AttributeArg> m_args;
    SourceSpan m_span;

    // 2. Constructors
    AttributeNode() = default;
    AttributeNode(SymbolID name, std::vector<AttributeArg> args, SourceSpan span)
        : m_name(name), m_args(std::move(args)), m_span(span) {}
};

/**
 * @class ASTNode
 * @brief Base AST node containing source location coordinates.
 * @details Nodes are arena allocated; Arena::create registers destructors of nodes that own
 *          heap memory (std::vector members), so they are released with the arena.
 */
class ASTNode {
public:
    // 1. Member variables
    SourceSpan m_span;

    // 2. Constructors & Destructor
    ASTNode() = default;
    virtual ~ASTNode() = default;
};

// ---------------------------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------------------------

class TypeNode;
class ExprNode;

/**
 * @struct GenericArg
 * @brief One argument inside '<...>': a type (Atomic<u32>) or a named constant (Math<SIMD = true>).
 */
struct GenericArg {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID}; ///< Set for 'NAME = value' arguments
    TypeNode* m_type{nullptr};          ///< Set for type arguments
    ExprNode* m_value{nullptr};         ///< Set for 'NAME = value' arguments
};

/**
 * @struct GenericParam
 * @brief One parameter inside a declaration's '<...>': 'T', 'T: Bound' or 'SIMD: bool = false'.
 */
struct GenericParam {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    TypeNode* m_constraint{nullptr};    ///< Ruleset bound or constant type
    ExprNode* m_default{nullptr};
};

enum class TypeNodeKind : uint8_t {
    Path,      ///< u32, std::core::Span<T>
    Pointer,   ///< *T / const* T
    Reference, ///< &T / const& T
    Never      ///< !
};

/**
 * @class TypeNode
 * @brief Parsed type expression.
 */
class TypeNode : public ASTNode {
public:
    // 1. Member variables
    TypeNodeKind m_kind{TypeNodeKind::Path};
    bool m_is_const{false};              ///< 'const*' / 'const&'
    std::vector<SymbolID> m_path;        ///< Path segments (Path only)
    std::vector<GenericArg> m_generic_args;
    TypeNode* m_inner{nullptr};          ///< Pointee / referent (Pointer, Reference)

    // 2. Constructors
    TypeNode() = default;
    explicit TypeNode(TypeNodeKind kind) : m_kind(kind) {}

    // 3. Member functions
    [[nodiscard]] bool is_const_view() const {
        return (m_kind == TypeNodeKind::Pointer || m_kind == TypeNodeKind::Reference) && m_is_const;
    }
    [[nodiscard]] bool is_view() const {
        return m_kind == TypeNodeKind::Pointer || m_kind == TypeNodeKind::Reference;
    }
};

// ---------------------------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------------------------

class ExprNode : public ASTNode {};

class IntegerLiteralExpr : public ExprNode {
public:
    // 1. Member variables
    int64_t m_value{0};

    // 2. Constructors
    explicit IntegerLiteralExpr(int64_t val) : m_value(val) {}
};

class FloatLiteralExpr : public ExprNode {
public:
    // 1. Member variables
    double m_value{0.0};

    // 2. Constructors
    explicit FloatLiteralExpr(double val) : m_value(val) {}
};

class StringLiteralExpr : public ExprNode {
public:
    // 1. Member variables
    std::string_view m_value; ///< Interned text including the surrounding quotes

    // 2. Constructors
    explicit StringLiteralExpr(std::string_view val) : m_value(val) {}
};

class BooleanLiteralExpr : public ExprNode {
public:
    // 1. Member variables
    bool m_value{false};

    // 2. Constructors
    explicit BooleanLiteralExpr(bool val) : m_value(val) {}
};

class IdentifierExpr : public ExprNode {
public:
    // 1. Member variables
    SymbolID m_identifier{INVALID_SYMBOL_ID};

    // 2. Constructors
    explicit IdentifierExpr(SymbolID id) : m_identifier(id) {}
};

/**
 * @class PathExpr
 * @brief Qualified expression path: ffi::wait, MemoryOrder::SeqCst, Atomic<u32>::new.
 */
class PathExpr : public ExprNode {
public:
    // 1. Member variables
    std::vector<SymbolID> m_segments;
    std::vector<GenericArg> m_generic_args; ///< Generic arguments written on the first segment

    // 2. Constructors
    PathExpr() = default;
};

class UnaryExpr : public ExprNode {
public:
    // 1. Member variables
    TokenKind m_op{TokenKind::Invalid}; ///< Bang, Minus, Ampersand (address-of), Star (dereference)
    ExprNode* m_operand{nullptr};

    // 2. Constructors
    UnaryExpr(TokenKind op, ExprNode* operand) : m_op(op), m_operand(operand) {}
};

class BinaryExpr : public ExprNode {
public:
    // 1. Member variables
    TokenKind m_op{TokenKind::Invalid}; ///< Equal means assignment
    ExprNode* m_lhs{nullptr};
    ExprNode* m_rhs{nullptr};

    // 2. Constructors
    BinaryExpr(TokenKind op, ExprNode* l, ExprNode* r)
        : m_op(op), m_lhs(l), m_rhs(r) {}
};

class CastExpr : public ExprNode {
public:
    // 1. Member variables
    ExprNode* m_operand{nullptr};
    TypeNode* m_target{nullptr};

    // 2. Constructors
    CastExpr(ExprNode* operand, TypeNode* target) : m_operand(operand), m_target(target) {}
};

class MemberExpr : public ExprNode {
public:
    // 1. Member variables
    ExprNode* m_object{nullptr};
    SymbolID m_member{INVALID_SYMBOL_ID};

    // 2. Constructors
    MemberExpr(ExprNode* object, SymbolID member) : m_object(object), m_member(member) {}
};

class IndexExpr : public ExprNode {
public:
    // 1. Member variables
    ExprNode* m_base{nullptr};
    ExprNode* m_index{nullptr};

    // 2. Constructors
    IndexExpr(ExprNode* base, ExprNode* index) : m_base(base), m_index(index) {}
};

/**
 * @struct CallArg
 * @brief Argument at a call site, optionally labeled for named argument binding.
 */
struct CallArg {
    // 1. Member variables
    SymbolID m_label{INVALID_SYMBOL_ID}; ///< e.g. "width:" in draw_box(width: 100)
    ExprNode* m_expr{nullptr};
    SourceSpan m_span;

    // 2. Constructors
    CallArg() = default;
    CallArg(SymbolID label, ExprNode* expr) : m_label(label), m_expr(expr) {}
};

class CallExpr : public ExprNode {
public:
    // 1. Member variables
    ExprNode* m_callee{nullptr};
    std::vector<CallArg> m_args;

    // 2. Constructors
    CallExpr() = default;
};

/**
 * @class IntrinsicCallExpr
 * @brief Compile-time intrinsic invocation such as @size_of(T).
 */
class IntrinsicCallExpr : public ExprNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<ExprNode*> m_args;

    // 2. Constructors
    IntrinsicCallExpr() = default;
};

struct FieldInit {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    ExprNode* m_value{nullptr};
    SourceSpan m_span;
};

class StructLiteralExpr : public ExprNode {
public:
    // 1. Member variables
    TypeNode* m_type{nullptr};
    std::vector<FieldInit> m_fields;

    // 2. Constructors
    StructLiteralExpr() = default;
};

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

class DeclNode;

class StmtNode : public ASTNode {};

class ReturnStmt : public StmtNode {
public:
    // 1. Member variables
    ExprNode* m_value{nullptr};

    // 2. Constructors
    ReturnStmt() = default;
    explicit ReturnStmt(ExprNode* val) : m_value(val) {}
};

class ExprStmt : public StmtNode {
public:
    // 1. Member variables
    ExprNode* m_expr{nullptr};

    // 2. Constructors
    ExprStmt() = default;
    explicit ExprStmt(ExprNode* expr) : m_expr(expr) {}
};

class DeclStmt : public StmtNode {
public:
    // 1. Member variables
    DeclNode* m_decl{nullptr};

    // 2. Constructors
    DeclStmt() = default;
    explicit DeclStmt(DeclNode* decl) : m_decl(decl) {}
};

class BlockStmt : public StmtNode {
public:
    // 1. Member variables
    std::vector<StmtNode*> m_statements;

    // 2. Constructors
    BlockStmt() = default;
};

class IfStmt : public StmtNode {
public:
    // 1. Member variables
    ExprNode* m_condition{nullptr};
    BlockStmt* m_then{nullptr};
    StmtNode* m_else{nullptr}; ///< nullptr, a BlockStmt, or a chained IfStmt

    // 2. Constructors
    IfStmt() = default;
};

class WhileStmt : public StmtNode {
public:
    // 1. Member variables
    ExprNode* m_condition{nullptr};
    BlockStmt* m_body{nullptr};

    // 2. Constructors
    WhileStmt() = default;
};

class ForStmt : public StmtNode {
public:
    // 1. Member variables
    SymbolID m_variable{INVALID_SYMBOL_ID};
    ExprNode* m_iterable{nullptr};
    BlockStmt* m_body{nullptr};

    // 2. Constructors
    ForStmt() = default;
};

class BreakStmt : public StmtNode {};
class ContinueStmt : public StmtNode {};

/**
 * @struct ConditionalBranch
 * @brief Represents a branch in a compile-time @if, @elif, or @else directive inside a function.
 */
struct ConditionalBranch {
    // 1. Member variables
    ExprNode* m_condition{nullptr};  ///< nullptr for @else
    std::vector<StmtNode*> m_body;   ///< Statements inside this branch

    // 2. Constructors
    ConditionalBranch() = default;
    ConditionalBranch(ExprNode* cond, std::vector<StmtNode*> body)
        : m_condition(cond), m_body(std::move(body)) {}
};

/**
 * @class ConditionalStmt
 * @brief Compile-time conditional branch inside function bodies (@if (...) { ... } @elif (...) { ... } @else { ... }).
 * Zero runtime overhead; inactive branches are pruned before type checking.
 */
class ConditionalStmt : public StmtNode {
public:
    // 1. Member variables
    std::vector<ConditionalBranch> m_branches;

    // 2. Constructors
    ConditionalStmt() = default;
};

// ---------------------------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------------------------

/**
 * @class DeclNode
 * @brief Base class for declarations with attached attributes and target activation flags.
 */
class DeclNode : public ASTNode {
public:
    // 1. Member variables
    std::vector<AttributeNode> m_attributes;
    bool m_is_active{true};                 ///< Cleared during Pass 0 if @target / @if prunes it
    SymbolID m_module{INVALID_SYMBOL_ID};   ///< 'module' section this declaration appeared in
    std::string_view m_file;                ///< Interned path of the defining file (set by the driver / module graph)

    // 2. Constructors
    DeclNode() = default;
};

/**
 * @struct ConditionalDeclBranch
 * @brief Represents a branch in a top-level compile-time declaration block (@if (cond) { ... } @else { ... }).
 */
struct ConditionalDeclBranch {
    // 1. Member variables
    ExprNode* m_condition{nullptr}; ///< nullptr for @else
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    ConditionalDeclBranch() = default;
    ConditionalDeclBranch(ExprNode* cond, std::vector<DeclNode*> decls)
        : m_condition(cond), m_declarations(std::move(decls)) {}
};

/**
 * @class ConditionalDeclBlock
 * @brief Top-level compile-time conditional (@if cond decl  |  @if cond { decls } @elif ... @else { ... }).
 */
class ConditionalDeclBlock : public DeclNode {
public:
    // 1. Member variables
    std::vector<ConditionalDeclBranch> m_branches;

    // 2. Constructors
    ConditionalDeclBlock() = default;
};

/**
 * @struct ImportArg
 * @brief Compile-time template argument passed into a parameterized module import (#import mod<A = 10, B = true>;).
 */
struct ImportArg {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    ExprNode* m_value{nullptr};
    SourceSpan m_span;

    // 2. Constructors
    ImportArg() = default;
    ImportArg(SymbolID name, ExprNode* val) : m_name(name), m_value(val) {}
};

/**
 * @class ImportDeclNode
 * @brief Module import directive (#import std::core; or #import math::matrix<Rows = 4>;)
 */
class ImportDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_module_path{INVALID_SYMBOL_ID};
    std::vector<ImportArg> m_template_args;

    // 2. Constructors
    ImportDeclNode() = default;
    explicit ImportDeclNode(SymbolID path) : m_module_path(path) {}
    ImportDeclNode(SymbolID path, std::vector<ImportArg> args)
        : m_module_path(path), m_template_args(std::move(args)) {}
};

/**
 * @class VarDeclNode
 * @brief Represents a variable declaration, immutable binding, or compile-time constant.
 */
class VarDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    TypeNode* m_type{nullptr};
    ExprNode* m_initializer{nullptr};
    BindingKind m_kind{BindingKind::Mutable};

    // 2. Constructors
    VarDeclNode() = default;
    VarDeclNode(SymbolID name, TypeNode* type, ExprNode* init, BindingKind kind)
        : m_name(name), m_type(type), m_initializer(init), m_kind(kind) {}
};

struct ParameterDecl {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    TypeNode* m_type{nullptr};
    ExprNode* m_default_value{nullptr}; ///< Supports middle and trailing defaults
    SourceSpan m_span;

    // 2. Constructors
    ParameterDecl() = default;
    ParameterDecl(SymbolID name, TypeNode* type, ExprNode* default_val = nullptr)
        : m_name(name), m_type(type), m_default_value(default_val) {}
};

struct ReceiverDecl {
    // 1. Member variables
    ReceiverKind m_kind{ReceiverKind::None};
    SymbolID m_self_name{INVALID_SYMBOL_ID};

    // 2. Constructors
    ReceiverDecl() = default;
    ReceiverDecl(ReceiverKind kind, SymbolID self_name)
        : m_kind(kind), m_self_name(self_name) {}
};

class FunctionDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    SymbolID m_owner{INVALID_SYMBOL_ID};        ///< 'Owner' in `fn Owner::name(...)`
    std::vector<GenericArg> m_owner_args;       ///< `fn Owner<T>::name` / `fn Owner<SIMD = true>::name`
    std::vector<GenericParam> m_type_params;    ///< `fn name<T>(...)`
    bool m_is_async{false};                     ///< Lowers to a poll-based state machine
    bool m_has_body{false};                     ///< false for interface declarations ending in ';'
    ReceiverDecl m_receiver;
    std::vector<ParameterDecl> m_params;
    TypeNode* m_return_type{nullptr};           ///< nullptr means void
    std::vector<StmtNode*> m_body;

    // 2. Constructors
    FunctionDeclNode() = default;
};

class RulesetDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<GenericParam> m_type_params;
    std::vector<FunctionDeclNode*> m_methods;

    // 2. Constructors
    RulesetDeclNode() = default;
};

class ClassDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    bool m_is_struct{false};
    std::vector<GenericParam> m_type_params;
    std::vector<ParameterDecl> m_fields;
    std::vector<FunctionDeclNode*> m_methods;

    // 2. Constructors
    ClassDeclNode() = default;
};

class BlueprintDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<GenericParam> m_type_params;
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    BlueprintDeclNode() = default;
};

struct EnumVariant {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<TypeNode*> m_payload; ///< Ready(T) carries one payload type
    SourceSpan m_span;
};

class EnumDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<GenericParam> m_type_params;
    std::vector<EnumVariant> m_variants;

    // 2. Constructors
    EnumDeclNode() = default;
};

/**
 * @struct ModuleParam
 * @brief Template parameter in a parameterized module declaration (module name<A = 1, B = 0>;).
 */
struct ModuleParam {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    ExprNode* m_default_value{nullptr};

    // 2. Constructors
    ModuleParam() = default;
    ModuleParam(SymbolID name, ExprNode* def_val = nullptr)
        : m_name(name), m_default_value(def_val) {}
};

class ModuleAST : public ASTNode {
public:
    // 1. Member variables
    SymbolID m_module_name{INVALID_SYMBOL_ID};   ///< First 'module' line of the file
    std::vector<ModuleParam> m_parameters;
    std::vector<SymbolID> m_module_sections;     ///< Every module named in the file, in order
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    ModuleAST() = default;
};

} // namespace fool
