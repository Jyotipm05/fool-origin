//
// Created by jmbab on 05-11-2026.
//

/**
 * @file ParserDecl.cpp
 * @brief Parser: module headers, imports, attributes, declarations and statements.
 */

module;

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.parser;

namespace fool {

namespace {

std::string_view stable_text(std::string_view text) {
    auto& interner = StringInterner::get();
    return interner.resolve(interner.intern(text));
}

std::string describe(const Token& tok) {
    return std::string(tok.m_raw_lexeme.empty() ? token_kind_to_str(tok.m_kind) : tok.m_raw_lexeme);
}

/// True for identifiers and keywords: module path segments may spell keywords (e.g. std::async).
bool is_name_token(const Token& tok) {
    if (tok.m_raw_lexeme.empty()) return false;
    char c = tok.m_raw_lexeme.front();
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_operator_token(TokenKind k) {
    switch (k) {
        case TokenKind::EqualEqual:
        case TokenKind::ExclaimEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
        case TokenKind::Caret:
        case TokenKind::Bang:
            return true;
        default:
            return false;
    }
}

bool starts_declaration(TokenKind k) {
    switch (k) {
        case TokenKind::KwFn:
        case TokenKind::KwAsync:
        case TokenKind::KwClass:
        case TokenKind::KwStruct:
        case TokenKind::KwRuleset:
        case TokenKind::KwBlueprint:
        case TokenKind::KwEnum:
        case TokenKind::KwModule:
        case TokenKind::KwVar:
        case TokenKind::KwConst:
        case TokenKind::KwConstx:
        case TokenKind::HashImport:
        case TokenKind::At:
        case TokenKind::Eof:
            return true;
        default:
            return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Module level
// ---------------------------------------------------------------------------------------------

ModuleAST* Parser::parse_module() {
    auto* module_ast = m_arena.create<ModuleAST>();
    m_current_module = module_ast;
    module_ast->m_span.m_start = current().m_span.m_start;

    while (!is_at_end()) {
        size_t before = m_cursor;
        if (check(TokenKind::KwModule)) {
            parse_module_header();
        } else if (DeclNode* decl = parse_declaration()) {
            module_ast->m_declarations.push_back(decl);
        }
        if (m_cursor == before) advance(); // guarantee forward progress after an error
    }
    module_ast->m_span.m_end = current().m_span.m_end;
    return module_ast;
}

std::string Parser::parse_module_path() {
    std::string path;
    if (!is_name_token(current())) {
        error_here("Expected module name, found '" + describe(current()) + "'");
        return path;
    }
    path = std::string(advance().m_raw_lexeme);
    while (check(TokenKind::DoubleColon) && is_name_token(peek())) {
        advance();
        path += "::";
        path += std::string(advance().m_raw_lexeme);
    }
    return path;
}

void Parser::parse_module_header() {
    advance(); // 'module'
    std::string path = parse_module_path();
    SymbolID name = path.empty() ? INVALID_SYMBOL_ID : StringInterner::get().intern(path);

    std::vector<ModuleParam> params;
    if (match(TokenKind::Less)) {
        while (!check(TokenKind::Greater) && !is_at_end()) {
            Token pname = consume(TokenKind::Identifier, "Expected module parameter name");
            if (pname.m_kind == TokenKind::Invalid) break;
            ModuleParam param{pname.m_symbol_id, nullptr};
            if (match(TokenKind::Equal)) {
                param.m_default_value = parse_angle_expression();
                if (param.m_default_value == nullptr) break;
            }
            params.push_back(param);
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::Greater, "Expected '>' to close module parameter list");
    }
    consume(TokenKind::Semicolon, "Expected ';' after module declaration");

    if (name == INVALID_SYMBOL_ID) return;
    m_current_section = name;
    if (m_current_module->m_module_name == INVALID_SYMBOL_ID) {
        m_current_module->m_module_name = name;
        m_current_module->m_parameters = std::move(params);
    }
    m_current_module->m_module_sections.push_back(name);
}

ImportDeclNode* Parser::parse_import() {
    advance(); // '#import'
    std::string path = parse_module_path();
    if (path.empty()) {
        synchronize_declaration();
        return nullptr;
    }
    auto* import_decl = m_arena.create<ImportDeclNode>(StringInterner::get().intern(path));

    if (match(TokenKind::Less)) {
        while (!check(TokenKind::Greater) && !is_at_end()) {
            ImportArg arg;
            arg.m_span.m_start = current().m_span.m_start;
            Token name = consume(TokenKind::Identifier, "Expected template argument name");
            if (name.m_kind == TokenKind::Invalid) break;
            arg.m_name = name.m_symbol_id;
            consume(TokenKind::Equal, "Expected '=' after template argument name");
            arg.m_value = parse_angle_expression();
            arg.m_span.m_end = previous_end();
            if (arg.m_value == nullptr) break;
            import_decl->m_template_args.push_back(arg);
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::Greater, "Expected '>' to close import template arguments");
    }
    consume(TokenKind::Semicolon, "Expected ';' after #import");
    return import_decl;
}

AttributeNode Parser::parse_attribute() {
    SourceLocation start = current().m_span.m_start;
    advance(); // '@'
    AttributeNode attr;
    Token name = consume(TokenKind::Identifier, "Expected attribute name after '@'");
    attr.m_name = name.m_symbol_id;

    if (match(TokenKind::OpenParen)) {
        while (!check(TokenKind::CloseParen) && !is_at_end()) {
            AttributeArg arg;
            if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Equal) {
                arg.m_key = advance().m_symbol_id;
                advance(); // '='
            }
            Token value = current();
            if (value.m_kind == TokenKind::StringLiteral) {
                std::string_view raw = value.m_raw_lexeme;
                if (raw.size() >= 2) raw = raw.substr(1, raw.size() - 2);
                arg.m_value = stable_text(raw);
                advance();
            } else if (value.m_kind == TokenKind::Identifier || value.m_kind == TokenKind::IntegerLiteral ||
                       value.m_kind == TokenKind::KwTrue || value.m_kind == TokenKind::KwFalse) {
                arg.m_value = stable_text(value.m_raw_lexeme);
                advance();
            } else {
                error_here("Expected attribute argument value, found '" + describe(value) + "'");
                break;
            }
            attr.m_args.push_back(arg);
            if (!match(TokenKind::Comma)) break;
        }
        consume(TokenKind::CloseParen, "Expected ')' after attribute arguments");
    }
    attr.m_span = span_from(start);
    return attr;
}

void Parser::synchronize_declaration() {
    if (!is_at_end()) advance();
    while (!starts_declaration(current().m_kind)) advance();
}

DeclNode* Parser::parse_declaration() {
    SourceLocation start = current().m_span.m_start;
    std::vector<AttributeNode> attrs;

    while (check(TokenKind::At)) {
        const Token& next = peek();
        if (next.m_kind == TokenKind::KwIf) {
            return parse_conditional_decl(std::move(attrs));
        }
        if (next.m_kind == TokenKind::KwElse || (next.m_kind == TokenKind::Identifier && next.m_raw_lexeme == "elif")) {
            error_here("'@" + std::string(next.m_raw_lexeme) + "' without a preceding '@if'");
            advance();
            advance();
            continue;
        }
        attrs.push_back(parse_attribute());
    }

    DeclNode* decl = nullptr;
    switch (current().m_kind) {
        case TokenKind::HashImport:
            decl = parse_import();
            break;
        case TokenKind::KwFn:
            advance();
            decl = parse_function(false);
            break;
        case TokenKind::KwAsync:
            advance();
            if (match(TokenKind::KwFn)) {
                decl = parse_function(true);
            } else {
                error_here("Expected 'fn' after 'async'");
                synchronize_declaration();
            }
            break;
        case TokenKind::KwClass:
            advance();
            decl = parse_class(false);
            break;
        case TokenKind::KwStruct:
            advance();
            decl = parse_class(true);
            break;
        case TokenKind::KwRuleset:
            advance();
            decl = parse_ruleset();
            break;
        case TokenKind::KwBlueprint:
            advance();
            decl = parse_blueprint();
            break;
        case TokenKind::KwEnum:
            advance();
            decl = parse_enum();
            break;
        case TokenKind::KwVar:
        case TokenKind::KwConst:
        case TokenKind::KwConstx:
            decl = parse_var_decl();
            break;
        case TokenKind::KwModule:
            parse_module_header();
            return nullptr;
        case TokenKind::Eof:
            if (!attrs.empty()) error_here("Attribute is not followed by a declaration");
            return nullptr;
        default:
            error_here("Unexpected '" + describe(current()) + "' at declaration level");
            synchronize_declaration();
            return nullptr;
    }

    if (decl != nullptr) {
        decl->m_attributes = std::move(attrs);
        decl->m_module = m_current_section;
        decl->m_span = span_from(start);
    }
    return decl;
}

/// `@if cond decl`  |  `@if cond { decls } [@elif cond { decls }]... [@else { decls }]`
DeclNode* Parser::parse_conditional_decl(std::vector<AttributeNode> attrs) {
    SourceLocation start = current().m_span.m_start;
    auto* block = m_arena.create<ConditionalDeclBlock>();
    block->m_attributes = std::move(attrs);

    advance(); // '@'
    advance(); // 'if'
    bool is_else = false;
    while (true) {
        ConditionalDeclBranch branch;
        if (!is_else) {
            bool saved = m_no_struct_literal;
            m_no_struct_literal = true;
            branch.m_condition = parse_expression();
            m_no_struct_literal = saved;
            if (branch.m_condition == nullptr) {
                synchronize_declaration();
                return nullptr;
            }
        }

        if (check(TokenKind::OpenBrace)) {
            advance();
            while (!check(TokenKind::CloseBrace) && !is_at_end()) {
                size_t before = m_cursor;
                if (DeclNode* inner = parse_declaration()) branch.m_declarations.push_back(inner);
                if (m_cursor == before) advance();
            }
            consume(TokenKind::CloseBrace, "Expected '}' to close conditional declaration block");
        } else if (DeclNode* inner = parse_declaration()) {
            branch.m_declarations.push_back(inner);
        }
        block->m_branches.push_back(std::move(branch));

        if (is_else) break;
        if (check(TokenKind::At) && peek().m_kind == TokenKind::Identifier && peek().m_raw_lexeme == "elif") {
            advance();
            advance();
        } else if (check(TokenKind::At) && peek().m_kind == TokenKind::KwElse) {
            advance();
            advance();
            is_else = true;
        } else {
            break;
        }
    }
    block->m_module = m_current_section;
    block->m_span = span_from(start);
    return block;
}

// ---------------------------------------------------------------------------------------------
// Functions, classes, rulesets, blueprints, enums, variables
// ---------------------------------------------------------------------------------------------

SymbolID Parser::parse_function_name_token() {
    if (!check(TokenKind::Identifier)) {
        error_here("Expected function name, found '" + describe(current()) + "'");
        return INVALID_SYMBOL_ID;
    }
    Token name = advance();
    if (name.m_raw_lexeme == "operator" && is_operator_token(current().m_kind)) {
        Token op = advance();
        std::string full = "operator" + std::string(op.m_raw_lexeme);
        return StringInterner::get().intern(full);
    }
    return name.m_symbol_id;
}

void Parser::parse_parameters(FunctionDeclNode* fn) {
    if (check(TokenKind::KwThis)) {
        advance();
        ReceiverKind kind = ReceiverKind::ByValue;
        if (check(TokenKind::KwConst) && peek().m_kind == TokenKind::Ampersand) {
            advance();
            advance();
            kind = ReceiverKind::ByConstRef;
        } else if (match(TokenKind::Ampersand)) {
            kind = ReceiverKind::ByMutRef;
        }
        Token self_name = consume(TokenKind::Identifier, "Expected receiver name after 'this'");
        fn->m_receiver = ReceiverDecl{kind, self_name.m_symbol_id};
        if (!match(TokenKind::Comma)) return;
    }

    while (!check(TokenKind::CloseParen) && !is_at_end()) {
        ParameterDecl param;
        param.m_span.m_start = current().m_span.m_start;
        Token name = consume(TokenKind::Identifier, "Expected parameter name");
        if (name.m_kind == TokenKind::Invalid) break;
        param.m_name = name.m_symbol_id;
        consume(TokenKind::Colon, "Expected ':' after parameter name");
        param.m_type = parse_type();
        if (param.m_type == nullptr) break;
        if (match(TokenKind::Equal)) {
            param.m_default_value = parse_expression();
            if (param.m_default_value == nullptr) break;
        }
        param.m_span.m_end = previous_end();
        fn->m_params.push_back(param);
        if (!match(TokenKind::Comma)) break;
    }
}

FunctionDeclNode* Parser::parse_function(bool is_async) {
    SourceLocation start = current().m_span.m_start;
    auto* fn = m_arena.create<FunctionDeclNode>();
    fn->m_is_async = is_async;
    fn->m_module = m_current_section;

    fn->m_name = parse_function_name_token();
    if (fn->m_name == INVALID_SYMBOL_ID) {
        synchronize_declaration();
        return nullptr;
    }

    if (check(TokenKind::Less)) {
        // `fn Owner<T>::name` / `fn Owner<SIMD = true>::name` versus `fn name<T>(...)`
        size_t save = m_cursor;
        begin_speculation();
        std::vector<GenericArg> args = parse_generic_args();
        bool is_owner_form = end_speculation() && check(TokenKind::DoubleColon);
        if (is_owner_form) {
            fn->m_owner_args = std::move(args);
        } else {
            m_cursor = save;
            fn->m_type_params = parse_generic_params();
        }
    }

    if (match(TokenKind::DoubleColon)) {
        fn->m_owner = fn->m_name;
        fn->m_name = parse_function_name_token();
        if (fn->m_name == INVALID_SYMBOL_ID) {
            synchronize_declaration();
            return nullptr;
        }
        if (check(TokenKind::DoubleColon)) {
            error_here("Nested method paths are not supported; use 'fn Owner::name'");
        }
    }

    consume(TokenKind::OpenParen, "Expected '(' to start parameter list");
    parse_parameters(fn);
    consume(TokenKind::CloseParen, "Expected ')' after parameter list");

    if (match(TokenKind::Arrow)) {
        fn->m_return_type = parse_type();
    }

    if (check(TokenKind::OpenBrace)) {
        fn->m_has_body = true;
        if (BlockStmt* body = parse_block()) fn->m_body = std::move(body->m_statements);
    } else {
        consume(TokenKind::Semicolon, "Expected ';' after function declaration or a '{' function body");
    }
    fn->m_span = span_from(start);
    return fn;
}

RulesetDeclNode* Parser::parse_ruleset() {
    auto* ruleset = m_arena.create<RulesetDeclNode>();
    Token name = consume(TokenKind::Identifier, "Expected ruleset name");
    ruleset->m_name = name.m_symbol_id;
    if (check(TokenKind::Less)) ruleset->m_type_params = parse_generic_params();

    consume(TokenKind::OpenBrace, "Expected '{' after ruleset name");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t before = m_cursor;
        bool is_async = match(TokenKind::KwAsync);
        if (match(TokenKind::KwFn)) {
            if (FunctionDeclNode* fn = parse_function(is_async)) ruleset->m_methods.push_back(fn);
        } else {
            error_here("Expected 'fn' inside ruleset, found '" + describe(current()) + "'");
            synchronize_statement();
        }
        if (m_cursor == before) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}' to close ruleset");
    return ruleset;
}

ClassDeclNode* Parser::parse_class(bool is_struct) {
    auto* cls = m_arena.create<ClassDeclNode>();
    cls->m_is_struct = is_struct;
    Token name = consume(TokenKind::Identifier, is_struct ? "Expected struct name" : "Expected class name");
    cls->m_name = name.m_symbol_id;
    if (check(TokenKind::Less)) cls->m_type_params = parse_generic_params();

    consume(TokenKind::OpenBrace, "Expected '{' after type name");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t before = m_cursor;
        std::vector<AttributeNode> attrs;
        while (check(TokenKind::At)) attrs.push_back(parse_attribute());

        bool is_async = check(TokenKind::KwAsync);
        if (is_async) advance();
        if (match(TokenKind::KwFn)) {
            if (FunctionDeclNode* fn = parse_function(is_async)) {
                fn->m_attributes = std::move(attrs);
                cls->m_methods.push_back(fn);
            }
        } else if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Colon) {
            ParameterDecl field;
            field.m_span.m_start = current().m_span.m_start;
            field.m_name = advance().m_symbol_id;
            advance(); // ':'
            field.m_type = parse_type();
            field.m_span.m_end = previous_end();
            consume(TokenKind::Semicolon, "Expected ';' after field declaration");
            if (field.m_type != nullptr) cls->m_fields.push_back(field);
        } else {
            error_here("Expected field or method in type body, found '" + describe(current()) + "'");
            synchronize_statement();
        }
        if (m_cursor == before) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}' to close type body");
    return cls;
}

BlueprintDeclNode* Parser::parse_blueprint() {
    auto* blueprint = m_arena.create<BlueprintDeclNode>();
    Token name = consume(TokenKind::Identifier, "Expected blueprint name");
    blueprint->m_name = name.m_symbol_id;
    if (check(TokenKind::Less)) blueprint->m_type_params = parse_generic_params();

    consume(TokenKind::OpenBrace, "Expected '{' after blueprint name");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t before = m_cursor;
        bool is_async = match(TokenKind::KwAsync);
        if (match(TokenKind::KwFn)) {
            if (FunctionDeclNode* fn = parse_function(is_async)) blueprint->m_declarations.push_back(fn);
        } else {
            error_here("Expected 'fn' inside blueprint, found '" + describe(current()) + "'");
            synchronize_statement();
        }
        if (m_cursor == before) advance();
    }
    consume(TokenKind::CloseBrace, "Expected '}' to close blueprint");
    return blueprint;
}

