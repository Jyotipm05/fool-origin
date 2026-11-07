//
// Created by jmbab on 05-11-2026.
//

/**
 * @file SymbolTable.ixx
 * @brief Module interface for the semantic passes:
 *        compile-time evaluation, Pass 0 target pruning / symbol collection,
 *        Pass 1 module graph, and Pass 2 type checking.
 */

module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

export module fool.sema;

import fool.common;
import fool.diagnostics;
import fool.lexer;
import fool.ast;

export namespace fool {

// =============================================================================================
// Target configuration
// =============================================================================================

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

    // 3. Member functions LAST at the bottom
    /// 'posix' / 'unix' match every POSIX-like OS; any other value must match exactly.
    [[nodiscard]] bool os_matches(std::string_view value) const;
    [[nodiscard]] bool arch_matches(std::string_view value) const;
    /// Does a platform-specific file (stem suffix such as `linux`, `windows`, `epoll`) apply to this target?
    [[nodiscard]] bool file_suffix_applies(std::string_view suffix) const;
    /// Does this file apply to the target, judging by its platform suffix (`mutex_linux.fex`)?
    [[nodiscard]] bool file_applies(const std::filesystem::path& path) const;
    /// Canonical triple-like text, e.g. `linux-x86_64`.
    [[nodiscard]] std::string to_string() const { return m_os + "-" + m_arch; }
};

// =============================================================================================
// Compile-time evaluation
// =============================================================================================

/**
 * @struct ConstValue
 * @brief Result of compile-time evaluation.
 */
struct ConstValue {
    // 1. Member variables
    enum class Kind : uint8_t { Int, Float, Bool };
    Kind m_kind{Kind::Int};
    int64_t m_int{0};
    double m_float{0.0};
    bool m_bool{false};

    // 2. Constructors
    ConstValue() = default;
    static ConstValue from_int(int64_t v) { ConstValue c; c.m_kind = Kind::Int; c.m_int = v; return c; }
    static ConstValue from_float(double v) { ConstValue c; c.m_kind = Kind::Float; c.m_float = v; return c; }
    static ConstValue from_bool(bool v) { ConstValue c; c.m_kind = Kind::Bool; c.m_bool = v; return c; }

    // 3. Member functions
    [[nodiscard]] bool truthy() const {
        switch (m_kind) {
            case Kind::Int: return m_int != 0;
            case Kind::Float: return m_float != 0.0;
            case Kind::Bool: return m_bool;
        }
        return false;
    }
    [[nodiscard]] double as_double() const {
        return m_kind == Kind::Float ? m_float : (m_kind == Kind::Bool ? (m_bool ? 1.0 : 0.0) : static_cast<double>(m_int));
    }
    [[nodiscard]] int64_t as_int() const {
        return m_kind == Kind::Int ? m_int : (m_kind == Kind::Bool ? (m_bool ? 1 : 0) : static_cast<int64_t>(m_float));
    }
    [[nodiscard]] std::string to_string() const;
};

using ConstTable = std::unordered_map<SymbolID, ConstValue>;

/**
 * @class ConstEvaluator
 * @brief Evaluates constant expressions: literals, named constants, `def(NAME)`, unary and binary operators.
 * @details Replaces the former string-based @if evaluator. Works on the parsed expression tree so
 *          `&&`, `||`, `!`, parentheses and comparisons compose correctly.
 */
class ConstEvaluator {
    // 1. Member variables FIRST on top with 'm_' prefix
    const ConstTable& m_table;
    DiagnosticEngine& m_diag;

public:
    // 2. Constructors and Destructors in the MIDDLE
    ConstEvaluator(const ConstTable& table, DiagnosticEngine& diag) : m_table(table), m_diag(diag) {}

    // 3. Member functions LAST at the bottom
    /// Returns nullopt (after reporting, unless `report` is false) if `expr` is not a constant expression.
    [[nodiscard]] std::optional<ConstValue> evaluate(const ExprNode* expr, bool report = true) const;

private:
    [[nodiscard]] std::optional<ConstValue> eval_unary(const UnaryExpr* expr, bool report) const;
    [[nodiscard]] std::optional<ConstValue> eval_binary(const BinaryExpr* expr, bool report) const;
    [[nodiscard]] std::optional<ConstValue> fail(const ExprNode* expr, bool report, std::string message) const;
};

