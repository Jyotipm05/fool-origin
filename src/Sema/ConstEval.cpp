//
// Created by jmbab on 07-11-2026.
//

/**
 * @file ConstEval.cpp
 * @brief Compile-time expression evaluation shared by @if pruning, module parameters and constx.
 */

module;

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module fool.sema;

namespace fool {

std::string ConstValue::to_string() const {
    switch (m_kind) {
        case Kind::Int: return std::to_string(m_int);
        case Kind::Float: return std::to_string(m_float);
        case Kind::Bool: return m_bool ? "true" : "false";
    }
    return "?";
}

std::optional<ConstValue> ConstEvaluator::fail(const ExprNode* expr, bool report, std::string message) const {
    if (report && expr != nullptr) m_diag.error(expr->m_span, std::move(message));
    return std::nullopt;
}

std::optional<ConstValue> ConstEvaluator::evaluate(const ExprNode* expr, bool report) const {
    if (expr == nullptr) return std::nullopt;

    if (const auto* lit = dynamic_cast<const IntegerLiteralExpr*>(expr)) return ConstValue::from_int(lit->m_value);
    if (const auto* lit = dynamic_cast<const FloatLiteralExpr*>(expr)) return ConstValue::from_float(lit->m_value);
    if (const auto* lit = dynamic_cast<const BooleanLiteralExpr*>(expr)) return ConstValue::from_bool(lit->m_value);

    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        auto it = m_table.find(id->m_identifier);
        if (it != m_table.end()) return it->second;
        return fail(expr, report, "'" + std::string(StringInterner::get().resolve(id->m_identifier)) +
                                  "' is not a known compile-time constant");
    }
    if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) return eval_cast(cast, report);
    if (const auto* un = dynamic_cast<const UnaryExpr*>(expr)) return eval_unary(un, report);
    if (const auto* bin = dynamic_cast<const BinaryExpr*>(expr)) return eval_binary(bin, report);

    if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        const auto* callee = dynamic_cast<const IdentifierExpr*>(call->m_callee);
        if (callee != nullptr && StringInterner::get().resolve(callee->m_identifier) == "def") {
            if (call->m_args.size() != 1 || call->m_args[0].m_label != INVALID_SYMBOL_ID) {
                return fail(expr, report, "def(...) takes exactly one name");
            }
            const auto* name = dynamic_cast<const IdentifierExpr*>(call->m_args[0].m_expr);
            if (name == nullptr) return fail(expr, report, "def(...) expects an identifier");
            return ConstValue::from_bool(m_table.find(name->m_identifier) != m_table.end());
        }
    }
    return fail(expr, report, "expression is not a compile-time constant");
}

namespace {

struct ScalarType {
    enum class Kind { None, Int, Float, Bool } m_kind{Kind::None};
    unsigned m_bits{0};
    bool m_signed{false};
};

ScalarType scalar_type_of(const TypeNode* type, unsigned pointer_width) {
    using Kind = ScalarType::Kind;
    if (type == nullptr || type->m_kind != TypeNodeKind::Path || type->m_path.size() != 1 || !type->m_generic_args.empty()) return {};
    const std::string_view name = StringInterner::get().resolve(type->m_path.front());
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
    return {};
}

bool fits(int64_t value, const ScalarType& type) {
    if (type.m_bits >= 64) return type.m_signed || value >= 0;
    if (type.m_signed) return value >= -(int64_t{1} << (type.m_bits - 1)) && value <= (int64_t{1} << (type.m_bits - 1)) - 1;
    return value >= 0 && value <= (int64_t{1} << type.m_bits) - 1;
}

/// Two's complement truncation of `value` to the width of `type`, sign-extended for signed types.
int64_t wrap_to(int64_t value, const ScalarType& type) {
    if (type.m_bits >= 64) return value;
    const uint64_t mask = (uint64_t{1} << type.m_bits) - 1;
    uint64_t bits = static_cast<uint64_t>(value) & mask;
    if (type.m_signed && (bits >> (type.m_bits - 1)) != 0) bits |= ~mask;
    return static_cast<int64_t>(bits);
}

} // namespace

