//
// Created by jmbab on 06-11-2026.
//

/**
 * @file BMICache.cpp
 * @brief `.flbmi` container format, cache key derivation and the on-disk cache manager.
 */

module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module fool.bmi;

#ifndef FOOL_FRONTEND_FINGERPRINT
#define FOOL_FRONTEND_FINGERPRINT "dev"
#endif

namespace fool {

namespace {

constexpr uint8_t BMI_MAGIC[8] = {'F', 'L', 'B', 'M', 'I', 0x1A, 0x00, 0x00};
constexpr size_t HEADER_SIZE = 8 + 4 + 32 + 32 + 8;
constexpr uintmax_t MAX_ENTRY_BYTES = 256ull * 1024 * 1024; ///< refuse absurd files instead of reading them

void put_u32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint32_t get_u32(const uint8_t* p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p[i]) << (8 * i);
    return v;
}

uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

/// Hashes a field together with its length so that field boundaries cannot be shifted
/// ("ab" + "c" must not collide with "a" + "bc").
void hash_field(Sha256& sha, std::string_view field) {
    uint8_t length[8];
    uint64_t n = field.size();
    for (int i = 0; i < 8; ++i) length[i] = static_cast<uint8_t>(n >> (8 * i));
    sha.update(length, sizeof(length));
    sha.update(field);
}

std::string random_suffix() {
    static thread_local std::mt19937_64 engine{std::random_device{}()};
    return std::to_string(engine());
}

} // namespace

std::string_view frontend_fingerprint() { return FOOL_FRONTEND_FINGERPRINT; }

Digest compute_cache_key(std::string_view source, std::string_view template_args, std::string_view target_triple) {
    Sha256 sha;
    hash_field(sha, "foolc-flbmi-key");
    uint8_t version[4];
    for (int i = 0; i < 4; ++i) version[i] = static_cast<uint8_t>(BMI_FORMAT_VERSION >> (8 * i));
    sha.update(version, sizeof(version));
    hash_field(sha, frontend_fingerprint());
    hash_field(sha, source);
    hash_field(sha, template_args);
    hash_field(sha, target_triple);
    return sha.finish();
}

std::vector<uint8_t> pack_bmi(const Digest& key, std::span<const uint8_t> payload) {
    std::vector<uint8_t> out;
    out.reserve(HEADER_SIZE + payload.size());
    out.insert(out.end(), std::begin(BMI_MAGIC), std::end(BMI_MAGIC));
    put_u32(out, BMI_FORMAT_VERSION);
    out.insert(out.end(), key.begin(), key.end());
    Digest payload_hash = Sha256::hash(payload.data(), payload.size());
    out.insert(out.end(), payload_hash.begin(), payload_hash.end());
    put_u64(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::optional<std::span<const uint8_t>> unpack_bmi(std::span<const uint8_t> file, const Digest& expected_key) {
    if (file.size() < HEADER_SIZE) return std::nullopt;
    const uint8_t* p = file.data();
    if (std::memcmp(p, BMI_MAGIC, sizeof(BMI_MAGIC)) != 0) return std::nullopt;
    if (get_u32(p + 8) != BMI_FORMAT_VERSION) return std::nullopt;
    if (std::memcmp(p + 12, expected_key.data(), expected_key.size()) != 0) return std::nullopt;
    uint64_t size = get_u64(p + 76);
    if (size != file.size() - HEADER_SIZE) return std::nullopt;
    std::span<const uint8_t> payload = file.subspan(HEADER_SIZE);
    Digest actual = Sha256::hash(payload.data(), payload.size());
    if (std::memcmp(p + 44, actual.data(), actual.size()) != 0) return std::nullopt;
    return payload;
}

std::filesystem::path BMICacheManager::path_for(const Digest& key) const {
    return m_directory / (to_hex(key) + ".flbmi");
}

ModuleAST* BMICacheManager::load(const Digest& key, Arena& arena) {
    if (!uses_disk()) return nullptr;
    const std::filesystem::path path = path_for(key);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        ++m_stats.m_misses;
        return nullptr;
    }
    const uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > MAX_ENTRY_BYTES) {
        ++m_stats.m_rejected;
        return nullptr;
    }

    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    std::ifstream in(path, std::ios::binary);
    if (!in || (size != 0 && !in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))) {
        ++m_stats.m_rejected;
        return nullptr;
    }

    std::optional<std::span<const uint8_t>> payload = unpack_bmi(bytes, key);
    ModuleAST* ast = payload ? decode_module(*payload, arena) : nullptr;
    if (ast == nullptr) {
        ++m_stats.m_rejected;
        return nullptr;
    }
    ++m_stats.m_hits;
    return ast;
}

bool BMICacheManager::store(const Digest& key, const ModuleAST& module) {
    if (!uses_disk()) return false;
    std::optional<std::vector<uint8_t>> payload = encode_module(module);
    if (!payload) {
        ++m_stats.m_store_failures;
        return false;
    }
    std::vector<uint8_t> file = pack_bmi(key, *payload);

    std::error_code ec;
    std::filesystem::create_directories(m_directory, ec);
    if (ec) {
        ++m_stats.m_store_failures;
        return false;
    }

    // Write to a private temporary name, then rename: readers (and parallel foolc processes) never
    // observe a half-written entry.
    const std::filesystem::path final_path = path_for(key);
    std::filesystem::path temp_path = final_path;
    temp_path += ".tmp" + random_suffix();
    {
        std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())) ||
            !out.flush()) {
            out.close();
            std::filesystem::remove(temp_path, ec);
            ++m_stats.m_store_failures;
            return false;
        }
    }
    std::filesystem::rename(temp_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(temp_path, ec);
        ++m_stats.m_store_failures;
        return false;
    }
    ++m_stats.m_stores;
    return true;
}

ModuleAST* BMICacheManager::parse_cached(std::string_view source, const std::filesystem::path& file,
                                         std::string_view target_triple, Arena& arena, DiagnosticEngine& diag) {
    const bool caching = uses_disk() || m_verify_roundtrip;
    Digest key{};
    if (caching) {
        key = compute_cache_key(source, "", target_triple);
        if (ModuleAST* cached = load(key, arena)) return cached;
    }

    const size_t errors_before = diag.error_count();
    const size_t warnings_before = diag.warning_count();
    diag.set_current_file(file.string());
    Lexer lexer(source, file.string(), diag);
    Parser parser(lexer.tokenize_all(), arena, diag);
    ModuleAST* ast = parser.parse_module();
    if (!caching) return ast;

    // Only clean parses are cached, so a cache hit can never swallow a diagnostic.
    if (diag.error_count() != errors_before || diag.warning_count() != warnings_before) return ast;

    if (uses_disk()) store(key, *ast);

    if (m_verify_roundtrip) {
        std::optional<std::vector<uint8_t>> first = encode_module(*ast);
        if (!first) return ast; // not representable (e.g. nesting too deep): keep the original tree
        ModuleAST* decoded = decode_module(*first, arena);
        std::optional<std::vector<uint8_t>> second;
        if (decoded != nullptr) second = encode_module(*decoded);
        if (decoded == nullptr || !second || *second != *first) {
            diag.error(ast->m_span, "internal error: the .flbmi round trip of this file is not lossless");
            return ast;
        }
        return decoded; // continue on the tree a cache hit would have produced
    }
    return ast;
}

} // namespace fool
