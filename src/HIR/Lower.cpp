//
// AST -> HIR lowering.
//

/**
 * @file Lower.cpp
 * @brief Lowers the checked AST of the monomorphic subset into HIR (see HIR.ixx).
 *
 * The lowering types every expression itself, from the declarations, instead of reading the checker's
 * inference table: that table keeps "integer literal" types and optional narrowing as flow state, which HIR has
 * to resolve (a literal gets the type of its context, `i32` / `f64` by default; a narrowed `T?` used as `T`
 * becomes an explicit Unwrap). The checker supplies what only it knows: which callee a call binds to (with
 * arguments in parameter order and defaults filled in) and which branch of a compile-time `@if` is active.
 *
 * Anything outside the subset aborts the function with a reason; the function is then reported as "not
 * lowerable yet" instead of producing a wrong tree.
 */

module;

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module fool.hir;

namespace fool::hir {

namespace {

struct NotLowerable {
    std::string m_reason;
    SourceSpan m_span;
};

[[noreturn]] void fail(std::string reason, SourceSpan span) { throw NotLowerable{std::move(reason), span}; }

/// A local that was initialized from an untyped literal (`sum = 0;`) is used as another numeric type
/// (`return sum;` in a function returning `f32`): the function is lowered again with that local typed accordingly.
struct RetypeLocal {
    LocalId m_local;
    TypeId m_type;
};

std::string name_of(SymbolID id) { return std::string(StringInterner::get().resolve(id)); }

using ExprPtr = std::unique_ptr<Expr>;

bool is_arithmetic(TokenKind op) {
    switch (op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
        case TokenKind::Caret: return true;
        default: return false;
    }
}

bool is_comparison(TokenKind op) {
    switch (op) {
        case TokenKind::EqualEqual:
        case TokenKind::ExclaimEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual: return true;
        default: return false;
    }
}

/// A literal (or arithmetic on literals, or `null`) whose type comes from the other operand or the context.
bool is_untyped(const ExprNode* node) {
    if (dynamic_cast<const IntegerLiteralExpr*>(node) != nullptr || dynamic_cast<const FloatLiteralExpr*>(node) != nullptr ||
        dynamic_cast<const NullLiteralExpr*>(node) != nullptr) {
        return true;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(node)) {
        return unary->m_op == TokenKind::Minus && is_untyped(unary->m_operand);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(node)) {
        return is_arithmetic(binary->m_op) && is_untyped(binary->m_lhs) && is_untyped(binary->m_rhs);
    }
    return false;
}

class Lowerer {
    // 1. Member variables
    const TypeCheckerPass2& m_checker;
    const SymbolTablePass0& m_pass0;
    Module& m_module;
    Function* m_fn{nullptr};
    std::vector<std::vector<std::pair<SymbolID, LocalId>>> m_scopes;
    bool m_isolated{false};   ///< lowering a parameter default: the caller's locals are not visible
    std::unordered_map<LocalId, TypeId> m_retypes;   ///< literal-initialized locals of the current function and the type their uses demand
    std::unordered_set<LocalId> m_defaulted;         ///< locals of the current attempt whose type came from an untyped literal

public:
    // 2. Constructors
    Lowerer(const TypeCheckerPass2& checker, const SymbolTablePass0& pass0, Module& module)
        : m_checker(checker), m_pass0(pass0), m_module(module) {}

    // 3. Member functions
    void run() {
        for (const DeclNode* decl : m_pass0.active_declarations()) {
            if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
                if (fn->m_has_body) lower_function(fn, fn->m_owner != INVALID_SYMBOL_ID ? m_pass0.find_class(fn->m_owner) : nullptr);
            } else if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
                for (const FunctionDeclNode* method : cls->m_methods) {
                    if (method->m_has_body) lower_function(method, cls);
                }
            }
        }
    }

private:
    // ---- types ---------------------------------------------------------------------------------

    [[nodiscard]] TypeId intern(Type type) { return m_module.m_types.intern(std::move(type)); }
    [[nodiscard]] const Type& ty(TypeId id) const { return m_module.m_types.get(id); }

