//
// Created by jmbab on 06-11-2026.
//

/**
 * @file TypeChecker.cpp
 * @brief Pass 2 (part 1): declaration consistency, scopes, function bodies and statements.
 */

module;

#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }
std::string name_of(SymbolID id) { return std::string(text_of(id)); }

/// Name of the class/struct a type designates, looking through references and pointers.
SymbolID nominal_name(const TypeNode* type) {
    if (type == nullptr) return INVALID_SYMBOL_ID;
    switch (type->m_kind) {
        case TypeNodeKind::Path:
            return type->m_path.empty() ? INVALID_SYMBOL_ID : type->m_path.back();
        case TypeNodeKind::Pointer:
        case TypeNodeKind::Reference:
            return nominal_name(type->m_inner);
        case TypeNodeKind::Never:
            return INVALID_SYMBOL_ID;
    }
    return INVALID_SYMBOL_ID;
}

std::string receiver_text(ReceiverKind kind) {
    switch (kind) {
        case ReceiverKind::None: return "";
        case ReceiverKind::ByValue: return "this self";
        case ReceiverKind::ByConstRef: return "this const& self";
        case ReceiverKind::ByMutRef: return "this &self";
    }
    return "";
}

/// Signature text used to compare an interface declaration with its definition.
std::string signature_of(const FunctionDeclNode* fn) {
    std::string sig = fn->m_is_async ? "async " : "";
    sig += receiver_text(fn->m_receiver.m_kind);
    sig += "(";
    for (size_t i = 0; i < fn->m_params.size(); ++i) {
        if (i != 0 || fn->m_receiver.m_kind != ReceiverKind::None) sig += ", ";
        sig += name_of(fn->m_params[i].m_name) + ": " + type_to_string(fn->m_params[i].m_type);
    }
    sig += ") -> " + type_to_string(fn->m_return_type);
    return sig;
}

std::string qualified_name(const FunctionDeclNode* fn) {
    if (fn->m_owner == INVALID_SYMBOL_ID) return name_of(fn->m_name);
    return name_of(fn->m_owner) + "::" + name_of(fn->m_name);
}

bool has_attribute(const DeclNode* decl, std::string_view name) {
    for (const AttributeNode& attr : decl->m_attributes) {
        if (text_of(attr.m_name) == name) return true;
    }
    return false;
}

void use_file(DiagnosticEngine& diag, const DeclNode* decl) {
    if (decl != nullptr && !decl->m_file.empty()) diag.set_current_file(std::string(decl->m_file));
}

} // namespace

TypeCheckerPass2::TypeCheckerPass2(SymbolTablePass0& pass0, DiagnosticEngine& diag)
    : m_pass0(pass0), m_diag(diag) {}

// ---------------------------------------------------------------------------------------------
// Scopes
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::push_scope() { m_scopes.emplace_back(); }
void TypeCheckerPass2::pop_scope() { m_scopes.pop_back(); }

SymbolBinding* TypeCheckerPass2::lookup(SymbolID name) {
    for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
        for (auto binding = scope->rbegin(); binding != scope->rend(); ++binding) {
            if (binding->m_name == name) return &*binding;
        }
    }
    return nullptr;
}

SymbolBinding* TypeCheckerPass2::lookup_in_current_scope(SymbolID name) {
    if (m_scopes.empty()) return nullptr;
    for (auto& binding : m_scopes.back()) {
        if (binding.m_name == name) return &binding;
    }
    return nullptr;
}

SymbolBinding* TypeCheckerPass2::declare(SymbolBinding binding) {
    if (m_scopes.empty()) push_scope();
    m_scopes.back().push_back(std::move(binding));
    return &m_scopes.back().back();
}

SymbolBinding TypeCheckerPass2::make_binding(SymbolID name, const TypeNode* type, BindingKind kind, SourceSpan span) const {
    SymbolBinding binding;
    binding.m_name = name;
    binding.m_binding_kind = kind;
    binding.m_span = span;
    if (type != nullptr) {
        binding.m_is_const_view = type->is_const_view();
        binding.m_is_pointer = type->m_kind == TypeNodeKind::Pointer;
        binding.m_is_view = type->is_view();
        binding.m_type_name = nominal_name(type);
    }
    return binding;
}

