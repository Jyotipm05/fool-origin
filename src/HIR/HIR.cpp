//
// HIR type table and text dump.
//

/**
 * @file HIR.cpp
 * @brief TypeTable, Module helpers and the `--dump-hir` printer.
 *
 * Text form (one function after another):
 *
 *     fn Counter::bump(self: &Counter, by: i32) -> i32 {
 *       let next#2: i32 = (+ (field (deref self#0) .count) by#1);
 *       (= (field (deref self#0) .count) next#2);
 *       return next#2;
 *     }
 *
 * Locals print as `name#index`, literals carry their type (`1:i32`), conversions and casts the target type
 * (`(convert:i64 x#0)`), calls the callee's display name (`(call geometry::dot a#0 b#1)`).
 */

module;

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

module fool.hir;

namespace fool::hir {

namespace {

std::string name_of(SymbolID id) { return std::string(StringInterner::get().resolve(id)); }

const char* cast_name(CastKind kind) {
    switch (kind) {
        case CastKind::Unchecked: return "unchecked";
        case CastKind::Static: return "static";
        case CastKind::Dynamic: return "dynamic";
        case CastKind::Reinterpret: return "reinterpret";
        case CastKind::Bit: return "bit";
    }
    return "unchecked";
}

std::string op_text(TokenKind op, bool unary) {
    switch (op) {
        case TokenKind::Plus: return "+";
        case TokenKind::Minus: return unary ? "neg" : "-";
        case TokenKind::Star: return "*";
        case TokenKind::Slash: return "/";
        case TokenKind::Percent: return "%";
        case TokenKind::Ampersand: return "&";
        case TokenKind::Pipe: return "|";
        case TokenKind::Caret: return "^";
        case TokenKind::EqualEqual: return "==";
        case TokenKind::ExclaimEqual: return "!=";
        case TokenKind::Less: return "<";
        case TokenKind::LessEqual: return "<=";
        case TokenKind::Greater: return ">";
        case TokenKind::GreaterEqual: return ">=";
        case TokenKind::AmpAmp: return "&&";
        case TokenKind::PipePipe: return "||";
        case TokenKind::Bang: return "not";
        default: return "?";
    }
}

class Printer {
    // 1. Member variables
    const Module& m_module;
    const Function* m_fn{nullptr};
    std::string m_out;

public:
    // 2. Constructors
    explicit Printer(const Module& module) : m_module(module) {}

    // 3. Member functions
    std::string run() {
        for (size_t i = 0; i < m_module.m_functions.size(); ++i) {
            if (i != 0) m_out += "\n";
            print_function(m_module.m_functions[i]);
        }
        m_out += "// " + std::to_string(m_module.lowered_count()) + " of " + std::to_string(m_module.m_functions.size()) +
                 " function(s) lowered\n";
        return std::move(m_out);
    }

private:
    std::string local_text(LocalId id) const {
        const Local& local = m_fn->m_locals[id];
        return name_of(local.m_name) + "#" + std::to_string(id);
    }

    std::string expr_text(const Expr& e) const {
        const auto operand = [&](size_t i) { return expr_text(*e.m_operands[i]); };
        const auto type = [&](TypeId id) { return m_module.m_types.text(id); };
        switch (e.m_kind) {
            case ExprKind::IntLit: return std::to_string(e.m_int) + ":" + type(e.m_type);
            case ExprKind::FloatLit: {
                char buffer[64];
                std::snprintf(buffer, sizeof buffer, "%g", e.m_float);
                std::string text = buffer;
                if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
                return text + ":" + type(e.m_type);
            }
            case ExprKind::BoolLit: return e.m_int != 0 ? "true" : "false";
            case ExprKind::StrLit: return e.m_text;
            case ExprKind::NullLit: return "null:" + type(e.m_type);
            case ExprKind::Local: return local_text(e.m_local);
            case ExprKind::Global: return "::" + name_of(e.m_name);
            case ExprKind::Unary: return "(" + op_text(e.m_op, true) + " " + operand(0) + ")";
            case ExprKind::Binary: return "(" + op_text(e.m_op, false) + " " + operand(0) + " " + operand(1) + ")";
            case ExprKind::Assign: return "(= " + operand(0) + " " + operand(1) + ")";
            case ExprKind::Convert: return "(convert:" + type(e.m_type) + " " + operand(0) + ")";
            case ExprKind::Cast: return "(cast." + std::string(cast_name(e.m_cast)) + ":" + type(e.m_type) + " " + operand(0) + ")";
            case ExprKind::Field: return "(field " + operand(0) + " ." + name_of(e.m_name) + ")";
            case ExprKind::Index: return "(index " + operand(0) + " " + operand(1) + ")";
            case ExprKind::AddrOf: return "(addr " + operand(0) + ")";
            case ExprKind::Deref: return "(deref " + operand(0) + ")";
            case ExprKind::Unwrap: return "(unwrap " + operand(0) + ")";
            case ExprKind::Wrap: return "(wrap:" + type(e.m_type) + " " + operand(0) + ")";
            case ExprKind::Call: {
                std::string text = "(call " + e.m_text;
                for (size_t i = 0; i < e.m_operands.size(); ++i) text += " " + operand(i);
                return text + ")";
            }
            case ExprKind::StructLit: {
                std::string text = "(new " + type(e.m_type);
                for (size_t i = 0; i < e.m_operands.size(); ++i) text += " " + operand(i);
                return text + ")";
            }
        }
        return "?";
    }

