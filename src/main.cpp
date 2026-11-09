//
// Created by jmbab on 05-11-2026.
//

/**
 * @file main.cpp
 * @brief foolc driver: file mode, project mode and expect-error test mode.
 *
 * Exit codes: 0 = success, 1 = diagnostics with errors (or failed expectations), 2 = usage error.
 */

#if defined(__MINGW64__) || defined(__GNUC__)
#include <algorithm>
#include <cstddef>
#endif
#include <cstdint>
#if defined(__MINGW64__) || defined(__GNUC__)
#include <cstdlib>
#endif
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

import fool.common;
import fool.diagnostics;
import fool.lexer;
import fool.ast;
import fool.parser;
import fool.sema;
import fool.borrow;

namespace {

namespace fs = std::filesystem;

struct Options {
    fool::TargetConfig m_target;
    std::vector<fs::path> m_files;
    std::vector<fs::path> m_roots;
    bool m_project{false};
    bool m_graph{false};
    bool m_quiet{false};
    bool m_check_borrow{false};
    bool m_warn_shadow{false};
    bool m_werror{false};
};

void print_usage() {
    std::cout <<
        "usage: foolc [options] [file.fo|file.fl|file.fex ...]\n"
        "\n"
        "  Without files, checks the whole workspace (stdlib/ and examples/) in project mode.\n"
        "\n"
        "options:\n"
        "  --target <os>-<arch>   target platform (default: linux-x86_64)\n"
        "  --project              check every module below the search roots as a unit\n"
        "  -I <dir>               add a module search root (project mode)\n"
        "  --graph                print the module compilation order (project mode)\n"
        "  --check-borrow         also run the opt-in borrow checker (aliasing XOR mutability)\n"
        "  --warn-shadow          warn when a declaration shadows one from an enclosing scope or a global\n"
        "  -Werror                treat warnings as errors\n"
        "  -q, --quiet            suppress the summary line\n"
        "  -h, --help             show this help\n"
        "\n"
        "Files containing '// expect-error: <text>' comments are checked in test mode: the run\n"
        "succeeds only if exactly the annotated errors are reported (a comment on its own line\n"
        "applies to the next line).\n";
}

uint8_t pointer_width_for(std::string_view arch) {
    if (arch == "wasm32" || arch == "i386" || arch == "x86" || arch == "arm" || arch == "riscv32") return 32;
    return 64;
}

bool parse_target(std::string_view text, fool::TargetConfig& out) {
    size_t dash = text.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 == text.size()) return false;
    std::string os(text.substr(0, dash));
    std::string arch(text.substr(dash + 1));
    out = fool::TargetConfig(os, arch, pointer_width_for(arch));
    return true;
}

