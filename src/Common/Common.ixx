//
// Created by jmbab on 05-11-2026.
//

/**
 * @file Common.ixx
 * @brief Core performance primitives for the Fool compiler frontend.
 * @details Implements a linear memory bump arena allocator to achieve O(1) AST allocations
 *          and 32-bit interned symbol identifiers for single-cycle string equality checks.
 */

module;

#include <cstdint>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <new>
#include <type_traits>
#include <utility>

export module fool.common;

export namespace fool {

/**
 * @typedef SymbolID
 * @brief 32-bit compact handle representing an interned string token or identifier.
 */
using SymbolID = uint32_t;

/**
 * @def INVALID_SYMBOL_ID
 * @brief Sentinel value indicating an unassigned or invalid interned symbol.
 */
constexpr SymbolID INVALID_SYMBOL_ID = 0;

/**
 * @struct SourceLocation
 * @brief Represents a single line, column, and byte offset inside a source file.
 */
struct SourceLocation {
    // 1. Member variables FIRST on top with 'm_' prefix
    uint32_t m_line{1};
    uint32_t m_column{1};
    uint32_t m_offset{0};

    // 2. Constructors in the MIDDLE
    SourceLocation() = default;
    SourceLocation(uint32_t line, uint32_t column, uint32_t offset)
        : m_line(line), m_column(column), m_offset(offset) {}
};

/**
 * @struct SourceSpan
 * @brief Continuous span bounded by start and end source locations.
 */
struct SourceSpan {
    // 1. Member variables FIRST on top with 'm_' prefix
    SourceLocation m_start;
    SourceLocation m_end;

    // 2. Constructors in the MIDDLE
    SourceSpan() = default;
    SourceSpan(SourceLocation start, SourceLocation end)
        : m_start(start), m_end(end) {}
};

/**
 * @enum FileRole
 * @brief Categorical file role derived from Fool file extension triad (.fo, .fl, .fex).
 */
enum class FileRole : uint8_t {
    Interface,       ///< .fo: Interface outline, public classes, rulesets, and signatures
    Implementation,  ///< .fl: Core algorithmic logic and function bodies
    ExternalOverlay, ///< .fex: Foreign ABI bindings (@extern), kernel syscalls, and patch overlays
    Unknown
};

/**
 * @brief Classifies a Fool source file based on its 2-char / 3-char extension.
 * @param path The filesystem path to check.
 * @return FileRole Categorized file role.
 */
inline FileRole classify_fool_file(const std::filesystem::path& path) {
    auto ext = path.extension().string();
    if (ext == ".fo")  return FileRole::Interface;
    if (ext == ".fl")  return FileRole::Implementation;
    if (ext == ".fex") return FileRole::ExternalOverlay;
    return FileRole::Unknown;
}

/**
 * @class Arena
 * @brief High-performance linear bump allocator.
 * @details Emits contiguous blocks of memory for AST nodes. Avoids individual malloc/free
 *          heap churn and pointer indirection overhead, matching the memory model of compilers like Jai.
 */
class Arena {
    /// Intrusive record, itself bump-allocated, that remembers how to destroy one arena object.
    struct DtorRecord {
        void (*m_destroy)(void*);
        void* m_object;
        DtorRecord* m_next;
    };

    // 1. Member variables FIRST on top with 'm_' prefix
    std::vector<char*> m_chunks;
    std::vector<size_t> m_chunk_sizes;
    char* m_current_chunk{nullptr};
    size_t m_current_capacity{0};
    size_t m_offset{0};
    size_t m_chunk_size{4 * 1024 * 1024};
    DtorRecord* m_dtor_head{nullptr}; ///< LIFO list of non-trivially destructible objects

public:
    // 2. Constructors and Destructors in the MIDDLE
    explicit Arena(size_t chunk_size = 4 * 1024 * 1024)
        : m_chunk_size(chunk_size) {
        alloc_chunk(m_chunk_size);
    }

    ~Arena() {
        run_destructors();
        for (char* chunk : m_chunks) {
            std::free(chunk);
        }
    }

    Arena(const Arena&) = delete;
    Arena& operator=(const Arena&) = delete;

    // 3. Member functions LAST at the bottom

    /// Constructs a T in the arena. Objects owning heap memory (std::vector, std::string members,
    /// virtual destructors, ...) are registered so that reset() / ~Arena() destroy them in reverse order.
    template <typename T, typename... Args>
    T* create(Args&&... args) {
        void* mem = allocate(sizeof(T), alignof(T));
        T* obj = ::new (mem) T(std::forward<Args>(args)...);
        if constexpr (!std::is_trivially_destructible_v<T>) {
            auto* rec = static_cast<DtorRecord*>(allocate(sizeof(DtorRecord), alignof(DtorRecord)));
            rec->m_destroy = [](void* p) { static_cast<T*>(p)->~T(); };
            rec->m_object = obj;
            rec->m_next = m_dtor_head;
            m_dtor_head = rec;
        }
        return obj;
    }

    void* allocate(size_t bytes, size_t alignment = 8) {
        m_offset = (m_offset + alignment - 1) & ~(alignment - 1);
        if (m_offset + bytes > m_current_capacity) {
            size_t next_size = (bytes + alignment > m_chunk_size) ? (bytes + alignment) * 2 : m_chunk_size;
            alloc_chunk(next_size);
            m_offset = (m_offset + alignment - 1) & ~(alignment - 1);
        }
        void* ptr = m_current_chunk + m_offset;
        m_offset += bytes;
        return ptr;
    }

    void reset() {
        run_destructors();
        if (!m_chunks.empty()) {
            for (size_t i = 1; i < m_chunks.size(); ++i) {
                std::free(m_chunks[i]);
            }
            m_chunks.resize(1);
            m_chunk_sizes.resize(1);
            m_current_chunk = m_chunks[0];
            m_current_capacity = m_chunk_sizes[0];
            m_offset = 0;
        }
    }

private:
    void run_destructors() {
        for (DtorRecord* rec = m_dtor_head; rec != nullptr; rec = rec->m_next) {
            rec->m_destroy(rec->m_object);
        }
        m_dtor_head = nullptr;
    }

    void alloc_chunk(size_t size) {
        auto* chunk = static_cast<char*>(std::malloc(size));
        m_chunks.push_back(chunk);
        m_chunk_sizes.push_back(size);
        m_current_chunk = chunk;
        m_current_capacity = size;
        m_offset = 0;
    }
};

/**
 * @class StringInterner
 * @brief Global string deduplication pool.
 * @details Transforms identifier string views into unique 32-bit integers to allow
 *          O(1) identifier comparisons across all compiler passes without string hashing or strcmp.
 */
class StringInterner {
    // 1. Member variables FIRST on top with 'm_' prefix
    Arena m_arena{256 * 1024};
    std::vector<std::string_view> m_storage;
    std::unordered_map<std::string_view, SymbolID> m_string_to_id;

public:
    // 2. Constructors and Destructors in the MIDDLE
    static StringInterner& get();

    // 3. Member functions LAST at the bottom
    SymbolID intern(std::string_view str);
    [[nodiscard]] std::string_view resolve(SymbolID id) const;

private:
    StringInterner();
};

} // namespace fool
