//
// Created by jmbab on 05-11-2026.
//

/**
 * @file SymbolTable.cpp
 * @brief Pass 0: target configuration, @target pruning, top-level @if selection, symbol collection.
 */

module;

#include <cstddef>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }

std::string_view last_segment(std::string_view module_path) {
    size_t pos = module_path.rfind("::");
    return pos == std::string_view::npos ? module_path : module_path.substr(pos + 2);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TargetConfig
// ---------------------------------------------------------------------------------------------

bool TargetConfig::os_matches(std::string_view value) const {
    if (value == m_os) return true;
    if (value == "posix" || value == "unix") {
        return m_os == "linux" || m_os == "darwin" || m_os == "freebsd" || m_os == "openbsd" ||
               m_os == "netbsd" || m_os == "android";
    }
    return false;
}

bool TargetConfig::arch_matches(std::string_view value) const { return value == m_arch; }

bool TargetConfig::file_suffix_applies(std::string_view suffix) const {
    if (suffix == "linux" || suffix == "epoll") return m_os == "linux";
    if (suffix == "darwin" || suffix == "kqueue") return m_os == "darwin";
    if (suffix == "windows" || suffix == "iocp") return m_os == "windows";
    if (suffix == "posix") return os_matches("posix");
    if (suffix == "wasi") return m_os == "wasi";
    if (suffix == "wasm") return m_arch == "wasm32";
    return true; // not a platform suffix: the file is portable
}

bool TargetConfig::file_applies(const std::filesystem::path& path) const {
    std::string stem = path.stem().string();
    size_t underscore = stem.rfind('_');
    if (underscore == std::string::npos) return true;
    return file_suffix_applies(std::string_view(stem).substr(underscore + 1));
}

// ---------------------------------------------------------------------------------------------
// type_to_string
// ---------------------------------------------------------------------------------------------

std::string type_to_string(const TypeNode* type) {
    if (type == nullptr) return "void";
    switch (type->m_kind) {
        case TypeNodeKind::Never:
            return "!";
        case TypeNodeKind::Optional:
            return type_to_string(type->m_inner) + "?";
        case TypeNodeKind::Function: {
            std::string text = "fn(";
            for (size_t i = 0; i < type->m_param_types.size(); ++i) text += (i ? ", " : "") + type_to_string(type->m_param_types[i]);
            text += ")";
            if (type->m_inner != nullptr) text += " -> " + type_to_string(type->m_inner);
            return text;
        }
        case TypeNodeKind::Pointer:
            return std::string(type->m_is_const ? "const* " : "*") + type_to_string(type->m_inner);
        case TypeNodeKind::Reference:
            return std::string(type->m_is_const ? "const& " : "&") + type_to_string(type->m_inner);
        case TypeNodeKind::Path: {
            std::string out;
            for (size_t i = 0; i < type->m_path.size(); ++i) {
                if (i != 0) out += "::";
                out += std::string(text_of(type->m_path[i]));
            }
            if (!type->m_generic_args.empty()) {
                out += "<";
                for (size_t i = 0; i < type->m_generic_args.size(); ++i) {
                    if (i != 0) out += ", ";
                    const GenericArg& arg = type->m_generic_args[i];
                    if (arg.m_type != nullptr) out += type_to_string(arg.m_type);
                    else out += std::string(text_of(arg.m_name)) + "=_";
                }
                out += ">";
            }
            return out;
        }
    }
    return "?";
}

std::string unit_key(std::string_view file) {
    std::filesystem::path path = std::filesystem::path(std::string(file)).lexically_normal();
    std::string stem = path.stem().string();
    const size_t underscore = stem.rfind('_');
    if (underscore != std::string::npos) {
        const std::string_view suffix = std::string_view(stem).substr(underscore + 1);
        for (std::string_view platform : {"linux", "epoll", "darwin", "kqueue", "windows", "iocp", "posix", "wasi", "wasm"}) {
            if (suffix == platform) { stem.resize(underscore); break; }
        }
    }
    return (path.parent_path() / stem).generic_string();
}

namespace {

SymbolID qualify(SymbolID ns, SymbolID name) {
    if (ns == INVALID_SYMBOL_ID) return name;
    return StringInterner::get().intern(std::string(text_of(ns)) + "::" + std::string(text_of(name)));
}

SymbolID join_path(const std::vector<SymbolID>& path, size_t from) {
    std::string text;
    for (size_t i = from; i < path.size(); ++i) {
        if (i != from) text += "::";
        text += std::string(text_of(path[i]));
    }
    return StringInterner::get().intern(text);
}

SymbolID parent_namespace(SymbolID ns) {
    const std::string_view text = text_of(ns);
    const size_t cut = text.rfind("::");
    if (cut == std::string_view::npos) return INVALID_SYMBOL_ID;
    return StringInterner::get().intern(std::string(text.substr(0, cut)));
}

void assign_file_recursive(const std::vector<DeclNode*>& decls, std::string_view file) {
    for (DeclNode* decl : decls) {
        if (decl == nullptr) continue;
        decl->m_file = file;
        if (auto* ns = dynamic_cast<NamespaceDeclNode*>(decl)) assign_file_recursive(ns->m_declarations, file);
        if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
            for (FunctionDeclNode* method : cls->m_methods) method->m_file = file;
        }
        if (auto* block = dynamic_cast<ConditionalDeclBlock*>(decl)) {
            for (const ConditionalDeclBranch& branch : block->m_branches) {
                assign_file_recursive(branch.m_declarations, file);
            }
        }
    }
}

} // namespace

