//
// Created for patch 11: `fool.toml` / `overwrite.toml` support.
//

/**
 * @file Toml.cpp
 * @brief The TOML reader behind `fool.toml` and `overwrite.toml` (see Toml.ixx for what it accepts).
 */

module;

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.toml;

namespace fool::toml {

const Value* Value::find(std::string_view key) const {
    if (m_kind != Kind::Table) return nullptr;
    for (size_t i = 0; i < m_keys.size(); ++i) {
        if (m_keys[i] == key) return &m_items[i];
    }
    return nullptr;
}

const Value* Value::find_path(std::string_view path) const {
    const Value* current = this;
    while (current != nullptr && !path.empty()) {
        const size_t dot = path.find('.');
        current = current->find(path.substr(0, dot));
        if (dot == std::string_view::npos) break;
        path.remove_prefix(dot + 1);
    }
    return current;
}

const char* Value::describe(Kind kind) {
    switch (kind) {
        case Kind::String: return "a string";
        case Kind::Integer: return "an integer";
        case Kind::Float: return "a number";
        case Kind::Boolean: return "a boolean";
        case Kind::Array: return "an array";
        case Kind::Table: return "a table";
    }
    return "a value";
}

void Value::set_origin(uint8_t origin) {
    m_origin = origin;
    for (Value& item : m_items) item.set_origin(origin);
}

void merge(Value& base, const Value& overlay) {
    if (base.m_kind != Kind::Table || overlay.m_kind != Kind::Table) {
        base = overlay;
        return;
    }
    for (size_t i = 0; i < overlay.m_keys.size(); ++i) {
        bool merged = false;
        for (size_t j = 0; j < base.m_keys.size(); ++j) {
            if (base.m_keys[j] != overlay.m_keys[i]) continue;
            merge(base.m_items[j], overlay.m_items[i]);
            merged = true;
            break;
        }
        if (!merged) {
            base.m_keys.push_back(overlay.m_keys[i]);
            base.m_items.push_back(overlay.m_items[i]);
        }
    }
}

namespace {

void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

bool is_bare_key_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

/// Reads one document. Every `parse_*` returns false after recording the first error.
class Reader {
    std::string_view m_text;
    size_t m_pos{0};
    ParseResult m_result;
    std::set<std::string> m_defined;   ///< tables named by a `[header]`, to catch repeats
    Value* m_table{nullptr};           ///< where `key = value` lines go

public:
    explicit Reader(std::string_view text) : m_text(text) { m_table = &m_result.m_root; }

    ParseResult run() {
        while (m_result.m_ok && !at_end()) {
            skip_blanks();
            if (at_end()) break;
            const char c = peek();
            if (c == '\n' || c == '\r') {
                ++m_pos;
            } else if (c == '#') {
                skip_comment();
            } else if (c == '[') {
                parse_header();
            } else {
                parse_key_value();
            }
        }
        return std::move(m_result);
    }

private:
    [[nodiscard]] bool at_end() const { return m_pos >= m_text.size(); }
    [[nodiscard]] char peek(size_t ahead = 0) const { return m_pos + ahead < m_text.size() ? m_text[m_pos + ahead] : '\0'; }

    void position(size_t pos, uint32_t& line, uint32_t& column) const {
        line = 1;
        column = 1;
        for (size_t i = 0; i < pos && i < m_text.size(); ++i) {
            if (m_text[i] == '\n') { ++line; column = 1; } else { ++column; }
        }
    }

    bool fail(size_t pos, std::string message) {
        if (!m_result.m_ok) return false;
        m_result.m_ok = false;
        m_result.m_message = std::move(message);
        position(pos, m_result.m_line, m_result.m_column);
        return false;
    }

    void locate(Value& value, size_t pos) const { position(pos, value.m_line, value.m_column); }

    void skip_blanks() {
        while (peek() == ' ' || peek() == '\t') ++m_pos;
    }

    void skip_comment() {
        while (!at_end() && peek() != '\n') ++m_pos;
    }

    /// Whitespace, comments and line breaks (between array elements).
    void skip_space_and_lines() {
        for (;;) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++m_pos;
            else if (c == '#') skip_comment();
            else return;
        }
    }

    bool end_of_line() {
        skip_blanks();
        if (peek() == '#') skip_comment();
        if (at_end()) return true;
        if (peek() == '\r') ++m_pos;
        if (peek() == '\n') { ++m_pos; return true; }
        return fail(m_pos, "expected the end of the line");
    }

