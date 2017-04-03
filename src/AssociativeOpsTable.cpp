#include "AssociativeOpsTable.h"

using std::string;
using std::vector;

namespace Halide {
namespace Internal {

namespace {

struct OpsIds {
    vector<string> ops;
    vector<int64_t> identities;
    bool is_commutative;

    OpsIds() : is_commutative(false) {}
    OpsIds(const vector<string> &ops, const vector<int64_t> &ids, bool is_commutative)
        : ops(ops), identities(ids), is_commutative(is_commutative) {}
    size_t size() const { return ops.size(); }
};

const size_t single_i32_add_size = 15;
const size_t single_i32_mul_size = 15;
const size_t single_i32_max_size = 15;
const size_t single_i32_min_size = 15;
const size_t single_i32_sub_size = 10;
const size_t single_i32_select_size = 0;

const size_t double_i32_add_size = 14;
const size_t double_i32_mul_size = 14;
const size_t double_i32_max_size = 15;
const size_t double_i32_min_size = 15;
const size_t double_i32_sub_size = 9;
const size_t double_i32_select_size = 0;

const OpsIds single_i32_add[] = {
    {{"Add(X0, Y0)"}, {0}, true},
    {{"Add(Max(Min(Y0, K0), Y0), X0)"}, {0}, true},
    {{"Add(Max(Sub(K0, Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Max(Y0, K0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Sub(K0, Y0), Y0), X0)"}, {0}, true},
    {{"Add(Max(Min(Min(Y0, K0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Max(Min(Mul(X0, Y0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Max(Min(Sub(X0, Y0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Max(Min(Sub(Y0, X0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Max(Max(Y0, K0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Max(Mul(X0, Y0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Max(Sub(X0, Y0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Max(Sub(Y0, X0), Y0), Y0), X0)"}, {0}, true},
    {{"Add(Min(Sub(Y0, X0), K0), Max(Y0, X0))"}, {-1}, true},
    {{"Add(Min(Y0, X0), Max(Sub(Y0, X0), K0))"}, {0}, true},
};
const OpsIds single_i32_mul[] = {
    {{"Mul(X0, Y0)"}, {1}, true},
    {{"Mul(Max(Min(Mul(X0, Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Sub(X0, Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Sub(Y0, X0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Min(Max(Mul(X0, Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Min(Max(Sub(X0, Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Min(Max(Sub(Y0, X0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Sub(Max(Min(X0, K0), Y0), Y0), X0)"}, {-1}, true},
    {{"Mul(Sub(Min(Max(X0, K0), Y0), Y0), X0)"}, {-1}, true},
    {{"Mul(Max(Min(Add(Max(X0, K0), Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Add(Min(X0, K0), Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Add(Min(Y0, X0), K0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Add(Mul(X0, K0), Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Add(Mul(X0, Y0), Y0), Y0), Y0), X0)"}, {1}, true},
    {{"Mul(Max(Min(Add(Y0, Mul(X0, K0)), Y0), Y0), X0)"}, {1}, true},
};
const OpsIds single_i32_max[] = {
    {{"Max(X0, Y0)"}, {-2147483648}, true},
    {{"Max(Y0, X0)"}, {-2147483648}, true},
    {{"Max(Min(X0, K0), Y0)"}, {-2147483648}, true},
    {{"Max(Min(Y0, X0), Y0)"}, {-2147483648}, true},
    {{"Max(Min(Y0, X0), Y0)"}, {0}, true},
    {{"Max(Add(Min(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Max(Min(Add(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Max(Min(Max(Y0, K0), X0), Y0)"}, {-2147483648}, true},
    {{"Max(Min(Max(Y0, K0), Y0), X0)"}, {-2147483648}, true},
    {{"Max(Min(Max(Y0, X0), K0), Y0)"}, {-2147483648}, true},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)"}, {-2147483648}, true},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Max(Min(Min(Y0, K0), X0), Y0)"}, {0}, true},
    {{"Max(Min(Mul(X0, Y0), Y0), Y0)"}, {0}, true},
    {{"Max(Min(Mul(Y0, X0), Y0), Y0)"}, {0}, true},
};
const OpsIds single_i32_min[] = {
    {{"Min(X0, Y0)"}, {2147483647}, true},
    {{"Min(Max(X0, K0), Y0)"}, {2147483647}, true},
    {{"Min(Max(Y0, X0), Y0)"}, {0}, true},
    {{"Min(Max(Y0, X0), Y0)"}, {2147483647}, true},
    {{"Min(Add(Max(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Min(Max(Add(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Min(Max(Max(Y0, K0), X0), Y0)"}, {0}, true},
    {{"Min(Max(Min(Y0, K0), X0), Y0)"}, {2147483647}, true},
    {{"Min(Max(Min(Y0, K0), Y0), X0)"}, {2147483647}, true},
    {{"Min(Max(Min(Y0, X0), K0), Y0)"}, {2147483647}, true},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)"}, {2147483647}, true},
    {{"Min(Max(Mul(X0, Y0), Y0), Y0)"}, {0}, true},
    {{"Min(Max(Mul(Y0, X0), Y0), Y0)"}, {0}, true},
    {{"Min(Max(Sub(K0, Y0), X0), Y0)"}, {0}, true},
};
const OpsIds single_i32_sub[] = {
    {{"Sub(Add(Max(Y0, X0), Y0), Max(X0, K0))"}, {-2147483648}, true},
    {{"Sub(Add(Min(Y0, X0), Y0), Min(X0, K0))"}, {2147483647}, true},
    {{"Sub(Max(Add(Y0, X0), K0), Max(Y0, X0))"}, {-1}, true},
    {{"Sub(Max(Y0, X0), Max(Sub(X0, Y0), K0))"}, {0}, true},
    {{"Sub(Min(Add(Y0, X0), K0), Min(Y0, X0))"}, {1}, true},
    {{"Sub(Min(Y0, X0), Min(Sub(X0, Y0), K0))"}, {0}, true},
    {{"Sub(Add(Max(Min(Min(Sub(Y0, X0), X0), K0), X0), Y0), X0)"}, {0}, true},
    {{"Sub(Add(Max(Min(X0, Y0), K0), Max(X0, Y0)), Max(X0, K0))"}, {-2147483648}, true},
    {{"Sub(Add(Min(Max(Max(Sub(Y0, X0), X0), K0), X0), Y0), X0)"}, {0}, true},
    {{"Sub(Add(Min(Max(X0, Y0), K0), Min(X0, Y0)), Min(X0, K0))"}, {2147483647}, true},
};
const OpsIds single_i32_select[] = {
};

