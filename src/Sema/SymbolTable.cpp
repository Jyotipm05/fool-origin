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
#include <cstdlib>
#include <filesystem>
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

namespace {

void assign_file_recursive(const std::vector<DeclNode*>& decls, std::string_view file) {
    for (DeclNode* decl : decls) {
        if (decl == nullptr) continue;
        decl->m_file = file;
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
    ConstEvaluator evaluator(m_definitions, m_diag);
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
    ConstEvaluator evaluator(m_definitions, m_diag);
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
    m_active_decls.push_back(decl);
    register_declaration(decl);
}

void SymbolTablePass0::register_declaration(DeclNode* decl) {
    auto redefinition = [&](const char* what, SymbolID name) {
        m_diag.error(decl->m_span, std::string("redefinition of ") + what + " '" + std::string(text_of(name)) + "'");
    };

    if (auto* fn = dynamic_cast<FunctionDeclNode*>(decl)) {
        if (fn->m_owner != INVALID_SYMBOL_ID) m_methods[fn->m_owner].push_back(fn);
        else m_functions[fn->m_name].push_back(fn);
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
        auto existing = m_globals.find(var->m_name);
        if (existing != m_globals.end()) {
            // A constant may be repeated in the interface (.fo) and the implementation (.fl) as
            // long as both declarations agree on kind, type and value.
            const VarDeclNode* first = existing->second;
            bool same = first->m_kind == var->m_kind && type_to_string(first->m_type) == type_to_string(var->m_type);
            if (same && var->m_kind == BindingKind::Constx && var->m_initializer != nullptr) {
                DiagnosticEngine scratch;
                scratch.set_echo(false);
                ConstEvaluator evaluator(m_definitions, scratch);
                auto value = evaluator.evaluate(var->m_initializer);
                auto previous = m_definitions.find(var->m_name);
                same = value.has_value() && previous != m_definitions.end() &&
                       value->to_string() == previous->second.to_string();
            } else if (same) {
                same = false; // only constants may be declared twice
            }
            if (!same) redefinition("variable", var->m_name);
            return;
        }
        m_globals.emplace(var->m_name, var);
        if (var->m_kind == BindingKind::Constx && var->m_initializer != nullptr) {
            ConstEvaluator evaluator(m_definitions, m_diag);
            if (auto value = evaluator.evaluate(var->m_initializer)) m_definitions[var->m_name] = *value;
        }
    }
}

void SymbolTablePass0::add_import(ImportDeclNode* decl, const SymbolTablePass0* table) {
    m_imports.push_back(ImportedModule{decl->m_module_path, table});
}

// --- lookups (local first, then one level of imports)

FunctionDeclNode* SymbolTablePass0::find_function(SymbolID name) const {
    auto it = m_functions.find(name);
    if (it != m_functions.end() && !it->second.empty()) return it->second.front();
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_functions.find(name);
        if (imp_it != imp.m_table->m_functions.end() && !imp_it->second.empty()) return imp_it->second.front();
    }
    return nullptr;
}

FunctionDeclNode* SymbolTablePass0::find_method(SymbolID owner, SymbolID name) const {
    auto search = [&](const SymbolTablePass0& table) -> FunctionDeclNode* {
        auto cls = table.m_classes.find(owner);
        if (cls != table.m_classes.end()) {
            for (FunctionDeclNode* method : cls->second->m_methods) {
                if (method->m_name == name) return method;
            }
        }
        auto impls = table.m_methods.find(owner);
        if (impls != table.m_methods.end()) {
            for (FunctionDeclNode* method : impls->second) {
                if (method->m_name == name) return method;
            }
        }
        return nullptr;
    };
    if (FunctionDeclNode* local = search(*this)) return local;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        if (FunctionDeclNode* found = search(*imp.m_table)) return found;
    }
    return nullptr;
}

ClassDeclNode* SymbolTablePass0::find_class(SymbolID name) const {
    auto it = m_classes.find(name);
    if (it != m_classes.end()) return it->second;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_classes.find(name);
        if (imp_it != imp.m_table->m_classes.end()) return imp_it->second;
    }
    return nullptr;
}

RulesetDeclNode* SymbolTablePass0::find_ruleset(SymbolID name) const {
    auto it = m_rulesets.find(name);
    if (it != m_rulesets.end()) return it->second;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_rulesets.find(name);
        if (imp_it != imp.m_table->m_rulesets.end()) return imp_it->second;
    }
    return nullptr;
}

VarDeclNode* SymbolTablePass0::find_global(SymbolID name) const {
    auto it = m_globals.find(name);
    if (it != m_globals.end()) return it->second;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_globals.find(name);
        if (imp_it != imp.m_table->m_globals.end()) return imp_it->second;
    }
    return nullptr;
}

BlueprintDeclNode* SymbolTablePass0::find_blueprint(SymbolID name) const {
    auto it = m_blueprints.find(name);
    if (it != m_blueprints.end()) return it->second;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_blueprints.find(name);
        if (imp_it != imp.m_table->m_blueprints.end()) return imp_it->second;
    }
    return nullptr;
}

EnumDeclNode* SymbolTablePass0::find_enum(SymbolID name) const {
    auto it = m_enums.find(name);
    if (it != m_enums.end()) return it->second;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        auto imp_it = imp.m_table->m_enums.find(name);
        if (imp_it != imp.m_table->m_enums.end()) return imp_it->second;
    }
    return nullptr;
}

bool SymbolTablePass0::is_known_global(SymbolID name) const {
    auto known_locally = [&](const SymbolTablePass0& t) {
        return t.m_functions.count(name) != 0 || t.m_classes.count(name) != 0 || t.m_rulesets.count(name) != 0 ||
               t.m_blueprints.count(name) != 0 || t.m_enums.count(name) != 0 || t.m_globals.count(name) != 0 ||
               t.m_definitions.count(name) != 0;
    };
    if (known_locally(*this)) return true;
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table != nullptr && known_locally(*imp.m_table)) return true;
    }
    return false;
}

bool SymbolTablePass0::is_import_alias(SymbolID alias) const {
    std::string_view alias_text = text_of(alias);
    for (const ImportedModule& imp : m_imports) {
        std::string_view full = text_of(imp.m_name);
        if (full == alias_text || last_segment(full) == alias_text) return true;
    }
    return false;
}

FunctionDeclNode* SymbolTablePass0::find_function_in_import(SymbolID alias, SymbolID name) const {
    std::string_view alias_text = text_of(alias);
    for (const ImportedModule& imp : m_imports) {
        if (imp.m_table == nullptr) continue;
        std::string_view full = text_of(imp.m_name);
        if (full != alias_text && last_segment(full) != alias_text) continue;
        auto it = imp.m_table->m_functions.find(name);
        if (it != imp.m_table->m_functions.end() && !it->second.empty()) return it->second.front();
    }
    return nullptr;
}

} // namespace fool