bool read_file(const fs::path& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

// ---------------------------------------------------------------------------------------------
// File mode
// ---------------------------------------------------------------------------------------------

/// Lex, parse, prune and check one file on its own. Imports are not resolved in this mode, so
/// checks that need symbols from other files are skipped rather than guessed.
void check_single_file(const fs::path& path, const std::string& source, const fool::TargetConfig& target,
                       fool::DiagnosticEngine& diag, const Options& options) {
    diag.set_current_file(path.string());
    fool::Arena arena;
    fool::Lexer lexer(source, path.string(), diag);
    fool::Parser parser(lexer.tokenize_all(), arena, diag);
    fool::ModuleAST* module = parser.parse_module();
    fool::assign_source_file(module, path.string());
    if (diag.has_errors()) return; // do not run semantic checks on a broken tree

    fool::SymbolTablePass0 pass0(target, diag);
    pass0.define_parameters(module->m_parameters);
    pass0.collect_declarations(module);

    fool::TypeCheckerPass2 checker(pass0, diag);
    checker.set_warn_shadow(options.m_warn_shadow);
    if (checker.check_module() && options.m_check_borrow) fool::BorrowCheckerPass(pass0, diag).check_module();
}

struct Expectation {
    fool::Severity m_severity{fool::Severity::Error};
    size_t m_line{0};
    std::string m_text;
    bool m_matched{false};
};

std::vector<Expectation> parse_expectations(const std::string& source) {
    struct Marker { std::string_view m_text; fool::Severity m_severity; };
    static constexpr Marker markers[] = {{"// expect-error:", fool::Severity::Error},
                                         {"// expect-warning:", fool::Severity::Warning}};
    std::vector<Expectation> expectations;
    std::istringstream stream(source);
    std::string line;
    size_t line_number = 0;
    std::vector<Expectation> pending; // standalone markers apply to the next line
    while (std::getline(stream, line)) {
        ++line_number;
        const Marker* found = nullptr;
        size_t pos = std::string::npos;
        for (const Marker& marker : markers) {
            if (size_t at = line.find(marker.m_text); at != std::string::npos) {
                found = &marker;
                pos = at;
            }
        }
        if (found == nullptr) {
            for (Expectation& e : pending) {
                e.m_line = line_number;
                expectations.push_back(std::move(e));
            }
            pending.clear();
            continue;
        }
        std::string text = line.substr(pos + found->m_text.size());
        size_t first = text.find_first_not_of(" \t\r");
        size_t last = text.find_last_not_of(" \t\r");
        text = first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
        Expectation expectation{found->m_severity, line_number, std::move(text), false};
        if (line.find_first_not_of(" \t") == pos) pending.push_back(std::move(expectation));
        else expectations.push_back(std::move(expectation));
    }
    return expectations;
}

/// Returns true when the reported diagnostics match the annotations exactly. Errors must always be
/// annotated; warnings only when the file contains at least one `expect-warning` marker.
bool verify_expectations(const fs::path& path, std::vector<Expectation>& expectations,
                         const fool::DiagnosticEngine& diag) {
    bool ok = true;
    const bool tracks_warnings = std::any_of(expectations.begin(), expectations.end(), [](const Expectation& e) {
        return e.m_severity == fool::Severity::Warning;
    });
    std::vector<bool> used(diag.diagnostics().size(), false);
    for (Expectation& expected : expectations) {
        for (size_t i = 0; i < diag.diagnostics().size(); ++i) {
            const fool::Diagnostic& d = diag.diagnostics()[i];
            if (used[i] || d.m_severity != expected.m_severity) continue;
            if (d.m_span.m_start.m_line == expected.m_line && d.m_message.find(expected.m_text) != std::string::npos) {
                used[i] = true;
                expected.m_matched = true;
                break;
            }
        }
        if (!expected.m_matched) {
            std::cerr << path.string() << ":" << expected.m_line << ": expected "
                      << (expected.m_severity == fool::Severity::Warning ? "warning" : "error") << " containing '"
                      << expected.m_text << "' was not reported\n";
            ok = false;
        }
    }
    for (size_t i = 0; i < diag.diagnostics().size(); ++i) {
        const fool::Diagnostic& d = diag.diagnostics()[i];
        const bool relevant = d.m_severity == fool::Severity::Error || (tracks_warnings && d.m_severity == fool::Severity::Warning);
        if (relevant && !used[i]) {
            std::cerr << "unexpected " << d.format() << "\n";
            ok = false;
        }
    }
    return ok;
}

int run_file_mode(const Options& options) {
    int failures = 0;
    size_t checked = 0;
    for (const fs::path& path : options.m_files) {
        std::string source;
        if (!read_file(path, source)) {
            std::cerr << "foolc: cannot open '" << path.string() << "'\n";
            ++failures;
            continue;
        }
        ++checked;
        std::vector<Expectation> expectations = parse_expectations(source);
        fool::DiagnosticEngine diag;
        diag.set_echo(expectations.empty());
        check_single_file(path, source, options.m_target, diag, options);

        if (expectations.empty()) {
            if (diag.has_errors() || (options.m_werror && diag.warning_count() > 0)) ++failures;
        } else if (!verify_expectations(path, expectations, diag)) {
            ++failures;
        }
    }
    if (!options.m_quiet) {
        std::cout << "foolc: checked " << checked << " file(s) for " << options.m_target.to_string() << ", "
                  << failures << " failed\n";
    }
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Project mode
// ---------------------------------------------------------------------------------------------

/// Builds the symbol table of one module (all of its files, filtered to the module's own section)
/// and runs the type checker over it with strict name resolution.
std::unique_ptr<fool::SymbolTablePass0>
check_module_group(const fool::ModuleNode& node, const fool::ConstTable& overrides, const fool::TargetConfig& target,
                   fool::DiagnosticEngine& diag, const Options& options,
                   const std::unordered_map<fool::SymbolID, const fool::SymbolTablePass0*>& built) {
    auto table = std::make_unique<fool::SymbolTablePass0>(target, diag);
    table->define_parameters(node.m_parameters, overrides);
    for (const fool::ModuleFile& file : node.m_files) {
        table->collect_declarations(file.m_ast, node.m_name);
    }
    for (fool::ImportDeclNode* imp : table->import_declarations()) {
        auto it = built.find(imp->m_module_path);
        table->add_import(imp, it != built.end() ? it->second : nullptr);
    }
    fool::TypeCheckerPass2 checker(*table, diag);
    checker.set_strict_names(true);
    checker.set_warn_shadow(options.m_warn_shadow);
    if (checker.check_module() && options.m_check_borrow) fool::BorrowCheckerPass(*table, diag).check_module();
    return table;
}

int run_project_mode(const Options& options) {
    fool::Arena arena;
    fool::DiagnosticEngine diag;
    fool::ModuleGraphPass1 graph(arena, diag, options.m_target);
    for (const fs::path& root : options.m_roots) graph.add_search_path(root);

    graph.index_search_paths();
    bool graph_ok = !diag.has_errors();
    graph_ok = graph.resolve_imports() && graph_ok;
    graph_ok = graph.sort_topological() && graph_ok;
    if (!graph_ok) {
        std::cerr << "foolc: module graph has errors; skipping semantic checks\n";
        return 1;
    }

    if (options.m_graph) {
        std::cout << "module compilation order (" << options.m_target.to_string() << "):\n";
        for (fool::SymbolID id : graph.get_compilation_order()) {
            const fool::ModuleNode* node = graph.get_module(id);
            std::cout << "  " << fool::StringInterner::get().resolve(id) << "  [" << node->m_files.size() << " file(s)";
            if (!node->m_dependencies.empty()) {
                std::cout << "; imports";
                for (fool::SymbolID dep : node->m_dependencies) std::cout << " " << fool::StringInterner::get().resolve(dep);
            }
            std::cout << "]\n";
        }
    }

    std::vector<std::unique_ptr<fool::SymbolTablePass0>> owned;
    std::unordered_map<fool::SymbolID, const fool::SymbolTablePass0*> built;
    size_t instantiations = 0;
    for (fool::SymbolID id : graph.get_compilation_order()) {
        const fool::ModuleNode* node = graph.get_module(id);
        owned.push_back(check_module_group(*node, {}, options.m_target, diag, options, built));
        built[id] = owned.back().get();

        // Each distinct set of import template arguments is checked as its own instantiation, so
        // @if branches that only become active for that configuration are verified too.
        for (const fool::ModuleInstantiation& inst : node->m_instantiations) {
            ++instantiations;
            size_t before = diag.error_count();
            check_module_group(*node, inst.m_overrides, options.m_target, diag, options, built);
            if (diag.error_count() != before) std::cerr << "note: errors above occurred in instantiation '" << inst.m_key << "'\n";
        }
    }

    if (!options.m_quiet) {
        std::cout << "foolc: checked " << graph.modules().size() << " module(s) from " << graph.indexed_file_count()
                  << " file(s) for " << options.m_target.to_string() << " (" << instantiations
                  << " instantiation(s)), " << diag.error_count() << " error(s), " << diag.warning_count()
                  << " warning(s)\n";
    }
    return (diag.has_errors() || (options.m_werror && diag.warning_count() > 0)) ? 1 : 0;
}

} // namespace

int main(const int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::string arg = argv[i]; arg == "-h" || arg == "--help") {
            print_usage();
            return 0;
        } else if (arg == "--target" && i + 1 < argc) {
            if (!parse_target(argv[++i], options.m_target)) {
                std::cerr << "foolc: invalid --target '" << argv[i] << "' (expected <os>-<arch>, e.g. linux-x86_64)\n";
                return 2;
            }
        } else if (arg == "-I" && i + 1 < argc) {
            options.m_roots.emplace_back(argv[++i]);
        } else if (arg == "--project") {
            options.m_project = true;
        } else if (arg == "--graph") {
            options.m_graph = true;
            options.m_project = true;
        } else if (arg == "--warn-shadow") {
            options.m_warn_shadow = true;
        } else if (arg == "-Werror") {
            options.m_werror = true;
        } else if (arg == "--check-borrow") {
            options.m_check_borrow = true;
        } else if (arg == "-q" || arg == "--quiet") {
            options.m_quiet = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "foolc: unknown option '" << arg << "' (see --help)\n";
            return 2;
        } else {
            options.m_files.emplace_back(arg);
        }
    }

    if (options.m_files.empty()) options.m_project = true; // default: check the whole workspace
    if (options.m_project) {
        if (options.m_roots.empty()) {
            options.m_roots.emplace_back("stdlib");
            options.m_roots.emplace_back("examples");
        }
        return run_project_mode(options);
    }
    return run_file_mode(options);
}
