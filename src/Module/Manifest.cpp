//
// Created for patch 11: `fool.toml` / `overwrite.toml` support.
//

/**
 * @file Manifest.cpp
 * @brief Loads `fool.toml` / `overwrite.toml` into a Manifest and finds `@pack:` packages.
 */

module;

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module fool.manifest;

import fool.common;
import fool.diagnostics;
import fool.toml;

namespace fool {

namespace fs = std::filesystem;

const Dependency* Manifest::find_dependency(std::string_view name) const {
    for (const Dependency& dependency : m_dependencies) {
        if (dependency.m_name == name) return &dependency;
    }
    return nullptr;
}

namespace {

constexpr const char* kManifestFile = "fool.toml";
constexpr const char* kOverwriteFile = "overwrite.toml";

/// Reports problems against the file a value came from.
class Reporter {
    DiagnosticEngine& m_diag;
    std::string m_files[2];

public:
    Reporter(DiagnosticEngine& diag, std::string manifest_file, std::string overwrite_file)
        : m_diag(diag), m_files{std::move(manifest_file), std::move(overwrite_file)} {}

    void report(Severity severity, uint8_t origin, uint32_t line, uint32_t column, std::string message) {
        const std::string saved = m_diag.current_file();
        m_diag.set_current_file(m_files[origin < 2 ? origin : 0]);
        const SourceLocation where(line, column, 0);
        m_diag.report(severity, SourceSpan(where, where), std::move(message));
        m_diag.set_current_file(saved);
    }

    void error(const toml::Value& at, std::string message) {
        report(Severity::Error, at.m_origin, at.m_line, at.m_column, std::move(message));
    }

