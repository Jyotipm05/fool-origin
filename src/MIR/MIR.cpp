//
// MIR text dump and verifier.
//

/**
 * @file MIR.cpp
 * @brief The `--dump-mir` printer and the structural/type verifier.
 *
 * Text form:
 *
 *     fn sum(data: const* f32, length: usize) -> f32 {
 *       let total#2: f32;
 *       let i#3: usize;
 *       let _4: bool;
 *       bb0:
 *         total#2 = 0.0:f32
 *         i#3 = 0:usize
 *         goto bb1
 *       bb1:
 *         _4 = i#3 < length#1
 *         branch _4, bb2, bb3
 *       ...
 *     }
 *
 * Locals print as `name#index` (temporaries as `_index`), places as `x#1`, `(*p#0)`, `x#1.field`, `p#0[i#3]`.
 */

module;

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

module fool.mir;

namespace fool::mir {

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
                 " function(s) lowered";
        if (m_module.m_templates != 0) m_out += ", " + std::to_string(m_module.m_templates) + " generic template(s) instantiated on use";
        m_out += "\n";
        return std::move(m_out);
    }

private:
    std::string local_text(LocalId id) const {
        const Local& local = m_fn->m_locals[id];
        if (local.m_name == INVALID_SYMBOL_ID) return "_" + std::to_string(id);
        return name_of(local.m_name) + "#" + std::to_string(id);
    }

    std::string place_text(const Place& place) const {
        std::string text = place.is_global() ? "::" + name_of(place.m_global) : local_text(place.m_local);
        TypeId current = place.m_base_type;
        for (const Projection& projection : place.m_projections) {
            switch (projection.m_kind) {
                case ProjectionKind::Deref: text = "(*" + text + ")"; break;
                case ProjectionKind::Field: text += "." + name_of(projection.m_name); break;
                case ProjectionKind::Index: text += "[" + local_text(projection.m_local) + "]"; break;
                case ProjectionKind::Payload: {
                    const hir::Type& enum_type = m_module.m_types.get(current);
                    const hir::EnumInfo* info = enum_type.m_kind == hir::TypeKind::Named ? m_module.find_enum(enum_type.m_name) : nullptr;
                    text += "." + (info != nullptr && projection.m_variant < info->m_variants.size()
                                       ? name_of(info->m_variants[projection.m_variant].m_name)
                                       : std::to_string(projection.m_variant)) +
                            "." + std::to_string(projection.m_index);
                    break;
                }
            }
            current = projection.m_type;
        }
        return text;
    }

    std::string operand_text(const Operand& operand) const {
        const auto type = [&](TypeId id) { return m_module.m_types.text(id); };
        switch (operand.m_kind) {
            case OperandKind::Unit: return "()";
            case OperandKind::Int: return std::to_string(operand.m_int) + ":" + type(operand.m_type);
            case OperandKind::Float: {
                char buffer[64];
                std::snprintf(buffer, sizeof buffer, "%g", operand.m_float);
                std::string text = buffer;
                if (text.find_first_of(".eEn") == std::string::npos) text += ".0";
                return text + ":" + type(operand.m_type);
            }
            case OperandKind::Bool: return operand.m_int != 0 ? "true" : "false";
            case OperandKind::Str: return operand.m_text;
            case OperandKind::Null: return "null:" + type(operand.m_type);
            case OperandKind::Read: return place_text(operand.m_place);
        }
        return "?";
    }

    std::string operands_text(const std::vector<Operand>& operands, const char* open, const char* close) const {
        std::string text = open;
        for (size_t i = 0; i < operands.size(); ++i) text += (i != 0 ? ", " : "") + operand_text(operands[i]);
        return text + close;
    }

    std::string rvalue_text(const Rvalue& value) const {
        const auto type = [&](TypeId id) { return m_module.m_types.text(id); };
        const auto operand = [&](size_t i) { return operand_text(value.m_operands[i]); };
        switch (value.m_kind) {
            case RvalueKind::Use: return operand(0);
            case RvalueKind::Unary: return op_text(value.m_op, true) + " " + operand(0);
            case RvalueKind::Binary: return operand(0) + " " + op_text(value.m_op, false) + " " + operand(1);
            case RvalueKind::Convert: return "convert:" + type(value.m_type) + " " + operand(0);
            case RvalueKind::Cast: return "cast." + std::string(cast_name(value.m_cast)) + ":" + type(value.m_type) + " " + operand(0);
            case RvalueKind::AddrOf: return "&" + place_text(value.m_place);
            case RvalueKind::Unwrap: return "unwrap " + operand(0);
            case RvalueKind::Wrap: return "wrap:" + type(value.m_type) + " " + operand(0);
            case RvalueKind::Aggregate: return "new " + type(value.m_type) + " " + operands_text(value.m_operands, "{", "}");
            case RvalueKind::Variant:
                return "variant " + value.m_text + (value.m_operands.empty() ? std::string() : operands_text(value.m_operands, "(", ")"));
            case RvalueKind::Call: return "call " + value.m_text + operands_text(value.m_operands, "(", ")");
        }
        return "?";
    }

    std::string terminator_text(const Terminator& end) const {
        switch (end.m_kind) {
            case TerminatorKind::Goto: return "goto bb" + std::to_string(end.m_target);
            case TerminatorKind::Branch:
                return "branch " + operand_text(end.m_value) + ", bb" + std::to_string(end.m_target) + ", bb" + std::to_string(end.m_else);
            case TerminatorKind::Return: return end.m_has_value ? "return " + operand_text(end.m_value) : std::string("return");
            case TerminatorKind::Switch: {
                const hir::Type& enum_type = m_module.m_types.get(end.m_value.m_type);
                const hir::EnumInfo* info = m_module.find_enum(enum_type.m_name);
                std::string text = "switch " + operand_text(end.m_value) + " [";
                for (size_t i = 0; i < end.m_cases.size(); ++i) {
                    const size_t variant = end.m_cases[i].m_variant;
                    text += (i != 0 ? ", " : "") + (info != nullptr && variant < info->m_variants.size() ? name_of(info->m_variants[variant].m_name) : std::to_string(variant)) +
                            " -> bb" + std::to_string(end.m_cases[i].m_target);
                }
                return text + "], else bb" + std::to_string(end.m_else);
            }
            case TerminatorKind::Unreachable: return "unreachable";
        }
        return "?";
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
        if (m_module.m_types.get(fn.m_return).m_kind != hir::TypeKind::Void) m_out += " -> " + m_module.m_types.text(fn.m_return);
        m_out += " {\n";
        for (size_t i = fn.m_param_count; i < fn.m_locals.size(); ++i) {
            const Local& local = fn.m_locals[i];
            m_out += std::string("  ") + (local.m_mutable ? "let " : "const ") + local_text(static_cast<LocalId>(i)) + ": " +
                     m_module.m_types.text(local.m_type) + ";\n";
        }
        for (size_t i = 0; i < fn.m_blocks.size(); ++i) {
            m_out += "  bb" + std::to_string(i) + ":\n";
            for (const Statement& statement : fn.m_blocks[i].m_statements) {
                if (statement.m_kind == StatementKind::Assign) {
                    m_out += "    " + place_text(statement.m_place) + " = " + rvalue_text(statement.m_value) + "\n";
                } else {
                    m_out += "    eval " + rvalue_text(statement.m_value) + "\n";
                }
            }
            m_out += "    " + terminator_text(fn.m_blocks[i].m_terminator) + "\n";
        }
        m_out += "}\n";
    }
};

