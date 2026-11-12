//
// Created by jmbab on 05-11-2026.
//

/**
 * @file BMICache.ixx
 * @brief Binary Module Interface (`.flbmi`) cache: SHA-256 content addressing, a lossless binary
 *        encoding of the parse tree, and a best-effort on-disk cache that lets `foolc` skip lexing
 *        and parsing of files that did not change.
 *
 * Cache key (see instruction.md, "Deterministic cache salting"):
 *
 *     key = SHA256( format-version || frontend fingerprint || source text || template args || target triple )
 *
 * `source text` is the base, un-instantiated source of the file; import template arguments are
 * resolved after parsing, so the driver currently passes an empty template-argument string and the
 * entry is shared by every instantiation. The parameter is part of the key so that instantiated
 * interfaces can be cached under their own entries without changing the file format.
 * The frontend fingerprint is a hash of the lexer, parser and AST sources computed by CMake, so a
 * compiler that parses differently never reads entries written by an older one.
 *
 * File layout (all integers little-endian):
 *
 *     offset  size  field
 *          0     8  magic        "FLBMI\x1a\0\0"
 *          8     4  version      BMI_FORMAT_VERSION
 *         12    32  key          the key this entry was stored under (self-check against renames)
 *         44    32  payload hash SHA-256 of the payload
 *         76     8  payload size
 *         84     n  payload      string table + module tree, see BMICodec.cpp
 *
 * Every read is bounds-checked and the payload hash is verified before decoding, so a truncated,
 * corrupted or hand-edited file is rejected (counted as `m_rejected`) and simply rebuilt.
 */

module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module fool.bmi;

import fool.common;
import fool.diagnostics;
import fool.lexer;
import fool.ast;
import fool.parser;

export namespace fool {

/// 256-bit digest.
using Digest = std::array<uint8_t, 32>;

/**
 * @class Sha256
 * @brief Streaming SHA-256 (FIPS 180-4). No external dependencies.
 */
class Sha256 {
    // 1. Member variables
    uint32_t m_state[8];
    uint8_t m_block[64];
    uint64_t m_total{0};
    size_t m_fill{0};

public:
    // 2. Constructors and Destructors
    Sha256();
    ~Sha256() = default;

    // 3. Member functions
    void update(const void* data, size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }
    /// Finalizes and returns the digest. The object must not be updated afterwards.
    [[nodiscard]] Digest finish();

    [[nodiscard]] static Digest hash(const void* data, size_t size);
    [[nodiscard]] static Digest hash(std::string_view text) { return hash(text.data(), text.size()); }

private:
    void process_block(const uint8_t* block);
};

/// Lower-case hexadecimal rendering of a digest (64 characters).
[[nodiscard]] std::string to_hex(const Digest& digest);

/// Bumped whenever the byte layout of an entry changes.
constexpr uint32_t BMI_FORMAT_VERSION = 1;

/// Fingerprint of the frontend (lexer + parser + AST) this compiler was built with.
[[nodiscard]] std::string_view frontend_fingerprint();

/**
 * @brief Computes the cache key of one source file.
 * @param source        full text of the file
 * @param template_args canonical text of the import template arguments (empty for the base entry)
 * @param target_triple e.g. `linux-x86_64`
 */
[[nodiscard]] Digest compute_cache_key(std::string_view source, std::string_view template_args,
                                       std::string_view target_triple);

/// Serializes a parse tree. Returns nullopt when the tree contains something the format cannot
/// represent (never a lossy encoding).
[[nodiscard]] std::optional<std::vector<uint8_t>> encode_module(const ModuleAST& module);

/// Rebuilds a parse tree inside `arena`. Returns nullptr for malformed input; never reads out of bounds.
[[nodiscard]] ModuleAST* decode_module(std::span<const uint8_t> payload, Arena& arena);

/// Wraps a payload in the `.flbmi` header.
[[nodiscard]] std::vector<uint8_t> pack_bmi(const Digest& key, std::span<const uint8_t> payload);

/// Validates the header (magic, version, key, size, payload hash) and returns the payload bytes.
[[nodiscard]] std::optional<std::span<const uint8_t>> unpack_bmi(std::span<const uint8_t> file, const Digest& expected_key);

/// Counters reported by `foolc` when the cache is enabled.
struct BMIStats {
    size_t m_hits{0};            ///< entries loaded instead of parsing
    size_t m_misses{0};          ///< no usable entry on disk
    size_t m_stores{0};          ///< entries written
    size_t m_rejected{0};        ///< entries found but unusable (corrupt, stale layout, wrong key)
    size_t m_store_failures{0};  ///< entries that could not be written (read-only directory, ...)
};

/**
 * @class BMICacheManager
 * @brief On-disk `.flbmi` cache plus the cached parse entry point used by the driver.
 * @details With an empty directory no file is touched; with `verify_roundtrip` every freshly
 *          parsed tree is encoded, decoded and compared, and the decoded copy is the one returned,
 *          so the whole test-suite can run on the code path a cache hit takes.
 */
class BMICacheManager {
    // 1. Member variables
    std::filesystem::path m_directory;
    bool m_verify_roundtrip{false};
    BMIStats m_stats;

public:
    // 2. Constructors and Destructors
    BMICacheManager() = default;
    explicit BMICacheManager(std::filesystem::path directory, bool verify_roundtrip = false)
        : m_directory(std::move(directory)), m_verify_roundtrip(verify_roundtrip) {}
    ~BMICacheManager() = default;

    // 3. Member functions
    [[nodiscard]] static std::filesystem::path default_directory() { return ".fool_cache/bmi"; }
    [[nodiscard]] bool uses_disk() const { return !m_directory.empty(); }
    [[nodiscard]] const std::filesystem::path& directory() const { return m_directory; }
    [[nodiscard]] const BMIStats& stats() const { return m_stats; }
    [[nodiscard]] std::filesystem::path path_for(const Digest& key) const;

    /// Loads the entry stored under `key`, or returns nullptr (miss / rejected).
    [[nodiscard]] ModuleAST* load(const Digest& key, Arena& arena);
    /// Stores `module` under `key`. Best effort and atomic (temp file + rename).
    bool store(const Digest& key, const ModuleAST& module);

    /// Lexes and parses `source`, or loads the cached tree. Entries are only written for files that
    /// parsed without any diagnostics, so a hit never hides a message.
    [[nodiscard]] ModuleAST* parse_cached(std::string_view source, const std::filesystem::path& file,
                                          std::string_view target_triple, Arena& arena, DiagnosticEngine& diag);
};

/// Runs the built-in checks (SHA-256 test vectors, key sensitivity, codec round-trip, corruption
/// resistance, on-disk cache behaviour) on an embedded sample plus every file in `samples`, and
/// prints a report. Returns true when everything passed.
bool run_bmi_selftest(std::ostream& out, const std::vector<std::filesystem::path>& samples = {});

} // namespace fool