void assign_source_file(ModuleAST* module, std::string_view path) {
    auto& interner = StringInterner::get();
    assign_file_recursive(module->m_declarations, interner.resolve(interner.intern(path)));
}

// ---------------------------------------------------------------------------------------------
// SymbolTablePass0
// ---------------------------------------------------------------------------------------------

SymbolTablePass0::SymbolTablePass0(TargetConfig target, DiagnosticEngine& diag)
    : m_target(std::move(target)), m_diag(diag) {}

void SymbolTablePass0::define_parameters(const std::vector<ModuleParam>& params, const ConstTable& overrides) {
    ConstEvaluator evaluator(m_definitions, m_diag, m_target.m_pointer_width);
    for (const ModuleParam& param : params) {
        auto override_it = overrides.find(param.m_name);
        if (override_it != overrides.end()) {
            m_definitions[param.m_name] = override_it->second;
            continue;
        }
        if (param.m_default_value == nullptr) {
            m_diag.error(SourceSpan{}, "module parameter '" + std::string(text_of(param.m_name)) +
                                       "' has no default value and no argument was supplied");
            continue;
        }
        if (auto value = evaluator.evaluate(param.m_default_value)) m_definitions[param.m_name] = *value;
    }
}

void SymbolTablePass0::validate_attributes(const DeclNode* decl) {
    for (const AttributeNode& attr : decl->m_attributes) {
        std::string_view name = text_of(attr.m_name);
        if (name == "target") {
            for (const AttributeArg& arg : attr.m_args) {
                if (arg.m_key == INVALID_SYMBOL_ID) {
                    m_diag.error(attr.m_span, "@target arguments must be 'key = \"value\"' pairs");
                    continue;
                }
                std::string_view key = text_of(arg.m_key);
                if (key != "os" && key != "arch" && key != "pointer_width") {
                    m_diag.error(attr.m_span, "unknown @target key '" + std::string(key) +
                                              "' (expected os, arch or pointer_width)");
                }
            }
        } else if (name == "extern") {
            if (attr.m_args.empty()) {
                m_diag.error(attr.m_span, "@extern requires an ABI name, e.g. @extern(\"C\")");
            }
        } else if (name == "serialize") {
            if (dynamic_cast<const ClassDeclNode*>(decl) == nullptr && dynamic_cast<const EnumDeclNode*>(decl) == nullptr) {
                m_diag.error(attr.m_span, "$serialize applies to classes, structs and enums");
            }
            for (const AttributeArg& arg : attr.m_args) {
                if (arg.m_key == INVALID_SYMBOL_ID || text_of(arg.m_key) != "format") {
                    m_diag.error(attr.m_span, "$serialize accepts only 'format = \"json\"', 'format = \"binary\"' or 'format = \"text\"'");
                } else if (arg.m_value != "json" && arg.m_value != "binary" && arg.m_value != "text") {
                    m_diag.error(attr.m_span, "unknown $serialize format '" + std::string(arg.m_value) + "' (expected json, binary or text)");
                }
            }
        } else if (name != "inline" && name != "align_as") {
            m_diag.error(attr.m_span, "unknown attribute '@" + std::string(name) +
                                      "' (expected target, extern, inline or align_as)");
        }
    }
}