class Verifier {
    // 1. Member variables
    const Module& m_module;
    const Function* m_fn{nullptr};
    std::vector<std::string> m_errors;

public:
    // 2. Constructors
    explicit Verifier(const Module& module) : m_module(module) {}

    // 3. Member functions
    std::vector<std::string> run() {
        for (const Function& fn : m_module.m_functions) {
            if (fn.m_lowered) check_function(fn);
        }
        return std::move(m_errors);
    }

private:
    void error(const std::string& message) { m_errors.push_back("in '" + m_fn->m_name + "': " + message); }

    bool is_kind(TypeId type, hir::TypeKind kind) const { return type != NO_TYPE && m_module.m_types.get(type).m_kind == kind; }

    void check_place(const Place& place) {
        if (place.is_global()) {
            if (place.m_global == INVALID_SYMBOL_ID) error("a place has neither a local nor a global base");
        } else if (place.m_local >= m_fn->m_locals.size()) {
            error("a place names local " + std::to_string(place.m_local) + ", which does not exist");
            return;
        } else if (place.m_base_type != m_fn->m_locals[place.m_local].m_type) {
            error("a place records another type than its local " + std::to_string(place.m_local) + " has");
        }
        TypeId current = place.m_base_type;
        for (const Projection& projection : place.m_projections) {
            if (projection.m_kind == ProjectionKind::Field) {
                if (!is_kind(current, hir::TypeKind::Named)) error("a field is read from a value that is not a class");
            } else if (projection.m_kind == ProjectionKind::Payload) {
                const hir::EnumInfo* info = is_kind(current, hir::TypeKind::Named) ? m_module.find_enum(m_module.m_types.get(current).m_name) : nullptr;
                if (info == nullptr) {
                    error("a payload value is read from '" + m_module.m_types.text(current) + "', which is not an enum");
                } else if (projection.m_variant >= info->m_variants.size() || projection.m_index >= info->m_variants[projection.m_variant].m_payload.size()) {
                    error("a payload projection names a variant or slot that '" + m_module.m_types.text(current) + "' does not have");
                } else if (info->m_variants[projection.m_variant].m_payload[projection.m_index] != projection.m_type) {
                    error("a payload projection of '" + m_module.m_types.text(current) + "' records the wrong type");
                }
            } else {
                const bool view = is_kind(current, hir::TypeKind::Pointer) || is_kind(current, hir::TypeKind::Reference);
                if (!view) {
                    error(std::string(projection.m_kind == ProjectionKind::Deref ? "dereference" : "index") + " of '" +
                          m_module.m_types.text(current) + "', which is not a pointer or reference");
                } else if (m_module.m_types.get(current).m_inner != projection.m_type) {
                    error("a projection of '" + m_module.m_types.text(current) + "' records the wrong element type");
                }
                if (projection.m_kind == ProjectionKind::Index &&
                    (projection.m_local >= m_fn->m_locals.size() || !is_kind(m_fn->m_locals[projection.m_local].m_type, hir::TypeKind::Int))) {
                    error("an index is not an integer local");
                }
            }
            current = projection.m_type;
        }
    }

