//
// Created by jmbab on 09-11-2026.
//

/**
 * @file TypeCheckerCasts.cpp
 * @brief Lightweight static types and validation of the named casts
 *        `@static_cast<T>`, `@dynamic_cast<T>`, `@reinterpret_cast<T>` and `@bit_cast<T>`.
 *
 * The checker has no full type system yet, so inference is deliberately partial: whenever a type
 * can not be determined (generics, other modules' functions) the corresponding check is skipped
 * instead of guessed.
 */

module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }

struct Primitive {
    StaticType::Kind m_kind{StaticType::Kind::Unknown};
    unsigned m_bits{0};
    bool m_signed{false};
};

Primitive primitive_named(std::string_view name, unsigned pointer_width) {
    using Kind = StaticType::Kind;
    if (name == "i8") return {Kind::Int, 8, true};
    if (name == "i16") return {Kind::Int, 16, true};
    if (name == "i32") return {Kind::Int, 32, true};
    if (name == "i64") return {Kind::Int, 64, true};
    if (name == "isize") return {Kind::Int, pointer_width, true};
    if (name == "u8") return {Kind::Int, 8, false};
    if (name == "u16") return {Kind::Int, 16, false};
    if (name == "u32") return {Kind::Int, 32, false};
    if (name == "u64") return {Kind::Int, 64, false};
    if (name == "usize") return {Kind::Int, pointer_width, false};
    if (name == "f32") return {Kind::Float, 32, true};
    if (name == "f64") return {Kind::Float, 64, true};
    if (name == "bool") return {Kind::Bool, 8, false};
    if (name == "void") return {Kind::Void, 0, false};
    return {};
}

StaticType make_view(StaticType::Kind kind, bool is_const, StaticType pointee) {
    StaticType view;
    view.m_kind = kind;
    view.m_const = is_const;
    view.m_pointee = std::make_shared<StaticType>(std::move(pointee));
    return view;
}

bool same_type(const StaticType& a, const StaticType& b) {
    if (a.m_kind != b.m_kind) return false;
    switch (a.m_kind) {
        case StaticType::Kind::Int:
        case StaticType::Kind::Float:
            if (a.m_literal || b.m_literal) return true; // an untyped literal adapts to the other side
            return a.m_bits == b.m_bits && a.m_signed == b.m_signed;
        case StaticType::Kind::Named: return a.m_name == b.m_name;
        case StaticType::Kind::Optional:
            return a.m_pointee && b.m_pointee && same_type(*a.m_pointee, *b.m_pointee);
        case StaticType::Kind::Function: {
            if (a.m_params.size() != b.m_params.size() || !a.m_pointee || !b.m_pointee) return false;
            for (size_t i = 0; i < a.m_params.size(); ++i) {
                // an unknown parameter type (generic, foreign) never makes two signatures differ
                if (a.m_params[i].known() && b.m_params[i].known() && !same_type(a.m_params[i], b.m_params[i])) return false;
            }
            return !a.m_pointee->known() || !b.m_pointee->known() || same_type(*a.m_pointee, *b.m_pointee);
        }
        case StaticType::Kind::Pointer:
        case StaticType::Kind::Reference:
            return a.m_const == b.m_const && a.m_pointee && b.m_pointee && same_type(*a.m_pointee, *b.m_pointee);
        default: return true;
    }
}

bool is_pointee_void(const StaticType& view) { return view.m_pointee && view.m_pointee->m_kind == StaticType::Kind::Void; }

} // namespace

// ---------------------------------------------------------------------------------------------
// Static types
// ---------------------------------------------------------------------------------------------

