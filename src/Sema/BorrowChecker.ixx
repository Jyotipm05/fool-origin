//
// Created by jmbab on 06-11-2026.
//
/**
 * @file BorrowChecker.ixx
 * @brief Opt-in Borrow Checker pass enforcing Aliasing XOR Mutability and non-lexical borrow validation.
 */

module;

#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <cstdint>

export module fool.borrow;

import fool.common;
import fool.lexer;
import fool.ast;
import fool.sema;

export namespace fool {

enum class BorrowKind {
    Shared,   ///< const& T (multiple concurrent readers allowed)
    Exclusive ///< &T (exactly one active writer, no concurrent readers)
};

struct BorrowRecord {
    // 1. Member variables FIRST on top with 'm_' prefix
    SymbolID m_borrower{INVALID_SYMBOL_ID};
    SymbolID m_origin{INVALID_SYMBOL_ID};
    BorrowKind m_kind{BorrowKind::Shared};
    SourceSpan m_span;
    size_t m_start_statement{0};
    size_t m_end_statement{0};

    // 2. Constructors in the MIDDLE
    BorrowRecord() = default;
    BorrowRecord(SymbolID borrower, SymbolID origin, BorrowKind kind, SourceSpan span, size_t start, size_t end)
        : m_borrower(borrower), m_origin(origin), m_kind(kind), m_span(span),
          m_start_statement(start), m_end_statement(end) {}
};

class BorrowCheckerPass {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<BorrowRecord> m_active_borrows;
    std::vector<std::string> m_diagnostics;
    size_t m_statement_index{0};
    size_t m_error_count{0};

public:
    // 2. Constructors in the MIDDLE
    BorrowCheckerPass() = default;
    ~BorrowCheckerPass() = default;

    // 3. Member functions LAST at the bottom
    bool check_function(const FunctionDeclNode* fn);
    bool check_module(const ModuleAST* module);
    [[nodiscard]] size_t get_error_count() const { return m_error_count; }
    [[nodiscard]] const std::vector<std::string>& get_diagnostics() const { return m_diagnostics; }

private:
    void record_borrow(SymbolID borrower, SymbolID origin, BorrowKind kind, SourceSpan span);
    void validate_mutation(SymbolID origin, SourceSpan span);
    void report_borrow_error(std::string_view msg, SourceSpan span);
};

} // namespace fool