EnumDeclNode* Parser::parse_enum() {
    auto* en = m_arena.create<EnumDeclNode>();
    Token name = consume(TokenKind::Identifier, "Expected enum name");
    en->m_name = name.m_symbol_id;
    if (check(TokenKind::Less)) en->m_type_params = parse_generic_params();

    consume(TokenKind::OpenBrace, "Expected '{' after enum name");
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        EnumVariant variant;
        variant.m_span.m_start = current().m_span.m_start;
        Token vname = consume(TokenKind::Identifier, "Expected enum variant name");
        if (vname.m_kind == TokenKind::Invalid) break;
        variant.m_name = vname.m_symbol_id;
        if (match(TokenKind::OpenParen)) {
            while (!check(TokenKind::CloseParen) && !is_at_end()) {
                TypeNode* payload = parse_type();
                if (payload == nullptr) break;
                variant.m_payload.push_back(payload);
                if (!match(TokenKind::Comma)) break;
            }
            consume(TokenKind::CloseParen, "Expected ')' after variant payload types");
        }
        variant.m_span.m_end = previous_end();
        en->m_variants.push_back(std::move(variant));
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::CloseBrace, "Expected '}' to close enum");
    return en;
}

VarDeclNode* Parser::parse_var_decl() {
    Token keyword = advance();
    BindingKind kind = BindingKind::Mutable;
    if (keyword.m_kind == TokenKind::KwConst) kind = BindingKind::Immutable;
    if (keyword.m_kind == TokenKind::KwConstx) kind = BindingKind::Constx;

    auto* var = m_arena.create<VarDeclNode>();
    var->m_kind = kind;
    Token name = consume(TokenKind::Identifier, "Expected variable name");
    if (name.m_kind == TokenKind::Invalid) {
        synchronize_statement();
        return nullptr;
    }
    var->m_name = name.m_symbol_id;
    if (match(TokenKind::Colon)) var->m_type = parse_type();
    if (match(TokenKind::Equal)) var->m_initializer = parse_expression();

    if (kind != BindingKind::Mutable && var->m_initializer == nullptr) {
        error(name.m_span, "'" + std::string(keyword.m_raw_lexeme) + "' binding '" +
              std::string(name.m_raw_lexeme) + "' requires an initializer");
    }
    if (var->m_type == nullptr && var->m_initializer == nullptr) {
        error(name.m_span, "variable '" + std::string(name.m_raw_lexeme) +
              "' needs a type annotation or an initializer");
    }
    consume(TokenKind::Semicolon, "Expected ';' after variable declaration");
    var->m_module = m_current_section;
    return var;
}

