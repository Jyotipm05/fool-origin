//
// Created by jmbab on 05-11-2026.
//
/**
 * @file Parser.cpp
 * @brief Implementation unit for the Fool Pratt parser and declaration rules.
 */

module;

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

module fool.parser;

namespace fool {

Parser::Parser(std::vector<Token> tokens, Arena& arena)
    : m_tokens(std::move(tokens)), m_arena(arena) {}

const Token& Parser::current() const {
    return m_tokens[m_cursor];
}

const Token& Parser::peek(size_t ahead) const {
    if (m_cursor + ahead >= m_tokens.size()) return m_tokens.back();
    return m_tokens[m_cursor + ahead];
}

Token Parser::advance() {
    if (!is_at_end()) m_cursor++;
    return m_tokens[m_cursor - 1];
}

bool Parser::check(TokenKind k) const {
    return current().m_kind == k;
}

bool Parser::is_at_end() const {
    return current().m_kind == TokenKind::Eof;
}

Token Parser::consume(TokenKind k, std::string_view err_msg) {
    if (check(k)) return advance();
    std::cerr << "Syntax Error at line " << current().m_span.m_start.m_line
              << ":" << current().m_span.m_start.m_column << " - " << err_msg << "\n";
    // If not at block/paren delimiter or EOF, advance past the unexpected token to prevent infinite loops
    if (!is_at_end() && !check(TokenKind::CloseBrace) && !check(TokenKind::CloseParen)) {
        return advance();
    }
    return current();
}

int Parser::get_precedence(TokenKind k) {
    switch (k) {
        case TokenKind::Plus:
        case TokenKind::Minus: return 10;
        case TokenKind::Star:
        case TokenKind::Slash: return 20;
        case TokenKind::OpenParen: return 30; // Call precedence
        default: return 0;
    }
}

ExprNode* Parser::parse_primary() {
    if (check(TokenKind::IntegerLiteral)) {
        Token tok = advance();
        int64_t val = std::stoll(std::string(tok.m_raw_lexeme));
        auto* node = m_arena.create<IntegerLiteralExpr>(val);
        node->m_span = tok.m_span;
        return node;
    }
    if (check(TokenKind::FloatLiteral)) {
        Token tok = advance();
        double val = std::stod(std::string(tok.m_raw_lexeme));
        auto* node = m_arena.create<FloatLiteralExpr>(val);
        node->m_span = tok.m_span;
        return node;
    }
    if (check(TokenKind::StringLiteral)) {
        Token tok = advance();
        auto* node = m_arena.create<StringLiteralExpr>(tok.m_raw_lexeme);
        node->m_span = tok.m_span;
        return node;
    }
    if (check(TokenKind::KwTrue) || check(TokenKind::KwFalse)) {
        Token tok = advance();
        auto* node = m_arena.create<BooleanLiteralExpr>(tok.m_kind == TokenKind::KwTrue);
        node->m_span = tok.m_span;
        return node;
    }
    if (check(TokenKind::Identifier)) {
        Token tok = advance();
        auto* node = m_arena.create<IdentifierExpr>(tok.m_symbol_id);
        node->m_span = tok.m_span;
        return node;
    }
    if (check(TokenKind::OpenParen)) {
        advance();
        ExprNode* expr = parse_expression(0);
        consume(TokenKind::CloseParen, "Expected ')'");
        return expr;
    }
    return nullptr;
}

ExprNode* Parser::parse_expression(int min_precedence) {
    ExprNode* lhs = parse_primary();

    while (!is_at_end()) {
        TokenKind op = current().m_kind;
        int prec = get_precedence(op);
        if (prec <= min_precedence) break;

        if (op == TokenKind::OpenParen) {
            advance();
            auto* call = m_arena.create<CallExpr>();
            call->m_callee = lhs;
            while (!check(TokenKind::CloseParen) && !is_at_end()) {
                size_t prev_cursor = m_cursor;
                CallArg arg;
                if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Colon) {
                    arg.m_label = advance().m_symbol_id;
                    advance(); // consume ':'
                }
                arg.m_expr = parse_expression(0);
                call->m_args.push_back(arg);
                if (!check(TokenKind::CloseParen)) consume(TokenKind::Comma, "Expected ','");
                if (m_cursor == prev_cursor && !is_at_end()) advance();
            }
            consume(TokenKind::CloseParen, "Expected ')' after call arguments");
            lhs = call;
        } else {
            advance();
            ExprNode* rhs = parse_expression(prec);
            auto* bin = m_arena.create<BinaryExpr>(op, lhs, rhs);
            lhs = bin;
        }
    }
    return lhs;
}

