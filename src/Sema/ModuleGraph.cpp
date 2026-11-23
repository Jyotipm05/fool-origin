//
// Created by jmbab on 06-11-2026.
//

/**
 * @file ModuleGraph.cpp
 * @brief Pass 1: module indexing by declared name, import resolution, template argument
 *        validation and topological ordering.
 */

module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.sema;

import fool.lexer;
import fool.parser;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }

bool role_for_extension(const std::filesystem::path& path, FileRole& role) {
    std::string ext = path.extension().string();
    if (ext == ".fo") { role = FileRole::Interface; return true; }
    if (ext == ".fl") { role = FileRole::Implementation; return true; }
    if (ext == ".fex") { role = FileRole::ExternalOverlay; return true; }
    return false;
}

/// Imports may sit inside top-level `@if` blocks; the graph conservatively follows all branches.
void collect_imports(const std::vector<DeclNode*>& decls, std::vector<ImportDeclNode*>& out) {
    for (DeclNode* decl : decls) {
        if (auto* imp = dynamic_cast<ImportDeclNode*>(decl)) {
            out.push_back(imp);
        } else if (auto* block = dynamic_cast<ConditionalDeclBlock*>(decl)) {
            for (const ConditionalDeclBranch& branch : block->m_branches) collect_imports(branch.m_declarations, out);
        }
    }
}

} // namespace

ModuleGraphPass1::ModuleGraphPass1(Arena& arena, DiagnosticEngine& diag, TargetConfig target)
    : m_arena(arena), m_diag(diag), m_target(std::move(target)) {}

void ModuleGraphPass1::add_search_path(const std::filesystem::path& path) {
    m_search_paths.push_back(path);
}

