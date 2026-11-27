//
// HIR -> MIR: control-flow graph construction.
//

/**
 * @file Build.cpp
 * @brief Turns every lowered HIR function into basic blocks of simple statements.
 *
 * Rules the builder keeps:
 *   - Evaluation order is the HIR order (operands left to right, an assignment's value before its place). A value read
 *     from a place is copied into a temporary when a later sibling operand contains a call, because the call could
 *     change what the place holds before the consuming statement runs.
 *   - After a `return`, `break` or `continue`, building continues in a fresh block that nothing jumps to; blocks that
 *     are unreachable from the entry are removed at the end, so they never reach the dump.
 *   - `&&` and `||` become branches that write a bool temporary; `for` keeps its step in a block of its own, which
 *     `continue` jumps to.
 */

module;

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module fool.mir;

namespace fool::mir {

namespace {

using HirExpr = hir::Expr;
using HirExprPtr = std::unique_ptr<hir::Expr>;
using HirStmt = hir::Stmt;

/// True when evaluating the expression can run a call.
bool has_call(const HirExpr& e) {
    if (e.m_kind == hir::ExprKind::Call) return true;
    for (const HirExprPtr& operand : e.m_operands) {
        if (has_call(*operand)) return true;
    }
    return false;
}

class Builder {
    // 1. Member variables
    const hir::Module& m_hir;
    const hir::Function& m_source;
    Function m_fn;
    BlockId m_cur{0};

    struct Loop {
        BlockId m_continue{NO_BLOCK};
        BlockId m_break{NO_BLOCK};
    };
    std::vector<Loop> m_loops;

public:
    // 2. Constructors
    Builder(const hir::Module& module, const hir::Function& source) : m_hir(module), m_source(source) {}

    // 3. Member functions
    Function build() {
        m_fn.m_name = m_source.m_name;
        m_fn.m_decl = m_source.m_decl;
        m_fn.m_param_count = m_source.m_param_count;
        m_fn.m_return = m_source.m_return;
        m_fn.m_lowered = true;
        for (const hir::Local& local : m_source.m_locals) {
            Local copy;
            copy.m_name = local.m_name;
            copy.m_type = local.m_type;
            copy.m_mutable = local.m_mutable;
            copy.m_is_param = local.m_is_param;
            m_fn.m_locals.push_back(copy);
        }
        m_cur = new_block();
        lower_block(m_source.m_body);
        if (!m_fn.m_blocks[m_cur].m_terminated) {
            Terminator end;
            if (m_hir.m_types.get(m_source.m_return).m_kind == hir::TypeKind::Void) {
                end.m_kind = TerminatorKind::Return;
            } else {
                end.m_kind = TerminatorKind::Unreachable;   // the checker proved every path returns
            }
            terminate(std::move(end));
        }
        prune_unreachable();
        return std::move(m_fn);
    }

private:
    // ---- blocks and temporaries ----------------------------------------------------------------

    BlockId new_block() {
        m_fn.m_blocks.emplace_back();
        return static_cast<BlockId>(m_fn.m_blocks.size() - 1);
    }

    void terminate(Terminator terminator) {
        Block& block = m_fn.m_blocks[m_cur];
        block.m_terminator = std::move(terminator);
        block.m_terminated = true;
    }

    void jump(BlockId target) {
        Terminator go;
        go.m_kind = TerminatorKind::Goto;
        go.m_target = target;
        terminate(std::move(go));
    }

    void branch(Operand condition, BlockId yes, BlockId no, SourceSpan span) {
        Terminator split;
        split.m_kind = TerminatorKind::Branch;
        split.m_span = span;
        split.m_value = std::move(condition);
        split.m_target = yes;
        split.m_else = no;
        terminate(std::move(split));
    }

    void emit(Statement statement) { m_fn.m_blocks[m_cur].m_statements.push_back(std::move(statement)); }