AttributeNode Parser::parse_attribute() {
    SourceLocation start = current().m_span.m_start;
    consume(TokenKind::At, "Expected '@'");
    Token name_tok = current();
    if (check(TokenKind::Identifier) || check(TokenKind::KwIf)) {
        advance();
    } else {
        name_tok = consume(TokenKind::Identifier, "Expected attribute name");
    }
    AttributeNode attr;
    auto& interner = StringInterner::get();
    attr.m_name = (name_tok.m_kind == TokenKind::KwIf) ? interner.intern("if") : name_tok.m_symbol_id;

    if (check(TokenKind::OpenParen)) {
        advance();
        if (interner.resolve(attr.m_name) == "if") {
            // Raw condition string supporting nested parens, def(...), &&, ==, etc.
            int paren_depth = 1;
            std::string condition;
            while (paren_depth > 0 && !is_at_end()) {
                if (check(TokenKind::OpenParen)) paren_depth++;
                else if (check(TokenKind::CloseParen)) {
                    paren_depth--;
                    if (paren_depth == 0) break;
                }
                if (!condition.empty() && current().m_kind != TokenKind::Dot && current().m_kind != TokenKind::OpenParen) {
                    condition += " ";
                }
                condition += current().m_raw_lexeme;
                advance();
            }
            consume(TokenKind::CloseParen, "Expected ')' after @if condition");
            attr.m_args.push_back(AttributeArg(interner.intern("condition"), interner.resolve(interner.intern(condition))));
        } else {
            while (!check(TokenKind::CloseParen) && !is_at_end()) {
                size_t prev_cursor = m_cursor;
                AttributeArg arg;
                if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Equal) {
                    arg.m_key = advance().m_symbol_id;
                    advance(); // consume '='
                }
                Token val = advance();
                arg.m_value = val.m_raw_lexeme;
                attr.m_args.push_back(arg);
                if (!check(TokenKind::CloseParen)) consume(TokenKind::Comma, "Expected ','");
                if (m_cursor == prev_cursor && !is_at_end()) advance();
            }
            consume(TokenKind::CloseParen, "Expected ')' after attribute arguments");
        }
    }
    attr.m_span = {start, current().m_span.m_end};
    return attr;
}

