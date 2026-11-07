//
// Created by jmbab on 06-11-2026.
//
/**
 * @file BMICache.cpp
 * @brief Implementation unit of Binary Module Interface (.flbmi) serialization and cache management.
 */

module;

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <cstdint>
#include <cstring>
#include <algorithm>

module fool.bmi;

namespace fool {

namespace {

uint64_t compute_file_hash(const std::filesystem::path& file_path) {
    std::ifstream stream(file_path, std::ios::binary);
    if (!stream.is_open()) return 0;

    // 64-bit FNV-1a hash
    uint64_t hash = 14695981039346656037ULL;
    char buffer[4096];
    while (stream.read(buffer, sizeof(buffer)) || stream.gcount() > 0) {
        size_t count = static_cast<size_t>(stream.gcount());
        for (size_t i = 0; i < count; ++i) {
            hash ^= static_cast<uint8_t>(buffer[i]);
            hash *= 1099511628211ULL;
        }
    }
    return hash;
}

void write_string(std::ofstream& out, std::string_view str) {
    uint32_t len = static_cast<uint32_t>(str.size());
    out.write(reinterpret_cast<const char*>(&len), sizeof(len));
    if (len > 0) {
        out.write(str.data(), len);
    }
}

std::string read_string(std::ifstream& in) {
    uint32_t len = 0;
    in.read(reinterpret_cast<char*>(&len), sizeof(len));
    if (len == 0 || !in.good()) return {};
    std::string s(len, '\0');
    in.read(s.data(), len);
    return s;
}

} // anonymous namespace

BMICacheManager::BMICacheManager()
    : m_cache_directory(".fool_cache/bmi") {}

BMICacheManager::BMICacheManager(std::filesystem::path cache_dir)
    : m_cache_directory(std::move(cache_dir)) {}

std::filesystem::path BMICacheManager::get_bmi_path(SymbolID module_name) const {
    auto& interner = StringInterner::get();
    std::string name(interner.resolve(module_name));
    // Replace "::" with "_" for disk filename
    size_t pos = 0;
    while ((pos = name.find("::", pos)) != std::string::npos) {
        name.replace(pos, 2, "_");
        pos += 1;
    }
    return m_cache_directory / (name + ".flbmi");
}

bool BMICacheManager::is_cache_valid(SymbolID module_name, const std::filesystem::path& source_path, const TargetConfig& target) const {
    std::filesystem::path bmi_path = get_bmi_path(module_name);
    if (!std::filesystem::exists(bmi_path) || !std::filesystem::exists(source_path)) {
        return false;
    }

    std::ifstream in(bmi_path, std::ios::binary);
    if (!in.is_open()) return false;

    BMIHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good() || header.m_magic != BMI_MAGIC || header.m_version != BMI_VERSION) {
        return false;
    }

    if (std::string_view(header.m_target_os) != target.m_os ||
        std::string_view(header.m_target_arch) != target.m_arch) {
        return false;
    }

    uint64_t current_source_hash = compute_file_hash(source_path);
    return (header.m_source_hash == current_source_hash);
}