/// Several `@target` attributes are alternatives (OR); the keys inside one `@target` must all match (AND).
bool SymbolTablePass0::evaluate_target_attributes(const DeclNode* decl) {
    bool saw_target = false;
    for (const AttributeNode& attr : decl->m_attributes) {
        if (text_of(attr.m_name) != "target") continue;
        saw_target = true;
        bool all_match = true;
        for (const AttributeArg& arg : attr.m_args) {
            if (arg.m_key == INVALID_SYMBOL_ID) continue; // already reported by validate_attributes
            std::string_view key = text_of(arg.m_key);
            if (key == "os") all_match = all_match && m_target.os_matches(arg.m_value);
            else if (key == "arch") all_match = all_match && m_target.arch_matches(arg.m_value);
            else if (key == "pointer_width")
                all_match = all_match && (std::strtol(std::string(arg.m_value).c_str(), nullptr, 10) == m_target.m_pointer_width);
        }
        if (all_match) return true;
    }
    return !saw_target;
}

bool SymbolTablePass0::select_branch_conditions(const std::vector<ExprNode*>& conditions, size_t& chosen) {
    ConstEvaluator evaluator(m_definitions, m_diag, m_target.m_pointer_width);
    for (size_t i = 0; i < conditions.size(); ++i) {
        if (conditions[i] == nullptr) { // @else
            chosen = i;
            return true;
        }
        auto value = evaluator.evaluate(conditions[i]);
        if (!value) return false; // error already reported: do not guess a branch
        if (value->truthy()) {
            chosen = i;
            return true;
        }
    }
    chosen = conditions.size();
    return true;
}

void SymbolTablePass0::collect_declarations(const ModuleAST* module, SymbolID section) {
    for (DeclNode* decl : module->m_declarations) {
        collect_one(decl, section);
    }
}

void SymbolTablePass0::collect_one(DeclNode* decl, SymbolID section) {
    if (decl == nullptr) return;
    if (section != INVALID_SYMBOL_ID && decl->m_module != section) return;
    if (!decl->m_file.empty()) m_diag.set_current_file(std::string(decl->m_file));

    validate_attributes(decl);

    if (auto* block = dynamic_cast<ConditionalDeclBlock*>(decl)) {
        if (!evaluate_target_attributes(block)) {
            ++m_pruned_count;
            return;
        }
        std::vector<ExprNode*> conditions;
        for (const ConditionalDeclBranch& branch : block->m_branches) conditions.push_back(branch.m_condition);
        size_t chosen = conditions.size();
        if (!select_branch_conditions(conditions, chosen)) return;
        for (size_t i = 0; i < block->m_branches.size(); ++i) {
            if (i == chosen) {
                for (DeclNode* child : block->m_branches[i].m_declarations) collect_one(child, INVALID_SYMBOL_ID);
            } else {
                m_pruned_count += block->m_branches[i].m_declarations.size();
            }
        }
        return;
    }

    if (!evaluate_target_attributes(decl)) {
        ++m_pruned_count;
        return;
    }
    if (auto* ns = dynamic_cast<NamespaceDeclNode*>(decl)) {
        // A namespace is only a scope: its functions and constants join the module under `ns::name`.
        m_namespaces.insert(qualify(ns->m_namespace, ns->m_name));
        for (DeclNode* child : ns->m_declarations) collect_one(child, INVALID_SYMBOL_ID);
        return;
    }
    m_active_decls.push_back(decl);
    register_declaration(decl);
}

