//
// Created by jmbab on 06-11-2026.
//

/**
 * @file BMISelfTest.cpp
 * @brief `foolc --bmi-selftest`: checks the SHA-256 implementation, the cache key, the codec and the
 *        on-disk cache without needing a separate test executable.
 */

module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module fool.bmi;

namespace fool {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view EMBEDDED_SAMPLE = R"(module selftest::sample;

class Pair {
    left: u32;
    right: u32;
}

ruleset Shape {
    fn area(this const& self) -> i32;
}

@target(os = linux)
fn run(count: i32, ratio: f32, raw: *void, shape: &Shape) -> i32 {
    var a = @static_cast<u8>(200);
    var bits = @bit_cast<u32>(ratio);
    var joined = Pair { left: 1, right: 2 };
    var wide = count as i64;
    var i = 0;
    while (i < 10) {
        if (i == 3) { break; } else { i = i + 1; }
    }
    @if (true) {
        var inside = 1.5;
    }
    return count;
}
)";

class Report {
    // 1. Member variables
    std::ostream& m_out;
    size_t m_checks{0};
    size_t m_failed{0};

public:
    // 2. Constructors
    explicit Report(std::ostream& out) : m_out(out) {}

    // 3. Member functions
    void check(bool condition, const std::string& what) {
        ++m_checks;
        if (!condition) {
            ++m_failed;
            m_out << "  FAIL: " << what << "\n";
        }
    }
    [[nodiscard]] size_t checks() const { return m_checks; }
    [[nodiscard]] size_t failed() const { return m_failed; }
};

std::string sha_hex(std::string_view text) { return to_hex(Sha256::hash(text)); }

