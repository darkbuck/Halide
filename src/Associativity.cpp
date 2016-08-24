#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"
#include "AssociativeOpsTable.h"
#include "Util.h"
#include "IRMatch.h"

#include <algorithm>
#include <sstream>

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

namespace {

class CountExprDepth : public IRVisitor {
public:
    int depth = 0;

    using IRVisitor::visit;

    template<typename T>
    void visit_const(const T *op) {
        depth = 1;
    }

    void visit(const IntImm *op)    { visit_const<IntImm>(op); }
    void visit(const UIntImm *op)   { visit_const<UIntImm>(op); }
    void visit(const FloatImm *op)  { visit_const<FloatImm>(op); }
    void visit(const Variable *op)  { visit_const<Variable>(op); }

    void visit(const Cast *op) {
        op->value.accept(this);
        depth += 1;
    }

    void visit(const Not *op) {
        op->a.accept(this);
        depth += 1;
    }

    template<typename T>
    void visit_binary_operator(const T *op) {
        op->a.accept(this);
        int depth_a = depth;
        op->b.accept(this);
        int depth_b = depth;
        depth = 1 + std::max(depth_a, depth_b);
    }

    void visit(const Add *op) {visit_binary_operator(op);}
    void visit(const Sub *op) {visit_binary_operator(op);}
    void visit(const Mul *op) {visit_binary_operator(op);}
    void visit(const Div *op) {visit_binary_operator(op);}
    void visit(const Mod *op) {visit_binary_operator(op);}
    void visit(const Min *op) {visit_binary_operator(op);}
    void visit(const Max *op) {visit_binary_operator(op);}
    void visit(const EQ *op) {visit_binary_operator(op);}
    void visit(const NE *op) {visit_binary_operator(op);}
    void visit(const LT *op) {visit_binary_operator(op);}
    void visit(const LE *op) {visit_binary_operator(op);}
    void visit(const GT *op) {visit_binary_operator(op);}
    void visit(const GE *op) {visit_binary_operator(op);}
    void visit(const And *op) {visit_binary_operator(op);}
    void visit(const Or *op) {visit_binary_operator(op);}

    void visit(const Select *op) {
        internal_error << "TODO(psuriana): implement this later\n";
    }
};

int count_expr_depth(Expr e) {
    CountExprDepth c;
    e.accept(&c);
    return c.depth;
}

// Replace self-reference to Func 'func' with arguments 'args' at index
// 'value_index' in the Expr/Stmt with Var 'substitute'
class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    // If that function has multiple values, which value does this
    // call node refer to?
    int value_index;
    vector<string> op_x_names;
    map<int, Expr> *x_subs;
    bool is_conditional;

    void visit(const Call *op) {
        if (!is_solvable) {
            return;
        }
        IRMutator::visit(op);
        op = expr.as<Call>();
        internal_assert(op);

        if ((op->call_type == Call::Halide) && (func == op->name)) {
            internal_assert(!op->func.defined())
                << "Func should not have been defined for a self-reference\n";
            internal_assert(args.size() == op->args.size())
                << "Self-reference should have the same number of args as the original\n";
            if (is_conditional && (op->value_index == value_index)) {
                debug(4) << "Self-reference of " << op->name
                         << " inside a conditional. Operation is not associative\n";
                is_solvable = false;
                return;
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(4) << "Self-reference of " << op->name
                             << " with different args from the LHS. Operation is not associative\n";
                    is_solvable = false;
                    return;
                }
            }
            // Substitute the call
            const auto &iter = x_subs->find(op->value_index);
            if (iter != x_subs->end()) {
                const Variable *v = iter->second.as<Variable>();
                internal_assert(v && (v->type == op->type));
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << v->name << "\n";
                expr = iter->second;
            } else {
                internal_assert(op->value_index < (int)op_x_names.size());
                debug(4) << "   Substituting Call " << op->name << " at value index "
                         << op->value_index << " with " << op_x_names[op->value_index] << "\n";
                expr = Variable::make(op->type, op_x_names[op->value_index]);
                x_subs->emplace(op->value_index, expr);
            }
            if (op->value_index == value_index) {
                x_part = op;
            } else {
                x_dependencies.insert(op->value_index);
            }
        }
    }

    void visit(const Select *op) {
        is_conditional = true;
        Expr cond = mutate(op->condition);
        is_conditional = false;

        Expr t = mutate(op->true_value);
        Expr f = mutate(op->false_value);
        if (cond.same_as(op->condition) &&
            t.same_as(op->true_value) &&
            f.same_as(op->false_value)) {
            expr = op;
        } else {
            expr = Select::make(cond, t, f);
        }
    }

