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
 */
struct AttributeArg {
    // 1. Member variables
    SymbolID m_key{INVALID_SYMBOL_ID};
    std::string_view m_value;

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
 */
class ASTNode {
public:
    // 1. Member variables
    SourceSpan m_span;

    // 2. Constructors & Destructor
    ASTNode() = default;
    virtual ~ASTNode() = default;
};

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
    std::string_view m_value;

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

class BinaryExpr : public ExprNode {
public:
    // 1. Member variables
    TokenKind m_op{TokenKind::Invalid};
    ExprNode* m_lhs{nullptr};
    ExprNode* m_rhs{nullptr};

    // 2. Constructors
    BinaryExpr(TokenKind op, ExprNode* l, ExprNode* r)
        : m_op(op), m_lhs(l), m_rhs(r) {}
};

/**
 * @struct CallArg
 * @brief Argument at a call site, optionally labeled for named argument binding.
 */
struct CallArg {
    // 1. Member variables
    SymbolID m_label{INVALID_SYMBOL_ID}; ///< e.g. "width:" in draw_box(width: 100)
    ExprNode* m_expr{nullptr};

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

/**
 * @struct ConditionalBranch
 * @brief Represents a branch in a compile-time @if, @elif, or @else directive inside a function.
 */
struct ConditionalBranch {
    // 1. Member variables
    std::string m_condition_text;  ///< Condition (e.g. "def(ASIO_IOURING_ON) && (ASIO_IOURING_ON == 1)"), empty for @else
    std::vector<StmtNode*> m_body; ///< Statements inside this branch

    // 2. Constructors
    ConditionalBranch() = default;
    ConditionalBranch(std::string cond, std::vector<StmtNode*> body)
        : m_condition_text(std::move(cond)), m_body(std::move(body)) {}
};

/**
 * @class ConditionalStmt
 * @brief Compile-time conditional branch inside function bodies (@if (...) { ... } @elif (...) { ... } @else { ... }).
 * Zero runtime overhead; inactive branches are pruned before codegen.
 */
class ConditionalStmt : public StmtNode {
public:
    // 1. Member variables
    std::vector<ConditionalBranch> m_branches;

    // 2. Constructors
    ConditionalStmt() = default;
    explicit ConditionalStmt(std::vector<ConditionalBranch> branches)
        : m_branches(std::move(branches)) {}
};

/**
 * @class DeclNode
 * @brief Base class for declarations with attached attributes and target activation flags.
 */
class DeclNode : public ASTNode {
public:
    // 1. Member variables
    std::vector<AttributeNode> m_attributes;
    bool m_is_active{true}; ///< Cleared during Pass 0 if @target mismatch occurs

    // 2. Constructors
    DeclNode() = default;
};

/**
 * @struct ConditionalDeclBranch
 * @brief Represents a branch in a top-level compile-time declaration block (@if (cond) { ... } @else { ... }).
 */
struct ConditionalDeclBranch {
    // 1. Member variables
    std::string m_condition_text;
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    ConditionalDeclBranch() = default;
    ConditionalDeclBranch(std::string cond, std::vector<DeclNode*> decls)
        : m_condition_text(std::move(cond)), m_declarations(std::move(decls)) {}
};

/**
 * @class ConditionalDeclBlock
 * @brief Top-level compile-time conditional block containing multiple declarations.
 */
class ConditionalDeclBlock : public DeclNode {
public:
    // 1. Member variables
    std::vector<ConditionalDeclBranch> m_branches;

    // 2. Constructors
    ConditionalDeclBlock() = default;
    explicit ConditionalDeclBlock(std::vector<ConditionalDeclBranch> branches)
        : m_branches(std::move(branches)) {}
};

/**
 * @struct ImportArg
 * @brief Compile-time template argument passed into a parameterized module import (#import mod<A = 10, B = true>;).
 */
struct ImportArg {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    ExprNode* m_value{nullptr};

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
    SymbolID m_type_name{INVALID_SYMBOL_ID};
    ExprNode* m_initializer{nullptr};
    BindingKind m_kind{BindingKind::Mutable};

    // 2. Constructors
    VarDeclNode() = default;
    VarDeclNode(SymbolID name, SymbolID type_name, ExprNode* init, BindingKind kind)
        : m_name(name), m_type_name(type_name), m_initializer(init), m_kind(kind) {}
};

struct ParameterDecl {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    SymbolID m_type_name{INVALID_SYMBOL_ID};
    ExprNode* m_default_value{nullptr}; ///< Supports middle and trailing defaults

    // 2. Constructors
    ParameterDecl() = default;
    ParameterDecl(SymbolID name, SymbolID type_name, ExprNode* default_val = nullptr)
        : m_name(name), m_type_name(type_name), m_default_value(default_val) {}
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
    bool m_is_async{false}; ///< Lowers to a poll-based state machine
    ReceiverDecl m_receiver;
    std::vector<ParameterDecl> m_params;
    SymbolID m_return_type{INVALID_SYMBOL_ID};
    std::vector<StmtNode*> m_body;

    // 2. Constructors
    FunctionDeclNode() = default;
};

class RulesetDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    SymbolID m_type_param{INVALID_SYMBOL_ID};
    SymbolID m_bound{INVALID_SYMBOL_ID}; ///< e.g. T: Equatable
    std::vector<FunctionDeclNode*> m_methods;

    // 2. Constructors
    RulesetDeclNode() = default;
};

class ClassDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<ParameterDecl> m_fields;
    std::vector<FunctionDeclNode*> m_methods;

    // 2. Constructors
    ClassDeclNode() = default;
};

class BlueprintDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<ParameterDecl> m_type_params;
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    BlueprintDeclNode() = default;
};

class EnumDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<SymbolID> m_variants;

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
    SymbolID m_module_name{INVALID_SYMBOL_ID};
    std::vector<ModuleParam> m_parameters;
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    ModuleAST() = default;
};

} // namespace fool
