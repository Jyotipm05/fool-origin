//
// Created by jmbab on 07-11-2026.
//

/**
 * @file TypeCheckerExpr.cpp
 * @brief Pass 2 (part 2): expressions, assignments, mutability, receivers, moves and call binding.
 */

module;

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }
std::string name_of(SymbolID id) { return std::string(text_of(id)); }

SymbolID nominal_name(const TypeNode* type) {
    if (type == nullptr) return INVALID_SYMBOL_ID;
    switch (type->m_kind) {
        case TypeNodeKind::Path: return type->m_path.empty() ? INVALID_SYMBOL_ID : type->m_path.back();
        case TypeNodeKind::Pointer:
        case TypeNodeKind::Reference: return nominal_name(type->m_inner);
        case TypeNodeKind::Never: return INVALID_SYMBOL_ID;
    }
    return INVALID_SYMBOL_ID;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Resolution helpers
// ---------------------------------------------------------------------------------------------

const FunctionDeclNode* TypeCheckerPass2::resolve_callee(const CallExpr* call) const {
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(call->m_callee)) {
        return m_pass0.find_function(id->m_identifier);
    }
    if (const auto* path = dynamic_cast<const PathExpr*>(call->m_callee)) {
        if (path->m_segments.size() != 2) return nullptr;
        if (const FunctionDeclNode* method = m_pass0.find_method(path->m_segments[0], path->m_segments[1])) {
            return method;
        }
        return m_pass0.find_function_in_import(path->m_segments[0], path->m_segments[1]);
    }
    return nullptr;
}