public:
    ConvertSelfRef(const string &f, const vector<Expr> &args, int idx,
                   const vector<string> &x_names, map<int, Expr> *subs) :
        func(f), args(args), value_index(idx), op_x_names(x_names), x_subs(subs), is_conditional(false) {}

    bool is_solvable = true;
    set<int> x_dependencies; // Contains dependencies on self-reference at different tuple indices
    Expr x_part;
};

class ExtractYPart : public IRMutator {
    using IRMutator::visit;

    vector<string> op_x_names;
    vector<string> op_y_names;
    map<int, Expr> *y_subs;

    enum OpType {
        OP_X,       // x only or mixed of x/constant
        OP_Y,       // y only
        OP_MIXED,   // mixed of x/y
    } type;

    bool is_x(const string &name) const {
        for (const auto &x : op_x_names) {
            if (x == name) {
                return true;
            }
        }
        return false;
    }

    string get_op_y_name() {
        if (current_y_name.empty()) {
            if (index < (int)op_y_names.size()) {
                current_y_name = op_y_names[index];
            } else {
                // Use a dummy name. If it turns out the final 'y_part' is
                // not in y_subs, the expression is not solvable since we're
                // using more distinct y variables than the tuple size.
                current_y_name = unique_name("_dummy");
            }
        }
        return current_y_name;
    }

    void reset_y() {
        debug(0) << "****RESETING for " << current_y << ", index: " << index << "\n";
        // Check if 'current_y' already has a op_y sub associated with it.
        for (const auto &iter : *y_subs) {
            if (equal(iter.second, current_y)) {
                // Since it's already there, we need to update the expr to use
                // that variable instead.
                internal_assert(iter.first < (int)y_subs->size());
                Expr y_var = Variable::make(current_y.type(), op_y_names[iter.first]);
                debug(0) << "FIND REPLACEMENT for " << current_y << " -> yvar: " << y_var << ", name: " << get_op_y_name() << "\n";
                debug(0) << "before replacement: " << expr << "\n";
                expr = substitute(get_op_y_name(), y_var, expr);
                debug(0) << "after replacement: " << expr << "\n";
                current_y = Expr();
                current_y_name = "";
                return;
            }
        }

        debug(0) << "****ADDING REPLACEMENT for: " << current_y << ", index: " << index << "\n";
        // It is not in y_subs, we need to add the replacement to y_subs.
        if (index < (int)op_y_names.size()) {
            debug(0) << "++++PUTTING INTO SUBS\n";
            y_subs->emplace(index, current_y);
            index += 1;
        } else {
            // We've used up all the 'op_y' vars to substitute the expr.
            is_solvable = false;
        }
        current_y = Expr();
        current_y_name = "";
    }

    template<typename T>
    void visit_unary_op(const T *op) {
        if (!is_solvable) {
            return;
        }
        type = OP_Y;
        current_y = Expr(op);
        expr = Variable::make(op->type, get_op_y_name());
    }

    void visit(const IntImm *op)    { visit_unary_op<IntImm>(op); }
    void visit(const UIntImm *op)   { visit_unary_op<UIntImm>(op); }
    void visit(const FloatImm *op)  { visit_unary_op<FloatImm>(op); }
    void visit(const StringImm *op) { visit_unary_op<StringImm>(op); }

    void visit(const Variable *op) {
        if (!is_solvable) {
            return;
        }
        if (is_x(op->name)) {
            type = OP_X;
            expr = op;
            return;
        }
        type = OP_Y;
        current_y = Expr(op);
        expr = Variable::make(op->type, get_op_y_name());
    }