/// Renders a type for diagnostics and signature comparison, e.g. `const& Span<u8>`.
std::string type_to_string(const TypeNode* type);

/// Records the defining file on every declaration of `module` (including inside top-level @if blocks)
/// so diagnostics point at the right file even when several files are analysed together.
void assign_source_file(ModuleAST* module, std::string_view path);

// =============================================================================================
// Pass 0: target pruning and out-of-order symbol collection
// =============================================================================================

class SymbolTablePass0;

/**
 * @struct ImportedModule
 * @brief A resolved `#import`: the module name and (if available) its symbol table.
 */
struct ImportedModule {
    // 1. Member variables
    SymbolID m_name{INVALID_SYMBOL_ID};
    const SymbolTablePass0* m_table{nullptr};
};

/**
 * @class SymbolTablePass0
 * @brief Pass 0 symbol engine responsible for target filtering and out-of-order symbol collection.
 * @details Never mutates the AST: the set of active declarations (after `@target` pruning and
 *          top-level `@if` selection) is kept in the table, so one AST can be analysed under several
 *          module-parameter instantiations.
 */
class SymbolTablePass0 {
    // 1. Member variables FIRST on top with 'm_' prefix
    TargetConfig m_target;
    DiagnosticEngine& m_diag;
    ConstTable m_definitions;                  ///< Module parameters and top-level `constx` values
    std::vector<DeclNode*> m_active_decls;     ///< Flattened, pruned declarations in source order
    std::vector<ImportDeclNode*> m_import_decls;
    std::vector<ImportedModule> m_imports;
    std::unordered_map<SymbolID, std::vector<FunctionDeclNode*>> m_functions; ///< Free functions by name
    std::unordered_map<SymbolID, std::vector<FunctionDeclNode*>> m_methods;   ///< `Owner::name` and class-body methods, keyed by owner
    std::unordered_map<SymbolID, ClassDeclNode*> m_classes;
    std::unordered_map<SymbolID, RulesetDeclNode*> m_rulesets;
    std::unordered_map<SymbolID, BlueprintDeclNode*> m_blueprints;
    std::unordered_map<SymbolID, EnumDeclNode*> m_enums;
    std::unordered_map<SymbolID, VarDeclNode*> m_globals;
    size_t m_pruned_count{0};

public:
    // 2. Constructors in the MIDDLE
    SymbolTablePass0(TargetConfig target, DiagnosticEngine& diag);
    ~SymbolTablePass0() = default;

    // 3. Member functions LAST at the bottom
    /// Evaluates module parameter defaults (or the caller-supplied overrides) into the definition table.
    void define_parameters(const std::vector<ModuleParam>& params, const ConstTable& overrides = {});
    /// Collects the active declarations of `module`; only those of `section` when it is valid.
    void collect_declarations(const ModuleAST* module, SymbolID section = INVALID_SYMBOL_ID);
    void add_import(ImportDeclNode* decl, const SymbolTablePass0* table);
    void define(SymbolID name, ConstValue value) { m_definitions[name] = value; }

    [[nodiscard]] FunctionDeclNode* find_function(SymbolID name) const;
    [[nodiscard]] FunctionDeclNode* find_method(SymbolID owner, SymbolID name) const;
    [[nodiscard]] ClassDeclNode* find_class(SymbolID name) const;
    [[nodiscard]] RulesetDeclNode* find_ruleset(SymbolID name) const;
    [[nodiscard]] BlueprintDeclNode* find_blueprint(SymbolID name) const;
    [[nodiscard]] EnumDeclNode* find_enum(SymbolID name) const;
    [[nodiscard]] VarDeclNode* find_global(SymbolID name) const;
    [[nodiscard]] bool is_known_global(SymbolID name) const;
    /// Resolves `alias::function` against the imports (alias = last segment of the module path).
    [[nodiscard]] FunctionDeclNode* find_function_in_import(SymbolID alias, SymbolID name) const;
    [[nodiscard]] bool is_import_alias(SymbolID alias) const;

