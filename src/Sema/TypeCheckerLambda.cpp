//
// Created by jmbab on 17-11-2026.
//

/**
 * @file TypeCheckerLambda.cpp
 * @brief Pass 2 (part 5): lambdas, function values and explicit captures.
 *
 *     twice = (x: i32) -> i32 { return x * 2; };          // a lambda; `twice(3)` and `twice(x = 3)` both work
 *     run(capture=[counter, log]) { counter = counter + 1; }   // trailing block passed as the last argument
 *
 * A lambda body is checked as if it were a function of its own. The only names it can see besides its parameters
 * and locals are the variables listed in its capture list and program-wide functions, types and constants.
 * Using any other variable of the enclosing code, or any mutable global, is an error, so what a block like
 * `thread_task(capture=[a, b]) { ... }` can touch is exactly what its capture list says. Captured variables are
 * shared with the enclosing code (captured by reference); they keep their type, constness and moved state.
 * The guarantee covers names written inside the body: what a called function does with other globals is not tracked.
 */

module;

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string name_of(SymbolID id) { return std::string(StringInterner::get().resolve(id)); }

} // namespace

bool TypeCheckerPass2::is_enclosing_variable(SymbolID name) const {
    for (const auto& scopes : m_enclosing_scopes) {
        for (const auto& scope : scopes) {
            for (const SymbolBinding& binding : scope) {
                if (binding.m_name == name) return true;
            }
        }
    }
    return false;
}

bool TypeCheckerPass2::report_uncaptured(SymbolID name, SourceSpan span) {
    if (m_enclosing_scopes.empty()) return false;
    if (lookup(name) != nullptr) return false;
    const std::string text = name_of(name);
    if (is_enclosing_variable(name)) {
        m_diag.error(span, "'" + text + "' belongs to the enclosing code and is not captured by this lambda: add it to the capture list, e.g. capture=[" + text + "]");
        return true;
    }
    if (const VarDeclNode* global = m_pass0.find_global(name); global != nullptr && global->m_kind == BindingKind::Mutable) {
        m_diag.error(span, "global variable '" + text + "' is not captured by this lambda: add it to the capture list, e.g. capture=[" + text + "]");
        return true;
    }
    return false;
}

void TypeCheckerPass2::check_lambda(const LambdaExpr* lambda) {
    const FunctionDeclNode* fn = lambda->m_function;
    if (fn == nullptr) return;

    // 1. Resolve the capture list in the enclosing code, before its scopes are set aside.
    std::vector<SymbolBinding> captured;
    std::set<SymbolID> seen;
    for (const CaptureName& capture : lambda->m_captures) {
        const std::string text = name_of(capture.m_name);
        if (!seen.insert(capture.m_name).second) {
            m_diag.error(capture.m_span, "'" + text + "' is captured more than once");
            continue;
        }
        if (const SymbolBinding* outer = lookup(capture.m_name)) {
            if (outer->m_is_moved) m_diag.error(capture.m_span, "use of moved value '" + text + "'");
            SymbolBinding copy = *outer;
            copy.m_narrowed = false; // the enclosing code may change it while the lambda lives
            captured.push_back(std::move(copy));
        } else if (const VarDeclNode* global = m_pass0.find_global(capture.m_name)) {
            captured.push_back(make_binding(capture.m_name, global->m_type, global->m_kind, capture.m_span));
        } else {
            m_diag.error(capture.m_span, "cannot capture '" + text + "': there is no variable of that name here");
        }
    }

    // 2. Check the body like a function of its own, with only the captures visible.
    m_enclosing_scopes.push_back(std::move(m_scopes));
    m_scopes.clear();
    std::vector<SymbolID> saved_log = std::move(m_narrow_log);
    std::vector<size_t> saved_marks = std::move(m_narrow_marks);
    m_narrow_log.clear();
    m_narrow_marks.clear();
    const FunctionDeclNode* saved_fn = m_current_fn;
    const ClassDeclNode* saved_owner = m_current_owner;
    const int saved_loops = m_loop_depth;
    m_current_fn = fn;
    m_current_owner = nullptr;
    m_loop_depth = 0;

    push_scope();
    for (SymbolBinding& binding : captured) m_scopes.back().push_back(std::move(binding));
    push_scope();

    for (const ParameterDecl& param : fn->m_params) {
        if (lookup_in_current_scope(param.m_name) != nullptr) {
            m_diag.error(param.m_span, "duplicate parameter name '" + name_of(param.m_name) + "'");
            continue;
        }
        declare(make_binding(param.m_name, param.m_type, BindingKind::Mutable, param.m_span));
    }
    check_statements(fn->m_body);
    if (fn->m_return_type != nullptr && fn->m_return_type->m_kind != TypeNodeKind::Never && !always_returns(fn->m_body)) {
        m_diag.error(fn->m_span, "lambda does not return a value on all paths");
    }
    pop_scope();
    pop_scope();

    m_loop_depth = saved_loops;
    m_current_owner = saved_owner;
    m_current_fn = saved_fn;
    m_narrow_log = std::move(saved_log);
    m_narrow_marks = std::move(saved_marks);
    m_scopes = std::move(m_enclosing_scopes.back());
    m_enclosing_scopes.pop_back();
}

void TypeCheckerPass2::check_function_value_call(const CallExpr* call, const StaticType& callee, const LambdaExpr* lambda,
                                                 const std::string& display) {
    // The variable still holds the lambda it was initialized with: its parameter names, defaults and
    // labels work exactly like a function's.
    if (lambda != nullptr && lambda->m_function != nullptr) {
        bind_arguments(lambda->m_function, call, display, nullptr);
        return;
    }
    const std::string name = display.empty() ? "function value" : "'" + display + "'";
    bool labelled = false;
    for (const CallArg& arg : call->m_args) {
        if (arg.m_label == INVALID_SYMBOL_ID) continue;
        labelled = true;
        m_diag.error(arg.m_span, "argument '" + name_of(arg.m_label) + "' cannot be named: the parameter names of a function value are not part of its type '" +
                                 describe(callee) + "'");
    }
    if (labelled) return;
    if (call->m_args.size() != callee.m_params.size()) {
        m_diag.error(call->m_span, name + " of type '" + describe(callee) + "' takes " + std::to_string(callee.m_params.size()) +
                                   " argument(s) but " + std::to_string(call->m_args.size()) + " were given");
        return;
    }
    for (size_t i = 0; i < call->m_args.size(); ++i) {
        check_assignable(callee.m_params[i], call->m_args[i].m_expr, "argument " + std::to_string(i + 1) + " of " + name);
    }
}

} // namespace fool