StaticType TypeCheckerPass2::static_type_of(const TypeNode* node) const {
    if (node == nullptr) return {};
    if (node->m_kind == TypeNodeKind::Function) {
        StaticType function;
        function.m_kind = StaticType::Kind::Function;
        for (const TypeNode* param : node->m_param_types) function.m_params.push_back(static_type_of(param));
        StaticType result;
        if (node->m_inner == nullptr) result.m_kind = StaticType::Kind::Void;
        else result = static_type_of(node->m_inner);
        function.m_pointee = std::make_shared<StaticType>(std::move(result));
        return function;
    }
    if (node->m_kind == TypeNodeKind::Optional) {
        StaticType optional;
        optional.m_kind = StaticType::Kind::Optional;
        optional.m_pointee = std::make_shared<StaticType>(static_type_of(node->m_inner));
        return optional;
    }
    if (node->m_kind == TypeNodeKind::Pointer || node->m_kind == TypeNodeKind::Reference) {
        return make_view(node->m_kind == TypeNodeKind::Pointer ? StaticType::Kind::Pointer : StaticType::Kind::Reference,
                         node->m_is_const, static_type_of(node->m_inner));
    }
    if (node->m_kind != TypeNodeKind::Path || node->m_path.empty()) return {};
    if (node->m_path.size() == 1 && node->m_generic_args.empty()) {
        const SymbolID name = node->m_path.front();
        Primitive primitive = primitive_named(text_of(name), m_pass0.target().m_pointer_width);
        if (primitive.m_kind != StaticType::Kind::Unknown) {
            StaticType type;
            type.m_kind = primitive.m_kind;
            type.m_name = name;
            type.m_bits = primitive.m_bits;
            type.m_signed = primitive.m_signed;
            return type;
        }
        if (is_generic_parameter(name)) return {}; // resolved only after instantiation
    }
    StaticType named;
    named.m_kind = StaticType::Kind::Named;
    named.m_name = node->m_path.back();
    if (!node->m_generic_args.empty()) named.m_name = INVALID_SYMBOL_ID; // generic instances: layout unknown
    if (named.m_name == INVALID_SYMBOL_ID) return {};
    return named;
}

StaticType TypeCheckerPass2::infer_type(const ExprNode* expr) {
    if (expr == nullptr) return {};
    if (auto it = m_expr_types.find(expr); it != m_expr_types.end()) return it->second;
    StaticType type = compute_type(expr);
    m_expr_types.emplace(expr, type);
    return type;
}

const StaticType* TypeCheckerPass2::type_of(const ExprNode* expr) const {
    auto it = m_expr_types.find(expr);
    return it == m_expr_types.end() ? nullptr : &it->second;
}

