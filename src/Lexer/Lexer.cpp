//
// Created by jmbab on 05-11-2026.
//
/**
 * @file Lexer.cpp
 * @brief Implementation unit of the Fool lexical scanner.
 */

module;

#include <cctype>
#include <string_view>

module fool.lexer;

namespace fool {

const char* token_kind_to_str(TokenKind kind) {
    switch (kind) {
        case TokenKind::Eof: return "Eof";
        case TokenKind::Identifier: return "Identifier";
        case TokenKind::IntegerLiteral: return "IntegerLiteral";
        case TokenKind::FloatLiteral: return "FloatLiteral";
        case TokenKind::StringLiteral: return "StringLiteral";
        case TokenKind::KwImport: return "import";
        case TokenKind::KwModule: return "module";
        case TokenKind::KwClass: return "class";
        case TokenKind::KwRuleset: return "ruleset";
        case TokenKind::KwBlueprint: return "blueprint";
        case TokenKind::KwEnum: return "enum";
        case TokenKind::KwFn: return "fn";
        case TokenKind::KwAsync: return "async";
        case TokenKind::KwAwait: return "await";
        case TokenKind::KwThis: return "this";
        case TokenKind::KwVar: return "var";
        case TokenKind::KwConst: return "const";
        case TokenKind::KwConstx: return "constx";
        case TokenKind::KwRef: return "ref";
        case TokenKind::KwReturn: return "return";
        case TokenKind::KwIf: return "if";
        case TokenKind::KwElse: return "else";
        case TokenKind::KwFor: return "for";
        case TokenKind::KwIn: return "in";
        case TokenKind::KwTrue: return "true";
        case TokenKind::KwFalse: return "false";
        case TokenKind::Hash: return "#";
        case TokenKind::HashImport: return "#import";
        case TokenKind::At: return "@";
        case TokenKind::OpenParen: return "(";
        case TokenKind::CloseParen: return ")";
        case TokenKind::OpenBrace: return "{";
        case TokenKind::CloseBrace: return "}";
        case TokenKind::OpenBracket: return "[";
        case TokenKind::CloseBracket: return "]";
        case TokenKind::Semicolon: return ";";
        case TokenKind::Colon: return ":";
        case TokenKind::DoubleColon: return "::";
        case TokenKind::Comma: return ",";
        case TokenKind::Dot: return ".";
        case TokenKind::DotDot: return "..";
        case TokenKind::Arrow: return "->";
        case TokenKind::Equal: return "=";
        case TokenKind::EqualEqual: return "==";
        case TokenKind::ExclaimEqual: return "!=";
        case TokenKind::Less: return "<";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEqual: return ">=";
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Ampersand: return "&";
        default: return "Invalid";
    }
}

Lexer::Lexer(std::string_view source, std::string_view filename)
    : m_source(source), m_filename(filename) {}

char Lexer::peek(size_t ahead) const {
    if (m_cursor + ahead >= m_source.size()) return '\0';
    return m_source[m_cursor + ahead];
}

char Lexer::advance() {
    if (m_cursor >= m_source.size()) return '\0';
    char current = m_source[m_cursor++];
    m_loc.m_offset++;
    if (current == '\n') {
        m_loc.m_line++;
        m_loc.m_column = 1;
    } else {
        m_loc.m_column++;
    }
    return current;
}

bool Lexer::match(char expected) {
    if (peek() == expected) {
        advance();
        return true;
    }
    return false;
}

void Lexer::skip_whitespace_and_comments() {
    while (m_cursor < m_source.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peek(1) == '/') {
            while (peek() != '\0' && peek() != '\n') {
                advance();
            }
        } else {
            break;
        }
    }
}

Token Lexer::make_token(TokenKind kind, SourceLocation start) {
    size_t length = m_loc.m_offset - start.m_offset;
    std::string_view lexeme = m_source.substr(start.m_offset, length);
    return Token{kind, INVALID_SYMBOL_ID, lexeme, SourceSpan{start, m_loc}};
}

TokenKind Lexer::classify_keyword(std::string_view text) {
    if (text == "import")    return TokenKind::KwImport;
    if (text == "module")    return TokenKind::KwModule;
    if (text == "class")     return TokenKind::KwClass;
    if (text == "ruleset")   return TokenKind::KwRuleset;
    if (text == "blueprint") return TokenKind::KwBlueprint;
    if (text == "enum")      return TokenKind::KwEnum;
    if (text == "fn")        return TokenKind::KwFn;
    if (text == "async")     return TokenKind::KwAsync;
    if (text == "await")     return TokenKind::KwAwait;
    if (text == "this")      return TokenKind::KwThis;
    if (text == "var")       return TokenKind::KwVar;
    if (text == "const")     return TokenKind::KwConst;
    if (text == "constx")    return TokenKind::KwConstx;
    if (text == "ref")       return TokenKind::KwRef;
    if (text == "return")    return TokenKind::KwReturn;
    if (text == "if")        return TokenKind::KwIf;
    if (text == "else")      return TokenKind::KwElse;
    if (text == "for")       return TokenKind::KwFor;
    if (text == "in")        return TokenKind::KwIn;
    if (text == "true")      return TokenKind::KwTrue;
    if (text == "false")     return TokenKind::KwFalse;
    return TokenKind::Identifier;
}

