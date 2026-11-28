//
// Created by jmbab on 06-11-2026.
//

/**
 * @file TypeChecker.cpp
 * @brief Pass 2 (part 1): declaration consistency, scopes, function bodies and statements.
 */

module;
#include <filesystem>
#include <ranges>
#include <memory>
#include <optional>

#if defined(__MINGW64__) || defined(__GNUC__)
#include <cstddef>
#endif
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
        std::string_view text_of(const SymbolID id) { return StringInterner::get().resolve(id); }
        std::string name_of(const SymbolID id) { return std::string(text_of(id)); }

        /// Name of the class/struct a type designates, looking through references and pointers.
        SymbolID nominal_name(const TypeNode *type) {
            if (type == nullptr) return INVALID_SYMBOL_ID;
            switch (type->m_kind) {
                case TypeNodeKind::Path:
                    return type->m_path.empty() ? INVALID_SYMBOL_ID : type->m_path.back();
                case TypeNodeKind::Pointer:
                case TypeNodeKind::Reference:
                case TypeNodeKind::Optional:
                    return nominal_name(type->m_inner);
                case TypeNodeKind::Never:
                case TypeNodeKind::Function:
                    return INVALID_SYMBOL_ID;
            }
            return INVALID_SYMBOL_ID;
        }

        std::string receiver_text(const ReceiverKind kind) {
            switch (kind) {
                case ReceiverKind::None: return "";
                case ReceiverKind::ByValue: return "this self";
                case ReceiverKind::ByConstRef: return "this const& self";
                case ReceiverKind::ByMutRef: return "this &self";
            }
            return "";
        }

        /// Signature text used to compare an interface declaration with its definition.
        std::string signature_of(const FunctionDeclNode *fn) {
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

        std::string qualified_name(const FunctionDeclNode *fn) {
            if (fn->m_owner == INVALID_SYMBOL_ID) return name_of(fn->m_name);
            return name_of(fn->m_owner) + "::" + name_of(fn->m_name);
        }

        bool has_attribute(const DeclNode *decl, const std::string_view name) {
            for (const AttributeNode &attr: decl->m_attributes) {
                if (text_of(attr.m_name) == name) return true;
            }
            return false;
        }

        void use_file(DiagnosticEngine &diag, const DeclNode *decl) {
            if (decl != nullptr && !decl->m_file.empty()) diag.set_current_file(std::string(decl->m_file));
        }
    } // namespace

    // ---------------------------------------------------------------------------------------------
    // Visibility
    // ---------------------------------------------------------------------------------------------

    void TypeCheckerPass2::check_access(const DeclNode *decl, const SourceSpan span, const std::string_view what,
                                        const std::string &name) const {
        if (m_pass0.is_accessible(decl, m_diag.current_file())) return;
        const std::string file = std::filesystem::path(std::string(decl->m_file)).filename().string();
        m_diag.error(span, std::string(what) + " '" + name + "' is private to '" + file +
                           "'; declare it `pub` to use it from another file");
        note_declared_here(decl->m_file, decl->m_span, name);
    }

    /// A note at the private declaration, which lives in another file than the one being checked.
    void TypeCheckerPass2::note_declared_here(const std::string_view file, const SourceSpan span, const std::string &name) const {
        const std::string viewing = m_diag.current_file();
        m_diag.set_current_file(std::string(file));
        m_diag.note(span, "'" + name + "' is declared here without `pub`");
        m_diag.set_current_file(viewing);
    }

    void TypeCheckerPass2::check_type_access(const TypeNode *type, const SourceSpan span) const {
        if (type == nullptr) return;
        switch (type->m_kind) {
            case TypeNodeKind::Path: {
                if (type->m_path.empty()) return;
                const SymbolID name = type->m_path.back();
                const DeclNode *decl = m_pass0.find_class(name);
                if (decl == nullptr) decl = m_pass0.find_enum(name);
                if (decl == nullptr) decl = m_pass0.find_ruleset(name);
                if (decl == nullptr) decl = m_pass0.find_blueprint(name);
                if (decl != nullptr && !is_generic_parameter(name)) check_access(decl, span, "type", name_of(name));
                for (const GenericArg &arg: type->m_generic_args) check_type_access(arg.m_type, span);
                return;
            }
            case TypeNodeKind::Pointer:
            case TypeNodeKind::Reference:
            case TypeNodeKind::Optional:
                check_type_access(type->m_inner, span);
                return;
            case TypeNodeKind::Function:
                for (const TypeNode *param: type->m_param_types) check_type_access(param, span);
                check_type_access(type->m_inner, span);
                return;
            case TypeNodeKind::Never:
                return;
        }
    }

    void TypeCheckerPass2::check_field_access(const ClassDeclNode *cls, const ParameterDecl &field,
                                              const SourceSpan span) const {
        if (field.m_is_pub || cls->m_file.empty() || m_diag.current_file().empty()) return;
        if (unit_key(cls->m_file) == unit_key(m_diag.current_file())) return;
        const std::string file = std::filesystem::path(std::string(cls->m_file)).filename().string();
        m_diag.error(span, "field '" + name_of(field.m_name) + "' of '" + name_of(cls->m_name) + "' is private to '" + file +
                           "'; declare it `pub` inside the " + (cls->m_is_struct ? "struct" : "class"));
        note_declared_here(cls->m_file, field.m_span, name_of(field.m_name));
    }

    void TypeCheckerPass2::check_method_access(const FunctionDeclNode *method, const SymbolID owner,
                                               const SourceSpan span) const {
        // Methods written in a class body are public only with `pub`; those of a ruleset or blueprint follow it.
        if (m_pass0.find_ruleset(owner) != nullptr || m_pass0.find_blueprint(owner) != nullptr) return;
        check_access(method, span, "method", name_of(owner) + "::" + name_of(method->m_name));
    }

    bool TypeCheckerPass2::is_known_name(const SymbolID name) const {
        return m_pass0.is_known_global(name) || m_pass0.find_global_from(m_namespace, name) != nullptr ||
               m_pass0.find_function_from(m_namespace, name) != nullptr;
    }

    std::vector<SymbolID> TypeCheckerPass2::path_segments(const PathExpr *path) const {
        return {path->m_segments.begin(), path->m_segments.end()};
    }

    TypeCheckerPass2::TypeCheckerPass2(SymbolTablePass0 &pass0, DiagnosticEngine &diag)
        : m_pass0(pass0), m_diag(diag) {
    }

    // ---------------------------------------------------------------------------------------------
    // Scopes
    // ---------------------------------------------------------------------------------------------

    void TypeCheckerPass2::push_scope() {
        m_scopes.emplace_back();
        m_narrow_marks.push_back(m_narrow_log.size());
    }

    void TypeCheckerPass2::pop_scope() {
        // Narrowing established inside the scope ends with it (before its bindings go away, so the names still resolve).
        unnarrow_to(m_narrow_marks.back());
        m_narrow_marks.pop_back();
        m_scopes.pop_back();
    }

    SymbolBinding *TypeCheckerPass2::lookup(const SymbolID name) {
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            for (auto binding = scope->rbegin(); binding != scope->rend(); ++binding) {
                if (binding->m_name == name) return &*binding;
            }
        }
        return nullptr;
    }

    SymbolBinding *TypeCheckerPass2::lookup_in_current_scope(const SymbolID name) {
        if (m_scopes.empty()) return nullptr;
        for (auto &binding: m_scopes.back()) {
            if (binding.m_name == name) return &binding;
        }
        return nullptr;
    }

    /// `--warn-shadow`: the declaration is about to enter the innermost scope; report what it hides.
    void TypeCheckerPass2::warn_if_shadowing(const SymbolBinding &binding) const {
        // Outer local scopes, innermost first (the scope the binding goes into is the last one).
        for (size_t i = m_scopes.size() - 1; i-- > 0;) {
            for (auto it = m_scopes[i].rbegin(); it != m_scopes[i].rend(); ++it) {
                if (it->m_name != binding.m_name) continue;
                m_diag.warning(binding.m_span, "declaration of '" + name_of(binding.m_name) +
                                               "' shadows a declaration in an enclosing scope");
                m_diag.note(it->m_span, "the shadowed declaration is here");
                return;
            }
        }
        if (const VarDeclNode *global = m_pass0.find_global(binding.m_name)) {
            m_diag.warning(binding.m_span, "declaration of '" + name_of(binding.m_name) + "' shadows a global variable");
            m_diag.note(global->m_span, "the global variable is declared here");
        }
    }

    SymbolBinding *TypeCheckerPass2::declare(SymbolBinding binding) {
        if (m_scopes.empty()) push_scope();
        if (m_warn_shadow) warn_if_shadowing(binding);
        m_scopes.back().push_back(std::move(binding));
        return &m_scopes.back().back();
    }

    SymbolBinding TypeCheckerPass2::make_binding(const SymbolID name, const TypeNode *type, const BindingKind kind,
                                                 const SourceSpan &span) {
        SymbolBinding binding;
        binding.m_name = name;
        binding.m_binding_kind = kind;
        binding.m_span = span;
        if (type != nullptr) {
            // `T?` has the access properties of `T` (a nullable pointer is still a pointer).
            const TypeNode *shape = type->m_kind == TypeNodeKind::Optional && type->m_inner != nullptr ? type->m_inner : type;
            binding.m_is_const_view = shape->is_const_view();
            binding.m_is_pointer = shape->m_kind == TypeNodeKind::Pointer;
            binding.m_is_view = shape->is_view();
            binding.m_type_name = nominal_name(type);
            binding.m_static_type = static_type_of(type);
        }
        return binding;
    }

    ClassDeclNode *TypeCheckerPass2::class_of_binding(const SymbolBinding &binding) const {
        if (binding.m_type_name == INVALID_SYMBOL_ID) return nullptr;
        return m_pass0.find_class(binding.m_type_name);
    }

    ConstTable TypeCheckerPass2::visible_constants() const {
        ConstTable table = m_pass0.definitions();
        // Walk the scopes outermost-first so that an inner declaration shadows an outer one: a local
        // `constx` contributes its value, any other binding hides a constant of the same name.
        for (const auto &scope: m_scopes) {
            for (const SymbolBinding &binding: scope) {
                if (binding.m_constant) table[binding.m_name] = *binding.m_constant;
                else table.erase(binding.m_name);
            }
        }
        return table;
    }

    bool TypeCheckerPass2::is_generic_parameter(const SymbolID name) const {
        if (m_current_fn != nullptr) {
            for (const GenericParam &param: m_current_fn->m_type_params) {
                if (param.m_name == name) return true;
            }
            for (const GenericArg &arg: m_current_fn->m_owner_args) {
                if (arg.m_type != nullptr && arg.m_type->m_kind == TypeNodeKind::Path &&
                    arg.m_type->m_path.size() == 1 && arg.m_type->m_path[0] == name)
                    return true;
            }
        }
        if (m_current_owner != nullptr) {
            for (const GenericParam &param: m_current_owner->m_type_params) {
                if (param.m_name == name) return true;
            }
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------
    // Constant / literal checks
    // ---------------------------------------------------------------------------------------------

    void TypeCheckerPass2::check_const_fits_type(const TypeNode *type, const ConstValue &value, const SourceSpan &span,
                                                 const std::string_view what) const {
        if (type == nullptr || type->m_kind != TypeNodeKind::Path || type->m_path.size() != 1) return;
        const std::string_view name = text_of(type->m_path[0]);

        int bits = 0;
        bool is_signed = false;
        if (name == "i8") {
            bits = 8;
            is_signed = true;
        } else if (name == "i16") {
            bits = 16;
            is_signed = true;
        } else if (name == "i32") {
            bits = 32;
            is_signed = true;
        } else if (name == "i64") {
            bits = 64;
            is_signed = true;
        } else if (name == "isize") {
            bits = m_pass0.target().m_pointer_width;
            is_signed = true;
        } else if (name == "u8") bits = 8;
        else if (name == "u16") bits = 16;
        else if (name == "u32") bits = 32;
        else if (name == "u64") bits = 64;
        else if (name == "usize") bits = m_pass0.target().m_pointer_width;

        const std::string subject = std::string(what) + " of type '" + std::string(name) + "'";
        if (bits != 0) {
            if (value.m_kind == ConstValue::Kind::Float) {
                m_diag.error(span, "cannot initialize " + subject + " with a floating-point value");
            } else if (value.m_kind == ConstValue::Kind::Bool) {
                m_diag.error(span, "cannot initialize " + subject + " with a boolean value");
            } else {
                const int64_t v = value.m_int;
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
        const size_t errors_before = m_diag.error_count();

        check_type_declarations();
        check_declaration_consistency();
        check_serializable_declarations();

        for (DeclNode *decl: m_pass0.active_declarations()) {
            use_file(m_diag, decl);
            if (const auto *var = dynamic_cast<VarDeclNode *>(decl)) {
                check_global_variable(var);
            } else if (const auto *fn = dynamic_cast<FunctionDeclNode *>(decl)) {
                check_extern_rules(fn);
                if (fn->m_has_body) {
                    const ClassDeclNode *owner = fn->m_owner != INVALID_SYMBOL_ID
                                                     ? m_pass0.find_class(fn->m_owner)
                                                     : nullptr;
                    check_function(fn, owner);
                }
            } else if (const auto *cls = dynamic_cast<ClassDeclNode *>(decl)) {
                for (const FunctionDeclNode *method: cls->m_methods) {
                    check_extern_rules(method);
                    if (method->m_has_body) check_function(method, cls);
                }
            }
        }
        return m_diag.error_count() == errors_before;
    }

    void TypeCheckerPass2::check_type_declarations() const {
        for (DeclNode *decl: m_pass0.active_declarations()) {
            use_file(m_diag, decl);
            if (const auto *cls = dynamic_cast<ClassDeclNode *>(decl)) {
                std::set<SymbolID> fields;
                for (const ParameterDecl &field: cls->m_fields) {
                    check_type_access(field.m_type, field.m_span);
                    if (!fields.insert(field.m_name).second) {
                        m_diag.error(field.m_span,
                                     "duplicate field '" + name_of(field.m_name) + "' in '" + name_of(cls->m_name) +
                                     "'");
                    }
                }
                std::set<SymbolID> methods;
                for (const FunctionDeclNode *method: cls->m_methods) {
                    if (!methods.insert(method->m_name).second) {
                        m_diag.error(method->m_span,
                                     "duplicate method '" + name_of(method->m_name) + "' in '" + name_of(cls->m_name) +
                                     "'");
                    }
                    if (fields.contains(method->m_name)) {
                        m_diag.error(method->m_span,
                                     "method '" + name_of(method->m_name) +
                                     "' conflicts with a field of the same name");
                    }
                }
            } else if (const auto *en = dynamic_cast<EnumDeclNode *>(decl)) {
                std::set<SymbolID> variants;
                for (const EnumVariant &variant: en->m_variants) {
                    if (!variants.insert(variant.m_name).second) {
                        m_diag.error(variant.m_span,
                                     "duplicate variant '" + name_of(variant.m_name) + "' in enum '" + name_of(
                                         en->m_name) + "'");
                    }
                }
                // `enum Mode: u8 { Read = 2, .. }`: values that are constant, fit the type and are unique
                const EnumValues values = evaluate_enum_values(*en, m_pass0.definitions(), m_pass0.target().m_pointer_width, m_diag);
                for (const EnumValueProblem &problem: values.m_problems) m_diag.error(problem.m_span, problem.m_message);
            }
        }
    }

    /// Interface (.fo) versus definition (.fl) agreement, redefinitions and owner checks.
    void TypeCheckerPass2::check_declaration_consistency() const {
        auto check_group = [&](const std::vector<FunctionDeclNode *> &group) {
            std::vector<const FunctionDeclNode *> plain;
            for (const FunctionDeclNode *fn: group) {
                if (fn->m_owner_args.empty()) plain.push_back(fn); // specializations are distinct entities
            }
            const FunctionDeclNode *first = nullptr;
            const FunctionDeclNode *first_body = nullptr;
            for (const FunctionDeclNode *fn: plain) {
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

        for (const auto &group: m_pass0.functions() | std::views::values) {
            check_group(group);
            for (const FunctionDeclNode *fn: group) {
                if (fn->m_receiver.m_kind != ReceiverKind::None) {
                    use_file(m_diag, fn);
                    m_diag.error(fn->m_span, "free function '" + name_of(fn->m_name) +
                                             "' has a 'this' receiver; declare it as 'fn Type::" + name_of(fn->m_name) +
                                             "'");
                }
            }
        }

        for (const auto &[owner, group]: m_pass0.methods()) {
            // Group per method name.
            std::unordered_map<SymbolID, std::vector<FunctionDeclNode *> > by_name;
            for (FunctionDeclNode *fn: group) by_name[fn->m_name].push_back(fn);

            const ClassDeclNode *cls = m_pass0.find_class(owner);
            const BlueprintDeclNode *blueprint = m_pass0.find_blueprint(owner);

            for (auto &[method_name, impls]: by_name) {
                // Implementations of one method (ignoring specializations) must be unique and consistent.
                std::vector<FunctionDeclNode *> with_declaration = impls;
                if (cls != nullptr) {
                    for (FunctionDeclNode *declared: cls->m_methods) {
                        if (declared->m_name == method_name) with_declaration.
                                insert(with_declaration.begin(), declared);
                    }
                }
                check_group(with_declaration);

                for (const FunctionDeclNode *impl: impls) {
                    use_file(m_diag, impl);
                    if (cls == nullptr && blueprint == nullptr && m_pass0.is_namespace(owner)) {
                        m_diag.error(impl->m_span, "functions of namespace '" + name_of(owner) + "' are written inside its block, not as 'fn " +
                                                   name_of(owner) + "::" + name_of(method_name) + "'");
                    }
                    if (cls != nullptr) {
                        bool declared = false;
                        for (const FunctionDeclNode *method: cls->m_methods)
                            declared = declared || method->m_name == method_name;
                        if (!declared) {
                            m_diag.error(impl->m_span,
                                         "method '" + name_of(method_name) + "' is not declared in class '" + name_of(
                                             owner) + "'");
                        }
                        if (impl->m_receiver.m_kind == ReceiverKind::None && !impl->m_has_body) continue;
                    } else if (blueprint != nullptr) {
                        bool declared = false;
                        for (const DeclNode *member: blueprint->m_declarations) {
                            const auto *member_fn = dynamic_cast<const FunctionDeclNode *>(member);
                            declared = declared || (member_fn != nullptr && member_fn->m_name == method_name);
                        }
                        if (!declared) {
                            m_diag.error(impl->m_span,
                                         "blueprint '" + name_of(owner) + "' has no function '" + name_of(method_name) +
                                         "'");
                        }
                        for (const GenericArg &arg: impl->m_owner_args) {
                            if (arg.m_name == INVALID_SYMBOL_ID) continue;
                            bool known = false;
                            for (const GenericParam &param: blueprint->m_type_params)
                                known = known || param.m_name == arg.m_name;
                            if (!known) {
                                m_diag.error(impl->m_span,
                                             "blueprint '" + name_of(owner) + "' has no parameter '" + name_of(
                                                 arg.m_name) + "'");
                            }
                        }
                    }
                }
            }
        }
    }

    void TypeCheckerPass2::check_global_variable(const VarDeclNode *var) {
        check_type_access(var->m_type, var->m_span);
        if (var->m_initializer == nullptr) return;
        m_namespace = var->m_namespace;
        struct ResetNamespace {
            SymbolID &m_ref;
            ~ResetNamespace() { m_ref = INVALID_SYMBOL_ID; }
        } reset{m_namespace};
        if (var->m_kind == BindingKind::Constx) {
            std::string key = var->m_namespace != INVALID_SYMBOL_ID ? name_of(var->m_namespace) + "::" : std::string();
            key += name_of(var->m_name);
            if (const auto it = m_pass0.definitions().find(StringInterner::get().intern(key)); it != m_pass0.definitions().end())
                check_const_fits_type(
                var->m_type, it->second, var->m_span, "constant");
            return;
        }
        const ConstEvaluator evaluator(m_pass0.definitions(), m_diag, m_pass0.target().m_pointer_width, m_namespace);
        if (const auto value = evaluator.evaluate(var->m_initializer, /*report=*/false)) {
            check_const_fits_type(var->m_type, *value, var->m_span, "variable");
        }
        push_scope();
        check_expression(var->m_initializer);
        pop_scope();
    }

    void TypeCheckerPass2::check_extern_rules(const FunctionDeclNode *fn) const {
        if (has_attribute(fn, "extern") && fn->m_has_body) {
            m_diag.error(fn->m_span, "'@extern' function '" + qualified_name(fn) + "' cannot have a body");
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Functions
    // ---------------------------------------------------------------------------------------------

    bool TypeCheckerPass2::check_function(const FunctionDeclNode *fn, const ClassDeclNode *owner) {
        const size_t errors_before = m_diag.error_count();
        m_current_fn = fn;
        m_current_owner = owner;
        m_namespace = fn->m_namespace;
        m_loop_depth = 0;
        for (const ParameterDecl &param: fn->m_params) check_type_access(param.m_type, param.m_span);
        check_type_access(fn->m_return_type, fn->m_span);
        m_scopes.clear();
        m_narrow_log.clear();
        m_narrow_marks.clear();
        push_scope();

        if (fn->m_receiver.m_kind != ReceiverKind::None) {
            SymbolBinding self;
            self.m_name = fn->m_receiver.m_self_name;
            self.m_span = fn->m_span;
            self.m_type_name = fn->m_owner != INVALID_SYMBOL_ID
                                   ? fn->m_owner
                                   : (owner != nullptr ? owner->m_name : INVALID_SYMBOL_ID);
            self.m_is_view = fn->m_receiver.m_kind != ReceiverKind::ByValue;
            self.m_is_const_view = fn->m_receiver.m_kind == ReceiverKind::ByConstRef;
            if (self.m_type_name != INVALID_SYMBOL_ID) {
                StaticType owner_type;
                owner_type.m_kind = StaticType::Kind::Named;
                owner_type.m_name = self.m_type_name;
                if (fn->m_receiver.m_kind == ReceiverKind::ByValue) {
                    self.m_static_type = owner_type;
                } else {
                    self.m_static_type.m_kind = StaticType::Kind::Reference;
                    self.m_static_type.m_const = fn->m_receiver.m_kind == ReceiverKind::ByConstRef;
                    self.m_static_type.m_pointee = std::make_shared<StaticType>(owner_type);
                }
            }
            declare(self);
        }

        for (const ParameterDecl &param: fn->m_params) {
            if (lookup_in_current_scope(param.m_name) != nullptr) {
                m_diag.error(param.m_span, "duplicate parameter name '" + name_of(param.m_name) + "'");
                continue;
            }
            declare(make_binding(param.m_name, param.m_type, BindingKind::Mutable, param.m_span));
            if (param.m_default_value != nullptr) {
                ConstEvaluator evaluator(m_pass0.definitions(), m_diag, m_pass0.target().m_pointer_width, m_namespace);
                if (auto value = evaluator.evaluate(param.m_default_value, /*report=*/false)) {
                    check_const_fits_type(param.m_type, *value, param.m_span, "parameter default");
                }
            }
        }

        check_statements(fn->m_body);

        if (const bool returns_value = fn->m_return_type != nullptr && fn->m_return_type->m_kind != TypeNodeKind::Never;
            fn->m_has_body && returns_value && !always_returns(fn->m_body)) {
            m_diag.error(fn->m_span, "function '" + qualified_name(fn) + "' does not return a value on all paths");
        }

        pop_scope();
        m_current_fn = nullptr;
        m_current_owner = nullptr;
        m_namespace = INVALID_SYMBOL_ID;
        return m_diag.error_count() == errors_before;
    }

    // ---------------------------------------------------------------------------------------------
    // Statements
    // ---------------------------------------------------------------------------------------------

    const ConditionalBranch *TypeCheckerPass2::select_branch(const ConditionalStmt *stmt, const bool report) const {
        const ConstEvaluator evaluator(m_pass0.definitions(), m_diag, m_pass0.target().m_pointer_width, m_namespace);
        for (const ConditionalBranch &branch: stmt->m_branches) {
            if (branch.m_condition == nullptr) return &branch; // @else
            const auto value = evaluator.evaluate(branch.m_condition, report);
            if (!value) return nullptr; // not a constant: reported above, do not guess
            if (value->truthy()) return &branch;
        }
        return nullptr;
    }

    void TypeCheckerPass2::check_statements(const std::vector<StmtNode *> &stmts) {
        for (const StmtNode *stmt: stmts) {
            if (const auto *conditional = dynamic_cast<const ConditionalStmt *>(stmt)) {
                // Compile-time @if: only the selected branch exists as far as later passes are concerned.
                if (const ConditionalBranch *branch = select_branch(conditional, true)) {
                    push_scope(); // a branch body is a block of its own, like `if constexpr` in C++
                    check_statements(branch->m_body);
                    pop_scope();
                }
                continue;
            }
            check_statement(stmt);
        }
    }

    void TypeCheckerPass2::check_block_scoped(const BlockStmt *block) {
        if (block == nullptr) return;
        push_scope();
        check_statements(block->m_statements);
        pop_scope();
    }

    void TypeCheckerPass2::check_statement(const StmtNode *stmt) {
        if (stmt == nullptr) return;

        if (const auto *ret = dynamic_cast<const ReturnStmt *>(stmt)) {
            if (ret->m_value != nullptr) check_expression(ret->m_value);
            if (m_current_fn == nullptr) return;
            const TypeNode *rt = m_current_fn->m_return_type;
            const bool is_void = rt == nullptr;
            const bool is_never = rt != nullptr && rt->m_kind == TypeNodeKind::Never;
            if (ret->m_value != nullptr && is_void) {
                m_diag.error(ret->m_span, "void function '" + qualified_name(m_current_fn) + "' cannot return a value");
            } else if (ret->m_value != nullptr && is_never) {
                m_diag.error(ret->m_span, "function '" + qualified_name(m_current_fn) + "' never returns");
            } else if (ret->m_value == nullptr && !is_void && !is_never) {
                m_diag.error(ret->m_span,
                             "non-void function '" + qualified_name(m_current_fn) + "' must return a value of type '" +
                             type_to_string(rt) + "'");
            } else if (ret->m_value != nullptr) {
                const size_t errors_before_const_checks = m_diag.error_count();
                const ConstTable constants = visible_constants(); // must outlive the evaluator, which keeps a reference
                const ConstEvaluator evaluator(constants, m_diag, m_pass0.target().m_pointer_width, m_namespace);
                if (const auto value = evaluator.evaluate(ret->m_value, /*report=*/false)) {
                    check_const_fits_type(rt, *value, ret->m_span, "return value");
                }
                if (m_diag.error_count() == errors_before_const_checks) {
                    check_assignable(static_type_of(rt), ret->m_value, "return value of '" + qualified_name(m_current_fn) + "'");
                }
            }
        } else if (const auto *expr_stmt = dynamic_cast<const ExprStmt *>(stmt)) {
            check_expression(expr_stmt->m_expr);
        } else if (const auto *decl_stmt = dynamic_cast<const DeclStmt *>(stmt)) {
            if (const auto *var = dynamic_cast<const VarDeclNode *>(decl_stmt->m_decl)) check_var_decl(var);
        } else if (const auto *block = dynamic_cast<const BlockStmt *>(stmt)) {
            check_block_scoped(block);
        } else if (const auto *if_stmt = dynamic_cast<const IfStmt *>(stmt)) {
            check_expression(if_stmt->m_condition);
            check_condition(if_stmt->m_condition, "if");
            std::vector<SymbolID> holds_value_then;
            std::vector<SymbolID> holds_value_else;
            collect_narrowing(if_stmt->m_condition, true, holds_value_then);
            collect_narrowing(if_stmt->m_condition, false, holds_value_else);

            size_t mark = m_narrow_log.size();
            narrow(holds_value_then);
            check_block_scoped(if_stmt->m_then);
            unnarrow_to(mark);
            if (if_stmt->m_else != nullptr) {
                narrow(holds_value_else);
                check_statement(if_stmt->m_else);
                unnarrow_to(mark);
            }
            // `if (!a) return;` leaves `a` narrowed for the rest of the enclosing block.
            const bool then_exits = stmt_always_exits(if_stmt->m_then);
            const bool else_exits = if_stmt->m_else != nullptr && stmt_always_exits(if_stmt->m_else);
            if (then_exits && !else_exits) narrow(holds_value_else);
            else if (else_exits && !then_exits) narrow(holds_value_then);
        } else if (const auto *match_stmt = dynamic_cast<const MatchStmt *>(stmt)) {
            check_match(match_stmt);
        } else if (const auto *while_stmt = dynamic_cast<const WhileStmt *>(stmt)) {
            kill_assigned_in(while_stmt->m_body); // later iterations see what the earlier ones assigned
            kill_assigned_in(while_stmt->m_condition);
            check_expression(while_stmt->m_condition);
            check_condition(while_stmt->m_condition, "while");
            std::vector<SymbolID> holds_value;
            collect_narrowing(while_stmt->m_condition, true, holds_value);
            const size_t mark = m_narrow_log.size();
            narrow(holds_value);
            ++m_loop_depth;
            check_block_scoped(while_stmt->m_body);
            --m_loop_depth;
            unnarrow_to(mark);
        } else if (const auto *for_stmt = dynamic_cast<const ForStmt *>(stmt)) {
            kill_assigned_in(for_stmt->m_body); // later iterations see what the earlier ones assigned
            kill_assigned_in(for_stmt->m_step);
            kill_assigned_in(for_stmt->m_condition);
            push_scope(); // the initializer's variable lives in a scope around the condition, the step and the body
            check_statement(for_stmt->m_init);
            std::vector<SymbolID> holds_value;
            if (for_stmt->m_condition != nullptr) {
                check_expression(for_stmt->m_condition);
                check_condition(for_stmt->m_condition, "for");
                collect_narrowing(for_stmt->m_condition, true, holds_value);
            }
            const size_t mark = m_narrow_log.size();
            narrow(holds_value);
            ++m_loop_depth;
            check_block_scoped(for_stmt->m_body); // the body is a block inside the loop-variable scope, like C++
            --m_loop_depth;
            unnarrow_to(mark);
            if (for_stmt->m_step != nullptr) check_expression(for_stmt->m_step);
            pop_scope();
        } else if (dynamic_cast<const BreakStmt *>(stmt) != nullptr) {
            if (m_loop_depth == 0) m_diag.error(stmt->m_span, "'break' outside of a loop");
        } else if (dynamic_cast<const ContinueStmt *>(stmt) != nullptr) {
            if (m_loop_depth == 0) m_diag.error(stmt->m_span, "'continue' outside of a loop");
        } else if (const auto *conditional = dynamic_cast<const ConditionalStmt *>(stmt)) {
            if (const ConditionalBranch *branch = select_branch(conditional, true)) {
                push_scope();
                check_statements(branch->m_body);
                pop_scope();
            }
        }
    }

    void TypeCheckerPass2::check_var_decl(const VarDeclNode *var) {
        if (var->m_initializer != nullptr) check_expression(var->m_initializer);
        check_type_access(var->m_type, var->m_span);

        const size_t errors_before_const_checks = m_diag.error_count();
        const ConstTable constants = visible_constants(); // must outlive the evaluator, which keeps a reference
        const ConstEvaluator evaluator(constants, m_diag, m_pass0.target().m_pointer_width, m_namespace);
        std::optional<ConstValue> constant;
        if (var->m_kind == BindingKind::Constx) {
            if (var->m_initializer != nullptr) {
                if (const auto value = evaluator.evaluate(var->m_initializer)) {
                    check_const_fits_type(var->m_type, *value, var->m_span, "constant");
                    constant = *value;
                }
            }
        } else if (var->m_type != nullptr && var->m_initializer != nullptr) {
            if (const auto value = evaluator.evaluate(var->m_initializer, /*report=*/false)) {
                check_const_fits_type(var->m_type, *value, var->m_span, "variable");
            }
        }
        // The constant checks above already explain a literal of the wrong kind; do not say it twice.
        if (var->m_type != nullptr && var->m_initializer != nullptr && m_diag.error_count() == errors_before_const_checks) {
            check_assignable(static_type_of(var->m_type), var->m_initializer, "initialization of '" + name_of(var->m_name) + "'");
        }

        if (var->m_type == nullptr && var->m_initializer != nullptr && infer_type(var->m_initializer).m_kind == StaticType::Kind::Null) {
            m_diag.error(var->m_initializer->m_span, "cannot infer the type of '" + name_of(var->m_name) +
                                                     "' from 'null'; declare it, e.g. 'Entity? " + name_of(var->m_name) + " = null;'");
        }
        if (lookup_in_current_scope(var->m_name) != nullptr) {
            m_diag.error(var->m_span, "redefinition of '" + name_of(var->m_name) + "' in this scope");
            return;
        }
        SymbolBinding binding = make_binding(var->m_name, var->m_type, var->m_kind, var->m_span);
        if (var->m_type == nullptr && var->m_initializer != nullptr) {
            if (const auto *lit = dynamic_cast<const StructLiteralExpr *>(var->m_initializer)) {
                binding.m_type_name = nominal_name(lit->m_type);
            } else if (const auto *cast = dynamic_cast<const CastExpr *>(var->m_initializer)) {
                binding.m_type_name = nominal_name(cast->m_target);
                binding.m_is_const_view = cast->m_target != nullptr && cast->m_target->is_const_view();
                binding.m_is_pointer = cast->m_target != nullptr && cast->m_target->m_kind == TypeNodeKind::Pointer;
                binding.m_is_view = cast->m_target != nullptr && cast->m_target->is_view();
            }
        }
        binding.m_constant = constant;
        binding.m_lambda = dynamic_cast<const LambdaExpr*>(var->m_initializer);
        if (var->m_type == nullptr && var->m_initializer != nullptr) binding.m_static_type = infer_type(var->m_initializer);
        // A `T?` initialized with a plain `T` starts out holding a value.
        if (var->m_type != nullptr && var->m_initializer != nullptr && binding.m_static_type.is_optional()) {
            const StaticType initial = infer_type(var->m_initializer);
            binding.m_narrowed = initial.known() && initial.m_kind != StaticType::Kind::Null && !initial.is_optional();
        }
        SymbolBinding *declared = declare(binding);
        if (declared->m_narrowed) { declared->m_narrowed = false; narrow({var->m_name}); }
    }

    void TypeCheckerPass2::check_match(const MatchStmt *match) {
        check_match_arms(match->m_subject, match->m_arms, match->m_span, nullptr);
    }

    void TypeCheckerPass2::check_match_expression(const MatchExpr *match) {
        check_match_arms(match->m_subject, match->m_arms, match->m_span, match);
    }

    /// The shared work of both forms of `match`. `value` is the expression form (arms give values, whose common type
    /// becomes the type of `value`), or null for the statement form (arms are blocks).
    void TypeCheckerPass2::check_match_arms(const ExprNode *subject_expr, const std::vector<MatchArm> &arms, const SourceSpan &match_span,
                                            const MatchExpr *value) {
        check_expression(subject_expr);
        std::vector<std::pair<const ExprNode *, StaticType> > arm_types;   // expression form: each arm's value and its type
        const auto check_arm_body = [&](const MatchArm &arm) {
            if (value == nullptr) {
                check_block_scoped(arm.m_body);
            } else if (arm.m_value != nullptr) {
                check_expression(arm.m_value);
                arm_types.emplace_back(arm.m_value, infer_type(arm.m_value));   // while the payload names are still in scope
            }
        };

        const EnumDeclNode *enum_decl = nullptr;
        SymbolID enum_name = INVALID_SYMBOL_ID;
        for (const MatchArm &arm: arms) {
            if (arm.m_wildcard || arm.m_path.size() != 2) continue;
            enum_name = arm.m_path[0];
            enum_decl = m_pass0.find_enum(enum_name);
            break;
        }

        // The value must be of the enum the arms name (when the checker can tell what it is).
        if (enum_decl != nullptr) {
            const StaticType subject = infer_type(subject_expr);
            using Kind = StaticType::Kind;
            const bool plain_other = subject.m_kind == Kind::Int || subject.m_kind == Kind::Float || subject.m_kind == Kind::Bool ||
                                     subject.m_kind == Kind::Void || subject.m_kind == Kind::Null || subject.m_kind == Kind::Function;
            if (plain_other || (subject.m_kind == Kind::Named && subject.m_name != enum_name)) {
                m_diag.error(subject_expr->m_span, "match value has type '" + describe(subject) + "', but its arms name enum '" +
                                                       name_of(enum_name) + "'");
            }
        }

        std::set<SymbolID> handled;
        bool wildcard_seen = false;
        for (const MatchArm &arm: arms) {
            if (wildcard_seen) {
                m_diag.error(arm.m_span, "this arm can never run: an earlier '_' arm already matches everything");
                continue;
            }
            if (arm.m_wildcard) {
                wildcard_seen = true;
                check_arm_body(arm);
                continue;
            }
            if (arm.m_path.size() != 2) { // the parser already said so
                check_arm_body(arm);
                continue;
            }

            const EnumDeclNode *arm_enum = m_pass0.find_enum(arm.m_path[0]);
            const EnumVariant *variant = nullptr;
            if (arm_enum == nullptr) {
                m_diag.error(arm.m_span, "'" + name_of(arm.m_path[0]) + "' is not an enum, so '" + name_of(arm.m_path[0]) + "::" +
                                         name_of(arm.m_path[1]) + "' cannot be matched");
            } else if (arm_enum != enum_decl) {
                m_diag.error(arm.m_span, "all arms of a match must name the same enum: expected '" + name_of(enum_name) + "', found '" +
                                         name_of(arm.m_path[0]) + "'");
            } else {
                check_access(arm_enum, arm.m_span, "type", name_of(arm.m_path[0]));
                for (const EnumVariant &candidate: arm_enum->m_variants) {
                    if (candidate.m_name == arm.m_path[1]) variant = &candidate;
                }
                if (variant == nullptr) {
                    m_diag.error(arm.m_span, "enum '" + name_of(arm.m_path[0]) + "' has no variant '" + name_of(arm.m_path[1]) + "'");
                } else if (!handled.insert(variant->m_name).second) {
                    m_diag.error(arm.m_span, "variant '" + name_of(arm.m_path[0]) + "::" + name_of(arm.m_path[1]) +
                                             "' is already handled by an earlier arm");
                }
            }

            if (variant != nullptr && arm.m_has_bindings && arm.m_bindings.size() != variant->m_payload.size()) {
                m_diag.error(arm.m_span, "variant '" + name_of(arm.m_path[0]) + "::" + name_of(arm.m_path[1]) + "' carries " +
                                         std::to_string(variant->m_payload.size()) + " value(s), but the pattern binds " +
                                         std::to_string(arm.m_bindings.size()));
            }

            push_scope();   // the payload names live in a scope around the arm's block
            if (variant != nullptr && arm.m_bindings.size() == variant->m_payload.size()) {
                const bool generic = !arm_enum->m_type_params.empty();   // payload types name type parameters: left unknown
                for (size_t i = 0; i < arm.m_bindings.size(); ++i) {
                    const MatchBinding &binding = arm.m_bindings[i];
                    if (binding.m_ignored) continue;
                    if (lookup_in_current_scope(binding.m_name) != nullptr) {
                        m_diag.error(binding.m_span, "'" + name_of(binding.m_name) + "' is bound twice in this pattern");
                        continue;
                    }
                    declare(make_binding(binding.m_name, generic ? nullptr : variant->m_payload[i], BindingKind::Immutable, binding.m_span));
                }
            } else {
                for (const MatchBinding &binding: arm.m_bindings) {   // arity error: still declare the names so the body is checked
                    if (!binding.m_ignored && lookup_in_current_scope(binding.m_name) == nullptr) {
                        declare(make_binding(binding.m_name, nullptr, BindingKind::Immutable, binding.m_span));
                    }
                }
            }
            check_arm_body(arm);
            pop_scope();
        }

        if (!wildcard_seen && enum_decl != nullptr) {
            std::string missing;
            for (const EnumVariant &variant: enum_decl->m_variants) {
                if (handled.count(variant.m_name) != 0) continue;
                missing += (missing.empty() ? "" : ", ") + name_of(enum_name) + "::" + name_of(variant.m_name);
            }
            if (!missing.empty()) {
                m_diag.error(match_span, "match is not exhaustive: missing " + missing + " (or add a '_' arm)");
            }
        }

        if (value == nullptr) return;

        // Expression form: the first arm with a known type (a plain value rather than `null`, a typed one rather than a
        // literal) sets the type of the whole `match`, and every other arm must convert to it.
        using Kind = StaticType::Kind;
        const StaticType *result = nullptr;
        for (const auto &entry: arm_types) {
            const StaticType &type = entry.second;
            if (!type.known() || type.m_kind == Kind::Null) continue;
            if (result == nullptr || (result->m_literal && !type.m_literal)) result = &type;
            if (!result->m_literal) break;
        }
        StaticType merged;
        if (result != nullptr) {
            merged = *result;
            for (const auto &[node, type]: arm_types) {
                if (!type.known() || &type == result) continue;
                const std::string reason = conversion_error(merged, type);
                if (!reason.empty()) m_diag.error(node->m_span, "the arms of a match give different types: " + reason);
            }
        } else if (!arm_types.empty() && arm_types.front().second.m_kind == Kind::Null) {
            merged = arm_types.front().second;
        }
        m_expr_types[value] = merged;   // assign, not emplace: an earlier query may have cached 'unknown'
    }

    bool TypeCheckerPass2::always_returns(const std::vector<StmtNode *> &stmts) const {
        for (const StmtNode *stmt: stmts) {
            if (stmt_always_returns(stmt)) return true;
        }
        return false;
    }

    bool TypeCheckerPass2::stmt_always_returns(const StmtNode *stmt) const {
        if (stmt == nullptr) return false;
        if (dynamic_cast<const ReturnStmt *>(stmt) != nullptr) return true;
        if (const auto *block = dynamic_cast<const BlockStmt *>(stmt)) return always_returns(block->m_statements);
        if (const auto *if_stmt = dynamic_cast<const IfStmt *>(stmt)) {
            return if_stmt->m_else != nullptr && stmt_always_returns(if_stmt->m_then) && stmt_always_returns(
                       if_stmt->m_else);
        }
        if (const auto *match = dynamic_cast<const MatchStmt *>(stmt)) {
            // Exhaustiveness is checked where the match is checked, so every arm returning is enough here.
            if (match->m_arms.empty()) return false;
            for (const MatchArm &arm: match->m_arms) {
                if (!stmt_always_returns(arm.m_body)) return false;
            }
            return true;
        }
        if (const auto *conditional = dynamic_cast<const ConditionalStmt *>(stmt)) {
            const ConditionalBranch *branch = select_branch(conditional, /*report=*/false);
            return branch != nullptr && always_returns(branch->m_body);
        }
        if (const auto *expr_stmt = dynamic_cast<const ExprStmt *>(stmt)) {
            // A call to a function declared `-> !` never comes back.
            if (const auto *call = dynamic_cast<const CallExpr *>(expr_stmt->m_expr)) {
                const FunctionDeclNode *callee = resolve_callee(call);
                return callee != nullptr && callee->m_return_type != nullptr &&
                       callee->m_return_type->m_kind == TypeNodeKind::Never;
            }
        }
        return false;
    }
} // namespace fool