    void visit(const Cast *op) {
        if (!is_solvable) {
            return;
        }
        Expr val = mutate(op->value);
        if (type == OP_Y) {
            current_y = Expr(op);
            expr = Variable::make(op->type, get_op_y_name());
        } else {
            // Either x or pair of x/y
            expr = Cast::make(op->type, val);
        }
    }

    template<typename T>
    void visit_binary_op(const T *op) {
        if (!is_solvable) {
            return;
        }
        Expr a = mutate(op->a);
        OpType a_type = type;
        Expr b = mutate(op->b);
        OpType b_type = type;

        internal_assert(a.type() == b.type());
        if ((a_type == OP_X) && (b_type == OP_X)) {
            is_solvable = false;
            return;
        } else if ((a_type == OP_Y) && (b_type == OP_Y)) {
            type = OP_Y;
            current_y = Expr(op);
            expr = Variable::make(op->type, get_op_y_name());
        } else if ((a_type == OP_Y) || (b_type == OP_Y)) {
            // Pair of x and y
            type = OP_MIXED;
            expr = T::make(a, b);
            reset_y();
        } else {
            expr = T::make(a, b);
        }
    }

    void visit(const Add *op) { visit_binary_op<Add>(op); }
    void visit(const Sub *op) { visit_binary_op<Sub>(op); }
    void visit(const Mul *op) { visit_binary_op<Mul>(op); }
    void visit(const Div *op) { visit_binary_op<Div>(op); }
    void visit(const Mod *op) { visit_binary_op<Mod>(op); }
    void visit(const Min *op) { visit_binary_op<Min>(op); }
    void visit(const Max *op) { visit_binary_op<Max>(op); }
    void visit(const And *op) { visit_binary_op<And>(op); }
    void visit(const Or *op) { visit_binary_op<Or>(op); }
    void visit(const LE *op) { visit_binary_op<LE>(op); }
    void visit(const LT *op) { visit_binary_op<LT>(op); }
    void visit(const GE *op) { visit_binary_op<GE>(op); }
    void visit(const GT *op) { visit_binary_op<GT>(op); }
    void visit(const EQ *op) { visit_binary_op<EQ>(op); }
    void visit(const NE *op) { visit_binary_op<NE>(op); }

    void visit(const Load *op) {
        internal_error << "Can't handle Load\n";
    }

    void visit(const Ramp *op) {
        internal_error << "Can't handle Ramp\n";
    }

    void visit(const Broadcast *op) {
        internal_error << "Can't handle Broadcast\n";
    }

    void visit(const Let *op) {
        internal_error << "Let should have been substituted before calling this mutator\n";
    }

    void visit(const Select *op) {
        //TODO(psuriana)
        internal_error << "Select NOT yet implemented\n";
    }

    void visit(const Not *op) {
        if (!is_solvable) {
            return;
        }
        Expr a = mutate(op->a);
        if (type == OP_Y) {
            current_y = Expr(op);
            expr = Variable::make(op->type, get_op_y_name());
        } else {
            expr = Not::make(a);
        }
    }

    void visit(const Call *op) {
        if (!is_solvable) {
            return;
        }
        if (op->call_type != Call::Halide) {
            is_solvable = false;
            return;
        }

        // Mutate the args
        for (size_t i = 0; i < op->args.size(); i++) {
            Expr new_args = mutate(op->args[i]);
            if (type != OP_Y) {
                is_solvable = false;
                return;
            }
        }
        internal_assert(type == OP_Y);
        current_y = Expr(op);
        expr = Variable::make(op->type, get_op_y_name());
    }

public:
    ExtractYPart(const vector<string> &x_names, const vector<string> &y_names,
                 map<int, Expr> *subs) :
        op_x_names(x_names), op_y_names(y_names), y_subs(subs), is_solvable(true), index(subs->size()) {}

    bool is_solvable;
    Expr current_y;
    string current_y_name;
    int index;
};

