//
// Created by jmbab on 05-11-2026.
//

/**
 * @file BMICodec.cpp
 * @brief Lossless binary encoding of a parsed module (the `.flbmi` payload).
 *
 * Payload layout:
 *
 *     u32 string_count, then string_count x { u32 length, bytes }      -- the string table
 *     module: sym name, params, sections, declarations, span
 *
 * Names (`SymbolID`) are written as 1-based indices into the string table (0 = invalid symbol),
 * because interner ids are only stable within one process. Nodes start with a one-byte tag followed
 * by their source span; a 0 tag is a null pointer. The decoder checks every length against the bytes
 * that remain and limits nesting, so arbitrary input can neither read out of bounds nor exhaust memory
 * or the stack.
 */

module;

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

module fool.bmi;

namespace fool {

namespace {

constexpr int MAX_DEPTH = 1000;

enum Tag : uint8_t {
    // expressions
    ExprNull = 0, ExprInt = 1, ExprFloat, ExprString, ExprBool, ExprIdent, ExprPath, ExprUnary, ExprBinary,
    ExprCast, ExprMember, ExprIndex, ExprCall, ExprIntrinsic, ExprStruct,
    // statements
    StmtReturn = 1, StmtExpr, StmtDecl, StmtBlock, StmtIf, StmtWhile, StmtFor, StmtBreak, StmtContinue, StmtConditional,
    // declarations
    DeclCondBlock = 1, DeclImport, DeclVar, DeclFunction, DeclRuleset, DeclClass, DeclBlueprint, DeclEnum
};

// ---------------------------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------------------------

class Writer {
    // 1. Member variables
    std::vector<uint8_t> m_body;
    std::vector<std::string> m_strings;
    std::unordered_map<std::string, uint32_t> m_string_index;
    bool m_ok{true};
    int m_depth{0};

public:
    // 2. Member functions
    [[nodiscard]] bool ok() const { return m_ok; }

    std::vector<uint8_t> finish() {
        std::vector<uint8_t> out;
        append_u32(out, static_cast<uint32_t>(m_strings.size()));
        for (const std::string& s : m_strings) {
            append_u32(out, static_cast<uint32_t>(s.size()));
            out.insert(out.end(), s.begin(), s.end());
        }
        out.insert(out.end(), m_body.begin(), m_body.end());
        return out;
    }

    void write_module(const ModuleAST& m) {
        sym(m.m_module_name);
        span(m.m_span);
        count(m.m_parameters.size());
        for (const ModuleParam& p : m.m_parameters) {
            sym(p.m_name);
            expr(p.m_default_value);
        }
        syms(m.m_module_sections);
        decls(m.m_declarations);
    }

private:
    static void append_u32(std::vector<uint8_t>& out, uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }

    void u8(uint8_t v) { m_body.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) m_body.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void u32(uint32_t v) { append_u32(m_body, v); }
    void u64(uint64_t v) { for (int i = 0; i < 8; ++i) m_body.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void count(size_t n) { u32(static_cast<uint32_t>(n)); }
    void boolean(bool b) { u8(b ? 1 : 0); }
    void fail() { m_ok = false; }

    uint32_t intern_text(std::string_view text) {
        std::string key(text);
        auto it = m_string_index.find(key);
        if (it != m_string_index.end()) return it->second;
        m_strings.push_back(key);
        uint32_t index = static_cast<uint32_t>(m_strings.size()); // 1-based
        m_string_index.emplace(std::move(key), index);
        return index;
    }

    void sym(SymbolID id) {
        if (id == INVALID_SYMBOL_ID) { u32(0); return; }
        u32(intern_text(StringInterner::get().resolve(id)));
    }
    void raw(std::string_view text) { u32(intern_text(text)); }
    void syms(const std::vector<SymbolID>& v) {
        count(v.size());
        for (SymbolID id : v) sym(id);
    }
    void span(const SourceSpan& s) {
        u32(s.m_start.m_line); u32(s.m_start.m_column); u32(s.m_start.m_offset);
        u32(s.m_end.m_line); u32(s.m_end.m_column); u32(s.m_end.m_offset);
    }

    struct DepthGuard {
        Writer& m_writer;
        explicit DepthGuard(Writer& w) : m_writer(w) { if (++m_writer.m_depth > MAX_DEPTH) m_writer.fail(); }
        ~DepthGuard() { --m_writer.m_depth; }
        [[nodiscard]] bool too_deep() const { return m_writer.m_depth > MAX_DEPTH; }
    };

    void generic_args(const std::vector<GenericArg>& args) {
        count(args.size());
        for (const GenericArg& a : args) {
            sym(a.m_name);
            type(a.m_type);
            expr(a.m_value);
        }
    }

    void generic_params(const std::vector<GenericParam>& params) {
        count(params.size());
        for (const GenericParam& p : params) {
            sym(p.m_name);
            type(p.m_constraint);
            expr(p.m_default);
        }
    }

    void parameters(const std::vector<ParameterDecl>& params) {
        count(params.size());
        for (const ParameterDecl& p : params) {
            sym(p.m_name);
            type(p.m_type);
            expr(p.m_default_value);
            span(p.m_span);
        }
    }

    void type(const TypeNode* t) {
        if (t == nullptr) { u8(0); return; }
        DepthGuard guard(*this);
        if (guard.too_deep()) return;
        u8(1);
        span(t->m_span);
        u8(static_cast<uint8_t>(t->m_kind));
        boolean(t->m_is_const);
        syms(t->m_path);
        generic_args(t->m_generic_args);
        type(t->m_inner);
    }

    void exprs(const std::vector<ExprNode*>& v) {
        count(v.size());
        for (const ExprNode* e : v) expr(e);
    }

    void expr(const ExprNode* e) {
        if (e == nullptr) { u8(ExprNull); return; }
        DepthGuard guard(*this);
        if (guard.too_deep()) return;
        if (auto* n = dynamic_cast<const IntegerLiteralExpr*>(e)) {
            u8(ExprInt); span(n->m_span); u64(static_cast<uint64_t>(n->m_value));
        } else if (auto* n = dynamic_cast<const FloatLiteralExpr*>(e)) {
            u8(ExprFloat); span(n->m_span); u64(std::bit_cast<uint64_t>(n->m_value));
        } else if (auto* n = dynamic_cast<const StringLiteralExpr*>(e)) {
            u8(ExprString); span(n->m_span); raw(n->m_value);
        } else if (auto* n = dynamic_cast<const BooleanLiteralExpr*>(e)) {
            u8(ExprBool); span(n->m_span); boolean(n->m_value);
        } else if (auto* n = dynamic_cast<const IdentifierExpr*>(e)) {
            u8(ExprIdent); span(n->m_span); sym(n->m_identifier);
        } else if (auto* n = dynamic_cast<const PathExpr*>(e)) {
            u8(ExprPath); span(n->m_span); syms(n->m_segments); generic_args(n->m_generic_args);
        } else if (auto* n = dynamic_cast<const UnaryExpr*>(e)) {
            u8(ExprUnary); span(n->m_span); u16(static_cast<uint16_t>(n->m_op)); expr(n->m_operand);
        } else if (auto* n = dynamic_cast<const BinaryExpr*>(e)) {
            u8(ExprBinary); span(n->m_span); u16(static_cast<uint16_t>(n->m_op)); expr(n->m_lhs); expr(n->m_rhs);
        } else if (auto* n = dynamic_cast<const CastExpr*>(e)) {
            u8(ExprCast); span(n->m_span); expr(n->m_operand); type(n->m_target);
            u8(static_cast<uint8_t>(n->m_cast_kind));
        } else if (auto* n = dynamic_cast<const MemberExpr*>(e)) {
            u8(ExprMember); span(n->m_span); expr(n->m_object); sym(n->m_member);
        } else if (auto* n = dynamic_cast<const IndexExpr*>(e)) {
            u8(ExprIndex); span(n->m_span); expr(n->m_base); expr(n->m_index);
        } else if (auto* n = dynamic_cast<const CallExpr*>(e)) {
            u8(ExprCall); span(n->m_span); expr(n->m_callee);
            count(n->m_args.size());
            for (const CallArg& a : n->m_args) { sym(a.m_label); expr(a.m_expr); span(a.m_span); }
        } else if (auto* n = dynamic_cast<const IntrinsicCallExpr*>(e)) {
            u8(ExprIntrinsic); span(n->m_span); sym(n->m_name); exprs(n->m_args);
        } else if (auto* n = dynamic_cast<const StructLiteralExpr*>(e)) {
            u8(ExprStruct); span(n->m_span); type(n->m_type);
            count(n->m_fields.size());
            for (const FieldInit& f : n->m_fields) { sym(f.m_name); expr(f.m_value); span(f.m_span); }
        } else {
            fail(); // unknown expression node: refuse to write a lossy entry
        }
    }

    void stmts(const std::vector<StmtNode*>& v) {
        count(v.size());
        for (const StmtNode* s : v) stmt(s);
    }

    void stmt(const StmtNode* s) {
        if (s == nullptr) { u8(0); return; }
        DepthGuard guard(*this);
        if (guard.too_deep()) return;
        if (auto* n = dynamic_cast<const ReturnStmt*>(s)) {
            u8(StmtReturn); span(n->m_span); expr(n->m_value);
        } else if (auto* n = dynamic_cast<const ExprStmt*>(s)) {
            u8(StmtExpr); span(n->m_span); expr(n->m_expr);
        } else if (auto* n = dynamic_cast<const DeclStmt*>(s)) {
            u8(StmtDecl); span(n->m_span); decl(n->m_decl);
        } else if (auto* n = dynamic_cast<const BlockStmt*>(s)) {
            u8(StmtBlock); span(n->m_span); stmts(n->m_statements);
        } else if (auto* n = dynamic_cast<const IfStmt*>(s)) {
            u8(StmtIf); span(n->m_span); expr(n->m_condition); stmt(n->m_then); stmt(n->m_else);
        } else if (auto* n = dynamic_cast<const WhileStmt*>(s)) {
            u8(StmtWhile); span(n->m_span); expr(n->m_condition); stmt(n->m_body);
        } else if (auto* n = dynamic_cast<const ForStmt*>(s)) {
            u8(StmtFor); span(n->m_span); sym(n->m_variable); expr(n->m_iterable); stmt(n->m_body);
        } else if (dynamic_cast<const BreakStmt*>(s) != nullptr) {
            u8(StmtBreak); span(s->m_span);
        } else if (dynamic_cast<const ContinueStmt*>(s) != nullptr) {
            u8(StmtContinue); span(s->m_span);
        } else if (auto* n = dynamic_cast<const ConditionalStmt*>(s)) {
            u8(StmtConditional); span(n->m_span);
            count(n->m_branches.size());
            for (const ConditionalBranch& b : n->m_branches) { expr(b.m_condition); stmts(b.m_body); }
        } else {
            fail();
        }
    }

    void decls(const std::vector<DeclNode*>& v) {
        count(v.size());
        for (const DeclNode* d : v) decl(d);
    }

    void methods(const std::vector<FunctionDeclNode*>& v) {
        count(v.size());
        for (const FunctionDeclNode* f : v) decl(f);
    }

    void decl(const DeclNode* d) {
        if (d == nullptr) { u8(0); return; }
        DepthGuard guard(*this);
        if (guard.too_deep()) return;

        uint8_t tag = 0;
        if (dynamic_cast<const ConditionalDeclBlock*>(d)) tag = DeclCondBlock;
        else if (dynamic_cast<const ImportDeclNode*>(d)) tag = DeclImport;
        else if (dynamic_cast<const VarDeclNode*>(d)) tag = DeclVar;
        else if (dynamic_cast<const FunctionDeclNode*>(d)) tag = DeclFunction;
        else if (dynamic_cast<const RulesetDeclNode*>(d)) tag = DeclRuleset;
        else if (dynamic_cast<const ClassDeclNode*>(d)) tag = DeclClass;
        else if (dynamic_cast<const BlueprintDeclNode*>(d)) tag = DeclBlueprint;
        else if (dynamic_cast<const EnumDeclNode*>(d)) tag = DeclEnum;
        if (tag == 0) { fail(); return; }

        u8(tag);
        span(d->m_span);
        count(d->m_attributes.size());
        for (const AttributeNode& a : d->m_attributes) {
            sym(a.m_name);
            span(a.m_span);
            count(a.m_args.size());
            for (const AttributeArg& arg : a.m_args) { sym(arg.m_key); raw(arg.m_value); }
        }
        boolean(d->m_is_active);
        sym(d->m_module);
        // DeclNode::m_file is deliberately not stored: it names the path the file has *now*, and the
        // driver assigns it after loading (a renamed file with identical content still hits the cache).

        switch (tag) {
        case DeclCondBlock: {
            const auto* n = static_cast<const ConditionalDeclBlock*>(d);
            count(n->m_branches.size());
            for (const ConditionalDeclBranch& b : n->m_branches) { expr(b.m_condition); decls(b.m_declarations); }
            break;
        }
        case DeclImport: {
            const auto* n = static_cast<const ImportDeclNode*>(d);
            sym(n->m_module_path);
            count(n->m_template_args.size());
            for (const ImportArg& a : n->m_template_args) { sym(a.m_name); expr(a.m_value); span(a.m_span); }
            break;
        }
        case DeclVar: {
            const auto* n = static_cast<const VarDeclNode*>(d);
            sym(n->m_name); type(n->m_type); expr(n->m_initializer); u8(static_cast<uint8_t>(n->m_kind));
            break;
        }
        case DeclFunction: {
            const auto* n = static_cast<const FunctionDeclNode*>(d);
            sym(n->m_name); sym(n->m_owner); generic_args(n->m_owner_args); generic_params(n->m_type_params);
            boolean(n->m_is_async); boolean(n->m_has_body);
            u8(static_cast<uint8_t>(n->m_receiver.m_kind)); sym(n->m_receiver.m_self_name);
            parameters(n->m_params);
            type(n->m_return_type);
            stmts(n->m_body);
            break;
        }
        case DeclRuleset: {
            const auto* n = static_cast<const RulesetDeclNode*>(d);
            sym(n->m_name); generic_params(n->m_type_params); methods(n->m_methods);
            break;
        }
        case DeclClass: {
            const auto* n = static_cast<const ClassDeclNode*>(d);
            sym(n->m_name); boolean(n->m_is_struct); generic_params(n->m_type_params);
            parameters(n->m_fields); methods(n->m_methods);
            break;
        }
        case DeclBlueprint: {
            const auto* n = static_cast<const BlueprintDeclNode*>(d);
            sym(n->m_name); generic_params(n->m_type_params); decls(n->m_declarations);
            break;
        }
        case DeclEnum: {
            const auto* n = static_cast<const EnumDeclNode*>(d);
            sym(n->m_name); generic_params(n->m_type_params);
            count(n->m_variants.size());
            for (const EnumVariant& v : n->m_variants) {
                sym(v.m_name);
                count(v.m_payload.size());
                for (const TypeNode* t : v.m_payload) type(t);
                span(v.m_span);
            }
            break;
        }
        default:
            fail();
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------------------------

class Reader {
    // 1. Member variables
    const uint8_t* m_pos;
    const uint8_t* m_end;
    Arena& m_arena;
    std::vector<SymbolID> m_ids;            ///< string table entry -> interned id
    std::vector<std::string_view> m_views;  ///< string table entry -> interned text
    bool m_ok{true};
    int m_depth{0};

public:
    // 2. Constructors and member functions
    Reader(std::span<const uint8_t> bytes, Arena& arena)
        : m_pos(bytes.data()), m_end(bytes.data() + bytes.size()), m_arena(arena) {}

    ModuleAST* run() {
        if (!read_string_table()) return nullptr;
        auto* m = m_arena.create<ModuleAST>();
        m->m_module_name = sym();
        m->m_span = span();
        uint32_t n = count(1);
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            ModuleParam p;
            p.m_name = sym();
            p.m_default_value = expr();
            m->m_parameters.push_back(p);
        }
        m->m_module_sections = syms();
        m->m_declarations = decls();
        if (!m_ok || m_pos != m_end) return nullptr; // trailing garbage is as bad as truncation
        return m;
    }

private:
    [[nodiscard]] size_t remaining() const { return static_cast<size_t>(m_end - m_pos); }
    void fail() { m_ok = false; m_pos = m_end; }

    uint8_t u8() {
        if (remaining() < 1) { fail(); return 0; }
        return *m_pos++;
    }
    uint16_t u16() {
        if (remaining() < 2) { fail(); return 0; }
        uint16_t v = static_cast<uint16_t>(m_pos[0] | (m_pos[1] << 8));
        m_pos += 2;
        return v;
    }
    uint32_t u32() {
        if (remaining() < 4) { fail(); return 0; }
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(m_pos[i]) << (8 * i);
        m_pos += 4;
        return v;
    }
    uint64_t u64() {
        if (remaining() < 8) { fail(); return 0; }
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(m_pos[i]) << (8 * i);
        m_pos += 8;
        return v;
    }
    bool boolean() {
        uint8_t b = u8();
        if (b > 1) fail();
        return b == 1;
    }
    /// Element count; each element takes at least `min_bytes`, which bounds allocations by the input size.
    uint32_t count(size_t min_bytes) {
        uint32_t n = u32();
        if (!m_ok) return 0;
        if (min_bytes != 0 && n > remaining() / min_bytes) { fail(); return 0; }
        return n;
    }
    template <typename E>
    E enum8(uint8_t max_value) {
        uint8_t v = u8();
        if (v > max_value) fail();
        return static_cast<E>(m_ok ? v : 0);
    }

    bool read_string_table() {
        uint32_t n = count(4);
        m_ids.reserve(n);
        m_views.reserve(n);
        auto& interner = StringInterner::get();
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            uint32_t length = u32();
            if (!m_ok || length > remaining()) { fail(); break; }
            SymbolID id = interner.intern(std::string_view(reinterpret_cast<const char*>(m_pos), length));
            m_pos += length;
            m_ids.push_back(id);
            m_views.push_back(interner.resolve(id));
        }
        return m_ok;
    }

    SymbolID sym() {
        uint32_t index = u32();
        if (!m_ok || index == 0) return INVALID_SYMBOL_ID;
        if (index > m_ids.size()) { fail(); return INVALID_SYMBOL_ID; }
        return m_ids[index - 1];
    }
    std::string_view raw() {
        uint32_t index = u32();
        if (!m_ok || index == 0 || index > m_views.size()) { fail(); return {}; }
        return m_views[index - 1];
    }
    std::vector<SymbolID> syms() {
        std::vector<SymbolID> out;
        uint32_t n = count(4);
        for (uint32_t i = 0; i < n && m_ok; ++i) out.push_back(sym());
        return out;
    }
    SourceSpan span() {
        SourceSpan s;
        s.m_start.m_line = u32(); s.m_start.m_column = u32(); s.m_start.m_offset = u32();
        s.m_end.m_line = u32(); s.m_end.m_column = u32(); s.m_end.m_offset = u32();
        return s;
    }

    struct DepthGuard {
        Reader& m_reader;
        explicit DepthGuard(Reader& r) : m_reader(r) { if (++m_reader.m_depth > MAX_DEPTH) m_reader.fail(); }
        ~DepthGuard() { --m_reader.m_depth; }
        [[nodiscard]] bool too_deep() const { return m_reader.m_depth > MAX_DEPTH; }
    };

    std::vector<GenericArg> generic_args() {
        std::vector<GenericArg> out;
        uint32_t n = count(6);
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            GenericArg a;
            a.m_name = sym();
            a.m_type = type();
            a.m_value = expr();
            out.push_back(a);
        }
        return out;
    }

    std::vector<GenericParam> generic_params() {
        std::vector<GenericParam> out;
        uint32_t n = count(6);
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            GenericParam p;
            p.m_name = sym();
            p.m_constraint = type();
            p.m_default = expr();
            out.push_back(p);
        }
        return out;
    }