void SymbolTablePass0::register_declaration(DeclNode* decl) {
    auto redefinition = [&](const char* what, SymbolID name) {
        m_diag.error(decl->m_span, std::string("redefinition of ") + what + " '" + std::string(text_of(name)) + "'");
    };

    if (auto* fn = dynamic_cast<FunctionDeclNode*>(decl)) {
        const SymbolID key = fn->m_owner != INVALID_SYMBOL_ID ? fn->m_name : qualify(fn->m_namespace, fn->m_name);
        if (fn->m_is_pub) m_pub_functions.emplace(fn->m_owner, key);
        if (fn->m_owner != INVALID_SYMBOL_ID) m_methods[fn->m_owner].push_back(fn);
        else m_functions[key].push_back(fn);
    } else if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
        if (!m_classes.emplace(cls->m_name, cls).second) redefinition(cls->m_is_struct ? "struct" : "class", cls->m_name);
    } else if (auto* ruleset = dynamic_cast<RulesetDeclNode*>(decl)) {
        if (!m_rulesets.emplace(ruleset->m_name, ruleset).second) redefinition("ruleset", ruleset->m_name);
    } else if (auto* blueprint = dynamic_cast<BlueprintDeclNode*>(decl)) {
        if (!m_blueprints.emplace(blueprint->m_name, blueprint).second) redefinition("blueprint", blueprint->m_name);
    } else if (auto* en = dynamic_cast<EnumDeclNode*>(decl)) {
        if (!m_enums.emplace(en->m_name, en).second) redefinition("enum", en->m_name);
    } else if (auto* import_decl = dynamic_cast<ImportDeclNode*>(decl)) {
        m_import_decls.push_back(import_decl);
    } else if (auto* var = dynamic_cast<VarDeclNode*>(decl)) {
        const SymbolID key = qualify(var->m_namespace, var->m_name);
        if (var->m_is_pub) m_pub_globals.insert(key);
        auto existing = m_globals.find(key);
        if (existing != m_globals.end()) {
            // A constant may be repeated in the interface (.fo) and the implementation (.fl) as
            // long as both declarations agree on kind, type and value.
            const VarDeclNode* first = existing->second;
            bool same = first->m_kind == var->m_kind && type_to_string(first->m_type) == type_to_string(var->m_type);
            if (same && var->m_kind == BindingKind::Constx && var->m_initializer != nullptr) {
                DiagnosticEngine scratch;
                scratch.set_echo(false);
                ConstEvaluator evaluator(m_definitions, scratch, m_target.m_pointer_width, var->m_namespace);
                auto value = evaluator.evaluate(var->m_initializer);
                auto previous = m_definitions.find(key);
                same = value.has_value() && previous != m_definitions.end() &&
                       value->to_string() == previous->second.to_string();
            } else if (same) {
                same = false; // only constants may be declared twice
            }
            if (!same) redefinition("variable", key);
            return;
        }
        m_globals.emplace(key, var);
        if (var->m_kind == BindingKind::Constx && var->m_initializer != nullptr) {
            ConstEvaluator evaluator(m_definitions, m_diag, m_target.m_pointer_width, var->m_namespace);
            if (auto value = evaluator.evaluate(var->m_initializer)) m_definitions[key] = *value;
        }
    }
}