    void check_operand(const Operand& operand) {
        if (operand.m_kind == OperandKind::Read) check_place(operand.m_place);
    }

    void check_function(const Function& fn) {
        m_fn = &fn;
        if (fn.m_blocks.empty()) {
            error("no blocks");
            return;
        }
        for (size_t b = 0; b < fn.m_blocks.size(); ++b) {
            const Block& block = fn.m_blocks[b];
            if (!block.m_terminated) error("bb" + std::to_string(b) + " has no terminator");
            for (const Statement& statement : block.m_statements) check_statement(statement);
            check_terminator(fn, block.m_terminator, b);
        }
    }

    void check_statement(const Statement& statement) {
        const Rvalue& value = statement.m_value;
        for (const Operand& operand : value.m_operands) check_operand(operand);
        if (value.m_kind == RvalueKind::AddrOf) check_place(value.m_place);
        if (statement.m_kind == StatementKind::Eval) return;
        check_place(statement.m_place);
        if (statement.m_place.type() != value.m_type) {
            error("assigns a value of type '" + m_module.m_types.text(value.m_type) + "' to a place of type '" +
                  m_module.m_types.text(statement.m_place.type()) + "'");
        }
        if (value.m_kind == RvalueKind::Use && (value.m_operands.size() != 1 || value.m_operands[0].m_kind == OperandKind::Unit)) {
            error("stores a value that does not exist (a call that returns nothing)");
        }
    }

