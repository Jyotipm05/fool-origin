//
// Created by jmbab on 05-11-2026.
//

/**
 * @file Parser.cpp
 * @brief Parser core: token cursor, Pratt expression parser, types and generic argument lists.
 */

module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.parser;

namespace fool {

namespace {

/// Interns a lexeme and returns a view that outlives the source buffer.
std::string_view stable_text(std::string_view text) {
    auto& interner = StringInterner::get();
    return interner.resolve(interner.intern(text));
}

std::string strip_underscores(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Token cursor
// ---------------------------------------------------------------------------------------------

Parser::Parser(std::vector<Token> tokens, Arena& arena, DiagnosticEngine& diag)
    : m_tokens(std::move(tokens)), m_arena(arena), m_diag(diag) {
    if (m_tokens.empty() || m_tokens.back().m_kind != TokenKind::Eof) {
        m_tokens.emplace_back(TokenKind::Eof, INVALID_SYMBOL_ID, std::string_view{}, SourceSpan{});
    }
}

const Token& Parser::current() const {
    return m_tokens[m_cursor < m_tokens.size() ? m_cursor : m_tokens.size() - 1];
}

const Token& Parser::peek(size_t ahead) const {
    size_t idx = m_cursor + ahead;
    return m_tokens[idx < m_tokens.size() ? idx : m_tokens.size() - 1];
}

SourceLocation Parser::previous_end() const {
    if (m_cursor == 0) return current().m_span.m_start;
    return m_tokens[m_cursor - 1].m_span.m_end;
}

Token Parser::advance() {
    Token tok = current();
    if (!is_at_end()) ++m_cursor;
    return tok;
}

bool Parser::check(TokenKind k) const { return current().m_kind == k; }

bool Parser::check_identifier(std::string_view text) const {
    return current().m_kind == TokenKind::Identifier && current().m_raw_lexeme == text;
}

bool Parser::match(TokenKind k) {
    if (!check(k)) return false;
    advance();
    return true;
}

bool Parser::is_at_end() const { return current().m_kind == TokenKind::Eof; }

void Parser::error(const SourceSpan& span, std::string message) {
    if (m_speculation_depth > 0) {
        m_speculation_failed = true;
        return;
    }
    m_diag.error(span, std::move(message));
}

void Parser::error_here(std::string message) { error(current().m_span, std::move(message)); }

SourceSpan Parser::span_from(SourceLocation start) const { return SourceSpan{start, previous_end()}; }

void Parser::begin_speculation() {
    ++m_speculation_depth;
    m_speculation_failed = false;
}

bool Parser::end_speculation() {
    bool ok = !m_speculation_failed;
    --m_speculation_depth;
    m_speculation_failed = false;
    return ok;
}

Token Parser::consume(TokenKind k, std::string_view err_msg) {
    if (check(k)) return advance();
    error_here(std::string(err_msg) + ", found '" + std::string(current().m_raw_lexeme.empty()
              ? token_kind_to_str(current().m_kind) : current().m_raw_lexeme) + "'");
    return Token{TokenKind::Invalid, INVALID_SYMBOL_ID, std::string_view{}, current().m_span};
}

// ---------------------------------------------------------------------------------------------
// Expressions
// ---------------------------------------------------------------------------------------------

/// Binding power of binary operators; -1 for tokens that are not binary operators here.
int Parser::get_precedence(TokenKind k) {
    switch (k) {
        case TokenKind::Equal:         return 1;  // assignment (right associative)
        case TokenKind::DotDot:        return 2;  // range
        case TokenKind::PipePipe:      return 3;
        case TokenKind::AmpAmp:        return 4;
        case TokenKind::EqualEqual:
        case TokenKind::ExclaimEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual:  return 5;
        case TokenKind::Pipe:          return 6;
        case TokenKind::Caret:         return 7;
        case TokenKind::Ampersand:     return 8;
        case TokenKind::Plus:
        case TokenKind::Minus:         return 10;
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:       return 20;
        case TokenKind::KwAs:          return 25; // handled specially: right-hand side is a type
        default:                       return -1;
    }
}

ExprNode* Parser::parse_expression(int min_precedence) {
    SourceLocation start = current().m_span.m_start;
    ExprNode* lhs = parse_unary();
    if (lhs == nullptr) return nullptr;

    while (true) {
        TokenKind op = current().m_kind;
        if (op == TokenKind::Greater && m_in_angle_list) break; // '>' closes a generic list
        int prec = get_precedence(op);
        if (prec < 0 || prec <= min_precedence) break;
        advance();

        if (op == TokenKind::KwAs) {
            TypeNode* target = parse_type(/*allow_generic_args=*/false);
            auto* cast = m_arena.create<CastExpr>(lhs, target);
            cast->m_span = span_from(start);
            lhs = cast;
            continue;
        }

        bool right_assoc = (op == TokenKind::Equal);
        ExprNode* rhs = parse_expression(right_assoc ? prec - 1 : prec);
        if (rhs == nullptr) return lhs;
        auto* bin = m_arena.create<BinaryExpr>(op, lhs, rhs);
        bin->m_span = span_from(start);
        lhs = bin;
    }
    return lhs;
}

ExprNode* Parser::parse_unary() {
    SourceLocation start = current().m_span.m_start;
    TokenKind k = current().m_kind;
    if (k == TokenKind::Bang || k == TokenKind::Minus || k == TokenKind::Ampersand ||
        k == TokenKind::Star || k == TokenKind::KwAwait) {
        advance();
        ExprNode* operand = parse_unary();
        if (operand == nullptr) return nullptr;
        auto* un = m_arena.create<UnaryExpr>(k, operand);
        un->m_span = span_from(start);
        return un;
    }
    ExprNode* primary = parse_primary();
    if (primary == nullptr) return nullptr;
    return parse_postfix(primary);
}

void Parser::parse_call_arguments(CallExpr* call) {
    bool saved_no_struct = m_no_struct_literal;
    bool saved_angle = m_in_angle_list;
    m_no_struct_literal = false;
    m_in_angle_list = false;
    while (!check(TokenKind::CloseParen) && !is_at_end()) {
        CallArg arg;
        arg.m_span.m_start = current().m_span.m_start;
        if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Colon) {
            arg.m_label = current().m_symbol_id;
            advance();
            advance();
        }
        arg.m_expr = parse_expression();
        arg.m_span.m_end = previous_end();
        if (arg.m_expr == nullptr) break;
        call->m_args.push_back(arg);
        if (!match(TokenKind::Comma)) break;
    }
    m_no_struct_literal = saved_no_struct;
    m_in_angle_list = saved_angle;
}

ExprNode* Parser::parse_postfix(ExprNode* base) {
    SourceLocation start = base->m_span.m_start;
    while (true) {
        if (check(TokenKind::Dot)) {
            advance();
            Token member = consume(TokenKind::Identifier, "Expected member name after '.'");
            if (member.m_kind == TokenKind::Invalid) return base;
            auto* mem = m_arena.create<MemberExpr>(base, member.m_symbol_id);
            mem->m_span = span_from(start);
            base = mem;
        } else if (check(TokenKind::OpenParen)) {
            advance();
            auto* call = m_arena.create<CallExpr>();
            call->m_callee = base;
            parse_call_arguments(call);
            consume(TokenKind::CloseParen, "Expected ')' after call arguments");
            call->m_span = span_from(start);
            base = call;
        } else if (check(TokenKind::OpenBracket)) {
            advance();
            bool saved_no_struct = m_no_struct_literal;
            bool saved_angle = m_in_angle_list;
            m_no_struct_literal = false;
            m_in_angle_list = false;
            ExprNode* index = parse_expression();
            m_no_struct_literal = saved_no_struct;
            m_in_angle_list = saved_angle;
            consume(TokenKind::CloseBracket, "Expected ']' after index expression");
            if (index == nullptr) return base;
            auto* idx = m_arena.create<IndexExpr>(base, index);
            idx->m_span = span_from(start);
            base = idx;
        } else {
            return base;
        }
    }
}

ExprNode* Parser::parse_number() {
    Token tok = advance();
    std::string text = strip_underscores(tok.m_raw_lexeme);
    SourceLocation start = tok.m_span.m_start;

    if (tok.m_kind == TokenKind::FloatLiteral) {
        auto* lit = m_arena.create<FloatLiteralExpr>(std::strtod(text.c_str(), nullptr));
        lit->m_span = span_from(start);
        return lit;
    }

    int base = 10;
    std::string digits = text;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        digits = text.substr(2);
    } else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        digits = text.substr(2);
    }
    errno = 0;
    unsigned long long value = std::strtoull(digits.c_str(), nullptr, base);
    if (errno == ERANGE) {
        error(tok.m_span, "integer literal '" + std::string(tok.m_raw_lexeme) + "' is out of range");
        value = 0;
    }
    auto* lit = m_arena.create<IntegerLiteralExpr>(static_cast<int64_t>(value));
    lit->m_span = span_from(start);
    return lit;
}

