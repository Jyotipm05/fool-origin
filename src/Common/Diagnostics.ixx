/**
 * @file Diagnostics.ixx
 * @brief Compiler-wide diagnostic collection (errors, warnings, notes).
 * @details Every pass reports through a DiagnosticEngine instead of writing to std::cerr directly.
 *          The driver derives its exit code from the error count, and the test harness can
 *          compare collected diagnostics against `// expect-error:` annotations.
 */

module;

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module fool.diagnostics;

import fool.common;

export namespace fool {

enum class Severity : uint8_t {
    Note,
    Warning,
    Error
};

/**
 * @struct Diagnostic
 * @brief A single reported message with its originating file and span.
 */
struct Diagnostic {
    // 1. Member variables
    Severity m_severity{Severity::Error};
    std::string m_file;
    SourceSpan m_span;
    std::string m_message;

    // 2. Constructors
    Diagnostic() = default;
    Diagnostic(Severity severity, std::string file, SourceSpan span, std::string message)
        : m_severity(severity), m_file(std::move(file)), m_span(span), m_message(std::move(message)) {}

    // 3. Member functions
    [[nodiscard]] std::string format() const {
        const char* label = (m_severity == Severity::Error) ? "error"
                          : (m_severity == Severity::Warning) ? "warning" : "note";
        std::string out = m_file.empty() ? std::string("<input>") : m_file;
        out += ":" + std::to_string(m_span.m_start.m_line) + ":" + std::to_string(m_span.m_start.m_column);
        out += ": ";
        out += label;
        out += ": ";
        out += m_message;
        return out;
    }
};

/**
 * @class DiagnosticEngine
 * @brief Collects diagnostics; optionally echoes them to stderr as they are reported.
 */
class DiagnosticEngine {
    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<Diagnostic> m_diagnostics;
    std::string m_current_file;
    size_t m_error_count{0};
    size_t m_warning_count{0};
    bool m_echo{true};

public:
    // 2. Constructors and Destructors in the MIDDLE
    DiagnosticEngine() = default;
    ~DiagnosticEngine() = default;

    // 3. Member functions LAST at the bottom
    void set_current_file(std::string file) { m_current_file = std::move(file); }
    void set_echo(bool echo) { m_echo = echo; }

    void report(Severity severity, SourceSpan span, std::string message) {
        if (severity == Severity::Error) ++m_error_count;
        if (severity == Severity::Warning) ++m_warning_count;
        m_diagnostics.emplace_back(severity, m_current_file, span, std::move(message));
        if (m_echo) std::cerr << m_diagnostics.back().format() << "\n";
    }

    void error(SourceSpan span, std::string message) { report(Severity::Error, span, std::move(message)); }
    void warning(SourceSpan span, std::string message) { report(Severity::Warning, span, std::move(message)); }
    void note(SourceSpan span, std::string message) { report(Severity::Note, span, std::move(message)); }

    [[nodiscard]] size_t error_count() const { return m_error_count; }
    [[nodiscard]] size_t warning_count() const { return m_warning_count; }
    [[nodiscard]] bool has_errors() const { return m_error_count != 0; }
    [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const { return m_diagnostics; }

    void clear() {
        m_diagnostics.clear();
        m_error_count = 0;
        m_warning_count = 0;
    }
};

} // namespace fool