FunctionDeclNode* Parser::parse_function(bool is_async) {
    consume(TokenKind::KwFn, "Expected 'fn'");
    Token name = consume(TokenKind::Identifier, "Expected function name");
    auto* fn = m_arena.create<FunctionDeclNode>();
    fn->m_name = name.m_symbol_id;
    fn->m_is_async = is_async;

    consume(TokenKind::OpenParen, "Expected '(' after function name");

    // Explicit Receiver Check:
    // fn len(this const& self) -> ByConstRef (Rust-like strict immutable receiver)
    // fn append(this &self)    -> ByMutRef   (Default mutable receiver)
    // fn destroy(this self)    -> ByValue    (Consuming move receiver)
    if (check(TokenKind::KwThis)) {
        advance();
        if (check(TokenKind::KwConst) && peek().m_kind == TokenKind::Ampersand) {
            advance(); advance();
            fn->m_receiver.m_kind = ReceiverKind::ByConstRef;
        } else if (check(TokenKind::Ampersand)) {
            advance();
            fn->m_receiver.m_kind = ReceiverKind::ByMutRef;
        } else {
            fn->m_receiver.m_kind = ReceiverKind::ByValue;
        }
        Token self_tok = consume(TokenKind::Identifier, "Expected receiver name (e.g., 'self')");
        fn->m_receiver.m_self_name = self_tok.m_symbol_id;
        if (check(TokenKind::Comma)) advance();
    }

    // Parameters (supporting middle and trailing defaults)
    while (!check(TokenKind::CloseParen) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        ParameterDecl param;
        param.m_name = consume(TokenKind::Identifier, "Expected parameter name").m_symbol_id;
        consume(TokenKind::Colon, "Expected ':'");
        param.m_type_name = consume(TokenKind::Identifier, "Expected parameter type").m_symbol_id;

        if (check(TokenKind::Equal)) {
            advance();
            param.m_default_value = parse_expression(0);
        }
        fn->m_params.push_back(param);
        if (!check(TokenKind::CloseParen)) consume(TokenKind::Comma, "Expected ','");

        if (m_cursor == prev_cursor && !is_at_end()) advance();
    }
    consume(TokenKind::CloseParen, "Expected ')'");

    if (check(TokenKind::Arrow)) {
        advance();
        fn->m_return_type = consume(TokenKind::Identifier, "Expected return type").m_symbol_id;
    }

    if (check(TokenKind::Semicolon)) {
        // Interface declaration without body (e.g. inside .fo or ruleset)
        advance();
        return fn;
    }

    consume(TokenKind::OpenBrace, "Expected '{' or ';'");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        fn->m_body.push_back(parse_statement());
        if (m_cursor == prev_cursor && !is_at_end()) {
            advance();
        }
    }
    consume(TokenKind::CloseBrace, "Expected '}'");
    return fn;
}

RulesetDeclNode* Parser::parse_ruleset() {
    consume(TokenKind::KwRuleset, "Expected 'ruleset'");
    Token name = consume(TokenKind::Identifier, "Expected ruleset name");
    auto* rs = m_arena.create<RulesetDeclNode>();
    rs->m_name = name.m_symbol_id;

    if (check(TokenKind::Less)) {
        advance();
        rs->m_type_param = consume(TokenKind::Identifier, "Expected type parameter").m_symbol_id;
        if (check(TokenKind::Colon)) {
            advance();
            rs->m_bound = consume(TokenKind::Identifier, "Expected bound name").m_symbol_id;
        }
        consume(TokenKind::Greater, "Expected '>'");
    }

    consume(TokenKind::OpenBrace, "Expected '{'");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        if (check(TokenKind::KwFn)) {
            rs->m_methods.push_back(parse_function(false));
        } else {
            advance();
        }
        if (m_cursor == prev_cursor && !is_at_end()) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}'");
    return rs;
}

ClassDeclNode* Parser::parse_class() {
    consume(TokenKind::KwClass, "Expected 'class'");
    Token name = consume(TokenKind::Identifier, "Expected class name");
    auto* cls = m_arena.create<ClassDeclNode>();
    cls->m_name = name.m_symbol_id;

    if (check(TokenKind::Less)) {
        advance();
        while (!check(TokenKind::Greater) && !is_at_end()) {
            advance();
        }
        consume(TokenKind::Greater, "Expected '>'");
    }

    consume(TokenKind::OpenBrace, "Expected '{'");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        if (check(TokenKind::KwFn)) {
            cls->m_methods.push_back(parse_function(false));
        } else if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Colon) {
            ParameterDecl field;
            field.m_name = advance().m_symbol_id;
            advance(); // consume ':'
            field.m_type_name = consume(TokenKind::Identifier, "Expected field type").m_symbol_id;
            consume(TokenKind::Semicolon, "Expected ';'");
            cls->m_fields.push_back(field);
        } else {
            advance();
        }
        if (m_cursor == prev_cursor && !is_at_end()) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}'");
    return cls;
}

