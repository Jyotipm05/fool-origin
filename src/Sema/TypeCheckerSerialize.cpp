//
// Created by jmbab on 18-11-2026.
//

/**
 * @file TypeCheckerSerialize.cpp
 * @brief Pass 2 (part 6): `$serialize`, the built-in `serialize(value)`, and compile-time reflection.
 *
 *     $serialize(format = "json")
 *     struct Player { id: i32; name: StringView; score: f64?; }
 *
 *     var text = serialize(player);          // StringView
 *     &field_count(Player)                   // usize
 *     &type_name(player)                     // StringView
 *     &is_serializable(Player)               // bool
 *
 * `$serialize` is an opt-in: every class, struct or enum reachable from a serialized type has to carry it too,
 * and every field has to be a number, `bool`, a `StringView`, another `$serialize` type, or an optional of one.
 * Pointers, references and function values have no stable representation outside the running program and are
 * rejected with a message that points at the offending field. This stage checks types and records metadata
 * (`--dump-reflection`); it does not generate code, so `serialize` has a type but no body yet.
 */

module;

#include <cstddef>
#include <set>
#include <string>
#include <string_view>
#include <vector>

module fool.sema;

namespace fool {

namespace {

std::string_view text_of(SymbolID id) { return StringInterner::get().resolve(id); }
std::string name_of(SymbolID id) { return std::string(text_of(id)); }

const AttributeNode* find_attribute(const DeclNode* decl, std::string_view name) {
    if (decl == nullptr) return nullptr;
    for (const AttributeNode& attr : decl->m_attributes) {
        if (text_of(attr.m_name) == name) return &attr;
    }
    return nullptr;
}

std::string serialize_format(const DeclNode* decl) {
    if (const AttributeNode* attr = find_attribute(decl, "serialize")) {
        for (const AttributeArg& arg : attr->m_args) {
            if (arg.m_key != INVALID_SYMBOL_ID && text_of(arg.m_key) == "format") return std::string(arg.m_value);
        }
    }
    return "json";
}

constexpr std::string_view TEXT_TYPE = "StringView";

} // namespace

// ---------------------------------------------------------------------------------------------
// What can be serialized
// ---------------------------------------------------------------------------------------------

std::string TypeCheckerPass2::serialization_problem(const TypeNode* type) const {
    if (type == nullptr) return {};
    switch (type->m_kind) {
        case TypeNodeKind::Optional: return serialization_problem(type->m_inner);
        case TypeNodeKind::Pointer:
        case TypeNodeKind::Reference:
            return "pointers and references have no stable representation outside the running program";
        case TypeNodeKind::Function: return "function values cannot be serialized";
        case TypeNodeKind::Never: return "the never type has no values";
        case TypeNodeKind::Path: break;
        default: return {};
    }
    if (type->m_path.empty()) return {};
    if (type->m_path.size() == 1) {
        const SymbolID name = type->m_path.front();
        const std::string text = name_of(name);
        if (type->m_generic_args.empty()) {
            const StaticType known = static_type_of(type);
            if (known.m_kind == StaticType::Kind::Int || known.m_kind == StaticType::Kind::Float || known.m_kind == StaticType::Kind::Bool) return {};
            if (known.m_kind == StaticType::Kind::Void) return "'void' has no values";
            if (is_generic_parameter(name)) return {}; // judged when the generic type is instantiated
            if (text == TEXT_TYPE) return {};
        }
        const DeclNode* decl = m_pass0.find_class(name);
        if (decl == nullptr) decl = m_pass0.find_enum(name);
        if (decl != nullptr) {
            if (find_attribute(decl, "serialize") == nullptr) {
                return "'" + text + "' is not marked $serialize; add it to its declaration";
            }
            for (const GenericArg& arg : type->m_generic_args) {
                if (arg.m_type == nullptr) continue;
                if (std::string inner = serialization_problem(arg.m_type); !inner.empty()) return inner;
            }
        }
    }
    return {}; // names from other modules are judged where they are declared
}

std::string TypeCheckerPass2::serialization_problem(const StaticType& type) const {
    using Kind = StaticType::Kind;
    switch (type.m_kind) {
        case Kind::Optional: return type.m_pointee ? serialization_problem(*type.m_pointee) : std::string{};
        case Kind::Pointer:
        case Kind::Reference: // `serialize(&player)` writes the player it refers to
            return type.m_pointee ? serialization_problem(*type.m_pointee) : std::string{};
        case Kind::Function: return "function values cannot be serialized";
        case Kind::Null: return "'null' is not a value of any type";
        case Kind::Named: {
            const std::string text = name_of(type.m_name);
            if (text == TEXT_TYPE) return {};
            const DeclNode* decl = m_pass0.find_class(type.m_name);
            if (decl == nullptr) decl = m_pass0.find_enum(type.m_name);
            if (decl != nullptr && find_attribute(decl, "serialize") == nullptr) {
                return "'" + text + "' is not marked $serialize; add it to its declaration";
            }
            return {};
        }
        default: return {};
    }
}

void TypeCheckerPass2::check_serializable_declarations() {
    for (DeclNode* decl : m_pass0.active_declarations()) {
        if (find_attribute(decl, "serialize") == nullptr) continue;
        if (!decl->m_file.empty()) m_diag.set_current_file(std::string(decl->m_file));
        if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
            for (const ParameterDecl& field : cls->m_fields) {
                if (std::string problem = serialization_problem(field.m_type); !problem.empty()) {
                    m_diag.error(field.m_span, "field '" + name_of(field.m_name) + "' of $serialize " + (cls->m_is_struct ? "struct" : "class") + " '" +
                                               name_of(cls->m_name) + "' cannot be serialized: " + problem);
                }
            }
        } else if (const auto* en = dynamic_cast<const EnumDeclNode*>(decl)) {
            for (const EnumVariant& variant : en->m_variants) {
                for (const TypeNode* payload : variant.m_payload) {
                    if (std::string problem = serialization_problem(payload); !problem.empty()) {
                        m_diag.error(variant.m_span, "variant '" + name_of(variant.m_name) + "' of $serialize enum '" + name_of(en->m_name) +
                                                     "' cannot be serialized: " + problem);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// serialize(value)
// ---------------------------------------------------------------------------------------------

bool TypeCheckerPass2::is_builtin_serialize(SymbolID name) {
    if (name == INVALID_SYMBOL_ID || text_of(name) != "serialize") return false;
    return lookup(name) == nullptr && m_pass0.find_function(name) == nullptr && m_pass0.find_global(name) == nullptr;
}

void TypeCheckerPass2::check_serialize_call(const CallExpr* call) {
    if (call->m_args.size() != 1) {
        m_diag.error(call->m_span, "'serialize' takes exactly one value, but " + std::to_string(call->m_args.size()) + " were given");
    }
    for (const CallArg& arg : call->m_args) {
        if (arg.m_label != INVALID_SYMBOL_ID) {
            m_diag.error(arg.m_span, "the argument of 'serialize' cannot be named");
            return;
        }
    }
    if (call->m_args.size() != 1) return;
    const ExprNode* value = call->m_args.front().m_expr;
    const StaticType type = infer_type(value);
    if (!type.known()) return;
    if (std::string problem = serialization_problem(type); !problem.empty()) {
        m_diag.error(value->m_span, "cannot serialize a value of type '" + describe(type) + "': " + problem);
    }
}

// ---------------------------------------------------------------------------------------------
// Reflection intrinsics: &type_name(x), &field_count(T), &is_serializable(T)
// ---------------------------------------------------------------------------------------------

StaticType TypeCheckerPass2::intrinsic_subject(const ExprNode* arg) {
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(arg)) {
        if (const SymbolBinding* binding = lookup(id->m_identifier)) return binding->m_static_type; // a variable: its type
        if (m_pass0.find_class(id->m_identifier) != nullptr || m_pass0.find_enum(id->m_identifier) != nullptr) {
            StaticType named;
            named.m_kind = StaticType::Kind::Named;
            named.m_name = id->m_identifier;
            return named;
        }
        // a primitive type name such as i32 or f64
        TypeNode primitive(TypeNodeKind::Path);
        primitive.m_path.push_back(id->m_identifier);
        return static_type_of(&primitive);
    }
    return infer_type(arg);
}

namespace {
bool is_reflection_intrinsic(std::string_view name) { return name == "type_name" || name == "field_count" || name == "is_serializable"; }
} // namespace

StaticType TypeCheckerPass2::reflection_result_type(const IntrinsicCallExpr* intrinsic) {
    using Kind = StaticType::Kind;
    const std::string_view name = text_of(intrinsic->m_name);
    if (!is_reflection_intrinsic(name)) return {};
    StaticType result;
    if (name == "is_serializable") {
        result.m_kind = Kind::Bool;
    } else if (name == "field_count") {
        result.m_kind = Kind::Int;
        result.m_name = StringInterner::get().intern("usize");
        result.m_bits = m_pass0.target().m_pointer_width;
        result.m_signed = false;
    } else {
        result.m_kind = Kind::Named;
        result.m_name = StringInterner::get().intern(std::string(TEXT_TYPE));
    }
    return result;
}

void TypeCheckerPass2::check_reflection_intrinsic(const IntrinsicCallExpr* intrinsic) {
    const std::string_view name = text_of(intrinsic->m_name);
    if (!is_reflection_intrinsic(name)) return;
    const std::string spelling = "&" + std::string(name);
    if (intrinsic->m_args.size() != 1) {
        m_diag.error(intrinsic->m_span, spelling + " takes exactly one argument: a type or a value");
        return;
    }
    const ExprNode* arg = intrinsic->m_args.front();
    if (const auto* id = dynamic_cast<const IdentifierExpr*>(arg)) {
        const StaticType guess = intrinsic_subject(arg);
        const bool unresolved_name = guess.m_kind == StaticType::Kind::Named && lookup(id->m_identifier) == nullptr &&
                                     !is_known_name(id->m_identifier) && !is_generic_parameter(id->m_identifier);
        if (unresolved_name || (!guess.known() && lookup(id->m_identifier) == nullptr && !is_generic_parameter(id->m_identifier))) {
            m_diag.error(arg->m_span, spelling + ": '" + name_of(id->m_identifier) + "' is neither a type nor a variable");
            return;
        }
    }
    StaticType subject = intrinsic_subject(arg);
    if (!subject.known()) return;
    if (name == "field_count") {
        while ((subject.is_view() || subject.is_optional()) && subject.m_pointee) subject = *subject.m_pointee; // look through views and optionals
        if (!subject.known()) return;
        const bool is_class = subject.m_kind == StaticType::Kind::Named && m_pass0.find_class(subject.m_name) != nullptr;
        const bool is_enum = subject.m_kind == StaticType::Kind::Named && m_pass0.find_enum(subject.m_name) != nullptr;
        if (!is_class) {
            m_diag.error(arg->m_span, spelling + " needs a class or struct, not " + (is_enum ? "the enum '" : "'") + describe(subject) + "'");
        }
    }
}

// ---------------------------------------------------------------------------------------------
// --dump-reflection
// ---------------------------------------------------------------------------------------------

std::string TypeCheckerPass2::reflection_report() const {
    std::string out;
    auto status = [&](const TypeNode* type) {
        std::string problem = serialization_problem(type);
        return problem.empty() ? std::string("[ok]") : "[not serializable: " + problem + "]";
    };
    for (const DeclNode* decl : m_pass0.active_declarations()) {
        const bool serialized = find_attribute(decl, "serialize") != nullptr;
        if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
            out += std::string(cls->m_is_struct ? "struct " : "class ") + name_of(cls->m_name) + ": ";
            out += serialized ? "serialize format=" + serialize_format(decl) : std::string("not serialized");
            out += " fields=" + std::to_string(cls->m_fields.size()) + "\n";
            for (const ParameterDecl& field : cls->m_fields) {
                out += "  " + name_of(field.m_name) + ": " + type_to_string(field.m_type) + " " + status(field.m_type) + "\n";
            }
        } else if (const auto* en = dynamic_cast<const EnumDeclNode*>(decl)) {
            out += "enum " + name_of(en->m_name) + ": ";
            out += serialized ? "serialize format=" + serialize_format(decl) : std::string("not serialized");
            out += " variants=" + std::to_string(en->m_variants.size()) + "\n";
            for (const EnumVariant& variant : en->m_variants) {
                out += "  " + name_of(variant.m_name);
                if (!variant.m_payload.empty()) {
                    out += "(";
                    for (size_t i = 0; i < variant.m_payload.size(); ++i) out += (i ? ", " : "") + type_to_string(variant.m_payload[i]);
                    out += ")";
                }
                std::string problem;
                for (const TypeNode* payload : variant.m_payload) {
                    if (problem.empty()) problem = serialization_problem(payload);
                }
                out += problem.empty() ? " [ok]\n" : " [not serializable: " + problem + "]\n";
            }
        }
    }
    return out;
}

} // namespace fool
