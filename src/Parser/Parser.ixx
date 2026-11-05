//
// Created by jmbab on 05-11-2026.
//

/**
 * @file Parser.ixx
 * @brief Pratt & recursive descent parser interface.
 */

module;

#include <vector>
#include <string_view>

export module fool.parser;

import fool.common;
import fool.lexer;
import fool.ast;

export namespace fool {

/**
 * @class Parser
 * @brief Parses linear token streams into an Arena-allocated ModuleAST.
 */
class Parser {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<Token> m_tokens;
    size_t m_cursor{0};
    Arena& m_arena;

public:
    // 2. Constructors and Destructors in the MIDDLE
    Parser(std::vector<Token> tokens, Arena& arena);
    ~Parser() = default;

    // 3. Member functions LAST at the bottom
    ModuleAST* parse_module();

private:
    [[nodiscard]] const Token& current() const;
    [[nodiscard]] const Token& peek(size_t ahead = 1) const;
    Token advance();
    [[nodiscard]] bool check(TokenKind k) const;
    [[nodiscard]] bool is_at_end() const;
    Token consume(TokenKind k, std::string_view err_msg);

    static int get_precedence(TokenKind k);
    ExprNode* parse_expression(int precedence = 0);
    ExprNode* parse_primary();

    DeclNode* parse_declaration();
    AttributeNode parse_attribute();
    FunctionDeclNode* parse_function(bool is_async);
    RulesetDeclNode* parse_ruleset();
    ClassDeclNode* parse_class();
    BlueprintDeclNode* parse_blueprint();
    EnumDeclNode* parse_enum();
    StmtNode* parse_statement();
    VarDeclNode* parse_var_decl();
};

} // namespace fool