// ---------------------------------------------------------------------------------------------
// Statements
// ---------------------------------------------------------------------------------------------

void Parser::synchronize_statement() {
    while (!is_at_end()) {
        if (check(TokenKind::Semicolon)) {
            advance();
            return;
        }
        if (check(TokenKind::CloseBrace)) return;
        switch (current().m_kind) {
            case TokenKind::KwReturn:
            case TokenKind::KwVar:
            case TokenKind::KwConst:
            case TokenKind::KwConstx:
            case TokenKind::KwIf:
            case TokenKind::KwWhile:
            case TokenKind::KwFor:
            case TokenKind::KwBreak:
            case TokenKind::KwContinue:
            case TokenKind::KwFn:
                return;
            default:
                advance();
        }
    }
}

BlockStmt* Parser::parse_block() {
    SourceLocation start = current().m_span.m_start;
    if (!check(TokenKind::OpenBrace)) {
        error_here("Expected '{', found '" + describe(current()) + "'");
        return nullptr;
    }
    advance();
    auto* block = m_arena.create<BlockStmt>();
    bool saved_no_struct = m_no_struct_literal;
    bool saved_angle = m_in_angle_list;
    m_no_struct_literal = false;
    m_in_angle_list = false;
    while (!check(TokenKind::CloseBrace) && !is_at_end()) {
        size_t before = m_cursor;
        if (StmtNode* stmt = parse_statement()) block->m_statements.push_back(stmt);
        if (m_cursor == before) advance();
    }
    m_no_struct_literal = saved_no_struct;
    m_in_angle_list = saved_angle;
    consume(TokenKind::CloseBrace, "Expected '}' to close block");
    block->m_span = span_from(start);
    return block;
}

