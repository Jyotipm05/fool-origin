//
// Created by jmbab on 06-11-2026.
//

/**
 * @file BorrowChecker.cpp
 * @brief Implementation unit of the opt-in borrow checker (see BorrowChecker.ixx for the model).
 *
 * The function body is traversed twice with the same walker. The Collect pass numbers every
 * identifier use and records the last use of each name (extended to the end of an enclosing loop),
 * the Check pass replays the traversal with identical numbering and reports conflicts between the
 * borrows that are live at each event.
 */

module;

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module fool.borrow;

namespace fool {

namespace {

struct AddressOf {
    const ExprNode* m_operand{nullptr}; ///< Operand of the `&`, nullptr when the expression is not `&place`
    BorrowKind m_kind{BorrowKind::Exclusive};
};

/// Matches `&place`, optionally wrapped in casts; a cast to `const*` / `const&` makes it a shared borrow.
AddressOf match_address_of(const ExprNode* expr) {
    bool shared = false;
    while (const auto* cast = dynamic_cast<const CastExpr*>(expr)) {
        if (cast->m_target != nullptr && cast->m_target->is_const_view()) shared = true;
        expr = cast->m_operand;
    }
    const auto* unary = dynamic_cast<const UnaryExpr*>(expr);
    if (unary == nullptr || unary->m_op != TokenKind::Ampersand || unary->m_operand == nullptr) return {};
    return AddressOf{unary->m_operand, shared ? BorrowKind::Shared : BorrowKind::Exclusive};
}

/// Two places overlap when one is a prefix of the other (`x` overlaps `x.a`, `x.a` does not overlap `x.b`).
bool overlaps(const std::vector<SymbolID>& a, const std::vector<SymbolID>& b) {
    size_t common = std::min(a.size(), b.size());
    for (size_t i = 0; i < common; ++i) {
        if (a[i] != b[i]) return false;
    }
    return true;
}

SymbolID nominal_name(const TypeNode* type) {
    while (type != nullptr && type->m_kind != TypeNodeKind::Path) type = type->m_inner;
    return (type == nullptr || type->m_path.empty()) ? INVALID_SYMBOL_ID : type->m_path.back();
}

bool is_view_type(const TypeNode* type) { return type != nullptr && type->is_view(); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------

std::string BorrowCheckerPass::name_of(SymbolID id) const {
    std::string name(StringInterner::get().resolve(id));
    if (size_t mark = name.find('#'); mark != std::string::npos) name.resize(mark); // strip the declaration suffix
    return name;
}

/// Gives a declaration its own identity, so shadowing declarations never share liveness or borrows.
/// The numbering is deterministic, hence identical in the Collect and Check passes.
SymbolID BorrowCheckerPass::declare(SymbolID name) {
    SymbolID unique = StringInterner::get().intern(name_of(name) + "#" + std::to_string(++m_decl_counter));
    m_scopes.back()[name] = unique;
    return unique;
}

/// Innermost visible declaration of `name`; names that were never declared locally (globals, functions) map to themselves.
SymbolID BorrowCheckerPass::resolve(SymbolID name) const {
    for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
        auto found = scope->find(name);
        if (found != scope->end()) return found->second;
    }
    return name;
}

/// Pure variant of walk_path: the place an lvalue expression designates, empty if it has none.
std::vector<SymbolID> BorrowCheckerPass::place_of(const ExprNode* expr) const {
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) return {resolve(id->m_identifier)};
    if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        std::vector<SymbolID> place = place_of(member->m_object);
        if (!place.empty()) place.push_back(member->m_member);
        return place;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) return place_of(index->m_base);
    return {};
}

std::string BorrowCheckerPass::place_text(const std::vector<SymbolID>& place) const {
    std::string text;
    for (size_t i = 0; i < place.size(); ++i) {
        if (i != 0) text += ".";
        text += name_of(place[i]);
    }
    return text;
}

bool BorrowCheckerPass::is_live(const BorrowRecord& record) const {
    if (record.m_borrower == INVALID_SYMBOL_ID) return true; // call temporary, removed at the end of the call
    auto it = m_last_use.find(record.m_borrower);
    return it != m_last_use.end() && it->second >= m_pos;
}