/// Folds `as`, `@static_cast` and `@bit_cast` between scalar types. Pointer casts are never constant.
std::optional<ConstValue> ConstEvaluator::eval_cast(const CastExpr* cast, bool report) const {
    using Kind = ScalarType::Kind;
    const ScalarType target = scalar_type_of(cast->m_target, m_pointer_width);
    if (target.m_kind == Kind::None || cast->m_cast_kind == CastKind::Dynamic || cast->m_cast_kind == CastKind::Reinterpret) {
        return fail(cast, report, "this cast is not allowed in a compile-time constant");
    }
    auto operand = evaluate(cast->m_operand, report);
    if (!operand) return std::nullopt;

    if (cast->m_cast_kind == CastKind::Bit) {
        // The operand's own width is not tracked; the target's width decides how the bits are read.
        if (target.m_kind == Kind::Int && operand->m_kind == ConstValue::Kind::Float) {
            if (target.m_bits == 32) {
                float narrow = static_cast<float>(operand->m_float);
                uint32_t bits = 0;
                std::memcpy(&bits, &narrow, sizeof bits);
                return ConstValue::from_int(wrap_to(static_cast<int64_t>(bits), target));
            }
            if (target.m_bits == 64) {
                uint64_t bits = 0;
                std::memcpy(&bits, &operand->m_float, sizeof bits);
                if (!target.m_signed && bits > static_cast<uint64_t>(INT64_MAX)) {
                    return fail(cast, report, "constant does not fit in a 64-bit unsigned compile-time integer");
                }
                return ConstValue::from_int(static_cast<int64_t>(bits));
            }
        } else if (target.m_kind == Kind::Float && operand->m_kind == ConstValue::Kind::Int) {
            if (target.m_bits == 32) {
                uint32_t bits = static_cast<uint32_t>(static_cast<uint64_t>(operand->m_int));
                float wide = 0;
                std::memcpy(&wide, &bits, sizeof bits);
                return ConstValue::from_float(static_cast<double>(wide));
            }
            double wide = 0;
            std::memcpy(&wide, &operand->m_int, sizeof wide);
            return ConstValue::from_float(wide);
        } else if (target.m_kind == Kind::Int && operand->m_kind != ConstValue::Kind::Float) {
            const int64_t source = operand->as_int();
            if (!target.m_signed && target.m_bits >= 64 && source < 0) {
                return fail(cast, report, "constant does not fit in a 64-bit unsigned compile-time integer");
            }
            return ConstValue::from_int(wrap_to(source, target));
        } else if (target.m_kind == Kind::Bool && operand->m_kind == ConstValue::Kind::Bool) {
            return operand;
        }
        return fail(cast, report, "@bit_cast between these constant types is not supported");
    }

    const bool wrap = cast->m_cast_kind == CastKind::Unchecked; // `as` wraps, @static_cast must be exact
    switch (target.m_kind) {
        case Kind::Int: {
            int64_t integer = 0;
            if (operand->m_kind == ConstValue::Kind::Float) {
                if (!std::isfinite(operand->m_float) || std::fabs(operand->m_float) >= 9.2e18) {
                    return fail(cast, report, "floating-point constant is out of range for an integer");
                }
                integer = static_cast<int64_t>(operand->m_float);
            } else {
                integer = operand->as_int();
            }
            if (wrap) return ConstValue::from_int(wrap_to(integer, target));
            if (!fits(integer, target)) {
                return fail(cast, report, "constant " + std::to_string(integer) + " does not fit in the target type of @static_cast");
            }
            return ConstValue::from_int(integer);
        }
        case Kind::Float: {
            double real = operand->as_double();
            if (target.m_bits == 32) real = static_cast<double>(static_cast<float>(real));
            return ConstValue::from_float(real);
        }
        case Kind::Bool:
            if (!wrap) return fail(cast, report, "@static_cast to bool is not allowed; compare explicitly");
            return ConstValue::from_bool(operand->truthy());
        default: break;
    }
    return fail(cast, report, "this cast is not allowed in a compile-time constant");
}

std::optional<ConstValue> ConstEvaluator::eval_unary(const UnaryExpr* expr, bool report) const {
    if (expr->m_op != TokenKind::Bang && expr->m_op != TokenKind::Minus) {
        return fail(expr, report, "operator is not allowed in a compile-time constant");
    }
    auto operand = evaluate(expr->m_operand, report);
    if (!operand) return std::nullopt;
    if (expr->m_op == TokenKind::Bang) return ConstValue::from_bool(!operand->truthy());
    if (operand->m_kind == ConstValue::Kind::Float) return ConstValue::from_float(-operand->m_float);
    int64_t negated = 0;
    if (__builtin_sub_overflow(int64_t{0}, operand->as_int(), &negated)) {
        return fail(expr, report, "integer overflow in constant expression");
    }
    return ConstValue::from_int(negated);
}