    bool parse_basic_string(std::string& out) {
        const size_t start = m_pos;
        ++m_pos;   // opening quote
        for (;;) {
            if (at_end() || peek() == '\n') return fail(start, "unterminated string");
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            const char e = m_pos < m_text.size() ? m_text[m_pos++] : '\0';
            switch (e) {
                case 'b': out += '\b'; break;
                case 't': out += '\t'; break;
                case 'n': out += '\n'; break;
                case 'f': out += '\f'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case 'u':
                case 'U': {
                    const size_t digits = e == 'u' ? 4 : 8;
                    uint32_t code = 0;
                    for (size_t i = 0; i < digits; ++i) {
                        const char h = peek();
                        uint32_t v;
                        if (h >= '0' && h <= '9') v = static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') v = static_cast<uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = static_cast<uint32_t>(h - 'A' + 10);
                        else return fail(m_pos, "invalid unicode escape");
                        code = code * 16 + v;
                        ++m_pos;
                    }
                    if (code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) return fail(m_pos, "invalid unicode escape");
                    append_utf8(out, code);
                    break;
                }
                default: return fail(m_pos - 1, "invalid escape sequence in string");
            }
        }
    }

    bool parse_literal_string(std::string& out) {
        const size_t start = m_pos;
        ++m_pos;
        while (!at_end() && peek() != '\'' && peek() != '\n') out += m_text[m_pos++];
        if (peek() != '\'') return fail(start, "unterminated string");
        ++m_pos;
        return true;
    }

    bool parse_key(std::string& out) {
        const char c = peek();
        if (c == '"') return parse_basic_string(out);
        if (c == '\'') return parse_literal_string(out);
        const size_t start = m_pos;
        while (is_bare_key_char(peek())) ++m_pos;
        if (m_pos == start) return fail(m_pos, "expected a key");
        out.assign(m_text.substr(start, m_pos - start));
        return true;
    }

    bool parse_key_path(std::vector<std::string>& keys) {
        for (;;) {
            skip_blanks();
            std::string key;
            if (!parse_key(key)) return false;
            keys.push_back(std::move(key));
            skip_blanks();
            if (peek() != '.') return true;
            ++m_pos;
        }
    }

    /// The table member `key` of `table`, created when missing; fails when `key` names something else.
    Value* descend(Value& table, const std::string& key, size_t pos) {
        for (size_t i = 0; i < table.m_keys.size(); ++i) {
            if (table.m_keys[i] != key) continue;
            if (!table.m_items[i].is_table()) {
                fail(pos, "'" + key + "' is " + Value::describe(table.m_items[i].m_kind) + ", not a table");
                return nullptr;
            }
            return &table.m_items[i];
        }
        table.m_keys.push_back(key);
        table.m_items.emplace_back(Kind::Table);
        locate(table.m_items.back(), pos);
        return &table.m_items.back();
    }

    void parse_header() {
        const size_t start = m_pos;
        ++m_pos;   // [
        if (peek() == '[') {
            fail(start, "arrays of tables ('[[name]]') are not supported");
            return;
        }
        std::vector<std::string> keys;
        if (!parse_key_path(keys)) return;
        skip_blanks();
        if (peek() != ']') {
            fail(m_pos, "expected ']' after the table name");
            return;
        }
        ++m_pos;
        Value* table = &m_result.m_root;
        std::string full;
        for (const std::string& key : keys) {
            table = descend(*table, key, start);
            if (table == nullptr) return;
            if (!full.empty()) full += '.';
            full += key;
        }
        if (!m_defined.insert(full).second) {
            fail(start, "table '" + full + "' is defined twice");
            return;
        }
        m_table = table;
        end_of_line();
    }

    /// Adds `value` under the dotted `keys` inside `table`.
    bool insert(Value& table, const std::vector<std::string>& keys, Value value, size_t pos) {
        Value* target = &table;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            target = descend(*target, keys[i], pos);
            if (target == nullptr) return false;
        }
        const std::string& last = keys.back();
        if (target->find(last) != nullptr) return fail(pos, "key '" + last + "' is defined twice");
        target->m_keys.push_back(last);
        target->m_items.push_back(std::move(value));
        return true;
    }

