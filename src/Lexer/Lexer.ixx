//
// Created by jmbab on 05-11-2026.
//

/**
 * @file Lexer.ixx
 * @brief Module interface for the Fool lexical analyzer.
 */

module;

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

export module fool.lexer;

import fool.common;
import fool.diagnostics;

export namespace fool {

/**
 * @enum TokenKind
 * @brief Complete set of lexical tokens for the Fool programming language.
 */
enum class TokenKind : uint16_t {
    Eof,
    Invalid,

    // Identifiers & Literals
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,

    // Keywords
    KwImport,        ///< 'import'
    KwModule,        ///< 'module'
    KwClass,         ///< 'class'
    KwRuleset,       ///< 'ruleset'
    KwBlueprint,     ///< 'blueprint'
    KwEnum,          ///< 'enum'
    KwFn,            ///< 'fn'
    KwAsync,         ///< 'async'
    KwAwait,         ///< 'await'
    KwThis,          ///< 'this' explicit receiver parameter
    KwVar,           ///< 'var'
    KwConst,         ///< 'const' Rust-like immutability
    KwConstx,        ///< 'constx' C++ constexpr / Rust const
    KwRef,           ///< 'ref'
    KwReturn,        ///< 'return'
    KwIf,            ///< 'if'
    KwElse,          ///< 'else'
    KwFor,           ///< 'for'
    KwIn,            ///< 'in'
    KwTrue,          ///< 'true'
    KwFalse,         ///< 'false'
    KwAs,            ///< 'as' explicit cast
    KwWhile,         ///< 'while'
    KwStruct,        ///< 'struct' plain data aggregate
    KwBreak,         ///< 'break'
    KwContinue,      ///< 'continue'

    // Directives & Punctuation
    Hash,            ///< '#' directive prefix
    HashImport,      ///< '#import' module import directive
    At,              ///< '@' compile-time attribute directive marker
    OpenParen,       ///< '('
    CloseParen,      ///< ')'
    OpenBrace,       ///< '{'
    CloseBrace,      ///< '}'
    OpenBracket,     ///< '['
    CloseBracket,    ///< ']'
    Semicolon,       ///< ';'
    Colon,           ///< ':'
    DoubleColon,     ///< '::'
    Comma,           ///< ','
    Dot,             ///< '.'
    DotDot,          ///< '..'
    Arrow,           ///< '->'
    Equal,           ///< '=' mutable assignment/declaration
    EqualEqual,      ///< '=='
    ExclaimEqual,    ///< '!='
    Less,            ///< '<'
    LessEqual,       ///< '<='
    Greater,         ///< '>'
    GreaterEqual,    ///< '>='
    Plus,            ///< '+'
    Minus,           ///< '-'
    Star,            ///< '*'
    Slash,           ///< '/'
    Ampersand,       ///< '&'
    AmpAmp,          ///< '&&'
    Pipe,            ///< '|'
    PipePipe,        ///< '||'
    Caret,           ///< '^'
    Percent,         ///< '%'
    Bang,            ///< '!' logical not / never type
};

/**
 * @struct Token
 * @brief Represents a scanned token, carrying an interned symbol handle and source coordinates.
 */
struct Token {
    // 1. Member variables FIRST on top with 'm_' prefix
    TokenKind m_kind{TokenKind::Invalid};
    SymbolID m_symbol_id{INVALID_SYMBOL_ID};
    std::string_view m_raw_lexeme;
    SourceSpan m_span;

    // 2. Constructors in the MIDDLE
    Token() = default;
    Token(TokenKind kind, SymbolID symbol_id, std::string_view raw_lexeme, SourceSpan span)
        : m_kind(kind), m_symbol_id(symbol_id), m_raw_lexeme(raw_lexeme), m_span(span) {}

    // 3. Member functions LAST at the bottom
    [[nodiscard]] TokenKind kind() const { return m_kind; }
    [[nodiscard]] SymbolID symbol_id() const { return m_symbol_id; }
    [[nodiscard]] std::string_view raw_lexeme() const { return m_raw_lexeme; }
    [[nodiscard]] const SourceSpan& span() const { return m_span; }
    [[nodiscard]] bool is(TokenKind k) const { return m_kind == k; }
    [[nodiscard]] bool is_not(TokenKind k) const { return m_kind != k; }
};

const char* token_kind_to_str(TokenKind kind);

/**
 * @class Lexer
 * @brief High-speed zero-copy lexical scanner operating over contiguous UTF-8 character views.
 */
class Lexer {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::string_view m_source;
    std::string_view m_filename;
    size_t m_cursor{0};
    SourceLocation m_loc{1, 1, 0};
    DiagnosticEngine& m_diag;

public:
    // 2. Constructors and Destructors in the MIDDLE
    Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag);
    ~Lexer() = default;

    // 3. Member functions LAST at the bottom
    Token next_token();
    std::vector<Token> tokenize_all();

private:
    char peek(size_t ahead = 0) const;
    char advance();
    bool match(char expected);
    void skip_whitespace_and_comments();
    void skip_block_comment();

    Token make_token(TokenKind kind, SourceLocation start);
    Token make_identifier_or_keyword(SourceLocation start);
    Token make_directive(SourceLocation start);
    Token make_number(SourceLocation start);
    Token make_string(SourceLocation start);
    Token make_invalid(SourceLocation start, const char* message);

    static TokenKind classify_keyword(std::string_view text);
};

} // namespace fool
