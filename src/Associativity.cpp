#include "Associativity.h"
#include "Substitute.h"
#include "Simplify.h"
#include "IROperator.h"
#include "Solve.h"

namespace Halide {
namespace Internal {

using std::string;
using std::vector;


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

    //check_associativity(min(x, y), true);
    check_associativity(x + y, true);
    check_associativity(x * y, true);
    check_associativity(x - y, false);
    check_associativity(x / y, false);

    std::cout << "associativity test passed" << std::endl;
}


}
}