bool Parser::looks_like_struct_body() const {
    if (!check(TokenKind::OpenBrace)) return false;
    if (peek().m_kind == TokenKind::CloseBrace) return true;
    return peek().m_kind == TokenKind::Identifier && peek(2).m_kind == TokenKind::Colon;
}

ExprNode* Parser::parse_intrinsic_call() {
    SourceLocation start = current().m_span.m_start;
    advance(); // '@'
    Token name = consume(TokenKind::Identifier, "Expected intrinsic name after '@'");
    if (name.m_kind == TokenKind::Invalid) return nullptr;

    // Named casts: @static_cast<T>(expr), @dynamic_cast<T>(expr), @reinterpret_cast<T>(expr), @bit_cast<T>(expr).
    const std::string_view intrinsic_name = StringInterner::get().resolve(name.m_symbol_id);
    CastKind cast_kind = CastKind::Unchecked;
    if (intrinsic_name == "static_cast") cast_kind = CastKind::Static;
    else if (intrinsic_name == "dynamic_cast") cast_kind = CastKind::Dynamic;
    else if (intrinsic_name == "reinterpret_cast") cast_kind = CastKind::Reinterpret;
    else if (intrinsic_name == "bit_cast") cast_kind = CastKind::Bit;
    if (cast_kind != CastKind::Unchecked) return parse_named_cast(start, intrinsic_name, cast_kind);

    auto* call = m_arena.create<IntrinsicCallExpr>();
    call->m_name = name.m_symbol_id;
    consume(TokenKind::OpenParen, "Expected '(' after intrinsic name");
    bool saved_no_struct = m_no_struct_literal;
    bool saved_angle = m_in_angle_list;
    m_no_struct_literal = false;
    m_in_angle_list = false;
    while (!check(TokenKind::CloseParen) && !is_at_end()) {
        ExprNode* arg = parse_expression();
        if (arg == nullptr) break;
        call->m_args.push_back(arg);
        if (!match(TokenKind::Comma)) break;
    }
    m_no_struct_literal = saved_no_struct;
    m_in_angle_list = saved_angle;
    consume(TokenKind::CloseParen, "Expected ')' after intrinsic arguments");
    call->m_span = span_from(start);
    return call;
}

