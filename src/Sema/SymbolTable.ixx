//
// Created by jmbab on 05-11-2026.
//

/**
 * @file SymbolTable.ixx
 * @brief Module interface for Pass 0 symbol collection and target declaration pruning.
 */

module;

#include <string>
#include <unordered_map>
#include <cstdint>
#include <utility>

export module fool.sema;

import fool.common;
import fool.ast;

export namespace fool {

/**
 * @struct TargetConfig
 * @brief Platform configuration passed by the compiler build driver flag (--target).
 */
struct TargetConfig {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::string m_os{"linux"};
    std::string m_arch{"x86_64"};
    uint8_t m_pointer_width{64};

    // 2. Constructors in the MIDDLE
    TargetConfig() = default;
    TargetConfig(std::string os, std::string arch, uint8_t ptr_width)
        : m_os(std::move(os)), m_arch(std::move(arch)), m_pointer_width(ptr_width) {}
};

/**
 * @class SymbolTablePass0
 * @brief Pass 0 symbol engine responsible for target filtering and out-of-order symbol collection.
 */
class SymbolTablePass0 {
    // 1. Member variables FIRST on top with 'm_' prefix
    TargetConfig m_target;
    std::unordered_map<SymbolID, FunctionDeclNode*> m_functions;
    std::unordered_map<SymbolID, ClassDeclNode*> m_classes;
    std::unordered_map<SymbolID, RulesetDeclNode*> m_rulesets;
    std::unordered_map<SymbolID, BlueprintDeclNode*> m_blueprints;

public:
    // 2. Constructors in the MIDDLE
    explicit SymbolTablePass0(TargetConfig target);
    ~SymbolTablePass0() = default;

    // 3. Member functions LAST at the bottom
    void collect_declarations(ModuleAST* module);
    [[nodiscard]] FunctionDeclNode* find_function(SymbolID name) const;
    [[nodiscard]] ClassDeclNode* find_class(SymbolID name) const;
    [[nodiscard]] RulesetDeclNode* find_ruleset(SymbolID name) const;
    [[nodiscard]] BlueprintDeclNode* find_blueprint(SymbolID name) const;

private:
    bool evaluate_target_attributes(const DeclNode* decl) const;
};

} // namespace fool