    void indent(int depth) { m_out.append(static_cast<size_t>(depth) * 2, ' '); }

    void print_block(const std::vector<Stmt>& body, int depth) {
        for (const Stmt& stmt : body) print_stmt(stmt, depth);
    }

    void print_stmt(const Stmt& s, int depth) {
        indent(depth);
        switch (s.m_kind) {
            case StmtKind::Let: {
                const Local& local = m_fn->m_locals[s.m_local];
                m_out += std::string(local.m_mutable ? "let " : "const ") + local_text(s.m_local) + ": " +
                         m_module.m_types.text(local.m_type);
                if (s.m_expr) m_out += " = " + expr_text(*s.m_expr);
                m_out += ";\n";
                break;
            }
            case StmtKind::Expr: m_out += expr_text(*s.m_expr) + ";\n"; break;
            case StmtKind::Return:
                m_out += s.m_expr ? "return " + expr_text(*s.m_expr) + ";\n" : std::string("return;\n");
                break;
            case StmtKind::If:
                m_out += "if " + expr_text(*s.m_expr) + " {\n";
                print_block(s.m_body, depth + 1);
                indent(depth);
                if (s.m_else.empty()) {
                    m_out += "}\n";
                } else {
                    m_out += "} else {\n";
                    print_block(s.m_else, depth + 1);
                    indent(depth);
                    m_out += "}\n";
                }
                break;
            case StmtKind::While:
                m_out += "while " + expr_text(*s.m_expr) + " {\n";
                print_block(s.m_body, depth + 1);
                indent(depth);
                m_out += "}\n";
                break;
            case StmtKind::Break: m_out += "break;\n"; break;
            case StmtKind::Continue: m_out += "continue;\n"; break;
            case StmtKind::Block:
                m_out += "{\n";
                print_block(s.m_body, depth + 1);
                indent(depth);
                m_out += "}\n";
                break;
        }
    }

    void print_function(const Function& fn) {
        m_fn = &fn;
        if (!fn.m_lowered) {
            m_out += "fn " + fn.m_name + ": not lowerable yet: " + fn.m_reason + " (line " +
                     std::to_string(fn.m_reason_span.m_start.m_line) + ")\n";
            return;
        }
        m_out += "fn " + fn.m_name + "(";
        for (size_t i = 0; i < fn.m_param_count; ++i) {
            if (i != 0) m_out += ", ";
            m_out += name_of(fn.m_locals[i].m_name) + ": " + m_module.m_types.text(fn.m_locals[i].m_type);
        }
        m_out += ")";
        if (m_module.m_types.get(fn.m_return).m_kind != TypeKind::Void) m_out += " -> " + m_module.m_types.text(fn.m_return);
        m_out += " {\n";
        print_block(fn.m_body, 1);
        m_out += "}\n";
    }
};

} // namespace

TypeId TypeTable::intern(Type type) {
    for (size_t i = 0; i < m_types.size(); ++i) {
        if (m_types[i] == type) return static_cast<TypeId>(i);
    }
    m_types.push_back(std::move(type));
    return static_cast<TypeId>(m_types.size() - 1);
}

std::string TypeTable::text(TypeId id) const {
    if (id == NO_TYPE) return "<none>";
    const Type& type = get(id);
    switch (type.m_kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Never: return "!";
        case TypeKind::Bool: return "bool";
        case TypeKind::Int:
        case TypeKind::Float:
        case TypeKind::Named: return name_of(type.m_name);
        case TypeKind::Pointer: return std::string(type.m_const ? "const* " : "*") + text(type.m_inner);
        case TypeKind::Reference: return std::string(type.m_const ? "const& " : "&") + text(type.m_inner);
        case TypeKind::Optional: return text(type.m_inner) + "?";
        case TypeKind::Function: {
            std::string result = "fn(";
            for (size_t i = 0; i < type.m_params.size(); ++i) result += (i ? ", " : "") + text(type.m_params[i]);
            result += ")";
            if (type.m_inner != NO_TYPE && get(type.m_inner).m_kind != TypeKind::Void) result += " -> " + text(type.m_inner);
            return result;
        }
    }
    return "?";
}

size_t Module::lowered_count() const {
    size_t count = 0;
    for (const Function& fn : m_functions) count += fn.m_lowered ? 1 : 0;
    return count;
}

std::string dump(const Module& module) { return Printer(module).run(); }

} // namespace fool::hir