/// Parses the rest of a named cast after `@name`: `<Type>(expr)`.
ExprNode* Parser::parse_named_cast(SourceLocation start, std::string_view name, CastKind kind) {
    const std::string spelling = "@" + std::string(name);
    if (!check(TokenKind::Less)) {
        error_here("Expected '<' and a target type after '" + spelling + "', e.g. " + spelling + "<T>(value)");
        // Recover by parsing the parenthesized operand so that no follow-up errors cascade.
        if (!match(TokenKind::OpenParen)) return nullptr;
        ExprNode* operand = parse_expression();
        consume(TokenKind::CloseParen, "Expected ')' after the operand of '" + spelling + "'");
        return operand;
    }
    advance(); // '<'
    bool saved_angle = m_in_angle_list;
    bool saved_no_struct = m_no_struct_literal;
    m_in_angle_list = true;
    TypeNode* target = parse_type();
    m_in_angle_list = saved_angle;
    if (target == nullptr) return nullptr;
    consume(TokenKind::Greater, "Expected '>' after the target type of '" + spelling + "'");

    consume(TokenKind::OpenParen, "Expected '(' before the operand of '" + spelling + "'");
    m_no_struct_literal = false;
    m_in_angle_list = false;
    ExprNode* operand = parse_expression();
    m_no_struct_literal = saved_no_struct;
    m_in_angle_list = saved_angle;
    if (operand == nullptr) return nullptr;
    if (check(TokenKind::Comma)) {
        error_here("'" + spelling + "' takes exactly one operand");
        while (match(TokenKind::Comma)) {
            if (parse_expression() == nullptr) break;
        }
    }
    consume(TokenKind::CloseParen, "Expected ')' after the operand of '" + spelling + "'");

    auto* cast = m_arena.create<CastExpr>(operand, target, kind);
    cast->m_span = span_from(start);
    return cast;
}

