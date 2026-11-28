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
import fool.hir;
import fool.mir;
import fool.llvm;
import fool.bmi;
import fool.manifest;

namespace {

namespace fs = std::filesystem;

struct Options {
    fool::TargetConfig m_target;
    std::vector<fs::path> m_files;
    std::vector<fs::path> m_roots;
    fs::path m_std_root{"stdlib"};
    bool m_project{false};
    bool m_graph{false};
    bool m_show_packages{false};
    bool m_quiet{false};
    bool m_check_borrow{false};
    bool m_warn_shadow{false};
    bool m_werror{false};
    bool m_dump_calls{false};
    bool m_dump_types{false};
    bool m_dump_reflection{false};
    bool m_dump_hir{false};
    bool m_dump_mir{false};
    bool m_emit_llvm{false};          ///< --emit-llvm: print the LLVM IR
    bool m_build{false};              ///< --build: write the IR, let clang produce the executable
    bool m_keep_ll{false};            ///< --keep-ll: keep `<output>.ll` after a build
    fs::path m_output;                ///< -o
    std::string m_clang;              ///< --clang (else $FOOL_CLANG, else `clang`)
    bool m_mir_selftest{false};
    bool m_use_cache{false};
    fs::path m_cache_dir;
    bool m_bmi_roundtrip{false};
    bool m_bmi_selftest{false};
    bool m_toml_selftest{false};
    /// Parse entry point shared by every mode; a default-constructed manager simply parses.
    std::shared_ptr<fool::BMICacheManager> m_cache{std::make_shared<fool::BMICacheManager>()};
};

/// Prints every call whose arguments were bound: parameters in ABI order, each filled by the argument
/// at the given source position or by the parameter's default.
void dump_resolved_calls(const fool::TypeCheckerPass2& checker) {
    auto& interner = fool::StringInterner::get();
    for (const fool::ResolvedCall& call : checker.resolved_calls()) {
        const auto& start = call.m_call->m_span.m_start;
        std::cout << "call " << call.m_display << " (" << start.m_line << ":" << start.m_column << "):";
        if (call.m_receiver != nullptr) std::cout << " self=receiver";
        for (size_t i = 0; i < call.m_slots.size(); ++i) {
            const fool::ResolvedArgument& slot = call.m_slots[i];
            std::cout << ' ' << interner.resolve(call.m_callee->m_params[i].m_name) << '=';
            if (slot.m_is_default) std::cout << "default";
            else std::cout << "arg" << slot.m_source_index;
        }
        std::cout << '\n';
    }
}

/// Prints the inferred type of every checked expression, in source order: `line:col-endcol kind type`
/// (`?` when the type could not be inferred).
void dump_expression_types(const fool::TypeCheckerPass2& checker) {
    struct Entry { const fool::ExprNode* m_expr; const fool::StaticType* m_type; };
    std::vector<Entry> entries;
    for (const auto& [expr, type] : checker.expression_types()) entries.push_back({expr, &type});
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const auto& sa = a.m_expr->m_span;
        const auto& sb = b.m_expr->m_span;
        if (sa.m_start.m_offset != sb.m_start.m_offset) return sa.m_start.m_offset < sb.m_start.m_offset;
        return sa.m_end.m_offset > sb.m_end.m_offset; // enclosing expression first
    });
    auto kind_of = [](const fool::ExprNode* e) -> const char* {
        if (dynamic_cast<const fool::IntegerLiteralExpr*>(e)) return "int";
        if (dynamic_cast<const fool::FloatLiteralExpr*>(e)) return "float";
        if (dynamic_cast<const fool::StringLiteralExpr*>(e)) return "string";
        if (dynamic_cast<const fool::BooleanLiteralExpr*>(e)) return "bool";
        if (dynamic_cast<const fool::NullLiteralExpr*>(e)) return "null";
        if (dynamic_cast<const fool::LambdaExpr*>(e)) return "lambda";
        if (dynamic_cast<const fool::MatchExpr*>(e)) return "match";
        if (dynamic_cast<const fool::IdentifierExpr*>(e)) return "ident";
        if (dynamic_cast<const fool::PathExpr*>(e)) return "path";
        if (dynamic_cast<const fool::UnaryExpr*>(e)) return "unary";
        if (dynamic_cast<const fool::BinaryExpr*>(e)) return "binary";
        if (dynamic_cast<const fool::CastExpr*>(e)) return "cast";
        if (dynamic_cast<const fool::MemberExpr*>(e)) return "member";
        if (dynamic_cast<const fool::IndexExpr*>(e)) return "index";
        if (dynamic_cast<const fool::CallExpr*>(e)) return "call";
        if (dynamic_cast<const fool::IntrinsicCallExpr*>(e)) return "intrinsic";
        return "struct";
    };
    for (const Entry& entry : entries) {
        const auto& span = entry.m_expr->m_span;
        std::cout << span.m_start.m_line << ':' << span.m_start.m_column << '-';
        if (span.m_end.m_line != span.m_start.m_line) std::cout << span.m_end.m_line << ':';
        std::cout << span.m_end.m_column << ' ' << kind_of(entry.m_expr) << ' '
                  << (entry.m_type->known() ? checker.type_text(*entry.m_type) : std::string("?")) << '\n';
    }
}