    bool parse_key_value() {
        const size_t start = m_pos;
        std::vector<std::string> keys;
        if (!parse_key_path(keys)) return false;
        skip_blanks();
        if (peek() != '=') return fail(m_pos, "expected '=' after the key");
        ++m_pos;
        skip_blanks();
        Value value;
        if (!parse_value(value)) return false;
        if (!insert(*m_table, keys, std::move(value), start)) return false;
        return end_of_line();
    }

    bool parse_number(Value& out) {
        const size_t start = m_pos;
        while (!at_end() && std::string_view(",]} \t\r\n#").find(peek()) == std::string_view::npos) ++m_pos;
        std::string token;
        for (char c : m_text.substr(start, m_pos - start)) {
            if (c != '_') token += c;
        }
        size_t digits = 0;
        size_t i = (!token.empty() && (token[0] == '+' || token[0] == '-')) ? 1 : 0;
        const size_t sign_end = i;
        while (i < token.size() && token[i] >= '0' && token[i] <= '9') { ++i; ++digits; }
        if (digits > 0 && i == token.size()) {
            errno = 0;
            out.m_kind = Kind::Integer;
            out.m_integer = std::strtoll(token.c_str(), nullptr, 10);
            if (errno != 0) return fail(start, "integer is out of range");
            return true;
        }
        if (digits > 0 && i < token.size() && (token[i] == '-' || token[i] == ':')) {
            return fail(start, "date-times are not supported");
        }
        if (token.find_first_of(".eE") != std::string::npos && sign_end < token.size()) {
            char* end = nullptr;
            const double number = std::strtod(token.c_str(), &end);
            if (end != nullptr && *end == '\0') {
                out.m_kind = Kind::Float;
                out.m_float = number;
                return true;
            }
        }
        return fail(start, "invalid value '" + std::string(m_text.substr(start, m_pos - start)) + "'");
    }

    bool parse_keyword(Value& out, std::string_view word, bool truth) {
        if (m_text.substr(m_pos, word.size()) != word || is_bare_key_char(peek(word.size()))) {
            return fail(m_pos, "expected a value");
        }
        m_pos += word.size();
        out.m_kind = Kind::Boolean;
        out.m_boolean = truth;
        return true;
    }

    bool parse_array(Value& out) {
        out.m_kind = Kind::Array;
        ++m_pos;   // [
        for (;;) {
            skip_space_and_lines();
            if (peek() == ']') { ++m_pos; return true; }
            Value item;
            if (!parse_value(item)) return false;
            out.m_items.push_back(std::move(item));
            skip_space_and_lines();
            if (peek() == ',') { ++m_pos; continue; }
            if (peek() == ']') { ++m_pos; return true; }
            return fail(m_pos, "expected ',' or ']' in the array");
        }
    }

    bool parse_inline_table(Value& out) {
        out.m_kind = Kind::Table;
        ++m_pos;   // {
        skip_blanks();
        if (peek() == '}') { ++m_pos; return true; }
        for (;;) {
            const size_t start = m_pos;
            std::vector<std::string> keys;
            if (!parse_key_path(keys)) return false;
            skip_blanks();
            if (peek() != '=') return fail(m_pos, "expected '=' after the key");
            ++m_pos;
            skip_blanks();
            Value value;
            if (!parse_value(value)) return false;
            if (!insert(out, keys, std::move(value), start)) return false;
            skip_blanks();
            if (peek() == ',') { ++m_pos; skip_blanks(); continue; }
            if (peek() == '}') { ++m_pos; return true; }
            return fail(m_pos, "expected ',' or '}' in the inline table");
        }
    }

    bool parse_value(Value& out) {
        locate(out, m_pos);
        const char c = peek();
        if (c == '"' || c == '\'') {
            if (peek(1) == c && peek(2) == c) return fail(m_pos, "multi-line strings are not supported");
            out.m_kind = Kind::String;
            return c == '"' ? parse_basic_string(out.m_string) : parse_literal_string(out.m_string);
        }
        if (c == '[') return parse_array(out);
        if (c == '{') return parse_inline_table(out);
        if (c == 't') return parse_keyword(out, "true", true);
        if (c == 'f') return parse_keyword(out, "false", false);
        if ((c >= '0' && c <= '9') || c == '+' || c == '-') return parse_number(out);
        return fail(m_pos, "expected a value");
    }
};

}  // namespace

ParseResult parse(std::string_view text) {
    // A leading byte-order mark is ignored.
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);
    return Reader(text).run();
}

}  // namespace fool::toml