    void check_terminator(const Function& fn, const Terminator& end, size_t block) {
        const auto target = [&](BlockId id) {
            if (id >= fn.m_blocks.size()) error("bb" + std::to_string(block) + " jumps to bb" + std::to_string(id) + ", which does not exist");
        };
        switch (end.m_kind) {
            case TerminatorKind::Goto: target(end.m_target); break;
            case TerminatorKind::Branch:
                target(end.m_target);
                target(end.m_else);
                check_operand(end.m_value);
                if (!is_kind(end.m_value.m_type, hir::TypeKind::Bool)) error("bb" + std::to_string(block) + " branches on a value that is not bool");
                break;
            case TerminatorKind::Switch: {
                check_operand(end.m_value);
                const hir::EnumInfo* info = is_kind(end.m_value.m_type, hir::TypeKind::Named) ? m_module.find_enum(m_module.m_types.get(end.m_value.m_type).m_name) : nullptr;
                if (info == nullptr) error("bb" + std::to_string(block) + " switches on a value that is not an enum");
                target(end.m_else);
                for (const SwitchCase& arm : end.m_cases) {
                    target(arm.m_target);
                    if (info != nullptr && arm.m_variant >= info->m_variants.size()) error("bb" + std::to_string(block) + " switches on a variant the enum does not have");
                }
                break;
            }
            case TerminatorKind::Return: {
                const bool void_result = is_kind(fn.m_return, hir::TypeKind::Void);
                if (end.m_has_value == void_result) {
                    error("bb" + std::to_string(block) + (void_result ? " returns a value from a function that returns nothing" : " returns nothing from a function that returns a value"));
                } else if (end.m_has_value) {
                    check_operand(end.m_value);
                    if (end.m_value.m_type != fn.m_return) error("bb" + std::to_string(block) + " returns a value of the wrong type");
                }
                break;
            }
            case TerminatorKind::Unreachable: break;
        }
    }
};

} // namespace

std::vector<std::string> verify(const Module& module) { return Verifier(module).run(); }