    std::vector<ParameterDecl> parameters() {
        std::vector<ParameterDecl> out;
        uint32_t n = count(7);
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            ParameterDecl p;
            p.m_name = sym();
            p.m_type = type();
            p.m_default_value = expr();
            p.m_span = span();
            out.push_back(p);
        }
        return out;
    }

    TypeNode* type() {
        uint8_t tag = u8();
        if (!m_ok || tag == 0) return nullptr;
        if (tag != 1) { fail(); return nullptr; }
        DepthGuard guard(*this);
        if (guard.too_deep()) return nullptr;
        auto* t = m_arena.create<TypeNode>();
        t->m_span = span();
        t->m_kind = enum8<TypeNodeKind>(static_cast<uint8_t>(TypeNodeKind::Never));
        t->m_is_const = boolean();
        t->m_path = syms();
        t->m_generic_args = generic_args();
        t->m_inner = type();
        return m_ok ? t : nullptr;
    }

    std::vector<ExprNode*> exprs() {
        std::vector<ExprNode*> out;
        uint32_t n = count(1);
        for (uint32_t i = 0; i < n && m_ok; ++i) out.push_back(expr());
        return out;
    }

    ExprNode* expr() {
        uint8_t tag = u8();
        if (!m_ok || tag == ExprNull) return nullptr;
        DepthGuard guard(*this);
        if (guard.too_deep()) return nullptr;
        ExprNode* result = nullptr;
        SourceSpan sp;
        switch (tag) {
        case ExprInt: { sp = span(); result = m_arena.create<IntegerLiteralExpr>(static_cast<int64_t>(u64())); break; }
        case ExprFloat: { sp = span(); result = m_arena.create<FloatLiteralExpr>(std::bit_cast<double>(u64())); break; }
        case ExprString: { sp = span(); result = m_arena.create<StringLiteralExpr>(raw()); break; }
        case ExprBool: { sp = span(); result = m_arena.create<BooleanLiteralExpr>(boolean()); break; }
        case ExprIdent: { sp = span(); result = m_arena.create<IdentifierExpr>(sym()); break; }
        case ExprPath: {
            sp = span();
            auto* n = m_arena.create<PathExpr>();
            n->m_segments = syms();
            n->m_generic_args = generic_args();
            result = n;
            break;
        }
        case ExprUnary: {
            sp = span();
            auto op = static_cast<TokenKind>(u16());
            result = m_arena.create<UnaryExpr>(op, expr());
            break;
        }
        case ExprBinary: {
            sp = span();
            auto op = static_cast<TokenKind>(u16());
            ExprNode* lhs = expr();
            ExprNode* rhs = expr();
            result = m_arena.create<BinaryExpr>(op, lhs, rhs);
            break;
        }
        case ExprCast: {
            sp = span();
            ExprNode* operand = expr();
            TypeNode* target = type();
            auto kind = enum8<CastKind>(static_cast<uint8_t>(CastKind::Bit));
            result = m_arena.create<CastExpr>(operand, target, kind);
            break;
        }
        case ExprMember: {
            sp = span();
            ExprNode* object = expr();
            result = m_arena.create<MemberExpr>(object, sym());
            break;
        }
        case ExprIndex: {
            sp = span();
            ExprNode* base = expr();
            ExprNode* index = expr();
            result = m_arena.create<IndexExpr>(base, index);
            break;
        }
        case ExprCall: {
            sp = span();
            auto* n = m_arena.create<CallExpr>();
            n->m_callee = expr();
            uint32_t args = count(9);
            for (uint32_t i = 0; i < args && m_ok; ++i) {
                CallArg a;
                a.m_label = sym();
                a.m_expr = expr();
                a.m_span = span();
                n->m_args.push_back(a);
            }
            result = n;
            break;
        }
        case ExprIntrinsic: {
            sp = span();
            auto* n = m_arena.create<IntrinsicCallExpr>();
            n->m_name = sym();
            n->m_args = exprs();
            result = n;
            break;
        }
        case ExprStruct: {
            sp = span();
            auto* n = m_arena.create<StructLiteralExpr>();
            n->m_type = type();
            uint32_t fields = count(9);
            for (uint32_t i = 0; i < fields && m_ok; ++i) {
                FieldInit f;
                f.m_name = sym();
                f.m_value = expr();
                f.m_span = span();
                n->m_fields.push_back(f);
            }
            result = n;
            break;
        }
        default:
            fail();
        }
        if (!m_ok || result == nullptr) return nullptr;
        result->m_span = sp;
        return result;
    }