StmtNode* Parser::parse_if() {
    SourceLocation start = current().m_span.m_start;
    advance(); // 'if'
    auto* stmt = m_arena.create<IfStmt>();

    bool saved = m_no_struct_literal;
    m_no_struct_literal = true;
    stmt->m_condition = parse_expression();
    m_no_struct_literal = saved;
    if (stmt->m_condition == nullptr) {
        synchronize_statement();
        return nullptr;
    }

    stmt->m_then = parse_block();
    if (stmt->m_then == nullptr) return nullptr;

    if (match(TokenKind::KwElse)) {
        stmt->m_else = check(TokenKind::KwIf) ? parse_if() : parse_block();
    }
    stmt->m_span = span_from(start);
    return stmt;
}

StmtNode* Parser::parse_while() {
    SourceLocation start = current().m_span.m_start;
    advance(); // 'while'
    auto* stmt = m_arena.create<WhileStmt>();

    bool saved = m_no_struct_literal;
    m_no_struct_literal = true;
    stmt->m_condition = parse_expression();
    m_no_struct_literal = saved;
    if (stmt->m_condition == nullptr) {
        synchronize_statement();
        return nullptr;
    }
    stmt->m_body = parse_block();
    if (stmt->m_body == nullptr) return nullptr;
    stmt->m_span = span_from(start);
    return stmt;
}