Expr extract_y_part(int index, const vector<string> &x_names, const vector<string> &y_names,
                    map<int, Expr> &y_subs, Expr &expr) {
    // Substitute all exprs that already have replacements to cut down the
    // number of cases ExtractYPart needs to handle.
    debug(0) << "Before substituting y_subs: " << expr << "\n";
    for (const auto &iter : y_subs) {
        expr = substitute(iter.second, Variable::make(iter.second.type(), y_names[iter.first]), expr);
    }
    debug(5) << "After substituting y_subs: " << expr << "\n";

    ExtractYPart conv(x_names, y_names, &y_subs);
    expr = conv.mutate(expr);
    debug(0) << "After mutation: " << expr << "\n";

    if (!conv.is_solvable) {
        return Expr();
    }

    if(conv.current_y.defined()) {
        debug(0) << "***NEED REPLACEMENT\n";
        for (const auto &iter : y_subs) {
            if (equal(iter.second, conv.current_y)) {
                // Since it's already there, we need to update the expr to use
                // that variable instead.
                internal_assert(iter.first < (int)y_subs.size());
                Expr y_var = Variable::make(conv.current_y.type(), y_names[iter.first]);
                expr = substitute(conv.current_y_name, y_var, expr);
                return conv.current_y;
            }
        }
        // It is not in y_subs, we need to add the replacement to y_subs.
        if (conv.index < (int)y_names.size()) {
            y_subs.emplace(conv.index, conv.current_y);
            return conv.current_y;
        }
        // We've used up all the 'op_y' vars to substitute the expr.
        return Expr();
    }

    const auto &iter = y_subs.find(index);
    return iter->second;
}


template<typename T>
bool visit_associative_binary_op(const string &op_x, const string &op_y, Expr x_part,
                                 Expr lhs, Expr rhs, AssociativeOp &op) {
    const Variable *var_a = lhs.as<Variable>();
    if (!var_a || (var_a->name != op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else if (expr_uses_var(rhs, op_x)) {
        debug(4) << "Can't prove associativity of " << T::make(lhs, rhs) << "\n";
        return false;
    } else {
        // op(x, y)
        op.x = {op_x, x_part};
        op.y = {op_y, rhs};
    }
    return true;
}

bool compare_expr_depths(const AssociativePair& lhs, const AssociativePair& rhs) {
    int lhs_depth = count_expr_depth(lhs.op);
    int rhs_depth = count_expr_depth(rhs.op);
    return lhs_depth < rhs_depth;
}

bool find_match(const vector<AssociativePair> &table, Expr e, Expr &identity) {
    //TODO(psuriana): Find a tighter bound
    auto lower = std::lower_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);
    auto upper = std::upper_bound(table.begin(), table.end(), AssociativePair(e), compare_expr_depths);

    //TODO: use the result
    map<string, Expr> result;
    for (; lower < upper; lower++) {
        AssociativePair pattern = *lower;
        if (expr_match(pattern.op, e, result)) {
            identity = pattern.identity;
            return true;
        }
    }

    return false;
}

bool lookup_single_i32_associative_ops_table(Expr e, Expr &identity) {
    Type t = Int(32);
    internal_assert(e.type() == t);

    debug(5) << "**lookup_single_i32_associative_ops_table: " << e << "\n";

    if (e.as<Add>()) {
        return find_match(get_single_i32_ops_table_add(), e, identity);
    } else if (e.as<Sub>()) {
        return find_match(get_single_i32_ops_table_sub(), e, identity);
    } else if (e.as<Mul>()) {
        return find_match(get_single_i32_ops_table_mul(), e, identity);
    } else if (e.as<Min>()) {
        return find_match(get_single_i32_ops_table_min(), e, identity);
    } else if (e.as<Max>()) {
        return find_match(get_single_i32_ops_table_max(), e, identity);
    }
    return false;
}

bool extract_associative_op(int index, const vector<string> &op_x_names,
                            const vector<string> &op_y_names,
                            map<int, Expr> &y_subs, Expr x_part,
                            Expr e, AssociativeOp &op) {
    Type t = e.type();
    const string &op_x = op_x_names[index];
    const string &op_y = op_y_names[index];
    Expr x = Variable::make(t, op_x);
    Expr y = Variable::make(t, op_y);

    if (!x_part.defined()) { // op(y)
        // Update with no self-recurrence is associative and the identity can be
        // anything since it's going to be replaced anyway
        op.op = y;
        op.identity = make_const(t, 0);
        op.x = {"", Expr()};
        op.y = {op_y, e};
        return true;
    }

    bool success = false;
    if (const Add *a = e.as<Add>()) {
        op.op = x + y;
        op.identity = make_const(t, 0);
        success = visit_associative_binary_op<Add>(op_x, op_y, x_part, a->a, a->b, op);
    } else if (const Sub *s = e.as<Sub>()) {
        op.op = x + y;
        op.identity = make_const(t, 0);
        success = visit_associative_binary_op<Sub>(op_x, op_y, x_part, s->a, s->b, op);
    } else if (const Mul *m = e.as<Mul>()) {
        op.op = x * y;
        op.identity = make_const(t, 1);
        success = visit_associative_binary_op<Mul>(op_x, op_y, x_part, m->a, m->b, op);
    } else if (const Min *m = e.as<Min>()) {
        op.op = Min::make(x, y);
        op.identity = t.max();
        success = visit_associative_binary_op<Min>(op_x, op_y, x_part, m->a, m->b, op);
    } else if (const Max *m = e.as<Max>()) {
        op.op = Max::make(x, y);
        op.identity = t.min();
        success = visit_associative_binary_op<Max>(op_x, op_y, x_part, m->a, m->b, op);
    } else if (const And *a = e.as<And>()) {
        op.op = And::make(x, y);
        op.identity = make_const(t, 1);
        success = visit_associative_binary_op<And>(op_x, op_y, x_part, a->a, a->b, op);
    } else if (const Or *o = e.as<Or>()) {
        op.op = Or::make(x, y);
        op.identity = make_const(t, 0);
        success = visit_associative_binary_op<Or>(op_x, op_y, x_part, o->a, o->b, op);
    } else if (e.as<Let>()) {
        internal_error << "Let should have been substituted before calling this function\n";
    }

    if (!success && t.is_int() && (t.bits() == 32)) {
        // It's non-trivial binary ops. Try looking at the associative ops table for int32
        debug(5) << "Look-up associativity table for: " << e << "\n";
        Expr y_part = extract_y_part(index, op_x_names, op_y_names, y_subs, e);
        if (!y_part.defined()) {
            return false;
        }

        // Canonicalize the expression
        e = solve_expression(e, op_y).result;
        e = solve_expression(e, op_x).result;
        e = simplify(e);
        debug(5) << "Canonicalized expr: " << e << "\n";

        {
            // We need to convert the variable names into x{tuple_idx} and y
            // {tuple_idx} to match it with the associative ops table.
            string x = "x" + std::to_string(index);
            string y = "y" + std::to_string(index);
            Expr temp = substitute(op_x, Variable::make(t, x), e);
            temp = substitute(op_y, Variable::make(t, y), temp);
            debug(5) << "After replacement " << e << " -> " << temp << "\n";
            success = lookup_single_i32_associative_ops_table(temp, op.identity);
        }

        if (success) {
            debug(5) << "Find associative ops for " << e << "\n";
            op.op = e;
            op.x = {op_x, x_part};
            op.y = {op_y, y_part};
        }
    }
    return success;
}

} // anonymous namespace


