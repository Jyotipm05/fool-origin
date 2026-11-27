//
// AST -> HIR lowering.
//

/**
 * @file Lower.cpp
 * @brief Lowers the checked AST of the monomorphic subset into HIR (see HIR.ixx).
 *
 * The lowering types every expression itself, from the declarations, instead of reading the checker's
 * inference table: that table keeps "integer literal" types and optional narrowing as flow state, which HIR has
 * to resolve (a literal gets the type of its context, `i32` / `f64` by default; a narrowed `T?` used as `T`
 * becomes an explicit Unwrap). The checker supplies what only it knows: which callee a call binds to (with
 * arguments in parameter order and defaults filled in) and which branch of a compile-time `@if` is active.
 *
 * Anything outside the subset aborts the function with a reason; the function is then reported as "not
 * lowerable yet" instead of producing a wrong tree.
 */

module;

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

module fool.hir;

namespace fool::hir {

namespace {

struct NotLowerable {
    std::string m_reason;
    SourceSpan m_span;
};

[[noreturn]] void fail(std::string reason, SourceSpan span) { throw NotLowerable{std::move(reason), span}; }

/// A local that was initialized from an untyped literal (`sum = 0;`) is used as another numeric type
/// (`return sum;` in a function returning `f32`): the function is lowered again with that local typed accordingly.
struct RetypeLocal {
    LocalId m_local;
    TypeId m_type;
};

std::string name_of(SymbolID id) { return std::string(StringInterner::get().resolve(id)); }

using ExprPtr = std::unique_ptr<Expr>;

bool is_arithmetic(TokenKind op) {
    switch (op) {
        case TokenKind::Plus:
        case TokenKind::Minus:
        case TokenKind::Star:
        case TokenKind::Slash:
        case TokenKind::Percent:
        case TokenKind::Ampersand:
        case TokenKind::Pipe:
        case TokenKind::Caret: return true;
        default: return false;
    }
}

bool is_comparison(TokenKind op) {
    switch (op) {
        case TokenKind::EqualEqual:
        case TokenKind::ExclaimEqual:
        case TokenKind::Less:
        case TokenKind::LessEqual:
        case TokenKind::Greater:
        case TokenKind::GreaterEqual: return true;
        default: return false;
    }
}

/// A literal (or arithmetic on literals, or `null`) whose type comes from the other operand or the context.
bool is_untyped(const ExprNode* node) {
    if (dynamic_cast<const IntegerLiteralExpr*>(node) != nullptr || dynamic_cast<const FloatLiteralExpr*>(node) != nullptr ||
        dynamic_cast<const NullLiteralExpr*>(node) != nullptr) {
        return true;
    }
    if (const auto* unary = dynamic_cast<const UnaryExpr*>(node)) {
        return unary->m_op == TokenKind::Minus && is_untyped(unary->m_operand);
    }
    if (const auto* binary = dynamic_cast<const BinaryExpr*>(node)) {
        return is_arithmetic(binary->m_op) && is_untyped(binary->m_lhs) && is_untyped(binary->m_rhs);
    }
    return false;
}

/// Type arguments in force: generic parameter name -> concrete type.
using Bindings = std::unordered_map<SymbolID, TypeId>;

/// A function body to lower for one set of type arguments (generic function, or method of a generic type).
struct Instantiation {
    const FunctionDeclNode* m_decl{nullptr};
    const ClassDeclNode* m_owner{nullptr};
    Bindings m_bindings;
    TypeId m_self{NO_TYPE};      ///< the instantiated owner type of a method (`Box<i32>`)
    std::string m_name;
};

/// The class behind a value type, with its type arguments bound to the class's own parameter names.
struct ClassView {
    const ClassDeclNode* m_decl{nullptr};
    Bindings m_bindings;
    std::vector<TypeId> m_args;
};

class Lowerer {
    // 1. Member variables
    const TypeCheckerPass2& m_checker;
    const SymbolTablePass0& m_pass0;
    Module& m_module;
    Function* m_fn{nullptr};
    std::vector<std::vector<std::pair<SymbolID, LocalId>>> m_scopes;
    bool m_isolated{false};   ///< lowering a parameter default: the caller's locals are not visible
    std::unordered_map<LocalId, TypeId> m_retypes;   ///< literal-initialized locals of the current function and the type their uses demand
    std::unordered_set<LocalId> m_defaulted;         ///< locals of the current attempt whose type came from an untyped literal
    Bindings m_subst;                                ///< type arguments of the function being lowered
    std::vector<Instantiation> m_queue;              ///< instantiations requested so far; lowered after the non-generic functions
    std::unordered_map<const FunctionDeclNode*, const ClassDeclNode*> m_method_owner;   ///< methods declared inside a class body -> that class
    std::unordered_set<SymbolID> m_registering;      ///< enums whose payload types are being lowered right now

    /// Swaps in other type arguments for the duration of a scope (the callee's signature, a class's field types).
    class SubstScope {
        Lowerer& m_lowerer;
        Bindings m_saved;

    public:
        SubstScope(Lowerer& lowerer, Bindings bindings) : m_lowerer(lowerer), m_saved(std::exchange(lowerer.m_subst, std::move(bindings))) {}
        SubstScope(const SubstScope&) = delete;
        SubstScope& operator=(const SubstScope&) = delete;
        ~SubstScope() { m_lowerer.m_subst = std::move(m_saved); }
    };

public:
    // 2. Constructors
    Lowerer(const TypeCheckerPass2& checker, const SymbolTablePass0& pass0, Module& module)
        : m_checker(checker), m_pass0(pass0), m_module(module) {}

    // 3. Member functions
    void run() {
        for (const DeclNode* decl : m_pass0.active_declarations()) {
            if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
                for (const FunctionDeclNode* method : cls->m_methods) m_method_owner[method] = cls;
            }
        }
        for (const DeclNode* decl : m_pass0.active_declarations()) {
            if (const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl)) {
                if (fn->m_has_body) add_function(fn, fn->m_owner != INVALID_SYMBOL_ID ? m_pass0.find_class(fn->m_owner) : nullptr);
            } else if (const auto* cls = dynamic_cast<const ClassDeclNode*>(decl)) {
                for (const FunctionDeclNode* method : cls->m_methods) {
                    if (method->m_has_body) add_function(method, cls);
                }
            }
        }
        // Instantiations requested by the code above, and by the instantiations themselves, in request order.
        for (size_t i = 0; i < m_queue.size(); ++i) {
            const Instantiation instantiation = m_queue[i];   // copied: lowering may request more and move the queue
            lower_function(instantiation.m_decl, instantiation.m_owner, &instantiation);
        }
    }

private:
    // ---- namespaces ----------------------------------------------------------------------------

    /// The namespace of the function being lowered (an instance uses its template's).
    [[nodiscard]] SymbolID current_namespace() const {
        return m_fn != nullptr && m_fn->m_decl != nullptr ? m_fn->m_decl->m_namespace : INVALID_SYMBOL_ID;
    }

    /// `name` for a top-level function, `ns::name` for one declared in a namespace.
    [[nodiscard]] static std::string namespaced(SymbolID ns, SymbolID name) {
        return ns == INVALID_SYMBOL_ID ? name_of(name) : name_of(ns) + "::" + name_of(name);
    }

    /// The name a global is known by in the program: namespace-qualified.
    [[nodiscard]] static SymbolID global_key(const VarDeclNode* var) {
        if (var->m_namespace == INVALID_SYMBOL_ID) return var->m_name;
        return StringInterner::get().intern(namespaced(var->m_namespace, var->m_name));
    }

    // ---- types ---------------------------------------------------------------------------------

    [[nodiscard]] TypeId intern(Type type) { return m_module.m_types.intern(std::move(type)); }
    [[nodiscard]] const Type& ty(TypeId id) const { return m_module.m_types.get(id); }