/// HIR -> MIR for the checked module, verified: a verifier failure is a compiler bug and is reported as an error.
std::string build_mir(const fool::TypeCheckerPass2& checker, const fool::SymbolTablePass0& pass0, fool::DiagnosticEngine& diag) {
    const fool::mir::Module module = fool::mir::lower(fool::hir::lower_module(checker, pass0));
    for (const std::string& problem : fool::mir::verify(module)) diag.error(fool::SourceSpan{}, "internal error: invalid MIR: " + problem);
    return fool::mir::dump(module);
}

/// HIR -> MIR -> LLVM IR for the checked module. A verifier failure is a compiler bug and is reported as an error.
fool::llvm::EmitResult emit_llvm(const fool::TypeCheckerPass2& checker, const fool::SymbolTablePass0& pass0, fool::DiagnosticEngine& diag,
                                 const std::string& module_name) {
    const fool::mir::Module module = fool::mir::lower(fool::hir::lower_module(checker, pass0));
    for (const std::string& problem : fool::mir::verify(module)) diag.error(fool::SourceSpan{}, "internal error: invalid MIR: " + problem);
    fool::llvm::EmitOptions emit_options;
    emit_options.m_module_name = module_name;
    return fool::llvm::emit(module, emit_options);
}

/// True when `text` can be put between double quotes on a command line as it is.
bool is_plain_argument(const std::string& text) {
    return text.find_first_of("\"\r\n") == std::string::npos && !text.empty();
}

/// `--build`: writes the IR next to the output and lets clang compile and link it. Reports why it cannot, and returns success.
bool build_executable(const Options& options, const fs::path& source, const fool::llvm::EmitResult& result, fool::DiagnosticEngine& diag) {
    if (!result.m_skipped.empty()) {
        std::string list;
        for (const fool::llvm::SkippedFunction& skipped : result.m_skipped) list += "\n  " + skipped.m_name + ": " + skipped.m_reason;
        diag.error(fool::SourceSpan{}, "cannot build: " + std::to_string(result.m_skipped.size()) +
                                           " function(s) cannot be compiled to LLVM yet:" + list);
        return false;
    }
    for (const std::string& problem : result.m_problems) diag.error(fool::SourceSpan{}, "cannot build: " + problem);
    if (!result.m_problems.empty()) return false;
    if (!result.m_has_main) {
        diag.error(fool::SourceSpan{}, "cannot build: the program has no 'fn main()' (it takes no parameters and returns nothing or i32)");
        return false;
    }

    fs::path output = options.m_output;
    if (output.empty()) {
        output = source.stem();
#ifdef _WIN32
        output.replace_extension(".exe");
#endif
    }
    const fs::path ll_path = fs::path(output.string() + ".ll");
    std::string clang = !options.m_clang.empty() ? options.m_clang : std::string();
    if (clang.empty()) {
        // std::getenv is flagged as unsafe by the MSVC runtime (-Wdeprecated-declarations); _dupenv_s is its replacement.
#ifdef _MSC_VER
        char* env = nullptr;
        size_t env_size = 0;
        if (_dupenv_s(&env, &env_size, "FOOL_CLANG") == 0 && env != nullptr && *env != '\0') clang = env;
        std::free(env);
#else
        if (const char* env = std::getenv("FOOL_CLANG"); env != nullptr && *env != '\0') clang = env;
#endif
        if (clang.empty()) clang = "clang";
    }
    if (!is_plain_argument(clang) || !is_plain_argument(output.string())) {
        diag.error(fool::SourceSpan{}, "cannot build: the clang or output path contains a quote or a line break");
        return false;
    }
    {
        std::ofstream file(ll_path, std::ios::binary);
        file << result.m_ir;
        if (!file) {
            diag.error(fool::SourceSpan{}, "cannot build: cannot write '" + ll_path.string() + "'");
            return false;
        }
    }
    std::string command = "\"" + clang + "\" -O1 -Wno-override-module -x ir \"" + ll_path.string() + "\" -o \"" + output.string() + "\"";
#ifdef _WIN32
    command = "\"" + command + "\"";   // cmd.exe strips one pair of outer quotes
#endif
    const int status = std::system(command.c_str());
    if (!options.m_keep_ll) {
        std::error_code ignored;
        fs::remove(ll_path, ignored);
    }
    if (status != 0) {
        diag.error(fool::SourceSpan{}, "cannot build: '" + clang + "' failed (status " + std::to_string(status) + "); is clang installed? use --clang <path> or FOOL_CLANG");
        return false;
    }
    if (!options.m_quiet) std::cout << "foolc: built '" << output.string() << "'\n";
    return true;
}

