//
// Created by jmbab on 05-11-2026.
//
/**
 * @file main.cpp
 * @brief Driver entry point verifying the .fo/.fl/.fex classifier, parser, and Pass 0 target pruning.
 */

#include <iostream>
#include <string_view>
#include <string>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

import fool.common;
import fool.lexer;
import fool.ast;
import fool.parser;
import fool.sema;

bool test_source_file(const std::filesystem::path& file_path, const fool::TargetConfig& target) {
    std::cout << "\n========================================\n";
    std::cout << "Testing: " << file_path.string() << "\n";
    std::cout << "========================================\n";

    // 1. File Classification
    fool::FileRole role = fool::classify_fool_file(file_path);
    std::string_view role_name = (role == fool::FileRole::Interface) ? "Interface (.fo)" :
                                 (role == fool::FileRole::Implementation) ? "Implementation (.fl)" :
                                 (role == fool::FileRole::ExternalOverlay) ? "External/Overlay (.fex)" : "Unknown";
    std::cout << "Role: " << role_name << "\n";

    // 2. Read Source
    std::ifstream file(file_path);
    if (!file.is_open()) {
        std::cerr << "Failed to open file: " << file_path << "\n";
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string source_code = buffer.str();

    // 3. Tokenize & Parse
    fool::Arena arena;
    fool::Lexer lexer(source_code, file_path.string());
    auto tokens = lexer.tokenize_all();
    std::cout << "Tokens: " << tokens.size() << " tokens generated\n";

    fool::Parser parser(std::move(tokens), arena);
    fool::ModuleAST* module_ast = parser.parse_module();
    if (!module_ast) {
        std::cerr << "Parsing failed for file: " << file_path << "\n";
        return false;
    }
    std::cout << "Parsed " << module_ast->m_declarations.size() << " top-level declarations\n";

    // 4. Pass 0 Target Pruning
    fool::SymbolTablePass0 pass0(target);
    pass0.collect_declarations(module_ast);

    // 5. Inspect AST
    auto& interner = fool::StringInterner::get();
    for (const auto* decl : module_ast->m_declarations) {
        if (const auto* fn = dynamic_cast<const fool::FunctionDeclNode*>(decl)) {
            std::cout << "  - Function '" << interner.resolve(fn->m_name)
                      << "' [active: " << (fn->m_is_active ? "YES" : "NO (Pruned)")
                      << ", async: " << (fn->m_is_async ? "YES" : "NO") << "]\n";
        } else if (const auto* cls = dynamic_cast<const fool::ClassDeclNode*>(decl)) {
            std::cout << "  - Class '" << interner.resolve(cls->m_name)
                      << "' [fields: " << cls->m_fields.size()
                      << ", methods: " << cls->m_methods.size() << "]\n";
        } else if (const auto* rs = dynamic_cast<const fool::RulesetDeclNode*>(decl)) {
            std::cout << "  - Ruleset '" << interner.resolve(rs->m_name)
                      << "' [methods: " << rs->m_methods.size() << "]\n";
        } else if (const auto* bp = dynamic_cast<const fool::BlueprintDeclNode*>(decl)) {
            std::cout << "  - Blueprint '" << interner.resolve(bp->m_name)
                      << "' [params: " << bp->m_type_params.size() << "]\n";
        } else if (const auto* var = dynamic_cast<const fool::VarDeclNode*>(decl)) {
            std::string_view kind_str = (var->m_kind == fool::BindingKind::Constx) ? "constx" :
                                        (var->m_kind == fool::BindingKind::Immutable) ? "const (immutable)" : "= (mutable)";
            std::cout << "  - Binding '" << interner.resolve(var->m_name)
                      << "' [" << kind_str << "]\n";
        } else if (const auto* imp = dynamic_cast<const fool::ImportDeclNode*>(decl)) {
            std::cout << "  - Import '#import " << interner.resolve(imp->m_module_path) << ";'\n";
        }
    }
    return true;
}

int main(int argc, char* argv[]) {
    // Target configuration (Linux x86_64 default)
    fool::TargetConfig target;
    target.m_os = "linux";
    target.m_arch = "x86_64";
    target.m_pointer_width = 64;

    std::cout << "=== Fool Language Compiler Driver (foolc) ===\n";
    std::cout << "Active Target: " << target.m_os << " (" << target.m_arch << ", "
              << target.m_pointer_width << "-bit)\n";

    if (argc > 1) {
        int failures = 0;
        for (int i = 1; i < argc; ++i) {
            if (!test_source_file(argv[i], target)) {
                failures++;
            }
        }
        return (failures > 0) ? 1 : 0;
    }

    // Default test suite across sample files in examples/
    std::vector<std::filesystem::path> sample_files = {
        "examples/01_basics.fo",
        "examples/01_basics.fl",
        "examples/02_receivers_and_mutability.fo",
        "examples/02_receivers_and_mutability.fl",
        "examples/03_middle_defaults.fo",
        "examples/03_middle_defaults.fl",
        "examples/04_rulesets_and_blueprints.fo",
        "examples/04_rulesets_and_blueprints.fl",
        "examples/05_target_pruning.fex",
        "examples/06_full_program.fo",
        "examples/06_full_program.fl"
    };

    int failures = 0;
    bool ran_samples = false;
    for (const auto& sample : sample_files) {
        if (std::filesystem::exists(sample)) {
            if (!test_source_file(sample, target)) {
                failures++;
            }
            ran_samples = true;
        }
    }

    if (!ran_samples) {
        std::cout << "\nNo sample files found in examples/. Run with: foolc <path_to_fool_file>\n";
        return 1;
    }

    return (failures > 0) ? 1 : 0;
}