    std::vector<StmtNode*> stmts() {
        std::vector<StmtNode*> out;
        uint32_t n = count(1);
        for (uint32_t i = 0; i < n && m_ok; ++i) out.push_back(stmt());
        return out;
    }

    BlockStmt* block() {
        StmtNode* s = stmt();
        if (s == nullptr) return nullptr;
        auto* b = dynamic_cast<BlockStmt*>(s);
        if (b == nullptr) fail();
        return b;
    }

    StmtNode* stmt() {
        uint8_t tag = u8();
        if (!m_ok || tag == 0) return nullptr;
        DepthGuard guard(*this);
        if (guard.too_deep()) return nullptr;
        StmtNode* result = nullptr;
        SourceSpan sp = span();
        switch (tag) {
        case StmtReturn: result = m_arena.create<ReturnStmt>(expr()); break;
        case StmtExpr: result = m_arena.create<ExprStmt>(expr()); break;
        case StmtDecl: result = m_arena.create<DeclStmt>(decl()); break;
        case StmtBlock: {
            auto* n = m_arena.create<BlockStmt>();
            n->m_statements = stmts();
            result = n;
            break;
        }
        case StmtIf: {
            auto* n = m_arena.create<IfStmt>();
            n->m_condition = expr();
            n->m_then = block();
            n->m_else = stmt();
            result = n;
            break;
        }
        case StmtWhile: {
            auto* n = m_arena.create<WhileStmt>();
            n->m_condition = expr();
            n->m_body = block();
            result = n;
            break;
        }
        case StmtFor: {
            auto* n = m_arena.create<ForStmt>();
            n->m_variable = sym();
            n->m_iterable = expr();
            n->m_body = block();
            result = n;
            break;
        }
        case StmtBreak: result = m_arena.create<BreakStmt>(); break;
        case StmtContinue: result = m_arena.create<ContinueStmt>(); break;
        case StmtConditional: {
            auto* n = m_arena.create<ConditionalStmt>();
            uint32_t branches = count(5);
            for (uint32_t i = 0; i < branches && m_ok; ++i) {
                ConditionalBranch b;
                b.m_condition = expr();
                b.m_body = stmts();
                n->m_branches.push_back(std::move(b));
            }
            result = n;
            break;
        }
        default:
            fail();
        }
        if (!m_ok || result == nullptr) return nullptr;
        result->m_span = sp;
        return result;
    }