void ModuleGraphPass1::index_search_paths() {
    std::vector<std::filesystem::path> files;
    for (const auto& root : m_search_paths) {
        std::error_code ec;
        if (!std::filesystem::is_directory(root, ec)) continue;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root, ec)) {
            FileRole role;
            if (entry.is_regular_file() && role_for_extension(entry.path(), role) && m_target.file_applies(entry.path())) {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (const auto& file : files) add_source_file(file);
}

void ModuleGraphPass1::add_source_file(const std::filesystem::path& path) {
    FileRole role;
    if (!role_for_extension(path, role)) {
        m_diag.set_current_file(path.string());
        m_diag.error(SourceSpan{}, "unsupported file extension (expected .fo, .fl or .fex)");
        return;
    }

    if (!m_loaded.insert(path.lexically_normal().generic_string()).second) return; // already indexed

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        m_diag.set_current_file(path.string());
        m_diag.error(SourceSpan{}, "cannot open file");
        return;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string source = buffer.str();

    m_diag.set_current_file(path.string());
    ModuleAST* ast = nullptr;
    if (m_cache != nullptr) {
        ast = m_cache->parse_cached(source, path, m_target.to_string(), m_arena, m_diag);
    } else {
        Lexer lexer(source, path.string(), m_diag);
        Parser parser(lexer.tokenize_all(), m_arena, m_diag);
        ast = parser.parse_module();
    }
    ++m_indexed_files;
    assign_source_file(ast, path.string());
    m_files.push_back(ModuleFile{path, role, ast});

    if (ast->m_module_sections.empty()) {
        m_diag.error(SourceSpan{}, "file does not declare a module (expected 'module name;')");
        return;
    }

    std::set<SymbolID> seen;
    for (SymbolID section : ast->m_module_sections) {
        if (!seen.insert(section).second) continue;
        ModuleNode& node = m_modules[std::string(text_of(section))];
        node.m_name = section;
        node.m_files.push_back(ModuleFile{path, role, ast});

        if (section != ast->m_module_name || ast->m_parameters.empty()) continue;
        if (node.m_parameters.empty()) {
            node.m_parameters = ast->m_parameters;
            continue;
        }
        bool same = node.m_parameters.size() == ast->m_parameters.size();
        for (size_t i = 0; same && i < node.m_parameters.size(); ++i) {
            same = node.m_parameters[i].m_name == ast->m_parameters[i].m_name;
        }
        if (!same) {
            m_diag.error(ast->m_span, "module '" + std::string(text_of(section)) +
                                      "' declares a different template parameter list than its other files");
        }
    }
}

std::filesystem::path ModuleGraphPass1::project_root(const std::filesystem::path& file) const {
    namespace fs = std::filesystem;
    const fs::path normal = file.lexically_normal();
    for (fs::path dir = normal.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        std::error_code ec;
        if (fs::exists(dir / "fool.toml", ec)) return dir;
        if (dir == dir.parent_path()) break;
    }
    const std::string text = normal.generic_string();
    for (const fs::path& root : m_search_paths) {
        const std::string prefix = root.lexically_normal().generic_string();
        if (text.rfind(prefix, 0) == 0) return root.lexically_normal();
    }
    return normal.parent_path();
}

std::filesystem::path ModuleGraphPass1::import_base(const ImportDeclNode& imp, const std::filesystem::path& from,
                                                    std::string& error) const {
    namespace fs = std::filesystem;
    const fs::path dir = from.lexically_normal().parent_path();
    fs::path base;
    // The nearest enclosing folder called `name` (`src`, `lib`), else `<project root>/name`, else the project root.
    auto anchored = [&](const char* name) {
        for (fs::path p = dir; !p.empty(); p = p.parent_path()) {
            if (p.filename() == name) return p;
            if (p == p.parent_path()) break;
        }
        const fs::path root = project_root(from);
        std::error_code ec;
        return fs::is_directory(root / name, ec) ? root / name : root;
    };
    switch (imp.m_root) {
        case ImportRoot::Relative: base = dir; break;
        case ImportRoot::Std: base = m_std_root; break;
        case ImportRoot::Src: base = anchored("src"); break;
        case ImportRoot::Lib: base = anchored("lib"); break;
        case ImportRoot::Sup:
            base = dir;
            for (uint32_t i = 0; i < imp.m_up; ++i) base /= "..";
            break;
        case ImportRoot::Pack: {
            const fs::path package = project_root(from) / "packages" / std::string(text_of(imp.m_pack));
            std::error_code ec;
            if (!fs::is_directory(package, ec)) {
                error = "package '" + std::string(text_of(imp.m_pack)) + "' is not installed (expected in '" +
                        package.generic_string() + "')";
                return {};
            }
            base = fs::is_directory(package / "lib", ec) ? package / "lib" : package;
            break;
        }
        case ImportRoot::Drive: {
            std::string drive(text_of(imp.m_drive));
            for (char& c : drive) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            base = fs::path(drive + ":/");
            break;
        }
    }
    for (SymbolID segment : imp.m_segments) base /= std::string(text_of(segment));
    base = base.lexically_normal();
    if (base.filename().empty()) base = base.parent_path();
    if (base.empty()) base = ".";
    return base;
}

std::vector<size_t> ModuleGraphPass1::match_files(const std::filesystem::path& base) const {
    const std::string key = base.generic_string();
    std::vector<size_t> matched;
    for (size_t i = 0; i < m_files.size(); ++i) {
        if (unit_key(m_files[i].m_path.string()) == key) matched.push_back(i);
    }
    if (!matched.empty()) return matched;
    for (size_t i = 0; i < m_files.size(); ++i) {
        if (m_files[i].m_path.lexically_normal().parent_path().generic_string() == key) matched.push_back(i);
    }
    return matched;
}

bool ModuleGraphPass1::load_from_disk(const std::filesystem::path& base) {
    namespace fs = std::filesystem;
    const std::string key = base.generic_string();
    std::vector<fs::path> found;
    auto scan = [&](const fs::path& dir, bool by_unit) {
        std::error_code ec;
        if (!fs::is_directory(dir.empty() ? fs::path(".") : dir, ec)) return;
        for (const auto& entry : fs::directory_iterator(dir.empty() ? fs::path(".") : dir, ec)) {
            FileRole role;
            if (!entry.is_regular_file(ec) || !role_for_extension(entry.path(), role) || !m_target.file_applies(entry.path())) continue;
            if (by_unit && unit_key(entry.path().string()) != key) continue;
            found.push_back(entry.path());
        }
    };
    scan(base.parent_path(), true);
    if (found.empty()) scan(base, false);
    std::sort(found.begin(), found.end());
    for (const fs::path& file : found) add_source_file(file);
    return !found.empty();
}

const ResolvedImport* ModuleGraphPass1::resolved_import(const ImportDeclNode* decl) const {
    auto it = m_resolved.find(decl);
    return it == m_resolved.end() ? nullptr : &it->second;
}

bool ModuleGraphPass1::resolve_imports() {
    size_t errors_before = m_diag.error_count();
    TargetConfig target = m_target;

    // Files an import names that were not indexed yet are loaded now; their own imports are followed in turn.
    for (size_t i = 0; i < m_files.size(); ++i) {
        const std::filesystem::path from = m_files[i].m_path;
        std::vector<ImportDeclNode*> imports;
        collect_imports(m_files[i].m_ast->m_declarations, imports);
        for (ImportDeclNode* imp : imports) {
            std::string ignored;
            const std::filesystem::path base = import_base(*imp, from, ignored);
            if (base.empty() || !match_files(base).empty()) continue;
            load_from_disk(base);
        }
    }

    for (auto& [module_name, node] : m_modules) {
        std::set<std::string> dependencies;
        for (const ModuleFile& file : node.m_files) {
            std::vector<ImportDeclNode*> imports;
            collect_imports(file.m_ast->m_declarations, imports);
            m_diag.set_current_file(file.m_path.string());

            for (ImportDeclNode* imp : imports) {
                if (imp->m_module != node.m_name) continue;
                const std::string spelled(text_of(imp->m_module_path));
                std::string error;
                const std::filesystem::path base = import_base(*imp, file.m_path, error);
                if (base.empty()) {
                    m_diag.error(imp->m_span, "unresolved import '" + spelled + "': " + error);
                    continue;
                }
                const std::vector<size_t> matched = match_files(base);
                if (matched.empty()) {
                    m_diag.error(imp->m_span, "unresolved import '" + spelled + "': no file or folder '" + base.generic_string() +
                                              "' (looked for .fo, .fl and .fex files)");
                    continue;
                }

                // The modules the named files belong to, and which files of each the import covers.
                std::map<std::string, std::set<std::string>> by_module;
                for (size_t index : matched) {
                    for (SymbolID section : m_files[index].m_ast->m_module_sections) {
                        by_module[std::string(text_of(section))].insert(unit_key(m_files[index].m_path.string()));
                    }
                }
                ResolvedImport resolved;
                for (auto& [name, units] : by_module) {
                    if (name == module_name) continue;
                    resolved.m_targets.push_back(ResolvedImportTarget{StringInterner::get().intern(name), std::move(units)});
                    dependencies.insert(name);
                }
                if (resolved.m_targets.empty()) {
                    m_diag.error(imp->m_span, "module '" + module_name + "' imports itself ('" + spelled + "' names only its own files)");
                    continue;
                }

                // Template arguments belong to one module: names must exist on it, values must be constants.
                if (!imp->m_template_args.empty() && resolved.m_targets.size() != 1) {
                    m_diag.error(imp->m_span, "template arguments need an import that names exactly one module, but '" + spelled +
                                              "' names " + std::to_string(resolved.m_targets.size()));
                    m_resolved[imp] = std::move(resolved);
                    continue;
                }
                const std::string target_name(text_of(resolved.m_targets.front().m_module));
                ModuleNode& dep = m_modules.at(target_name);
                m_resolved[imp] = std::move(resolved);
                ConstTable overrides;
                ConstTable empty;
                ConstEvaluator evaluator(empty, m_diag);
                std::set<SymbolID> given;
                for (const ImportArg& arg : imp->m_template_args) {
                    bool known = std::any_of(dep.m_parameters.begin(), dep.m_parameters.end(),
                                             [&](const ModuleParam& p) { return p.m_name == arg.m_name; });
                    if (!known) {
                        m_diag.error(arg.m_span, "module '" + target_name + "' has no template parameter '" +
                                                 std::string(text_of(arg.m_name)) + "'");
                        continue;
                    }
                    if (!given.insert(arg.m_name).second) {
                        m_diag.error(arg.m_span, "template argument '" + std::string(text_of(arg.m_name)) +
                                                 "' is specified more than once");
                        continue;
                    }
                    if (auto value = evaluator.evaluate(arg.m_value)) overrides[arg.m_name] = *value;
                }

                if (overrides.empty()) continue;
                std::vector<std::pair<std::string, std::string>> rendered;
                for (const auto& [name, value] : overrides) {
                    rendered.emplace_back(std::string(text_of(name)), value.to_string());
                }
                std::sort(rendered.begin(), rendered.end());
                std::string key = target_name + "<";
                for (size_t i = 0; i < rendered.size(); ++i) {
                    if (i != 0) key += ",";
                    key += rendered[i].first + "=" + rendered[i].second;
                }
                key += ">@" + target.to_string();
                bool exists = std::any_of(dep.m_instantiations.begin(), dep.m_instantiations.end(),
                                          [&](const ModuleInstantiation& inst) { return inst.m_key == key; });
                if (!exists) dep.m_instantiations.push_back(ModuleInstantiation{key, std::move(overrides)});
            }
        }

        node.m_dependencies.clear();
        for (const std::string& dependency : dependencies) {
            node.m_dependencies.push_back(StringInterner::get().intern(dependency));
        }
    }
    return m_diag.error_count() == errors_before;
}

bool ModuleGraphPass1::sort_topological() {
    enum class Mark { Unvisited, InProgress, Done };
    std::map<std::string, Mark> marks;
    for (const auto& entry : m_modules) marks[entry.first] = Mark::Unvisited;
    std::vector<std::string> stack;
    bool ok = true;
    m_topological_order.clear();
    m_diag.set_current_file("");

    // Recursive lambda via std::function-free explicit helper struct.
    struct Visitor {
        ModuleGraphPass1& m_graph;
        std::map<std::string, Mark>& m_marks;
        std::vector<std::string>& m_stack;
        bool& m_ok;

        void visit(const std::string& name) {
            m_marks[name] = Mark::InProgress;
            m_stack.push_back(name);
            const ModuleNode& node = m_graph.m_modules.at(name);
            for (SymbolID dep_id : node.m_dependencies) {
                std::string dep(StringInterner::get().resolve(dep_id));
                if (m_graph.m_modules.find(dep) == m_graph.m_modules.end()) continue;
                if (m_marks[dep] == Mark::Unvisited) {
                    visit(dep);
                } else if (m_marks[dep] == Mark::InProgress) {
                    std::string cycle;
                    auto start = std::find(m_stack.begin(), m_stack.end(), dep);
                    for (auto it = start; it != m_stack.end(); ++it) cycle += *it + " -> ";
                    cycle += dep;
                    m_graph.m_diag.error(SourceSpan{}, "circular module dependency: " + cycle);
                    m_ok = false;
                }
            }
            m_stack.pop_back();
            m_marks[name] = Mark::Done;
            m_graph.m_topological_order.push_back(node.m_name);
        }
    };

    Visitor visitor{*this, marks, stack, ok};
    for (const auto& entry : m_modules) {
        if (marks[entry.first] == Mark::Unvisited) visitor.visit(entry.first);
    }
    return ok;
}

const ModuleNode* ModuleGraphPass1::get_module(SymbolID name) const {
    auto it = m_modules.find(std::string(text_of(name)));
    return it == m_modules.end() ? nullptr : &it->second;
}

} // namespace fool