    LocalId temp(TypeId type) {
        Local local;
        local.m_type = type;
        m_fn.m_locals.push_back(local);
        return static_cast<LocalId>(m_fn.m_locals.size() - 1);
    }

    // ---- places and operands -------------------------------------------------------------------

    Place local_place(LocalId id) const {
        Place place;
        place.m_local = id;
        place.m_base_type = m_fn.m_locals[id].m_type;
        return place;
    }

    static Operand read(Place place) {
        Operand operand;
        operand.m_kind = OperandKind::Read;
        operand.m_type = place.type();
        operand.m_place = std::move(place);
        return operand;
    }

    static Operand unit(TypeId type) {
        Operand operand;
        operand.m_kind = OperandKind::Unit;
        operand.m_type = type;
        return operand;
    }

    void assign(Place place, Rvalue value, SourceSpan span) {
        Statement statement;
        statement.m_kind = StatementKind::Assign;
        statement.m_span = span;
        statement.m_place = std::move(place);
        statement.m_value = std::move(value);
        emit(std::move(statement));
    }

    static Rvalue use(Operand operand) {
        Rvalue value;
        value.m_kind = RvalueKind::Use;
        value.m_type = operand.m_type;
        value.m_operands.push_back(std::move(operand));
        return value;
    }

    /// Copies the operand into a fresh temporary (so that later statements cannot change it).
    Operand spill(Operand operand, SourceSpan span) {
        const TypeId type = operand.m_type;
        const LocalId slot = temp(type);
        assign(local_place(slot), use(std::move(operand)), span);
        return read(local_place(slot));
    }

    Operand spill_rvalue(Rvalue value, SourceSpan span) {
        const TypeId type = value.m_type;
        const LocalId slot = temp(type);
        assign(local_place(slot), std::move(value), span);
        return read(local_place(slot));
    }

    /// The place an operand reads; a constant gets a temporary first.
    Place place_from(Operand operand, SourceSpan span) {
        if (operand.m_kind == OperandKind::Read) return std::move(operand.m_place);
        Operand slot = spill(std::move(operand), span);
        return std::move(slot.m_place);
    }

    /// A local that holds the operand's value, for an `Index` projection.
    LocalId index_local(Operand operand, SourceSpan span) {
        if (operand.m_kind == OperandKind::Read && operand.m_place.m_projections.empty() && !operand.m_place.is_global()) {
            return operand.m_place.m_local;
        }
        Operand slot = spill(std::move(operand), span);
        return slot.m_place.m_local;
    }

    Place place_of(const HirExpr& e) {
        switch (e.m_kind) {
            case hir::ExprKind::Local: return local_place(e.m_local);
            case hir::ExprKind::Global: {
                Place place;
                place.m_global = e.m_name;
                place.m_base_type = e.m_type;
                return place;
            }
            case hir::ExprKind::Field: {
                Place place = place_of(*e.m_operands[0]);
                Projection projection;
                projection.m_kind = ProjectionKind::Field;
                projection.m_type = e.m_type;
                projection.m_index = e.m_index;
                projection.m_name = e.m_name;
                place.m_projections.push_back(projection);
                return place;
            }
            case hir::ExprKind::Deref: {
                Place place = place_from(lower_value(*e.m_operands[0]), e.m_span);
                Projection projection;
                projection.m_kind = ProjectionKind::Deref;
                projection.m_type = e.m_type;
                place.m_projections.push_back(projection);
                return place;
            }
            case hir::ExprKind::Index: {
                Operand pointer = lower_value(*e.m_operands[0]);
                if (has_call(*e.m_operands[1]) && needs_copy(pointer)) pointer = spill(std::move(pointer), e.m_span);
                Operand index = lower_value(*e.m_operands[1]);
                Place place = place_from(std::move(pointer), e.m_span);
                Projection projection;
                projection.m_kind = ProjectionKind::Index;
                projection.m_type = e.m_type;
                projection.m_local = index_local(std::move(index), e.m_span);
                place.m_projections.push_back(projection);
                return place;
            }
            default: return place_from(lower_value(e), e.m_span);
        }
    }

