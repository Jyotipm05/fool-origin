//
// Created by jmbab on 17-11-2026.
//

/**
 * @file TypeCheckerOptional.cpp
 * @brief Pass 2 (part 4): optional types `T?` and `null`, and the flow analysis that narrows an optional
 *        variable to a plain `T` where the program has checked it.
 *
 * A variable of type `T?` may only be used as a `T` where the compiler can see that it holds a value:
 *
 *     Entity? a = find();
 *     if (a) { a.update(); }                  // narrowed in the `then` branch
 *     if (a == null) { return; }
 *     a.update();                             // narrowed for the rest of the block (early exit)
 *     while (a != null && a.alive) { ... }    // `&&` narrows its right operand, loops narrow their body
 *
 * Narrowing is per local variable and per scope; an assignment, or taking its address, forgets it.
 * Parameters are locals too. Fields, globals and call results are never narrowed: bind them to a local first.
 */

module;

#include <cstddef>
#include <set>
#include <string>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string name_of(SymbolID id) { return std::string(StringInterner::get().resolve(id)); }

const IdentifierExpr* as_identifier(const ExprNode* expr) { return dynamic_cast<const IdentifierExpr*>(expr); }
bool is_null_literal(const ExprNode* expr) { return dynamic_cast<const NullLiteralExpr*>(expr) != nullptr; }

} // namespace

// ---------------------------------------------------------------------------------------------
// Narrowing bookkeeping
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::collect_narrowing(const ExprNode* condition, bool when_true, std::vector<SymbolID>& out) {
    if (condition == nullptr) return;
    if (const auto* id = as_identifier(condition)) {
        // `if (a)` holds a value when true; nothing is known about `a` when it is false.
        const SymbolBinding* binding = lookup(id->m_identifier);
        if (when_true && binding != nullptr && binding->m_static_type.is_optional()) out.push_back(id->m_identifier);
        return;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(condition)) {
        if (unary->m_op == TokenKind::Bang) collect_narrowing(unary->m_operand, !when_true, out);
        return;
    }
    const auto* binary = dynamic_cast<const BinaryExpr*>(condition);
    if (binary == nullptr) return;
    switch (binary->m_op) {
        case TokenKind::AmpAmp:
            if (when_true) {
                collect_narrowing(binary->m_lhs, true, out);
                collect_narrowing(binary->m_rhs, true, out);
            }
            return;
        case TokenKind::PipePipe:
            if (!when_true) {
                collect_narrowing(binary->m_lhs, false, out);
                collect_narrowing(binary->m_rhs, false, out);
            }
            return;
        case TokenKind::EqualEqual:
        case TokenKind::ExclaimEqual: {
            const ExprNode* subject = is_null_literal(binary->m_rhs) ? binary->m_lhs : is_null_literal(binary->m_lhs) ? binary->m_rhs : nullptr;
            const auto* id = as_identifier(subject);
            if (id == nullptr) return;
            const bool holds_value = (binary->m_op == TokenKind::ExclaimEqual) == when_true; // `a != null` true, `a == null` false
            const SymbolBinding* binding = lookup(id->m_identifier);
            if (holds_value && binding != nullptr && binding->m_static_type.is_optional()) out.push_back(id->m_identifier);
            return;
        }
        default: return;
    }
}

void TypeCheckerPass2::narrow(const std::vector<SymbolID>& names) {
    for (const SymbolID name : names) {
        SymbolBinding* binding = lookup(name);
        if (binding == nullptr || binding->m_narrowed) continue;
        binding->m_narrowed = true;
        m_narrow_log.push_back(name);
    }
}

void TypeCheckerPass2::unnarrow_to(size_t log_size) {
    while (m_narrow_log.size() > log_size) {
        if (SymbolBinding* binding = lookup(m_narrow_log.back())) binding->m_narrowed = false;
        m_narrow_log.pop_back();
    }
}

void TypeCheckerPass2::kill_narrowing(SymbolID name) {
    if (SymbolBinding* binding = lookup(name)) binding->m_narrowed = false;
}