const OpsIds double_i32_add[] = {
    {{"Add(X0, Y0)", "Add(X0, Y1)"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Add(X1, Y0)"}, {0, 0}, true},
    {{"Add(X0, Y1)", "Add(X1, Y1)"}, {0, 0}, true},
    {{"Add(X1, Y0)", "Add(X1, Y1)"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Add(Mul(X0, K0), Y1)"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Add(Mul(X0, Y0), Add(Y1, X1))"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Max(Min(X0, X1), Max(X1, Y1))"}, {0, -2147483648}, true},
    {{"Add(X0, Y0)", "Max(Min(X0, Y1), Max(Y1, X1))"}, {0, -2147483648}, true},
    {{"Add(X0, Y0)", "Min(Max(X0, X1), Min(X1, Y1))"}, {0, 2147483647}, true},
    {{"Add(X0, Y0)", "Min(Max(X0, Y1), Min(Y1, X1))"}, {0, 2147483647}, true},
    {{"Add(X0, Y0)", "Sub(X1, Y0)"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Sub(Y1, X0)"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Sub(Y1, Mul(X0, K0))"}, {0, 0}, true},
    {{"Add(X0, Y0)", "Sub(Add(Y1, X1), Mul(X0, Y0))"}, {0, 0}, true},
};
const OpsIds double_i32_mul[] = {
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y1), X1)"}, {1, 0}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Y1)"}, {1, 0}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y0), Sub(Y1, Y0))"}, {1, 0}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y1), Mul(X1, Y0))"}, {1, 0}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Add(Y0, Y1))"}, {1, -1}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Sub(Y1, Y0))"}, {1, 1}, true},
    {{"Mul(X0, Y0)", "Mul(X0, Y1)"}, {1, 0}, true},
    {{"Mul(X0, Y0)", "Mul(X1, Y0)"}, {1, 0}, true},
    {{"Mul(X1, Y0)", "Mul(X1, Y1)"}, {0, 1}, true},
    {{"Mul(X0, Y0)", "Max(Min(X0, X1), Max(X1, Y1))"}, {1, -2147483648}, true},
    {{"Mul(X0, Y0)", "Max(Min(X0, Y1), Max(Y1, X1))"}, {1, -2147483648}, true},
    {{"Mul(X0, Y0)", "Min(Max(X0, X1), Min(X1, Y1))"}, {1, 2147483647}, true},
    {{"Mul(X0, Y0)", "Min(Max(X0, Y1), Min(Y1, X1))"}, {1, 2147483647}, true},
    {{"Mul(X0, Y0)", "Sub(Add(Y0, Y1), Mul(X0, Y0))"}, {1, 0}, true},
};
const OpsIds double_i32_max[] = {
    {{"Max(X0, Y0)", "Select(LT(Y0, X0), X1, Y1)"}, {-2147483648, 0}, true},
    {{"Max(X0, Y0)", "Add(Max(X0, Y0), Sub(Y1, Y0))"}, {-2147483648, 0}, true},
    {{"Max(X0, Y0)", "Add(Min(X0, Y0), Add(Y1, X1))"}, {-2147483648, -2147483648}, true},
    {{"Max(X0, Y0)", "Add(Min(X0, Y0), Sub(X1, Y0))"}, {-2147483648, 0}, true},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", "Add(Max(Sub(X1, X0), Y0), Max(X0, Y0))"}, {-2147483648, 0}, true},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", "Add(Min(Max(X0, K0), Y0), Sub(X1, Y0))"}, {-2147483648, 0}, true},
    {{"Max(Min(Min(Mul(X0, Y0), X0), K0), X0)", "Add(Mul(Max(X0, X1), Y1), Add(X1, Y1))"}, {0, 0}, true},
    {{"Max(Min(Max(X0, K0), Min(K0, X1)), Y0)", "Mul(Sub(Add(Max(X1, Y1), X1), Min(X0, X1)), Add(X0, X1))"}, {-2147483648, -2147483648}, true},
    {{"Max(X0, Y0)", "Max(X0, Y1)"}, {-2147483648, 0}, true},
    {{"Max(X0, Y0)", "Max(X1, Y0)"}, {-2147483648, 0}, true},
    {{"Max(X0, Y0)", "Max(Y0, X1)"}, {-2147483648, 0}, true},
    {{"Max(X0, Y1)", "Max(X1, Y1)"}, {0, -2147483648}, true},
};
const OpsIds double_i32_min[] = {
    {{"Min(X0, Y0)", "Select(LT(X0, Y0), X1, Y1)"}, {2147483647, 0}, true},
    {{"Min(X0, Y0)", "Add(Max(X0, Y0), Add(Y1, X1))"}, {2147483647, -2147483647}, true},
    {{"Min(X0, Y0)", "Add(Max(X0, Y0), Sub(X1, Y0))"}, {2147483647, 0}, true},
    {{"Min(X0, Y0)", "Add(Min(X0, Y0), Sub(Y1, Y0))"}, {2147483647, 0}, true},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", "Add(Max(Min(X0, K0), Y0), Sub(X1, Y0))"}, {2147483647, 0}, true},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", "Add(Min(Sub(X1, X0), Y0), Min(X0, Y0))"}, {2147483647, 0}, true},
    {{"Min(X0, Y0)", "Mul(Max(X0, Y0), Mul(Y1, X1))"}, {2147483647, 2147483647}, true},
    {{"Min(X0, Y0)", "Max(Min(X0, Y1), X1)"}, {2147483647, -2147483648}, true},
};
const OpsIds double_i32_sub[] = {
    {{"Sub(X0, Y1)", "Add(X1, Y1)"}, {0, 0}, true},
    {{"Sub(Y0, X1)", "Add(X1, Y1)"}, {0, 0}, true},
    {{"Sub(Mul(X0, Y0), Mul(X1, Y1))", "Add(Mul(X1, Y0), Mul(X0, Y1))"}, {1, 0}, true},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), K0))", "Sub(Add(X1, Y1), Max(Sub(X1, X0), K0))"}, {0, 2147483647}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", "Sub(Add(X1, Y1), Min(Sub(X1, X0), K0))"}, {0, -2147483648}, true},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), K0))", "Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))"}, {0, 2147483647}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", "Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))"}, {0, -2147483648}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", "Sub(Y1, Mul(Min(Mul(X0, X1), X0), Sub(X0, X1)))"}, {0, -2147483648}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", "Sub(Max(X1, Y1), Mul(Min(Mul(X0, X1), X0), Add(X0, X1)))"}, {0, -2147483648}, true},
};
const OpsIds double_i32_select[] = {
};