    // ---- expressions ---------------------------------------------------------------------------

    /// A read of a variable or memory can be changed by a later call; a temporary is written once and never changes.
    bool needs_copy(const Operand& operand) const {
        if (operand.m_kind != OperandKind::Read) return false;
        const Place& place = operand.m_place;
        const bool temporary = !place.is_global() && place.m_projections.empty() && place.m_local >= m_source.m_locals.size();
        return !temporary;
    }

    /// Lowers operands left to right; a place read is copied out when a later operand calls something.
    std::vector<Operand> lower_operands(const std::vector<HirExprPtr>& expressions) {
        std::vector<Operand> operands;
        for (size_t i = 0; i < expressions.size(); ++i) {
            Operand operand = lower_value(*expressions[i]);
            bool later_call = false;
            for (size_t j = i + 1; j < expressions.size(); ++j) later_call = later_call || has_call(*expressions[j]);
            if (later_call && needs_copy(operand)) operand = spill(std::move(operand), expressions[i]->m_span);
            operands.push_back(std::move(operand));
        }
        return operands;
    }

    Operand lower_short_circuit(const HirExpr& e) {
        const bool is_and = e.m_op == TokenKind::AmpAmp;
        const LocalId result = temp(e.m_type);
        Operand lhs = lower_value(*e.m_operands[0]);
        const BlockId rhs_block = new_block();
        const BlockId short_block = new_block();
        const BlockId join = new_block();
        branch(std::move(lhs), is_and ? rhs_block : short_block, is_and ? short_block : rhs_block, e.m_span);

        m_cur = short_block;
        Operand decided;
        decided.m_kind = OperandKind::Bool;
        decided.m_type = e.m_type;
        decided.m_int = is_and ? 0 : 1;
        assign(local_place(result), use(std::move(decided)), e.m_span);
        jump(join);

        m_cur = rhs_block;
        Operand rhs = lower_value(*e.m_operands[1]);
        assign(local_place(result), use(std::move(rhs)), e.m_span);
        jump(join);

        m_cur = join;
        return read(local_place(result));
    }

    /// Lowers an expression to the rvalue that computes it (its operands are already simple); nothing is stored yet.
    Rvalue lower_rvalue(const HirExpr& e) {
        Rvalue value;
        value.m_type = e.m_type;
        switch (e.m_kind) {
            case hir::ExprKind::Unary: {
                value.m_kind = RvalueKind::Unary;
                value.m_op = e.m_op;
                value.m_operands = lower_operands(e.m_operands);
                return value;
            }
            case hir::ExprKind::Binary: {
                if (e.m_op == TokenKind::AmpAmp || e.m_op == TokenKind::PipePipe) return use(lower_short_circuit(e));
                value.m_kind = RvalueKind::Binary;
                value.m_op = e.m_op;
                value.m_operands = lower_operands(e.m_operands);
                return value;
            }
            case hir::ExprKind::Convert: value.m_kind = RvalueKind::Convert; break;
            case hir::ExprKind::Cast:
                value.m_kind = RvalueKind::Cast;
                value.m_cast = e.m_cast;
                break;
            case hir::ExprKind::Unwrap: value.m_kind = RvalueKind::Unwrap; break;
            case hir::ExprKind::Wrap: value.m_kind = RvalueKind::Wrap; break;
            case hir::ExprKind::AddrOf:
                value.m_kind = RvalueKind::AddrOf;
                value.m_place = place_of(*e.m_operands[0]);
                return value;
            case hir::ExprKind::Call:
                value.m_kind = RvalueKind::Call;
                value.m_text = e.m_text;
                value.m_callee = e.m_callee;
                value.m_operands = lower_operands(e.m_operands);
                return value;
            case hir::ExprKind::StructLit:
                value.m_kind = RvalueKind::Aggregate;
                value.m_operands = lower_operands(e.m_operands);
                return value;
            case hir::ExprKind::EnumLit:
                value.m_kind = RvalueKind::Variant;
                value.m_index = e.m_index;
                value.m_text = e.m_text;
                value.m_operands = lower_operands(e.m_operands);
                return value;
            default: return use(lower_value(e));
        }
        value.m_operands = lower_operands(e.m_operands);   // Convert, Cast, Unwrap, Wrap: one operand
        return value;
    }