bool ImportedModule::admits_name(SymbolID key) const {
    if (!m_selective) return true;
    const std::string_view text = text_of(key);
    const size_t cut = text.find("::");
    return m_only.find(cut == std::string_view::npos ? text : text.substr(0, cut)) != m_only.end();
}

bool ImportedModule::admits_file(std::string_view file) const {
    if (m_units.empty() || file.empty()) return true;
    auto cached = m_file_cache.find(file);
    if (cached != m_file_cache.end()) return cached->second;
    const bool admitted = m_units.count(unit_key(file)) != 0;
    m_file_cache.emplace(file, admitted);
    return admitted;
}

namespace {

/// The view of `inner` (something an imported module re-exports) as seen through the import `outer`.
ImportedModule compose(const ImportedModule& outer, const ImportedModule& inner) {
    ImportedModule view;
    view.m_name = inner.m_name;
    view.m_table = inner.m_table;
    view.m_decl = outer.m_decl;
    view.m_alias = outer.m_alias;
    view.m_units = inner.m_units;
    view.m_is_pub = outer.m_is_pub;
    view.m_direct = false;
    if (outer.m_selective && inner.m_selective) {
        view.m_selective = true;
        for (const std::string& name : outer.m_only) {
            if (inner.m_only.count(name) != 0) view.m_only.insert(name);
        }
    } else if (outer.m_selective || inner.m_selective) {
        view.m_selective = true;
        view.m_only = outer.m_selective ? outer.m_only : inner.m_only;
    }
    return view;
}

} // namespace

void SymbolTablePass0::add_import(const ImportDeclNode* decl, SymbolID module, const SymbolTablePass0* table,
                                  std::set<std::string> units) {
    ImportedModule imp;
    imp.m_name = module;
    imp.m_table = table;
    imp.m_decl = decl;
    imp.m_alias = decl->m_segments.empty() ? INVALID_SYMBOL_ID : decl->m_segments.back();
    imp.m_units = std::move(units);
    imp.m_selective = decl->m_selective;
    for (SymbolID name : decl->m_names) imp.m_only.emplace(text_of(name));
    imp.m_is_pub = decl->m_is_pub;
    m_imports.push_back(imp);
    m_views.push_back(imp);
    if (imp.m_is_pub) m_exports.push_back(imp);
    if (table == nullptr) return;
    for (const ImportedModule& inner : table->m_exports) {
        m_views.push_back(compose(imp, inner));
        if (imp.m_is_pub) m_exports.push_back(compose(imp, inner));
    }
}

void SymbolTablePass0::check_selective_imports() const {
    std::map<const ImportDeclNode*, std::vector<const ImportedModule*>> groups;
    for (const ImportedModule& view : m_views) {
        if (view.m_decl != nullptr && view.m_decl->m_selective) groups[view.m_decl].push_back(&view);
    }
    for (const auto& [decl, views] : groups) {
        const std::string saved = m_diag.current_file();
        m_diag.set_current_file(std::string(decl->m_file));
        for (SymbolID name : decl->m_names) {
            bool exists = false;
            bool usable = false;
            const DeclNode* private_hit = nullptr;
            for (const ImportedModule* view : views) {
                if (view->m_table == nullptr) { exists = usable = true; break; }   // unresolved: reported elsewhere
                if (!view->admits_name(name)) continue;
                const SymbolTablePass0& table = *view->m_table;
                // Is `name` declared by this module, in a file the import covers? (A namespace has no single declaration.)
                bool hit_exists = false;
                const DeclNode* hit_decl = nullptr;
                auto fn = table.m_functions.find(name);
                if (fn != table.m_functions.end()) {
                    for (const FunctionDeclNode* f : fn->second) {
                        if (view->admits_file(f->m_file)) { hit_exists = true; hit_decl = f; break; }
                    }
                }
                for (const DeclNode* other : {static_cast<const DeclNode*>(table.find_local_class(name)),
                                              static_cast<const DeclNode*>(table.find_local_enum(name)),
                                              static_cast<const DeclNode*>(table.find_local_ruleset(name)),
                                              static_cast<const DeclNode*>(table.find_local_blueprint(name)),
                                              static_cast<const DeclNode*>(table.find_local_global(name))}) {
                    if (hit_exists || other == nullptr || !view->admits_file(other->m_file)) continue;
                    hit_exists = true;
                    hit_decl = other;
                }
                if (!hit_exists && table.m_namespaces.count(name) != 0) hit_exists = true;
                if (!hit_exists) continue;
                exists = true;
                if (hit_decl == nullptr || table.is_accessible(hit_decl, decl->m_file)) { usable = true; break; }
                private_hit = hit_decl;
            }
            const std::string spelled(text_of(name));
            if (!exists) {
                m_diag.error(decl->m_span, "'" + spelled + "' is not declared in '" + std::string(text_of(decl->m_module_path)) + "'");
            } else if (!usable && private_hit != nullptr) {
                m_diag.error(decl->m_span, "'" + spelled + "' is private to '" + std::filesystem::path(std::string(private_hit->m_file)).filename().string() +
                                           "'; declare it `pub` to import it");
                m_diag.set_current_file(std::string(private_hit->m_file));
                m_diag.note(private_hit->m_span, "'" + spelled + "' is declared here without `pub`");
                m_diag.set_current_file(std::string(decl->m_file));
            }
        }
        m_diag.set_current_file(saved);
    }
}