bool BMICacheManager::write_bmi(const ModuleAST* ast, const std::filesystem::path& source_path, const TargetConfig& target) const {
    if (!ast) return false;

    std::filesystem::create_directories(m_cache_directory);
    std::filesystem::path bmi_path = get_bmi_path(ast->m_module_name);

    std::ofstream out(bmi_path, std::ios::binary);
    if (!out.is_open()) return false;

    BMIHeader header;
    header.m_magic = BMI_MAGIC;
    header.m_version = BMI_VERSION;
    header.m_source_hash = compute_file_hash(source_path);
    header.m_declaration_count = static_cast<uint32_t>(ast->m_declarations.size());

    size_t os_len = std::min(target.m_os.size(), sizeof(header.m_target_os) - 1);
    std::memcpy(header.m_target_os, target.m_os.data(), os_len);
    header.m_target_os[os_len] = '\0';

    size_t arch_len = std::min(target.m_arch.size(), sizeof(header.m_target_arch) - 1);
    std::memcpy(header.m_target_arch, target.m_arch.data(), arch_len);
    header.m_target_arch[arch_len] = '\0';

    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    auto& interner = StringInterner::get();
    // Write module name
    write_string(out, interner.resolve(ast->m_module_name));

    // Write module parameters count
    uint32_t param_count = static_cast<uint32_t>(ast->m_parameters.size());
    out.write(reinterpret_cast<const char*>(&param_count), sizeof(param_count));
    for (const auto& param : ast->m_parameters) {
        write_string(out, interner.resolve(param.m_name));
    }

    // Write declarations metadata
    for (const auto* decl : ast->m_declarations) {
        if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
            uint8_t kind = 1; // Function
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            write_string(out, interner.resolve(fn->m_name));
            uint8_t flags = (fn->m_is_async ? 1 : 0) | (fn->m_is_active ? 2 : 0);
            out.write(reinterpret_cast<const char*>(&flags), sizeof(flags));
        } else if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
            uint8_t kind = 2; // Class
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            write_string(out, interner.resolve(cls->m_name));
            uint32_t field_cnt = static_cast<uint32_t>(cls->m_fields.size());
            out.write(reinterpret_cast<const char*>(&field_cnt), sizeof(field_cnt));
        } else if (const auto* rs = dynamic_cast<const RulesetDeclNode*>(decl)) {
            uint8_t kind = 3; // Ruleset
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            write_string(out, interner.resolve(rs->m_name));
        } else if (const auto* bp = dynamic_cast<const BlueprintDeclNode*>(decl)) {
            uint8_t kind = 4; // Blueprint
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            write_string(out, interner.resolve(bp->m_name));
        } else if (const auto* var = dynamic_cast<const VarDeclNode*>(decl)) {
            uint8_t kind = 5; // Var
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
            write_string(out, interner.resolve(var->m_name));
            uint8_t bkind = static_cast<uint8_t>(var->m_kind);
            out.write(reinterpret_cast<const char*>(&bkind), sizeof(bkind));
        } else {
            uint8_t kind = 0; // Unknown / Other
            out.write(reinterpret_cast<const char*>(&kind), sizeof(kind));
        }
    }

    return true;
}

ModuleAST* BMICacheManager::read_bmi(SymbolID module_name, Arena& arena) const {
    std::filesystem::path bmi_path = get_bmi_path(module_name);
    if (!std::filesystem::exists(bmi_path)) return nullptr;

    std::ifstream in(bmi_path, std::ios::binary);
    if (!in.is_open()) return nullptr;

    BMIHeader header;
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in.good() || header.m_magic != BMI_MAGIC || header.m_version != BMI_VERSION) {
        return nullptr;
    }

    auto& interner = StringInterner::get();
    std::string mod_name_str = read_string(in);
    SymbolID mod_id = interner.intern(mod_name_str);

    auto* ast = arena.create<ModuleAST>();
    ast->m_module_name = mod_id;

    uint32_t param_count = 0;
    in.read(reinterpret_cast<char*>(&param_count), sizeof(param_count));
    for (uint32_t i = 0; i < param_count; ++i) {
        std::string pname = read_string(in);
        ModuleParam param;
        param.m_name = interner.intern(pname);
        ast->m_parameters.push_back(param);
    }

    for (uint32_t i = 0; i < header.m_declaration_count; ++i) {
        uint8_t kind = 0;
        in.read(reinterpret_cast<char*>(&kind), sizeof(kind));
        if (kind == 1) { // Function
            std::string name = read_string(in);
            uint8_t flags = 0;
            in.read(reinterpret_cast<char*>(&flags), sizeof(flags));

            auto* fn = arena.create<FunctionDeclNode>();
            fn->m_name = interner.intern(name);
            fn->m_is_async = (flags & 1) != 0;
            fn->m_is_active = (flags & 2) != 0;
            ast->m_declarations.push_back(fn);
        } else if (kind == 2) { // Class
            std::string name = read_string(in);
            uint32_t field_cnt = 0;
            in.read(reinterpret_cast<char*>(&field_cnt), sizeof(field_cnt));

            auto* cls = arena.create<ClassDeclNode>();
            cls->m_name = interner.intern(name);
            ast->m_declarations.push_back(cls);
        } else if (kind == 3) { // Ruleset
            std::string name = read_string(in);
            auto* rs = arena.create<RulesetDeclNode>();
            rs->m_name = interner.intern(name);
            ast->m_declarations.push_back(rs);
        } else if (kind == 4) { // Blueprint
            std::string name = read_string(in);
            auto* bp = arena.create<BlueprintDeclNode>();
            bp->m_name = interner.intern(name);
            ast->m_declarations.push_back(bp);
        } else if (kind == 5) { // Var
            std::string name = read_string(in);
            uint8_t bkind = 0;
            in.read(reinterpret_cast<char*>(&bkind), sizeof(bkind));

            auto* var = arena.create<VarDeclNode>();
            var->m_name = interner.intern(name);
            var->m_kind = static_cast<BindingKind>(bkind);
            ast->m_declarations.push_back(var);
        }
    }

    return ast;
}

} // namespace fool

