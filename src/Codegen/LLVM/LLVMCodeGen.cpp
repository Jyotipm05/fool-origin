//
// LLVM backend: MIR -> textual LLVM IR.
//

/**
 * @file LLVMCodeGen.cpp
 * @brief The emitter behind `--emit-llvm` and `--build`; see LLVMCodeGen.ixx for what it covers.
 */

module;

#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

module fool.llvm;

import fool.ast;
import fool.hir;
import fool.lexer;

namespace fool::llvm {

using hir::TypeId;

namespace {

std::string name_of(SymbolID id) { return id == INVALID_SYMBOL_ID ? std::string() : std::string(StringInterner::get().resolve(id)); }

/// Thrown inside one function when it uses something the backend does not emit yet; the function is skipped.
struct Unsupported : std::runtime_error {
    explicit Unsupported(const std::string& what) : std::runtime_error(what) {}
};

/// A name in quotes, with the characters LLVM needs escaped: `@"main.identity<i32>"`.
std::string quoted(const std::string& text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\' || c < 0x20 || c >= 0x7f) {
            char escape[8];
            std::snprintf(escape, sizeof escape, "\\%02X", static_cast<unsigned>(c));
            out += escape;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

/// LLVM spells a floating-point constant as the hex of its double-precision bits (a `float` constant is first rounded to float).
std::string float_constant(double value, unsigned bits) {
    if (bits == 32) value = static_cast<double>(static_cast<float>(value));
    char text[32];
    std::snprintf(text, sizeof text, "0x%016llX", static_cast<unsigned long long>(std::bit_cast<uint64_t>(value)));
    return text;
}

bool has_attribute(const FunctionDeclNode* fn, const char* name) {
    for (const AttributeNode& attribute : fn->m_attributes) {
        if (name_of(attribute.m_name) == name) return true;
    }
    return false;
}

/// `link_name` of an `@extern("C", link_name = "puts")`, else the function's own name.
std::string extern_symbol(const FunctionDeclNode* fn) {
    for (const AttributeNode& attribute : fn->m_attributes) {
        if (name_of(attribute.m_name) != "extern") continue;
        for (const AttributeArg& arg : attribute.m_args) {
            if (name_of(arg.m_key) == "link_name") return std::string(arg.m_value);
        }
    }
    return name_of(fn->m_name);
}

class Emitter {
    // 1. Member variables
    const mir::Module& m_module;
    const EmitOptions& m_options;
    EmitResult m_result;
    std::map<std::string, std::string> m_externs;   ///< symbol -> `declare ...` line (sorted, so the output is stable)
    std::map<std::string, std::string> m_extern_signature;

    // State of the function being emitted
    const mir::Function* m_fn{nullptr};
    std::string m_body;
    unsigned m_next_temp{0};

public:
    // 2. Constructors
    Emitter(const mir::Module& module, const EmitOptions& options) : m_module(module), m_options(options) {}

    // 3. Member functions
    EmitResult run() {
        std::string functions;
        for (const mir::Function& fn : m_module.m_functions) {
            if (!fn.m_lowered) {
                m_result.m_skipped.push_back({fn.m_name, fn.m_reason});
                continue;
            }
            try {
                functions += emit_function(fn);
                ++m_result.m_emitted;
            } catch (const Unsupported& problem) {
                m_result.m_skipped.push_back({fn.m_name, problem.what()});
            }
        }

        std::string out = "; ModuleID = '" + m_options.m_module_name + "'\n";
        out += "source_filename = " + quoted(m_options.m_module_name) + "\n";
        if (!m_options.m_target_triple.empty()) out += "target triple = " + quoted(m_options.m_target_triple) + "\n";
        for (const SkippedFunction& skipped : m_result.m_skipped) {
            out += "; not emitted: " + one_line(skipped.m_name) + ": " + one_line(skipped.m_reason) + "\n";
        }
        out += "\n";
        for (const auto& [symbol, line] : m_externs) out += line + "\n";
        if (!m_externs.empty()) out += "\n";
        out += functions;
        m_result.m_ir = std::move(out);
        return std::move(m_result);
    }

private:
    static std::string one_line(std::string text) {
        for (char& c : text) {
            if (c == '\n' || c == '\r') c = ' ';
        }
        return text;
    }

    // ---- names ----------------------------------------------------------------------------------

    /// The symbol of a Fool function: `<module>.<name>`.
    [[nodiscard]] std::string symbol_of(const FunctionDeclNode* decl, const std::string& display) const {
        std::string module = decl != nullptr ? name_of(decl->m_module) : std::string();
        if (module.empty()) module = m_options.m_module_name;
        return "@" + quoted(module + "." + display);
    }

    [[nodiscard]] static bool is_entry(const mir::Function& fn) {
        return fn.m_name == "main" && fn.m_param_count == 0 && fn.m_decl != nullptr && fn.m_decl->m_owner == INVALID_SYMBOL_ID;
    }

    // ---- types ----------------------------------------------------------------------------------

    [[nodiscard]] std::string type(TypeId id) const {
        const hir::Type& t = m_module.m_types.get(id);
        switch (t.m_kind) {
            case hir::TypeKind::Void:
            case hir::TypeKind::Never: return "void";
            case hir::TypeKind::Bool: return "i1";
            case hir::TypeKind::Int: return "i" + std::to_string(t.m_bits);
            case hir::TypeKind::Float:
                if (t.m_bits == 32) return "float";
                if (t.m_bits == 64) return "double";
                break;
            case hir::TypeKind::Pointer:
            case hir::TypeKind::Reference: return "ptr";
            case hir::TypeKind::Named:
            case hir::TypeKind::Optional:
            case hir::TypeKind::Function: break;
        }
        throw Unsupported("the type '" + m_module.m_types.text(id) + "' (only integers, floats, bools and pointers are emitted so far)");
    }

    [[nodiscard]] const hir::Type& info(TypeId id) const { return m_module.m_types.get(id); }
    [[nodiscard]] bool is_float(TypeId id) const { return info(id).m_kind == hir::TypeKind::Float; }
    [[nodiscard]] bool is_int(TypeId id) const { return info(id).m_kind == hir::TypeKind::Int; }
    [[nodiscard]] bool is_bool(TypeId id) const { return info(id).m_kind == hir::TypeKind::Bool; }
    [[nodiscard]] bool is_signed(TypeId id) const { return info(id).m_signed; }
    [[nodiscard]] bool is_view(TypeId id) const {
        const hir::TypeKind kind = info(id).m_kind;
        return kind == hir::TypeKind::Pointer || kind == hir::TypeKind::Reference;
    }
    [[nodiscard]] bool is_void(TypeId id) const {
        const hir::TypeKind kind = info(id).m_kind;
        return kind == hir::TypeKind::Void || kind == hir::TypeKind::Never;
    }

    // ---- emission helpers -----------------------------------------------------------------------

    std::string temp() { return "%t" + std::to_string(m_next_temp++); }

    void line(const std::string& text) { m_body += "  " + text + "\n"; }

    static std::string local_slot(mir::LocalId id) { return "%l" + std::to_string(id); }
    static std::string block_label(mir::BlockId id) { return "bb" + std::to_string(id); }

    /// The address of a place: its local's slot, followed through the place's projections.
    std::string address(const mir::Place& place) {
        if (place.is_global()) throw Unsupported("a global variable");
        std::string current = local_slot(place.m_local);
        for (const mir::Projection& projection : place.m_projections) {
            switch (projection.m_kind) {
                case mir::ProjectionKind::Deref: {
                    const std::string loaded = temp();
                    line(loaded + " = load ptr, ptr " + current);
                    current = loaded;
                    break;
                }
                case mir::ProjectionKind::Index: {
                    const std::string base = temp();
                    line(base + " = load ptr, ptr " + current);
                    const TypeId index_type = m_fn->m_locals[projection.m_local].m_type;
                    const std::string index = temp();
                    line(index + " = load " + type(index_type) + ", ptr " + local_slot(projection.m_local));
                    const std::string element = temp();
                    line(element + " = getelementptr " + type(projection.m_type) + ", ptr " + base + ", " + type(index_type) + " " + index);
                    current = element;
                    break;
                }
                case mir::ProjectionKind::Field: throw Unsupported("a field access (classes are not emitted yet)");
                case mir::ProjectionKind::Payload: throw Unsupported("an enum payload (enums are not emitted yet)");
            }
        }
        return current;
    }

    /// The value of an operand as an LLVM value text (a constant, or a temporary holding a load).
    std::string value(const mir::Operand& operand) {
        switch (operand.m_kind) {
            case mir::OperandKind::Int: return std::to_string(operand.m_int);
            case mir::OperandKind::Bool: return operand.m_int != 0 ? "true" : "false";
            case mir::OperandKind::Float: return float_constant(operand.m_float, info(operand.m_type).m_bits);
            case mir::OperandKind::Read: {
                const std::string slot = address(operand.m_place);
                const std::string loaded = temp();
                line(loaded + " = load " + type(operand.m_place.type()) + ", ptr " + slot);
                return loaded;
            }
            case mir::OperandKind::Null: throw Unsupported("a null optional");
            case mir::OperandKind::Str: throw Unsupported("a string literal");
            case mir::OperandKind::Unit: throw Unsupported("the value of a call that returns nothing");
        }
        throw Unsupported("an operand");
    }

    // ---- rvalues --------------------------------------------------------------------------------

    struct Value {
        std::string m_text;   ///< the LLVM value, or empty for a call to a function that returns nothing
    };

    Value rvalue(const mir::Rvalue& rv) {
        switch (rv.m_kind) {
            case mir::RvalueKind::Use: return {value(rv.m_operands[0])};
            case mir::RvalueKind::Unary: return unary(rv);
            case mir::RvalueKind::Binary: return binary(rv);
            case mir::RvalueKind::Convert:
            case mir::RvalueKind::Cast: return convert(rv);
            case mir::RvalueKind::AddrOf: return {address(rv.m_place)};
            case mir::RvalueKind::Call: return call(rv);
            case mir::RvalueKind::Unwrap:
            case mir::RvalueKind::Wrap: throw Unsupported("an optional value");
            case mir::RvalueKind::Aggregate: throw Unsupported("a class value");
            case mir::RvalueKind::Variant: throw Unsupported("an enum value");
        }
        throw Unsupported("an rvalue");
    }

    Value unary(const mir::Rvalue& rv) {
        const mir::Operand& operand = rv.m_operands[0];
        const std::string v = value(operand);
        const std::string result = temp();
        if (rv.m_op == TokenKind::Bang) {
            line(result + " = xor i1 " + v + ", true");
        } else if (is_float(operand.m_type)) {
            line(result + " = fneg " + type(operand.m_type) + " " + v);
        } else if (is_int(operand.m_type)) {
            line(result + " = sub " + type(operand.m_type) + " 0, " + v);
        } else {
            throw Unsupported("a unary operator on '" + m_module.m_types.text(operand.m_type) + "'");
        }
        return {result};
    }

    Value binary(const mir::Rvalue& rv) {
        const mir::Operand& lhs = rv.m_operands[0];
        const TypeId operand_type = lhs.m_type;
        const std::string a = value(lhs);
        const std::string b = value(rv.m_operands[1]);
        const std::string t = type(operand_type);
        const std::string result = temp();
        const bool floating = is_float(operand_type);
        const bool integer = is_int(operand_type);
        const bool sign = is_signed(operand_type);
        if (!floating && !integer && !is_bool(operand_type) && !is_view(operand_type)) {
            throw Unsupported("a binary operator on '" + m_module.m_types.text(operand_type) + "'");
        }
        const auto arithmetic = [&](const char* int_op, const char* float_op) {
            if (floating) line(result + " = " + float_op + " " + t + " " + a + ", " + b);
            else if (integer) line(result + " = " + int_op + " " + t + " " + a + ", " + b);
            else throw Unsupported("arithmetic on '" + m_module.m_types.text(operand_type) + "'");
        };
        const auto bitwise = [&](const char* op) {
            if (!integer && !is_bool(operand_type)) throw Unsupported("a bitwise operator on '" + m_module.m_types.text(operand_type) + "'");
            line(result + " = " + op + " " + t + " " + a + ", " + b);
        };
        const auto compare = [&](const char* signed_pred, const char* unsigned_pred, const char* float_pred) {
            if (floating) line(result + " = fcmp " + float_pred + " " + t + " " + a + ", " + b);
            else line(result + " = icmp " + (sign ? signed_pred : unsigned_pred) + " " + t + " " + a + ", " + b);
        };
        switch (rv.m_op) {
            case TokenKind::Plus: arithmetic("add", "fadd"); break;
            case TokenKind::Minus: arithmetic("sub", "fsub"); break;
            case TokenKind::Star: arithmetic("mul", "fmul"); break;
            case TokenKind::Slash: arithmetic(sign ? "sdiv" : "udiv", "fdiv"); break;
            case TokenKind::Percent: arithmetic(sign ? "srem" : "urem", "frem"); break;
            case TokenKind::Ampersand: bitwise("and"); break;
            case TokenKind::Pipe: bitwise("or"); break;
            case TokenKind::Caret: bitwise("xor"); break;
            case TokenKind::EqualEqual: compare("eq", "eq", "oeq"); break;
            case TokenKind::ExclaimEqual: compare("ne", "ne", "une"); break;
            case TokenKind::Less: compare("slt", "ult", "olt"); break;
            case TokenKind::LessEqual: compare("sle", "ule", "ole"); break;
            case TokenKind::Greater: compare("sgt", "ugt", "ogt"); break;
            case TokenKind::GreaterEqual: compare("sge", "uge", "oge"); break;
            default: throw Unsupported("a binary operator");
        }
        return {result};
    }

    /// An implicit conversion or a cast between integers, floats, bools and pointers.
    Value convert(const mir::Rvalue& rv) {
        const mir::Operand& operand = rv.m_operands[0];
        const TypeId from = operand.m_type;
        const TypeId to = rv.m_type;
        if (rv.m_kind == mir::RvalueKind::Cast && (rv.m_cast == CastKind::Dynamic || rv.m_cast == CastKind::Bit)) {
            throw Unsupported("a dynamic or bit cast");
        }
        if (from == to) return {value(operand)};
        const std::string v = value(operand);
        const std::string src = type(from);
        const std::string dst = type(to);
        const std::string result = temp();
        const hir::Type& f = info(from);
        const hir::Type& t = info(to);
        const auto source_is_int = f.m_kind == hir::TypeKind::Int || f.m_kind == hir::TypeKind::Bool;
        if (is_view(from) && is_view(to)) return {v};   // opaque pointers: nothing to convert
        if (source_is_int && t.m_kind == hir::TypeKind::Int) {
            const unsigned from_bits = f.m_kind == hir::TypeKind::Bool ? 1 : f.m_bits;
            if (from_bits == t.m_bits) return {v};
            if (from_bits > t.m_bits) {
                line(result + " = trunc " + src + " " + v + " to " + dst);
            } else {
                line(result + " = " + (f.m_signed && f.m_kind == hir::TypeKind::Int ? "sext " : "zext ") + src + " " + v + " to " + dst);
            }
            return {result};
        }
        if (f.m_kind == hir::TypeKind::Int && t.m_kind == hir::TypeKind::Bool) {
            line(result + " = icmp ne " + src + " " + v + ", 0");
            return {result};
        }
        if (f.m_kind == hir::TypeKind::Bool && t.m_kind == hir::TypeKind::Bool) return {v};
        if (source_is_int && t.m_kind == hir::TypeKind::Float) {
            line(result + " = " + (f.m_kind == hir::TypeKind::Int && f.m_signed ? "sitofp " : "uitofp ") + src + " " + v + " to " + dst);
            return {result};
        }
        if (f.m_kind == hir::TypeKind::Float && t.m_kind == hir::TypeKind::Int) {
            line(result + " = " + (t.m_signed ? "fptosi " : "fptoui ") + src + " " + v + " to " + dst);
            return {result};
        }
        if (f.m_kind == hir::TypeKind::Float && t.m_kind == hir::TypeKind::Float) {
            line(result + " = " + (f.m_bits < t.m_bits ? "fpext " : "fptrunc ") + src + " " + v + " to " + dst);
            return {result};
        }
        if (is_view(from) && t.m_kind == hir::TypeKind::Int) {
            line(result + " = ptrtoint ptr " + v + " to " + dst);
            return {result};
        }
        if (f.m_kind == hir::TypeKind::Int && is_view(to)) {
            line(result + " = inttoptr " + src + " " + v + " to ptr");
            return {result};
        }
        throw Unsupported("a conversion from '" + m_module.m_types.text(from) + "' to '" + m_module.m_types.text(to) + "'");
    }

    // ---- calls ----------------------------------------------------------------------------------

    Value call(const mir::Rvalue& rv) {
        std::string target;
        // A function of this module: same declaration and the same (instance) name.
        for (const mir::Function& candidate : m_module.m_functions) {
            if (candidate.m_lowered && candidate.m_decl == rv.m_callee && candidate.m_name == rv.m_text) {
                target = symbol_of(candidate.m_decl, candidate.m_name);
                break;
            }
        }
        std::vector<std::string> args;
        std::string signature;
        for (size_t i = 0; i < rv.m_operands.size(); ++i) {
            const mir::Operand& operand = rv.m_operands[i];
            args.push_back(type(operand.m_type) + " " + value(operand));
            signature += (i != 0 ? ", " : "") + type(operand.m_type);
        }
        const std::string ret = is_void(rv.m_type) ? "void" : type(rv.m_type);
        if (target.empty()) {
            if (rv.m_callee == nullptr || !has_attribute(rv.m_callee, "extern")) {
                throw Unsupported("a call to '" + rv.m_text + "', which is not a function of this module (calls into other modules are not emitted yet)");
            }
            const std::string symbol = extern_symbol(rv.m_callee);
            const std::string full = ret + " (" + signature + ")";
            const auto [at, inserted] = m_extern_signature.emplace(symbol, full);
            if (!inserted && at->second != full) {
                m_result.m_problems.push_back("'" + symbol + "' is called with two different signatures: " + at->second + " and " + full);
            }
            m_externs[symbol] = "declare " + ret + " @" + quoted(symbol) + "(" + signature + ")";
            target = "@" + quoted(symbol);
        }
        std::string joined;
        for (size_t i = 0; i < args.size(); ++i) joined += (i != 0 ? ", " : "") + args[i];
        if (ret == "void") {
            line("call void " + target + "(" + joined + ")");
            return {};
        }
        const std::string result = temp();
        line(result + " = call " + ret + " " + target + "(" + joined + ")");
        return {result};
    }

    // ---- statements, terminators, functions -----------------------------------------------------

    void statement(const mir::Statement& s) {
        if (s.m_kind == mir::StatementKind::Eval) {
            (void)rvalue(s.m_value);
            return;
        }
        // Compute the value first, then the destination's address: both only read, and this keeps loads before the store.
        const Value v = rvalue(s.m_value);
        const std::string destination = address(s.m_place);
        const TypeId place_type = s.m_place.type();
        if (v.m_text.empty()) throw Unsupported("storing the value of a call that returns nothing");
        line("store " + type(place_type) + " " + v.m_text + ", ptr " + destination);
    }

    void terminator(const mir::Terminator& t) {
        switch (t.m_kind) {
            case mir::TerminatorKind::Goto: line("br label %" + block_label(t.m_target)); break;
            case mir::TerminatorKind::Branch: {
                const std::string condition = value(t.m_value);
                line("br i1 " + condition + ", label %" + block_label(t.m_target) + ", label %" + block_label(t.m_else));
                break;
            }
            case mir::TerminatorKind::Return:
                if (t.m_has_value && !is_void(m_fn->m_return) && t.m_value.m_kind != mir::OperandKind::Unit) {
                    const std::string v = value(t.m_value);
                    line("ret " + type(m_fn->m_return) + " " + v);
                } else {
                    line("ret void");
                }
                break;
            case mir::TerminatorKind::Switch: throw Unsupported("a match (enums are not emitted yet)");
            case mir::TerminatorKind::Unreachable: line("unreachable"); break;
        }
    }

    std::string emit_function(const mir::Function& fn) {
        m_fn = &fn;
        m_body.clear();
        m_next_temp = 0;

        std::string params;
        for (size_t i = 0; i < fn.m_param_count; ++i) {
            params += (i != 0 ? ", " : "") + type(fn.m_locals[i].m_type) + " %p" + std::to_string(i);
        }
        const std::string ret = is_void(fn.m_return) ? "void" : type(fn.m_return);
        const std::string symbol = symbol_of(fn.m_decl, fn.m_name);

        m_body += "entry:\n";
        for (size_t i = 0; i < fn.m_locals.size(); ++i) {
            if (is_void(fn.m_locals[i].m_type)) continue;
            line(local_slot(static_cast<mir::LocalId>(i)) + " = alloca " + type(fn.m_locals[i].m_type));
        }
        for (size_t i = 0; i < fn.m_param_count; ++i) {
            line("store " + type(fn.m_locals[i].m_type) + " %p" + std::to_string(i) + ", ptr " + local_slot(static_cast<mir::LocalId>(i)));
        }
        line("br label %" + block_label(0));
        for (size_t b = 0; b < fn.m_blocks.size(); ++b) {
            m_body += block_label(static_cast<mir::BlockId>(b)) + ":\n";
            for (const mir::Statement& s : fn.m_blocks[b].m_statements) statement(s);
            terminator(fn.m_blocks[b].m_terminator);
        }

        std::string out = "define " + ret + " " + symbol + "(" + params + ") {\n" + m_body + "}\n\n";
        if (is_entry(fn)) out += entry_wrapper(fn, symbol);
        return out;
    }

    /// The C `main` that runs a Fool `fn main`: its integer result is the exit status; a function returning nothing exits with 0.
    std::string entry_wrapper(const mir::Function& fn, const std::string& symbol) {
        const std::string ret = is_void(fn.m_return) ? "void" : type(fn.m_return);
        if (ret != "void" && ret != "i32") {
            m_result.m_problems.push_back("'main' must return nothing or i32, not '" + m_module.m_types.text(fn.m_return) + "'");
            return {};
        }
        m_result.m_has_main = true;
        std::string out = "define i32 @main() {\nentry:\n";
        if (ret == "void") {
            out += "  call void " + symbol + "()\n  ret i32 0\n";
        } else {
            out += "  %status = call i32 " + symbol + "()\n  ret i32 %status\n";
        }
        return out + "}\n\n";
    }
};

}  // namespace

EmitResult emit(const mir::Module& module, const EmitOptions& options) {
    return Emitter(module, options).run();
}

}  // namespace fool::llvm