BlueprintDeclNode* Parser::parse_blueprint() {
    consume(TokenKind::KwBlueprint, "Expected 'blueprint'");
    Token name = consume(TokenKind::Identifier, "Expected blueprint name");
    auto* bp = m_arena.create<BlueprintDeclNode>();
    bp->m_name = name.m_symbol_id;

    if (check(TokenKind::Less)) {
        advance();
        while (!check(TokenKind::Greater) && !is_at_end()) {
            ParameterDecl param;
            param.m_name = consume(TokenKind::Identifier, "Expected template param name").m_symbol_id;
            if (check(TokenKind::Colon)) {
                advance();
                param.m_type_name = consume(TokenKind::Identifier, "Expected param type").m_symbol_id;
            }
            if (check(TokenKind::Equal)) {
                advance();
                param.m_default_value = parse_expression(0);
            }
            bp->m_type_params.push_back(param);
            if (!check(TokenKind::Greater)) consume(TokenKind::Comma, "Expected ','");
        }
        consume(TokenKind::Greater, "Expected '>'");
    }

    consume(TokenKind::OpenBrace, "Expected '{'");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        DeclNode* decl = parse_declaration();
        if (decl) bp->m_declarations.push_back(decl);
        if (m_cursor == prev_cursor && !is_at_end()) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}'");
    return bp;
}

EnumDeclNode* Parser::parse_enum() {
    consume(TokenKind::KwEnum, "Expected 'enum'");
    Token name = consume(TokenKind::Identifier, "Expected enum name");
    auto* en = m_arena.create<EnumDeclNode>();
    en->m_name = name.m_symbol_id;

    consume(TokenKind::OpenBrace, "Expected '{'");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t prev_cursor = m_cursor;
        if (check(TokenKind::Identifier)) {
            en->m_variants.push_back(advance().m_symbol_id);
            if (check(TokenKind::Comma)) advance();
        } else {
            advance();
        }
        if (m_cursor == prev_cursor && !is_at_end()) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}'");
    return en;
}

VarDeclNode* Parser::parse_var_decl() {
    BindingKind kind = BindingKind::Mutable;
    if (check(TokenKind::KwConstx)) {
        kind = BindingKind::Constx;
        advance();
    } else if (check(TokenKind::KwConst)) {
        kind = BindingKind::Immutable;
        advance();
    } else if (check(TokenKind::KwVar)) {
        kind = BindingKind::Mutable;
        advance();
    }

    Token name = consume(TokenKind::Identifier, "Expected variable name");
    SymbolID type_name = INVALID_SYMBOL_ID;
    if (check(TokenKind::Colon)) {
        advance();
        type_name = consume(TokenKind::Identifier, "Expected variable type").m_symbol_id;
    }

    ExprNode* init = nullptr;
    if (check(TokenKind::Equal)) {
        advance();
        init = parse_expression(0);
    }

    consume(TokenKind::Semicolon, "Expected ';'");
    return m_arena.create<VarDeclNode>(name.m_symbol_id, type_name, init, kind);
}

