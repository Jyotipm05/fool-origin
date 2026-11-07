//
// Created by jmbab on 05-11-2026.
//

/**
 * @file Parser.ixx
 * @brief Pratt & recursive descent parser interface.
 */

module;

#include <cstddef>
#include <string_view>
#include <vector>
#include <string>

export module fool.parser;

import fool.common;
import fool.diagnostics;
import fool.lexer;
import fool.ast;

export namespace fool {

/**
 * @class Parser
 * @brief Parses linear token streams into an Arena-allocated ModuleAST.
 * @details Syntax errors are reported to the DiagnosticEngine; the parser then resynchronizes and
 *          keeps going so that one run reports as many independent errors as possible.
 */
class Parser {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<Token> m_tokens;
    size_t m_cursor{0};
    Arena& m_arena;
    DiagnosticEngine& m_diag;
    ModuleAST* m_current_module{nullptr};
    SymbolID m_current_section{INVALID_SYMBOL_ID}; ///< Module named by the latest 'module' line
    int m_speculation_depth{0};                    ///< >0: errors are swallowed and flagged instead
    bool m_speculation_failed{false};
    bool m_no_struct_literal{false};               ///< Inside if/while/for headers: '{' opens the body
    bool m_in_angle_list{false};                   ///< Inside '<...>': '>' closes the list, not an operator

public:
    // 2. Constructors and Destructors in the MIDDLE
    Parser(std::vector<Token> tokens, Arena& arena, DiagnosticEngine& diag);
    ~Parser() = default;

    // 3. Member functions LAST at the bottom
    ModuleAST* parse_module();

private:
    // --- token cursor (Parser.cpp)
    [[nodiscard]] const Token& current() const;
    [[nodiscard]] const Token& peek(size_t ahead = 1) const;
    [[nodiscard]] SourceLocation previous_end() const;
    Token advance();
    [[nodiscard]] bool check(TokenKind k) const;
    [[nodiscard]] bool check_identifier(std::string_view text) const;
    bool match(TokenKind k);
    [[nodiscard]] bool is_at_end() const;
    Token consume(TokenKind k, std::string_view err_msg);
    void error(const SourceSpan& span, std::string message);
    void error_here(std::string message);
    [[nodiscard]] SourceSpan span_from(SourceLocation start) const;
    void begin_speculation();
    bool end_speculation(); ///< Returns true if the speculative parse had no error

    // --- expressions (Parser.cpp)
    static int get_precedence(TokenKind k);
    ExprNode* parse_expression(int min_precedence = 0);
    ExprNode* parse_unary();
    ExprNode* parse_postfix(ExprNode* base);
    ExprNode* parse_primary();
    ExprNode* parse_path_expression();
    ExprNode* parse_intrinsic_call();
    ExprNode* parse_number();
    [[nodiscard]] bool looks_like_struct_body() const;
    void parse_call_arguments(CallExpr* call);

    // --- types and generics (Parser.cpp)
    TypeNode* parse_type(bool allow_generic_args = true);
    std::vector<GenericArg> parse_generic_args();
    std::vector<GenericParam> parse_generic_params();
    ExprNode* parse_angle_expression();

    // --- declarations (ParserDecl.cpp)
    DeclNode* parse_declaration();
    AttributeNode parse_attribute();
    void parse_module_header();
    ImportDeclNode* parse_import();
    std::string parse_module_path();
    DeclNode* parse_conditional_decl(std::vector<AttributeNode> attrs);
    FunctionDeclNode* parse_function(bool is_async);
    void parse_parameters(FunctionDeclNode* fn);
    SymbolID parse_function_name_token();
    RulesetDeclNode* parse_ruleset();
    ClassDeclNode* parse_class(bool is_struct);
    BlueprintDeclNode* parse_blueprint();
    EnumDeclNode* parse_enum();
    VarDeclNode* parse_var_decl();
    void synchronize_declaration();

    // --- statements (ParserDecl.cpp)
    BlockStmt* parse_block();
    StmtNode* parse_statement();
    StmtNode* parse_if();
    StmtNode* parse_while();
    StmtNode* parse_for();
    StmtNode* parse_conditional_stmt();
    void synchronize_statement();
};

} // namespace fool