StaticType TypeCheckerPass2::compute_type(const ExprNode* expr) {
    using Kind = StaticType::Kind;
    if (expr == nullptr) return {};

    if (dynamic_cast<const IntegerLiteralExpr*>(expr) != nullptr) {
        StaticType type;
        type.m_kind = Kind::Int;
        type.m_literal = true;
        return type;
    }
    if (dynamic_cast<const FloatLiteralExpr*>(expr) != nullptr) {
        StaticType type;
        type.m_kind = Kind::Float;
        type.m_literal = true;
        return type;
    }
    if (dynamic_cast<const BooleanLiteralExpr*>(expr) != nullptr) {
        StaticType type;
        type.m_kind = Kind::Bool;
        type.m_bits = 8;
        return type;
    }
    if (dynamic_cast<const NullLiteralExpr*>(expr) != nullptr) {
        StaticType type;
        type.m_kind = Kind::Null;
        return type;
    }
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        if (const SymbolBinding* binding = lookup(id->m_identifier)) {
            // Where the flow analysis proved the optional holds a value, it is used as the plain type.
            if (binding->m_narrowed && binding->m_static_type.is_optional() && binding->m_static_type.m_pointee) {
                return *binding->m_static_type.m_pointee;
            }
            return binding->m_static_type;
        }
        if (const VarDeclNode* global = m_pass0.find_global_from(m_namespace, id->m_identifier)) return static_type_of(global->m_type);
        return {};
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) return static_type_of(cast->m_target);
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        StaticType operand = infer_type(unary->m_operand);
        switch (unary->m_op) {
            case TokenKind::Ampersand:
                return operand.known() ? make_view(Kind::Reference, false, std::move(operand)) : StaticType{};
            case TokenKind::Star:
                return (operand.is_view() && operand.m_pointee) ? *operand.m_pointee : StaticType{};
            case TokenKind::Bang: {
                // `!` is logical not on a bool and bitwise complement on an integer (the stdlib relies on it).
                if (operand.m_kind == Kind::Int) return operand;
                if (operand.m_kind != Kind::Bool) return {};
                StaticType type;
                type.m_kind = Kind::Bool;
                type.m_bits = 8;
                return type;
            }
            case TokenKind::Minus: return operand;
            default: return {};
        }
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        switch (binary->m_op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
            case TokenKind::Ampersand:
            case TokenKind::Pipe:
            case TokenKind::Caret: {
                StaticType lhs = infer_type(binary->m_lhs);
                StaticType rhs = infer_type(binary->m_rhs);
                if (!lhs.is_numeric() || !rhs.is_numeric()) return {};
                if (lhs.m_literal && rhs.m_literal) return lhs.m_kind == Kind::Float ? lhs : rhs.m_kind == Kind::Float ? rhs : lhs;
                if (lhs.m_literal) return rhs;
                if (rhs.m_literal) return lhs;
                // Both typed: the operand the other converts to (the wider one); mismatches are reported by check_operands.
                return conversion_error(lhs, rhs).empty() ? lhs : rhs;
            }
            case TokenKind::EqualEqual:
            case TokenKind::ExclaimEqual:
            case TokenKind::Less:
            case TokenKind::LessEqual:
            case TokenKind::Greater:
            case TokenKind::GreaterEqual:
            case TokenKind::AmpAmp:
            case TokenKind::PipePipe: {
                StaticType type;
                type.m_kind = Kind::Bool;
                type.m_bits = 8;
                return type;
            }
            default: return {};
        }
    }
    if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        StaticType base = infer_type(member->m_object);
        while (base.is_view() && base.m_pointee) base = *base.m_pointee;
        if (base.m_kind != Kind::Named) return {};
        const ClassDeclNode* cls = m_pass0.find_class(base.m_name);
        if (cls == nullptr || !cls->m_type_params.empty()) return {};
        for (const ParameterDecl& field : cls->m_fields) {
            if (field.m_name == member->m_member) return static_type_of(field.m_type);
        }
        return {};
    }
    if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(expr)) return static_type_of(literal->m_type);
    if (const auto* lambda = dynamic_cast<const LambdaExpr*>(expr)) {
        StaticType function;
        function.m_kind = Kind::Function;
        for (const ParameterDecl& param : lambda->m_function->m_params) function.m_params.push_back(static_type_of(param.m_type));
        StaticType result;
        if (lambda->m_function->m_return_type == nullptr) result.m_kind = Kind::Void;
        else result = static_type_of(lambda->m_function->m_return_type);
        function.m_pointee = std::make_shared<StaticType>(std::move(result));
        return function;
    }
    if (const auto* intrinsic = dynamic_cast<const IntrinsicCallExpr*>(expr)) return reflection_result_type(intrinsic);
    if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        if (const auto* callee_id = dynamic_cast<const IdentifierExpr*>(call->m_callee); callee_id != nullptr && is_builtin_serialize(callee_id->m_identifier)) {
            StaticType text; // `serialize(value)` yields the text of the value, as a StringView
            text.m_kind = Kind::Named;
            text.m_name = StringInterner::get().intern("StringView");
            return text;
        }
        if (const auto* callee_id = dynamic_cast<const IdentifierExpr*>(call->m_callee); callee_id != nullptr && lookup(callee_id->m_identifier) == nullptr) {
            // not a variable: a named function (handled below)
        } else {
            const StaticType callee = infer_type(call->m_callee);
            if (callee.m_kind == Kind::Function && callee.m_pointee) {
                return callee.m_pointee->m_kind == Kind::Void ? StaticType{} : *callee.m_pointee;
            }
        }
        const FunctionDeclNode* fn = resolve_callee(call);
        if (fn == nullptr || !fn->m_type_params.empty() || fn->m_return_type == nullptr) return {};
        return static_type_of(fn->m_return_type);
    }
    return {};
}

std::optional<uint64_t> TypeCheckerPass2::type_alignment(const StaticType& type, unsigned depth) const {
    switch (type.m_kind) {
        case StaticType::Kind::Int:
        case StaticType::Kind::Float:
            if (type.m_bits == 0) return std::nullopt;
            return type.m_bits / 8;
        case StaticType::Kind::Bool: return 1;
        case StaticType::Kind::Pointer:
        case StaticType::Kind::Reference: return m_pass0.target().m_pointer_width / 8;
        case StaticType::Kind::Named: {
            const ClassDeclNode* cls = m_pass0.find_class(type.m_name);
            if (cls == nullptr || !cls->m_type_params.empty() || depth > 8) return std::nullopt;
            uint64_t alignment = 1;
            for (const ParameterDecl& field : cls->m_fields) {
                auto field_alignment = type_alignment(static_type_of(field.m_type), depth + 1);
                if (!field_alignment) return std::nullopt;
                alignment = std::max(alignment, *field_alignment);
            }
            return alignment;
        }
        default: return std::nullopt;
    }
}

