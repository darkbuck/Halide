#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "IREquality.h"
#include "IRMutator.h"
#include "Scope.h"
#include "Solve.h"
#include "ExprUsesVar.h"
#include "CSE.h"

#include <set>

namespace Halide {
namespace Internal {

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

class ConvertSelfRef : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    const string subs;

    void visit(const Call *op) {
        IRMutator::visit(op);
        op = expr.as<Call>();
        internal_assert(op);

        if ((op->call_type == Call::Halide) && (func == op->name)) {
            internal_assert(!op->func.defined())
                << "Func should not have been defined for a self-reference\n";
            internal_assert(args.size() == op->args.size())
                << "Self-reference should have the same number of args as the original\n";
            for (size_t i = 0; i < op->args.size(); i++) {
                if (!equal(op->args[i], args[i])) {
                    debug(0) << "Find self-reference with different args: operation is not associative\n";
                    is_not_associative = true;
                    return;
                }
            }
            // Substitute the call
            expr = Variable::make(op->type, subs);
        }
    }

public:
    ConvertSelfRef(const string &f, const vector<Expr> &args, const string &s) :
        func(f), args(args), subs(s), is_not_associative(false) {}

    bool is_not_associative;
};

class BinaryOpConverter : public IRMutator {
    using IRMutator::visit;

    const string func;
    const vector<Expr> args;
    Scope<string> rvars;
    string op_x;
    string op_y;

    bool is_not_associative;

    Expr lhs, rhs;

    enum OpType {
        OP_X = 0,
        OP_Y,
        OP_CONST,
        OP_NONE
    };

    OpType get_op_type(Expr e) {
        if (is_const(e)) {
            return OP_CONST;
        }
        const Variable *v = e.as<Variable>();
        if (v) {
            if (v->name == op_x) {
                return OP_X;
            } else if (v->name == op_y) {
                return OP_Y;
            }
        }
        if (expr_uses_vars(e, rvars)) {
            return OP_Y;
        } else {
            return OP_CONST;
        }
    }

    void visit(const Variable *op) {
        if ((op->name == op_x) || (op->name == op_y)) {
            return;
        }
        if (rvars.contains(op->name)) {
            expr = Variable::make(op->type, op_y);
        } else {
            // Const Var
            expr = make_const(op->type, 0);
        }
    }

    void visit(const Cast *op) {
        Expr val = mutate(op->value);
        OpType op_type = get_op_type(val);
        if ((op_type == OP_X) || (op_type == OP_NONE)) {
            // Either x only or mixed of x/constant or x/y
            expr = Cast::make(op->type, val);
        } else  {
            // Either y or constant
            expr = val;
        }
    }

    template<typename T, typename Opp>
    void visit_binary_op(const T *op) {
        Expr a = mutate(op->a);
        Expr b = mutate(op->b);
        internal_assert(a.type() == b.type());
        OpType a_op_type = get_op_type(a);
        OpType b_op_type = get_op_type(b);

        if ((a_op_type == OP_X) || (b_op_type == OP_X) ||
            (a_op_type == OP_NONE) || (b_op_type == OP_NONE)) {
            lhs = a;
            rhs = b;
        } else if (a_op_type == OP_Y) {
            lhs = a;
            rhs = Expr();
        } else if (b_op_type == OP_Y) {
            lhs = b;
            rhs = Expr();
        } else { // Both a and b are constant
            lhs = a;
            rhs = Expr();
        }
        expr = rhs.defined() ? Opp::make(lhs, rhs) : lhs;
    }

    void visit(const Add *op) {
        visit_binary_op<Add, Add>(op);
    }

    void visit(const Sub *op) {
        visit_binary_op<Sub, Sub>(op);
    }

    void visit(const Mul *op) {
        visit_binary_op<Mul, Mul>(op);
    }

    void visit(const Div *op) {
        visit_binary_op<Div, Div>(op);
    }

    void visit(const Mod *op) {
        visit_binary_op<Mod, Mod>(op);
    }

    void visit(const Min *op) {
        visit_binary_op<Min, Min>(op);
    }

    void visit(const Max *op) {
        visit_binary_op<Max, Max>(op);
    }

    void visit(const Or *op) {
        visit_binary_op<Or, Or>(op);
    }

    void visit(const And *op) {
        visit_binary_op<And, And>(op);
    }

    void visit(const Let *op) {
        internal_error << "Should be substituted before calling this mutator\n";
    }

    void visit(const Select *op) {
        //TODO(psuriana)

    }