ClassDeclNode* TypeCheckerPass2::class_of_binding(const SymbolBinding& binding) const {
    if (binding.m_type_name == INVALID_SYMBOL_ID) return nullptr;
    return m_pass0.find_class(binding.m_type_name);
}

ConstTable TypeCheckerPass2::visible_constants() const {
    ConstTable table = m_pass0.definitions();
    for (const auto& [name, value] : m_local_constants) table[name] = value;
    return table;
}

bool TypeCheckerPass2::is_generic_parameter(SymbolID name) const {
    if (m_current_fn != nullptr) {
        for (const GenericParam& param : m_current_fn->m_type_params) {
            if (param.m_name == name) return true;
        }
        for (const GenericArg& arg : m_current_fn->m_owner_args) {
            if (arg.m_type != nullptr && arg.m_type->m_kind == TypeNodeKind::Path &&
                arg.m_type->m_path.size() == 1 && arg.m_type->m_path[0] == name) return true;
        }
    }
    if (m_current_owner != nullptr) {
        for (const GenericParam& param : m_current_owner->m_type_params) {
            if (param.m_name == name) return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// Constant / literal checks
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::check_const_fits_type(const TypeNode* type, const ConstValue& value, SourceSpan span,
                                             std::string_view what) {
    if (type == nullptr || type->m_kind != TypeNodeKind::Path || type->m_path.size() != 1) return;
    std::string_view name = text_of(type->m_path[0]);

    int bits = 0;
    bool is_signed = false;
    if (name == "i8") { bits = 8; is_signed = true; }
    else if (name == "i16") { bits = 16; is_signed = true; }
    else if (name == "i32") { bits = 32; is_signed = true; }
    else if (name == "i64") { bits = 64; is_signed = true; }
    else if (name == "isize") { bits = m_pass0.target().m_pointer_width; is_signed = true; }
    else if (name == "u8") bits = 8;
    else if (name == "u16") bits = 16;
    else if (name == "u32") bits = 32;
    else if (name == "u64") bits = 64;
    else if (name == "usize") bits = m_pass0.target().m_pointer_width;

    std::string subject = std::string(what) + " of type '" + std::string(name) + "'";
    if (bits != 0) {
        if (value.m_kind == ConstValue::Kind::Float) {
            m_diag.error(span, "cannot initialize " + subject + " with a floating-point value");
        } else if (value.m_kind == ConstValue::Kind::Bool) {
            m_diag.error(span, "cannot initialize " + subject + " with a boolean value");
        } else {
            int64_t v = value.m_int;
            bool fits = true;
            if (is_signed) {
                if (bits < 64) fits = v >= -(int64_t{1} << (bits - 1)) && v <= (int64_t{1} << (bits - 1)) - 1;
            } else {
                fits = v >= 0 && (bits >= 63 || v <= (int64_t{1} << bits) - 1);
            }
            if (!fits) m_diag.error(span, "value " + value.to_string() + " does not fit in " + subject);
        }
    } else if (name == "f32" || name == "f64") {
        if (value.m_kind == ConstValue::Kind::Bool) {
            m_diag.error(span, "cannot initialize " + subject + " with a boolean value");
        }
    } else if (name == "bool") {
        if (value.m_kind != ConstValue::Kind::Bool) {
            m_diag.error(span, "cannot initialize " + subject + " with a non-boolean value");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Declarations
// ---------------------------------------------------------------------------------------------

bool TypeCheckerPass2::check_module() {
    size_t errors_before = m_diag.error_count();

    check_type_declarations();
    check_declaration_consistency();

    for (DeclNode* decl : m_pass0.active_declarations()) {
        use_file(m_diag, decl);
        if (auto* var = dynamic_cast<VarDeclNode*>(decl)) {
            check_global_variable(var);
        } else if (auto* fn = dynamic_cast<FunctionDeclNode*>(decl)) {
            check_extern_rules(fn);
            if (fn->m_has_body) {
                const ClassDeclNode* owner = fn->m_owner != INVALID_SYMBOL_ID ? m_pass0.find_class(fn->m_owner) : nullptr;
                check_function(fn, owner);
            }
        } else if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
            for (FunctionDeclNode* method : cls->m_methods) {
                check_extern_rules(method);
                if (method->m_has_body) check_function(method, cls);
            }
        }
    }
    return m_diag.error_count() == errors_before;
}

void TypeCheckerPass2::check_type_declarations() {
    for (DeclNode* decl : m_pass0.active_declarations()) {
        use_file(m_diag, decl);
        if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
            std::set<SymbolID> fields;
            for (const ParameterDecl& field : cls->m_fields) {
                if (!fields.insert(field.m_name).second) {
                    m_diag.error(field.m_span, "duplicate field '" + name_of(field.m_name) + "' in '" + name_of(cls->m_name) + "'");
                }
            }
            std::set<SymbolID> methods;
            for (const FunctionDeclNode* method : cls->m_methods) {
                if (!methods.insert(method->m_name).second) {
                    m_diag.error(method->m_span, "duplicate method '" + name_of(method->m_name) + "' in '" + name_of(cls->m_name) + "'");
                }
                if (fields.count(method->m_name) != 0) {
                    m_diag.error(method->m_span, "method '" + name_of(method->m_name) + "' conflicts with a field of the same name");
                }
            }
        } else if (auto* en = dynamic_cast<EnumDeclNode*>(decl)) {
            std::set<SymbolID> variants;
            for (const EnumVariant& variant : en->m_variants) {
                if (!variants.insert(variant.m_name).second) {
                    m_diag.error(variant.m_span, "duplicate variant '" + name_of(variant.m_name) + "' in enum '" + name_of(en->m_name) + "'");
                }
            }
        }
    }
}

/// Interface (.fo) versus definition (.fl) agreement, redefinitions and owner checks.
void TypeCheckerPass2::check_declaration_consistency() {
    auto check_group = [&](const std::vector<FunctionDeclNode*>& group) {
        std::vector<const FunctionDeclNode*> plain;
        for (const FunctionDeclNode* fn : group) {
            if (fn->m_owner_args.empty()) plain.push_back(fn); // specializations are distinct entities
        }
        const FunctionDeclNode* first = nullptr;
        const FunctionDeclNode* first_body = nullptr;
        for (const FunctionDeclNode* fn : plain) {
            use_file(m_diag, fn);
            if (first == nullptr) {
                first = fn;
            } else if (signature_of(fn) != signature_of(first)) {
                m_diag.error(fn->m_span, "declaration of '" + qualified_name(fn) + "' " + signature_of(fn) +
                                         " does not match its earlier declaration " + signature_of(first));
            }
            if (fn->m_has_body) {
                if (first_body != nullptr) {
                    m_diag.error(fn->m_span, "redefinition of '" + qualified_name(fn) + "'");
                } else {
                    first_body = fn;
                }
            }
        }
    };

    for (const auto& [name, group] : m_pass0.functions()) {
        check_group(group);
        for (const FunctionDeclNode* fn : group) {
            if (fn->m_receiver.m_kind != ReceiverKind::None) {
                use_file(m_diag, fn);
                m_diag.error(fn->m_span, "free function '" + name_of(fn->m_name) +
                                         "' has a 'this' receiver; declare it as 'fn Type::" + name_of(fn->m_name) + "'");
            }
        }
    }

    for (const auto& [owner, group] : m_pass0.methods()) {
        // Group per method name.
        std::unordered_map<SymbolID, std::vector<FunctionDeclNode*>> by_name;
        for (FunctionDeclNode* fn : group) by_name[fn->m_name].push_back(fn);

        const ClassDeclNode* cls = m_pass0.find_class(owner);
        const BlueprintDeclNode* blueprint = m_pass0.find_blueprint(owner);

        for (auto& [method_name, impls] : by_name) {
            // Implementations of one method (ignoring specializations) must be unique and consistent.
            std::vector<FunctionDeclNode*> with_declaration = impls;
            if (cls != nullptr) {
                for (FunctionDeclNode* declared : cls->m_methods) {
                    if (declared->m_name == method_name) with_declaration.insert(with_declaration.begin(), declared);
                }
            }
            check_group(with_declaration);

            for (const FunctionDeclNode* impl : impls) {
                use_file(m_diag, impl);
                if (cls != nullptr) {
                    bool declared = false;
                    for (const FunctionDeclNode* method : cls->m_methods) declared = declared || method->m_name == method_name;
                    if (!declared) {
                        m_diag.error(impl->m_span, "method '" + name_of(method_name) + "' is not declared in class '" + name_of(owner) + "'");
                    }
                    if (impl->m_receiver.m_kind == ReceiverKind::None && !impl->m_has_body) continue;
                } else if (blueprint != nullptr) {
                    bool declared = false;
                    for (const DeclNode* member : blueprint->m_declarations) {
                        const auto* member_fn = dynamic_cast<const FunctionDeclNode*>(member);
                        declared = declared || (member_fn != nullptr && member_fn->m_name == method_name);
                    }
                    if (!declared) {
                        m_diag.error(impl->m_span, "blueprint '" + name_of(owner) + "' has no function '" + name_of(method_name) + "'");
                    }
                    for (const GenericArg& arg : impl->m_owner_args) {
                        if (arg.m_name == INVALID_SYMBOL_ID) continue;
                        bool known = false;
                        for (const GenericParam& param : blueprint->m_type_params) known = known || param.m_name == arg.m_name;
                        if (!known) {
                            m_diag.error(impl->m_span, "blueprint '" + name_of(owner) + "' has no parameter '" + name_of(arg.m_name) + "'");
                        }
                    }
                }
            }
        }
    }
}

void TypeCheckerPass2::check_global_variable(const VarDeclNode* var) {
    if (var->m_initializer == nullptr) return;
    if (var->m_kind == BindingKind::Constx) {
        auto it = m_pass0.definitions().find(var->m_name);
        if (it != m_pass0.definitions().end()) check_const_fits_type(var->m_type, it->second, var->m_span, "constant");
        return;
    }
    ConstEvaluator evaluator(m_pass0.definitions(), m_diag);
    if (auto value = evaluator.evaluate(var->m_initializer, /*report=*/false)) {
        check_const_fits_type(var->m_type, *value, var->m_span, "variable");
    }
    push_scope();
    check_expression(var->m_initializer);
    pop_scope();
}

void TypeCheckerPass2::check_extern_rules(const FunctionDeclNode* fn) {
    if (has_attribute(fn, "extern") && fn->m_has_body) {
        m_diag.error(fn->m_span, "'@extern' function '" + qualified_name(fn) + "' cannot have a body");
    }
}

// ---------------------------------------------------------------------------------------------
// Functions
// ---------------------------------------------------------------------------------------------

bool TypeCheckerPass2::check_function(const FunctionDeclNode* fn, const ClassDeclNode* owner) {
    size_t errors_before = m_diag.error_count();
    m_current_fn = fn;
    m_current_owner = owner;
    m_loop_depth = 0;
    m_local_constants.clear();
    m_scopes.clear();
    push_scope();

    if (fn->m_receiver.m_kind != ReceiverKind::None) {
        SymbolBinding self;
        self.m_name = fn->m_receiver.m_self_name;
        self.m_span = fn->m_span;
        self.m_type_name = fn->m_owner != INVALID_SYMBOL_ID ? fn->m_owner
                           : (owner != nullptr ? owner->m_name : INVALID_SYMBOL_ID);
        self.m_is_view = fn->m_receiver.m_kind != ReceiverKind::ByValue;
        self.m_is_const_view = fn->m_receiver.m_kind == ReceiverKind::ByConstRef;
        declare(self);
    }

    for (const ParameterDecl& param : fn->m_params) {
        if (lookup_in_current_scope(param.m_name) != nullptr) {
            m_diag.error(param.m_span, "duplicate parameter name '" + name_of(param.m_name) + "'");
            continue;
        }
        declare(make_binding(param.m_name, param.m_type, BindingKind::Mutable, param.m_span));
        if (param.m_default_value != nullptr) {
            ConstEvaluator evaluator(m_pass0.definitions(), m_diag);
            if (auto value = evaluator.evaluate(param.m_default_value, /*report=*/false)) {
                check_const_fits_type(param.m_type, *value, param.m_span, "parameter default");
            }
        }
    }

    check_statements(fn->m_body);

    bool returns_value = fn->m_return_type != nullptr && fn->m_return_type->m_kind != TypeNodeKind::Never;
    if (fn->m_has_body && returns_value && !always_returns(fn->m_body)) {
        m_diag.error(fn->m_span, "function '" + qualified_name(fn) + "' does not return a value on all paths");
    }

    pop_scope();
    m_current_fn = nullptr;
    m_current_owner = nullptr;
    return m_diag.error_count() == errors_before;
}

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

const ConditionalBranch* TypeCheckerPass2::select_branch(const ConditionalStmt* stmt, bool report) const {
    ConstEvaluator evaluator(m_pass0.definitions(), m_diag);
    for (const ConditionalBranch& branch : stmt->m_branches) {
        if (branch.m_condition == nullptr) return &branch; // @else
        auto value = evaluator.evaluate(branch.m_condition, report);
        if (!value) return nullptr; // not a constant: reported above, do not guess
        if (value->truthy()) return &branch;
    }
    return nullptr;
}

void TypeCheckerPass2::check_statements(const std::vector<StmtNode*>& stmts) {
    for (const StmtNode* stmt : stmts) {
        if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
            // Compile-time @if: only the selected branch exists as far as later passes are concerned.
            if (const ConditionalBranch* branch = select_branch(conditional, true)) check_statements(branch->m_body);
            continue;
        }
        check_statement(stmt);
    }
}

void TypeCheckerPass2::check_block_scoped(const BlockStmt* block) {
    if (block == nullptr) return;
    push_scope();
    check_statements(block->m_statements);
    pop_scope();
}

void TypeCheckerPass2::check_statement(const StmtNode* stmt) {
    if (stmt == nullptr) return;

    if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        if (ret->m_value != nullptr) check_expression(ret->m_value);
        if (m_current_fn == nullptr) return;
        const TypeNode* rt = m_current_fn->m_return_type;
        bool is_void = rt == nullptr;
        bool is_never = rt != nullptr && rt->m_kind == TypeNodeKind::Never;
        if (ret->m_value != nullptr && is_void) {
            m_diag.error(ret->m_span, "void function '" + qualified_name(m_current_fn) + "' cannot return a value");
        } else if (ret->m_value != nullptr && is_never) {
            m_diag.error(ret->m_span, "function '" + qualified_name(m_current_fn) + "' never returns");
        } else if (ret->m_value == nullptr && !is_void && !is_never) {
            m_diag.error(ret->m_span, "non-void function '" + qualified_name(m_current_fn) + "' must return a value of type '" +
                                      type_to_string(rt) + "'");
        } else if (ret->m_value != nullptr) {
            ConstEvaluator evaluator(visible_constants(), m_diag);
            if (auto value = evaluator.evaluate(ret->m_value, /*report=*/false)) {
                check_const_fits_type(rt, *value, ret->m_span, "return value");
            }
        }
    } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
        check_expression(expr_stmt->m_expr);
    } else if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
        if (const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl)) check_var_decl(var);
    } else if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        check_block_scoped(block);
    } else if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
        check_expression(if_stmt->m_condition);
        check_block_scoped(if_stmt->m_then);
        if (if_stmt->m_else != nullptr) check_statement(if_stmt->m_else);
    } else if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
        check_expression(while_stmt->m_condition);
        ++m_loop_depth;
        check_block_scoped(while_stmt->m_body);
        --m_loop_depth;
    } else if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
        check_expression(for_stmt->m_iterable);
        push_scope();
        SymbolBinding var;
        var.m_name = for_stmt->m_variable;
        var.m_binding_kind = BindingKind::Immutable;
        var.m_span = for_stmt->m_span;
        declare(var);
        ++m_loop_depth;
        if (for_stmt->m_body != nullptr) check_statements(for_stmt->m_body->m_statements);
        --m_loop_depth;
        pop_scope();
    } else if (dynamic_cast<const BreakStmt*>(stmt) != nullptr) {
        if (m_loop_depth == 0) m_diag.error(stmt->m_span, "'break' outside of a loop");
    } else if (dynamic_cast<const ContinueStmt*>(stmt) != nullptr) {
        if (m_loop_depth == 0) m_diag.error(stmt->m_span, "'continue' outside of a loop");
    } else if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
        if (const ConditionalBranch* branch = select_branch(conditional, true)) check_statements(branch->m_body);
    }
}