    [[nodiscard]] TypeId simple(TypeKind kind) {
        Type type;
        type.m_kind = kind;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId named(SymbolID name) {
        Type type;
        type.m_kind = TypeKind::Named;
        type.m_name = name;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId view(TypeKind kind, bool is_const, TypeId inner) {
        Type type;
        type.m_kind = kind;
        type.m_const = is_const;
        type.m_inner = inner;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId optional_of(TypeId inner) {
        Type type;
        type.m_kind = TypeKind::Optional;
        type.m_inner = inner;
        return intern(std::move(type));
    }

    [[nodiscard]] std::optional<TypeId> primitive(std::string_view name) {
        Type type;
        const unsigned pointer_bits = m_pass0.target().m_pointer_width;
        if (name == "bool") { type.m_kind = TypeKind::Bool; return intern(std::move(type)); }
        if (name == "void") { type.m_kind = TypeKind::Void; return intern(std::move(type)); }
        const auto integer = [&](unsigned bits, bool is_signed) {
            type.m_kind = TypeKind::Int;
            type.m_bits = bits;
            type.m_signed = is_signed;
            type.m_name = StringInterner::get().intern(name);
            return std::optional<TypeId>(intern(std::move(type)));
        };
        const auto real = [&](unsigned bits) {
            type.m_kind = TypeKind::Float;
            type.m_bits = bits;
            type.m_signed = true;
            type.m_name = StringInterner::get().intern(name);
            return std::optional<TypeId>(intern(std::move(type)));
        };
        if (name == "i8") return integer(8, true);
        if (name == "i16") return integer(16, true);
        if (name == "i32") return integer(32, true);
        if (name == "i64") return integer(64, true);
        if (name == "isize") return integer(pointer_bits, true);
        if (name == "u8") return integer(8, false);
        if (name == "u16") return integer(16, false);
        if (name == "u32") return integer(32, false);
        if (name == "u64") return integer(64, false);
        if (name == "usize") return integer(pointer_bits, false);
        if (name == "f32") return real(32);
        if (name == "f64") return real(64);
        return std::nullopt;
    }

    [[nodiscard]] TypeId prim(std::string_view name) { return *primitive(name); }
    [[nodiscard]] TypeId bool_type() { return simple(TypeKind::Bool); }
    [[nodiscard]] TypeId void_type() { return simple(TypeKind::Void); }

    [[nodiscard]] TypeId lower_type(const TypeNode* node) {
        if (node == nullptr) return void_type();
        switch (node->m_kind) {
            case TypeNodeKind::Never: return simple(TypeKind::Never);
            case TypeNodeKind::Pointer: return view(TypeKind::Pointer, node->m_is_const, lower_type(node->m_inner));
            case TypeNodeKind::Reference: return view(TypeKind::Reference, node->m_is_const, lower_type(node->m_inner));
            case TypeNodeKind::Optional: return optional_of(lower_type(node->m_inner));
            case TypeNodeKind::Function: {
                Type type;
                type.m_kind = TypeKind::Function;
                for (const TypeNode* param : node->m_param_types) type.m_params.push_back(lower_type(param));
                type.m_inner = lower_type(node->m_inner);
                return intern(std::move(type));
            }
            case TypeNodeKind::Path: break;
        }
        if (node->m_path.empty()) fail("empty type path", node->m_span);
        const SymbolID last = node->m_path.back();
        const std::string text = name_of(last);
        if (!node->m_generic_args.empty()) fail("generic type '" + text + "' (generics instantiation is not implemented)", node->m_span);
        if (node->m_path.size() == 1) {
            if (const auto builtin = primitive(text)) return *builtin;
        }
        if (const ClassDeclNode* cls = m_pass0.find_class(last)) {
            if (!cls->m_type_params.empty()) fail("generic type '" + text + "' (generics instantiation is not implemented)", node->m_span);
            return named(last);
        }
        if (const EnumDeclNode* en = m_pass0.find_enum(last)) {
            if (!en->m_type_params.empty()) fail("generic type '" + text + "' (generics instantiation is not implemented)", node->m_span);
            return named(last);
        }
        if (m_pass0.find_ruleset(last) != nullptr) fail("ruleset type '" + text + "' (ruleset views are not lowerable yet)", node->m_span);
        fail("cannot resolve type '" + text + "' (generic parameters and types of modules that are not loaded are not lowerable yet)", node->m_span);
    }

    // ---- locals --------------------------------------------------------------------------------

    void push_scope() { m_scopes.emplace_back(); }
    void pop_scope() { m_scopes.pop_back(); }

    LocalId declare(SymbolID name, TypeId type, bool is_mutable, bool is_param) {
        Local local;
        local.m_name = name;
        local.m_type = type;
        local.m_mutable = is_mutable;
        local.m_is_param = is_param;
        m_fn->m_locals.push_back(local);
        const auto id = static_cast<LocalId>(m_fn->m_locals.size() - 1);
        m_scopes.back().emplace_back(name, id);
        return id;
    }

    [[nodiscard]] std::optional<LocalId> find_local(SymbolID name) const {
        if (m_isolated) return std::nullopt;
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            for (auto entry = scope->rbegin(); entry != scope->rend(); ++entry) {
                if (entry->first == name) return entry->second;
            }
        }
        return std::nullopt;
    }

    // ---- node construction ---------------------------------------------------------------------

    [[nodiscard]] static ExprPtr node(ExprKind kind, TypeId type, SourceSpan span) {
        auto expr = std::make_unique<Expr>();
        expr->m_kind = kind;
        expr->m_type = type;
        expr->m_span = span;
        return expr;
    }

    [[nodiscard]] static ExprPtr wrap_one(ExprKind kind, TypeId type, ExprPtr operand) {
        const SourceSpan span = operand->m_span;
        ExprPtr expr = node(kind, type, span);
        expr->m_operands.push_back(std::move(operand));
        return expr;
    }

    // ---- conversions ---------------------------------------------------------------------------

    /// True when `coerce` can turn a value of type `from` into `to` (the conversions the checker allows implicitly).
    [[nodiscard]] bool convertible(TypeId from, TypeId to) const {
        if (from == to) return true;
        const Type& f = ty(from);
        const Type& t = ty(to);
        if (f.m_kind == TypeKind::Never) return true;
        if (f.m_kind == TypeKind::Int && t.m_kind == TypeKind::Int) {
            if (f.m_bits == t.m_bits && f.m_signed == t.m_signed) return true;   // usize <-> u64 on this target
            if (t.m_bits <= f.m_bits) return false;
            return f.m_signed == t.m_signed || (!f.m_signed && t.m_signed);
        }
        if (f.m_kind == TypeKind::Float && t.m_kind == TypeKind::Float) return f.m_bits <= t.m_bits;
        if ((f.m_kind == TypeKind::Pointer || f.m_kind == TypeKind::Reference) &&
            (t.m_kind == TypeKind::Pointer || t.m_kind == TypeKind::Reference)) {
            return f.m_inner == t.m_inner && (t.m_const || !f.m_const);
        }
        if (t.m_kind == TypeKind::Optional && f.m_kind != TypeKind::Optional) return convertible(from, t.m_inner);
        if (f.m_kind == TypeKind::Optional && t.m_kind != TypeKind::Optional) return convertible(f.m_inner, to);
        return false;
    }

    /// If `value` is a literal-initialized local that is being used as another numeric type, ask for a second attempt.
    void maybe_retype(const Expr& value, TypeId target) const {
        if (value.m_kind != ExprKind::Local || !m_defaulted.contains(value.m_local) || m_retypes.contains(value.m_local)) return;
        const TypeKind from_kind = ty(value.m_type).m_kind;
        const TypeKind to_kind = ty(target).m_kind;
        const auto numeric = [](TypeKind kind) { return kind == TypeKind::Int || kind == TypeKind::Float; };
        if (numeric(from_kind) && numeric(to_kind) && value.m_type != target) throw RetypeLocal{value.m_local, target};
    }

    [[nodiscard]] ExprPtr coerce(ExprPtr value, TypeId to, SourceSpan span) {
        if (value->m_type == to) return value;
        if (!convertible(value->m_type, to)) {
            maybe_retype(*value, to);
            fail("cannot convert '" + m_module.m_types.text(value->m_type) + "' to '" + m_module.m_types.text(to) + "'", span);
        }
        const Type& f = ty(value->m_type);
        const Type& t = ty(to);
        if (t.m_kind == TypeKind::Optional && f.m_kind != TypeKind::Optional) {
            return wrap_one(ExprKind::Wrap, to, coerce(std::move(value), t.m_inner, span));
        }
        if (f.m_kind == TypeKind::Optional && t.m_kind != TypeKind::Optional) {
            // The checker only lets a `T?` stand for a `T` where flow analysis proved it holds a value.
            return coerce(wrap_one(ExprKind::Unwrap, f.m_inner, std::move(value)), to, span);
        }
        return wrap_one(ExprKind::Convert, to, std::move(value));
    }

    /// A condition: `bool`, or an optional (true when it holds a value).
    [[nodiscard]] ExprPtr as_condition(ExprPtr value, SourceSpan span) {
        if (ty(value->m_type).m_kind == TypeKind::Optional) {
            ExprPtr null = node(ExprKind::NullLit, value->m_type, span);
            ExprPtr test = node(ExprKind::Binary, bool_type(), span);
            test->m_op = TokenKind::ExclaimEqual;
            test->m_operands.push_back(std::move(value));
            test->m_operands.push_back(std::move(null));
            return test;
        }
        return coerce(std::move(value), bool_type(), span);
    }

    /// Strips optionals (proved non-null by the checker) and views until a value is left, for member access.
    [[nodiscard]] ExprPtr to_value(ExprPtr value) {
        while (true) {
            const Type& type = ty(value->m_type);
            if (type.m_kind == TypeKind::Optional) {
                const TypeId inner = type.m_inner;
                value = wrap_one(ExprKind::Unwrap, inner, std::move(value));
            } else if (type.m_kind == TypeKind::Pointer || type.m_kind == TypeKind::Reference) {
                const TypeId inner = type.m_inner;
                value = wrap_one(ExprKind::Deref, inner, std::move(value));
            } else {
                return value;
            }
        }
    }

    /// Makes both operands one type: the checker allowed one to convert to the other.
    void unify(ExprPtr& lhs, ExprPtr& rhs, bool equality, SourceSpan span) {
        if (lhs->m_type == rhs->m_type) return;
        const bool lhs_optional = ty(lhs->m_type).m_kind == TypeKind::Optional;
        const bool rhs_optional = ty(rhs->m_type).m_kind == TypeKind::Optional;
        if (equality && lhs_optional != rhs_optional) {
            // `a == 5` compares optionals: the plain side is wrapped.
            if (lhs_optional) rhs = coerce(std::move(rhs), lhs->m_type, span);
            else lhs = coerce(std::move(lhs), rhs->m_type, span);
            return;
        }
        if (convertible(rhs->m_type, lhs->m_type)) {
            rhs = coerce(std::move(rhs), lhs->m_type, span);
        } else if (convertible(lhs->m_type, rhs->m_type)) {
            lhs = coerce(std::move(lhs), rhs->m_type, span);
        } else {
            maybe_retype(*lhs, rhs->m_type);
            maybe_retype(*rhs, lhs->m_type);
            fail("operands of type '" + m_module.m_types.text(lhs->m_type) + "' and '" + m_module.m_types.text(rhs->m_type) +
                     "' have no common type",
                 span);
        }
    }

    // ---- functions -----------------------------------------------------------------------------

    void lower_function(const FunctionDeclNode* decl, const ClassDeclNode* owner) {
        Function fn;
        const SymbolID owner_name = decl->m_owner != INVALID_SYMBOL_ID ? decl->m_owner : (owner != nullptr ? owner->m_name : INVALID_SYMBOL_ID);
        fn.m_name = (owner_name != INVALID_SYMBOL_ID ? name_of(owner_name) + "::" : std::string()) + name_of(decl->m_name);
        fn.m_decl = decl;
        fn.m_reason_span = decl->m_span;
        m_module.m_functions.push_back(std::move(fn));
        m_fn = &m_module.m_functions.back();
        m_isolated = false;
        m_retypes.clear();

        // One attempt per literal-initialized local that turns out to need another type; bounded by the number of locals.
        for (size_t attempt = 0;; ++attempt) {
            m_scopes.clear();
            m_defaulted.clear();
            m_fn->m_locals.clear();
            m_fn->m_body.clear();
            m_fn->m_param_count = 0;
            try {
                lower_body(decl, owner, owner_name);
                m_fn->m_lowered = true;
                break;
            } catch (const RetypeLocal& retype) {
                if (attempt >= 64) {
                    m_fn->m_reason = "the types of literal-initialized locals do not settle";
                    break;
                }
                m_retypes[retype.m_local] = retype.m_type;
            } catch (const NotLowerable& reason) {
                m_fn->m_reason = reason.m_reason;
                m_fn->m_reason_span = reason.m_span;
                break;
            }
        }
        if (!m_fn->m_lowered) {
            m_fn->m_body.clear();
            m_fn->m_locals.clear();
            m_fn->m_param_count = 0;
        }
        m_scopes.clear();
        m_fn = nullptr;
    }

    void lower_body(const FunctionDeclNode* decl, const ClassDeclNode* owner, SymbolID owner_name) {
        if (decl->m_is_async) fail("async functions (state machine lowering is not implemented)", decl->m_span);
        if (!decl->m_type_params.empty()) fail("generic function (generics instantiation is not implemented)", decl->m_span);
        if (owner != nullptr && !owner->m_type_params.empty()) fail("method of a generic type (generics instantiation is not implemented)", decl->m_span);
        m_fn->m_return = lower_type(decl->m_return_type);
        push_scope();
        if (decl->m_receiver.m_kind != ReceiverKind::None) {
            if (owner_name == INVALID_SYMBOL_ID) fail("method without an owner type", decl->m_span);
            const TypeId self_type = named(owner_name);
            switch (decl->m_receiver.m_kind) {
                case ReceiverKind::ByValue: declare(decl->m_receiver.m_self_name, self_type, true, true); break;
                case ReceiverKind::ByConstRef:
                    declare(decl->m_receiver.m_self_name, view(TypeKind::Reference, true, self_type), true, true);
                    break;
                default: declare(decl->m_receiver.m_self_name, view(TypeKind::Reference, false, self_type), true, true); break;
            }
        }
        for (const ParameterDecl& param : decl->m_params) declare(param.m_name, lower_type(param.m_type), true, true);
        m_fn->m_param_count = m_fn->m_locals.size();
        lower_statements(decl->m_body, m_fn->m_body);
        pop_scope();
    }

    // ---- statements ----------------------------------------------------------------------------

    void lower_statements(const std::vector<StmtNode*>& stmts, std::vector<Stmt>& out) {
        for (const StmtNode* stmt : stmts) lower_statement(stmt, out);
    }

    void lower_scoped(const BlockStmt* block, std::vector<Stmt>& out) {
        if (block == nullptr) return;
        push_scope();
        lower_statements(block->m_statements, out);
        pop_scope();
    }

    [[nodiscard]] static Stmt make_stmt(StmtKind kind, SourceSpan span) {
        Stmt stmt;
        stmt.m_kind = kind;
        stmt.m_span = span;
        return stmt;
    }

    void lower_statement(const StmtNode* stmt, std::vector<Stmt>& out) {
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::Return, stmt->m_span);
            if (ret->m_value != nullptr) {
                if (ty(m_fn->m_return).m_kind == TypeKind::Never) fail("'return' in a function that never returns", stmt->m_span);
                s.m_expr = coerce(lower_expr(ret->m_value, m_fn->m_return), m_fn->m_return, ret->m_value->m_span);
            }
            out.push_back(std::move(s));
        } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
            lower_expression_statement(expr_stmt, out);
        } else if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
            const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl);
            if (var == nullptr) fail("declarations other than variables inside a function", stmt->m_span);
            Stmt s = make_stmt(StmtKind::Let, stmt->m_span);
            TypeId type = NO_TYPE;
            if (var->m_type != nullptr) {
                type = lower_type(var->m_type);
                if (var->m_initializer != nullptr) s.m_expr = coerce(lower_expr(var->m_initializer, type), type, var->m_initializer->m_span);
            } else {
                if (var->m_initializer == nullptr) fail("variable '" + name_of(var->m_name) + "' has neither a type nor an initializer", stmt->m_span);
                out.push_back(lower_inferred_let(var->m_name, var->m_initializer, var->m_kind == BindingKind::Mutable, stmt->m_span));
                return;
            }
            require_storable(type, stmt->m_span);
            s.m_local = declare(var->m_name, type, var->m_kind == BindingKind::Mutable, false);
            out.push_back(std::move(s));
        } else if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::Block, stmt->m_span);
            lower_scoped(block, s.m_body);
            out.push_back(std::move(s));
        } else if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
            out.push_back(lower_if(if_stmt));
        } else if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::While, stmt->m_span);
            s.m_expr = as_condition(lower_expr(while_stmt->m_condition, bool_type()), while_stmt->m_condition->m_span);
            lower_scoped(while_stmt->m_body, s.m_body);
            out.push_back(std::move(s));
        } else if (dynamic_cast<const ForStmt*>(stmt) != nullptr) {
            fail("'for' loops (the iteration protocol is not defined yet)", stmt->m_span);
        } else if (dynamic_cast<const BreakStmt*>(stmt) != nullptr) {
            out.push_back(make_stmt(StmtKind::Break, stmt->m_span));
        } else if (dynamic_cast<const ContinueStmt*>(stmt) != nullptr) {
            out.push_back(make_stmt(StmtKind::Continue, stmt->m_span));
        } else if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
            if (const ConditionalBranch* branch = m_checker.active_branch(conditional)) {
                Stmt s = make_stmt(StmtKind::Block, stmt->m_span);
                push_scope();
                lower_statements(branch->m_body, s.m_body);
                pop_scope();
                out.push_back(std::move(s));
            }
        } else {
            fail("unsupported statement", stmt->m_span);
        }
    }

    /// `var x = value;` / `x = value;`: the local takes the value's type. A literal gives `i32` / `f64` unless the
    /// local's uses demanded another numeric type on an earlier attempt.
    [[nodiscard]] Stmt lower_inferred_let(SymbolID name, const ExprNode* init, bool is_mutable, SourceSpan span) {
        Stmt s = make_stmt(StmtKind::Let, span);
        const auto next_id = static_cast<LocalId>(m_fn->m_locals.size());
        const bool literal = is_untyped(init) && dynamic_cast<const NullLiteralExpr*>(init) == nullptr;
        TypeId type = NO_TYPE;
        if (const auto retyped = m_retypes.find(next_id); literal && retyped != m_retypes.end()) {
            type = retyped->second;
            s.m_expr = coerce(lower_expr(init, type), type, span);
        } else {
            s.m_expr = lower_expr(init, NO_TYPE);
            type = s.m_expr->m_type;
        }
        require_storable(type, span);
        s.m_local = declare(name, type, is_mutable, false);
        if (literal) m_defaulted.insert(s.m_local);
        return s;
    }

    void require_storable(TypeId type, SourceSpan span) {
        const TypeKind kind = ty(type).m_kind;
        if (kind == TypeKind::Void || kind == TypeKind::Never) fail("a variable cannot hold a value of type '" + m_module.m_types.text(type) + "'", span);
    }

    Stmt lower_if(const IfStmt* if_stmt) {
        Stmt s = make_stmt(StmtKind::If, if_stmt->m_span);
        s.m_expr = as_condition(lower_expr(if_stmt->m_condition, bool_type()), if_stmt->m_condition->m_span);
        lower_scoped(if_stmt->m_then, s.m_body);
        if (const auto* chained = dynamic_cast<const IfStmt*>(if_stmt->m_else)) {
            s.m_else.push_back(lower_if(chained));
        } else if (const auto* block = dynamic_cast<const BlockStmt*>(if_stmt->m_else)) {
            lower_scoped(block, s.m_else);
        }
        return s;
    }

    void lower_expression_statement(const ExprStmt* expr_stmt, std::vector<Stmt>& out) {
        // `x = value;` with no `x` in scope declares it: a mutable local of the value's type.
        if (const auto* assign = dynamic_cast<const BinaryExpr*>(expr_stmt->m_expr); assign != nullptr && assign->m_op == TokenKind::Equal) {
            if (const auto* id = dynamic_cast<const IdentifierExpr*>(assign->m_lhs);
                id != nullptr && !find_local(id->m_identifier) && m_pass0.find_global(id->m_identifier) == nullptr) {
                out.push_back(lower_inferred_let(id->m_identifier, assign->m_rhs, true, expr_stmt->m_span));
                return;
            }
        }
        Stmt s = make_stmt(StmtKind::Expr, expr_stmt->m_span);
        s.m_expr = lower_expr(expr_stmt->m_expr, NO_TYPE);
        out.push_back(std::move(s));
    }

    // ---- expressions ---------------------------------------------------------------------------

    /// Type a literal takes: the context's (through an optional) when it fits the literal's kind, else the default.
    [[nodiscard]] TypeId literal_type(TypeId expected, TypeKind kind, TypeId fallback) const {
        if (expected == NO_TYPE) return fallback;
        const Type* type = &ty(expected);
        if (type->m_kind == TypeKind::Optional) type = &ty(type->m_inner);
        if (type->m_kind != kind) return fallback;
        return type == &ty(expected) ? expected : ty(expected).m_inner;
    }

    [[nodiscard]] ExprPtr lower_expr(const ExprNode* e, TypeId expected) {
        if (e == nullptr) fail("missing expression", SourceSpan{});
        if (const auto* literal = dynamic_cast<const IntegerLiteralExpr*>(e)) {
            if (const TypeId real = literal_type(expected, TypeKind::Float, NO_TYPE); real != NO_TYPE) {
                // An integer literal adapts to a floating-point context (`scale: f32 = 1`).
                ExprPtr out = node(ExprKind::FloatLit, real, e->m_span);
                out->m_float = static_cast<double>(literal->m_value);
                return out;
            }
            const bool fits_i32 = literal->m_value >= INT32_MIN && literal->m_value <= INT32_MAX;
            ExprPtr out = node(ExprKind::IntLit, literal_type(expected, TypeKind::Int, prim(fits_i32 ? "i32" : "i64")), e->m_span);
            out->m_int = literal->m_value;
            return out;
        }
        if (const auto* literal = dynamic_cast<const FloatLiteralExpr*>(e)) {
            ExprPtr out = node(ExprKind::FloatLit, literal_type(expected, TypeKind::Float, prim("f64")), e->m_span);
            out->m_float = literal->m_value;
            return out;
        }
        if (const auto* literal = dynamic_cast<const BooleanLiteralExpr*>(e)) {
            ExprPtr out = node(ExprKind::BoolLit, bool_type(), e->m_span);
            out->m_int = literal->m_value ? 1 : 0;
            return out;
        }
        if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(e)) {
            TypeId text_type = named(StringInterner::get().intern("StringView"));
            if (expected != NO_TYPE && ty(expected).m_kind == TypeKind::Pointer) {
                const Type& pointee = ty(ty(expected).m_inner);
                if (pointee.m_kind == TypeKind::Int && pointee.m_bits == 8) text_type = expected;   // a literal doubles as `const* u8`
            }
            ExprPtr out = node(ExprKind::StrLit, text_type, e->m_span);
            out->m_text = std::string(literal->m_value);
            return out;
        }
        if (dynamic_cast<const NullLiteralExpr*>(e) != nullptr) {
            if (expected == NO_TYPE || ty(expected).m_kind != TypeKind::Optional) fail("'null' needs an optional type from its context", e->m_span);
            return node(ExprKind::NullLit, expected, e->m_span);
        }
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(e)) return lower_identifier(id);
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(e)) return lower_unary(unary, expected);
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(e)) return lower_binary(binary, expected);
        if (const auto* cast = dynamic_cast<const CastExpr*>(e)) {
            ExprPtr operand = lower_expr(cast->m_operand, NO_TYPE);
            ExprPtr out = node(ExprKind::Cast, lower_type(cast->m_target), e->m_span);
            out->m_cast = cast->m_cast_kind;
            out->m_operands.push_back(std::move(operand));
            return out;
        }
        if (const auto* member = dynamic_cast<const MemberExpr*>(e)) return lower_member(member);
        if (const auto* index = dynamic_cast<const IndexExpr*>(e)) return lower_index(index);
        if (const auto* call = dynamic_cast<const CallExpr*>(e)) return lower_call(call);
        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(e)) return lower_struct_literal(literal);
        if (dynamic_cast<const LambdaExpr*>(e) != nullptr) fail("lambdas (closure conversion is not implemented)", e->m_span);
        if (dynamic_cast<const IntrinsicCallExpr*>(e) != nullptr) fail("compile-time intrinsics and reflection (not evaluated yet)", e->m_span);
        if (dynamic_cast<const PathExpr*>(e) != nullptr) fail("qualified paths such as enum variants (not lowerable yet)", e->m_span);
        fail("unsupported expression", e->m_span);
    }

    [[nodiscard]] ExprPtr lower_identifier(const IdentifierExpr* id) {
        if (const auto local = find_local(id->m_identifier)) {
            ExprPtr out = node(ExprKind::Local, m_fn->m_locals[*local].m_type, id->m_span);
            out->m_local = *local;
            return out;
        }
        if (const VarDeclNode* global = m_pass0.find_global(id->m_identifier)) {
            if (global->m_type == nullptr) fail("global '" + name_of(id->m_identifier) + "' has no declared type", id->m_span);
            ExprPtr out = node(ExprKind::Global, lower_type(global->m_type), id->m_span);
            out->m_name = id->m_identifier;
            return out;
        }
        if (m_pass0.find_function(id->m_identifier) != nullptr) fail("function '" + name_of(id->m_identifier) + "' used as a value (function values are not lowerable yet)", id->m_span);
        fail("unresolved name '" + name_of(id->m_identifier) + "'", id->m_span);
    }

    [[nodiscard]] ExprPtr lower_unary(const UnaryExpr* unary, TypeId expected) {
        switch (unary->m_op) {
            case TokenKind::Minus: {
                ExprPtr operand = lower_expr(unary->m_operand, expected);
                const TypeId type = operand->m_type;
                ExprPtr out = wrap_one(ExprKind::Unary, type, std::move(operand));
                out->m_op = TokenKind::Minus;
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Bang: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                if (ty(operand->m_type).m_kind == TypeKind::Optional) {
                    // `!a` on an optional: true when it is empty.
                    ExprPtr null = node(ExprKind::NullLit, operand->m_type, unary->m_span);
                    ExprPtr test = node(ExprKind::Binary, bool_type(), unary->m_span);
                    test->m_op = TokenKind::EqualEqual;
                    test->m_operands.push_back(std::move(operand));
                    test->m_operands.push_back(std::move(null));
                    return test;
                }
                const TypeId type = operand->m_type;
                ExprPtr out = wrap_one(ExprKind::Unary, type, std::move(operand));
                out->m_op = TokenKind::Bang;
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Ampersand: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                const TypeId ref_type = view(TypeKind::Reference, false, operand->m_type);
                ExprPtr out = wrap_one(ExprKind::AddrOf, ref_type, std::move(operand));
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Star: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                if (ty(operand->m_type).m_kind == TypeKind::Optional) {
                    const TypeId inner = ty(operand->m_type).m_inner;
                    operand = wrap_one(ExprKind::Unwrap, inner, std::move(operand));
                }
                const Type& type = ty(operand->m_type);
                if (type.m_kind != TypeKind::Pointer && type.m_kind != TypeKind::Reference) {
                    fail("cannot dereference a value of type '" + m_module.m_types.text(operand->m_type) + "'", unary->m_span);
                }
                const TypeId deref_type = type.m_inner;
                ExprPtr out = wrap_one(ExprKind::Deref, deref_type, std::move(operand));
                out->m_span = unary->m_span;
                return out;
            }
            default: fail("unsupported unary operator", unary->m_span);
        }
    }

    [[nodiscard]] ExprPtr lower_binary(const BinaryExpr* binary, TypeId expected) {
        const SourceSpan span = binary->m_span;
        if (binary->m_op == TokenKind::Equal) return lower_assignment(binary);
        if (binary->m_op == TokenKind::AmpAmp || binary->m_op == TokenKind::PipePipe) {
            ExprPtr lhs = as_condition(lower_expr(binary->m_lhs, bool_type()), binary->m_lhs->m_span);
            ExprPtr rhs = as_condition(lower_expr(binary->m_rhs, bool_type()), binary->m_rhs->m_span);
            ExprPtr out = node(ExprKind::Binary, bool_type(), span);
            out->m_op = binary->m_op;
            out->m_operands.push_back(std::move(lhs));
            out->m_operands.push_back(std::move(rhs));
            return out;
        }
        const bool comparison = is_comparison(binary->m_op);
        if (!comparison && !is_arithmetic(binary->m_op)) fail("unsupported binary operator", span);

        // A literal takes its type from the other operand (or from the context when both are literals).
        const bool lhs_untyped = is_untyped(binary->m_lhs);
        const bool rhs_untyped = is_untyped(binary->m_rhs);
        const TypeId hint = comparison ? NO_TYPE : expected;
        ExprPtr lhs;
        ExprPtr rhs;
        if (lhs_untyped && !rhs_untyped) {
            rhs = lower_expr(binary->m_rhs, NO_TYPE);
            lhs = lower_expr(binary->m_lhs, rhs->m_type);
        } else {
            lhs = lower_expr(binary->m_lhs, lhs_untyped ? hint : NO_TYPE);
            rhs = lower_expr(binary->m_rhs, lhs->m_type);
        }
        const bool equality = binary->m_op == TokenKind::EqualEqual || binary->m_op == TokenKind::ExclaimEqual;
        unify(lhs, rhs, equality, span);
        const TypeId type = comparison ? bool_type() : lhs->m_type;
        ExprPtr out = node(ExprKind::Binary, type, span);
        out->m_op = binary->m_op;
        out->m_operands.push_back(std::move(lhs));
        out->m_operands.push_back(std::move(rhs));
        return out;
    }

    /// A place that can be assigned to or have its address taken: a local, a global, a field, a dereference or an index.
    [[nodiscard]] ExprPtr lower_place(const ExprNode* target) {
        if (dynamic_cast<const IdentifierExpr*>(target) != nullptr || dynamic_cast<const MemberExpr*>(target) != nullptr ||
            dynamic_cast<const IndexExpr*>(target) != nullptr) {
            return lower_expr(target, NO_TYPE);
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(target); unary != nullptr && unary->m_op == TokenKind::Star) {
            return lower_expr(target, NO_TYPE);
        }
        fail("assignment to something that is not a place", target->m_span);
    }

    [[nodiscard]] ExprPtr lower_assignment(const BinaryExpr* assign) {
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(assign->m_lhs);
            id != nullptr && !find_local(id->m_identifier) && m_pass0.find_global(id->m_identifier) == nullptr) {
            fail("declaration '" + name_of(id->m_identifier) + " = ...' used as a value", assign->m_span);
        }
        ExprPtr place = lower_place(assign->m_lhs);
        // The value is evaluated first (as in the checker), then the place is written.
        ExprPtr value = lower_expr(assign->m_rhs, place->m_type);
        if (value->m_type != place->m_type && !convertible(value->m_type, place->m_type)) maybe_retype(*place, value->m_type);
        value = coerce(std::move(value), place->m_type, assign->m_rhs->m_span);
        ExprPtr out = node(ExprKind::Assign, void_type(), assign->m_span);
        out->m_operands.push_back(std::move(place));
        out->m_operands.push_back(std::move(value));
        return out;
    }

    [[nodiscard]] const ClassDeclNode* class_of(TypeId value_type, SourceSpan span) {
        const Type& type = ty(value_type);
        if (type.m_kind != TypeKind::Named) fail("member access on a value of type '" + m_module.m_types.text(value_type) + "'", span);
        const ClassDeclNode* cls = m_pass0.find_class(type.m_name);
        if (cls == nullptr) fail("'" + name_of(type.m_name) + "' is not a class or struct (enum members are not lowerable yet)", span);
        if (!cls->m_type_params.empty()) fail("generic type '" + name_of(type.m_name) + "' (generics instantiation is not implemented)", span);
        return cls;
    }

    [[nodiscard]] ExprPtr lower_member(const MemberExpr* member) {
        ExprPtr object = to_value(lower_expr(member->m_object, NO_TYPE));
        const ClassDeclNode* cls = class_of(object->m_type, member->m_span);
        for (size_t i = 0; i < cls->m_fields.size(); ++i) {
            if (cls->m_fields[i].m_name != member->m_member) continue;
            ExprPtr out = node(ExprKind::Field, lower_type(cls->m_fields[i].m_type), member->m_span);
            out->m_index = i;
            out->m_name = member->m_member;
            out->m_operands.push_back(std::move(object));
            return out;
        }
        fail("'" + name_of(cls->m_name) + "' has no field '" + name_of(member->m_member) + "' (methods are only lowerable when called)", member->m_span);
    }

    [[nodiscard]] ExprPtr lower_index(const IndexExpr* index) {
        ExprPtr base = lower_expr(index->m_base, NO_TYPE);
        if (ty(base->m_type).m_kind == TypeKind::Optional) {
            const TypeId inner = ty(base->m_type).m_inner;
            base = wrap_one(ExprKind::Unwrap, inner, std::move(base));
        }
        const TypeKind base_kind = ty(base->m_type).m_kind;
        if (base_kind != TypeKind::Pointer && base_kind != TypeKind::Reference) {
            fail("indexing a value of type '" + m_module.m_types.text(base->m_type) + "' (only pointers and references can be indexed)", index->m_span);
        }
        const TypeId element = ty(base->m_type).m_inner;   // copied: interning below may move the type table
        ExprPtr position = lower_expr(index->m_index, prim("usize"));
        if (ty(position->m_type).m_kind != TypeKind::Int) fail("an index must be an integer", index->m_index->m_span);
        ExprPtr out = node(ExprKind::Index, element, index->m_span);
        out->m_operands.push_back(std::move(base));
        out->m_operands.push_back(std::move(position));
        return out;
    }

    [[nodiscard]] ExprPtr lower_call(const CallExpr* call) {
        const ResolvedCall* resolved = m_checker.find_resolved_call(call);
        if (resolved == nullptr) fail("call that is not bound to a function (function values, built-ins and calls into unseen modules are not lowerable yet)", call->m_span);
        const FunctionDeclNode* callee = resolved->m_callee;
        if (!callee->m_type_params.empty()) fail("call of generic function '" + resolved->m_display + "' (generics instantiation is not implemented)", call->m_span);
        if (callee->m_owner != INVALID_SYMBOL_ID) {
            if (const ClassDeclNode* owner = m_pass0.find_class(callee->m_owner); owner != nullptr && !owner->m_type_params.empty()) {
                fail("call of a method of generic type '" + name_of(owner->m_name) + "' (generics instantiation is not implemented)", call->m_span);
            }
        }

        ExprPtr out = node(ExprKind::Call, lower_type(callee->m_return_type), call->m_span);
        out->m_callee = callee;
        out->m_text = resolved->m_display;
        out->m_name = callee->m_name;

        if (resolved->m_receiver != nullptr) {
            const ReceiverKind kind = callee->m_receiver.m_kind;
            if (kind == ReceiverKind::None) fail("method call of '" + resolved->m_display + "', which has no receiver", call->m_span);
            ExprPtr object = lower_expr(resolved->m_receiver, NO_TYPE);
            if (ty(object->m_type).m_kind == TypeKind::Optional) {
                const TypeId inner = ty(object->m_type).m_inner;
                object = wrap_one(ExprKind::Unwrap, inner, std::move(object));
            }
            if (kind == ReceiverKind::ByValue) {
                out->m_operands.push_back(to_value(std::move(object)));
            } else {
                if (const TypeKind object_kind = ty(object->m_type).m_kind; object_kind != TypeKind::Pointer && object_kind != TypeKind::Reference) {
                    const TypeId ref_type = view(TypeKind::Reference, false, object->m_type);
                    object = wrap_one(ExprKind::AddrOf, ref_type, std::move(object));
                }
                const TypeId inner = ty(object->m_type).m_inner;
                const TypeId target_view = view(TypeKind::Reference, kind == ReceiverKind::ByConstRef, inner);
                out->m_operands.push_back(coerce(std::move(object), target_view, call->m_span));
            }
        } else if (callee->m_receiver.m_kind != ReceiverKind::None) {
            fail("call of method '" + resolved->m_display + "' without a receiver", call->m_span);
        }

        for (size_t i = 0; i < resolved->m_slots.size(); ++i) {
            const ParameterDecl& param = callee->m_params[i];
            const TypeId param_type = lower_type(param.m_type);
            const ResolvedArgument& slot = resolved->m_slots[i];
            const bool saved_isolated = m_isolated;
            m_isolated = slot.m_is_default;   // a default is written in the callee's world: no locals of the caller
            ExprPtr value = lower_expr(slot.m_value, param_type);
            m_isolated = saved_isolated;
            out->m_operands.push_back(coerce(std::move(value), param_type, slot.m_value != nullptr ? slot.m_value->m_span : call->m_span));
        }
        return out;
    }

    [[nodiscard]] ExprPtr lower_struct_literal(const StructLiteralExpr* literal) {
        const TypeId type = lower_type(literal->m_type);
        const ClassDeclNode* cls = class_of(type, literal->m_span);
        ExprPtr out = node(ExprKind::StructLit, type, literal->m_span);
        for (const ParameterDecl& field : cls->m_fields) {
            const TypeId field_type = lower_type(field.m_type);
            const FieldInit* init = nullptr;
            for (const FieldInit& candidate : literal->m_fields) {
                if (candidate.m_name == field.m_name) init = &candidate;
            }
            if (init != nullptr) {
                out->m_operands.push_back(coerce(lower_expr(init->m_value, field_type), field_type, init->m_span));
            } else if (ty(field_type).m_kind == TypeKind::Optional) {
                out->m_operands.push_back(node(ExprKind::NullLit, field_type, literal->m_span));   // an omitted optional field starts as null
            } else {
                fail("missing initializer for field '" + name_of(field.m_name) + "'", literal->m_span);
            }
        }
        return out;
    }
};

} // namespace

Module lower_module(const TypeCheckerPass2& checker, const SymbolTablePass0& pass0) {
    Module module;
    Lowerer(checker, pass0, module).run();
    return module;
}

} // namespace fool::hir