// TODO(psuriana): This does not handle cross-dependencies among tuple elements.
// It also is not able to handle associative select() (e.g. argmin/argmax)
pair<bool, vector<AssociativeOp>> prove_associativity(const string &f, vector<Expr> args,
                                                      vector<Expr> exprs) {
    vector<AssociativeOp> ops;
    map<int, Expr> x_subs;
    map<int, Expr> y_subs;

    for (Expr &arg : args) {
        arg = common_subexpression_elimination(arg);
        arg = simplify(arg);
        arg = substitute_in_all_lets(arg);
    }

    vector<string> op_x_names(exprs.size()), op_y_names(exprs.size());
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        op_x_names[idx] = unique_name("_x_" + std::to_string(idx));
        op_y_names[idx] = unique_name("_y_" + std::to_string(idx));
    }

    vector<set<int>> dependencies(exprs.size());

    // For a Tuple of exprs to be associative, each element of the Tuple
    // has to be associative.
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        string op_x = op_x_names[idx];
        string op_y = op_y_names[idx];

        Expr expr = simplify(exprs[idx]);

        // Replace any self-reference to Func 'f' with a Var
        ConvertSelfRef csr(f, args, idx, op_x_names, &x_subs);
        expr = csr.mutate(expr);
        if (!csr.is_solvable) {
            return std::make_pair(false, vector<AssociativeOp>());
        }
        dependencies[idx] = csr.x_dependencies;

        expr = common_subexpression_elimination(expr);
        expr = simplify(expr);
        expr = solve_expression(expr, op_x).result; // Move 'x' to the left as possible
        expr = substitute_in_all_lets(expr);

        // Try to infer the 'y' part of the operator. If we couldn't find
        // a single 'y' that satisfy the operator, give up
        AssociativeOp op;
        bool is_associative = extract_associative_op(idx, op_x_names, op_y_names,
                                                     y_subs, csr.x_part, expr, op);
        if (!is_associative) {
            return std::make_pair(false, vector<AssociativeOp>());
        }
        ops.push_back(op);
    }
    return std::make_pair(true, ops);
}