ExprNode* Parser::parse_path_expression() {
    SourceLocation start = current().m_span.m_start;
    std::vector<SymbolID> segments;
    std::vector<GenericArg> generic_args;
    segments.push_back(advance().m_symbol_id);

    // `Atomic<u32>::new(...)` / `Mutex<T> { ... }`: a '<' only opens generic arguments when the
    // matching '>' is followed by '::' or a struct body; otherwise it is the less-than operator.
    if (check(TokenKind::Less)) {
        size_t save = m_cursor;
        begin_speculation();
        std::vector<GenericArg> args = parse_generic_args();
        bool ok = end_speculation();
        ok = ok && (check(TokenKind::DoubleColon) ||
                    (!m_no_struct_literal && looks_like_struct_body()));
        if (ok) {
            generic_args = std::move(args);
        } else {
            m_cursor = save;
        }
    }

    while (check(TokenKind::DoubleColon) && peek().m_kind == TokenKind::Identifier) {
        advance();
        segments.push_back(advance().m_symbol_id);
    }

    if (!m_no_struct_literal && looks_like_struct_body()) {
        auto* type = m_arena.create<TypeNode>(TypeNodeKind::Path);
        type->m_path = segments;
        type->m_generic_args = generic_args;
        type->m_span = span_from(start);

        auto* lit = m_arena.create<StructLiteralExpr>();
        lit->m_type = type;
        advance(); // '{'
        bool saved_angle = m_in_angle_list;
        m_in_angle_list = false;
        while (!check(TokenKind::CloseBrace) && !is_at_end()) {
            FieldInit field;
            field.m_span.m_start = current().m_span.m_start;
            Token name = consume(TokenKind::Identifier, "Expected field name in struct literal");
            if (name.m_kind == TokenKind::Invalid) break;
            field.m_name = name.m_symbol_id;
            consume(TokenKind::Colon, "Expected ':' after field name");
            field.m_value = parse_expression();
            field.m_span.m_end = previous_end();
            if (field.m_value == nullptr) break;
            lit->m_fields.push_back(field);
            if (!match(TokenKind::Comma)) break;
        }
        m_in_angle_list = saved_angle;
        consume(TokenKind::CloseBrace, "Expected '}' after struct literal");
        lit->m_span = span_from(start);
        return lit;
    }

    if (segments.size() == 1 && generic_args.empty()) {
        auto* id = m_arena.create<IdentifierExpr>(segments.front());
        id->m_span = span_from(start);
        return id;
    }
    auto* path = m_arena.create<PathExpr>();
    path->m_segments = std::move(segments);
    path->m_generic_args = std::move(generic_args);
    path->m_span = span_from(start);
    return path;
}