    [[nodiscard]] const std::vector<DeclNode*>& active_declarations() const { return m_active_decls; }
    [[nodiscard]] const std::vector<ImportDeclNode*>& import_declarations() const { return m_import_decls; }
    [[nodiscard]] const std::unordered_map<SymbolID, std::vector<FunctionDeclNode*>>& functions() const { return m_functions; }
    [[nodiscard]] const std::unordered_map<SymbolID, std::vector<FunctionDeclNode*>>& methods() const { return m_methods; }
    [[nodiscard]] const ConstTable& definitions() const { return m_definitions; }
    [[nodiscard]] const TargetConfig& target() const { return m_target; }
    [[nodiscard]] size_t pruned_count() const { return m_pruned_count; }
    [[nodiscard]] DiagnosticEngine& diagnostics() const { return m_diag; }

private:
    bool evaluate_target_attributes(const DeclNode* decl);
    void collect_one(DeclNode* decl, SymbolID section);
    void register_declaration(DeclNode* decl);
    void validate_attributes(const DeclNode* decl);
    bool select_branch_conditions(const std::vector<ExprNode*>& conditions, size_t& chosen);
};

// =============================================================================================
// Pass 1: module graph
// =============================================================================================

/**
 * @struct ModuleFile
 * @brief One parsed source file contributing declarations to a module.
 */
struct ModuleFile {
    // 1. Member variables
    std::filesystem::path m_path;
    FileRole m_role{FileRole::Implementation};
    ModuleAST* m_ast{nullptr};
};

/**
 * @struct ModuleInstantiation
 * @brief A distinct set of template arguments an importer requested for a parameterized module.
 */
struct ModuleInstantiation {
    // 1. Member variables
    std::string m_key;      ///< Canonical text such as `examples::import_templates<ASIO_IOURING_ON=1>@linux-x86_64`
    ConstTable m_overrides;
};

/**
 * @struct ModuleNode
 * @brief Represents a module within the Pass 1 dependency graph.
 */
struct ModuleNode {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolID m_name{INVALID_SYMBOL_ID};
    std::vector<ModuleFile> m_files;
    std::vector<ModuleParam> m_parameters;
    std::vector<SymbolID> m_dependencies;
    std::vector<ModuleInstantiation> m_instantiations;

    // 2. Constructors in the MIDDLE
    ModuleNode() = default;
};

/**
 * @class ModuleGraphPass1
 * @brief Pass 1 Module Graph: indexes source roots by their declared `module` names, resolves
 *        imports (including template arguments), and produces a topological compilation order.
 * @details Modules are identified by the name written in the `module` line, not by file path, so a
 *          module may span any number of files (`.fo` interface, `.fl` implementation, platform
 *          `.fex` overlays) and one file may open several module sections.
 */
class ModuleGraphPass1 {
    // 1. Member variables FIRST on top with 'm_' prefix
    Arena& m_arena;
    DiagnosticEngine& m_diag;
    TargetConfig m_target;
    std::vector<std::filesystem::path> m_search_paths;
    std::map<std::string, ModuleNode> m_modules;   ///< Ordered by name for deterministic output
    std::vector<SymbolID> m_topological_order;
    size_t m_indexed_files{0};

public:
    // 2. Constructors and Destructors in the MIDDLE
    ModuleGraphPass1(Arena& arena, DiagnosticEngine& diag, TargetConfig target);
    ~ModuleGraphPass1() = default;

    // 3. Member functions LAST at the bottom
    void add_search_path(const std::filesystem::path& path);
    /// Parses every source file below the search paths that applies to the target.
    void index_search_paths();
    /// Parses one file and registers it under every module section it declares.
    void add_source_file(const std::filesystem::path& path);
    /// Resolves imports, validates template arguments and records instantiations.
    bool resolve_imports();
    /// Orders modules so dependencies come first; reports import cycles.
    bool sort_topological();

    [[nodiscard]] const std::vector<SymbolID>& get_compilation_order() const { return m_topological_order; }
    [[nodiscard]] const ModuleNode* get_module(SymbolID name) const;
    [[nodiscard]] const std::map<std::string, ModuleNode>& modules() const { return m_modules; }
    [[nodiscard]] size_t indexed_file_count() const { return m_indexed_files; }
    [[nodiscard]] const TargetConfig& target() const { return m_target; }
};

// =============================================================================================
// Pass 2: type checking
// =============================================================================================

