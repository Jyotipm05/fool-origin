//
// Created by jmbab on 05-11-2026.
//

/**
 * @file AST.ixx
 * @brief Arena-allocated Abstract Syntax Tree (AST) definitions for Fool.
 */

module;

#include <vector>
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
 * @class ImportDeclNode
 * @brief Module import directive (#import std::core;)
 */
class ImportDeclNode : public DeclNode {
public:
    // 1. Member variables
    SymbolID m_module_path{INVALID_SYMBOL_ID};

    // 2. Constructors
    ImportDeclNode() = default;
    explicit ImportDeclNode(SymbolID path) : m_module_path(path) {}
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

class ModuleAST : public ASTNode {
public:
    // 1. Member variables
    SymbolID m_module_name{INVALID_SYMBOL_ID};
    std::vector<DeclNode*> m_declarations;

    // 2. Constructors
    ModuleAST() = default;
};

} // namespace fool