    [[nodiscard]] TypeId simple(TypeKind kind) {
        Type type;
        type.m_kind = kind;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId named(SymbolID name) {
        Type type;
        type.m_kind = TypeKind::Named;
        type.m_name = name;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId view(TypeKind kind, bool is_const, TypeId inner) {
        Type type;
        type.m_kind = kind;
        type.m_const = is_const;
        type.m_inner = inner;
        return intern(std::move(type));
    }

    [[nodiscard]] TypeId optional_of(TypeId inner) {
        Type type;
        type.m_kind = TypeKind::Optional;
        type.m_inner = inner;
        return intern(std::move(type));
    }

    [[nodiscard]] std::optional<TypeId> primitive(std::string_view name) {
        Type type;
        const unsigned pointer_bits = m_pass0.target().m_pointer_width;
        if (name == "bool") { type.m_kind = TypeKind::Bool; return intern(std::move(type)); }
        if (name == "void") { type.m_kind = TypeKind::Void; return intern(std::move(type)); }
        const auto integer = [&](unsigned bits, bool is_signed) {
            type.m_kind = TypeKind::Int;
            type.m_bits = bits;
            type.m_signed = is_signed;
            type.m_name = StringInterner::get().intern(name);
            return std::optional<TypeId>(intern(std::move(type)));
        };
        const auto real = [&](unsigned bits) {
            type.m_kind = TypeKind::Float;
            type.m_bits = bits;
            type.m_signed = true;
            type.m_name = StringInterner::get().intern(name);
            return std::optional<TypeId>(intern(std::move(type)));
        };
        if (name == "i8") return integer(8, true);
        if (name == "i16") return integer(16, true);
        if (name == "i32") return integer(32, true);
        if (name == "i64") return integer(64, true);
        if (name == "isize") return integer(pointer_bits, true);
        if (name == "u8") return integer(8, false);
        if (name == "u16") return integer(16, false);
        if (name == "u32") return integer(32, false);
        if (name == "u64") return integer(64, false);
        if (name == "usize") return integer(pointer_bits, false);
        if (name == "f32") return real(32);
        if (name == "f64") return real(64);
        return std::nullopt;
    }

    [[nodiscard]] TypeId prim(std::string_view name) { return *primitive(name); }
    [[nodiscard]] TypeId bool_type() { return simple(TypeKind::Bool); }
    [[nodiscard]] TypeId void_type() { return simple(TypeKind::Void); }

    [[nodiscard]] TypeId lower_type(const TypeNode* node) {
        if (node == nullptr) return void_type();
        switch (node->m_kind) {
            case TypeNodeKind::Never: return simple(TypeKind::Never);
            case TypeNodeKind::Pointer: return view(TypeKind::Pointer, node->m_is_const, lower_type(node->m_inner));
            case TypeNodeKind::Reference: return view(TypeKind::Reference, node->m_is_const, lower_type(node->m_inner));
            case TypeNodeKind::Optional: return optional_of(lower_type(node->m_inner));
            case TypeNodeKind::Function: {
                Type type;
                type.m_kind = TypeKind::Function;
                for (const TypeNode* param : node->m_param_types) type.m_params.push_back(lower_type(param));
                type.m_inner = lower_type(node->m_inner);
                return intern(std::move(type));
            }
            case TypeNodeKind::Path: break;
        }
        if (node->m_path.empty()) fail("empty type path", node->m_span);
        const SymbolID last = node->m_path.back();
        const std::string text = name_of(last);
        if (node->m_path.size() == 1 && node->m_generic_args.empty()) {
            if (const auto bound = m_subst.find(last); bound != m_subst.end()) return bound->second;   // a type argument in force
            if (const auto builtin = primitive(text)) return *builtin;
        }
        if (const ClassDeclNode* cls = m_pass0.find_class(last)) {
            if (cls->m_type_params.empty()) {
                if (!node->m_generic_args.empty()) fail("type '" + text + "' is not generic", node->m_span);
                return named(last);
            }
            if (node->m_generic_args.size() != cls->m_type_params.size()) {
                fail("generic type '" + text + "' takes " + std::to_string(cls->m_type_params.size()) + " type argument(s)", node->m_span);
            }
            std::vector<TypeId> args;
            for (const GenericArg& arg : node->m_generic_args) {
                if (arg.m_type == nullptr || arg.m_name != INVALID_SYMBOL_ID) fail("constant generic arguments of '" + text + "' (not lowerable yet)", node->m_span);
                args.push_back(lower_type(arg.m_type));
            }
            return instance_of(cls, args);
        }
        if (const EnumDeclNode* en = m_pass0.find_enum(last)) {
            if (!node->m_generic_args.empty()) fail("generic enum '" + text + "' (generic enums are not lowerable yet)", node->m_span);
            return enum_type(en, node->m_span);
        }
        if (m_pass0.find_ruleset(last) != nullptr) fail("ruleset type '" + text + "' (ruleset views are not lowerable yet)", node->m_span);
        fail("cannot resolve type '" + text + "' (generic parameters and types of modules that are not loaded are not lowerable yet)", node->m_span);
    }

    // ---- enums ---------------------------------------------------------------------------------

    /// The Named type of a non-generic enum; the first use records it, with the types of every variant's payload.
    [[nodiscard]] TypeId enum_type(const EnumDeclNode* en, SourceSpan span) {
        if (!en->m_type_params.empty()) fail("generic enum '" + name_of(en->m_name) + "' (generic enums are not lowerable yet)", span);
        const TypeId type = named(en->m_name);
        if (m_module.find_enum(en->m_name) != nullptr || m_registering.count(en->m_name) != 0) return type;
        m_registering.insert(en->m_name);   // a payload may mention the enum itself
        EnumInfo info;
        info.m_name = en->m_name;
        info.m_decl = en;
        try {
            for (const EnumVariant& variant : en->m_variants) {
                EnumVariantInfo entry;
                entry.m_name = variant.m_name;
                for (const TypeNode* payload : variant.m_payload) entry.m_payload.push_back(lower_type(payload));
                info.m_variants.push_back(std::move(entry));
            }
        } catch (...) {
            m_registering.erase(en->m_name);
            throw;
        }
        m_registering.erase(en->m_name);
        m_module.m_enums.push_back(std::move(info));
        return type;
    }

    /// `Enum::Variant` when the path names one (and is not a function of the enum).
    [[nodiscard]] const EnumDeclNode* variant_path(const PathExpr* path, size_t& index) const {
        if (path->m_segments.size() != 2 || !path->m_generic_args.empty()) return nullptr;
        const EnumDeclNode* en = m_pass0.find_enum(path->m_segments[0]);
        if (en == nullptr) return nullptr;
        for (size_t i = 0; i < en->m_variants.size(); ++i) {
            if (en->m_variants[i].m_name == path->m_segments[1]) {
                index = i;
                return en;
            }
        }
        return nullptr;
    }

    /// `Mode::Read` or `Shape::Circle(2.0)`: the payload arguments are converted to the variant's payload types.
    [[nodiscard]] ExprPtr lower_enum_value(const EnumDeclNode* en, size_t index, const std::vector<CallArg>& args, SourceSpan span) {
        const TypeId type = enum_type(en, span);
        const EnumInfo* info = m_module.find_enum(en->m_name);
        const EnumVariantInfo variant = info->m_variants[index];   // a copy: lowering the arguments may register more enums
        const std::string full = name_of(en->m_name) + "::" + name_of(variant.m_name);
        if (args.size() != variant.m_payload.size()) {
            fail("variant '" + full + "' takes " + std::to_string(variant.m_payload.size()) + " payload value(s), not " + std::to_string(args.size()), span);
        }
        ExprPtr out = node(ExprKind::EnumLit, type, span);
        out->m_index = index;
        out->m_text = full;
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i].m_label != INVALID_SYMBOL_ID) fail("a labelled payload value of '" + full + "' (not lowerable yet)", args[i].m_span);
            out->m_operands.push_back(coerce(lower_expr(args[i].m_expr, variant.m_payload[i]), variant.m_payload[i], args[i].m_span));
        }
        return out;
    }

    // ---- generics ------------------------------------------------------------------------------

    /// `Box<i32>`: records the instance (once) and returns its type.
    [[nodiscard]] TypeId instance_of(const ClassDeclNode* cls, const std::vector<TypeId>& args) {
        std::string text = name_of(cls->m_name) + "<";
        for (size_t i = 0; i < args.size(); ++i) text += (i != 0 ? ", " : "") + m_module.m_types.text(args[i]);
        text += ">";
        const SymbolID name = StringInterner::get().intern(text);
        if (m_module.find_instance(name) == nullptr) {
            ClassInstance instance;
            instance.m_name = name;
            instance.m_decl = cls;
            instance.m_args = args;
            m_module.m_instances.push_back(std::move(instance));
        }
        return named(name);
    }

    [[nodiscard]] std::optional<ClassView> lookup_class(TypeId value_type) const {
        const Type& type = ty(value_type);
        if (type.m_kind != TypeKind::Named) return std::nullopt;
        ClassView result;
        if (const ClassInstance* instance = m_module.find_instance(type.m_name)) {
            result.m_decl = instance->m_decl;
            result.m_args = instance->m_args;
            for (size_t i = 0; i < instance->m_decl->m_type_params.size() && i < instance->m_args.size(); ++i) {
                result.m_bindings[instance->m_decl->m_type_params[i].m_name] = instance->m_args[i];
            }
            return result;
        }
        if (const ClassDeclNode* cls = m_pass0.find_class(type.m_name); cls != nullptr && cls->m_type_params.empty()) {
            result.m_decl = cls;
            return result;
        }
        return std::nullopt;
    }

    [[nodiscard]] ClassView class_view(TypeId value_type, SourceSpan span) const {
        if (const auto result = lookup_class(value_type)) return *result;
        const Type& type = ty(value_type);
        if (type.m_kind != TypeKind::Named) fail("member access on a value of type '" + m_module.m_types.text(value_type) + "'", span);
        fail("'" + name_of(type.m_name) + "' is not a class or struct (enum members are not lowerable yet)", span);
    }

    /// The type a value of `id` finally refers to: optionals and views stripped.
    [[nodiscard]] TypeId value_type(TypeId id) const {
        while (true) {
            const Type& type = ty(id);
            if (type.m_kind != TypeKind::Optional && type.m_kind != TypeKind::Pointer && type.m_kind != TypeKind::Reference) return id;
            id = type.m_inner;
        }
    }

    /// True when `node` names a generic parameter that has no binding yet.
    [[nodiscard]] static bool mentions_unbound(const TypeNode* node, const std::unordered_set<SymbolID>& generics, const Bindings& bindings) {
        if (node == nullptr || generics.empty()) return false;
        switch (node->m_kind) {
            case TypeNodeKind::Path:
                if (node->m_path.size() == 1 && node->m_generic_args.empty() && generics.contains(node->m_path[0])) return !bindings.contains(node->m_path[0]);
                for (const GenericArg& arg : node->m_generic_args) {
                    if (mentions_unbound(arg.m_type, generics, bindings)) return true;
                }
                return false;
            case TypeNodeKind::Pointer:
            case TypeNodeKind::Reference:
            case TypeNodeKind::Optional: return mentions_unbound(node->m_inner, generics, bindings);
            case TypeNodeKind::Function:
                for (const TypeNode* param : node->m_param_types) {
                    if (mentions_unbound(param, generics, bindings)) return true;
                }
                return mentions_unbound(node->m_inner, generics, bindings);
            case TypeNodeKind::Never: return false;
        }
        return false;
    }

    /// Binds generic parameters by matching a parameter's written type against the type of its argument.
    void infer(const TypeNode* pattern, TypeId actual, Bindings& bound, const std::unordered_set<SymbolID>& generics) const {
        if (pattern == nullptr) return;
        const Type& type = ty(actual);   // nothing below interns, so the reference stays valid
        switch (pattern->m_kind) {
            case TypeNodeKind::Path:
                if (pattern->m_path.size() == 1 && pattern->m_generic_args.empty() && generics.contains(pattern->m_path[0])) {
                    bound.try_emplace(pattern->m_path[0], actual);
                } else if (!pattern->m_generic_args.empty() && type.m_kind == TypeKind::Named) {
                    const ClassInstance* instance = m_module.find_instance(type.m_name);
                    if (instance == nullptr || instance->m_decl->m_name != pattern->m_path.back() ||
                        instance->m_args.size() != pattern->m_generic_args.size()) {
                        return;
                    }
                    for (size_t i = 0; i < instance->m_args.size(); ++i) infer(pattern->m_generic_args[i].m_type, instance->m_args[i], bound, generics);
                }
                return;
            case TypeNodeKind::Pointer:
            case TypeNodeKind::Reference:
                if (type.m_kind == TypeKind::Pointer || type.m_kind == TypeKind::Reference) infer(pattern->m_inner, type.m_inner, bound, generics);
                return;
            case TypeNodeKind::Optional: infer(pattern->m_inner, type.m_kind == TypeKind::Optional ? type.m_inner : actual, bound, generics); return;
            case TypeNodeKind::Function:
                if (type.m_kind != TypeKind::Function || type.m_params.size() != pattern->m_param_types.size()) return;
                for (size_t i = 0; i < type.m_params.size(); ++i) infer(pattern->m_param_types[i], type.m_params[i], bound, generics);
                infer(pattern->m_inner, type.m_inner, bound, generics);
                return;
            case TypeNodeKind::Never: return;
        }
    }

    /// The class a method belongs to: the one written in `fn Owner::name`, else the one whose body declares it.
    [[nodiscard]] const ClassDeclNode* owner_of(const FunctionDeclNode* fn) const {
        if (fn->m_owner != INVALID_SYMBOL_ID) return m_pass0.find_class(fn->m_owner);
        const auto declared = m_method_owner.find(fn);
        return declared != m_method_owner.end() ? declared->second : nullptr;
    }

    /// A call binds to a declaration (a class-body `fn get(...);` or an interface entry); instantiating needs the body.
    [[nodiscard]] const FunctionDeclNode* find_definition(const FunctionDeclNode* declaration) const {
        if (declaration->m_has_body) return declaration;
        const ClassDeclNode* declared_owner = owner_of(declaration);
        const SymbolID owner_name = declared_owner != nullptr ? declared_owner->m_name : declaration->m_owner;
        for (const DeclNode* decl : m_pass0.active_declarations()) {
            const auto* fn = dynamic_cast<const FunctionDeclNode*>(decl);
            if (fn != nullptr && fn->m_has_body && fn->m_name == declaration->m_name && fn->m_params.size() == declaration->m_params.size() &&
                fn->m_owner == owner_name && fn->m_namespace == declaration->m_namespace) {
                return fn;
            }
        }
        return nullptr;
    }

    void request_instance(const FunctionDeclNode* decl, const ClassDeclNode* owner, Bindings bindings, TypeId self, std::string name, SourceSpan span) {
        for (const Instantiation& known : m_queue) {
            if (known.m_decl == decl && known.m_name == name) return;
        }
        if (m_queue.size() >= 1024) fail("more than 1024 instantiations (recursive generic instantiation?)", span);
        Instantiation instantiation;
        instantiation.m_decl = decl;
        instantiation.m_owner = owner;
        instantiation.m_bindings = std::move(bindings);
        instantiation.m_self = self;
        instantiation.m_name = std::move(name);
        m_queue.push_back(std::move(instantiation));
    }

    // ---- locals --------------------------------------------------------------------------------

    void push_scope() { m_scopes.emplace_back(); }
    void pop_scope() { m_scopes.pop_back(); }

    LocalId declare(SymbolID name, TypeId type, bool is_mutable, bool is_param) {
        Local local;
        local.m_name = name;
        local.m_type = type;
        local.m_mutable = is_mutable;
        local.m_is_param = is_param;
        m_fn->m_locals.push_back(local);
        const auto id = static_cast<LocalId>(m_fn->m_locals.size() - 1);
        m_scopes.back().emplace_back(name, id);
        return id;
    }

    [[nodiscard]] std::optional<LocalId> find_local(SymbolID name) const {
        if (m_isolated) return std::nullopt;
        for (auto scope = m_scopes.rbegin(); scope != m_scopes.rend(); ++scope) {
            for (auto entry = scope->rbegin(); entry != scope->rend(); ++entry) {
                if (entry->first == name) return entry->second;
            }
        }
        return std::nullopt;
    }

    // ---- node construction ---------------------------------------------------------------------

    [[nodiscard]] static ExprPtr node(ExprKind kind, TypeId type, SourceSpan span) {
        auto expr = std::make_unique<Expr>();
        expr->m_kind = kind;
        expr->m_type = type;
        expr->m_span = span;
        return expr;
    }

    [[nodiscard]] static ExprPtr wrap_one(ExprKind kind, TypeId type, ExprPtr operand) {
        const SourceSpan span = operand->m_span;
        ExprPtr expr = node(kind, type, span);
        expr->m_operands.push_back(std::move(operand));
        return expr;
    }

    // ---- conversions ---------------------------------------------------------------------------

    /// True when `coerce` can turn a value of type `from` into `to` (the conversions the checker allows implicitly).
    [[nodiscard]] bool convertible(TypeId from, TypeId to) const {
        if (from == to) return true;
        const Type& f = ty(from);
        const Type& t = ty(to);
        if (f.m_kind == TypeKind::Never) return true;
        if (f.m_kind == TypeKind::Int && t.m_kind == TypeKind::Int) {
            if (f.m_bits == t.m_bits && f.m_signed == t.m_signed) return true;   // usize <-> u64 on this target
            if (t.m_bits <= f.m_bits) return false;
            return f.m_signed == t.m_signed || (!f.m_signed && t.m_signed);
        }
        if (f.m_kind == TypeKind::Float && t.m_kind == TypeKind::Float) return f.m_bits <= t.m_bits;
        if ((f.m_kind == TypeKind::Pointer || f.m_kind == TypeKind::Reference) &&
            (t.m_kind == TypeKind::Pointer || t.m_kind == TypeKind::Reference)) {
            return f.m_inner == t.m_inner && (t.m_const || !f.m_const);
        }
        if (t.m_kind == TypeKind::Optional && f.m_kind != TypeKind::Optional) return convertible(from, t.m_inner);
        if (f.m_kind == TypeKind::Optional && t.m_kind != TypeKind::Optional) return convertible(f.m_inner, to);
        return false;
    }

    /// If `value` is a literal-initialized local that is being used as another numeric type, ask for a second attempt.
    void maybe_retype(const Expr& value, TypeId target) const {
        if (value.m_kind != ExprKind::Local || !m_defaulted.contains(value.m_local) || m_retypes.contains(value.m_local)) return;
        const TypeKind from_kind = ty(value.m_type).m_kind;
        const TypeKind to_kind = ty(target).m_kind;
        const auto numeric = [](TypeKind kind) { return kind == TypeKind::Int || kind == TypeKind::Float; };
        if (numeric(from_kind) && numeric(to_kind) && value.m_type != target) throw RetypeLocal{value.m_local, target};
    }

    [[nodiscard]] ExprPtr coerce(ExprPtr value, TypeId to, SourceSpan span) {
        if (value->m_type == to) return value;
        if (!convertible(value->m_type, to)) {
            maybe_retype(*value, to);
            fail("cannot convert '" + m_module.m_types.text(value->m_type) + "' to '" + m_module.m_types.text(to) + "'", span);
        }
        const Type& f = ty(value->m_type);
        const Type& t = ty(to);
        if (t.m_kind == TypeKind::Optional && f.m_kind != TypeKind::Optional) {
            return wrap_one(ExprKind::Wrap, to, coerce(std::move(value), t.m_inner, span));
        }
        if (f.m_kind == TypeKind::Optional && t.m_kind != TypeKind::Optional) {
            // The checker only lets a `T?` stand for a `T` where flow analysis proved it holds a value.
            return coerce(wrap_one(ExprKind::Unwrap, f.m_inner, std::move(value)), to, span);
        }
        return wrap_one(ExprKind::Convert, to, std::move(value));
    }

    /// A condition: `bool`, or an optional (true when it holds a value).
    [[nodiscard]] ExprPtr as_condition(ExprPtr value, SourceSpan span) {
        if (ty(value->m_type).m_kind == TypeKind::Optional) {
            ExprPtr null = node(ExprKind::NullLit, value->m_type, span);
            ExprPtr test = node(ExprKind::Binary, bool_type(), span);
            test->m_op = TokenKind::ExclaimEqual;
            test->m_operands.push_back(std::move(value));
            test->m_operands.push_back(std::move(null));
            return test;
        }
        return coerce(std::move(value), bool_type(), span);
    }

    /// Strips optionals (proved non-null by the checker) and views until a value is left, for member access.
    [[nodiscard]] ExprPtr to_value(ExprPtr value) {
        while (true) {
            const Type& type = ty(value->m_type);
            if (type.m_kind == TypeKind::Optional) {
                const TypeId inner = type.m_inner;
                value = wrap_one(ExprKind::Unwrap, inner, std::move(value));
            } else if (type.m_kind == TypeKind::Pointer || type.m_kind == TypeKind::Reference) {
                const TypeId inner = type.m_inner;
                value = wrap_one(ExprKind::Deref, inner, std::move(value));
            } else {
                return value;
            }
        }
    }

    /// Makes both operands one type: the checker allowed one to convert to the other.
    void unify(ExprPtr& lhs, ExprPtr& rhs, bool equality, SourceSpan span) {
        if (lhs->m_type == rhs->m_type) return;
        const bool lhs_optional = ty(lhs->m_type).m_kind == TypeKind::Optional;
        const bool rhs_optional = ty(rhs->m_type).m_kind == TypeKind::Optional;
        if (equality && lhs_optional != rhs_optional) {
            // `a == 5` compares optionals: the plain side is wrapped.
            if (lhs_optional) rhs = coerce(std::move(rhs), lhs->m_type, span);
            else lhs = coerce(std::move(lhs), rhs->m_type, span);
            return;
        }
        if (convertible(rhs->m_type, lhs->m_type)) {
            rhs = coerce(std::move(rhs), lhs->m_type, span);
        } else if (convertible(lhs->m_type, rhs->m_type)) {
            lhs = coerce(std::move(lhs), rhs->m_type, span);
        } else {
            maybe_retype(*lhs, rhs->m_type);
            maybe_retype(*rhs, lhs->m_type);
            fail("operands of type '" + m_module.m_types.text(lhs->m_type) + "' and '" + m_module.m_types.text(rhs->m_type) +
                     "' have no common type",
                 span);
        }
    }

    // ---- functions -----------------------------------------------------------------------------

    /// A non-generic function is lowered now; a generic one is kept as a template and lowered per instantiation.
    void add_function(const FunctionDeclNode* decl, const ClassDeclNode* owner) {
        const bool generic = !decl->m_type_params.empty() || (owner != nullptr && !owner->m_type_params.empty());
        if (generic) add_template(decl, owner);
        else lower_function(decl, owner, nullptr);
    }

    void add_template(const FunctionDeclNode* decl, const ClassDeclNode* owner) {
        const SymbolID owner_name = decl->m_owner != INVALID_SYMBOL_ID ? decl->m_owner : (owner != nullptr ? owner->m_name : INVALID_SYMBOL_ID);
        std::string name;
        if (owner_name != INVALID_SYMBOL_ID) {
            name = name_of(owner_name);
            if (owner != nullptr && !owner->m_type_params.empty()) {
                name += "<";
                if (!decl->m_owner_args.empty()) {
                    for (size_t i = 0; i < decl->m_owner_args.size(); ++i) {
                        const TypeNode* arg = decl->m_owner_args[i].m_type;
                        name += (i != 0 ? ", " : "") + (arg != nullptr && !arg->m_path.empty() ? name_of(arg->m_path.back()) : std::string("?"));
                    }
                } else {
                    for (size_t i = 0; i < owner->m_type_params.size(); ++i) name += (i != 0 ? ", " : "") + name_of(owner->m_type_params[i].m_name);
                }
                name += ">";
            }
            name += "::";
        } else {
            name = decl->m_namespace == INVALID_SYMBOL_ID ? std::string() : name_of(decl->m_namespace) + "::";
        }
        name += name_of(decl->m_name);
        if (!decl->m_type_params.empty()) {
            name += "<";
            for (size_t i = 0; i < decl->m_type_params.size(); ++i) name += (i != 0 ? ", " : "") + name_of(decl->m_type_params[i].m_name);
            name += ">";
        }
        Function fn;
        fn.m_name = std::move(name);
        fn.m_decl = decl;
        fn.m_is_template = true;
        fn.m_reason_span = decl->m_span;
        m_module.m_functions.push_back(std::move(fn));
    }

    void lower_function(const FunctionDeclNode* decl, const ClassDeclNode* owner, const Instantiation* instantiation) {
        Function fn;
        const SymbolID owner_name = decl->m_owner != INVALID_SYMBOL_ID ? decl->m_owner : (owner != nullptr ? owner->m_name : INVALID_SYMBOL_ID);
        fn.m_name = instantiation != nullptr ? instantiation->m_name
                                             : (owner_name != INVALID_SYMBOL_ID ? name_of(owner_name) + "::" : (decl->m_namespace == INVALID_SYMBOL_ID ? std::string() : name_of(decl->m_namespace) + "::")) + name_of(decl->m_name);
        fn.m_decl = decl;
        fn.m_reason_span = decl->m_span;
        m_module.m_functions.push_back(std::move(fn));
        m_fn = &m_module.m_functions.back();
        m_retypes.clear();

        // One attempt per literal-initialized local that turns out to need another type; bounded by the number of locals.
        for (size_t attempt = 0;; ++attempt) {
            m_scopes.clear();
            m_defaulted.clear();
            m_isolated = false;
            m_subst = instantiation != nullptr ? instantiation->m_bindings : Bindings{};
            m_fn->m_locals.clear();
            m_fn->m_body.clear();
            m_fn->m_param_count = 0;
            try {
                lower_body(decl, owner, owner_name, instantiation);
                m_fn->m_lowered = true;
                break;
            } catch (const RetypeLocal& retype) {
                if (attempt >= 64) {
                    m_fn->m_reason = "the types of literal-initialized locals do not settle";
                    break;
                }
                m_retypes[retype.m_local] = retype.m_type;
            } catch (const NotLowerable& reason) {
                m_fn->m_reason = reason.m_reason;
                m_fn->m_reason_span = reason.m_span;
                break;
            }
        }
        if (!m_fn->m_lowered) {
            m_fn->m_body.clear();
            m_fn->m_locals.clear();
            m_fn->m_param_count = 0;
        }
        m_scopes.clear();
        m_subst.clear();
        m_isolated = false;
        m_fn = nullptr;
    }

    void lower_body(const FunctionDeclNode* decl, const ClassDeclNode* owner, SymbolID owner_name, const Instantiation* instantiation) {
        if (decl->m_is_async) fail("async functions (state machine lowering is not implemented)", decl->m_span);
        if (instantiation == nullptr) {
            if (!decl->m_type_params.empty()) fail("generic function lowered without type arguments", decl->m_span);
            if (owner != nullptr && !owner->m_type_params.empty()) fail("method of a generic type lowered without type arguments", decl->m_span);
        }
        m_fn->m_return = lower_type(decl->m_return_type);
        push_scope();
        if (decl->m_receiver.m_kind != ReceiverKind::None) {
            TypeId self_type = instantiation != nullptr ? instantiation->m_self : NO_TYPE;
            if (self_type == NO_TYPE) {
                if (owner_name == INVALID_SYMBOL_ID) fail("method without an owner type", decl->m_span);
                self_type = named(owner_name);
            }
            switch (decl->m_receiver.m_kind) {
                case ReceiverKind::ByValue: declare(decl->m_receiver.m_self_name, self_type, true, true); break;
                case ReceiverKind::ByConstRef:
                    declare(decl->m_receiver.m_self_name, view(TypeKind::Reference, true, self_type), true, true);
                    break;
                default: declare(decl->m_receiver.m_self_name, view(TypeKind::Reference, false, self_type), true, true); break;
            }
        }
        for (const ParameterDecl& param : decl->m_params) declare(param.m_name, lower_type(param.m_type), true, true);
        m_fn->m_param_count = m_fn->m_locals.size();
        lower_statements(decl->m_body, m_fn->m_body);
        pop_scope();
    }

    // ---- statements ----------------------------------------------------------------------------

    void lower_statements(const std::vector<StmtNode*>& stmts, std::vector<Stmt>& out) {
        for (const StmtNode* stmt : stmts) lower_statement(stmt, out);
    }

    void lower_scoped(const BlockStmt* block, std::vector<Stmt>& out) {
        if (block == nullptr) return;
        push_scope();
        lower_statements(block->m_statements, out);
        pop_scope();
    }

    [[nodiscard]] static Stmt make_stmt(StmtKind kind, SourceSpan span) {
        Stmt stmt;
        stmt.m_kind = kind;
        stmt.m_span = span;
        return stmt;
    }

    void lower_statement(const StmtNode* stmt, std::vector<Stmt>& out) {
        if (const auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::Return, stmt->m_span);
            if (ret->m_value != nullptr) {
                if (ty(m_fn->m_return).m_kind == TypeKind::Never) fail("'return' in a function that never returns", stmt->m_span);
                s.m_expr = coerce(lower_expr(ret->m_value, m_fn->m_return), m_fn->m_return, ret->m_value->m_span);
            }
            out.push_back(std::move(s));
        } else if (const auto* expr_stmt = dynamic_cast<const ExprStmt*>(stmt)) {
            lower_expression_statement(expr_stmt, out);
        } else if (const auto* decl_stmt = dynamic_cast<const DeclStmt*>(stmt)) {
            const auto* var = dynamic_cast<const VarDeclNode*>(decl_stmt->m_decl);
            if (var == nullptr) fail("declarations other than variables inside a function", stmt->m_span);
            Stmt s = make_stmt(StmtKind::Let, stmt->m_span);
            TypeId type = NO_TYPE;
            if (var->m_type != nullptr) {
                type = lower_type(var->m_type);
                if (var->m_initializer != nullptr) s.m_expr = coerce(lower_expr(var->m_initializer, type), type, var->m_initializer->m_span);
            } else {
                if (var->m_initializer == nullptr) fail("variable '" + name_of(var->m_name) + "' has neither a type nor an initializer", stmt->m_span);
                out.push_back(lower_inferred_let(var->m_name, var->m_initializer, var->m_kind == BindingKind::Mutable, stmt->m_span));
                return;
            }
            require_storable(type, stmt->m_span);
            s.m_local = declare(var->m_name, type, var->m_kind == BindingKind::Mutable, false);
            out.push_back(std::move(s));
        } else if (const auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::Block, stmt->m_span);
            lower_scoped(block, s.m_body);
            out.push_back(std::move(s));
        } else if (const auto* if_stmt = dynamic_cast<const IfStmt*>(stmt)) {
            out.push_back(lower_if(if_stmt));
        } else if (const auto* match_stmt = dynamic_cast<const MatchStmt*>(stmt)) {
            out.push_back(lower_match(match_stmt));
        } else if (const auto* while_stmt = dynamic_cast<const WhileStmt*>(stmt)) {
            Stmt s = make_stmt(StmtKind::While, stmt->m_span);
            s.m_expr = as_condition(lower_expr(while_stmt->m_condition, bool_type()), while_stmt->m_condition->m_span);
            lower_scoped(while_stmt->m_body, s.m_body);
            out.push_back(std::move(s));
        } else if (const auto* for_stmt = dynamic_cast<const ForStmt*>(stmt)) {
            lower_for(for_stmt, out);
        } else if (dynamic_cast<const BreakStmt*>(stmt) != nullptr) {
            out.push_back(make_stmt(StmtKind::Break, stmt->m_span));
        } else if (dynamic_cast<const ContinueStmt*>(stmt) != nullptr) {
            out.push_back(make_stmt(StmtKind::Continue, stmt->m_span));
        } else if (const auto* conditional = dynamic_cast<const ConditionalStmt*>(stmt)) {
            if (const ConditionalBranch* branch = m_checker.active_branch(conditional)) {
                Stmt s = make_stmt(StmtKind::Block, stmt->m_span);
                push_scope();
                lower_statements(branch->m_body, s.m_body);
                pop_scope();
                out.push_back(std::move(s));
            }
        } else {
            fail("unsupported statement", stmt->m_span);
        }
    }

    /// `var x = value;` / `x = value;`: the local takes the value's type. A literal gives `i32` / `f64` unless the
    /// local's uses demanded another numeric type on an earlier attempt.
    [[nodiscard]] Stmt lower_inferred_let(SymbolID name, const ExprNode* init, bool is_mutable, SourceSpan span) {
        Stmt s = make_stmt(StmtKind::Let, span);
        const auto next_id = static_cast<LocalId>(m_fn->m_locals.size());
        const bool literal = is_untyped(init) && dynamic_cast<const NullLiteralExpr*>(init) == nullptr;
        TypeId type = NO_TYPE;
        if (const auto retyped = m_retypes.find(next_id); literal && retyped != m_retypes.end()) {
            type = retyped->second;
            s.m_expr = coerce(lower_expr(init, type), type, span);
        } else {
            s.m_expr = lower_expr(init, NO_TYPE);
            type = s.m_expr->m_type;
        }
        require_storable(type, span);
        s.m_local = declare(name, type, is_mutable, false);
        if (literal) m_defaulted.insert(s.m_local);
        return s;
    }

    void require_storable(TypeId type, SourceSpan span) {
        const TypeKind kind = ty(type).m_kind;
        if (kind == TypeKind::Void || kind == TypeKind::Never) fail("a variable cannot hold a value of type '" + m_module.m_types.text(type) + "'", span);
    }

    /// `for (init; condition; step) { body }`: the initializer and the loop share a scope, so they become a Block
    /// (when there is an initializer). The step is its own field, not appended to the body, because `continue` must
    /// still run it.
    void lower_for(const ForStmt* for_stmt, std::vector<Stmt>& out) {
        push_scope();
        Stmt loop = make_stmt(StmtKind::For, for_stmt->m_span);
        Stmt scope = make_stmt(StmtKind::Block, for_stmt->m_span);
        if (for_stmt->m_init != nullptr) lower_statement(for_stmt->m_init, scope.m_body);
        if (for_stmt->m_condition != nullptr) {
            loop.m_expr = as_condition(lower_expr(for_stmt->m_condition, bool_type()), for_stmt->m_condition->m_span);
        }
        lower_scoped(for_stmt->m_body, loop.m_body);
        if (for_stmt->m_step != nullptr) loop.m_step = lower_expr(for_stmt->m_step, NO_TYPE);
        pop_scope();
        if (for_stmt->m_init == nullptr) {
            out.push_back(std::move(loop));
        } else {
            scope.m_body.push_back(std::move(loop));
            out.push_back(std::move(scope));
        }
    }

    /// `match value { Enum::A(x) => {...} _ => {...} }`: each arm gets locals for the payload it names.
    Stmt lower_match(const MatchStmt* match_stmt) {
        Stmt s = make_stmt(StmtKind::Match, match_stmt->m_span);
        s.m_expr = lower_expr(match_stmt->m_subject, NO_TYPE);
        const Type& subject = ty(s.m_expr->m_type);
        const EnumInfo* found = subject.m_kind == TypeKind::Named ? m_module.find_enum(subject.m_name) : nullptr;
        if (found == nullptr) fail("a match on '" + m_module.m_types.text(s.m_expr->m_type) + "' (only non-generic enum values can be matched)", match_stmt->m_subject->m_span);
        const EnumInfo info = *found;   // a copy: lowering an arm may register more enums
        for (const MatchArm& arm : match_stmt->m_arms) {
            MatchCase lowered;
            lowered.m_span = arm.m_span;
            push_scope();
            if (arm.m_wildcard) {
                lowered.m_wildcard = true;
            } else {
                bool known = false;
                for (size_t i = 0; i < info.m_variants.size(); ++i) {
                    if (info.m_variants[i].m_name == arm.m_path.back()) { lowered.m_variant = i; known = true; }
                }
                if (!known) fail("variant '" + name_of(arm.m_path.back()) + "' of '" + name_of(info.m_name) + "'", arm.m_span);
                const EnumVariantInfo& variant = info.m_variants[lowered.m_variant];
                if (arm.m_has_bindings && arm.m_bindings.size() == variant.m_payload.size()) {
                    for (size_t i = 0; i < arm.m_bindings.size(); ++i) {
                        lowered.m_bindings.push_back(arm.m_bindings[i].m_ignored ? NO_LOCAL
                                                                                 : declare(arm.m_bindings[i].m_name, variant.m_payload[i], false, false));
                    }
                }
            }
            lower_scoped(arm.m_body, lowered.m_body);
            pop_scope();
            s.m_arms.push_back(std::move(lowered));
        }
        return s;
    }

    Stmt lower_if(const IfStmt* if_stmt) {
        Stmt s = make_stmt(StmtKind::If, if_stmt->m_span);
        s.m_expr = as_condition(lower_expr(if_stmt->m_condition, bool_type()), if_stmt->m_condition->m_span);
        lower_scoped(if_stmt->m_then, s.m_body);
        if (const auto* chained = dynamic_cast<const IfStmt*>(if_stmt->m_else)) {
            s.m_else.push_back(lower_if(chained));
        } else if (const auto* block = dynamic_cast<const BlockStmt*>(if_stmt->m_else)) {
            lower_scoped(block, s.m_else);
        }
        return s;
    }

    void lower_expression_statement(const ExprStmt* expr_stmt, std::vector<Stmt>& out) {
        // `x = value;` with no `x` in scope declares it: a mutable local of the value's type.
        if (const auto* assign = dynamic_cast<const BinaryExpr*>(expr_stmt->m_expr); assign != nullptr && assign->m_op == TokenKind::Equal) {
            if (const auto* id = dynamic_cast<const IdentifierExpr*>(assign->m_lhs);
                id != nullptr && !find_local(id->m_identifier) && m_pass0.find_global_from(current_namespace(), id->m_identifier) == nullptr) {
                out.push_back(lower_inferred_let(id->m_identifier, assign->m_rhs, true, expr_stmt->m_span));
                return;
            }
        }
        Stmt s = make_stmt(StmtKind::Expr, expr_stmt->m_span);
        s.m_expr = lower_expr(expr_stmt->m_expr, NO_TYPE);
        out.push_back(std::move(s));
    }

    // ---- expressions ---------------------------------------------------------------------------

    /// Type a literal takes: the context's (through an optional) when it fits the literal's kind, else the default.
    [[nodiscard]] TypeId literal_type(TypeId expected, TypeKind kind, TypeId fallback) const {
        if (expected == NO_TYPE) return fallback;
        const Type* type = &ty(expected);
        if (type->m_kind == TypeKind::Optional) type = &ty(type->m_inner);
        if (type->m_kind != kind) return fallback;
        return type == &ty(expected) ? expected : ty(expected).m_inner;
    }

    [[nodiscard]] ExprPtr lower_expr(const ExprNode* e, TypeId expected) {
        if (e == nullptr) fail("missing expression", SourceSpan{});
        if (const auto* literal = dynamic_cast<const IntegerLiteralExpr*>(e)) {
            if (const TypeId real = literal_type(expected, TypeKind::Float, NO_TYPE); real != NO_TYPE) {
                // An integer literal adapts to a floating-point context (`scale: f32 = 1`).
                ExprPtr out = node(ExprKind::FloatLit, real, e->m_span);
                out->m_float = static_cast<double>(literal->m_value);
                return out;
            }
            const bool fits_i32 = literal->m_value >= INT32_MIN && literal->m_value <= INT32_MAX;
            ExprPtr out = node(ExprKind::IntLit, literal_type(expected, TypeKind::Int, prim(fits_i32 ? "i32" : "i64")), e->m_span);
            out->m_int = literal->m_value;
            return out;
        }
        if (const auto* literal = dynamic_cast<const FloatLiteralExpr*>(e)) {
            ExprPtr out = node(ExprKind::FloatLit, literal_type(expected, TypeKind::Float, prim("f64")), e->m_span);
            out->m_float = literal->m_value;
            return out;
        }
        if (const auto* literal = dynamic_cast<const BooleanLiteralExpr*>(e)) {
            ExprPtr out = node(ExprKind::BoolLit, bool_type(), e->m_span);
            out->m_int = literal->m_value ? 1 : 0;
            return out;
        }
        if (const auto* literal = dynamic_cast<const StringLiteralExpr*>(e)) {
            TypeId text_type = named(StringInterner::get().intern("StringView"));
            if (expected != NO_TYPE && ty(expected).m_kind == TypeKind::Pointer) {
                const Type& pointee = ty(ty(expected).m_inner);
                if (pointee.m_kind == TypeKind::Int && pointee.m_bits == 8) text_type = expected;   // a literal doubles as `const* u8`
            }
            ExprPtr out = node(ExprKind::StrLit, text_type, e->m_span);
            out->m_text = std::string(literal->m_value);
            return out;
        }
        if (dynamic_cast<const NullLiteralExpr*>(e) != nullptr) {
            if (expected == NO_TYPE || ty(expected).m_kind != TypeKind::Optional) fail("'null' needs an optional type from its context", e->m_span);
            return node(ExprKind::NullLit, expected, e->m_span);
        }
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(e)) return lower_identifier(id);
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(e)) return lower_unary(unary, expected);
        if (const auto* binary = dynamic_cast<const BinaryExpr*>(e)) return lower_binary(binary, expected);
        if (const auto* cast = dynamic_cast<const CastExpr*>(e)) {
            ExprPtr operand = lower_expr(cast->m_operand, NO_TYPE);
            ExprPtr out = node(ExprKind::Cast, lower_type(cast->m_target), e->m_span);
            out->m_cast = cast->m_cast_kind;
            out->m_operands.push_back(std::move(operand));
            return out;
        }
        if (const auto* member = dynamic_cast<const MemberExpr*>(e)) return lower_member(member);
        if (const auto* index = dynamic_cast<const IndexExpr*>(e)) return lower_index(index);
        if (const auto* call = dynamic_cast<const CallExpr*>(e)) return lower_call(call);
        if (const auto* literal = dynamic_cast<const StructLiteralExpr*>(e)) return lower_struct_literal(literal);
        if (dynamic_cast<const LambdaExpr*>(e) != nullptr) fail("lambdas (closure conversion is not implemented)", e->m_span);
        if (dynamic_cast<const IntrinsicCallExpr*>(e) != nullptr) fail("compile-time intrinsics and reflection (not evaluated yet)", e->m_span);
        if (const auto* path = dynamic_cast<const PathExpr*>(e)) {
            size_t index = 0;
            if (const EnumDeclNode* en = variant_path(path, index)) return lower_enum_value(en, index, {}, e->m_span);
            if (const VarDeclNode* global = m_pass0.find_namespace_global({path->m_segments.begin(), path->m_segments.end()}, current_namespace())) {
                if (global->m_type == nullptr) fail("global '" + name_of(global->m_name) + "' has no declared type", e->m_span);
                ExprPtr out = node(ExprKind::Global, lower_type(global->m_type), e->m_span);
                out->m_name = global_key(global);
                return out;
            }
            fail("qualified paths other than enum variants and namespace constants (not lowerable yet)", e->m_span);
        }
        fail("unsupported expression", e->m_span);
    }

    [[nodiscard]] ExprPtr lower_identifier(const IdentifierExpr* id) {
        if (const auto local = find_local(id->m_identifier)) {
            ExprPtr out = node(ExprKind::Local, m_fn->m_locals[*local].m_type, id->m_span);
            out->m_local = *local;
            return out;
        }
        if (const VarDeclNode* global = m_pass0.find_global_from(current_namespace(), id->m_identifier)) {
            if (global->m_type == nullptr) fail("global '" + name_of(id->m_identifier) + "' has no declared type", id->m_span);
            ExprPtr out = node(ExprKind::Global, lower_type(global->m_type), id->m_span);
            out->m_name = global_key(global);
            return out;
        }
        if (m_pass0.find_function_from(current_namespace(), id->m_identifier) != nullptr) fail("function '" + name_of(id->m_identifier) + "' used as a value (function values are not lowerable yet)", id->m_span);
        fail("unresolved name '" + name_of(id->m_identifier) + "'", id->m_span);
    }

    [[nodiscard]] ExprPtr lower_unary(const UnaryExpr* unary, TypeId expected) {
        switch (unary->m_op) {
            case TokenKind::Minus: {
                ExprPtr operand = lower_expr(unary->m_operand, expected);
                const TypeId type = operand->m_type;
                ExprPtr out = wrap_one(ExprKind::Unary, type, std::move(operand));
                out->m_op = TokenKind::Minus;
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Bang: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                if (ty(operand->m_type).m_kind == TypeKind::Optional) {
                    // `!a` on an optional: true when it is empty.
                    ExprPtr null = node(ExprKind::NullLit, operand->m_type, unary->m_span);
                    ExprPtr test = node(ExprKind::Binary, bool_type(), unary->m_span);
                    test->m_op = TokenKind::EqualEqual;
                    test->m_operands.push_back(std::move(operand));
                    test->m_operands.push_back(std::move(null));
                    return test;
                }
                const TypeId type = operand->m_type;
                ExprPtr out = wrap_one(ExprKind::Unary, type, std::move(operand));
                out->m_op = TokenKind::Bang;
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Ampersand: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                const TypeId ref_type = view(TypeKind::Reference, false, operand->m_type);
                ExprPtr out = wrap_one(ExprKind::AddrOf, ref_type, std::move(operand));
                out->m_span = unary->m_span;
                return out;
            }
            case TokenKind::Star: {
                ExprPtr operand = lower_expr(unary->m_operand, NO_TYPE);
                if (ty(operand->m_type).m_kind == TypeKind::Optional) {
                    const TypeId inner = ty(operand->m_type).m_inner;
                    operand = wrap_one(ExprKind::Unwrap, inner, std::move(operand));
                }
                const Type& type = ty(operand->m_type);
                if (type.m_kind != TypeKind::Pointer && type.m_kind != TypeKind::Reference) {
                    fail("cannot dereference a value of type '" + m_module.m_types.text(operand->m_type) + "'", unary->m_span);
                }
                const TypeId deref_type = type.m_inner;
                ExprPtr out = wrap_one(ExprKind::Deref, deref_type, std::move(operand));
                out->m_span = unary->m_span;
                return out;
            }
            default: fail("unsupported unary operator", unary->m_span);
        }
    }

    [[nodiscard]] ExprPtr lower_binary(const BinaryExpr* binary, TypeId expected) {
        const SourceSpan span = binary->m_span;
        if (binary->m_op == TokenKind::Equal) return lower_assignment(binary);
        if (binary->m_op == TokenKind::AmpAmp || binary->m_op == TokenKind::PipePipe) {
            ExprPtr lhs = as_condition(lower_expr(binary->m_lhs, bool_type()), binary->m_lhs->m_span);
            ExprPtr rhs = as_condition(lower_expr(binary->m_rhs, bool_type()), binary->m_rhs->m_span);
            ExprPtr out = node(ExprKind::Binary, bool_type(), span);
            out->m_op = binary->m_op;
            out->m_operands.push_back(std::move(lhs));
            out->m_operands.push_back(std::move(rhs));
            return out;
        }
        const bool comparison = is_comparison(binary->m_op);
        if (!comparison && !is_arithmetic(binary->m_op)) fail("unsupported binary operator", span);

        // A literal takes its type from the other operand (or from the context when both are literals).
        const bool lhs_untyped = is_untyped(binary->m_lhs);
        const bool rhs_untyped = is_untyped(binary->m_rhs);
        const TypeId hint = comparison ? NO_TYPE : expected;
        ExprPtr lhs;
        ExprPtr rhs;
        if (lhs_untyped && !rhs_untyped) {
            rhs = lower_expr(binary->m_rhs, NO_TYPE);
            lhs = lower_expr(binary->m_lhs, rhs->m_type);
        } else {
            lhs = lower_expr(binary->m_lhs, lhs_untyped ? hint : NO_TYPE);
            rhs = lower_expr(binary->m_rhs, lhs->m_type);
        }
        const bool equality = binary->m_op == TokenKind::EqualEqual || binary->m_op == TokenKind::ExclaimEqual;
        unify(lhs, rhs, equality, span);
        const EnumInfo* enum_info = ty(lhs->m_type).m_kind == TypeKind::Named ? m_module.find_enum(ty(lhs->m_type).m_name) : nullptr;
        if (const EnumInfo* info = enum_info) {
            if (!equality) fail("ordering of enum values (not defined)", span);
            if (!info->is_plain()) fail("comparing values of enum '" + name_of(info->m_name) + "', whose variants carry payloads (equality is not defined for them)", span);
        }
        const TypeId type = comparison ? bool_type() : lhs->m_type;
        ExprPtr out = node(ExprKind::Binary, type, span);
        out->m_op = binary->m_op;
        out->m_operands.push_back(std::move(lhs));
        out->m_operands.push_back(std::move(rhs));
        return out;
    }

    /// A place that can be assigned to or have its address taken: a local, a global, a field, a dereference or an index.
    [[nodiscard]] ExprPtr lower_place(const ExprNode* target) {
        if (dynamic_cast<const IdentifierExpr*>(target) != nullptr || dynamic_cast<const MemberExpr*>(target) != nullptr ||
            dynamic_cast<const IndexExpr*>(target) != nullptr) {
            return lower_expr(target, NO_TYPE);
        }
        if (const auto* unary = dynamic_cast<const UnaryExpr*>(target); unary != nullptr && unary->m_op == TokenKind::Star) {
            return lower_expr(target, NO_TYPE);
        }
        fail("assignment to something that is not a place", target->m_span);
    }

    [[nodiscard]] ExprPtr lower_assignment(const BinaryExpr* assign) {
        if (const auto* id = dynamic_cast<const IdentifierExpr*>(assign->m_lhs);
            id != nullptr && !find_local(id->m_identifier) && m_pass0.find_global_from(current_namespace(), id->m_identifier) == nullptr) {
            fail("declaration '" + name_of(id->m_identifier) + " = ...' used as a value", assign->m_span);
        }
        ExprPtr place = lower_place(assign->m_lhs);
        // The value is evaluated first (as in the checker), then the place is written.
        ExprPtr value = lower_expr(assign->m_rhs, place->m_type);
        if (value->m_type != place->m_type && !convertible(value->m_type, place->m_type)) maybe_retype(*place, value->m_type);
        value = coerce(std::move(value), place->m_type, assign->m_rhs->m_span);
        ExprPtr out = node(ExprKind::Assign, void_type(), assign->m_span);
        out->m_operands.push_back(std::move(place));
        out->m_operands.push_back(std::move(value));
        return out;
    }

    /// The type of a field of the class behind `cls`, written in terms of the class's own type parameters.
    [[nodiscard]] TypeId field_type(const ClassView& cls, const ParameterDecl& field) {
        SubstScope scope(*this, cls.m_bindings);
        return lower_type(field.m_type);
    }

    [[nodiscard]] ExprPtr lower_member(const MemberExpr* member) {
        ExprPtr object = to_value(lower_expr(member->m_object, NO_TYPE));
        const ClassView cls = class_view(object->m_type, member->m_span);
        for (size_t i = 0; i < cls.m_decl->m_fields.size(); ++i) {
            if (cls.m_decl->m_fields[i].m_name != member->m_member) continue;
            ExprPtr out = node(ExprKind::Field, field_type(cls, cls.m_decl->m_fields[i]), member->m_span);
            out->m_index = i;
            out->m_name = member->m_member;
            out->m_operands.push_back(std::move(object));
            return out;
        }
        fail("'" + name_of(cls.m_decl->m_name) + "' has no field '" + name_of(member->m_member) + "' (methods are only lowerable when called)", member->m_span);
    }

    [[nodiscard]] ExprPtr lower_index(const IndexExpr* index) {
        ExprPtr base = lower_expr(index->m_base, NO_TYPE);
        if (ty(base->m_type).m_kind == TypeKind::Optional) {
            const TypeId inner = ty(base->m_type).m_inner;
            base = wrap_one(ExprKind::Unwrap, inner, std::move(base));
        }
        const TypeKind base_kind = ty(base->m_type).m_kind;
        if (base_kind != TypeKind::Pointer && base_kind != TypeKind::Reference) {
            fail("indexing a value of type '" + m_module.m_types.text(base->m_type) + "' (only pointers and references can be indexed)", index->m_span);
        }
        const TypeId element = ty(base->m_type).m_inner;   // copied: interning below may move the type table
        ExprPtr position = lower_expr(index->m_index, prim("usize"));
        if (ty(position->m_type).m_kind != TypeKind::Int) fail("an index must be an integer", index->m_index->m_span);
        ExprPtr out = node(ExprKind::Index, element, index->m_span);
        out->m_operands.push_back(std::move(base));
        out->m_operands.push_back(std::move(position));
        return out;
    }

    [[nodiscard]] ExprPtr lower_call(const CallExpr* call) {
        if (const auto* path = dynamic_cast<const PathExpr*>(call->m_callee)) {
            size_t index = 0;
            if (const EnumDeclNode* en = variant_path(path, index)) return lower_enum_value(en, index, call->m_args, call->m_span);
        }
        const ResolvedCall* resolved = m_checker.find_resolved_call(call);
        if (resolved == nullptr) fail("call that is not bound to a function (function values, built-ins and calls into unseen modules are not lowerable yet)", call->m_span);
        const FunctionDeclNode* declaration = resolved->m_callee;
        const ReceiverKind kind = declaration->m_receiver.m_kind;

        // The receiver is lowered first: its type also tells which instance of a generic owner is meant.
        ExprPtr receiver;
        if (resolved->m_receiver != nullptr) {
            if (kind == ReceiverKind::None) fail("method call of '" + resolved->m_display + "', which has no receiver", call->m_span);
            receiver = lower_expr(resolved->m_receiver, NO_TYPE);
            if (ty(receiver->m_type).m_kind == TypeKind::Optional) {
                const TypeId inner = ty(receiver->m_type).m_inner;
                receiver = wrap_one(ExprKind::Unwrap, inner, std::move(receiver));
            }
        } else if (kind != ReceiverKind::None) {
            fail("call of method '" + resolved->m_display + "' without a receiver", call->m_span);
        }

        const ClassDeclNode* owner = owner_of(declaration);
        std::optional<ClassView> receiver_class;
        if (receiver != nullptr && (owner == nullptr || !owner->m_type_params.empty())) {
            receiver_class = lookup_class(value_type(receiver->m_type));
            if (owner == nullptr && receiver_class.has_value()) owner = receiver_class->m_decl;
        }
        const bool generic_owner = owner != nullptr && !owner->m_type_params.empty();
        const bool generic_fn = !declaration->m_type_params.empty();

        // Generic callees are instantiated from their definition (its spelling of the type parameters is the one its body uses).
        const FunctionDeclNode* callee = declaration;
        if (generic_owner || generic_fn) {
            callee = find_definition(declaration);
            if (callee == nullptr) fail("definition of generic '" + resolved->m_display + "' is not visible in this module (cross-module instantiation is not implemented)", call->m_span);
        }

        // Type arguments the callee's signature may mention; bound from the receiver, written arguments and the call's arguments.
        Bindings bindings;
        std::unordered_set<SymbolID> generics;
        std::vector<SymbolID> owner_params;   // the owner's parameters as the callee spells them, in order
        if (generic_owner) {
            if (!callee->m_owner_args.empty()) {
                for (const GenericArg& arg : callee->m_owner_args) {
                    if (arg.m_type == nullptr || arg.m_type->m_kind != TypeNodeKind::Path || arg.m_type->m_path.size() != 1 || !arg.m_type->m_generic_args.empty()) {
                        fail("method of a generic type whose owner arguments are not plain type parameters", call->m_span);
                    }
                    owner_params.push_back(arg.m_type->m_path[0]);
                }
            } else {
                for (const GenericParam& param : owner->m_type_params) owner_params.push_back(param.m_name);
            }
            if (owner_params.size() != owner->m_type_params.size()) fail("owner arguments of '" + resolved->m_display + "' do not match the type's parameters", call->m_span);
            for (const SymbolID name : owner_params) generics.insert(name);
            const auto* path = dynamic_cast<const PathExpr*>(call->m_callee);
            if (receiver_class.has_value() && receiver_class->m_decl == owner && receiver_class->m_args.size() == owner_params.size()) {
                for (size_t i = 0; i < owner_params.size(); ++i) bindings[owner_params[i]] = receiver_class->m_args[i];
            } else if (path != nullptr && !path->m_generic_args.empty()) {
                if (path->m_generic_args.size() != owner_params.size()) fail("wrong number of type arguments for '" + name_of(owner->m_name) + "'", call->m_span);
                for (size_t i = 0; i < owner_params.size(); ++i) {
                    const GenericArg& arg = path->m_generic_args[i];
                    if (arg.m_type == nullptr || arg.m_name != INVALID_SYMBOL_ID) fail("constant generic arguments (not lowerable yet)", call->m_span);
                    bindings[owner_params[i]] = lower_type(arg.m_type);
                }
            }
        }
        for (const GenericParam& param : callee->m_type_params) generics.insert(param.m_name);

        // Arguments: typed ones first (they bind the type arguments), then literals and defaults (which adapt to them).
        const size_t count = resolved->m_slots.size();
        std::vector<ExprPtr> values(count);
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < count; ++i) {
                const ResolvedArgument& slot = resolved->m_slots[i];
                if ((slot.m_is_default || is_untyped(slot.m_value)) != (pass == 1)) continue;
                const TypeNode* param_node = callee->m_params[i].m_type;
                const bool unbound = mentions_unbound(param_node, generics, bindings);
                TypeId expected = NO_TYPE;
                if (!unbound) {
                    SubstScope scope(*this, bindings);
                    expected = lower_type(param_node);
                }
                const bool saved_isolated = m_isolated;
                m_isolated = slot.m_is_default;   // a default is written in the callee's world: no locals of the caller
                if (slot.m_is_default) {
                    SubstScope scope(*this, bindings);
                    values[i] = lower_expr(slot.m_value, expected);
                } else {
                    values[i] = lower_expr(slot.m_value, expected);
                }
                m_isolated = saved_isolated;
                if (unbound) infer(param_node, values[i]->m_type, bindings, generics);
            }
        }
        for (const SymbolID name : generics) {
            if (!bindings.contains(name)) fail("cannot infer the type argument '" + name_of(name) + "' of '" + resolved->m_display + "'", call->m_span);
        }

        // The signature under the type arguments.
        TypeId return_type = NO_TYPE;
        std::vector<TypeId> param_types(count, NO_TYPE);
        {
            SubstScope scope(*this, bindings);
            return_type = lower_type(callee->m_return_type);
            for (size_t i = 0; i < count; ++i) param_types[i] = lower_type(callee->m_params[i].m_type);
        }

        // A generic callee is lowered once per set of type arguments: name the instance and queue it.
        std::string display = resolved->m_display;
        if (generic_owner || generic_fn) {
            TypeId self_type = NO_TYPE;
            display.clear();
            if (owner != nullptr) {
                if (generic_owner) {
                    std::vector<TypeId> owner_args;
                    for (const SymbolID name : owner_params) owner_args.push_back(bindings.at(name));
                    self_type = instance_of(owner, owner_args);
                    display = m_module.m_types.text(self_type);
                } else {
                    display = name_of(owner->m_name);
                }
                display += "::";
            } else if (callee->m_namespace != INVALID_SYMBOL_ID) {
                display = name_of(callee->m_namespace) + "::";
            }
            display += name_of(callee->m_name);
            if (generic_fn) {
                display += "<";
                for (size_t i = 0; i < callee->m_type_params.size(); ++i) {
                    display += (i != 0 ? ", " : "") + m_module.m_types.text(bindings.at(callee->m_type_params[i].m_name));
                }
                display += ">";
            }
            request_instance(callee, owner, bindings, self_type, display, call->m_span);
        }

        ExprPtr out = node(ExprKind::Call, return_type, call->m_span);
        out->m_callee = callee;
        out->m_text = display;
        out->m_name = callee->m_name;

        if (receiver != nullptr) {
            if (kind == ReceiverKind::ByValue) {
                out->m_operands.push_back(to_value(std::move(receiver)));
            } else {
                if (const TypeKind object_kind = ty(receiver->m_type).m_kind; object_kind != TypeKind::Pointer && object_kind != TypeKind::Reference) {
                    const TypeId ref_type = view(TypeKind::Reference, false, receiver->m_type);
                    receiver = wrap_one(ExprKind::AddrOf, ref_type, std::move(receiver));
                }
                const TypeId inner = ty(receiver->m_type).m_inner;
                const TypeId target_view = view(TypeKind::Reference, kind == ReceiverKind::ByConstRef, inner);
                out->m_operands.push_back(coerce(std::move(receiver), target_view, call->m_span));
            }
        }
        for (size_t i = 0; i < count; ++i) {
            const SourceSpan span = resolved->m_slots[i].m_value != nullptr ? resolved->m_slots[i].m_value->m_span : call->m_span;
            out->m_operands.push_back(coerce(std::move(values[i]), param_types[i], span));
        }
        return out;
    }

    [[nodiscard]] ExprPtr lower_struct_literal(const StructLiteralExpr* literal) {
        const TypeId type = lower_type(literal->m_type);
        const ClassView cls = class_view(type, literal->m_span);
        ExprPtr out = node(ExprKind::StructLit, type, literal->m_span);
        for (const ParameterDecl& field : cls.m_decl->m_fields) {
            const TypeId declared = field_type(cls, field);
            const FieldInit* init = nullptr;
            for (const FieldInit& candidate : literal->m_fields) {
                if (candidate.m_name == field.m_name) init = &candidate;
            }
            if (init != nullptr) {
                out->m_operands.push_back(coerce(lower_expr(init->m_value, declared), declared, init->m_span));
            } else if (ty(declared).m_kind == TypeKind::Optional) {
                out->m_operands.push_back(node(ExprKind::NullLit, declared, literal->m_span));   // an omitted optional field starts as null
            } else {
                fail("missing initializer for field '" + name_of(field.m_name) + "'", literal->m_span);
            }
        }
        return out;
    }
};

} // namespace

Module lower_module(const TypeCheckerPass2& checker, const SymbolTablePass0& pass0) {
    Module module;
    Lowerer(checker, pass0, module).run();
    return module;
}

} // namespace fool::hir