void BorrowCheckerPass::prune() {
    m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
                                  [this](const BorrowRecord& record) { return !is_live(record); }),
                   m_active.end());
}

void BorrowCheckerPass::tick_use(SymbolID name) {
    ++m_pos;
    if (m_mode == Mode::Collect) m_last_use[name] = m_pos;
    else prune();
}

bool BorrowCheckerPass::check_borrow(const std::vector<SymbolID>& place, BorrowKind kind, SourceSpan span) {
    if (m_mode != Mode::Check) return true;
    for (const BorrowRecord& existing : m_active) {
        if (!overlaps(existing.m_place, place)) continue;
        if (kind == BorrowKind::Shared && existing.m_kind == BorrowKind::Shared) continue;

        std::string what = "'" + place_text(place) + "'";
        if (kind == BorrowKind::Exclusive && existing.m_kind == BorrowKind::Exclusive) {
            m_diag.error(span, "cannot borrow " + what + " as mutable more than once at a time");
        } else if (kind == BorrowKind::Exclusive) {
            m_diag.error(span, "cannot borrow " + what + " as mutable because it is also borrowed as immutable");
        } else {
            m_diag.error(span, "cannot borrow " + what + " as immutable because it is also borrowed as mutable");
        }
        m_diag.note(existing.m_span, "previous borrow of '" + place_text(existing.m_place) + "' occurs here");
        return false;
    }
    return true;
}

void BorrowCheckerPass::add_borrow(SymbolID borrower, std::vector<SymbolID> place, BorrowKind kind, SourceSpan span,
                                   size_t call_id) {
    m_active.push_back(BorrowRecord{borrower, std::move(place), kind, span, call_id});
}

void BorrowCheckerPass::report_read(const std::vector<SymbolID>& place, SourceSpan span) {
    if (m_mode != Mode::Check) return;
    for (const BorrowRecord& existing : m_active) {
        if (existing.m_kind != BorrowKind::Exclusive || !overlaps(existing.m_place, place)) continue;
        std::string by = existing.m_borrower == INVALID_SYMBOL_ID ? "in this call"
                                                                   : "by '" + name_of(existing.m_borrower) + "'";
        m_diag.error(span, "cannot use '" + place_text(place) + "' because it is mutably borrowed " + by);
        m_diag.note(existing.m_span, "mutable borrow occurs here");
        return;
    }
}

void BorrowCheckerPass::report_write(const std::vector<SymbolID>& place, SourceSpan span) {
    if (m_mode != Mode::Check) return;
    for (const BorrowRecord& existing : m_active) {
        if (!overlaps(existing.m_place, place)) continue;
        std::string by = existing.m_borrower == INVALID_SYMBOL_ID ? "in this call"
                                                                   : "by '" + name_of(existing.m_borrower) + "'";
        m_diag.error(span, "cannot assign to '" + place_text(place) + "' because it is borrowed " + by);
        m_diag.note(existing.m_span, "borrow occurs here");
        return;
    }
}

const ConditionalBranch* BorrowCheckerPass::select_branch(const ConditionalStmt* stmt) const {
    ConstEvaluator evaluator(m_symbols.definitions(), m_diag);
    for (const ConditionalBranch& branch : stmt->m_branches) {
        if (branch.m_condition == nullptr) return &branch;
        auto value = evaluator.evaluate(branch.m_condition, /*report=*/false);
        if (!value) return nullptr;
        if (value->truthy()) return &branch;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Traversal
// ---------------------------------------------------------------------------------------------

/// Walks an lvalue chain (`x`, `x.a`, `x[i].b`), numbering its identifier use and visiting index
/// expressions. Returns false when the chain is not rooted at a plain identifier.
bool BorrowCheckerPass::walk_path(const ExprNode* expr, std::vector<SymbolID>& place) {
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        SymbolID variable = resolve(id->m_identifier);
        tick_use(variable);
        place = {variable};
        return true;
    }
    if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        if (!walk_path(member->m_object, place)) return false;
        place.push_back(member->m_member);
        return true;
    }
    if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        bool rooted = walk_path(index->m_base, place);
        walk_expr(index->m_index);
        return rooted;
    }
    walk_expr(expr);
    return false;
}

