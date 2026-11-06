//
// Created by jmbab on 06-11-2026.
//
/**
 * @file ModuleGraph.cpp
 * @brief Implementation unit of Pass 1 Module Graph resolution and topological sorting.
 */

module;

#include <iostream>
#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

module fool.sema;

namespace fool {

ModuleGraphPass1::ModuleGraphPass1() {
    // Default search paths: stdlib and current workspace
    m_search_paths.emplace_back("stdlib");
    m_search_paths.emplace_back(".");
}

void ModuleGraphPass1::add_search_path(const std::filesystem::path& path) {
    m_search_paths.push_back(path);
}

bool ModuleGraphPass1::resolve_module(SymbolID module_name, const TargetConfig& target) {
    auto& interner = StringInterner::get();
    std::string_view mod_path_str = interner.resolve(module_name);

    // Convert module path e.g. "std::core" or "std::sync::mutex" to relative filesystem path
    // std::sync::mutex -> sync/mutex (if under stdlib) or std/sync/mutex
    std::string rel_path;
    size_t start = 0;
    while (start < mod_path_str.size()) {
        size_t next_colon = mod_path_str.find("::", start);
        std::string_view part = (next_colon == std::string_view::npos) 
            ? mod_path_str.substr(start) 
            : mod_path_str.substr(start, next_colon - start);

        if (!rel_path.empty()) rel_path += "/";
        rel_path += part;

        if (next_colon == std::string_view::npos) break;
        start = next_colon + 2;
    }

    ModuleNode node;
    node.m_name = module_name;

    // Search for .fo, .fl, and .fex across search roots
    bool found_interface = false;
    for (const auto& search_root : m_search_paths) {
        // Try direct path: <search_root>/<rel_path>.fo
        std::filesystem::path candidate_fo = search_root / (rel_path + ".fo");
        std::filesystem::path candidate_fl = search_root / (rel_path + ".fl");

        // Also check if prefix is stripped: e.g. std::core in stdlib/core/
        std::string stripped_rel = rel_path;
        if (stripped_rel.starts_with("std/")) {
            stripped_rel = stripped_rel.substr(4);
        }
        std::filesystem::path alt_fo = search_root / (stripped_rel + ".fo");
        std::filesystem::path alt_fl = search_root / (stripped_rel + ".fl");

        if (std::filesystem::exists(candidate_fo)) {
            node.m_interface_path = candidate_fo;
            if (std::filesystem::exists(candidate_fl)) {
                node.m_implementation_path = candidate_fl;
            }
            found_interface = true;
            break;
        } else if (std::filesystem::exists(alt_fo)) {
            node.m_interface_path = alt_fo;
            if (std::filesystem::exists(alt_fl)) {
                node.m_implementation_path = alt_fl;
            }
            found_interface = true;
            break;
        }
    }

    if (!found_interface) {
        return false;
    }

    // Check for platform overlays: <rel_path>_<os>.fex
    for (const auto& search_root : m_search_paths) {
        std::string overlay_candidate = rel_path + "_" + target.m_os + ".fex";
        std::filesystem::path p = search_root / overlay_candidate;
        if (std::filesystem::exists(p)) {
            node.m_overlay_paths.push_back(p);
        }
    }

    m_modules[module_name] = std::move(node);
    return true;
}

bool ModuleGraphPass1::sort_topological() {
    m_topological_order.clear();
    std::unordered_set<SymbolID> visited;
    std::unordered_set<SymbolID> visiting;

    auto dfs = [&](auto& self, SymbolID mod_id) -> bool {
        if (visiting.contains(mod_id)) {
            // Circular dependency detected
            return false;
        }
        if (visited.contains(mod_id)) {
            return true;
        }

        visiting.insert(mod_id);

        auto it = m_modules.find(mod_id);
        if (it != m_modules.end()) {
            for (SymbolID dep_id : it->second.m_dependencies) {
                if (!self(self, dep_id)) return false;
            }
        }

        visiting.erase(mod_id);
        visited.insert(mod_id);
        m_topological_order.push_back(mod_id);
        return true;
    };

    for (const auto& [id, _] : m_modules) {
        if (!visited.contains(id)) {
            if (!dfs(dfs, id)) return false;
        }
    }

    return true;
}

const std::vector<SymbolID>& ModuleGraphPass1::get_compilation_order() const {
    return m_topological_order;
}

const ModuleNode* ModuleGraphPass1::get_module(SymbolID name) const {
    auto it = m_modules.find(name);
    return (it != m_modules.end()) ? &it->second : nullptr;
}

} // namespace fool