namespace {

std::string print_args(const string &f, const vector<Expr> &args, const vector<Expr> &exprs) {
    std::ostringstream stream;
    stream << f << "(";
    for (size_t i = 0; i < args.size(); ++i) {
        stream << args[i];
        if (i != args.size() - 1) {
            stream << ", ";
        }
    }
    stream << ") = ";

    if (exprs.size() == 1) {
        stream << exprs[0];
    } else if (exprs.size() > 1) {
        stream << "Tuple(";
        for (size_t i = 0; i < exprs.size(); ++i) {
            stream << exprs[i];
            if (i != exprs.size() - 1) {
                stream << ", ";
            }
        }
        stream << ")";
    }
    return stream.str();
}

void check_associativity(const string &f, vector<Expr> args, vector<Expr> exprs,
                         bool is_associative, vector<AssociativeOp> ops) {
    auto result = prove_associativity(f, args, exprs);
    internal_assert(result.first == is_associative)
        << "Checking associativity: " << print_args(f, args, exprs) << "\n"
        << "  Expect is_associative: " << is_associative << "\n"
        << "  instead of " << result.first << "\n";
    if (is_associative) {
        for (size_t i = 0; i < ops.size(); ++i) {
            const AssociativeOp &op = result.second[i];
            internal_assert(equal(op.identity, ops[i].identity))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Expect identity: " << ops[i].identity << "\n"
                << "  instead of " << op.identity << "\n";
            internal_assert(equal(op.x.second, ops[i].x.second))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Expect x: " << ops[i].x.second << "\n"
                << "  instead of " << op.x.second << "\n";
            internal_assert(equal(op.y.second, ops[i].y.second))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Expect y: " << ops[i].y.second << "\n"
                << "  instead of " << op.y.second << "\n";
            Expr expected_op = ops[i].op;
            if (op.y.second.defined()) {
                expected_op = substitute(
                    ops[i].y.first, Variable::make(op.y.second.type(), op.y.first), expected_op);
            }
            if (op.x.second.defined()) {
                expected_op = substitute(
                    ops[i].x.first, Variable::make(op.x.second.type(), op.x.first), expected_op);
            }
            internal_assert(equal(op.op, expected_op))
                << "Checking associativity: " << print_args(f, args, exprs) << "\n"
                << "  Expect bin op: " << expected_op << "\n"
                << "  instead of " << op.op << "\n";

            debug(4) << "\nExpected op: " << expected_op << "\n";
            debug(4) << "Operator: " << op.op << "\n";
            debug(4) << "   identity: " << op.identity << "\n";
            debug(4) << "   x: " << op.x.first << " -> " << op.x.second << "\n";
            debug(4) << "   y: " << op.y.first << " -> " << op.y.second << "\n";
        }
    }
}

} // anonymous namespace