StmtNode* Parser::parse_for() {
    SourceLocation start = current().m_span.m_start;
    advance(); // 'for'
    auto* stmt = m_arena.create<ForStmt>();

    Token var = consume(TokenKind::Identifier, "Expected loop variable name after 'for'");
    if (var.m_kind == TokenKind::Invalid) {
        synchronize_statement();
        return nullptr;
    }
    stmt->m_variable = var.m_symbol_id;
    consume(TokenKind::KwIn, "Expected 'in' after loop variable");

    bool saved = m_no_struct_literal;
    m_no_struct_literal = true;
    stmt->m_iterable = parse_expression();
    m_no_struct_literal = saved;
    if (stmt->m_iterable == nullptr) {
        synchronize_statement();
        return nullptr;
    }
    stmt->m_body = parse_block();
    if (stmt->m_body == nullptr) return nullptr;
    stmt->m_span = span_from(start);
    return stmt;
}

/// Function-body form: `@if cond { ... } @elif cond { ... } @else { ... }`
StmtNode* Parser::parse_conditional_stmt() {
    SourceLocation start = current().m_span.m_start;
    advance(); // '@'
    advance(); // 'if'
    auto* stmt = m_arena.create<ConditionalStmt>();

    bool is_else = false;
    while (true) {
        ConditionalBranch branch;
        if (!is_else) {
            bool saved = m_no_struct_literal;
            m_no_struct_literal = true;
            branch.m_condition = parse_expression();
            m_no_struct_literal = saved;
            if (branch.m_condition == nullptr) {
                synchronize_statement();
                return nullptr;
            }
        }
        BlockStmt* body = parse_block();
        if (body == nullptr) return nullptr;
        branch.m_body = std::move(body->m_statements);
        stmt->m_branches.push_back(std::move(branch));

        if (is_else) break;
        if (check(TokenKind::At) && peek().m_kind == TokenKind::Identifier && peek().m_raw_lexeme == "elif") {
            advance();
            advance();
        } else if (check(TokenKind::At) && peek().m_kind == TokenKind::KwElse) {
            advance();
            advance();
            is_else = true;
        } else {
            break;
        }
    }
    stmt->m_span = span_from(start);
    return stmt;
}

