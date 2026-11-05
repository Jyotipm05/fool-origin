//
// Created by jmbab on 05-11-2026.
//
/**
 * @file StringInterner.cpp
 * @brief Implementation unit for the global StringInterner symbol pool.
 */

module;

#include <string_view>
#include <algorithm>

module fool.common;

namespace fool {

    StringInterner& StringInterner::get() {
        static StringInterner instance;
        return instance;
    }

    StringInterner::StringInterner() {
        m_storage.emplace_back(""); // Slot 0 is reserved for INVALID_SYMBOL_ID
    }

    SymbolID StringInterner::intern(std::string_view str) {
        auto it = m_string_to_id.find(str);
        if (it != m_string_to_id.end()) {
            return it->second;
        }

        SymbolID new_id = static_cast<SymbolID>(m_storage.size());
        auto* copy = static_cast<char*>(m_arena.allocate(str.size() + 1, 1));
        std::copy(str.begin(), str.end(), copy);
        copy[str.size()] = '\0';
        std::string_view stored_view(copy, str.size());

        m_storage.push_back(stored_view);
        m_string_to_id[stored_view] = new_id;
        return new_id;
    }

    std::string_view StringInterner::resolve(SymbolID id) const {
        if (id >= m_storage.size()) return "";
        return m_storage[id];
    }

} // namespace fool