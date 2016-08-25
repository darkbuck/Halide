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
const Expr i32_x1 = Variable::make(i32, "x1");
const Expr i32_y1 = Variable::make(i32, "y1");

const Expr i32_mul_x0y0 = Mul::make(i32_x0, i32_y0);
const Expr i32_add_x0y0 = Add::make(i32_x0, i32_y0);
const Expr i32_max_x0y0 = Max::make(i32_x0, i32_y0);
const Expr i32_min_x0y0 = Min::make(i32_x0, i32_y0);
const Expr i32_sub_x0y0 = Sub::make(i32_x0, i32_y0);

const Expr i32_mul_x1y1 = Mul::make(i32_x1, i32_y1);
const Expr i32_add_x1y1 = Add::make(i32_x1, i32_y1);
const Expr i32_max_x1y1 = Max::make(i32_x1, i32_y1);
const Expr i32_min_x1y1 = Min::make(i32_x1, i32_y1);
const Expr i32_sub_x1y1 = Sub::make(i32_x1, i32_y1);

} // anonymous namespace

const vector<AssociativePair> &get_single_i32_ops_table_add() {
    static bool init = false;
    static vector<AssociativePair> exprs(2);
    if (!init) {
        exprs[0] = {i32_add_x0y0, i32_zero};
        exprs[1] = {Add::make(Add::make(Min::make(Max::make(i32_mul_x0y0, i32_y0), i32_mul_x0y0), i32_y0), i32_x0), i32_zero};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_mul() {
    static bool init = false;
    static vector<AssociativePair> exprs(1);
    if (!init) {
        exprs[0] = {i32_mul_x0y0, i32_one};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_max() {
    static bool init = false;
    static vector<AssociativePair> exprs(4);
    if (!init) {
        exprs[0] = {i32_max_x0y0, i32_tmin};
        exprs[1] = {Max::make(Max::make(i32_min_x0y0, i32_x0), i32_y0), i32_tmin};
        exprs[2] = {Max::make(Min::make(i32_mul_x0y0, i32_x0), i32_mul_x0y0), i32_one};
        exprs[3] = {Max::make(Min::make(i32_mul_x0y0, i32_y0), i32_mul_x0y0), i32_one};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_min() {
    static bool init = false;
    static vector<AssociativePair> exprs(5);
    if (!init) {
        exprs[0] = {i32_min_x0y0, i32_tmax};
        exprs[1] = {Min::make(Min::make(i32_max_x0y0, i32_x0), i32_y0), i32_tmax};
        exprs[2] = {Min::make(Max::make(i32_mul_x0y0, i32_x0), i32_mul_x0y0), i32_one};
        exprs[3] = {Min::make(Max::make(i32_mul_x0y0, i32_y0), i32_mul_x0y0), i32_one};
        exprs[4] = {Min::make(Max::make(i32_mul_x0y0, i32_y0), i32_mul_x0y0), i32_zero};
        init = true;
    }
    return exprs;
}

const vector<AssociativePair> &get_single_i32_ops_table_sub() {
    static bool init = false;
    static vector<AssociativePair> exprs;
    if (!init) {
        exprs[0] = {i32_add_x0y0, i32_zero};
        init = true;
    }
    return exprs;
}




const vector<vector<AssociativePair>> &get_double_i32_ops_table_add() {
    static bool init = false;
    static vector<vector<AssociativePair>> exprs(2);
    if (!init) {
        init = true;
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_mul() {
    static bool init = false;
    static vector<vector<AssociativePair>> exprs(1);
    if (!init) {
        init = true;
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_max() {
    static bool init = false;
    static vector<vector<AssociativePair>> exprs(4);
    if (!init) {
        init = true;
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_min() {
    static bool init = false;
    static vector<vector<AssociativePair>> exprs(5);
    if (!init) {
        init = true;
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_sub() {
    static bool init = false;
    static vector<vector<AssociativePair>> exprs(1);
    if (!init) {
        exprs[0] = {{i32_mul_x0y0 - i32_mul_x1y1, i32_one}, {i32_x1 * i32_y0 + i32_x0 * i32_y1, i32_zero}};
        init = true;
    }
    return exprs;
}

const std::vector<std::vector<AssociativePair>> &get_i32_ops_table(const vector<Expr> &exprs) {
    internal_assert(exprs.size() == 2);
    internal_assert(exprs[0].type() == Int(32));

    static std::vector<std::vector<AssociativePair>> empty;
    if (exprs[0].as<Add>()) {
        debug(0) << "Returning add root table\n";
        return get_double_i32_ops_table_add();
    } else if (exprs[0].as<Sub>()) {
        debug(0) << "Returning sub root table\n";
        return get_double_i32_ops_table_sub();
    } else if (exprs[0].as<Mul>()) {
        debug(0) << "Returning mul root table\n";
        return get_double_i32_ops_table_mul();
    } else if (exprs[0].as<Min>()) {
        debug(0) << "Returning min root table\n";
        return get_double_i32_ops_table_min();
    } else if (exprs[0].as<Max>()) {
        debug(0) << "Returning max root table\n";
        return get_double_i32_ops_table_max();
    }
    debug(0) << "Returning empty table\n";
    return empty;
}

}
}