void TypeCheckerPass2::check_var_decl(const VarDeclNode* var) {
    if (var->m_initializer != nullptr) check_expression(var->m_initializer);

    ConstEvaluator evaluator(visible_constants(), m_diag);
    if (var->m_kind == BindingKind::Constx) {
        if (var->m_initializer != nullptr) {
            if (auto value = evaluator.evaluate(var->m_initializer)) {
                check_const_fits_type(var->m_type, *value, var->m_span, "constant");
                m_local_constants[var->m_name] = *value;
            }
        }
    } else if (var->m_type != nullptr && var->m_initializer != nullptr) {
        if (auto value = evaluator.evaluate(var->m_initializer, /*report=*/false)) {
            check_const_fits_type(var->m_type, *value, var->m_span, "variable");
        }
    }

    if (lookup_in_current_scope(var->m_name) != nullptr) {
        m_diag.error(var->m_span, "redefinition of '" + name_of(var->m_name) + "' in this scope");
        return;
    }
    SymbolBinding binding = make_binding(var->m_name, var->m_type, var->m_kind, var->m_span);
    if (var->m_type == nullptr && var->m_initializer != nullptr) {
        if (const auto* lit = dynamic_cast<const StructLiteralExpr*>(var->m_initializer)) {
            binding.m_type_name = nominal_name(lit->m_type);
        } else if (const auto* cast = dynamic_cast<const CastExpr*>(var->m_initializer)) {
            binding.m_type_name = nominal_name(cast->m_target);
            binding.m_is_const_view = cast->m_target != nullptr && cast->m_target->is_const_view();
            binding.m_is_pointer = cast->m_target != nullptr && cast->m_target->m_kind == TypeNodeKind::Pointer;
            binding.m_is_view = cast->m_target != nullptr && cast->m_target->is_view();
        }
    }
    declare(binding);
}

bool TypeCheckerPass2::always_returns(const std::vector<StmtNode*>& stmts) const {
    for (const StmtNode* stmt : stmts) {
        if (stmt_always_returns(stmt)) return true;
    }
    return false;
}

bool TypeCheckerPass2::stmt_always_returns(const StmtNode* stmt) const {
    if (stmt == nullptr) return false;
    if (dynamic_cast<const ReturnStmt*>(stmt) != nullptr) return true;
    if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) return always_returns(block->m_statements);
    if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
        return if_stmt->m_else != nullptr && stmt_always_returns(if_stmt->m_then) && stmt_always_returns(if_stmt->m_else);
    }
    if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
        const ConditionalBranch* branch = select_branch(conditional, /*report=*/false);
        return branch != nullptr && always_returns(branch->m_body);
    }
    if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
        // A call to a function declared `-> !` never comes back.
        if (const auto* call = dynamic_cast<const CallExpr*>(expr_stmt->m_expr)) {
            const FunctionDeclNode* callee = resolve_callee(call);
            return callee != nullptr && callee->m_return_type != nullptr &&
                   callee->m_return_type->m_kind == TypeNodeKind::Never;
        }
    }
    return false;
}

} // namespace fool
