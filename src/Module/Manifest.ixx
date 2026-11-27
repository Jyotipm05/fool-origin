//
// Created for patch 11: `fool.toml` / `overwrite.toml` support.
//

/**
 * @file Manifest.ixx
 * @brief Project manifests: `fool.toml` (the project and its packages) and `overwrite.toml` (personal settings that
 *        stay out of a distributed project), and the lookup of `@pack:` packages they drive.
 * @details `overwrite.toml` sits next to `fool.toml` and is laid over it key by key, so it can name a local package
 *          collection (`[packages] collections`) or replace a dependency (`[dependencies] name = { path = ".." }`)
 *          without the shared manifest knowing about either.
 */

module;

#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

export module fool.manifest;

import fool.diagnostics;

export namespace fool {

/// One `[dependencies]` entry.
struct Dependency {
    // 1. Member variables FIRST
    std::string m_name;
    std::string m_version;               ///< wanted version (a prefix such as `1` or `1.2`); empty accepts any
    std::filesystem::path m_path;        ///< `path = ".."`: use this folder (resolved against the project folder)
    std::string m_git;                   ///< `git = ".."`: recorded for the package manager, which does not exist yet
};

/**
 * @struct Manifest
 * @brief What `fool.toml` and `overwrite.toml` say about the project in `m_dir`. A folder with neither file gets the defaults.
 */
struct Manifest {
    // 1. Member variables FIRST
    std::filesystem::path m_dir;
    bool m_has_manifest{false};
    bool m_has_overwrite{false};
    std::string m_name;
    std::string m_version;
    std::string m_src{"src"};            ///< what `@src:` means inside this project
    std::string m_lib{"lib"};            ///< what `@lib:` means; also the folder of a package that other projects import from
    std::string m_packages{"packages"};  ///< the project's own install folder, relative to `m_dir`
    std::vector<Dependency> m_dependencies;
    std::vector<std::filesystem::path> m_collections;   ///< shared package collections searched after the install folder

    // 2. Member functions
    [[nodiscard]] const Dependency* find_dependency(std::string_view name) const;
    [[nodiscard]] std::filesystem::path packages_dir() const { return m_dir / m_packages; }
};

/// Reads `<dir>/fool.toml` and `<dir>/overwrite.toml` (either may be missing); problems are reported through `diag`.
Manifest load_manifest(const std::filesystem::path& dir, DiagnosticEngine& diag);

/// Whether `actual` satisfies `requirement`: empty or `*` accepts anything; otherwise the dot-separated parts of the
/// requirement must equal the leading parts of the version (`1` matches `1.4.2`, `1.4` does not match `1.5.0`).
bool version_matches(std::string_view requirement, std::string_view actual);

/// Where a package was found, or why it was not.
struct PackageLocation {
    // 1. Member variables FIRST
    std::filesystem::path m_root;        ///< the package folder; empty when not found
    std::string m_error;
};

/// Finds the package `name` for a project: the first of `projects` that declares a `path` for it wins, otherwise each
/// project's install folder and then its collections are searched in turn. `manifest_of` loads the manifest of a package folder.
PackageLocation locate_package(std::string_view name, const std::vector<const Manifest*>& projects,
                               const std::function<const Manifest&(const std::filesystem::path&)>& manifest_of);

/// Runs the manifest checks (version matching) and the TOML reader's own checks; true when all passed.
bool run_manifest_selftest(std::ostream& out);

}  // namespace fool
