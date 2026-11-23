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
    SymbolID m_current_namespace{INVALID_SYMBOL_ID}; ///< Qualified name of the `namespace` being parsed (`a::b`), INVALID outside one
    bool m_namespace_pub{false};                   ///< Some enclosing `namespace` is `pub`: its members are public
    int m_speculation_depth{0};                    ///< >0: errors are swallowed and flagged instead
    bool m_speculation_failed{false};
    bool m_no_struct_literal{false};               ///< Inside if/while/for headers: '{' opens the body
    bool m_in_angle_list{false};                   ///< Inside '<...>': '>' closes the list, not an operator
    std::vector<CaptureName> m_pending_captures;   ///< `capture=[...]` of the call whose arguments were just parsed
    bool m_pending_has_capture{false};

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
    ExprNode* parse_named_cast(SourceLocation start, std::string_view name, CastKind kind);
    ExprNode* parse_number();
    [[nodiscard]] bool looks_like_struct_body() const;
    void parse_call_arguments(CallExpr* call);
    [[nodiscard]] bool looks_like_lambda(size_t paren_offset) const;   ///< `(` at peek(offset) starts `(name: T` or `() ->` / `() {`
    void parse_capture_list(std::vector<CaptureName>& out);            ///< `[a, b]`
    LambdaExpr* parse_lambda();                                        ///< `(params) -> R capture=[...] { body }`
    FunctionDeclNode* make_lambda_function(SourceLocation start);
    FunctionDeclNode* parse_lambda_function_decl();                    ///< `name = (params) -> R { body };` at module level

    // --- types and generics (Parser.cpp)
    TypeNode* parse_type(bool allow_generic_args = true);
    TypeNode* parse_type_without_optional(bool allow_generic_args);
    std::vector<GenericArg> parse_generic_args();
    std::vector<GenericParam> parse_generic_params();
    ExprNode* parse_angle_expression();

    // --- declarations (ParserDecl.cpp)
    DeclNode* parse_declaration();
    AttributeNode parse_attribute();
    void parse_module_header();
    ImportDeclNode* parse_import();
    /// Parses `[@root:]a:b:c` into `imp` and returns it as written; empty after a reported error.
    std::string parse_import_path(ImportDeclNode& imp);
    std::string parse_module_path();
    DeclNode* parse_conditional_decl(std::vector<AttributeNode> attrs);
    FunctionDeclNode* parse_function(bool is_async);
    void parse_parameters(FunctionDeclNode* fn);
    SymbolID parse_function_name_token();
    RulesetDeclNode* parse_ruleset();
    ClassDeclNode* parse_class(bool is_struct);
    BlueprintDeclNode* parse_blueprint();
    NamespaceDeclNode* parse_namespace(bool is_pub);
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