void BorrowCheckerPass::walk_expr(const ExprNode* expr) {
    if (expr == nullptr) return;

    if (dynamic_cast<const IdentifierExpr*>(expr) != nullptr || dynamic_cast<const MemberExpr*>(expr) != nullptr ||
        dynamic_cast<const IndexExpr*>(expr) != nullptr) {
        std::vector<SymbolID> place;
        if (walk_path(expr, place)) report_read(place, expr->m_span);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        walk_call(call);
    } else if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        if (binary->m_op == TokenKind::Equal) {
            walk_assignment(binary);
        } else {
            walk_expr(binary->m_lhs);
            walk_expr(binary->m_rhs);
        }
    } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        if (unary->m_op == TokenKind::Ampersand) {
            // A free-standing `&place` is not tracked beyond this expression, but it must still be legal.
            std::vector<SymbolID> place;
            if (walk_path(unary->m_operand, place)) check_borrow(place, BorrowKind::Exclusive, expr->m_span);
        } else {
            walk_expr(unary->m_operand);
        }
    } else if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) {
        walk_expr(cast->m_operand);
    } else if (const auto* intrinsic = dynamic_cast<const IntrinsicCallExpr*>(expr)) {
        for (const ExprNode* arg : intrinsic->m_args) walk_expr(arg);
    } else if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(expr)) {
        for (const FieldInit& field : literal->m_fields) walk_expr(field.m_value);
    }
    // literals and qualified paths (module::function, Enum::Variant) hold no local variables
}

void BorrowCheckerPass::walk_assignment(const BinaryExpr* assign) {
    walk_expr(assign->m_rhs);
    std::vector<SymbolID> place;
    if (walk_path(assign->m_lhs, place)) report_write(place, assign->m_span);
}

void BorrowCheckerPass::walk_call(const CallExpr* call) {
    const size_t call_id = ++m_call_counter;

    // Method call: a `this &self` receiver borrows the object exclusively for the duration of the call.
    const auto* member = dynamic_cast<const MemberExpr*>(call->m_callee);
    const auto* object = member != nullptr ? dynamic_cast<const IdentifierExpr*>(member->m_object) : nullptr;
    const FunctionDeclNode* method = nullptr;
    if (object != nullptr) {
        auto cls = m_class_of.find(resolve(object->m_identifier));
        if (cls != m_class_of.end()) method = m_symbols.find_method(cls->second, member->m_member);
    }
    if (method != nullptr && method->m_receiver.m_kind == ReceiverKind::ByMutRef) {
        std::vector<SymbolID> place;
        walk_path(member->m_object, place);
        if (m_mode == Mode::Check && check_borrow(place, BorrowKind::Exclusive, call->m_span)) {
            add_borrow(INVALID_SYMBOL_ID, place, BorrowKind::Exclusive, call->m_span, call_id);
        }
    } else {
        walk_expr(call->m_callee);
    }

    // Arguments of the form `&place` borrow for the call; all of them are registered before any
    // argument is evaluated, so aliasing between arguments is caught regardless of their order.
    std::vector<AddressOf> refs;
    refs.reserve(call->m_args.size());
    for (const CallArg& arg : call->m_args) refs.push_back(match_address_of(arg.m_expr));
    if (m_mode == Mode::Check) {
        for (size_t i = 0; i < call->m_args.size(); ++i) {
            if (refs[i].m_operand == nullptr) continue;
            std::vector<SymbolID> place = place_of(refs[i].m_operand);
            if (place.empty()) continue;
            SourceSpan span = call->m_args[i].m_expr->m_span;
            if (check_borrow(place, refs[i].m_kind, span)) {
                add_borrow(INVALID_SYMBOL_ID, std::move(place), refs[i].m_kind, span, call_id);
            }
        }
    }
    for (size_t i = 0; i < call->m_args.size(); ++i) {
        std::vector<SymbolID> place;
        if (refs[i].m_operand != nullptr) walk_path(refs[i].m_operand, place);
        else walk_expr(call->m_args[i].m_expr);
    }

    m_active.erase(std::remove_if(m_active.begin(), m_active.end(),
                                  [call_id](const BorrowRecord& record) { return record.m_call_id == call_id; }),
                   m_active.end());
}