    /// Lowers an expression to something a statement can name: a constant, or a read of a place.
    Operand lower_value(const HirExpr& e) {
        Operand operand;
        operand.m_type = e.m_type;
        switch (e.m_kind) {
            case hir::ExprKind::IntLit:
                operand.m_kind = OperandKind::Int;
                operand.m_int = e.m_int;
                return operand;
            case hir::ExprKind::FloatLit:
                operand.m_kind = OperandKind::Float;
                operand.m_float = e.m_float;
                return operand;
            case hir::ExprKind::BoolLit:
                operand.m_kind = OperandKind::Bool;
                operand.m_int = e.m_int;
                return operand;
            case hir::ExprKind::StrLit:
                operand.m_kind = OperandKind::Str;
                operand.m_text = e.m_text;
                return operand;
            case hir::ExprKind::NullLit:
                operand.m_kind = OperandKind::Null;
                return operand;
            case hir::ExprKind::Local:
            case hir::ExprKind::Global:
            case hir::ExprKind::Field:
            case hir::ExprKind::Index:
            case hir::ExprKind::Deref: return read(place_of(e));
            case hir::ExprKind::Binary:
                if (e.m_op == TokenKind::AmpAmp || e.m_op == TokenKind::PipePipe) return lower_short_circuit(e);
                break;
            case hir::ExprKind::Assign:
                lower_assignment(e);
                return unit(e.m_type);
            case hir::ExprKind::Call:
                if (m_hir.m_types.get(e.m_type).m_kind == hir::TypeKind::Void) {
                    evaluate(lower_rvalue(e), e.m_span);
                    return unit(e.m_type);
                }
                break;
            default: break;
        }
        return spill_rvalue(lower_rvalue(e), e.m_span);
    }

    void evaluate(Rvalue value, SourceSpan span) {
        Statement statement;
        statement.m_kind = StatementKind::Eval;
        statement.m_span = span;
        statement.m_value = std::move(value);
        emit(std::move(statement));
    }

    /// `place = value`: the value is evaluated first, then the place (as in the HIR).
    void lower_assignment(const HirExpr& e) {
        const HirExpr& target = *e.m_operands[0];
        const HirExpr& source = *e.m_operands[1];
        // A call while computing the place would run before a not-yet-stored value is read: store the value first.
        Rvalue value = has_call(target) ? use(lower_value(source)) : lower_rvalue(source);
        Place place = place_of(target);
        assign(std::move(place), std::move(value), e.m_span);
    }

    /// An expression statement: evaluated for its effects only.
    void lower_effect(const HirExpr& e) {
        if (e.m_kind == hir::ExprKind::Assign) {
            lower_assignment(e);
        } else if (e.m_kind == hir::ExprKind::Call) {
            evaluate(lower_rvalue(e), e.m_span);
        } else {
            (void)lower_value(e);
        }
    }

    // ---- statements ----------------------------------------------------------------------------

    void lower_block(const std::vector<HirStmt>& body) {
        for (const HirStmt& stmt : body) lower_statement(stmt);
    }