// A loop body may run any number of times, so a variable it assigns (or whose address it takes) is not known to
// hold a value at the top of the body even if it did before the loop. Walk the body and forget those.
void TypeCheckerPass2::kill_assigned_in(const ExprNode* expr) {
    if (expr == nullptr) return;
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        if (binary->m_op == TokenKind::Equal) {
            if (const auto* id = as_identifier(binary->m_lhs)) kill_narrowing(id->m_identifier);
        }
        kill_assigned_in(binary->m_lhs);
        kill_assigned_in(binary->m_rhs);
    } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        if (unary->m_op == TokenKind::Ampersand) {
            if (const auto* id = as_identifier(unary->m_operand)) kill_narrowing(id->m_identifier);
        }
        kill_assigned_in(unary->m_operand);
    } else if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) {
        kill_assigned_in(cast->m_operand);
    } else if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        kill_assigned_in(member->m_object);
    } else if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        kill_assigned_in(index->m_base);
        kill_assigned_in(index->m_index);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        kill_assigned_in(call->m_callee);
        for (const CallArg& arg : call->m_args) kill_assigned_in(arg.m_expr);
    } else if (const auto* intrinsic = dynamic_cast<const IntrinsicCallExpr*>(expr)) {
        for (const ExprNode* arg : intrinsic->m_args) kill_assigned_in(arg);
    } else if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(expr)) {
        for (const FieldInit& field : literal->m_fields) kill_assigned_in(field.m_value);
    }
}

void TypeCheckerPass2::kill_assigned_in(const StmtNode* stmt) {
    if (stmt == nullptr) return;
    if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        kill_assigned_in(ret->m_value);
    } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
        kill_assigned_in(expr_stmt->m_expr);
    } else if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
        if (const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl)) kill_assigned_in(var->m_initializer);
    } else if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        for (const StmtNode* inner : block->m_statements) kill_assigned_in(inner);
    } else if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
        kill_assigned_in(if_stmt->m_condition);
        kill_assigned_in(if_stmt->m_then);
        kill_assigned_in(if_stmt->m_else);
    } else if (const auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        kill_assigned_in(match->m_subject);
        for (const MatchArm& arm : match->m_arms) kill_assigned_in(arm.m_body);
    } else if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
        kill_assigned_in(while_stmt->m_condition);
        kill_assigned_in(while_stmt->m_body);
    } else if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
        kill_assigned_in(for_stmt->m_init);
        kill_assigned_in(for_stmt->m_condition);
        kill_assigned_in(for_stmt->m_step);
        kill_assigned_in(for_stmt->m_body);
    } else if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
        for (const ConditionalBranch& branch : conditional->m_branches) {
            for (const StmtNode* inner : branch.m_body) kill_assigned_in(inner);
        }
    }
}

bool TypeCheckerPass2::stmt_always_exits(const StmtNode* stmt) const {
    if (stmt == nullptr) return false;
    if (dynamic_cast<const BreakStmt*>(stmt) != nullptr || dynamic_cast<const ContinueStmt*>(stmt) != nullptr) return true;
    if (stmt_always_returns(stmt)) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        for (const StmtNode* inner : block->m_statements) {
            if (stmt_always_exits(inner)) return true;
        }
        return false;
    }
    if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
        return if_stmt->m_else != nullptr && stmt_always_exits(if_stmt->m_then) && stmt_always_exits(if_stmt->m_else);
    }
    if (const auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        if (match->m_arms.empty()) return false;
        for (const MatchArm& arm : match->m_arms) {
            if (!stmt_always_exits(arm.m_body)) return false;
        }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Use of an optional value
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::require_non_null(const ExprNode* object, SourceSpan span) {
    if (object == nullptr) return;
    const StaticType type = infer_type(object);
    if (!type.is_optional()) return;
    if (const auto* id = as_identifier(object)) {
        m_diag.error(span, "'" + name_of(id->m_identifier) + "' may be null: check it first with 'if (" + name_of(id->m_identifier) + ")'");
    } else {
        m_diag.error(span, "a value of type '" + describe(type) + "' may be null: store it in a variable and check it first with 'if (...)'");
    }
}

} // namespace fool