std::vector<std::string> selftest() {
    std::vector<std::string> failures;
    Module module;
    hir::Type int_type;
    int_type.m_kind = hir::TypeKind::Int;
    int_type.m_bits = 32;
    int_type.m_signed = true;
    int_type.m_name = StringInterner::get().intern("i32");
    const TypeId i32 = module.m_types.intern(int_type);
    hir::Type bool_type;
    bool_type.m_kind = hir::TypeKind::Bool;
    const TypeId boolean = module.m_types.intern(bool_type);
    hir::Type void_type;
    const TypeId nothing = module.m_types.intern(void_type);

    const auto int_operand = [&](int64_t value) {
        Operand operand;
        operand.m_kind = OperandKind::Int;
        operand.m_type = i32;
        operand.m_int = value;
        return operand;
    };
    const auto local_place = [&](LocalId id, TypeId type) {
        Place place;
        place.m_local = id;
        place.m_base_type = type;
        return place;
    };
    const auto return_of = [&](Operand value) {
        Terminator end;
        end.m_kind = TerminatorKind::Return;
        end.m_value = std::move(value);
        end.m_has_value = true;
        return end;
    };
    const auto function = [&](const char* name, TypeId result) {
        Function fn;
        fn.m_name = name;
        fn.m_lowered = true;
        fn.m_return = result;
        Local local;
        local.m_type = i32;
        fn.m_locals.push_back(local);
        fn.m_blocks.emplace_back();
        return fn;
    };
    const auto finish = [](Function& fn, size_t block, Terminator end) {
        fn.m_blocks[block].m_terminator = std::move(end);
        fn.m_blocks[block].m_terminated = true;
    };

    {   // sound: x = 1; return x
        Function fn = function("sound", i32);
        Statement store;
        store.m_place = local_place(0, i32);
        store.m_value.m_kind = RvalueKind::Use;
        store.m_value.m_type = i32;
        store.m_value.m_operands.push_back(int_operand(1));
        fn.m_blocks[0].m_statements.push_back(store);
        Operand read_back;
        read_back.m_kind = OperandKind::Read;
        read_back.m_type = i32;
        read_back.m_place = local_place(0, i32);
        finish(fn, 0, return_of(read_back));
        module.m_functions.push_back(std::move(fn));
    }
    {   // no terminator
        Function fn = function("open_block", nothing);
        module.m_functions.push_back(std::move(fn));
    }
    {   // jump to a block that does not exist
        Function fn = function("bad_target", nothing);
        Terminator go;
        go.m_kind = TerminatorKind::Goto;
        go.m_target = 7;
        finish(fn, 0, go);
        module.m_functions.push_back(std::move(fn));
    }
    {   // branch on an integer
        Function fn = function("bad_condition", nothing);
        Terminator split;
        split.m_kind = TerminatorKind::Branch;
        split.m_value = int_operand(1);
        split.m_target = 0;
        split.m_else = 0;
        finish(fn, 0, split);
        module.m_functions.push_back(std::move(fn));
    }
    {   // a switch on an integer
        Function fn = function("bad_switch", nothing);
        Terminator pick;
        pick.m_kind = TerminatorKind::Switch;
        pick.m_value = int_operand(1);
        pick.m_else = 0;
        pick.m_cases.push_back({0, 5});
        finish(fn, 0, pick);
        module.m_functions.push_back(std::move(fn));
    }
    {   // a payload read from an integer local
        Function fn = function("bad_payload", nothing);
        Statement store;
        store.m_place = local_place(0, i32);
        store.m_value.m_kind = RvalueKind::Use;
        store.m_value.m_type = i32;
        Operand payload;
        payload.m_kind = OperandKind::Read;
        payload.m_type = i32;
        payload.m_place = local_place(0, i32);
        Projection projection;
        projection.m_kind = ProjectionKind::Payload;
        projection.m_type = i32;
        payload.m_place.m_projections.push_back(projection);
        store.m_value.m_operands.push_back(payload);
        fn.m_blocks[0].m_statements.push_back(store);
        Terminator end;
        end.m_kind = TerminatorKind::Return;
        finish(fn, 0, end);
        module.m_functions.push_back(std::move(fn));
    }
    {   // a bool stored in an i32 local
        Function fn = function("bad_store", nothing);
        Statement store;
        store.m_place = local_place(0, i32);
        store.m_value.m_kind = RvalueKind::Use;
        store.m_value.m_type = boolean;
        Operand truth;
        truth.m_kind = OperandKind::Bool;
        truth.m_type = boolean;
        truth.m_int = 1;
        store.m_value.m_operands.push_back(truth);
        fn.m_blocks[0].m_statements.push_back(store);
        Terminator end;
        end.m_kind = TerminatorKind::Return;
        finish(fn, 0, end);
        module.m_functions.push_back(std::move(fn));
    }
    {   // a value returned from a function that returns nothing
        Function fn = function("bad_return", nothing);
        finish(fn, 0, return_of(int_operand(1)));
        module.m_functions.push_back(std::move(fn));
    }
    {   // a place that names a local that does not exist
        Function fn = function("bad_local", nothing);
        Statement store;
        store.m_place = local_place(9, i32);
        store.m_value.m_kind = RvalueKind::Use;
        store.m_value.m_type = i32;
        store.m_value.m_operands.push_back(int_operand(1));
        fn.m_blocks[0].m_statements.push_back(store);
        Terminator end;
        end.m_kind = TerminatorKind::Return;
        finish(fn, 0, end);
        module.m_functions.push_back(std::move(fn));
    }

    const std::vector<std::string> problems = verify(module);
    const auto expect = [&](const char* function_name, const char* text) {
        bool found = false;
        for (const std::string& problem : problems) {
            found = found || (problem.find(std::string("in '") + function_name + "'") != std::string::npos && problem.find(text) != std::string::npos);
        }
        if (!found) failures.push_back(std::string("the verifier did not report '") + text + "' in " + function_name);
    };
    expect("open_block", "no terminator");
    expect("bad_target", "does not exist");
    expect("bad_condition", "not bool");
    expect("bad_switch", "switches on a value that is not an enum");
    expect("bad_switch", "does not exist");
    expect("bad_payload", "which is not an enum");
    expect("bad_store", "assigns a value of type");
    expect("bad_return", "returns a value from a function that returns nothing");
    expect("bad_local", "does not exist");
    for (const std::string& problem : problems) {
        if (problem.find("in 'sound'") != std::string::npos) failures.push_back("the verifier rejected a sound function: " + problem);
    }
    return failures;
}

std::string dump(const Module& module) { return Printer(module).run(); }

} // namespace fool::mir