void print_cache_stats(const Options& options) {
    if (options.m_quiet || !options.m_cache->uses_disk()) return;
    const fool::BMIStats& stats = options.m_cache->stats();
    std::cout << "foolc: bmi cache " << options.m_cache->directory().string() << ": " << stats.m_hits << " hit(s), "
              << stats.m_misses << " miss(es), " << stats.m_stores << " stored";
    if (stats.m_rejected != 0) std::cout << ", " << stats.m_rejected << " rejected";
    if (stats.m_store_failures != 0) std::cout << ", " << stats.m_store_failures << " not stored";
    std::cout << "\n";
}

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
        "  --std-root <dir>       where '@std:' imports look (default: stdlib)\n"
        "  --graph                print the module compilation order (project mode)\n"
        "  --show-packages        print each project's fool.toml settings and where its packages resolve (project mode)\n"
        "  --check-borrow         also run the opt-in borrow checker (aliasing XOR mutability)\n"
        "  --warn-shadow          warn when a declaration shadows one from an enclosing scope or a global\n"
        "  -Werror                treat warnings as errors\n"
        "  --dump-calls           print each call's arguments in canonical (ABI) slot order, defaults filled\n"
        "  --dump-types           print the inferred type of every checked expression (line:col-end kind type)\n"
        "  --dump-reflection      print every class, struct and enum with its fields and whether it can be serialized\n"
        "  --dump-hir             lower the checked functions to HIR and print it (unsupported ones are listed with the reason)\n"
        "  --mir-selftest         check that the MIR verifier reports hand-built broken graphs\n"
        "  --dump-mir             build the MIR control-flow graph from the HIR, verify it, and print it\n"
        "  --emit-llvm            compile the MIR to LLVM IR text and print it (functions it cannot emit yet are listed as comments)\n"
        "  --build                compile one file to an executable through clang: foolc --build prog.fl -o prog\n"
        "  -o <path>              output of --build (default: the file's name)\n"
        "  --clang <path>         the clang used by --build (default: $FOOL_CLANG, else `clang` on the PATH)\n"
        "  --keep-ll              keep <output>.ll after --build\n"
        "  --cache                reuse parsed files from the .flbmi cache in .fool_cache/bmi\n"
        "  --cache-dir <dir>      like --cache, with an explicit cache directory\n"
        "  --bmi-roundtrip        debug: check every file on the tree decoded from its encoded .flbmi form\n"
        "  --toml-selftest        run the built-in fool.toml reader and version-matching checks\n"
        "  --bmi-selftest         run the built-in cache checks (SHA-256, codec, corruption, disk); extra\n"
        "                         files given on the command line are used as codec samples\n"
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
    fool::ModuleAST* module = options.m_cache->parse_cached(source, path, target.to_string(), arena, diag);
    fool::assign_source_file(module, path.string());
    if (diag.has_errors()) return; // do not run semantic checks on a broken tree

    fool::SymbolTablePass0 pass0(target, diag);
    pass0.define_parameters(module->m_parameters);
    pass0.collect_declarations(module);

    fool::TypeCheckerPass2 checker(pass0, diag);
    checker.set_warn_shadow(options.m_warn_shadow);
    if (checker.check_module() && options.m_check_borrow) fool::BorrowCheckerPass(pass0, diag).check_module();
    if (options.m_dump_calls && !diag.has_errors()) dump_resolved_calls(checker);
    if (options.m_dump_types && !diag.has_errors()) dump_expression_types(checker);
    if (options.m_dump_reflection && !diag.has_errors()) std::cout << checker.reflection_report();
    if (options.m_dump_hir && !diag.has_errors()) std::cout << fool::hir::dump(fool::hir::lower_module(checker, pass0));
    if (options.m_dump_mir && !diag.has_errors()) std::cout << build_mir(checker, pass0, diag);
    if ((options.m_emit_llvm || options.m_build) && !diag.has_errors()) {
        const fool::llvm::EmitResult result = emit_llvm(checker, pass0, diag, std::string(fool::StringInterner::get().resolve(module->m_module_name)));
        if (options.m_emit_llvm && !diag.has_errors()) std::cout << result.m_ir;
        if (options.m_build && !diag.has_errors()) (void)build_executable(options, path, result, diag);
    }
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
    print_cache_stats(options);
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------------------------
// Project mode
// ---------------------------------------------------------------------------------------------

