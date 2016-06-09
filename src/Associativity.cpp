#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"

#include <set>

namespace Halide {
namespace Internal {

using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

/** Substitute an expr for a var in a graph. */
class GraphSubstitute : public IRGraphMutator {
    string var;
    Expr value;

    using IRGraphMutator::visit;

    void visit(const Variable *op) {
        if (op->name == var) {
            expr = value;
        } else {
            expr = op;
        }
    }

public:
    GraphSubstitute(const string &var, Expr value) : var(var), value(value) {}
};

/** Substitute in all let Exprs in a piece of IR. Doesn't substitute
 * in let stmts, as this may change the meaning of the IR (e.g. by
 * moving a load after a store). Produces graphs of IR, so don't use
 * non-graph-aware visitors or mutators on it until you've CSE'd the
 * result. */
class SubstituteInAllLets : public IRGraphMutator {

    using IRGraphMutator::visit;

    void visit(const Let *op) {
        Expr value = mutate(op->value);
        Expr body = mutate(op->body);
        expr = GraphSubstitute(op->name, value).mutate(body);
    }

    void visit(const LetStmt *op) {
        Expr value = mutate(op->value);
        Stmt body = mutate(op->body);
        stmt = GraphSubstitute(op->name, value).mutate(body);
    }
};

// Replace self-reference to Func 'func' with arguments 'args' at index
// 'value_index' in the Expr/Stmt with Var 'substitute'
class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    // If that function has multiple values, which value does this
    // call node refer to?
    int value_index;
    string op_x;
    map<int, Expr> *self_ref_subs;
    bool is_conditional;

    void visit(const Call *op) {
        debug(0) << "VISIT CALL " << op->name << " at value index " << op->value_index << "\n";
        if (is_not_associative) {
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
                debug(0) << "Self-reference of " << op->name
                         << " inside a conditional. Operation is not associative\n";
                is_not_associative = true;
                return;
            }
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(0) << "Self-reference of " << op->name
                             << " with different args from the LHS. Operation is not associative\n";
                    is_not_associative = true;
                    return;
                }
            }
            // Substitute the call
            const auto &iter = self_ref_subs->find(op->value_index);
            if (iter != self_ref_subs->end()) {
                const Variable *v = iter->second.as<Variable>();
                internal_assert(v);
                internal_assert(v->type == op->type);
                debug(0) << "....Substituting Call " << op->name << " at value index " << op->value_index << " with " << v->name << "\n";
                expr = iter->second;
            } else {
                debug(0) << "....Substituting Call " << op->name << " at value index " << op->value_index << " with " << op_x << "\n";
                expr = Variable::make(op->type, op_x);
                self_ref_subs->emplace(op->value_index, expr);
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
    ConvertSelfRef(const string &f, const vector<Expr> &args, int idx, const string &x, map<int, Expr> *subs) :
        func(f), args(args), value_index(idx), op_x(x), self_ref_subs(subs), is_conditional(false), is_not_associative(false) {}

    bool is_not_associative;
};

class BinaryOpConverter : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    map<int, Expr> self_ref_subs;
    string op_y;

    enum OpType {
        OP_X,       // x only or mixed of x/constant
        OP_Y,       // y only
        OP_MIXED,   // mixed of x/y
    };

    OpType type;

    bool is_x(const string &name) {
        for (const auto &iter : self_ref_subs) {
            const Variable *v = iter.second.as<Variable>();
            internal_assert(v);
            if (v->name == name) {
                return true;
            }
        }
        return false;
    }

    void visit(const Variable *op) {
        if (!is_solvable) {
            return;
        }
        debug(0) << "Variable: " << op->name << "\n";

        if (is_x(op->name)) {
            type = OP_X;
            expr = op;
            return;
        }
        type = OP_Y;
        current_y = Expr(op);
        expr = Variable::make(op->type, op_y);
    }

    void visit(const Cast *op) {
        if (!is_solvable) {
            return;
        }
        Expr val = mutate(op->value);
        if (type == OP_Y) {
            current_y = Expr(op);
            expr = Variable::make(op->type, op_y);
        } else {
            // Either x or pair of x/y
            expr = Cast::make(op->type, val);
        }
    }

    template<typename T, typename Opp>
    void visit_binary_op(const T *op) {
        if (!is_solvable) {
            return;
        }
        debug(0) << "Binary op: " << op->a << " with " << op->b << "\n";
        Expr a = mutate(op->a);
        OpType a_type = type;
        Expr b = mutate(op->b);
        OpType b_type = type;

        internal_assert(a.type() == b.type());
        if ((a_type == OP_MIXED) || (b_type == OP_MIXED)) {
            debug(0) << "Found binary op of mixed type\n";
            is_solvable = false;
            return;
        }
        if ((a_type == OP_X) && (b_type == OP_X)) {
            debug(0) << "Found binary op of two x type\n";
            is_solvable = false;
            return;
        }

        if ((a_type == OP_X) || (b_type == OP_X)) {
            // Pair of x and y
            type = OP_MIXED;
            expr = Opp::make(a, b);
        } else {
            internal_assert((a_type == OP_Y) && (b_type == OP_Y));
            type = OP_Y;
            current_y = Expr(op);
            expr = Variable::make(op->type, op_y);
        }
    }

