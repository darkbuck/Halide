#include "AssociativeOpsTable.h"

using std::vector;

namespace Halide {
namespace Internal {

namespace {

const Type i32 = Int(32);
const Expr i32_zero = make_const(i32, 0);
const Expr i32_one = make_const(i32, 1);
const Expr i32_tmax = i32.max();
const Expr i32_tmin = i32.min();

const Expr i32_x0 = Variable::make(i32, "x0");
const Expr i32_y0 = Variable::make(i32, "y0");
const Expr i32_mul = Mul::make(i32_x0, i32_y0);
const Expr i32_add = Add::make(i32_x0, i32_y0);
const Expr i32_max = Max::make(i32_x0, i32_y0);
const Expr i32_min = Min::make(i32_x0, i32_y0);
const Expr i32_sub = Sub::make(i32_x0, i32_y0);

} // anonymous namespace

const vector<AssociativePair> &get_single_i32_ops_table_add() {
    static bool init = false;
    static vector<AssociativePair> exprs(2);
    if (!init) {
        exprs[0] = {i32_add, i32_zero};
        exprs[1] = {Add::make(Add::make(Min::make(Max::make(i32_mul, i32_y0), i32_mul), i32_y0), i32_x0), i32_zero};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_mul() {
    static bool init = false;
    static vector<AssociativePair> exprs(1);
    if (!init) {
        exprs[0] = {i32_mul, i32_one};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_max() {
    static bool init = false;
    static vector<AssociativePair> exprs(4);
    if (!init) {
        exprs[0] = {i32_max, i32_min};
        exprs[1] = {Max::make(Max::make(i32_min, i32_x0), i32_y0), i32_tmin};
        exprs[2] = {Max::make(Min::make(i32_mul, i32_x0), i32_mul), i32_one};
        exprs[3] = {Max::make(Min::make(i32_mul, i32_y0), i32_mul), i32_one};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_min() {
    static bool init = false;
    static vector<AssociativePair> exprs(5);
    if (!init) {
        exprs[0] = {i32_min, i32_max};
        exprs[1] = {Min::make(Min::make(i32_max, i32_x0), i32_y0), i32_tmax};
        exprs[2] = {Min::make(Max::make(i32_mul, i32_x0), i32_mul), i32_one};
        exprs[3] = {Min::make(Max::make(i32_mul, i32_y0), i32_mul), i32_one};
        exprs[4] = {Min::make(Max::make(i32_mul, i32_y0), i32_mul), i32_zero};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_sub() {
    static bool init = false;
    static vector<AssociativePair> exprs;
    if (!init) {
        init = true;
    }
    return exprs;
}

}
}