void BorrowCheckerPass::walk_var_decl(const VarDeclNode* var) {
    AddressOf ref = match_address_of(var->m_initializer);
    std::vector<SymbolID> place;
    bool tracked = false;
    if (ref.m_operand != nullptr) {
        tracked = walk_path(ref.m_operand, place);
    } else {
        walk_expr(var->m_initializer);
    }

    // The new name comes into scope after its initializer, which therefore still sees any outer variable of that name.
    const SymbolID variable = declare(var->m_name);
    if (m_mode == Mode::Collect) m_decl_pos[variable] = m_pos;

    // Class of the new binding, so later `x.method()` calls can be resolved.
    SymbolID cls = nominal_name(var->m_type);
    if (cls == INVALID_SYMBOL_ID) {
        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(var->m_initializer)) cls = nominal_name(literal->m_type);
    }
    if (cls != INVALID_SYMBOL_ID) m_class_of[variable] = cls;

    if (tracked) {
        BorrowKind kind = ref.m_kind;
        if (var->m_type != nullptr && var->m_type->is_const_view()) kind = BorrowKind::Shared;
        m_origin_of[variable] = place.front();
        if (check_borrow(place, kind, var->m_span)) add_borrow(variable, place, kind, var->m_span, 0);
        return;
    }

    // Known-owned values may not be returned by reference.
    bool owned = (var->m_type != nullptr && !is_view_type(var->m_type)) ||
                 (var->m_type == nullptr && (dynamic_cast<const StructLiteralExpr*>(var->m_initializer) != nullptr ||
                                             dynamic_cast<const IntegerLiteralExpr*>(var->m_initializer) != nullptr ||
                                             dynamic_cast<const FloatLiteralExpr*>(var->m_initializer) != nullptr ||
                                             dynamic_cast<const BooleanLiteralExpr*>(var->m_initializer) != nullptr));
    if (owned) m_locals.insert(variable);
}

void BorrowCheckerPass::walk_return(const ReturnStmt* ret) {
    if (ret->m_value == nullptr) return;
    AddressOf ref = match_address_of(ret->m_value);
    if (ref.m_operand != nullptr) {
        std::vector<SymbolID> place;
        if (walk_path(ref.m_operand, place)) {
            check_borrow(place, ref.m_kind, ret->m_span);
            if (m_mode == Mode::Check && m_locals.contains(place.front())) {
                m_diag.error(ret->m_span, "cannot return a reference to local variable '" + name_of(place.front()) +
                                          "': it does not live long enough");
            }
        }
        return;
    }
    walk_expr(ret->m_value);
    if (m_mode == Mode::Check) {
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(ret->m_value)) {
            auto origin = m_origin_of.find(resolve(id->m_identifier));
            if (origin != m_origin_of.end() && m_locals.contains(origin->second)) {
                m_diag.error(ret->m_span, "cannot return '" + name_of(id->m_identifier) + "': it borrows local variable '" +
                                          name_of(origin->second) + "' which does not live long enough");
            }
        }
    }
}

void BorrowCheckerPass::walk_block(const BlockStmt* block) {
    if (block == nullptr) return;
    push_scope();
    walk_statements(block->m_statements);
    pop_scope();
}