/// Builds the symbol table of one module (all of its files, filtered to the module's own section)
/// and runs the type checker over it with strict name resolution.
std::unique_ptr<fool::SymbolTablePass0>
check_module_group(const fool::ModuleGraphPass1& graph, const fool::ModuleNode& node, const fool::ConstTable& overrides,
                   const fool::TargetConfig& target, fool::DiagnosticEngine& diag, const Options& options,
                   const std::unordered_map<fool::SymbolID, const fool::SymbolTablePass0*>& built) {
    auto table = std::make_unique<fool::SymbolTablePass0>(target, diag);
    table->define_parameters(node.m_parameters, overrides);
    for (const fool::ModuleFile& file : node.m_files) {
        table->collect_declarations(file.m_ast, node.m_name);
    }
    for (fool::ImportDeclNode* imp : table->import_declarations()) {
        const fool::ResolvedImport* resolved = graph.resolved_import(imp);
        if (resolved == nullptr) continue;   // did not resolve: the graph already reported it
        for (const fool::ResolvedImportTarget& target_module : resolved->m_targets) {
            auto it = built.find(target_module.m_module);
            table->add_import(imp, target_module.m_module, it != built.end() ? it->second : nullptr, target_module.m_units);
        }
    }
    table->check_selective_imports();
    fool::TypeCheckerPass2 checker(*table, diag);
    checker.set_strict_names(true);
    checker.set_warn_shadow(options.m_warn_shadow);
    if (checker.check_module() && options.m_check_borrow) fool::BorrowCheckerPass(*table, diag).check_module();
    if (options.m_dump_calls && !diag.has_errors()) {
        std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << ":\n";
        dump_resolved_calls(checker);
    }
    if (options.m_dump_types && !diag.has_errors()) {
        std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << " types:\n";
        dump_expression_types(checker);
    }
    if (options.m_dump_reflection && !diag.has_errors()) {
        std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << " reflection:\n";
        std::cout << checker.reflection_report();
    }
    if (options.m_dump_hir && !diag.has_errors()) {
        std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << " hir:\n";
        std::cout << fool::hir::dump(fool::hir::lower_module(checker, *table));
    }
    if (options.m_dump_mir && !diag.has_errors()) {
        std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << " mir:\n";
        std::cout << build_mir(checker, *table, diag);
    }
    if (options.m_emit_llvm && !diag.has_errors()) {
        const fool::llvm::EmitResult result = emit_llvm(checker, *table, diag, std::string(fool::StringInterner::get().resolve(node.m_name)));
        if (!diag.has_errors()) {
            std::cout << "module " << fool::StringInterner::get().resolve(node.m_name) << " llvm:\n" << result.m_ir;
        }
    }
    return table;
}