Expr convert_to_expr_term_helper(const string &nodes, int &cursor, Type t) {
    string op = nodes.substr(cursor, 2);
    if (op == "X0") {
        cursor += 2;
        return Variable::make(t, "x0");
    } else if (op == "X1") {
        cursor += 2;
        return Variable::make(t, "x1");
    } else if (op == "Y0") {
        cursor += 2;
        return Variable::make(t, "y0");
    } else if (op == "Y1") {
        cursor += 2;
        return Variable::make(t, "y1");
    } else if (op == "K0") {
        cursor += 2;
        return Variable::make(t, "k0");
    } else if (op == "LT") {
        cursor += 3;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return LT::make(lhs, rhs);
    }

    op = nodes.substr(cursor, 3);
    if (op == "Add") {
        cursor += 4;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Add::make(lhs, rhs);
    } else if (op == "Sub") {
        cursor += 4;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Sub::make(lhs, rhs);
    } else if (op == "Mul") {
        cursor += 4;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Mul::make(lhs, rhs);
    } else if (op == "Max") {
        cursor += 4;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Max::make(lhs, rhs);
    } else if (op == "Min") {
        cursor += 4;
        Expr lhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Min::make(lhs, rhs);
    }

    op = nodes.substr(cursor, 6);
    if (op == "Select") {
        cursor += 7;
        Expr cond = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr true_val = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 2;
        Expr false_val = convert_to_expr_term_helper(nodes, cursor, t);
        cursor += 1;
        return Select::make(cond, true_val, false_val);
    }
    assert(false);
    return Expr();
}

