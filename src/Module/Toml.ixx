//
// Created for patch 11: `fool.toml` / `overwrite.toml` support.
//

/**
 * @file Toml.ixx
 * @brief A small TOML reader, just enough for `fool.toml` and `overwrite.toml`.
 * @details Supported: comments, `[table]` and `[a.b]` headers, `key = value` with bare, quoted and dotted keys,
 *          basic (`"..."`, with escapes) and literal (`'...'`) strings, integers, floats, booleans, arrays
 *          (possibly spanning lines) and inline tables. Not supported (reported as errors): `[[arrays of tables]]`,
 *          multi-line strings and date-times.
 */

module;

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

export module fool.toml;

export namespace fool::toml {

enum class Kind : uint8_t { String, Integer, Float, Boolean, Array, Table };

/**
 * @struct Value
 * @brief One parsed TOML value. A table keeps its keys in file order: `m_keys[i]` names `m_items[i]`.
 */
struct Value {
    // 1. Member variables FIRST
    Kind m_kind{Kind::Table};
    std::string m_string;
    int64_t m_integer{0};
    double m_float{0.0};
    bool m_boolean{false};
    std::vector<Value> m_items;          ///< array elements, or the values of a table
    std::vector<std::string> m_keys;     ///< table keys (parallel to `m_items`)
    uint32_t m_line{0};
    uint32_t m_column{0};
    uint8_t m_origin{0};                 ///< which file the value came from (0 = `fool.toml`, 1 = `overwrite.toml`)

    // 2. Constructors
    Value() = default;
    explicit Value(Kind kind) : m_kind(kind) {}

    // 3. Member functions
    [[nodiscard]] bool is_table() const { return m_kind == Kind::Table; }
    [[nodiscard]] bool is_string() const { return m_kind == Kind::String; }
    [[nodiscard]] bool is_array() const { return m_kind == Kind::Array; }
    /// The member `key` of a table, or null.
    [[nodiscard]] const Value* find(std::string_view key) const;
    /// The member at a dotted path (`a.b.c`) below this table, or null.
    [[nodiscard]] const Value* find_path(std::string_view path) const;
    /// Names the kind for diagnostics ("a string", "a table", ...).
    [[nodiscard]] static const char* describe(Kind kind);
    /// Sets the origin of this value and everything below it.
    void set_origin(uint8_t origin);
};

struct ParseResult {
    // 1. Member variables FIRST
    Value m_root;
    bool m_ok{true};
    std::string m_message;
    uint32_t m_line{0};
    uint32_t m_column{0};
};

/// Parses `text`. On failure `m_ok` is false and the message and position say what went wrong.
ParseResult parse(std::string_view text);

/// Lays `overlay` over `base`: tables merge key by key, anything else in `overlay` replaces what `base` has.
void merge(Value& base, const Value& overlay);

/// Runs the reader's built-in checks, writing a one-line verdict (and the failures) to `out`; true when all passed.
bool run_selftest(std::ostream& out);

}  // namespace fool::toml