const SymbolBinding* TypeCheckerPass2::root_binding(const ExprNode* expr) {
    while (expr != nullptr) {
        if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
            expr = member->m_object;
        } else if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
            expr = index->m_base;
        } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
            if (unary->m_op != TokenKind::Star) return nullptr;
            expr = unary->m_operand;
        } else if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
            return lookup(id->m_identifier);
        } else {
            return nullptr;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::check_expression(const ExprNode* expr) {
    if (expr == nullptr) return;

    if (const auto* id = dynamic_cast<const IdentifierExpr*>(expr)) {
        if (const SymbolBinding* binding = lookup(id->m_identifier)) {
            if (binding->m_is_moved) {
                m_diag.error(expr->m_span, "use of moved value '" + name_of(id->m_identifier) + "'");
            }
            return;
        }
        if (m_strict_names && !m_pass0.is_known_global(id->m_identifier) && !is_generic_parameter(id->m_identifier)) {
            m_diag.error(expr->m_span, "use of undeclared identifier '" + name_of(id->m_identifier) + "'");
        }
    } else if (const auto* path = dynamic_cast<const PathExpr*>(expr)) {
        SymbolID first = path->m_segments.front();
        bool known_type = m_pass0.find_class(first) != nullptr || m_pass0.find_enum(first) != nullptr ||
                          m_pass0.find_blueprint(first) != nullptr || m_pass0.find_ruleset(first) != nullptr;
        if (path->m_segments.size() == 2) {
            SymbolID second = path->m_segments[1];
            if (const EnumDeclNode* en = m_pass0.find_enum(first)) {
                bool found = m_pass0.find_method(first, second) != nullptr;
                for (const EnumVariant& variant : en->m_variants) found = found || variant.m_name == second;
                if (!found) {
                    m_diag.error(expr->m_span, "enum '" + name_of(first) + "' has no variant or function '" + name_of(second) + "'");
                }
            } else if (m_pass0.find_class(first) != nullptr && m_pass0.find_method(first, second) == nullptr) {
                m_diag.error(expr->m_span, "'" + name_of(first) + "' has no function '" + name_of(second) + "'");
            }
        }
        if (m_strict_names && !known_type && !m_pass0.is_import_alias(first) && !is_generic_parameter(first) &&
            lookup(first) == nullptr && !m_pass0.is_known_global(first)) {
            m_diag.error(expr->m_span, "unknown name '" + name_of(first) + "' in qualified path");
        }
    } else if (const auto* lit = dynamic_cast<const StructLiteralExpr*>(expr)) {
        check_struct_literal(lit);
    } else if (const auto* unary = dynamic_cast<const UnaryExpr*>(expr)) {
        check_expression(unary->m_operand);
    } else if (const auto* binary = dynamic_cast<const BinaryExpr*>(expr)) {
        if (binary->m_op == TokenKind::Equal) {
            check_assignment(binary);
        } else {
            check_expression(binary->m_lhs);
            check_expression(binary->m_rhs);
        }
    } else if (const auto* cast = dynamic_cast<const CastExpr*>(expr)) {
        check_expression(cast->m_operand);
        if (cast->m_cast_kind != CastKind::Unchecked) check_named_cast(cast);
    } else if (const auto* member = dynamic_cast<const MemberExpr*>(expr)) {
        check_member_access(member);
    } else if (const auto* index = dynamic_cast<const IndexExpr*>(expr)) {
        check_expression(index->m_base);
        check_expression(index->m_index);
    } else if (const auto* call = dynamic_cast<const CallExpr*>(expr)) {
        check_call(call);
    } else if (const auto* intrinsic = dynamic_cast<const IntrinsicCallExpr*>(expr)) {
        for (const ExprNode* arg : intrinsic->m_args) {
            // Intrinsics such as @size_of(T) take type names, which are not value expressions.
            if (dynamic_cast<const IdentifierExpr*>(arg) != nullptr || dynamic_cast<const PathExpr*>(arg) != nullptr) continue;
            check_expression(arg);
        }
    }
}

void TypeCheckerPass2::check_member_access(const MemberExpr* member) {
    check_expression(member->m_object);
    const auto* object = dynamic_cast<const IdentifierExpr*>(member->m_object);
    if (object == nullptr) return;
    const SymbolBinding* binding = lookup(object->m_identifier);
    if (binding == nullptr) return;
    const ClassDeclNode* cls = class_of_binding(*binding);
    if (cls == nullptr || cls->m_fields.empty()) return;
    for (const ParameterDecl& field : cls->m_fields) {
        if (field.m_name == member->m_member) return;
    }
    m_diag.error(member->m_span, "'" + name_of(cls->m_name) + "' has no field '" + name_of(member->m_member) + "'");
}

void TypeCheckerPass2::check_struct_literal(const StructLiteralExpr* lit) {
    for (const FieldInit& field : lit->m_fields) check_expression(field.m_value);

    SymbolID type_name = nominal_name(lit->m_type);
    const ClassDeclNode* cls = type_name != INVALID_SYMBOL_ID ? m_pass0.find_class(type_name) : nullptr;
    if (cls == nullptr) return;

    std::set<SymbolID> provided;
    for (const FieldInit& init : lit->m_fields) {
        bool exists = false;
        for (const ParameterDecl& field : cls->m_fields) exists = exists || field.m_name == init.m_name;
        if (!exists) {
            m_diag.error(init.m_span, "'" + name_of(cls->m_name) + "' has no field '" + name_of(init.m_name) + "'");
        } else if (!provided.insert(init.m_name).second) {
            m_diag.error(init.m_span, "field '" + name_of(init.m_name) + "' is initialized more than once");
        }
    }
    for (const ParameterDecl& field : cls->m_fields) {
        if (provided.count(field.m_name) == 0) {
            m_diag.error(lit->m_span, "missing initializer for field '" + name_of(field.m_name) + "' of '" + name_of(cls->m_name) + "'");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Assignment and mutability
// ---------------------------------------------------------------------------------------------

void TypeCheckerPass2::check_mutation_target(const ExprNode* target, SourceSpan span) {
    const SymbolBinding* root = root_binding(target);
    if (root == nullptr) return;
    std::string root_name = name_of(root->m_name);
    if (root->m_is_view) {
        // Writing through a reference or pointer depends on the view's constness only.
        if (root->m_is_const_view) {
            m_diag.error(span, "cannot assign through '" + root_name + "': it is a const reference or pointer");
        }
    } else if (root->m_binding_kind != BindingKind::Mutable) {
        m_diag.error(span, "cannot assign through '" + root_name + "': it is an immutable binding" +
                           (root->m_binding_kind == BindingKind::Constx ? " (constx)" : " (declared with 'const')"));
    }
}

void TypeCheckerPass2::check_assignment(const BinaryExpr* assign) {
    const ExprNode* lhs = assign->m_lhs;
    // The right-hand side is evaluated first, so `x = x + 1` never sees the new `x`.
    check_expression(assign->m_rhs);

    if (const auto* id = dynamic_cast<const IdentifierExpr*>(lhs)) {
        SymbolID name = id->m_identifier;
        if (SymbolBinding* binding = lookup(name)) {
            if (binding->m_binding_kind == BindingKind::Constx) {
                m_diag.error(lhs->m_span, "cannot assign to constant '" + name_of(name) + "'");
            } else if (binding->m_binding_kind == BindingKind::Immutable) {
                m_diag.error(lhs->m_span, "cannot assign twice to immutable binding '" + name_of(name) +
                                          "' (declared with 'const')");
            } else {
                binding->m_is_moved = false; // assignment re-initializes a moved-from variable
            }
            return;
        }
        if (const VarDeclNode* global = m_pass0.find_global(name)) {
            if (global->m_kind != BindingKind::Mutable) {
                m_diag.error(lhs->m_span, "cannot assign to " +
                                          std::string(global->m_kind == BindingKind::Constx ? "constant" : "immutable binding") +
                                          " '" + name_of(name) + "'");
            }
            return;
        }
        if (m_pass0.is_known_global(name)) {
            m_diag.error(lhs->m_span, "cannot assign to '" + name_of(name) + "': it is not a variable");
            return;
        }

        // First assignment to an unknown name declares a mutable variable in the current scope.
        SymbolBinding declared;
        declared.m_name = name;
        declared.m_binding_kind = BindingKind::Mutable;
        declared.m_span = lhs->m_span;
        if (const auto* lit = dynamic_cast<const StructLiteralExpr*>(assign->m_rhs)) {
            declared.m_type_name = nominal_name(lit->m_type);
        } else if (const auto* cast = dynamic_cast<const CastExpr*>(assign->m_rhs)) {
            declared.m_type_name = nominal_name(cast->m_target);
            declared.m_is_const_view = cast->m_target != nullptr && cast->m_target->is_const_view();
            declared.m_is_pointer = cast->m_target != nullptr && cast->m_target->m_kind == TypeNodeKind::Pointer;
            declared.m_is_view = cast->m_target != nullptr && cast->m_target->is_view();
        }
        declare(declared);
        return;
    }

    bool is_place = dynamic_cast<const MemberExpr*>(lhs) != nullptr || dynamic_cast<const IndexExpr*>(lhs) != nullptr;
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(lhs)) is_place = unary->m_op == TokenKind::Star;
    if (!is_place) {
        m_diag.error(lhs->m_span, "left-hand side of assignment is not assignable");
        return;
    }
    check_expression(lhs);
    check_mutation_target(lhs, lhs->m_span);
}

// ---------------------------------------------------------------------------------------------
// Calls
// ---------------------------------------------------------------------------------------------

const ResolvedCall* TypeCheckerPass2::find_resolved_call(const CallExpr* call) const {
    auto it = m_resolved_call_index.find(call);
    return it == m_resolved_call_index.end() ? nullptr : &m_resolved_calls[it->second];
}

void TypeCheckerPass2::bind_arguments(const FunctionDeclNode* fn, const CallExpr* call, std::string_view display_name,
                                      const ExprNode* receiver) {
    const auto& params = fn->m_params;
    std::vector<bool> filled(params.size(), false);
    std::vector<size_t> source_of(params.size(), ResolvedArgument::NO_SOURCE);
    const size_t errors_before = m_diag.error_count();
    bool seen_label = false;
    size_t next_positional = 0;
    bool reported_too_many = false;

    for (size_t arg_index = 0; arg_index < call->m_args.size(); ++arg_index) {
        const CallArg& arg = call->m_args[arg_index];
        if (arg.m_label == INVALID_SYMBOL_ID) {
            if (seen_label) {
                m_diag.error(arg.m_span, "positional argument cannot follow a named argument in call to '" +
                                         std::string(display_name) + "'");
                continue;
            }
            if (next_positional >= params.size()) {
                if (!reported_too_many) {
                    m_diag.error(arg.m_span, "too many arguments in call to '" + std::string(display_name) +
                                             "': expected at most " + std::to_string(params.size()));
                    reported_too_many = true;
                }
                continue;
            }
            source_of[next_positional] = arg_index;
            filled[next_positional++] = true;
        } else {
            seen_label = true;
            size_t index = params.size();
            for (size_t i = 0; i < params.size(); ++i) {
                if (params[i].m_name == arg.m_label) index = i;
            }
            if (index == params.size()) {
                m_diag.error(arg.m_span, "'" + std::string(display_name) + "' has no parameter named '" + name_of(arg.m_label) + "'");
            } else if (filled[index]) {
                m_diag.error(arg.m_span, "multiple values for parameter '" + name_of(arg.m_label) + "' in call to '" +
                                         std::string(display_name) + "'");
            } else {
                source_of[index] = arg_index;
                filled[index] = true;
            }
        }
    }

    for (size_t i = 0; i < params.size(); ++i) {
        if (!filled[i] && params[i].m_default_value == nullptr) {
            m_diag.error(call->m_span, "missing argument for parameter '" + name_of(params[i].m_name) +
                                       "' in call to '" + std::string(display_name) + "'");
        }
    }

    // Only a call that bound cleanly has a canonical form; erroneous calls never reach later stages.
    if (m_diag.error_count() != errors_before || m_resolved_call_index.count(call) != 0) return;
    ResolvedCall resolved;
    resolved.m_call = call;
    resolved.m_callee = fn;
    resolved.m_display = std::string(display_name);
    resolved.m_receiver = receiver;
    for (size_t i = 0; i < params.size(); ++i) {
        ResolvedArgument slot;
        if (filled[i]) {
            slot.m_source_index = source_of[i];
            slot.m_value = call->m_args[source_of[i]].m_expr;
        } else {
            slot.m_is_default = true;
            slot.m_value = params[i].m_default_value;
        }
        resolved.m_slots.push_back(slot);
    }
    m_resolved_call_index.emplace(call, m_resolved_calls.size());
    m_resolved_calls.push_back(std::move(resolved));
}

void TypeCheckerPass2::check_call(const CallExpr* call) {
    const FunctionDeclNode* target = nullptr;
    std::string display;
    const FunctionDeclNode* consuming_method = nullptr;
    SymbolID receiver_name = INVALID_SYMBOL_ID;
    const ExprNode* receiver_object = nullptr;

    if (const auto* id = dynamic_cast<const IdentifierExpr*>(call->m_callee)) {
        display = name_of(id->m_identifier);
        if (lookup(id->m_identifier) != nullptr) {
            check_expression(call->m_callee); // calling a local value
        } else if (const FunctionDeclNode* fn = m_pass0.find_function(id->m_identifier)) {
            target = fn;
        } else if (m_strict_names && !m_pass0.is_known_global(id->m_identifier) &&
                   !is_generic_parameter(id->m_identifier)) {
            m_diag.error(call->m_callee->m_span, "call to undeclared function '" + display + "'");
        }
    } else if (const auto* path = dynamic_cast<const PathExpr*>(call->m_callee)) {
        check_expression(call->m_callee);
        if (path->m_segments.size() == 2) {
            SymbolID first = path->m_segments[0];
            SymbolID second = path->m_segments[1];
            display = name_of(first) + "::" + name_of(second);
            if (const FunctionDeclNode* method = m_pass0.find_method(first, second)) {
                if (method->m_receiver.m_kind == ReceiverKind::None) target = method;
            } else if (const FunctionDeclNode* fn = m_pass0.find_function_in_import(first, second)) {
                target = fn;
            }
        }
    } else if (const auto* member = dynamic_cast<const MemberExpr*>(call->m_callee)) {
        check_expression(member->m_object);
        display = name_of(member->m_member);
        const auto* object = dynamic_cast<const IdentifierExpr*>(member->m_object);
        const SymbolBinding* binding = object != nullptr ? lookup(object->m_identifier) : nullptr;
        const ClassDeclNode* cls = binding != nullptr ? class_of_binding(*binding) : nullptr;
        if (cls != nullptr) {
            const FunctionDeclNode* method = m_pass0.find_method(cls->m_name, member->m_member);
            bool is_field = false;
            for (const ParameterDecl& field : cls->m_fields) is_field = is_field || field.m_name == member->m_member;
            if (method == nullptr && !is_field) {
                m_diag.error(member->m_span, "'" + name_of(cls->m_name) + "' has no method '" + display + "'");
            } else if (method != nullptr) {
                target = method;
                receiver_object = member->m_object;
                display = name_of(cls->m_name) + "::" + display;
                std::string binding_name = name_of(binding->m_name);
                ReceiverKind kind = method->m_receiver.m_kind;
                if (kind == ReceiverKind::None) {
                    m_diag.error(member->m_span, "'" + display + "' has no 'this' receiver and cannot be called on an instance");
                } else if (kind == ReceiverKind::ByMutRef) {
                    if (binding->m_is_view ? binding->m_is_const_view : binding->m_binding_kind != BindingKind::Mutable) {
                        m_diag.error(member->m_span, "cannot call '" + display + "' (takes 'this &self') on immutable '" +
                                                     binding_name + "'");
                    }
                } else if (kind == ReceiverKind::ByValue) {
                    if (binding->m_is_view) {
                        m_diag.error(member->m_span, "cannot call consuming method '" + display + "' on borrowed '" +
                                                     binding_name + "'");
                    } else {
                        consuming_method = method;
                        receiver_name = binding->m_name;
                    }
                }
            }
        }
    } else {
        check_expression(call->m_callee);
    }

    for (const CallArg& arg : call->m_args) check_expression(arg.m_expr);
    if (target != nullptr) bind_arguments(target, call, display, receiver_object);

    // `this self` consumes the receiver: later uses are errors until it is assigned again.
    if (consuming_method != nullptr) {
        if (SymbolBinding* binding = lookup(receiver_name)) binding->m_is_moved = true;
    }
}

} // namespace fool