AssociativePattern convert_op_to_halide_expr(const OpsIds &ops_ids, Type t) {
    AssociativePattern pattern(ops_ids.size());
    for (size_t i = 0; i < ops_ids.size(); ++i) {
        int cursor = 0;
        pattern.ops[i] = convert_to_expr_term_helper(ops_ids.ops[i], cursor, t);
        pattern.identities[i] = make_const(t, ops_ids.identities[i]);
    }
    pattern.is_commutative = ops_ids.is_commutative;
    return pattern;
}

const vector<AssociativePattern> &get_single_i32_ops_table_add() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_add_size);
    if (patterns.size() != single_i32_add_size) {
        for (int i = 0; i < (int)single_i32_add_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_add[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_single_i32_ops_table_mul() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_mul_size);
    if (patterns.size() != single_i32_mul_size) {
        for (int i = 0; i < (int)single_i32_mul_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_mul[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_single_i32_ops_table_max() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_max_size);
    if (patterns.size() != single_i32_max_size) {
        for (int i = 0; i < (int)single_i32_max_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_max[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_single_i32_ops_table_min() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_min_size);
    if (patterns.size() != single_i32_min_size) {
        for (int i = 0; i < (int)single_i32_min_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_min[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_single_i32_ops_table_sub() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_sub_size);
    if (patterns.size() != single_i32_sub_size) {
        for (int i = 0; i < (int)single_i32_sub_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_sub[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_single_i32_ops_table_select() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(single_i32_select_size);
    if (patterns.size() != single_i32_select_size) {
        for (int i = 0; i < (int)single_i32_select_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(single_i32_select[i], Int(32)));
        }
    }
    return patterns;
}


const vector<AssociativePattern> &get_double_i32_ops_table_add() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_add_size);
    if (patterns.size() != double_i32_add_size) {
        for (int i = 0; i < (int)double_i32_add_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_add[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_double_i32_ops_table_mul() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_mul_size);
    if (patterns.size() != double_i32_mul_size) {
        for (int i = 0; i < (int)double_i32_mul_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_mul[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_double_i32_ops_table_max() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_max_size);
    if (patterns.size() != double_i32_max_size) {
        for (int i = 0; i < (int)double_i32_max_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_max[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_double_i32_ops_table_min() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_min_size);
    if (patterns.size() != double_i32_min_size) {
        for (int i = 0; i < (int)double_i32_min_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_min[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_double_i32_ops_table_sub() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_sub_size);
    if (patterns.size() != double_i32_sub_size) {
        for (int i = 0; i < (int)double_i32_sub_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_sub[i], Int(32)));
        }
    }
    return patterns;
}

const vector<AssociativePattern> &get_double_i32_ops_table_select() {
    static vector<AssociativePattern> patterns;
    patterns.reserve(double_i32_select_size);
    if (patterns.size() != double_i32_select_size) {
        for (int i = 0; i < (int)double_i32_select_size; ++i) {
            patterns.push_back(convert_op_to_halide_expr(double_i32_select[i], Int(32)));
        }
    }
    return patterns;
}

} // anonymous namespace

const vector<AssociativePattern> &get_i32_ops_table(const vector<Expr> &exprs) {

    static vector<AssociativePattern> empty;
    if (exprs[0].as<Halide::Internal::Add>()) {
        debug(5) << "Returning add root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_add();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_add();
        }
    } else if (exprs[0].as<Halide::Internal::Sub>()) {
        debug(5) << "Returning sub root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_sub();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_sub();
        }
    } else if (exprs[0].as<Halide::Internal::Mul>()) {
        debug(5) << "Returning mul root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_mul();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_mul();
        }
    } else if (exprs[0].as<Halide::Internal::Min>()) {
        debug(5) << "Returning min root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_min();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_min();
        }
    } else if (exprs[0].as<Halide::Internal::Max>()) {
        debug(5) << "Returning max root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_max();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_max();
        }
    } else if (exprs[0].as<Halide::Internal::Select>()) {
        debug(5) << "Returning select root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_select();
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_select();
        }
    }
    debug(5) << "Returning empty table\n";
    return empty;
}

}
}