void associativity_test() {
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");
    Expr rx = Variable::make(Int(32), "rx");

    Expr f_call_0 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 0);
    Expr f_call_1 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 1);
    Expr f_call_2 = Call::make(Int(32), "f", {x}, Call::CallType::Halide, nullptr, 2);
    Expr g_call = Call::make(Int(32), "g", {rx}, Call::CallType::Halide, nullptr, 0);

    // f(x) = min(f(x), int16(z))
    /*check_associativity("f", {x}, {min(f_call_0, y + Cast::make(Int(16), z))},
                        true, {{min(x, y), Int(32).max(), {"x", f_call_0}, {"y", y + Cast::make(Int(16), z)}}});

    // f(x) = f(x) + g(rx) + y + z
    check_associativity("f", {x}, {y + z + f_call_0},
                        true, {{x + y, make_const(Int(32), 0), {"x", f_call_0}, {"y", y + z}}});

    // f(x) = max(y, f(x))
    check_associativity("f", {x}, {max(y, f_call_0)},
                        true, {{max(x, y), Int(32).min(), {"x", f_call_0}, {"y", y}}});

    // f(x) = Tuple(2, 3, f(x)[2] + z)
    check_associativity("f", {x}, {2, 3, f_call_2 + z},
                        true,
                        {{y, make_const(Int(32), 0), {"", Expr()}, {"y", 2}},
                         {y, make_const(Int(32), 0), {"", Expr()}, {"y", 3}},
                         {x + y, make_const(Int(32), 0), {"x", f_call_2}, {"y", z}},
                        });

    // f(x) = Tuple(min(f(x)[0], g(rx)), f(x)[1]*g(x)*2, f(x)[2] + z)
    check_associativity("f", {x}, {min(f_call_0, g_call), f_call_1*g_call*2, f_call_2 + z},
                        true,
                        {{min(x, y), Int(32).max(), {"x", f_call_0}, {"y", g_call}},
                         {x * y, make_const(Int(32), 1), {"x", f_call_1}, {"y", g_call*2}},
                         {x + y, make_const(Int(32), 0), {"x", f_call_2}, {"y", z}},
                        });

    // f(x) = max(f(x) + g(rx), g(rx)) -> not associative
    check_associativity("f", {x}, {max(f_call_0 + g_call, g_call)},
                        false, {});

    // f(x) = max(f(x) + g(rx), f(x) - 3) -> f(x) + max(g(rx) - 3)
    check_associativity("f", {x}, {max(f_call_0 + g_call, f_call_0 - 3)},
                        true, {{x + y, 0, {"x", f_call_0}, {"y", max(g_call, -3)}}});

    // f(x) = f(x) - g(rx) -> Is associative given that the merging operator is +
    check_associativity("f", {x}, {f_call_0 - g_call},
                        true, {{x + y, 0, {"x", f_call_0}, {"y", g_call}}});

    // f(x) = min(4, g(rx)) -> trivially associative
    check_associativity("f", {x}, {min(4, g_call)},
                        true, {{y, make_const(Int(32), 0), {"", Expr()}, {"y", min(g_call, 4)}}});

    // f(x) = f(x) -> associative but doesn't really make any sense, so we'll treat it as non-associative
    check_associativity("f", {x}, {f_call_0},
                        false, {});*/

    // f(x) = max(max(min(f(x), g(rx) + 2), f(x)), g(rx) + 2)
    check_associativity("f", {x}, {max(max(min(f_call_0, g_call + 2), f_call_0), g_call + 2)},
                        true, {{max(max(min(x, y), x), y), Int(32).min(), {"x", f_call_0}, {"y", g_call + 2}}});

    // f(x) = ((min(max((f(x)*g(rx)), g(rx)), (f(x)*g(rx))) + g(rx)) + f(x))
    check_associativity("f", {x}, {((min(max((g_call*f_call_0), g_call), (f_call_0*g_call)) + g_call) + f_call_0)},
                        true, {{((min(max((x*y), y), (x*y)) + y) + x), make_const(Int(32), 0), {"x", f_call_0}, {"y", g_call}}});

    /*Expr x0 = Variable::make(Int(32), "x0");
    Expr y0 = Variable::make(Int(32), "y0");

    Expr expr = max(max(min((x0*y0), x0), y0), x0); // Leaves 5
    int depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";

    expr = min(max(((x0 + y0)*x0), max((y0*y0), y0)), y0); // Leaves 6
    depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";

    expr = max(min(((y0 - x0)*(y0 - x0)), min((y0*y0), y0)), y0); // Leaves 8
    depth = count_expr_depth(expr);
    std::cout << "Depth of " << expr << ": " << depth << "\n";*/

    std::cout << "Associativity test passed" << std::endl;
}


}
}