void test_sha256(Report& r) {
    r.check(sha_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "sha256(\"\")");
    r.check(sha_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "sha256(\"abc\")");
    r.check(sha_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
                "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            "sha256(two-block message)");
    {
        Sha256 sha;
        const std::string chunk(1000, 'a');
        for (int i = 0; i < 1000; ++i) sha.update(chunk);
        r.check(to_hex(sha.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
                "sha256(one million 'a')");
    }
    // Block-boundary lengths: byte-wise streaming must equal one-shot hashing.
    for (size_t length : {size_t{55}, size_t{56}, size_t{63}, size_t{64}, size_t{65}, size_t{119}, size_t{120}, size_t{128}}) {
        std::string text(length, 'x');
        for (size_t i = 0; i < length; ++i) text[i] = static_cast<char>('a' + (i % 23));
        Sha256 streamed;
        for (char c : text) streamed.update(&c, 1);
        r.check(to_hex(streamed.finish()) == sha_hex(text), "streaming == one-shot at length " + std::to_string(length));
    }
}

void test_key(Report& r) {
    const Digest base = compute_cache_key("fn f() {}", "", "linux-x86_64");
    r.check(base == compute_cache_key("fn f() {}", "", "linux-x86_64"), "key is deterministic");
    r.check(base != compute_cache_key("fn g() {}", "", "linux-x86_64"), "key depends on the source");
    r.check(base != compute_cache_key("fn f() {}", "SIMD=1", "linux-x86_64"), "key depends on the template arguments");
    r.check(base != compute_cache_key("fn f() {}", "", "darwin-aarch64"), "key depends on the target triple");
    r.check(compute_cache_key("ab", "c", "t") != compute_cache_key("a", "bc", "t"), "key fields are length-delimited");
    r.check(compute_cache_key("a", "", "bc") != compute_cache_key("a", "b", "c"), "key fields cannot be shifted");
}

struct Parsed {
    Arena m_arena;
    DiagnosticEngine m_diag;
    ModuleAST* m_ast{nullptr};
};

bool parse(Parsed& parsed, std::string_view source, const std::string& name) {
    parsed.m_diag.set_echo(false);
    BMICacheManager plain;
    parsed.m_ast = plain.parse_cached(source, name, "linux-x86_64", parsed.m_arena, parsed.m_diag);
    return parsed.m_ast != nullptr && !parsed.m_diag.has_errors();
}

/// `budget` bounds how many mutations are tried per check (the embedded sample gets a deep run).
void test_codec(Report& r, std::string_view source, const std::string& name, size_t budget) {
    Parsed parsed;
    if (!parse(parsed, source, name)) {
        r.check(false, name + ": sample must parse without errors");
        return;
    }
    std::optional<std::vector<uint8_t>> payload = encode_module(*parsed.m_ast);
    r.check(payload.has_value(), name + ": encodes");
    if (!payload) return;

    Arena arena;
    ModuleAST* decoded = decode_module(*payload, arena);
    r.check(decoded != nullptr, name + ": decodes");
    if (decoded == nullptr) return;
    auto again = encode_module(*decoded);
    r.check(again.has_value() && *again == *payload, name + ": decode(encode(x)) re-encodes byte-identically");
    r.check(decoded->m_declarations.size() == parsed.m_ast->m_declarations.size(), name + ": declaration count preserved");

    // Container: pack/unpack, and every single-byte change anywhere in the file must be rejected.
    const Digest key = compute_cache_key(source, "", "linux-x86_64");
    std::vector<uint8_t> file = pack_bmi(key, *payload);
    r.check(unpack_bmi(file, key).has_value(), name + ": pack/unpack");
    r.check(!unpack_bmi(file, compute_cache_key(source, "other", "linux-x86_64")).has_value(),
            name + ": an entry is rejected under a different key");
    const size_t step = std::max<size_t>(1, file.size() / budget);
    size_t accepted = 0;
    for (size_t i = 0; i < file.size(); i += step) {
        std::vector<uint8_t> mutated = file;
        mutated[i] ^= 0x5A;
        if (unpack_bmi(mutated, key).has_value()) ++accepted;
    }
    r.check(accepted == 0, name + ": corrupted container bytes are all rejected");
    size_t truncated_accepted = 0;
    for (size_t length = 0; length < file.size(); length += std::max<size_t>(1, file.size() / (budget * 3 / 5))) {
        if (unpack_bmi(std::span<const uint8_t>(file.data(), length), key).has_value()) ++truncated_accepted;
    }
    r.check(truncated_accepted == 0, name + ": truncated containers are all rejected");

    // The decoder itself (without the hash in front of it) must survive arbitrary damage.
    size_t truncated_decoded = 0;
    for (size_t length = 0; length < payload->size(); length += std::max<size_t>(1, payload->size() / (budget * 3 / 5))) {
        Arena scratch;
        if (decode_module(std::span<const uint8_t>(payload->data(), length), scratch) != nullptr) ++truncated_decoded;
    }
    r.check(truncated_decoded == 0, name + ": truncated payloads never decode");
    const size_t payload_step = std::max<size_t>(1, payload->size() / (budget / 2));
    for (size_t i = 0; i < payload->size(); i += payload_step) {
        for (uint8_t mask : {uint8_t{0xFF}, uint8_t{0x01}, uint8_t{0x80}}) {
            std::vector<uint8_t> mutated = *payload;
            mutated[i] ^= mask;
            Arena scratch;
            (void)decode_module(mutated, scratch); // must not crash or read out of bounds (run under valgrind)
        }
    }
    r.check(true, name + ": damaged payloads handled");
}

void flip_byte(const fs::path& path, uintmax_t offset) {
    std::fstream f(path, std::ios::in | std::ios::out | std::ios::binary);
    f.seekg(static_cast<std::streamoff>(offset));
    char c = 0;
    f.get(c);
    c = static_cast<char>(c ^ 0x5A);
    f.seekp(static_cast<std::streamoff>(offset));
    f.put(c);
}

void test_disk(Report& r) {
    std::mt19937_64 engine{std::random_device{}()};
    const fs::path root = fs::temp_directory_path() / ("foolc-bmi-selftest-" + std::to_string(engine()));
    const fs::path dir = root / "cache";
    std::error_code ec;
    fs::remove_all(root, ec);

    const std::string source(EMBEDDED_SAMPLE);
    const fs::path file = "selftest/sample.fl";

    auto parse_with = [&](BMICacheManager& cache, const std::string& text, std::string_view triple, DiagnosticEngine& diag,
                          Arena& arena) { return cache.parse_cached(text, file, triple, arena, diag); };

    {
        BMICacheManager cache(dir);
        Arena arena_a, arena_b;
        DiagnosticEngine diag;
        diag.set_echo(false);
        ModuleAST* cold = parse_with(cache, source, "linux-x86_64", diag, arena_a);
        r.check(cold != nullptr && !diag.has_errors(), "disk: cold parse succeeds");
        r.check(cache.stats().m_misses == 1 && cache.stats().m_stores == 1 && cache.stats().m_hits == 0,
                "disk: cold run is one miss and one store");
        ModuleAST* warm = parse_with(cache, source, "linux-x86_64", diag, arena_b);
        r.check(warm != nullptr && cache.stats().m_hits == 1, "disk: warm run hits");
        auto cold_bytes = cold ? encode_module(*cold) : std::nullopt;
        auto warm_bytes = warm ? encode_module(*warm) : std::nullopt;
        r.check(cold_bytes && warm_bytes && *cold_bytes == *warm_bytes, "disk: the cached tree equals the parsed tree");

        Arena arena_c;
        parse_with(cache, source + "\n// edited\n", "linux-x86_64", diag, arena_c);
        r.check(cache.stats().m_hits == 1 && cache.stats().m_stores == 2, "disk: editing the source misses and stores a new entry");
        Arena arena_d;
        parse_with(cache, source, "darwin-aarch64", diag, arena_d);
        r.check(cache.stats().m_hits == 1 && cache.stats().m_stores == 3, "disk: another target misses and stores a new entry");
    }

    const Digest key = compute_cache_key(source, "", "linux-x86_64");
    const fs::path entry = BMICacheManager(dir).path_for(key);
    r.check(fs::is_regular_file(entry), "disk: entry file exists under its hex key");

    {
        flip_byte(entry, fs::file_size(entry) / 2); // damage the payload
        BMICacheManager cache(dir);
        Arena arena_a, arena_b;
        DiagnosticEngine diag;
        diag.set_echo(false);
        ModuleAST* ast = parse_with(cache, source, "linux-x86_64", diag, arena_a);
        r.check(ast != nullptr && !diag.has_errors(), "disk: a corrupted entry falls back to parsing");
        r.check(cache.stats().m_rejected == 1 && cache.stats().m_hits == 0 && cache.stats().m_stores == 1,
                "disk: the corrupted entry is rejected and rewritten");
        parse_with(cache, source, "linux-x86_64", diag, arena_b);
        r.check(cache.stats().m_hits == 1, "disk: the rewritten entry hits");
    }
    {
        fs::resize_file(entry, 40, ec); // truncated entry
        BMICacheManager cache(dir);
        Arena arena;
        DiagnosticEngine diag;
        diag.set_echo(false);
        parse_with(cache, source, "linux-x86_64", diag, arena);
        r.check(cache.stats().m_rejected == 1 && cache.stats().m_stores == 1, "disk: a truncated entry is rejected and rewritten");
    }
    {
        std::ofstream(entry, std::ios::binary | std::ios::trunc) << "not a module interface";
        BMICacheManager cache(dir);
        Arena arena;
        DiagnosticEngine diag;
        diag.set_echo(false);
        parse_with(cache, source, "linux-x86_64", diag, arena);
        r.check(cache.stats().m_rejected == 1 && cache.stats().m_hits == 0, "disk: a garbage entry is rejected");
    }
    {
        // A file with errors must never be cached: the diagnostics would disappear on the next run.
        BMICacheManager cache(dir);
        Arena arena_a, arena_b;
        DiagnosticEngine diag_a, diag_b;
        diag_a.set_echo(false);
        diag_b.set_echo(false);
        const std::string broken = "module selftest::broken;\nfn (((\n";
        parse_with(cache, broken, "linux-x86_64", diag_a, arena_a);
        parse_with(cache, broken, "linux-x86_64", diag_b, arena_b);
        r.check(diag_a.has_errors() && diag_b.has_errors() && cache.stats().m_stores == 0,
                "disk: a file with errors is never cached and keeps reporting them");
    }
    {
        // Unusable cache location (a path below a regular file): parsing still works.
        const fs::path blocker = root / "blocker";
        std::ofstream(blocker) << "x";
        BMICacheManager cache(blocker / "sub");
        Arena arena;
        DiagnosticEngine diag;
        diag.set_echo(false);
        ModuleAST* ast = parse_with(cache, source, "linux-x86_64", diag, arena);
        r.check(ast != nullptr && !diag.has_errors() && cache.stats().m_store_failures == 1,
                "disk: an unwritable cache directory does not break the build");
    }
    {
        // Round-trip verification mode: the returned tree is the decoded one.
        BMICacheManager cache{fs::path(), true};
        Arena arena;
        DiagnosticEngine diag;
        diag.set_echo(false);
        ModuleAST* ast = parse_with(cache, source, "linux-x86_64", diag, arena);
        r.check(ast != nullptr && !diag.has_errors(), "roundtrip mode: parses and verifies");
    }
    fs::remove_all(root, ec);
}

} // namespace

bool run_bmi_selftest(std::ostream& out, const std::vector<std::filesystem::path>& samples) {
    Report report(out);
    test_sha256(report);
    test_key(report);
    test_codec(report, EMBEDDED_SAMPLE, "embedded sample", 2500);
    for (const fs::path& sample : samples) {
        std::ifstream in(sample, std::ios::binary);
        if (!in) {
            report.check(false, "cannot open sample " + sample.string());
            continue;
        }
        std::stringstream buffer;
        buffer << in.rdbuf();
        test_codec(report, buffer.str(), sample.string(), 150);
    }
    test_disk(report);
    out << "foolc: bmi selftest: " << report.checks() << " check(s), " << report.failed() << " failed\n";
    return report.failed() == 0;
}

} // namespace fool