StmtNode* Parser::parse_statement() {
    size_t prev_cursor = m_cursor;

    // Compile-time conditional branch inside function:
    // @if (cond) { ... } @elif (cond) { ... } @else { ... } OR single-statement @if (cond) stmt;
    if (check(TokenKind::At) && (peek().m_kind == TokenKind::KwIf || peek().m_raw_lexeme == "if")) {
        std::vector<ConditionalBranch> branches;

        while (check(TokenKind::At) && !is_at_end()) {
            std::string_view dir_name = peek().m_raw_lexeme;
            if (peek().m_kind == TokenKind::KwIf) dir_name = "if";
            else if (peek().m_kind == TokenKind::KwElse) dir_name = "else";

            if (dir_name != "if" && dir_name != "elif" && dir_name != "else") {
                break;
            }

            advance(); // consume '@'
            advance(); // consume 'if' / 'elif' / 'else'

            std::string condition;
            if (dir_name == "if" || dir_name == "elif") {
                consume(TokenKind::OpenParen, "Expected '(' after @if / @elif");
                int paren_depth = 1;
                while (paren_depth > 0 && !is_at_end()) {
                    if (check(TokenKind::OpenParen)) paren_depth++;
                    else if (check(TokenKind::CloseParen)) {
                        paren_depth--;
                        if (paren_depth == 0) break;
                    }
                    if (!condition.empty() && current().m_kind != TokenKind::Dot && current().m_kind != TokenKind::OpenParen) {
                        condition += " ";
                    }
                    condition += current().m_raw_lexeme;
                    advance();
                }
                consume(TokenKind::CloseParen, "Expected ')' after condition");
            }

            std::vector<StmtNode*> body;
            if (check(TokenKind::OpenBrace)) {
                advance(); // consume '{'
                while (!check(TokenKind::CloseBrace) && !is_at_end()) {
                    size_t prev_cur = m_cursor;
                    body.push_back(parse_statement());
                    if (m_cursor == prev_cur && !is_at_end()) advance();
                }
                consume(TokenKind::CloseBrace, "Expected '}'");
            } else {
                body.push_back(parse_statement());
            }

            branches.emplace_back(std::move(condition), std::move(body));

            if (dir_name == "else") {
                break;
            }
        }
        return m_arena.create<ConditionalStmt>(std::move(branches));
    }

    if (check(TokenKind::KwReturn)) {
        advance();
        auto* ret = m_arena.create<ReturnStmt>();
        if (!check(TokenKind::Semicolon)) {
            ret->m_value = parse_expression(0);
        }
        consume(TokenKind::Semicolon, "Expected ';'");
        return ret;
    }

    if (check(TokenKind::KwVar) || check(TokenKind::KwConst) || check(TokenKind::KwConstx)) {
        return m_arena.create<DeclStmt>(parse_var_decl());
    }

    // Check for inline assignment / declaration: name = expr;
    if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Equal) {
        return m_arena.create<DeclStmt>(parse_var_decl());
    }

    ExprNode* expr = parse_expression(0);
    consume(TokenKind::Semicolon, "Expected ';'");

    // Panic recovery: if no tokens were consumed in this statement, advance to guarantee loop progress
    if (m_cursor == prev_cursor && !is_at_end()) {
        std::cerr << "Syntax Error at line " << current().m_span.m_start.m_line
                  << ":" << current().m_span.m_start.m_column << " - Unexpected token '"
                  << current().m_raw_lexeme << "'\n";
        advance();
        return nullptr;
    }

    return m_arena.create<ExprStmt>(expr);
}