/// Size in bytes with natural alignment (fields in declaration order, like a C struct).
std::optional<uint64_t> TypeCheckerPass2::type_size(const StaticType& type, unsigned depth) const {
    switch (type.m_kind) {
        case StaticType::Kind::Int:
        case StaticType::Kind::Float:
            if (type.m_bits == 0) return std::nullopt;
            return type.m_bits / 8;
        case StaticType::Kind::Bool: return 1;
        case StaticType::Kind::Pointer:
        case StaticType::Kind::Reference: return m_pass0.target().m_pointer_width / 8;
        case StaticType::Kind::Named: {
            const ClassDeclNode* cls = m_pass0.find_class(type.m_name);
            if (cls == nullptr || !cls->m_type_params.empty() || depth > 8) return std::nullopt;
            uint64_t offset = 0;
            uint64_t alignment = 1;
            for (const ParameterDecl& field : cls->m_fields) {
                StaticType field_type = static_type_of(field.m_type);
                auto size = type_size(field_type, depth + 1);
                auto field_alignment = type_alignment(field_type, depth + 1);
                if (!size || !field_alignment) return std::nullopt;
                offset = (offset + *field_alignment - 1) / *field_alignment * *field_alignment;
                offset += *size;
                alignment = std::max(alignment, *field_alignment);
            }
            return (offset + alignment - 1) / alignment * alignment;
        }
        default: return std::nullopt;
    }
}

std::string TypeCheckerPass2::describe(const StaticType& type) const {
    switch (type.m_kind) {
        case StaticType::Kind::Int: return type.m_literal ? "integer literal" : std::string(text_of(type.m_name));
        case StaticType::Kind::Float: return type.m_literal ? "floating-point literal" : std::string(text_of(type.m_name));
        case StaticType::Kind::Bool: return "bool";
        case StaticType::Kind::Void: return "void";
        case StaticType::Kind::Named: return std::string(text_of(type.m_name));
        case StaticType::Kind::Null: return "null";
        case StaticType::Kind::Function: {
            std::string text = "fn(";
            for (size_t i = 0; i < type.m_params.size(); ++i) text += (i ? ", " : "") + describe(type.m_params[i]);
            text += ")";
            if (type.m_pointee && type.m_pointee->m_kind != StaticType::Kind::Void) text += " -> " + describe(*type.m_pointee);
            return text;
        }
        case StaticType::Kind::Optional: return (type.m_pointee ? describe(*type.m_pointee) : "?") + "?";
        case StaticType::Kind::Pointer:
        case StaticType::Kind::Reference: {
            std::string text = type.m_kind == StaticType::Kind::Pointer ? "*" : "&";
            if (type.m_const) text = "const" + text + " ";
            return text + (type.m_pointee ? describe(*type.m_pointee) : "?");
        }
        default: return "<unknown>";
    }
}

// ---------------------------------------------------------------------------------------------
// Implicit conversions and operand rules
// ---------------------------------------------------------------------------------------------
//
// Only lossless conversions happen implicitly: an integer widens to a wider integer of the same signedness
// (or an unsigned one to a strictly wider signed one), `f32` widens to `f64`, a literal adapts to the type
// it is used as, `&T`/`*T` interconvert, and `const` may be added to a view. Everything else needs
// `@static_cast` / `@bit_cast` / ... . Types that are unknown (generics, foreign names, ...) are never
// reported: a missed error is acceptable, a wrong one is not.

bool TypeCheckerPass2::is_nominal(const StaticType& type) const {
    return type.m_kind == StaticType::Kind::Named &&
           (m_pass0.find_class(type.m_name) != nullptr || m_pass0.find_ruleset(type.m_name) != nullptr ||
            m_pass0.find_enum(type.m_name) != nullptr);
}