std::optional<ConstValue> ConstEvaluator::eval_binary(const BinaryExpr* expr, bool report) const {
    // Short-circuit operators evaluate lazily so `def(X) && X == 1` is valid when X is undefined.
    if (expr->m_op == TokenKind::AmpAmp || expr->m_op == TokenKind::PipePipe) {
        auto lhs = evaluate(expr->m_lhs, report);
        if (!lhs) return std::nullopt;
        bool is_and = (expr->m_op == TokenKind::AmpAmp);
        if (is_and && !lhs->truthy()) return ConstValue::from_bool(false);
        if (!is_and && lhs->truthy()) return ConstValue::from_bool(true);
        auto rhs = evaluate(expr->m_rhs, report);
        if (!rhs) return std::nullopt;
        return ConstValue::from_bool(rhs->truthy());
    }

    auto lhs = evaluate(expr->m_lhs, report);
    auto rhs = evaluate(expr->m_rhs, report);
    if (!lhs || !rhs) return std::nullopt;

    bool use_float = lhs->m_kind == ConstValue::Kind::Float || rhs->m_kind == ConstValue::Kind::Float;
    switch (expr->m_op) {
        case TokenKind::EqualEqual:
            return ConstValue::from_bool(use_float ? lhs->as_double() == rhs->as_double() : lhs->as_int() == rhs->as_int());
        case TokenKind::ExclaimEqual:
            return ConstValue::from_bool(use_float ? lhs->as_double() != rhs->as_double() : lhs->as_int() != rhs->as_int());
        case TokenKind::Less:
            return ConstValue::from_bool(use_float ? lhs->as_double() < rhs->as_double() : lhs->as_int() < rhs->as_int());
        case TokenKind::LessEqual:
            return ConstValue::from_bool(use_float ? lhs->as_double() <= rhs->as_double() : lhs->as_int() <= rhs->as_int());
        case TokenKind::Greater:
            return ConstValue::from_bool(use_float ? lhs->as_double() > rhs->as_double() : lhs->as_int() > rhs->as_int());
        case TokenKind::GreaterEqual:
            return ConstValue::from_bool(use_float ? lhs->as_double() >= rhs->as_double() : lhs->as_int() >= rhs->as_int());
        default:
            break;
    }

    if (use_float) {
        double a = lhs->as_double();
        double b = rhs->as_double();
        switch (expr->m_op) {
            case TokenKind::Plus: return ConstValue::from_float(a + b);
            case TokenKind::Minus: return ConstValue::from_float(a - b);
            case TokenKind::Star: return ConstValue::from_float(a * b);
            case TokenKind::Slash:
                if (b == 0.0) return fail(expr, report, "division by zero in constant expression");
                return ConstValue::from_float(a / b);
            default:
                return fail(expr, report, "operator is not defined for floating-point constants");
        }
    }

    int64_t a = lhs->as_int();
    int64_t b = rhs->as_int();
    int64_t out = 0;
    switch (expr->m_op) {
        case TokenKind::Plus:
            if (__builtin_add_overflow(a, b, &out)) return fail(expr, report, "integer overflow in constant expression");
            return ConstValue::from_int(out);
        case TokenKind::Minus:
            if (__builtin_sub_overflow(a, b, &out)) return fail(expr, report, "integer overflow in constant expression");
            return ConstValue::from_int(out);
        case TokenKind::Star:
            if (__builtin_mul_overflow(a, b, &out)) return fail(expr, report, "integer overflow in constant expression");
            return ConstValue::from_int(out);
        case TokenKind::Slash:
        case TokenKind::Percent:
            if (b == 0) return fail(expr, report, "division by zero in constant expression");
            if (a == INT64_MIN && b == -1) return fail(expr, report, "integer overflow in constant expression");
            return ConstValue::from_int(expr->m_op == TokenKind::Slash ? a / b : a % b);
        case TokenKind::Ampersand: return ConstValue::from_int(a & b);
        case TokenKind::Pipe: return ConstValue::from_int(a | b);
        case TokenKind::Caret: return ConstValue::from_int(a ^ b);
        default:
            return fail(expr, report, "operator is not allowed in a compile-time constant");
    }
}

} // namespace fool