Token Lexer::make_identifier_or_keyword(SourceLocation start) {
    while (std::isalnum(peek()) || peek() == '_') {
        advance();
    }
    size_t length = m_loc.m_offset - start.m_offset;
    std::string_view lexeme = m_source.substr(start.m_offset, length);
    TokenKind kind = classify_keyword(lexeme);
    SymbolID sym = StringInterner::get().intern(lexeme);
    return Token{kind, sym, lexeme, SourceSpan{start, m_loc}};
}

Token Lexer::make_directive(SourceLocation start) {
    while (std::isalpha(peek()) || peek() == '_') {
        advance();
    }
    size_t length = m_loc.m_offset - start.m_offset;
    std::string_view directive = m_source.substr(start.m_offset, length);
    if (directive == "#import") {
        return Token{TokenKind::HashImport, INVALID_SYMBOL_ID, directive, SourceSpan{start, m_loc}};
    }
    return Token{TokenKind::Hash, INVALID_SYMBOL_ID, directive, SourceSpan{start, m_loc}};
}

Token Lexer::make_number(SourceLocation start) {
    bool is_float = false;
    while (std::isdigit(peek())) advance();
    if (peek() == '.' && std::isdigit(peek(1))) {
        is_float = true;
        advance(); // consume '.'
        while (std::isdigit(peek())) advance();
    }
    return make_token(is_float ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, start);
}

Token Lexer::make_string(SourceLocation start) {
    while (peek() != '"' && peek() != '\0') {
        if (peek() == '\\') advance();
        advance();
    }
    if (peek() == '"') {
        advance();
        return make_token(TokenKind::StringLiteral, start);
    }
    return make_token(TokenKind::Invalid, start);
}

Token Lexer::next_token() {
    skip_whitespace_and_comments();
    SourceLocation start = m_loc;
    if (m_cursor >= m_source.size()) {
        return Token{TokenKind::Eof, INVALID_SYMBOL_ID, "", SourceSpan{start, start}};
    }

    char c = advance();
    if (c == '#') return make_directive(start);
    if (std::isalpha(c) || c == '_') return make_identifier_or_keyword(start);
    if (std::isdigit(c)) return make_number(start);

    switch (c) {
        case '@': return make_token(TokenKind::At, start);
        case '(': return make_token(TokenKind::OpenParen, start);
        case ')': return make_token(TokenKind::CloseParen, start);
        case '{': return make_token(TokenKind::OpenBrace, start);
        case '}': return make_token(TokenKind::CloseBrace, start);
        case '[': return make_token(TokenKind::OpenBracket, start);
        case ']': return make_token(TokenKind::CloseBracket, start);
        case ';': return make_token(TokenKind::Semicolon, start);
        case ',': return make_token(TokenKind::Comma, start);
        case '+': return make_token(TokenKind::Plus, start);
        case '*': return make_token(TokenKind::Star, start);
        case '/': return make_token(TokenKind::Slash, start);
        case '&': return make_token(TokenKind::Ampersand, start);
        case '"': return make_string(start);
        case ':': {
            if (match(':')) return make_token(TokenKind::DoubleColon, start);
            return make_token(TokenKind::Colon, start);
        }
        case '.': return make_token(match('.') ? TokenKind::DotDot : TokenKind::Dot, start);
        case '-': return make_token(match('>') ? TokenKind::Arrow : TokenKind::Minus, start);
        case '=': return make_token(match('=') ? TokenKind::EqualEqual : TokenKind::Equal, start);
        case '!': if (match('=')) return make_token(TokenKind::ExclaimEqual, start); break;
        case '<': return make_token(match('=') ? TokenKind::LessEqual : TokenKind::Less, start);
        case '>': return make_token(match('=') ? TokenKind::GreaterEqual : TokenKind::Greater, start);
        default: break;
    }
    return make_token(TokenKind::Invalid, start);
}

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = next_token();
        tokens.push_back(tok);
        if (tok.m_kind == TokenKind::Eof) break;
    }
    return tokens;
}

} // namespace fool