std::string TypeCheckerPass2::conversion_error(const StaticType& to, const StaticType& from) const {
    using Kind = StaticType::Kind;
    if (!to.known() || !from.known()) return {};
    if (to.m_kind == Kind::Named && !is_nominal(to)) return {};
    if (from.m_kind == Kind::Named && !is_nominal(from)) return {};

    const std::string base = "cannot convert '" + describe(from) + "' to '" + describe(to) + "'";
    const std::string numeric_hint = "; use @static_cast<" + describe(to) + ">(...) to convert explicitly";

    if (from.m_kind == Kind::Void) return to.m_kind == Kind::Void ? std::string{} : "a void value cannot be used as '" + describe(to) + "'";
    if (to.m_kind == Kind::Optional) {
        // `null` and any `U` that converts to `T` become a `T?`; `U?` converts to `T?` when `U` converts to `T`.
        if (from.m_kind == Kind::Null || !to.m_pointee) return {};
        if (from.m_kind == Kind::Optional) {
            if (!from.m_pointee || conversion_error(*to.m_pointee, *from.m_pointee).empty()) return {};
            return base;
        }
        return conversion_error(*to.m_pointee, from).empty() ? std::string{} : base;
    }
    if (to.m_kind == Kind::Function || from.m_kind == Kind::Function) {
        // A function value only converts to a function type with the same parameters and result.
        if (to.m_kind == Kind::Void) return {};
        return (to.m_kind == from.m_kind && same_type(to, from)) ? std::string{} : base;
    }
    if (from.m_kind == Kind::Null) {
        return to.m_kind == Kind::Void ? std::string{}
                                       : "cannot use 'null' as '" + describe(to) + "': only an optional type can hold null; declare it as '" + describe(to) + "?'";
    }
    if (from.m_kind == Kind::Optional && to.m_kind != Kind::Void) {
        return base + ": it may be null; check it first, e.g. 'if (x) { ... }'";
    }
    switch (to.m_kind) {
        case Kind::Void: return {};
        case Kind::Bool:
            if (from.m_kind == Kind::Bool) return {};
            return base + (from.is_numeric() ? "; compare explicitly, e.g. 'x != 0'" : "");
        case Kind::Int: {
            if (from.m_kind != Kind::Int) return base + (from.m_kind == Kind::Float || from.m_kind == Kind::Bool ? numeric_hint : "");
            if (from.m_literal || to.m_literal) return {};
            if (from.m_signed == to.m_signed && from.m_bits <= to.m_bits) return {};
            if (!from.m_signed && to.m_signed && from.m_bits < to.m_bits) return {};
            return base + numeric_hint;
        }
        case Kind::Float: {
            if (from.m_kind == Kind::Float) {
                if (from.m_literal || to.m_literal || to.m_bits >= from.m_bits) return {};
                return base + numeric_hint;
            }
            if (from.m_kind == Kind::Int && from.m_literal) return {};
            return base + (from.m_kind == Kind::Int || from.m_kind == Kind::Bool ? numeric_hint : "");
        }
        case Kind::Pointer:
        case Kind::Reference: {
            if (!from.is_view()) return base + (from.m_kind == Kind::Int ? "; use @reinterpret_cast to turn an address into a pointer" : "");
            if (from.m_const && !to.m_const) return base + " (discards const)";
            const StaticType* a = to.m_pointee.get();
            const StaticType* b = from.m_pointee.get();
            // Pointees are only compared when both are primitive: a named pointee may be a ruleset the other implements.
            const auto primitive = [](const StaticType* t) { return t != nullptr && t->known() && t->m_kind != Kind::Named && t->m_kind != Kind::Void; };
            if (primitive(a) && primitive(b) && !same_type(*a, *b)) return base + "; use @reinterpret_cast to change the pointee type";
            return {};
        }
        case Kind::Named: {
            if (from.m_kind != Kind::Named) return base;
            if (from.m_name == to.m_name) return {};
            // Distinct classes never convert; rulesets and enums may relate through conformance, so they are not judged.
            if (m_pass0.find_class(to.m_name) != nullptr && m_pass0.find_class(from.m_name) != nullptr) return base;
            return {};
        }
        default: return {};
    }
}

void TypeCheckerPass2::check_assignable(const StaticType& to, const ExprNode* value, std::string_view what) {
    if (value == nullptr || !to.known()) return;
    const std::string reason = conversion_error(to, infer_type(value));
    if (!reason.empty()) m_diag.error(value->m_span, reason + " in " + std::string(what));
}

void TypeCheckerPass2::check_condition(const ExprNode* condition, std::string_view what) {
    using Kind = StaticType::Kind;
    if (condition == nullptr) return;
    const StaticType type = infer_type(condition);
    if (!type.known() || type.m_kind == Kind::Bool || type.is_view() || type.is_optional() || (type.m_kind == Kind::Named && !is_nominal(type))) return;
    m_diag.error(condition->m_span, "the condition of '" + std::string(what) + "' must be 'bool', found '" + describe(type) +
                                    "'" + (type.is_numeric() ? "; compare explicitly, e.g. 'x != 0'" : ""));
}