StmtNode* Parser::parse_statement() {
    SourceLocation start = current().m_span.m_start;
    switch (current().m_kind) {
        case TokenKind::KwReturn: {
            advance();
            auto* ret = m_arena.create<ReturnStmt>();
            if (!check(TokenKind::Semicolon)) {
                ret->m_value = parse_expression();
                if (ret->m_value == nullptr) {
                    synchronize_statement();
                    return nullptr;
                }
            }
            consume(TokenKind::Semicolon, "Expected ';' after return statement");
            ret->m_span = span_from(start);
            return ret;
        }
        case TokenKind::KwVar:
        case TokenKind::KwConst:
        case TokenKind::KwConstx: {
            VarDeclNode* var = parse_var_decl();
            if (var == nullptr) return nullptr;
            var->m_span = span_from(start);
            auto* decl_stmt = m_arena.create<DeclStmt>(var);
            decl_stmt->m_span = var->m_span;
            return decl_stmt;
        }
        case TokenKind::KwIf:
            return parse_if();
        case TokenKind::KwWhile:
            return parse_while();
        case TokenKind::KwFor:
            return parse_for();
        case TokenKind::KwBreak:
        case TokenKind::KwContinue: {
            bool is_break = (advance().m_kind == TokenKind::KwBreak);
            consume(TokenKind::Semicolon, "Expected ';' after loop control statement");
            StmtNode* stmt = is_break ? static_cast<StmtNode*>(m_arena.create<BreakStmt>())
                                      : static_cast<StmtNode*>(m_arena.create<ContinueStmt>());
            stmt->m_span = span_from(start);
            return stmt;
        }
        case TokenKind::OpenBrace:
            return parse_block();
        case TokenKind::At: {
            const Token& next = peek();
            if (next.m_kind == TokenKind::KwIf) return parse_conditional_stmt();
            if (next.m_kind == TokenKind::KwElse ||
                (next.m_kind == TokenKind::Identifier && next.m_raw_lexeme == "elif")) {
                error_here("'@" + std::string(next.m_raw_lexeme) + "' without a preceding '@if'");
                synchronize_statement();
                return nullptr;
            }
            break; // intrinsic call used as an expression statement
        }
        default:
            break;
    }

    ExprNode* expr = parse_expression();
    if (expr == nullptr) {
        synchronize_statement();
        return nullptr;
    }
    consume(TokenKind::Semicolon, "Expected ';' after expression");
    auto* stmt = m_arena.create<ExprStmt>(expr);
    stmt->m_span = span_from(start);
    return stmt;
}

} // namespace fool