// --- lookups (local first, then the imports and what they re-export)

namespace {

bool alias_matches(const ImportedModule& imp, std::string_view alias_text) {
    if (imp.m_alias != INVALID_SYMBOL_ID && text_of(imp.m_alias) == alias_text) return true;
    const std::string_view full = text_of(imp.m_name);
    return full == alias_text || last_segment(full) == alias_text;
}

} // namespace

FunctionDeclNode* SymbolTablePass0::find_function(SymbolID name) const {
    auto it = m_functions.find(name);
    if (it != m_functions.end() && !it->second.empty()) return it->second.front();
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_functions.find(name);
        if (imp_it == imp.m_table->m_functions.end()) continue;
        for (FunctionDeclNode* fn : imp_it->second) {
            if (imp.admits(name, fn)) return fn;
        }
    }
    return nullptr;
}

FunctionDeclNode* SymbolTablePass0::find_method(SymbolID owner, SymbolID name) const {
    auto search = [&](const SymbolTablePass0& table, const ImportedModule* view) -> FunctionDeclNode* {
        if (view != nullptr && !view->admits_name(owner)) return nullptr;
        auto cls = table.m_classes.find(owner);
        const bool class_ok = cls != table.m_classes.end() && (view == nullptr || view->admits_file(cls->second->m_file));
        if (class_ok) {
            for (FunctionDeclNode* method : cls->second->m_methods) {
                if (method->m_name == name) return method;
            }
        }
        auto impls = table.m_methods.find(owner);
        if (impls != table.m_methods.end()) {
            for (FunctionDeclNode* method : impls->second) {
                if (method->m_name != name) continue;
                if (view == nullptr || class_ok || view->admits_file(method->m_file)) return method;
            }
        }
        return nullptr;
    };
    if (FunctionDeclNode* local = search(*this, nullptr)) return local;
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr) continue;
        if (FunctionDeclNode* found = search(*imp.m_table, &imp)) return found;
    }
    return nullptr;
}

#define FOOL_FIND_IN_VIEWS(MAP, NAME)                                                  \
    for (const ImportedModule& imp : m_views) {                                         \
        if (imp.m_table == nullptr) continue;                                           \
        auto imp_it = imp.m_table->MAP.find(NAME);                                      \
        if (imp_it != imp.m_table->MAP.end() && imp.admits(NAME, imp_it->second)) return imp_it->second; \
    }

ClassDeclNode* SymbolTablePass0::find_class(SymbolID name) const {
    auto it = m_classes.find(name);
    if (it != m_classes.end()) return it->second;
    FOOL_FIND_IN_VIEWS(m_classes, name)
    return nullptr;
}