void TypeCheckerPass2::check_operands(const BinaryExpr* binary) {
    using Kind = StaticType::Kind;
    const TokenKind op = binary->m_op;
    const bool arithmetic = op == TokenKind::Plus || op == TokenKind::Minus || op == TokenKind::Star ||
                            op == TokenKind::Slash || op == TokenKind::Percent;
    const bool equality = op == TokenKind::EqualEqual || op == TokenKind::ExclaimEqual;
    const bool ordering = op == TokenKind::Less || op == TokenKind::LessEqual || op == TokenKind::Greater ||
                          op == TokenKind::GreaterEqual;
    const bool logical = op == TokenKind::AmpAmp || op == TokenKind::PipePipe;
    if (!arithmetic && !equality && !ordering && !logical) return;

    const StaticType lhs = infer_type(binary->m_lhs);
    const StaticType rhs = infer_type(binary->m_rhs);
    if (!lhs.known() || !rhs.known()) return;

    // Optionals: only `x == null` / `x != null` (and truthiness in `&&` / `||`) are meaningful.
    if (lhs.m_kind == Kind::Null || rhs.m_kind == Kind::Null || lhs.is_optional() || rhs.is_optional()) {
        const auto text = [&] {
            switch (op) {
                case TokenKind::Plus: return "+"; case TokenKind::Minus: return "-"; case TokenKind::Star: return "*";
                case TokenKind::Slash: return "/"; case TokenKind::Percent: return "%"; case TokenKind::EqualEqual: return "==";
                case TokenKind::ExclaimEqual: return "!="; case TokenKind::Less: return "<"; case TokenKind::LessEqual: return "<=";
                case TokenKind::Greater: return ">"; case TokenKind::GreaterEqual: return ">="; case TokenKind::AmpAmp: return "&&";
                default: return "||";
            }
        }();
        if (logical) {
            for (const auto& [operand, node] : {std::pair{&lhs, binary->m_lhs}, std::pair{&rhs, binary->m_rhs}}) {
                if (operand->m_kind == Kind::Null) m_diag.error(node->m_span, "operand of '" + std::string(text) + "' cannot be 'null'");
            }
            return;
        }
        const bool lhs_null = lhs.m_kind == Kind::Null;
        const bool rhs_null = rhs.m_kind == Kind::Null;
        if (equality && (lhs_null != rhs_null)) {
            const StaticType& other = lhs_null ? rhs : lhs;
            if (!other.is_optional()) {
                m_diag.error(binary->m_span, "cannot compare '" + describe(other) + "' with 'null': it is not an optional type, so it is never null");
            }
            return;
        }
        if (equality && lhs_null && rhs_null) return;
        m_diag.error(binary->m_span, "operator '" + std::string(text) + "' cannot be applied to '" + describe(lhs) + "' and '" +
                                     describe(rhs) + "'" +
                                     (equality ? "; compare with 'null', or check the value with 'if (x)' and compare the plain value"
                                               : "; check the optional with 'if (x)' first"));
        return;
    }
    const auto judged = [&](const StaticType& t) { return t.m_kind == Kind::Int || t.m_kind == Kind::Float || t.m_kind == Kind::Bool; };
    if (!judged(lhs) || !judged(rhs)) return; // views, classes and the like: operators on them are not modelled

    const std::string symbol = [&] {
        switch (op) {
            case TokenKind::Plus: return "+"; case TokenKind::Minus: return "-"; case TokenKind::Star: return "*";
            case TokenKind::Slash: return "/"; case TokenKind::Percent: return "%"; case TokenKind::EqualEqual: return "==";
            case TokenKind::ExclaimEqual: return "!="; case TokenKind::Less: return "<"; case TokenKind::LessEqual: return "<=";
            case TokenKind::Greater: return ">"; case TokenKind::GreaterEqual: return ">="; case TokenKind::AmpAmp: return "&&";
            default: return "||";
        }
    }();

    if (logical) {
        for (const auto& [operand, node] : {std::pair{&lhs, binary->m_lhs}, std::pair{&rhs, binary->m_rhs}}) {
            if (operand->m_kind != Kind::Bool) {
                m_diag.error(node->m_span, "operand of '" + symbol + "' must be 'bool', found '" + describe(*operand) + "'");
            }
        }
        return;
    }
    if (lhs.m_kind == Kind::Bool || rhs.m_kind == Kind::Bool) {
        if (equality && lhs.m_kind == Kind::Bool && rhs.m_kind == Kind::Bool) return;
        m_diag.error(binary->m_span, "operator '" + symbol + "' cannot be applied to '" + describe(lhs) + "' and '" +
                                     describe(rhs) + "'");
        return;
    }
    // Both numeric: one side must convert to the other without loss (literals adapt).
    if (!conversion_error(lhs, rhs).empty() && !conversion_error(rhs, lhs).empty()) {
        m_diag.error(binary->m_span, "mismatched operand types for '" + symbol + "': '" + describe(lhs) + "' and '" +
                                     describe(rhs) + "'; convert one side with @static_cast");
    }
}

// ---------------------------------------------------------------------------------------------
// Named casts
// ---------------------------------------------------------------------------------------------