    void visit(const Add *op) { visit_binary_op<Add, Add>(op); }
    void visit(const Sub *op) { visit_binary_op<Sub, Sub>(op); }
    void visit(const Mul *op) { visit_binary_op<Mul, Mul>(op); }
    void visit(const Div *op) { visit_binary_op<Div, Div>(op); }
    void visit(const Mod *op) { visit_binary_op<Mod, Mod>(op); }
    void visit(const Min *op) { visit_binary_op<Min, Min>(op); }
    void visit(const Max *op) { visit_binary_op<Max, Max>(op); }
    void visit(const And *op) { visit_binary_op<And, And>(op); }
    void visit(const Or *op) { visit_binary_op<Or, Or>(op); }
    void visit(const LE *op) { visit_binary_op<LE, LE>(op); }
    void visit(const LT *op) { visit_binary_op<LT, LT>(op); }
    void visit(const GE *op) { visit_binary_op<GE, GE>(op); }
    void visit(const GT *op) { visit_binary_op<GT, GT>(op); }
    void visit(const EQ *op) { visit_binary_op<EQ, EQ>(op); }
    void visit(const NE *op) { visit_binary_op<NE, NE>(op); }

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
        if (!is_solvable) {
            return;
        }

        Expr old_y;

        Expr cond = mutate(op->condition);
        if (type != OP_X) {
            old_y = current_y;
        }
        if (!is_solvable) {
            return;
        }

        Expr true_value = mutate(op->true_value);
        if (!is_solvable) {
            return;
        }
        if (type == OP_MIXED) {
            is_solvable = false;
            return;
        } else if (type == OP_Y) {
            if (old_y.defined()) {
                if (!equal(old_y, current_y)) {
                    is_solvable = false;
                    return;
                }
            }
            old_y = current_y;
        }

        Expr false_value = mutate(op->false_value);
        if (!is_solvable) {
            return;
        }
        if (type == OP_MIXED) {
            is_solvable = false;
            return;
        } else if (type == OP_Y) {
            if (old_y.defined()) {
                if (!equal(old_y, current_y)) {
                    is_solvable = false;
                    return;
                }
            }
            old_y = current_y;
        }
        expr = Select::make(cond, true_value, false_value);
    }

    void visit(const Not *op) {
        if (!is_solvable) {
            return;
        }
        Expr a = mutate(op->a);
        if (type == OP_Y) {
            current_y = Expr(op);
            expr = Variable::make(op->type, op_y);
        } else {
            expr = Not::make(a);
        }
    }

    void visit(const Call *op) {
        if (!is_solvable) {
            return;
        }
        if (op->call_type != Call::Halide) {
            debug(0) << "Non halide call\n";
            is_solvable = false;
            return;
        }

        // Mutate the args
        for (size_t i = 0; i < op->args.size(); i++) {
            Expr new_args = mutate(op->args[i]);
            if (type != OP_Y) {
                debug(0) << "Found call op of mixed type\n";
                is_solvable = false;
                return;
            }
        }
        internal_assert(type == OP_Y);
        current_y = Expr(op);
        expr = Variable::make(op->type, op_y);
    }

public:
    BinaryOpConverter(const string &f, const vector<Expr> &args, const map<int, Expr> &subs, const string y) :
            func(f), args(args), self_ref_subs(subs), op_y(y), is_solvable(true) {}

    bool is_solvable;
    Expr current_y;
};

pair<bool, vector<Expr>> is_associative(const string &f, vector<Expr> args, vector<Expr> exprs) {
    vector<Expr> y_parts;
    map<int, Expr> self_ref_subs;
    for (size_t idx = 0; idx < exprs.size(); ++idx) {
        Expr expr = simplify(exprs[idx]);
        for (Expr &arg : args) {
            arg = common_subexpression_elimination(arg);
            arg = simplify(arg);
            arg = SubstituteInAllLets().mutate(arg);
        }
        debug(0) << "\nExpr: " << expr << "\n";
        string op_x = unique_name("_x");
        string op_y = unique_name("_y");
        ConvertSelfRef csr(f, args, idx, op_x, &self_ref_subs);
        expr = csr.mutate(expr);
        debug(0) << "Expr after ConvertSelfRef: " << expr << "\n";
        if (csr.is_not_associative) {
            return std::make_pair(false, vector<Expr>());
        }
        expr = common_subexpression_elimination(expr);
        expr = simplify(expr);
        expr = SubstituteInAllLets().mutate(expr);
        for (const auto &iter : self_ref_subs) {
            const Variable *v = iter.second.as<Variable>();
            internal_assert(v);
            expr = solve_expression(expr, v->name); // Move 'x' to the left as possible
        }
        debug(0) << "Expr after solve_expression " << op_x << ": " << expr << "\n";

        BinaryOpConverter conv(f, args, self_ref_subs, op_y);
        expr = conv.mutate(expr);
        debug(0) << "Output : " << expr << "\n";
        if (!conv.is_solvable) {
            debug(0) << "Not solvable\n";
            return std::make_pair(false, vector<Expr>());
        }
        Expr y_part = conv.current_y;
        debug(0) << "y_part: " << y_part << "\n";

        debug(0) << "Checking for associativity\n";
        if (!is_bin_op_associative(expr, op_x, op_y)){
            debug(0) << "Not solvable\n";
            return std::make_pair(false, vector<Expr>());
        }
        y_parts.push_back(y_part);
    }
    return std::make_pair(true, y_parts);
}