void BorrowCheckerPass::walk_loop(const ExprNode* condition, const ExprNode* iterable, SymbolID variable,
                                  const BlockStmt* body) {
    walk_expr(iterable);
    const size_t start = m_pos;
    push_scope(); // the loop variable lives in a scope around the body
    if (variable != INVALID_SYMBOL_ID) {
        SymbolID loop_variable = declare(variable);
        if (m_mode == Mode::Collect) m_decl_pos[loop_variable] = start + 1;
    }
    walk_expr(condition);
    walk_block(body);
    pop_scope();
    if (m_mode == Mode::Collect) {
        // A name declared before the loop and used inside it is live for the whole loop.
        for (auto& [name, last] : m_last_use) {
            auto decl = m_decl_pos.find(name);
            if (last > start && decl != m_decl_pos.end() && decl->second <= start) last = m_pos;
        }
    }
}

void BorrowCheckerPass::walk_statement(const StmtNode* stmt) {
    if (stmt == nullptr) return;

    if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        walk_return(ret);
    } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
        walk_expr(expr_stmt->m_expr);
    } else if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
        if (const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl)) walk_var_decl(var);
    } else if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        walk_block(block);
    } else if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
        walk_expr(if_stmt->m_condition);
        walk_block(if_stmt->m_then);
        walk_statement(if_stmt->m_else);
    } else if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
        walk_loop(while_stmt->m_condition, nullptr, INVALID_SYMBOL_ID, while_stmt->m_body);
    } else if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
        walk_loop(nullptr, for_stmt->m_iterable, for_stmt->m_variable, for_stmt->m_body);
    } else if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
        if (const ConditionalBranch* branch = select_branch(conditional)) {
            push_scope();
            walk_statements(branch->m_body);
            pop_scope();
        }
    }
}

void BorrowCheckerPass::walk_statements(const std::vector<StmtNode*>& stmts) {
    for (const StmtNode* stmt : stmts) walk_statement(stmt);
}

// ---------------------------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------------------------

void BorrowCheckerPass::run_pass(Mode mode) {
    m_mode = mode;
    m_pos = 0;
    m_decl_counter = 0;
    m_call_counter = 0;
    m_active.clear();
    m_scopes.clear();
    push_scope();

    // Parameters and the receiver share the function's outermost scope with its top-level statements.
    for (const ParameterDecl& param : m_fn->m_params) {
        SymbolID variable = declare(param.m_name);
        m_decl_pos[variable] = 0;
        if (param.m_type != nullptr && !is_view_type(param.m_type)) m_locals.insert(variable);
        if (SymbolID cls = nominal_name(param.m_type); cls != INVALID_SYMBOL_ID) m_class_of[variable] = cls;
    }
    if (m_fn->m_receiver.m_kind != ReceiverKind::None) {
        SymbolID self = declare(m_fn->m_receiver.m_self_name);
        m_decl_pos[self] = 0;
        if (m_owner != INVALID_SYMBOL_ID) m_class_of[self] = m_owner;
        if (m_fn->m_receiver.m_kind == ReceiverKind::ByValue) m_locals.insert(self);
    }

    walk_statements(m_fn->m_body);
    m_scopes.clear();
}

bool BorrowCheckerPass::check_function(const FunctionDeclNode* fn, SymbolID owner) {
    if (fn == nullptr || !fn->m_has_body) return true;
    const size_t errors_before = m_diag.error_count();

    m_fn = fn;
    m_owner = owner != INVALID_SYMBOL_ID ? owner : fn->m_owner;
    m_last_use.clear();
    m_decl_pos.clear();
    m_locals.clear();
    m_class_of.clear();
    m_origin_of.clear();
    m_call_counter = 0;

    run_pass(Mode::Collect);
    run_pass(Mode::Check);
    return m_diag.error_count() == errors_before;
}

bool BorrowCheckerPass::check_module() {
    const size_t errors_before = m_diag.error_count();
    auto use_file = [this](const DeclNode* decl) {
        if (!decl->m_file.empty()) m_diag.set_current_file(std::string(decl->m_file));
    };

    for (const DeclNode* decl : m_symbols.active_declarations()) {
        use_file(decl);
        if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
            check_function(fn, fn->m_owner);
        } else if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
            for (const FunctionDeclNode* method : cls->m_methods) check_function(method, cls->m_name);
        }
    }
    return m_diag.error_count() == errors_before;
}

} // namespace fool