ExprNode* Parser::parse_primary() {
    SourceLocation start = current().m_span.m_start;
    switch (current().m_kind) {
        case TokenKind::IntegerLiteral:
        case TokenKind::FloatLiteral:
            return parse_number();
        case TokenKind::StringLiteral: {
            Token tok = advance();
            auto* lit = m_arena.create<StringLiteralExpr>(stable_text(tok.m_raw_lexeme));
            lit->m_span = span_from(start);
            return lit;
        }
        case TokenKind::KwTrue:
        case TokenKind::KwFalse: {
            bool value = (advance().m_kind == TokenKind::KwTrue);
            auto* lit = m_arena.create<BooleanLiteralExpr>(value);
            lit->m_span = span_from(start);
            return lit;
        }
        case TokenKind::Identifier:
            return parse_path_expression();
        case TokenKind::At:
            return parse_intrinsic_call();
        case TokenKind::OpenParen: {
            advance();
            bool saved_no_struct = m_no_struct_literal;
            bool saved_angle = m_in_angle_list;
            m_no_struct_literal = false;
            m_in_angle_list = false;
            ExprNode* inner = parse_expression();
            m_no_struct_literal = saved_no_struct;
            m_in_angle_list = saved_angle;
            consume(TokenKind::CloseParen, "Expected ')' to close parenthesized expression");
            return inner;
        }
        default:
            error_here("Expected expression, found '" + std::string(current().m_raw_lexeme.empty()
                       ? token_kind_to_str(current().m_kind) : current().m_raw_lexeme) + "'");
            return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// Types and generics
// ---------------------------------------------------------------------------------------------

TypeNode* Parser::parse_type(bool allow_generic_args) {
    SourceLocation start = current().m_span.m_start;

    if (match(TokenKind::Bang)) {
        auto* never = m_arena.create<TypeNode>(TypeNodeKind::Never);
        never->m_span = span_from(start);
        return never;
    }

    bool is_const = false;
    if (check(TokenKind::KwConst) &&
        (peek().m_kind == TokenKind::Star || peek().m_kind == TokenKind::Ampersand)) {
        advance();
        is_const = true;
    }
    if (check(TokenKind::Star) || check(TokenKind::Ampersand)) {
        TypeNodeKind kind = check(TokenKind::Star) ? TypeNodeKind::Pointer : TypeNodeKind::Reference;
        advance();
        auto* view = m_arena.create<TypeNode>(kind);
        view->m_is_const = is_const;
        view->m_inner = parse_type(allow_generic_args);
        view->m_span = span_from(start);
        return view->m_inner ? view : nullptr;
    }

    if (!check(TokenKind::Identifier)) {
        error_here("Expected type, found '" + std::string(current().m_raw_lexeme.empty()
                   ? token_kind_to_str(current().m_kind) : current().m_raw_lexeme) + "'");
        return nullptr;
    }

    auto* path = m_arena.create<TypeNode>(TypeNodeKind::Path);
    path->m_path.push_back(advance().m_symbol_id);
    while (check(TokenKind::DoubleColon) && peek().m_kind == TokenKind::Identifier) {
        advance();
        path->m_path.push_back(advance().m_symbol_id);
    }
    if (allow_generic_args && check(TokenKind::Less)) {
        path->m_generic_args = parse_generic_args();
    }
    path->m_span = span_from(start);
    return path;
}

/// Expression inside '<...>'. A bare '>' terminates it instead of acting as the greater-than operator.
ExprNode* Parser::parse_angle_expression() {
    bool saved_angle = m_in_angle_list;
    bool saved_no_struct = m_no_struct_literal;
    m_in_angle_list = true;
    m_no_struct_literal = true;
    ExprNode* value = parse_expression();
    m_in_angle_list = saved_angle;
    m_no_struct_literal = saved_no_struct;
    return value;
}

std::vector<GenericArg> Parser::parse_generic_args() {
    std::vector<GenericArg> args;
    consume(TokenKind::Less, "Expected '<'");
    while (!check(TokenKind::Greater) && !is_at_end()) {
        GenericArg arg;
        if (check(TokenKind::Identifier) && peek().m_kind == TokenKind::Equal) {
            arg.m_name = advance().m_symbol_id;
            advance(); // '='
            arg.m_value = parse_angle_expression();
            if (arg.m_value == nullptr) break;
        } else {
            arg.m_type = parse_type();
            if (arg.m_type == nullptr) break;
        }
        args.push_back(arg);
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::Greater, "Expected '>' to close generic argument list");
    return args;
}

std::vector<GenericParam> Parser::parse_generic_params() {
    std::vector<GenericParam> params;
    consume(TokenKind::Less, "Expected '<'");
    while (!check(TokenKind::Greater) && !is_at_end()) {
        GenericParam param;
        Token name = consume(TokenKind::Identifier, "Expected generic parameter name");
        if (name.m_kind == TokenKind::Invalid) break;
        param.m_name = name.m_symbol_id;
        if (match(TokenKind::Colon)) {
            param.m_constraint = parse_type();
            if (param.m_constraint == nullptr) break;
        }
        if (match(TokenKind::Equal)) {
            param.m_default = parse_angle_expression();
            if (param.m_default == nullptr) break;
        }
        params.push_back(param);
        if (!match(TokenKind::Comma)) break;
    }
    consume(TokenKind::Greater, "Expected '>' to close generic parameter list");
    return params;
}

} // namespace fool
