//
// Created by jmbab on 06-11-2026.
//
/**
 * @file TypeChecker.cpp
 * @brief Implementation unit of Pass 2 Semantic Analysis & Type Checking.
 */

module;

#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>

module fool.sema;

namespace fool {

TypeCheckerPass2::TypeCheckerPass2(SymbolTablePass0& pass0)
    : m_pass0(pass0) {}

void TypeCheckerPass2::report_error(std::string_view msg, SourceSpan span) {
    m_error_count++;
    std::string diag = "Semantic Error at line " + std::to_string(span.m_start.m_line) + ":" +
                       std::to_string(span.m_start.m_column) + " - " + std::string(msg);
    m_diagnostics.push_back(diag);
    std::cerr << diag << "\n";
}

bool TypeCheckerPass2::evaluate_compile_time_condition(std::string_view cond) {
    const auto& defs = m_pass0.get_definitions();

    // Check for def(FLAG)
    size_t def_pos = cond.find("def(");
    if (def_pos != std::string::npos) {
        size_t close_pos = cond.find(")", def_pos);
        if (close_pos != std::string::npos) {
            std::string param_name = std::string(cond.substr(def_pos + 4, close_pos - (def_pos + 4)));
            param_name.erase(0, param_name.find_first_not_of(" \t"));
            param_name.erase(param_name.find_last_not_of(" \t") + 1);

            auto it = defs.find(param_name);
            if (it == defs.end()) {
                return false;
            }

            // Check if there is an equality check, e.g. && (PARAM == 1)
            size_t eq_pos = cond.find("==", close_pos);
            if (eq_pos != std::string::npos) {
                size_t val_start = cond.find_first_not_of(" \t", eq_pos + 2);
                size_t val_end = cond.find_first_of(" \t)", val_start);
                std::string exp_val_str = std::string(cond.substr(val_start, val_end - val_start));
                int64_t expected_val = std::stoll(exp_val_str);
                return it->second == expected_val;
            }
            return true;
        }
    }
    return true;
}

bool TypeCheckerPass2::check_var_decl(VarDeclNode* var) {
    if (!var) return false;

    Type type(TypeKind::Int32, var->m_type_name, var->m_kind == BindingKind::Immutable);
    SymbolBinding binding(var->m_name, type, var->m_kind);
    m_bindings[var->m_name] = binding;
    return true;
}

bool TypeCheckerPass2::check_statement(StmtNode* stmt, std::vector<StmtNode*>& out_pruned_stmts) {
    if (!stmt) return true;

    // 1. Compile-Time Conditional Branch (@if / @elif / @else) inside function
    if (auto* cond_stmt = dynamic_cast<ConditionalStmt*>(stmt)) {
        bool branch_taken = false;
        for (auto& branch : cond_stmt->m_branches) {
            if (branch_taken) break;

            // Empty condition string indicates '@else'
            bool cond_result = branch.m_condition_text.empty() ? true : evaluate_compile_time_condition(branch.m_condition_text);

            if (cond_result) {
                branch_taken = true;
                // Active compile-time branch: keep statements
                for (auto* inner_stmt : branch.m_body) {
                    check_statement(inner_stmt, out_pruned_stmts);
                }
            }
            // Inactive branches are silently pruned (zero runtime cost)
        }
        return true;
    }

    // 2. Declaration Statement
    if (auto* decl_stmt = dynamic_cast<DeclStmt*>(stmt)) {
        if (auto* var = dynamic_cast<VarDeclNode*>(decl_stmt->m_decl)) {
            check_var_decl(var);
        }
        out_pruned_stmts.push_back(stmt);
        return true;
    }

    // 3. Return Statement
    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt)) {
        out_pruned_stmts.push_back(stmt);
        return true;
    }

    // 4. Expression Statement (Assignments & Invocations)
    if (auto* expr_stmt = dynamic_cast<ExprStmt*>(stmt)) {
        if (auto* bin = dynamic_cast<BinaryExpr*>(expr_stmt->m_expr)) {
            if (bin->m_op == TokenKind::Equal) {
                // In-place reassignment: lhs = rhs;
                if (auto* id_expr = dynamic_cast<IdentifierExpr*>(bin->m_lhs)) {
                    auto it = m_bindings.find(id_expr->m_identifier);
                    if (it != m_bindings.end()) {
                        // Invariant Check: Transitive Immutability
                        if (it->second.m_binding_kind == BindingKind::Immutable) {
                            auto& interner = StringInterner::get();
                            report_error("Cannot reassign immutable binding '" +
                                         std::string(interner.resolve(id_expr->m_identifier)) +
                                         "' (declared with 'const')", bin->m_span);
                            return false;
                        }
                        // Invariant Check: Destructive Move
                        if (it->second.m_is_moved) {
                            auto& interner = StringInterner::get();
                            report_error("Use of invalidated moved value '" +
                                         std::string(interner.resolve(id_expr->m_identifier)) + "'", bin->m_span);
                            return false;
                        }
                    }
                }
            }
        }
        out_pruned_stmts.push_back(stmt);
        return true;
    }

    out_pruned_stmts.push_back(stmt);
    return true;
}

bool TypeCheckerPass2::check_function(FunctionDeclNode* fn) {
    if (!fn || !fn->m_is_active) return true;

    // Reset local scope bindings for the function
    m_bindings.clear();

    // Register receiver if present
    if (fn->m_receiver.m_kind != ReceiverKind::None) {
        bool is_const_receiver = (fn->m_receiver.m_kind == ReceiverKind::ByConstRef);
        Type recv_type(TypeKind::Reference, is_const_receiver);
        BindingKind bkind = is_const_receiver ? BindingKind::Immutable : BindingKind::Mutable;
        m_bindings[fn->m_receiver.m_self_name] = SymbolBinding(fn->m_receiver.m_self_name, recv_type, bkind);
    }

    // Register parameters
    for (const auto& param : fn->m_params) {
        Type ptype(TypeKind::Int32);
        m_bindings[param.m_name] = SymbolBinding(param.m_name, ptype, BindingKind::Immutable);
    }

    // Check statements and prune inactive @if branches
    std::vector<StmtNode*> active_stmts;
    for (auto* stmt : fn->m_body) {
        check_statement(stmt, active_stmts);
    }
    fn->m_body = std::move(active_stmts);

    return m_error_count == 0;
}

bool TypeCheckerPass2::check_module(ModuleAST* module) {
    if (!module) return false;

    // Reset diagnostics
    m_error_count = 0;
    m_diagnostics.clear();

    for (DeclNode* decl : module->m_declarations) {
        if (!decl->m_is_active) continue;

        if (auto* fn = dynamic_cast<FunctionDeclNode*>(decl)) {
            check_function(fn);
        } else if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
            for (auto* method : cls->m_methods) {
                check_function(method);
            }
        }
    }

    return m_error_count == 0;
}

size_t TypeCheckerPass2::get_error_count() const {
    return m_error_count;
}

const std::vector<std::string>& TypeCheckerPass2::get_diagnostics() const {
    return m_diagnostics;
}

} // namespace fool
