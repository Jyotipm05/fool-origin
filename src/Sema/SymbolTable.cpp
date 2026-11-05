//
// Created by jmbab on 05-11-2026.
//
/**
 * @file SymbolTable.cpp
 * @brief Implementation unit of Pass 0 declaration filtering.
 */

module;

#include <string>
#include <string_view>
#include <iostream>
#include <utility>

module fool.sema;

namespace fool {

SymbolTablePass0::SymbolTablePass0(TargetConfig target)
    : m_target(std::move(target)) {}

void SymbolTablePass0::collect_declarations(ModuleAST* module) {
    auto& interner = StringInterner::get();

    for (DeclNode* decl : module->m_declarations) {
        if (!evaluate_target_attributes(decl)) {
            decl->m_is_active = false;
            continue; // Pruned before Sema/Codegen
        }

        if (auto* fn = dynamic_cast<FunctionDeclNode*>(decl)) {
            m_functions[fn->m_name] = fn;
            std::cout << "[Pass 0 Registered Function]: "
                      << interner.resolve(fn->m_name) << "\n";
        } else if (auto* cls = dynamic_cast<ClassDeclNode*>(decl)) {
            m_classes[cls->m_name] = cls;
            std::cout << "[Pass 0 Registered Class]: "
                      << interner.resolve(cls->m_name) << "\n";
        } else if (auto* rs = dynamic_cast<RulesetDeclNode*>(decl)) {
            m_rulesets[rs->m_name] = rs;
            std::cout << "[Pass 0 Registered Ruleset]: "
                      << interner.resolve(rs->m_name) << "\n";
        } else if (auto* bp = dynamic_cast<BlueprintDeclNode*>(decl)) {
            m_blueprints[bp->m_name] = bp;
            std::cout << "[Pass 0 Registered Blueprint]: "
                      << interner.resolve(bp->m_name) << "\n";
        }
    }
}

FunctionDeclNode* SymbolTablePass0::find_function(SymbolID name) const {
    auto it = m_functions.find(name);
    return (it != m_functions.end()) ? it->second : nullptr;
}

ClassDeclNode* SymbolTablePass0::find_class(SymbolID name) const {
    auto it = m_classes.find(name);
    return (it != m_classes.end()) ? it->second : nullptr;
}

RulesetDeclNode* SymbolTablePass0::find_ruleset(SymbolID name) const {
    auto it = m_rulesets.find(name);
    return (it != m_rulesets.end()) ? it->second : nullptr;
}

BlueprintDeclNode* SymbolTablePass0::find_blueprint(SymbolID name) const {
    auto it = m_blueprints.find(name);
    return (it != m_blueprints.end()) ? it->second : nullptr;
}

bool SymbolTablePass0::evaluate_target_attributes(const DeclNode* decl) const {
    auto& interner = StringInterner::get();
    SymbolID target_sym = interner.intern("target");

    for (const auto& attr : decl->m_attributes) {
        if (attr.m_name != target_sym) continue;

        for (const auto& arg : attr.m_args) {
            std::string_view key = interner.resolve(arg.m_key);
            std::string_view val = arg.m_value;

            // Strip surrounding quotes: "windows" -> windows
            if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
                val = val.substr(1, val.size() - 2);
            }

            if (key == "os" && val != m_target.m_os) return false;
            if (key == "arch" && val != m_target.m_arch) return false;
            if (key == "pointer_width" && std::stoi(std::string(val)) != m_target.m_pointer_width) {
                return false;
            }
        }
    }
    return true;
}

} // namespace fool