    void warning(const toml::Value& at, std::string message) {
        report(Severity::Warning, at.m_origin, at.m_line, at.m_column, std::move(message));
    }
};

bool read_file(const fs::path& path, std::string& out) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

std::string home_folder() {
    auto env = [](const char* name) -> std::string {
#if defined(_MSC_VER)
        char* value = nullptr;
        size_t size = 0;
        std::string result;
        if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) result = value;
        std::free(value);
        return result;
#else
        const char* value = std::getenv(name);
        return value != nullptr ? value : "";
#endif
    };
    std::string home = env("HOME");
    if (home.empty()) home = env("USERPROFILE");
    return home;
}

/// A path written in a manifest: `~` is the home folder, a relative path is relative to the project folder.
bool resolve_path(Reporter& reporter, const toml::Value& at, const fs::path& dir, fs::path& out) {
    const std::string& text = at.m_string;
    if (text.empty()) {
        reporter.error(at, "a path cannot be empty");
        return false;
    }
    if (text == "~" || text.rfind("~/", 0) == 0 || text.rfind("~\\", 0) == 0) {
        const std::string home = home_folder();
        if (home.empty()) {
            reporter.error(at, "cannot expand '~' in '" + text + "': no home folder is set (HOME / USERPROFILE)");
            return false;
        }
        out = (fs::path(home) / text.substr(text.size() > 1 ? 2 : 1)).lexically_normal();
    } else {
        const fs::path given(text);
        out = given.is_absolute() ? given.lexically_normal() : (dir / given).lexically_normal();
    }
    if (out.filename().empty()) out = out.parent_path();
    return true;
}

/// `table[key]` as a string, or leaves `out` alone; a value of another kind is an error.
bool string_field(Reporter& reporter, const toml::Value& table, const char* key, const char* where, std::string& out) {
    const toml::Value* value = table.find(key);
    if (value == nullptr) return true;
    if (!value->is_string()) {
        reporter.error(*value, std::string("'") + where + "' must be a string, not " + toml::Value::describe(value->m_kind));
        return false;
    }
    out = value->m_string;
    return true;
}

void warn_unknown_keys(Reporter& reporter, const toml::Value& table, std::initializer_list<const char*> known, const std::string& where) {
    for (size_t i = 0; i < table.m_keys.size(); ++i) {
        bool ok = false;
        for (const char* name : known) ok = ok || table.m_keys[i] == name;
        if (!ok) reporter.warning(table.m_items[i], "unknown key '" + table.m_keys[i] + "' in " + where + " (ignored)");
    }
}

/// A single path component: `src`, `lib` and `packages` name folders, never somewhere else.
bool check_folder_name(Reporter& reporter, const toml::Value& table, const char* key, std::string& value, const char* fallback) {
    const toml::Value* at = table.find(key);
    if (at == nullptr || !at->is_string()) return true;
    const fs::path path(value);
    if (value.empty() || path.is_absolute() || value.find("..") != std::string::npos) {
        reporter.error(*at, std::string("'") + key + "' must be a folder inside the project, not '" + value + "'");
        value = fallback;
        return false;
    }
    return true;
}

void read_package(Reporter& reporter, const toml::Value& root, Manifest& manifest) {
    const toml::Value* package = root.find("package");
    if (package == nullptr) return;
    if (!package->is_table()) {
        reporter.error(*package, std::string("'package' must be a table, not ") + toml::Value::describe(package->m_kind));
        return;
    }
    string_field(reporter, *package, "name", "package.name", manifest.m_name);
    string_field(reporter, *package, "version", "package.version", manifest.m_version);
    string_field(reporter, *package, "src", "package.src", manifest.m_src);
    string_field(reporter, *package, "lib", "package.lib", manifest.m_lib);
    check_folder_name(reporter, *package, "src", manifest.m_src, "src");
    check_folder_name(reporter, *package, "lib", manifest.m_lib, "lib");
    warn_unknown_keys(reporter, *package, {"name", "version", "src", "lib"}, "[package]");
}

void read_packages(Reporter& reporter, const toml::Value& root, Manifest& manifest) {
    const toml::Value* packages = root.find("packages");
    if (packages == nullptr) return;
    if (!packages->is_table()) {
        reporter.error(*packages, std::string("'packages' must be a table, not ") + toml::Value::describe(packages->m_kind));
        return;
    }
    string_field(reporter, *packages, "dir", "packages.dir", manifest.m_packages);
    check_folder_name(reporter, *packages, "dir", manifest.m_packages, "packages");

    auto add = [&](const toml::Value& entry) {
        if (!entry.is_string()) {
            reporter.error(entry, std::string("a package collection must be a path string, not ") + toml::Value::describe(entry.m_kind));
            return;
        }
        fs::path folder;
        if (resolve_path(reporter, entry, manifest.m_dir, folder)) manifest.m_collections.push_back(std::move(folder));
    };
    if (const toml::Value* one = packages->find("collection")) add(*one);
    if (const toml::Value* many = packages->find("collections")) {
        if (!many->is_array()) {
            reporter.error(*many, std::string("'packages.collections' must be an array of paths, not ") + toml::Value::describe(many->m_kind));
        } else {
            for (const toml::Value& entry : many->m_items) add(entry);
        }
    }
    warn_unknown_keys(reporter, *packages, {"dir", "collection", "collections"}, "[packages]");
}

void read_dependencies(Reporter& reporter, const toml::Value& root, Manifest& manifest) {
    const toml::Value* dependencies = root.find("dependencies");
    if (dependencies == nullptr) return;
    if (!dependencies->is_table()) {
        reporter.error(*dependencies, std::string("'dependencies' must be a table, not ") + toml::Value::describe(dependencies->m_kind));
        return;
    }
    for (size_t i = 0; i < dependencies->m_keys.size(); ++i) {
        const toml::Value& entry = dependencies->m_items[i];
        Dependency dependency;
        dependency.m_name = dependencies->m_keys[i];
        if (entry.is_string()) {
            dependency.m_version = entry.m_string;
        } else if (entry.is_table()) {
            const std::string where = "dependency '" + dependency.m_name + "'";
            string_field(reporter, entry, "version", "version", dependency.m_version);
            string_field(reporter, entry, "git", "git", dependency.m_git);
            if (const toml::Value* path = entry.find("path")) {
                if (!path->is_string()) {
                    reporter.error(*path, std::string("'path' must be a string, not ") + toml::Value::describe(path->m_kind));
                } else {
                    resolve_path(reporter, *path, manifest.m_dir, dependency.m_path);
                }
            }
            warn_unknown_keys(reporter, entry, {"version", "path", "git", "branch", "tag", "rev"}, where);
        } else {
            reporter.error(entry, "dependency '" + dependency.m_name + "' must be a version string or a table, not " +
                                  toml::Value::describe(entry.m_kind));
            continue;
        }
        manifest.m_dependencies.push_back(std::move(dependency));
    }
}

std::vector<std::string_view> split_version(std::string_view text) {
    std::vector<std::string_view> parts;
    while (true) {
        const size_t dot = text.find('.');
        parts.push_back(text.substr(0, dot));
        if (dot == std::string_view::npos) return parts;
        text.remove_prefix(dot + 1);
    }
}

}  // namespace

Manifest load_manifest(const fs::path& dir, DiagnosticEngine& diag) {
    Manifest manifest;
    manifest.m_dir = dir.empty() ? fs::path(".") : dir;
    const fs::path files[2] = {manifest.m_dir / kManifestFile, manifest.m_dir / kOverwriteFile};
    Reporter reporter(diag, files[0].string(), files[1].string());

    toml::Value merged;
    for (uint8_t origin = 0; origin < 2; ++origin) {
        std::string text;
        if (!read_file(files[origin], text)) continue;
        (origin == 0 ? manifest.m_has_manifest : manifest.m_has_overwrite) = true;
        toml::ParseResult parsed = toml::parse(text);
        if (!parsed.m_ok) {
            reporter.report(Severity::Error, origin, parsed.m_line, parsed.m_column, parsed.m_message);
            continue;
        }
        parsed.m_root.set_origin(origin);
        toml::merge(merged, parsed.m_root);
    }

    read_package(reporter, merged, manifest);
    read_packages(reporter, merged, manifest);
    read_dependencies(reporter, merged, manifest);
    for (size_t i = 0; i < merged.m_keys.size(); ++i) {
        const std::string& key = merged.m_keys[i];
        if (key != "package" && key != "packages" && key != "dependencies") {
            reporter.warning(merged.m_items[i], "unknown table '" + key + "' (ignored)");
        }
    }
    return manifest;
}

