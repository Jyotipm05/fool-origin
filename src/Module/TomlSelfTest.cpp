//
// Created for patch 11: `fool.toml` / `overwrite.toml` support.
//

/**
 * @file TomlSelfTest.cpp
 * @brief Built-in checks for the TOML reader (`foolc --toml-selftest`).
 */

module;

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

module fool.toml;

namespace fool::toml {

namespace {

struct Checker {
    std::ostream& m_out;
    size_t m_checks{0};
    size_t m_failures{0};

    void expect(bool condition, const std::string& what) {
        ++m_checks;
        if (condition) return;
        ++m_failures;
        m_out << "toml selftest: FAILED " << what << "\n";
    }

    /// `text` must fail to parse with `fragment` in the message, on `line`.
    void rejects(std::string_view text, std::string_view fragment, uint32_t line) {
        const ParseResult result = parse(text);
        const std::string label = "rejecting '" + std::string(text.substr(0, 30)) + "'";
        expect(!result.m_ok, label + " (it parsed)");
        if (result.m_ok) return;
        expect(result.m_message.find(fragment) != std::string::npos,
               label + " with '" + std::string(fragment) + "' (got '" + result.m_message + "')");
        expect(result.m_line == line, label + " on line " + std::to_string(line) + " (got " + std::to_string(result.m_line) + ")");
    }
};

constexpr std::string_view kDocument = R"(# a comment
title = "x\ty\u00e9"          # trailing comment
literal = 'C:\path\n'
big = 1_000
negative = -5
ratio = 2.5e1
yes = true
no = false
list = [1, 2,
  3,   # trailing comma and comment
]
inline = { a = 1, b.c = "d" }
dotted.key.deep = 7
"quoted key" = "q"

[server]
host = "h"

[server.tls]
on = true
)";

}  // namespace

bool run_selftest(std::ostream& out) {
    Checker check{out};

    const ParseResult doc = parse(kDocument);
    check.expect(doc.m_ok, "the sample document parses (" + doc.m_message + ")");
    if (doc.m_ok) {
        const Value& root = doc.m_root;
        auto text = [&](const char* path) { const Value* v = root.find_path(path); return v != nullptr && v->is_string() ? v->m_string : std::string("<missing>"); };
        auto integer = [&](const char* path) { const Value* v = root.find_path(path); return v != nullptr && v->m_kind == Kind::Integer ? v->m_integer : -1; };
        check.expect(text("title") == "x\ty\xC3\xA9", "basic string escapes and \\u");
        check.expect(text("literal") == "C:\\path\\n", "literal strings keep backslashes");
        check.expect(integer("big") == 1000, "underscores in integers");
        check.expect(integer("negative") == -5, "negative integers");
        const Value* ratio = root.find("ratio");
        check.expect(ratio != nullptr && ratio->m_kind == Kind::Float && std::fabs(ratio->m_float - 25.0) < 1e-9, "floats");
        check.expect(root.find("yes") != nullptr && root.find("yes")->m_boolean, "true");
        check.expect(root.find("no") != nullptr && root.find("no")->m_kind == Kind::Boolean && !root.find("no")->m_boolean, "false");
        const Value* list = root.find("list");
        check.expect(list != nullptr && list->is_array() && list->m_items.size() == 3 && list->m_items[2].m_integer == 3,
                     "arrays across lines with a trailing comma");
        check.expect(integer("inline.a") == 1 && text("inline.b.c") == "d", "inline tables with dotted keys");
        check.expect(integer("dotted.key.deep") == 7, "dotted keys");
        check.expect(text("quoted key") == "q", "quoted keys");
        check.expect(text("server.host") == "h", "[table] headers");
        check.expect(root.find_path("server.tls.on") != nullptr && root.find_path("server.tls.on")->m_boolean, "[a.b] headers");
        const Value* host = root.find_path("server.host");
        check.expect(host != nullptr && host->m_line == 17 && host->m_column == 8, "values remember where they are");
        check.expect(root.find_path("server.nothing") == nullptr && root.find_path("nothing.at.all") == nullptr, "missing paths are null");
    }

    check.expect(parse("").m_ok && parse("\n# only a comment\n").m_ok, "empty documents");
    check.expect(parse("a = 1\r\nb = 2\r\n").m_ok, "CRLF line endings");
    check.expect(parse("\xEF\xBB\xBF" "a = 1").m_ok, "a byte-order mark");

    check.rejects("a = ", "expected a value", 1);
    check.rejects("a = tru", "expected a value", 1);
    check.rejects("= 3", "expected a key", 1);
    check.rejects("a = 1\na = 2", "key 'a' is defined twice", 2);
    check.rejects("[t]\nx = 1\n[t]", "table 't' is defined twice", 3);
    check.rejects("[[t]]", "arrays of tables", 1);
    check.rejects("a = \"\"\"x\"\"\"", "multi-line strings", 1);
    check.rejects("a = 1979-05-27", "date-times", 1);
    check.rejects("\n\na = \"x", "unterminated string", 3);
    check.rejects("a = [1, 2", "expected ',' or ']'", 1);
    check.rejects("a = 1 b = 2", "expected the end of the line", 1);
    check.rejects("a = \"\\q\"", "invalid escape", 1);
    check.rejects("a = \"\\uD800\"", "invalid unicode escape", 1);
    check.rejects("a = 1.2.3", "invalid value", 1);
    check.rejects("a = 1\na.b = 2", "'a' is an integer, not a table", 2);
    check.rejects("[t", "expected ']'", 1);
    check.rejects("a = { b = 1 c = 2 }", "expected ',' or '}'", 1);
    check.rejects("a = 99999999999999999999", "out of range", 1);

    // overlay semantics: tables merge key by key, everything else is replaced
    ParseResult base = parse("[p]\nx = 1\ny = 2\n[q]\nz = 1\nlist = [1, 2]\n");
    const ParseResult overlay = parse("[p]\ny = 3\nw = 4\n[q]\nlist = [9]\n");
    check.expect(base.m_ok && overlay.m_ok, "merge inputs parse");
    if (base.m_ok && overlay.m_ok) {
        merge(base.m_root, overlay.m_root);
        auto integer = [&](const char* path) { const Value* v = base.m_root.find_path(path); return v != nullptr ? v->m_integer : -1; };
        check.expect(integer("p.x") == 1 && integer("p.y") == 3 && integer("p.w") == 4 && integer("q.z") == 1, "merge combines tables");
        const Value* list = base.m_root.find_path("q.list");
        check.expect(list != nullptr && list->m_items.size() == 1 && list->m_items[0].m_integer == 9, "merge replaces arrays");
    }

    if (check.m_failures == 0) out << "toml selftest: ok (" << check.m_checks << " checks)\n";
    return check.m_failures == 0;
}

}  // namespace fool::toml