    template<typename T, typename Opp>
    void visit_cmp(const T *op) {
        IRMutator::visit(op);
        //TODO
    }

    void visit(const LE *op) {
        visit_cmp(op);
    }

    void visit(const LT *op) {
        visit_cmp(op);
    }

    void visit(const GE *op) {
        visit_cmp(op);
    }

    void visit(const GT *op) {
        visit_cmp(op);
    }

    void visit(const EQ *op) {
        visit_cmp(op);
    }

    void visit(const NE *op) {
        visit_cmp(op);
    }

    void visit(const Not *op) {
        //TODO
    }

    void visit(const Call *op) {
        IRMutator::visit(op);
        op = expr.as<Call>();
        internal_assert(op);

        if ((op->call_type == Call::Halide) && (func == op->name)) {
            internal_error << "The self-reference should be replaced before calling this mutator\n";
        }
        for (const Expr &arg : args) {
            if (get_op_type(arg) == OP_Y) { // arg depends on RVars
                // Substitute with 'y'
                expr = Variable::make(op->type, op_y);
                return;
            }
        }
        //TODO(psuriana): fixed this to account for 'x'
        // Substitute with 'constant'
        expr = make_const(op->type, 0);
    }

public:
    BinaryOpConverter(const string &f, const vector<Expr> &args, const Scope<string> *rv) :
            func(f), args(args), op_x("x"), op_y("y"), is_not_associative(false) {
        rvars.set_containing_scope(rv);
    }
};

bool is_associative(const string &f, const Scope<string> *rvars, vector<Expr> args, Expr expr) {
    for (Expr &arg :args) {
        arg = common_subexpression_elimination(arg);
        arg = simplify(arg);
        arg = SubstituteInAllLets().mutate(arg);
    }
    debug(0) << "Expr: " << expr << "\n";
    string op_x = "x";
    ConvertSelfRef csr(f, args, op_x);
    expr = csr.mutate(expr);
    debug(0) << "Expr after ConvertSelfRef: " << expr << "\n";
    if (csr.is_not_associative) {
        return false;
    }
    expr = common_subexpression_elimination(expr);
    expr = simplify(expr);
    expr = SubstituteInAllLets().mutate(expr);
    expr = solve_expression(expr, op_x); // Move 'x' to the left as possible
    debug(0) << "Expr after solve_expression: " << expr << "\n";

    BinaryOpConverter conv(f, args, rvars);
    expr = conv.mutate(expr);

    return is_bin_op_associative(expr);
}


// Given a binary expression operator 'bin_op' in the form of op(x, y), prove that
// 'bin_op' is associative, i.e. prove that (x op y) op z == x op (y op z)
bool is_bin_op_associative(Expr bin_op) {
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");

    Expr lhs = substitute("y", z, bin_op);
    lhs = substitute("x", bin_op, lhs);

    Expr rhs = substitute({{"x", y}, {"y", z}}, bin_op);
    rhs = substitute("y", rhs, bin_op);

    debug(0) << "\nBefore solve lhs: " << lhs << "; rhs: " << rhs << "\n";

    // Canonicalize the lhs and rhs before comparing them so that we get
    // a better chance of simplifying the equality.
    vector<string> vars = {"x", "y", "z"};
    for (const string &v : vars) {
        lhs = solve_expression(lhs, v);
        rhs = solve_expression(rhs, v);
    }
    debug(0) << "After solve lhs: " << lhs << "; rhs: " << rhs << "\n";

    Expr compare = simplify(lhs == rhs);
    debug(0) << "Checking for associativity: " << compare << "\n";
    return is_one(compare);
}

namespace {

void check_associativity(Expr bin_op, bool expected) {
    //debug(0) << "Checking that " << a << " -> " << b << "\n";
    bool result = is_bin_op_associative(bin_op);
    if (result != expected) {
        internal_error
            << "\nCheck associativity failure:\n"
            << "Result: " << result << '\n'
            << "Expected: " << expected << '\n';
    }
}

}

void associativity_test() {
    Expr x = Variable::make(Int(32), "x");
    Expr y = Variable::make(Int(32), "y");
    Expr z = Variable::make(Int(32), "z");
    Expr t = Variable::make(Int(32), "t");

    //check_associativity(min(x, y), true);
    /*check_associativity(x + y, true);
    check_associativity(x * y, true);
    check_associativity(x - y, false);
    check_associativity(x / y, false);*/

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

    std::cout << "associativity test passed" << std::endl;
}


}
}