bool version_matches(std::string_view requirement, std::string_view actual) {
    while (!requirement.empty() && (requirement.front() == '=' || requirement.front() == '^' ||
                                    requirement.front() == '~' || requirement.front() == ' ')) {
        requirement.remove_prefix(1);
    }
    if (requirement.empty() || requirement == "*") return true;
    const std::vector<std::string_view> wanted = split_version(requirement);
    const std::vector<std::string_view> have = split_version(actual);
    if (have.size() < wanted.size()) return false;
    for (size_t i = 0; i < wanted.size(); ++i) {
        if (wanted[i] != have[i]) return false;
    }
    return true;
}

PackageLocation locate_package(std::string_view name, const std::vector<const Manifest*>& projects,
                               const std::function<const Manifest&(const fs::path&)>& manifest_of) {
    PackageLocation result;
    const std::string package(name);
    std::vector<fs::path> searched;
    std::string mismatch;
    std::string git_source;

    for (const Manifest* project : projects) {
        const Dependency* dependency = project->find_dependency(name);
        const std::string wanted = dependency != nullptr ? dependency->m_version : std::string();

        if (dependency != nullptr && !dependency->m_path.empty()) {
            std::error_code ec;
            if (!fs::is_directory(dependency->m_path, ec)) {
                result.m_error = "dependency '" + package + "' points to '" + dependency->m_path.generic_string() +
                                 "', which is not a folder";
                return result;
            }
            const Manifest& found = manifest_of(dependency->m_path);
            if (!wanted.empty() && !found.m_version.empty() && !version_matches(wanted, found.m_version)) {
                result.m_error = "package '" + package + "' " + found.m_version + " in '" + dependency->m_path.generic_string() +
                                 "' does not satisfy the required version '" + wanted + "'";
                return result;
            }
            result.m_root = dependency->m_path;
            return result;
        }
        if (dependency != nullptr && !dependency->m_git.empty() && git_source.empty()) git_source = dependency->m_git;

        std::vector<fs::path> roots{project->packages_dir()};
        roots.insert(roots.end(), project->m_collections.begin(), project->m_collections.end());
        for (const fs::path& root : roots) {
            const fs::path candidate = (root / package).lexically_normal();
            bool seen = false;
            for (const fs::path& done : searched) seen = seen || done == candidate;
            if (seen) continue;
            searched.push_back(candidate);
            std::error_code ec;
            if (!fs::is_directory(candidate, ec)) continue;
            const Manifest& found = manifest_of(candidate);
            if (!wanted.empty() && !found.m_version.empty() && !version_matches(wanted, found.m_version)) {
                if (mismatch.empty()) {
                    mismatch = "package '" + package + "' " + found.m_version + " in '" + candidate.generic_string() +
                               "' does not satisfy the required version '" + wanted + "'";
                }
                continue;
            }
            result.m_root = candidate;
            return result;
        }
    }

    if (!mismatch.empty()) {
        result.m_error = mismatch;
        return result;
    }
    result.m_error = "package '" + package + "' is not installed";
    if (!searched.empty()) {
        result.m_error += " (expected in '" + searched.front().generic_string() + "'";
        for (size_t i = 1; i < searched.size(); ++i) {
            result.m_error += std::string(i == 1 ? "; also looked in '" : ", '") + searched[i].generic_string() + "'";
        }
        result.m_error += ")";
    }
    if (!git_source.empty()) {
        result.m_error += "; its git source '" + git_source + "' needs the package manager, which does not exist yet";
    }
    return result;
}

bool run_manifest_selftest(std::ostream& out) {
    struct Case { const char* requirement; const char* actual; bool expected; };
    const Case cases[] = {
        {"", "1.2.3", true},      {"*", "0.0.1", true},     {"1", "1.2.3", true},    {"1.2", "1.2.9", true},
        {"1.2", "1.3.0", false},  {"2", "1.9.9", false},    {"1.2.3", "1.2", false}, {"^1", "1.4.0", true},
        {"=1.2.3", "1.2.3", true}, {"1", "10.0.0", false},  {"1.0", "1.00", false},
    };
    size_t failures = 0;
    for (const Case& c : cases) {
        if (version_matches(c.requirement, c.actual) == c.expected) continue;
        ++failures;
        out << "manifest selftest: version_matches('" << c.requirement << "', '" << c.actual << "') should be "
            << (c.expected ? "true" : "false") << "\n";
    }
    const bool toml_ok = toml::run_selftest(out);
    if (failures == 0) out << "manifest selftest: ok (" << (sizeof(cases) / sizeof(cases[0])) << " version checks)\n";
    return failures == 0 && toml_ok;
}

}  // namespace fool