    void lower_statement(const HirStmt& s) {
        switch (s.m_kind) {
            case hir::StmtKind::Let:
                if (s.m_expr) {
                    Rvalue value = lower_rvalue(*s.m_expr);
                    assign(local_place(s.m_local), std::move(value), s.m_span);
                }
                break;
            case hir::StmtKind::Expr: lower_effect(*s.m_expr); break;
            case hir::StmtKind::Return: {
                Terminator done;
                done.m_kind = TerminatorKind::Return;
                done.m_span = s.m_span;
                if (s.m_expr) {
                    done.m_value = lower_value(*s.m_expr);
                    done.m_has_value = true;
                }
                terminate(std::move(done));
                m_cur = new_block();   // whatever follows is dead code
                break;
            }
            case hir::StmtKind::If: {
                Operand condition = lower_value(*s.m_expr);
                const BlockId then_block = new_block();
                const BlockId else_block = s.m_else.empty() ? NO_BLOCK : new_block();
                const BlockId join = new_block();
                branch(std::move(condition), then_block, s.m_else.empty() ? join : else_block, s.m_span);
                m_cur = then_block;
                lower_block(s.m_body);
                jump(join);
                if (!s.m_else.empty()) {
                    m_cur = else_block;
                    lower_block(s.m_else);
                    jump(join);
                }
                m_cur = join;
                break;
            }
            case hir::StmtKind::While: {
                const BlockId header = new_block();
                const BlockId body = new_block();
                const BlockId exit = new_block();
                jump(header);
                m_cur = header;
                Operand condition = lower_value(*s.m_expr);
                branch(std::move(condition), body, exit, s.m_span);
                m_loops.push_back({header, exit});
                m_cur = body;
                lower_block(s.m_body);
                jump(header);
                m_loops.pop_back();
                m_cur = exit;
                break;
            }
            case hir::StmtKind::For: {
                const BlockId header = new_block();
                const BlockId body = new_block();
                const BlockId latch = new_block();   // the step: `continue` lands here
                const BlockId exit = new_block();
                jump(header);
                m_cur = header;
                if (s.m_expr) {
                    Operand condition = lower_value(*s.m_expr);
                    branch(std::move(condition), body, exit, s.m_span);
                } else {
                    jump(body);
                }
                m_loops.push_back({latch, exit});
                m_cur = body;
                lower_block(s.m_body);
                jump(latch);
                m_loops.pop_back();
                m_cur = latch;
                if (s.m_step) lower_effect(*s.m_step);
                jump(header);
                m_cur = exit;
                break;
            }
            case hir::StmtKind::Break:
                jump(m_loops.back().m_break);
                m_cur = new_block();
                break;
            case hir::StmtKind::Continue:
                jump(m_loops.back().m_continue);
                m_cur = new_block();
                break;
            case hir::StmtKind::Block: lower_block(s.m_body); break;
            case hir::StmtKind::Match: lower_match(s); break;
        }
    }

    /// `match`: the value is copied into a temporary, a switch picks the arm by variant, and an arm that names payload
    /// values copies them out of the temporary into its locals before its body runs.
    void lower_match(const HirStmt& s) {
        Operand subject = spill(lower_value(*s.m_expr), s.m_span);
        const Place held = subject.m_place;
        const hir::Type& enum_type = m_hir.m_types.get(subject.m_type);
        const hir::EnumInfo* info = m_hir.find_enum(enum_type.m_name);

        const BlockId join = new_block();
        std::vector<BlockId> arm_blocks;
        Terminator pick;
        pick.m_kind = TerminatorKind::Switch;
        pick.m_span = s.m_span;
        pick.m_value = subject;
        BlockId fallback = NO_BLOCK;
        for (const hir::MatchCase& arm : s.m_arms) {
            const BlockId block = new_block();
            arm_blocks.push_back(block);
            if (arm.m_wildcard) {
                if (fallback == NO_BLOCK) fallback = block;
            } else {
                bool seen = false;
                for (const SwitchCase& earlier : pick.m_cases) seen = seen || earlier.m_variant == arm.m_variant;
                if (!seen) pick.m_cases.push_back({arm.m_variant, block});
            }
        }
        if (fallback == NO_BLOCK) {   // every variant has an arm (the checker proved it): nothing else can arrive
            fallback = new_block();
            Block& dead = m_fn.m_blocks[fallback];
            dead.m_terminator.m_kind = TerminatorKind::Unreachable;
            dead.m_terminated = true;
        }
        pick.m_else = fallback;
        terminate(std::move(pick));

        for (size_t i = 0; i < s.m_arms.size(); ++i) {
            const hir::MatchCase& arm = s.m_arms[i];
            m_cur = arm_blocks[i];
            for (size_t slot = 0; slot < arm.m_bindings.size(); ++slot) {
                if (arm.m_bindings[slot] == hir::NO_LOCAL) continue;
                Place payload = held;
                Projection projection;
                projection.m_kind = ProjectionKind::Payload;
                projection.m_type = info->m_variants[arm.m_variant].m_payload[slot];
                projection.m_variant = arm.m_variant;
                projection.m_index = slot;
                payload.m_projections.push_back(projection);
                assign(local_place(arm.m_bindings[slot]), use(read(std::move(payload))), arm.m_span);
            }
            lower_block(arm.m_body);
            jump(join);
        }
        m_cur = join;
    }

