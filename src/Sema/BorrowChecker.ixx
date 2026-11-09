//
// Created by jmbab on 06-11-2026.
//

/**
 * @file BorrowChecker.ixx
 * @brief Opt-in borrow checker: Aliasing XOR Mutability with non-lexical (last-use) borrow lifetimes.
 *
 * Enabled with `foolc --check-borrow`; it runs after the type checker and only on code that already
 * type-checks. The analysis is intra-procedural and name based:
 *
 *  - `var r = &x;` / `const r: const& T = &x;` creates a borrow of the root variable of `x`
 *    (exclusive for `&`, shared for `const&` / `const*`). It stays live until the last use of `r`
 *    (extended to the end of an enclosing loop when `r` is declared outside of it).
 *  - `f(&x, ...)` and `x.method()` with a `this &self` receiver borrow temporarily for the call.
 *  - Errors: second exclusive borrow, shared + exclusive overlap, reading or assigning a variable
 *    while it is exclusively (or, for assignment, at all) borrowed, aliasing arguments in one call,
 *    and returning a reference to a local.
 *
 * Scoping follows the type checker: every declaration gets its own identity (`x#3`), so a variable that
 * shadows another one in a nested block is tracked separately from the one it hides.
 *
 * Known limits (documented in the README): no inter-procedural lifetimes and no reborrow chains
 * through references.
 */

module;

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

export module fool.borrow;

import fool.common;
import fool.diagnostics;
import fool.lexer;
import fool.ast;
import fool.sema;

export namespace fool {

enum class BorrowKind {
    Shared,   ///< const& T / const* T (any number of concurrent readers)
    Exclusive ///< &T (exactly one active writer, no concurrent readers)
};

/// One active borrow of a place (`x`, `x.field`, ...). `m_borrower` is INVALID_SYMBOL_ID for
/// temporaries that only live for one call, identified by `m_call_id`.
struct BorrowRecord {
    SymbolID m_borrower{INVALID_SYMBOL_ID};
    std::vector<SymbolID> m_place;
    BorrowKind m_kind{BorrowKind::Shared};
    SourceSpan m_span;
    size_t m_call_id{0};
};

class BorrowCheckerPass {
    // 1. Member variables FIRST on top with 'm_' prefix
    enum class Mode { Collect, Check };

    const SymbolTablePass0& m_symbols;
    DiagnosticEngine& m_diag;

    Mode m_mode{Mode::Collect};
    size_t m_pos{0};                                         ///< Event counter, identical in both passes
    std::unordered_map<SymbolID, size_t> m_last_use;         ///< name -> position of its last use
    std::unordered_map<SymbolID, size_t> m_decl_pos;         ///< name -> position of its declaration
    std::vector<BorrowRecord> m_active;
    std::unordered_set<SymbolID> m_locals;                   ///< owned locals (never views)
    std::unordered_map<SymbolID, SymbolID> m_class_of;       ///< local/param -> class name, for method lookups
    std::unordered_map<SymbolID, SymbolID> m_origin_of;      ///< borrower -> origin variable
    size_t m_call_counter{0};
    std::vector<std::unordered_map<SymbolID, SymbolID>> m_scopes; ///< name -> unique declaration identity
    size_t m_decl_counter{0};
    const FunctionDeclNode* m_fn{nullptr};
    SymbolID m_owner{INVALID_SYMBOL_ID};

public:
    // 2. Constructors and Destructors in the MIDDLE
    BorrowCheckerPass(const SymbolTablePass0& symbols, DiagnosticEngine& diag) : m_symbols(symbols), m_diag(diag) {}
    ~BorrowCheckerPass() = default;

    // 3. Member functions LAST at the bottom
    /// Checks every active function and method of the module that `symbols` describes.
    bool check_module();
    /// `owner` is the class the method belongs to (INVALID_SYMBOL_ID for free functions).
    bool check_function(const FunctionDeclNode* fn, SymbolID owner = INVALID_SYMBOL_ID);

private:
    void run_pass(Mode mode);
    void push_scope() { m_scopes.emplace_back(); }
    void pop_scope() { m_scopes.pop_back(); }
    SymbolID declare(SymbolID name);
    [[nodiscard]] SymbolID resolve(SymbolID name) const;
    [[nodiscard]] std::vector<SymbolID> place_of(const ExprNode* expr) const;

    // traversal (shared by both passes)
    void walk_statements(const std::vector<StmtNode*>& stmts);
    void walk_statement(const StmtNode* stmt);
    void walk_block(const BlockStmt* block);
    void walk_expr(const ExprNode* expr);
    bool walk_path(const ExprNode* expr, std::vector<SymbolID>& place);
    void walk_assignment(const BinaryExpr* assign);
    void walk_call(const CallExpr* call);
    void walk_var_decl(const VarDeclNode* var);
    void walk_return(const ReturnStmt* ret);
    void walk_loop(const ExprNode* condition, const ExprNode* iterable, SymbolID variable, const BlockStmt* body);
    const ConditionalBranch* select_branch(const ConditionalStmt* stmt) const;

    // events
    void tick_use(SymbolID name);
    void prune();
    /// Reports a conflict between a new borrow of `place` and the live borrows; true when it is allowed.
    bool check_borrow(const std::vector<SymbolID>& place, BorrowKind kind, SourceSpan span);
    void add_borrow(SymbolID borrower, std::vector<SymbolID> place, BorrowKind kind, SourceSpan span, size_t call_id);
    void report_read(const std::vector<SymbolID>& place, SourceSpan span);
    void report_write(const std::vector<SymbolID>& place, SourceSpan span);

    // helpers
    [[nodiscard]] bool is_live(const BorrowRecord& record) const;
    [[nodiscard]] std::string name_of(SymbolID id) const;
    [[nodiscard]] std::string place_text(const std::vector<SymbolID>& place) const;
};

} // namespace fool
