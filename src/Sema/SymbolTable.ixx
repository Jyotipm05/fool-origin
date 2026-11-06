//
// Created by jmbab on 05-11-2026.
//

/**
 * @file SymbolTable.ixx
 * @brief Module interface for Pass 0 target pruning, Pass 1 module graph, and Pass 2 type checking.
 */

module;

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <utility>
#include <memory>
#include <optional>

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
    std::unordered_map<std::string, int64_t> m_definitions;
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
    [[nodiscard]] const std::unordered_map<std::string, int64_t>& get_definitions() const { return m_definitions; }
    void add_definition(std::string name, int64_t val) { m_definitions[std::move(name)] = val; }

private:
    bool evaluate_target_attributes(const DeclNode* decl) const;
};

/**
 * @struct ModuleNode
 * @brief Represents a module within the Pass 1 dependency graph.
 */
struct ModuleNode {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::filesystem::path m_interface_path;
    std::filesystem::path m_implementation_path;
    std::vector<std::filesystem::path> m_overlay_paths;
    std::vector<SymbolID> m_dependencies;
    ModuleAST* m_ast{nullptr};

    // 2. Constructors in the MIDDLE
    ModuleNode() = default;
};

/**
 * @class ModuleGraphPass1
 * @brief Pass 1 Module Graph: multi-module path resolution, topological sorting, and template instantiation.
 */
class ModuleGraphPass1 {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<std::filesystem::path> m_search_paths;
    std::unordered_map<SymbolID, ModuleNode> m_modules;
    std::vector<SymbolID> m_topological_order;

public:
    // 2. Constructors in the MIDDLE
    ModuleGraphPass1();
    ~ModuleGraphPass1() = default;

    // 3. Member functions LAST at the bottom
    void add_search_path(const std::filesystem::path& path);
    bool resolve_module(SymbolID module_name, const TargetConfig& target);
    bool sort_topological();
    [[nodiscard]] const std::vector<SymbolID>& get_compilation_order() const;
    [[nodiscard]] const ModuleNode* get_module(SymbolID name) const;
};

/**
 * @enum class TypeKind
 * @brief Semantic type classifications in Fool.
 */
enum class TypeKind {
    Void,
    Bool,
    Int8, Int16, Int32, Int64, ISize,
    UInt8, UInt16, UInt32, UInt64, USize,
    Float32, Float64,
    RawPointer,      ///< *T (mutable by default)
    ConstRawPointer, ///< const* T (immutable pointer)
    Reference,       ///< &T (mutable by default)
    ConstReference,  ///< const& T (strict transitive immutability)
    Class,
    RulesetBound
};

/**
 * @struct Type
 * @brief Semantic type descriptor.
 */
struct Type {
    // 1. Member variables FIRST on top with 'm_' prefix
    TypeKind m_kind{TypeKind::Void};
    SymbolID m_name{INVALID_SYMBOL_ID};
    bool m_is_const{false};

    // 2. Constructors in the MIDDLE
    Type() = default;
    explicit Type(TypeKind kind, bool is_const = false)
        : m_kind(kind), m_is_const(is_const) {}
    Type(TypeKind kind, SymbolID name, bool is_const = false)
        : m_kind(kind), m_name(name), m_is_const(is_const) {}
};

/**
 * @struct SymbolBinding
 * @brief Tracks variable binding mutability, type, and destructive move state in local scopes.
 */
struct SymbolBinding {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolID m_name{INVALID_SYMBOL_ID};
    Type m_type;
    BindingKind m_binding_kind{BindingKind::Mutable};
    bool m_is_moved{false}; ///< Tracked for destructive move invalidation

    // 2. Constructors in the MIDDLE
    SymbolBinding() = default;
    SymbolBinding(SymbolID name, Type type, BindingKind kind)
        : m_name(name), m_type(type), m_binding_kind(kind), m_is_moved(false) {}
};

/**
 * @class TypeCheckerPass2
 * @brief Pass 2 Semantic Engine: type checking, transitive immutability enforcement,
 * receiver verification, destructive move tracking, and compile-time @if statement pruning.
 */
class TypeCheckerPass2 {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolTablePass0& m_pass0;
    std::unordered_map<SymbolID, SymbolBinding> m_bindings;
    std::vector<std::string> m_diagnostics;
    size_t m_error_count{0};

public:
    // 2. Constructors in the MIDDLE
    explicit TypeCheckerPass2(SymbolTablePass0& pass0);
    ~TypeCheckerPass2() = default;

    // 3. Member functions LAST at the bottom
    bool check_module(ModuleAST* module);
    bool check_function(FunctionDeclNode* fn);
    bool check_statement(StmtNode* stmt, std::vector<StmtNode*>& out_pruned_stmts);
    bool check_var_decl(VarDeclNode* var);
    [[nodiscard]] size_t get_error_count() const;
    [[nodiscard]] const std::vector<std::string>& get_diagnostics() const;

private:
    void report_error(std::string_view msg, SourceSpan span);
    bool evaluate_compile_time_condition(std::string_view cond);
};

} // namespace fool