int run_project_mode(const Options& options) {
    fool::Arena arena;
    fool::DiagnosticEngine diag;
    fool::ModuleGraphPass1 graph(arena, diag, options.m_target);
    for (const fs::path& root : options.m_roots) graph.add_search_path(root);
    graph.set_cache(options.m_cache.get());
    graph.set_std_root(options.m_std_root);

    graph.index_search_paths();
    if (options.m_show_packages) std::cout << graph.describe_projects();
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
        owned.push_back(check_module_group(graph, *node, {}, options.m_target, diag, options, built));
        built[id] = owned.back().get();

        // Each distinct set of import template arguments is checked as its own instantiation, so
        // @if branches that only become active for that configuration are verified too.
        for (const fool::ModuleInstantiation& inst : node->m_instantiations) {
            ++instantiations;
            size_t before = diag.error_count();
            check_module_group(graph, *node, inst.m_overrides, options.m_target, diag, options, built);
            if (diag.error_count() != before) std::cerr << "note: errors above occurred in instantiation '" << inst.m_key << "'\n";
        }
    }

    if (!options.m_quiet) {
        std::cout << "foolc: checked " << graph.modules().size() << " module(s) from " << graph.indexed_file_count()
                  << " file(s) for " << options.m_target.to_string() << " (" << instantiations
                  << " instantiation(s)), " << diag.error_count() << " error(s), " << diag.warning_count()
                  << " warning(s)\n";
    }
    print_cache_stats(options);
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
        } else if (arg == "--std-root" && i + 1 < argc) {
            options.m_std_root = argv[++i];
        } else if (arg == "--project") {
            options.m_project = true;
        } else if (arg == "--dump-calls") {
            options.m_dump_calls = true;
        } else if (arg == "--dump-types") {
            options.m_dump_types = true;
        } else if (arg == "--dump-reflection") {
            options.m_dump_reflection = true;
        } else if (arg == "--dump-hir") {
            options.m_dump_hir = true;
        } else if (arg == "--mir-selftest") {
            options.m_mir_selftest = true;
        } else if (arg == "--dump-mir") {
            options.m_dump_mir = true;
        } else if (arg == "--emit-llvm") {
            options.m_emit_llvm = true;
        } else if (arg == "--build") {
            options.m_build = true;
        } else if (arg == "--keep-ll") {
            options.m_keep_ll = true;
        } else if (arg == "-o" && i + 1 < argc) {
            options.m_output = argv[++i];
        } else if (arg == "--clang" && i + 1 < argc) {
            options.m_clang = argv[++i];
        } else if (arg == "--show-packages") {
            options.m_show_packages = true;
        } else if (arg == "--graph") {
            options.m_graph = true;
            options.m_project = true;
        } else if (arg == "--warn-shadow") {
            options.m_warn_shadow = true;
        } else if (arg == "-Werror") {
            options.m_werror = true;
        } else if (arg == "--cache") {
            options.m_use_cache = true;
        } else if (arg == "--cache-dir" && i + 1 < argc) {
            options.m_use_cache = true;
            options.m_cache_dir = argv[++i];
        } else if (arg == "--bmi-roundtrip") {
            options.m_bmi_roundtrip = true;
        } else if (arg == "--toml-selftest") {
            options.m_toml_selftest = true;
        } else if (arg == "--bmi-selftest") {
            options.m_bmi_selftest = true;
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

    if (options.m_mir_selftest) {
        const std::vector<std::string> failures = fool::mir::selftest();
        for (const std::string& failure : failures) std::cerr << "mir selftest: " << failure << "\n";
        std::cout << (failures.empty() ? "mir selftest: ok\n" : "mir selftest: FAILED\n");
        return failures.empty() ? 0 : 1;
    }
    if (options.m_toml_selftest) return fool::run_manifest_selftest(std::cout) ? 0 : 1;
    if (options.m_bmi_selftest) return fool::run_bmi_selftest(std::cout, options.m_files) ? 0 : 1;

    if (options.m_use_cache || options.m_bmi_roundtrip) {
        fs::path dir;
        if (options.m_use_cache) dir = options.m_cache_dir.empty() ? fool::BMICacheManager::default_directory() : options.m_cache_dir;
        options.m_cache = std::make_shared<fool::BMICacheManager>(dir, options.m_bmi_roundtrip);
    }

    if (options.m_build && (options.m_project || options.m_files.size() != 1)) {
        std::cerr << "foolc: --build takes exactly one source file (project builds are not available yet)\n";
        return 2;
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
