//
// Created by jmbab on 07-11-2026.
//

/**
 * @file ConstEval.cpp
 * @brief Compile-time expression evaluation shared by @if pruning, module parameters and constx.
 */

module;

#include <cstdint>
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