/**
 * @struct SymbolBinding
 * @brief Tracks variable binding mutability, type shape, and destructive move state in local scopes.
 */
struct SymbolBinding {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolID m_name{INVALID_SYMBOL_ID};
    SymbolID m_type_name{INVALID_SYMBOL_ID}; ///< Class/struct name when known (through views)
    BindingKind m_binding_kind{BindingKind::Mutable};
    bool m_is_const_view{false};  ///< Declared as const& / const* (transitive immutability)
    bool m_is_pointer{false};
    bool m_is_view{false};        ///< Reference or pointer: cannot be moved out of
    bool m_is_moved{false};       ///< Tracked for destructive move invalidation
    SourceSpan m_span;

    // 2. Constructors in the MIDDLE
    SymbolBinding() = default;
};

/**
 * @class TypeCheckerPass2
 * @brief Pass 2 Semantic Engine: scoping, assignment-vs-declaration, transitive immutability,
 * receiver verification, destructive move tracking, call-argument binding (named and default
 * arguments), constant checking and compile-time @if pruning inside function bodies.
 */
class TypeCheckerPass2 {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolTablePass0& m_pass0;
    DiagnosticEngine& m_diag;
    std::vector<std::vector<SymbolBinding>> m_scopes;
    ConstTable m_local_constants;
    const FunctionDeclNode* m_current_fn{nullptr};
    const ClassDeclNode* m_current_owner{nullptr};
    int m_loop_depth{0};
    bool m_strict_names{false};

public:
    // 2. Constructors in the MIDDLE
    TypeCheckerPass2(SymbolTablePass0& pass0, DiagnosticEngine& diag);
    ~TypeCheckerPass2() = default;

    // 3. Member functions LAST at the bottom
    /// When enabled, uses of names that resolve nowhere are errors (requires every sibling file and import to be loaded).
    void set_strict_names(bool strict) { m_strict_names = strict; }
    /// Checks every active declaration known to the Pass 0 table. Returns true if no errors were reported.
    bool check_module();
    bool check_function(const FunctionDeclNode* fn, const ClassDeclNode* owner);

private:
    // scopes
    void push_scope();
    void pop_scope();
    SymbolBinding* lookup(SymbolID name);
    SymbolBinding* lookup_in_current_scope(SymbolID name);
    SymbolBinding* declare(SymbolBinding binding);
    SymbolBinding make_binding(SymbolID name, const TypeNode* type, BindingKind kind, SourceSpan span) const;

    // declarations
    void check_declaration_consistency();
    void check_type_declarations();
    void check_global_variable(const VarDeclNode* var);
    void check_extern_rules(const FunctionDeclNode* fn);

    // statements
    void check_statements(const std::vector<StmtNode*>& stmts);
    void check_statement(const StmtNode* stmt);
    void check_var_decl(const VarDeclNode* var);
    void check_block_scoped(const BlockStmt* block);
    [[nodiscard]] bool always_returns(const std::vector<StmtNode*>& stmts) const;
    [[nodiscard]] bool stmt_always_returns(const StmtNode* stmt) const;
    [[nodiscard]] const ConditionalBranch* select_branch(const ConditionalStmt* stmt, bool report) const;
    [[nodiscard]] bool is_generic_parameter(SymbolID name) const;
    [[nodiscard]] const FunctionDeclNode* resolve_callee(const CallExpr* call) const;

    // expressions
    void check_expression(const ExprNode* expr);
    void check_assignment(const BinaryExpr* assign);
    void check_mutation_target(const ExprNode* target, SourceSpan span);
    void check_call(const CallExpr* call);
    void check_member_access(const MemberExpr* member);
    void check_struct_literal(const StructLiteralExpr* lit);
    void bind_arguments(const FunctionDeclNode* fn, const CallExpr* call, std::string_view display_name);
    [[nodiscard]] const SymbolBinding* root_binding(const ExprNode* expr);
    [[nodiscard]] ConstTable visible_constants() const;
    void check_const_fits_type(const TypeNode* type, const ConstValue& value, SourceSpan span, std::string_view what);
    [[nodiscard]] ClassDeclNode* class_of_binding(const SymbolBinding& binding) const;
};

} // namespace fool