DeclNode* Parser::parse_declaration() {
    std::vector<AttributeNode> attributes;
    while (check(TokenKind::At)) {
        attributes.push_back(parse_attribute());
    }

    // Check for multi-declaration compile-time block: @if (cond) { decl1; decl2; }
    if (check(TokenKind::OpenBrace) && !attributes.empty()) {
        auto& interner = StringInterner::get();
        if (attributes.back().m_name == interner.intern("if")) {
            advance(); // consume '{'
            std::vector<DeclNode*> block_decls;
            while (!check(TokenKind::CloseBrace) && !is_at_end()) {
                size_t prev_cur = m_cursor;
                DeclNode* d = parse_declaration();
                if (d) block_decls.push_back(d);
                if (m_cursor == prev_cur && !is_at_end()) advance();
            }
            consume(TokenKind::CloseBrace, "Expected '}'");

            std::string cond_str;
            if (!attributes.back().m_args.empty()) {
                cond_str = attributes.back().m_args[0].m_value;
            }
            std::vector<ConditionalDeclBranch> branches;
            branches.emplace_back(std::move(cond_str), std::move(block_decls));
            auto* block = m_arena.create<ConditionalDeclBlock>(std::move(branches));
            block->m_attributes = std::move(attributes);
            return block;
        }
    }

    DeclNode* decl = nullptr;
    if (check(TokenKind::HashImport) || (check(TokenKind::Hash) && peek().m_kind == TokenKind::KwImport) || check(TokenKind::KwImport)) {
        if (check(TokenKind::HashImport)) {
            advance();
        } else if (check(TokenKind::Hash)) {
            advance(); // consume '#'
            advance(); // consume 'import'
        } else {
            advance(); // consume 'import'
        }
        std::string mod_path;
        while (!check(TokenKind::Semicolon) && !check(TokenKind::Less) && !is_at_end()) {
            mod_path += current().m_raw_lexeme;
            advance();
        }
        SymbolID sym = StringInterner::get().intern(mod_path);
        auto* imp = m_arena.create<ImportDeclNode>(sym);

        // Parse compile-time import template arguments: <A = ..., B = ...>
        if (check(TokenKind::Less)) {
            advance(); // consume '<'
            while (!check(TokenKind::Greater) && !is_at_end()) {
                size_t prev_cur = m_cursor;
                SymbolID arg_name = INVALID_SYMBOL_ID;
                if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Equal) {
                    arg_name = advance().m_symbol_id;
                    consume(TokenKind::Equal, "Expected '=' in import template argument");
                }
                ExprNode* val = parse_expression(0);
                imp->m_template_args.push_back(ImportArg(arg_name, val));

                if (check(TokenKind::Comma)) advance();
                if (m_cursor == prev_cur && !is_at_end()) advance();
            }
            consume(TokenKind::Greater, "Expected '>' after import template arguments");
        }

        consume(TokenKind::Semicolon, "Expected ';' after import path");
        decl = imp;
    } else if (check(TokenKind::KwAsync)) {
        advance();
        decl = parse_function(true);
    } else if (check(TokenKind::KwFn)) {
        decl = parse_function(false);
    } else if (check(TokenKind::KwRuleset)) {
        decl = parse_ruleset();
    } else if (check(TokenKind::KwClass)) {
        decl = parse_class();
    } else if (check(TokenKind::KwBlueprint)) {
        decl = parse_blueprint();
    } else if (check(TokenKind::KwEnum)) {
        decl = parse_enum();
    } else if (check(TokenKind::KwConstx) || check(TokenKind::KwConst) || check(TokenKind::KwVar)) {
        decl = parse_var_decl();
    } else if (check(TokenKind::KwModule)) {
        advance(); // consume 'module'
        std::string mod_name;
        while (!check(TokenKind::Semicolon) && !check(TokenKind::Less) && !is_at_end()) {
            mod_name += current().m_raw_lexeme;
            advance();
        }
        auto& interner = StringInterner::get();
        if (m_current_module) {
            m_current_module->m_module_name = interner.intern(mod_name);
        }

        // Module template parameters: module name<A = 1, B = 0>;
        if (check(TokenKind::Less)) {
            advance(); // consume '<'
            while (!check(TokenKind::Greater) && !is_at_end()) {
                size_t prev_cur = m_cursor;
                if (check(TokenKind::Identifier)) {
                    SymbolID p_name = advance().m_symbol_id;
                    ExprNode* def_val = nullptr;
                    if (check(TokenKind::Equal)) {
                        advance();
                        def_val = parse_expression(0);
                    }
                    if (m_current_module) {
                        m_current_module->m_parameters.push_back(ModuleParam(p_name, def_val));
                    }
                }
                if (check(TokenKind::Comma)) advance();
                if (m_cursor == prev_cur && !is_at_end()) advance();
            }
            consume(TokenKind::Greater, "Expected '>' after module template parameters");
        }
        consume(TokenKind::Semicolon, "Expected ';'");
        return nullptr;
    } else {
        advance();
        return nullptr;
    }

    if (decl) decl->m_attributes = std::move(attributes);
    return decl;
}

ModuleAST* Parser::parse_module() {
    auto* mod = m_arena.create<ModuleAST>();
    m_current_module = mod;
    while (!is_at_end()) {
        size_t prev_cursor = m_cursor;
        DeclNode* decl = parse_declaration();
        if (decl) mod->m_declarations.push_back(decl);
        if (m_cursor == prev_cursor && !is_at_end()) {
            advance();
        }
    }
    return mod;
}

} // namespace fool