namespace {

bool int_value_fits(int64_t value, unsigned bits, bool is_signed) {
    if (bits >= 64) return is_signed || value >= 0;
    if (is_signed) return value >= -(int64_t{1} << (bits - 1)) && value <= (int64_t{1} << (bits - 1)) - 1;
    return value >= 0 && value <= (int64_t{1} << bits) - 1;
}

} // namespace

void TypeCheckerPass2::check_named_cast(const CastExpr* cast) {
    using Kind = StaticType::Kind;
    StaticType target = static_type_of(cast->m_target);
    const StaticType source = infer_type(cast->m_operand);
    const SourceSpan span = cast->m_span;
    const unsigned pointer_bits = m_pass0.target().m_pointer_width;

    const char* spelling = "@static_cast";
    switch (cast->m_cast_kind) {
        case CastKind::Dynamic: spelling = "@dynamic_cast"; break;
        case CastKind::Reinterpret: spelling = "@reinterpret_cast"; break;
        case CastKind::Bit: spelling = "@bit_cast"; break;
        default: break;
    }
    const std::string name = spelling;
    auto fail = [&](const std::string& message) { m_diag.error(span, message); };
    auto would_discard_const = [&] { return source.is_view() && source.m_const && target.is_view() && !target.m_const; };

    if (cast->m_target != nullptr && cast->m_target->m_kind == TypeNodeKind::Never) {
        fail(name + " can not target the never type '!'");
        return;
    }
    // `@dynamic_cast<*T?>(p)` states that the result may be null; every other cast works on plain values.
    if (target.is_optional()) {
        if (cast->m_cast_kind != CastKind::Dynamic) {
            fail(name + " can not produce the optional type '" + describe(target) + "'; cast to '" +
                 (target.m_pointee ? describe(*target.m_pointee) : std::string("T")) + "' and let it convert implicitly");
            return;
        }
        if (target.m_pointee) target = *target.m_pointee;
    }
    if (source.is_optional() || source.m_kind == Kind::Null) {
        fail(name + " can not convert '" + describe(source) + "'; " +
             (source.is_optional() ? "check it first with 'if (x)' so it is a plain value" : "'null' has no representation to cast"));
        return;
    }

    switch (cast->m_cast_kind) {
    // -----------------------------------------------------------------------------------------
    case CastKind::Static: {
        if (!target.known() || !source.known()) return;
        if (target.m_kind == Kind::Void) return; // explicitly discard a value
        if (target.m_kind == Kind::Bool && source.m_kind != Kind::Bool) {
            fail("cannot " + name + " '" + describe(source) + "' to 'bool'; compare explicitly, e.g. 'x != 0'");
            return;
        }
        const bool source_numeric = source.is_numeric() || source.m_kind == Kind::Bool;
        if (target.is_numeric() && source_numeric) {
            if (target.m_kind == Kind::Int && !target.m_literal) {
                // A constant operand must be representable: static_cast never truncates silently.
                const ConstTable constants = visible_constants();
                const ConstEvaluator evaluator(constants, m_diag, pointer_bits, m_namespace);
                if (auto value = evaluator.evaluate(cast->m_operand, /*report=*/false)) {
                    int64_t integer = value->m_kind == ConstValue::Kind::Float ? static_cast<int64_t>(value->m_float)
                                                                                : (value->m_kind == ConstValue::Kind::Bool ? (value->m_bool ? 1 : 0)
                                                                                                                           : value->m_int);
                    if (!int_value_fits(integer, target.m_bits, target.m_signed)) {
                        fail("constant " + std::to_string(integer) + " does not fit in '" + describe(target) + "' (" + name +
                             " never truncates constants; use '" + "as" + "' to wrap explicitly)");
                    }
                }
            }
            return;
        }
        if (source.is_view() && target.is_view()) {
            if (target.m_kind == Kind::Reference && source.m_kind == Kind::Pointer) {
                fail("cannot " + name + " a pointer to a reference; dereference it instead");
            } else if (would_discard_const()) {
                fail(name + " from '" + describe(source) + "' to '" + describe(target) + "' would discard 'const'");
            } else if (source.m_pointee && target.m_pointee && !same_type(*source.m_pointee, *target.m_pointee) &&
                       !is_pointee_void(source) && !is_pointee_void(target) && source.m_pointee->known() &&
                       target.m_pointee->known()) {
                fail("cannot " + name + " between unrelated pointer types '" + describe(source) + "' and '" +
                     describe(target) + "'; use @reinterpret_cast");
            }
            return;
        }
        if ((source.is_view() && target.m_kind == Kind::Int) || (source.m_kind == Kind::Int && target.is_view())) {
            fail(name + " does not convert between pointers and integers; use @reinterpret_cast");
            return;
        }
        if (source.m_kind == Kind::Named && target.m_kind == Kind::Named && same_type(source, target)) return;
        fail("cannot " + name + " '" + describe(source) + "' to '" + describe(target) + "'");
        return;
    }
    // -----------------------------------------------------------------------------------------
    case CastKind::Dynamic: {
        if (target.m_kind != Kind::Pointer) {
            if (target.known()) fail(name + " needs a pointer target type (the result is null when the cast fails), found '" + describe(target) + "'");
            return;
        }
        // The target must designate a concrete data type, not a primitive.
        if (target.m_pointee) {
            const StaticType& to = *target.m_pointee;
            if ((to.known() && to.m_kind != Kind::Named) ||
                (to.m_kind == Kind::Named && (m_pass0.find_ruleset(to.m_name) != nullptr || m_pass0.find_enum(to.m_name) != nullptr))) {
                fail(name + " target must point to a class or struct, found '" + describe(*target.m_pointee) + "'");
                return;
            }
        }
        if (!source.known()) return;
        const StaticType* from = source.is_view() ? source.m_pointee.get() : nullptr;
        const bool is_ruleset_view = from != nullptr && from->m_kind == Kind::Named && m_pass0.find_ruleset(from->m_name) != nullptr;
        const bool known_not_ruleset = from == nullptr || (from->known() && !(from->m_kind == Kind::Named && m_pass0.find_ruleset(from->m_name) == nullptr && m_pass0.find_class(from->m_name) == nullptr));
        if (!is_ruleset_view && known_not_ruleset) {
            fail(name + " needs a view of a ruleset (&Ruleset / *Ruleset) as operand, found '" + describe(source) + "'; use @static_cast for statically known types");
            return;
        }
        if (would_discard_const()) fail(name + " would discard 'const'");
        return;
    }
    // -----------------------------------------------------------------------------------------
    case CastKind::Reinterpret: {
        if (!target.known() || !source.known()) return;
        auto pointer_sized = [&](const StaticType& t) { return t.m_kind == Kind::Int && (t.m_literal || t.m_bits == pointer_bits); };
        if (target.m_kind == Kind::Reference) {
            fail(name + " can not produce a reference; cast to a pointer and dereference it");
            return;
        }
        if (source.is_view() && target.m_kind == Kind::Pointer) {
            if (would_discard_const()) fail(name + " from '" + describe(source) + "' to '" + describe(target) + "' would discard 'const'");
            return;
        }
        if (source.is_view() && target.m_kind == Kind::Int) {
            if (!pointer_sized(target)) {
                fail(name + " from a pointer needs a pointer-sized integer (usize or isize); '" + describe(target) + "' has " +
                     std::to_string(target.m_bits) + " bits but pointers have " + std::to_string(pointer_bits) + " on this target");
            }
            return;
        }
        if (source.m_kind == Kind::Int && target.m_kind == Kind::Pointer) {
            if (!pointer_sized(source)) {
                fail(name + " to a pointer needs a pointer-sized integer (usize or isize); '" + describe(source) + "' has " +
                     std::to_string(source.m_bits) + " bits but pointers have " + std::to_string(pointer_bits) + " on this target");
            }
            return;
        }
        fail(name + " only converts between pointers and pointer-sized integers ('" + describe(source) + "' to '" +
             describe(target) + "'); use @static_cast for numeric conversions or @bit_cast to reinterpret the bits of a value");
        return;
    }
    // -----------------------------------------------------------------------------------------
    case CastKind::Bit: {
        if (target.m_kind == Kind::Reference || source.m_kind == Kind::Reference) {
            fail(name + " can not convert references; use @reinterpret_cast on a pointer");
            return;
        }
        if (!target.known() || !source.known() || source.m_literal) return;
        const auto target_size = type_size(target);
        const auto source_size = type_size(source);
        if (!target_size || !source_size) {
            const StaticType& unsized = !target_size ? target : source;
            fail(name + " needs types of known size; the size of '" + describe(unsized) + "' is not known at compile time");
            return;
        }
        if (*target_size != *source_size) {
            fail(name + " requires types of the same size: '" + describe(source) + "' is " + std::to_string(*source_size) +
                 " byte(s) but '" + describe(target) + "' is " + std::to_string(*target_size) + " byte(s)");
        }
        return;
    }
    default: return;
    }
}

} // namespace fool
