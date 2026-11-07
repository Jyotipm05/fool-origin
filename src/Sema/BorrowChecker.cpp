//
// Created by jmbab on 06-11-2026.
//
/**
 * @file BorrowChecker.cpp
 * @brief Implementation unit of the Opt-in Borrow Checker pass.
 */

module;

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>

module fool.borrow;

namespace fool {

void BorrowCheckerPass::report_borrow_error(std::string_view msg, SourceSpan span) {
    m_error_count++;
    std::ostringstream ss;
    ss << "Borrow Check Error at line " << span.m_start.m_line << ":" << span.m_start.m_column
       << " - " << msg;
    m_diagnostics.push_back(ss.str());
    std::cerr << ss.str() << "\n";
}

void BorrowCheckerPass::record_borrow(SymbolID borrower, SymbolID origin, BorrowKind kind, SourceSpan span) {
    auto& interner = StringInterner::get();
    std::string_view origin_name = interner.resolve(origin);

    for (const auto& existing : m_active_borrows) {
        if (existing.m_origin == origin) {
            if (kind == BorrowKind::Exclusive) {
                if (existing.m_kind == BorrowKind::Exclusive) {
                    report_borrow_error(std::string("Cannot borrow '") + std::string(origin_name) + "' as mutable more than once at a time", span);
                    return;
                } else {
                    report_borrow_error(std::string("Cannot borrow '") + std::string(origin_name) + "' as mutable because it is also borrowed as immutable", span);
                    return;
                }
            } else {
                if (existing.m_kind == BorrowKind::Exclusive) {
                    report_borrow_error(std::string("Cannot borrow '") + std::string(origin_name) + "' as immutable because it is also borrowed as mutable", span);
                    return;
                }
            }
        }
    }

    m_active_borrows.emplace_back(borrower, origin, kind, span, m_statement_index, 0);
}

void BorrowCheckerPass::validate_mutation(SymbolID origin, SourceSpan span) {
    auto& interner = StringInterner::get();
    std::string_view origin_name = interner.resolve(origin);

    for (const auto& existing : m_active_borrows) {
        if (existing.m_origin == origin) {
            report_borrow_error(std::string("Cannot mutate '") + std::string(origin_name) + "' while active borrow exists", span);
            return;
        }
    }
}

bool BorrowCheckerPass::check_function(const FunctionDeclNode* fn) {
    if (!fn || !fn->m_is_active) return true;

    m_active_borrows.clear();
    m_statement_index = 0;
    std::unordered_set<SymbolID> local_variables;

    for (const auto* stmt : fn->m_body) {
        m_statement_index++;

        if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
            if (const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl)) {
                local_variables.insert(var->m_name);

                // Check if variable is initialized with a borrow: e.g. ref = &orig or ref: const& = &orig
                if (var->m_initializer) {
                    if (const auto* unary = dynamic_cast<const UnaryExpr*>(var->m_initializer)) {
                        if (unary->m_op == TokenKind::Ampersand) {
                            if (const auto* ident = dynamic_cast<const IdentifierExpr*>(unary->m_operand)) {
                                SymbolID origin = ident->m_identifier;
                                bool is_const_ref = false;
                                if (var->m_type_name != INVALID_SYMBOL_ID) {
                                    auto& interner = StringInterner::get();
                                    std::string_view tname = interner.resolve(var->m_type_name);
                                    if (tname.find("const&") != std::string_view::npos) {
                                        is_const_ref = true;
                                    }
                                }
                                BorrowKind kind = is_const_ref ? BorrowKind::Shared : BorrowKind::Exclusive;
                                record_borrow(var->m_name, origin, kind, var->m_span);
                            }
                        }
                    }
                }
            }
        } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
            if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr_stmt->m_expr)) {
                if (binary->m_op == TokenKind::Equal) {
                    if (const auto* lhs = dynamic_cast<const IdentifierExpr*>(binary->m_lhs)) {
                        validate_mutation(lhs->m_identifier, expr_stmt->m_span);
                    }
                }
            }
        } else if (const auto* ret_stmt = dynamic_cast<const ReturnStmt*>(stmt)) {
            if (ret_stmt->m_value) {
                if (const auto* unary = dynamic_cast<const UnaryExpr*>(ret_stmt->m_value)) {
                    if (unary->m_op == TokenKind::Ampersand) {
                        if (const auto* ident = dynamic_cast<const IdentifierExpr*>(unary->m_operand)) {
                            if (local_variables.contains(ident->m_identifier)) {
                                auto& interner = StringInterner::get();
                                std::string msg = "Return of borrowed value referencing local variable '" +
                                                  std::string(interner.resolve(ident->m_identifier)) +
                                                  "' that does not live long enough";
                                report_borrow_error(msg, ret_stmt->m_span);
                            }
                        }
                    }
                }
            }
        }
    }

    return (m_error_count == 0);
}

bool BorrowCheckerPass::check_module(const ModuleAST* module) {
    if (!module) return true;

    for (const auto* decl : module->m_declarations) {
        if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
            check_function(fn);
        } else if (const auto* cb = dynamic_cast<const ConditionalDeclBlock*>(decl)) {
            for (const auto& branch : cb->m_branches) {
                for (const auto* bdecl : branch.m_declarations) {
                    if (const auto* fn2 = dynamic_cast<const FunctionDeclNode*>(bdecl)) {
                        check_function(fn2);
                    }
                }
            }
        }
    }

    return (m_error_count == 0);
}

} // namespace fool