    std::vector<DeclNode*> decls() {
        std::vector<DeclNode*> out;
        uint32_t n = count(1);
        for (uint32_t i = 0; i < n && m_ok; ++i) out.push_back(decl());
        return out;
    }

    std::vector<FunctionDeclNode*> methods() {
        std::vector<FunctionDeclNode*> out;
        uint32_t n = count(1);
        for (uint32_t i = 0; i < n && m_ok; ++i) {
            DeclNode* d = decl();
            auto* f = dynamic_cast<FunctionDeclNode*>(d);
            if (f == nullptr) { fail(); break; }
            out.push_back(f);
        }
        return out;
    }

    DeclNode* decl() {
        uint8_t tag = u8();
        if (!m_ok || tag == 0) return nullptr;
        DepthGuard guard(*this);
        if (guard.too_deep()) return nullptr;

        DeclNode* d = nullptr;
        switch (tag) {
        case DeclCondBlock: d = m_arena.create<ConditionalDeclBlock>(); break;
        case DeclImport: d = m_arena.create<ImportDeclNode>(); break;
        case DeclVar: d = m_arena.create<VarDeclNode>(); break;
        case DeclFunction: d = m_arena.create<FunctionDeclNode>(); break;
        case DeclRuleset: d = m_arena.create<RulesetDeclNode>(); break;
        case DeclClass: d = m_arena.create<ClassDeclNode>(); break;
        case DeclBlueprint: d = m_arena.create<BlueprintDeclNode>(); break;
        case DeclEnum: d = m_arena.create<EnumDeclNode>(); break;
        default: fail(); return nullptr;
        }

        d->m_span = span();
        uint32_t attrs = count(12);
        for (uint32_t i = 0; i < attrs && m_ok; ++i) {
            AttributeNode a;
            a.m_name = sym();
            a.m_span = span();
            uint32_t args = count(8);
            for (uint32_t j = 0; j < args && m_ok; ++j) {
                SymbolID key = sym();
                a.m_args.emplace_back(key, raw());
            }
            d->m_attributes.push_back(std::move(a));
        }
        d->m_is_active = boolean();
        d->m_module = sym();

        switch (tag) {
        case DeclCondBlock: {
            auto* n = static_cast<ConditionalDeclBlock*>(d);
            uint32_t branches = count(5);
            for (uint32_t i = 0; i < branches && m_ok; ++i) {
                ConditionalDeclBranch b;
                b.m_condition = expr();
                b.m_declarations = decls();
                n->m_branches.push_back(std::move(b));
            }
            break;
        }
        case DeclImport: {
            auto* n = static_cast<ImportDeclNode*>(d);
            n->m_module_path = sym();
            uint32_t args = count(9);
            for (uint32_t i = 0; i < args && m_ok; ++i) {
                ImportArg a;
                a.m_name = sym();
                a.m_value = expr();
                a.m_span = span();
                n->m_template_args.push_back(a);
            }
            break;
        }
        case DeclVar: {
            auto* n = static_cast<VarDeclNode*>(d);
            n->m_name = sym();
            n->m_type = type();
            n->m_initializer = expr();
            n->m_kind = enum8<BindingKind>(static_cast<uint8_t>(BindingKind::Constx));
            break;
        }
        case DeclFunction: {
            auto* n = static_cast<FunctionDeclNode*>(d);
            n->m_name = sym();
            n->m_owner = sym();
            n->m_owner_args = generic_args();
            n->m_type_params = generic_params();
            n->m_is_async = boolean();
            n->m_has_body = boolean();
            n->m_receiver.m_kind = enum8<ReceiverKind>(static_cast<uint8_t>(ReceiverKind::ByMutRef));
            n->m_receiver.m_self_name = sym();
            n->m_params = parameters();
            n->m_return_type = type();
            n->m_body = stmts();
            break;
        }
        case DeclRuleset: {
            auto* n = static_cast<RulesetDeclNode*>(d);
            n->m_name = sym();
            n->m_type_params = generic_params();
            n->m_methods = methods();
            break;
        }
        case DeclClass: {
            auto* n = static_cast<ClassDeclNode*>(d);
            n->m_name = sym();
            n->m_is_struct = boolean();
            n->m_type_params = generic_params();
            n->m_fields = parameters();
            n->m_methods = methods();
            break;
        }
        case DeclBlueprint: {
            auto* n = static_cast<BlueprintDeclNode*>(d);
            n->m_name = sym();
            n->m_type_params = generic_params();
            n->m_declarations = decls();
            break;
        }
        case DeclEnum: {
            auto* n = static_cast<EnumDeclNode*>(d);
            n->m_name = sym();
            n->m_type_params = generic_params();
            uint32_t variants = count(12);
            for (uint32_t i = 0; i < variants && m_ok; ++i) {
                EnumVariant v;
                v.m_name = sym();
                uint32_t payload = count(1);
                for (uint32_t j = 0; j < payload && m_ok; ++j) v.m_payload.push_back(type());
                v.m_span = span();
                n->m_variants.push_back(std::move(v));
            }
            break;
        }
        default:
            fail();
        }
        return m_ok ? d : nullptr;
    }
};

} // namespace

std::optional<std::vector<uint8_t>> encode_module(const ModuleAST& module) {
    Writer writer;
    writer.write_module(module);
    if (!writer.ok()) return std::nullopt;
    return writer.finish();
}

ModuleAST* decode_module(std::span<const uint8_t> payload, Arena& arena) {
    Reader reader(payload, arena);
    return reader.run();
}

} // namespace fool