RulesetDeclNode* SymbolTablePass0::find_ruleset(SymbolID name) const {
    auto it = m_rulesets.find(name);
    if (it != m_rulesets.end()) return it->second;
    FOOL_FIND_IN_VIEWS(m_rulesets, name)
    return nullptr;
}

VarDeclNode* SymbolTablePass0::find_global(SymbolID name) const {
    auto it = m_globals.find(name);
    if (it != m_globals.end()) return it->second;
    FOOL_FIND_IN_VIEWS(m_globals, name)
    return nullptr;
}

BlueprintDeclNode* SymbolTablePass0::find_blueprint(SymbolID name) const {
    auto it = m_blueprints.find(name);
    if (it != m_blueprints.end()) return it->second;
    FOOL_FIND_IN_VIEWS(m_blueprints, name)
    return nullptr;
}

EnumDeclNode* SymbolTablePass0::find_enum(SymbolID name) const {
    auto it = m_enums.find(name);
    if (it != m_enums.end()) return it->second;
    FOOL_FIND_IN_VIEWS(m_enums, name)
    return nullptr;
}

#undef FOOL_FIND_IN_VIEWS

bool SymbolTablePass0::is_known_global(SymbolID name) const {
    auto known_locally = [&](const SymbolTablePass0& t) {
        return t.m_functions.count(name) != 0 || t.m_classes.count(name) != 0 || t.m_rulesets.count(name) != 0 ||
               t.m_blueprints.count(name) != 0 || t.m_enums.count(name) != 0 || t.m_globals.count(name) != 0 ||
               t.m_definitions.count(name) != 0;
    };
    if (known_locally(*this)) return true;
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table != nullptr && imp.admits_name(name) && known_locally(*imp.m_table)) return true;
    }
    return false;
}

bool SymbolTablePass0::is_import_alias(SymbolID alias) const {
    const std::string_view alias_text = text_of(alias);
    for (const ImportedModule& imp : m_views) {
        if (alias_matches(imp, alias_text)) return true;
    }
    return false;
}

FunctionDeclNode* SymbolTablePass0::find_function_in_import(SymbolID alias, SymbolID name) const {
    const std::string_view alias_text = text_of(alias);
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr || !alias_matches(imp, alias_text)) continue;
        auto it = imp.m_table->m_functions.find(name);
        if (it == imp.m_table->m_functions.end()) continue;
        for (FunctionDeclNode* fn : it->second) {
            if (imp.admits(name, fn)) return fn;
        }
    }
    return nullptr;
}

bool SymbolTablePass0::is_namespace(SymbolID qualified) const {
    if (m_namespaces.count(qualified) != 0) return true;
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table != nullptr && imp.admits_name(qualified) && imp.m_table->m_namespaces.count(qualified) != 0) return true;
    }
    return false;
}

FunctionDeclNode* SymbolTablePass0::find_namespace_function(const std::vector<SymbolID>& path, SymbolID current) const {
    if (path.size() < 2) return nullptr;
    const SymbolID joined = join_path(path, 0);
    for (SymbolID ns = current; ; ns = parent_namespace(ns)) {
        auto it = m_functions.find(qualify(ns, joined));
        if (it != m_functions.end() && !it->second.empty()) return it->second.front();
        if (ns == INVALID_SYMBOL_ID) break;
    }
    // `ns::f` where `ns` is a namespace of an imported module (imported names are visible unqualified).
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr) continue;
        auto it = imp.m_table->m_functions.find(joined);
        if (it == imp.m_table->m_functions.end()) continue;
        for (FunctionDeclNode* fn : it->second) {
            if (imp.admits(joined, fn)) return fn;
        }
    }
    if (path.size() >= 3) {
        // `alias::ns::f`: the first segment names an import, the rest is qualified inside it.
        return find_function_in_import(path[0], join_path(path, 1));
    }
    return nullptr;
}