    // ---- cleanup -------------------------------------------------------------------------------

    /// Removes blocks the entry cannot reach (the dead code after `return` / `break` / `continue`) and renumbers.
    void prune_unreachable() {
        const size_t count = m_fn.m_blocks.size();
        std::vector<bool> reached(count, false);
        std::vector<BlockId> work{0};
        reached[0] = true;
        while (!work.empty()) {
            const BlockId id = work.back();
            work.pop_back();
            const Terminator& end = m_fn.m_blocks[id].m_terminator;
            std::vector<BlockId> targets;
            if (end.m_kind == TerminatorKind::Goto || end.m_kind == TerminatorKind::Branch) targets.push_back(end.m_target);
            if (end.m_kind == TerminatorKind::Branch || end.m_kind == TerminatorKind::Switch) targets.push_back(end.m_else);
            for (const SwitchCase& arm : end.m_cases) targets.push_back(arm.m_target);
            for (const BlockId target : targets) {
                if (target != NO_BLOCK && !reached[target]) {
                    reached[target] = true;
                    work.push_back(target);
                }
            }
        }
        std::vector<BlockId> renumbered(count, NO_BLOCK);
        std::vector<Block> kept;
        for (size_t i = 0; i < count; ++i) {
            if (!reached[i]) continue;
            renumbered[i] = static_cast<BlockId>(kept.size());
            kept.push_back(std::move(m_fn.m_blocks[i]));
        }
        for (Block& block : kept) {
            Terminator& end = block.m_terminator;
            if (end.m_kind == TerminatorKind::Goto || end.m_kind == TerminatorKind::Branch) end.m_target = renumbered[end.m_target];
            if (end.m_kind == TerminatorKind::Branch || end.m_kind == TerminatorKind::Switch) end.m_else = renumbered[end.m_else];
            for (SwitchCase& arm : end.m_cases) arm.m_target = renumbered[arm.m_target];
        }
        m_fn.m_blocks = std::move(kept);
    }
};

} // namespace

size_t Module::lowered_count() const {
    size_t count = 0;
    for (const Function& fn : m_functions) count += fn.m_lowered ? 1 : 0;
    return count;
}

const hir::EnumInfo* Module::find_enum(SymbolID name) const {
    for (const hir::EnumInfo& info : m_enums) {
        if (info.m_name == name) return &info;
    }
    return nullptr;
}

Module lower(const hir::Module& module) {
    Module out;
    out.m_types = module.m_types;
    out.m_enums = module.m_enums;
    for (const hir::Function& source : module.m_functions) {
        if (source.m_is_template) {
            ++out.m_templates;
            continue;
        }
        if (!source.m_lowered) {
            Function skipped;
            skipped.m_name = source.m_name;
            skipped.m_decl = source.m_decl;
            skipped.m_reason = source.m_reason;
            skipped.m_reason_span = source.m_reason_span;
            out.m_functions.push_back(std::move(skipped));
            continue;
        }
        Function built = Builder(module, source).build();
        out.m_functions.push_back(std::move(built));
    }
    return out;
}

} // namespace fool::mir