// Given a binary expression operator 'bin_op' in the form of op(x, y), prove that
// 'bin_op' is associative, i.e. prove that (x op y) op z == x op (y op z)
bool is_bin_op_associative(Expr bin_op, const string &op_x, const string &op_y) {
    Expr x = Variable::make(Int(32), op_x);
    Expr y = Variable::make(Int(32), op_y);
    string op_z = unique_name("_z");
    Expr z = Variable::make(Int(32), op_z);

    Expr lhs = substitute(op_y, z, bin_op);
    lhs = substitute(op_x, bin_op, lhs);

    Expr rhs = substitute({{op_x, y}, {op_y, z}}, bin_op);
    rhs = substitute(op_y, rhs, bin_op);

    debug(0) << "\nBefore solve lhs: " << lhs << "; rhs: " << rhs << "\n";

    // Canonicalize the lhs and rhs before comparing them so that we get
    // a better chance of simplifying the equality.
    vector<string> vars = {op_x, op_y, op_z};
    for (const string &v : vars) {
        lhs = solve_expression(lhs, v);
        rhs = solve_expression(rhs, v);
    }
    debug(0) << "After solve lhs: " << lhs << "; rhs: " << rhs << "\n";

    lhs = common_subexpression_elimination(lhs);
    rhs = common_subexpression_elimination(rhs);
    Expr compare = simplify(lhs == rhs);
    debug(0) << "Checking for associativity: " << compare << "\n";
    return is_one(compare);
}

void associativity_test() {
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");
    Expr t = Variable::make(Int(32), "t");

    //Expr expr = Let::make("x", y + 1, Block::make(x + 1, x));
    Expr mul = x + y;
    //Expr expr = mul + max(mul, 1) + min(mul, mul + 2);

    Expr expr1 = Call::make(Int(32), "f",
                {mul, x*y},
                Call::Extern);
    Expr expr2 = expr1 + x*y;

    Stmt let = LetStmt::make("p", expr1,
        Block::make(common_subexpression_elimination(Evaluate::make(expr2)),
                    common_subexpression_elimination(Evaluate::make(expr1))));

    std::cout << "Expr: \n" << let << "\n";
    Stmt s = common_subexpression_elimination(let);
    s = simplify(s);
    s = SubstituteInAllLets().mutate(s);
    std::cout << "After CSE: \n" << s << "\n";

    Expr e1 = common_subexpression_elimination(expr1 + x*y);
    e1 = simplify(e1);
    e1 = SubstituteInAllLets().mutate(e1);
    Expr e2 = common_subexpression_elimination(expr2);
    e2 = simplify(e2);
    e2 = SubstituteInAllLets().mutate(e2);
    std::cout << "e1: " << e1 << "\n";
    std::cout << "e2: " << e2 << "\n";
    std::cout << "is equal? " << equal(e1, e2) << "\n";

    Expr test = select(y + 3 < x, y+x, 3-x);
    std::cout << "Solve: " << solve_expression(test, "x") << "\n";


    Expr prev_val0 = Call::make(Int(32), "dummy", {x},
                               Call::CallType::Halide,
                               nullptr, 0);
    Expr prev_val1 = Call::make(Int(32), "dummy", {x},
                               Call::CallType::Halide,
                               nullptr, 1);

    //TODO(psuriana): error
    //auto res = is_associative("dummy", {x}, {Cast::make(Int(16), min(prev_val0, z))});
    //auto res = is_associative("dummy", {x}, {y + z + prev_val1});
    //auto res = is_associative("dummy", {x}, {max(y, prev_val1)});
    //TODO(psuriana): does not work for select
    auto res = is_associative("dummy", {x}, {min(prev_val0, z), select(z < prev_val0, z, prev_val1)});
    std::cout << "****is assoc? " << res.first << "\n";

    std::cout << "associativity test passed" << std::endl;
}


}
}