VarDeclNode* SymbolTablePass0::find_namespace_global(const std::vector<SymbolID>& path, SymbolID current) const {
    if (path.size() < 2) return nullptr;
    auto in_table = [](const SymbolTablePass0& table, SymbolID key, const ImportedModule* view) -> VarDeclNode* {
        auto it = table.m_globals.find(key);
        if (it == table.m_globals.end()) return nullptr;
        return view == nullptr || view->admits(key, it->second) ? it->second : nullptr;
    };
    const SymbolID joined = join_path(path, 0);
    for (SymbolID ns = current; ; ns = parent_namespace(ns)) {
        if (VarDeclNode* local = in_table(*this, qualify(ns, joined), nullptr)) return local;
        if (ns == INVALID_SYMBOL_ID) break;
    }
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr) continue;
        if (VarDeclNode* found = in_table(*imp.m_table, joined, &imp)) return found;
    }
    const std::string_view alias_text = text_of(path[0]);
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table == nullptr || !alias_matches(imp, alias_text)) continue;
        if (VarDeclNode* found = in_table(*imp.m_table, join_path(path, 1), &imp)) return found;
    }
    return nullptr;
}

bool SymbolTablePass0::is_namespace_from(SymbolID current, SymbolID first) const {
    for (SymbolID ns = current; ; ns = parent_namespace(ns)) {
        if (is_namespace(qualify(ns, first))) return true;
        if (ns == INVALID_SYMBOL_ID) break;
    }
    return false;
}

FunctionDeclNode* SymbolTablePass0::find_function_from(SymbolID current_namespace, SymbolID name) const {
    for (SymbolID ns = current_namespace; ns != INVALID_SYMBOL_ID; ns = parent_namespace(ns)) {
        auto it = m_functions.find(qualify(ns, name));
        if (it != m_functions.end() && !it->second.empty()) return it->second.front();
    }
    return find_function(name);
}

VarDeclNode* SymbolTablePass0::find_global_from(SymbolID current_namespace, SymbolID name) const {
    for (SymbolID ns = current_namespace; ns != INVALID_SYMBOL_ID; ns = parent_namespace(ns)) {
        auto it = m_globals.find(qualify(ns, name));
        if (it != m_globals.end()) return it->second;
    }
    return find_global(name);
}

bool SymbolTablePass0::is_public(const DeclNode* decl) const {
    if (decl == nullptr || decl->m_is_pub) return true;
    auto twin = [&](const SymbolTablePass0& table) {
        if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
            // Only functions registered in the table have twins: a method written inside a class body stands alone.
            const SymbolID key = fn->m_owner != INVALID_SYMBOL_ID ? fn->m_name : qualify(fn->m_namespace, fn->m_name);
            const auto& group = fn->m_owner != INVALID_SYMBOL_ID ? table.m_methods : table.m_functions;
            const auto found = group.find(fn->m_owner != INVALID_SYMBOL_ID ? fn->m_owner : key);
            if (found == group.end() || std::find(found->second.begin(), found->second.end(), fn) == found->second.end()) return false;
            return table.m_pub_functions.count({fn->m_owner, key}) != 0;
        }
        if (const auto* var = dynamic_cast<const VarDeclNode*>(decl)) return table.m_pub_globals.count(qualify(var->m_namespace, var->m_name)) != 0;
        return false;
    };
    if (twin(*this)) return true;
    for (const ImportedModule& imp : m_views) {
        if (imp.m_table != nullptr && twin(*imp.m_table)) return true;
    }
    return false;
}

bool SymbolTablePass0::is_accessible(const DeclNode* decl, std::string_view viewing_file) const {
    if (decl == nullptr || decl->m_file.empty() || viewing_file.empty()) return true;
    if (is_public(decl)) return true;
    return unit_key(decl->m_file) == unit_key(viewing_file);
}

} // namespace fool
