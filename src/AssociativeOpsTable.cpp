#include "AssociativeOpsTable.h"

using std::vector;

namespace Halide {
namespace Internal {

namespace {

struct OpIdPair {
    std::string op;
    int64_t identity;

    OpIdPair() {}
    OpIdPair(const std::string &op) : op(op) {}
    OpIdPair(const std::string &op, int64_t id) : op(op), identity(id) {}
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

const OpIdPair single_i32_add[][1]  = {
    {{"Add(X0, Y0)", 0}},
    {{"Add(Max(Min(Y0, K0), Y0), X0)", 0}},
    {{"Add(Max(Sub(K0, Y0), Y0), X0)", 0}},
    {{"Add(Min(Max(Y0, K0), Y0), X0)", 0}},
    {{"Add(Min(Sub(K0, Y0), Y0), X0)", 0}},
    {{"Add(Max(Min(Min(Y0, K0), Y0), Y0), X0)", 0}},
    {{"Add(Max(Min(Mul(X0, Y0), Y0), Y0), X0)", 0}},
    {{"Add(Max(Min(Sub(X0, Y0), Y0), Y0), X0)", 0}},
    {{"Add(Max(Min(Sub(Y0, X0), Y0), Y0), X0)", 0}},
    {{"Add(Min(Max(Max(Y0, K0), Y0), Y0), X0)", 0}},
    {{"Add(Min(Max(Mul(X0, Y0), Y0), Y0), X0)", 0}},
    {{"Add(Min(Max(Sub(X0, Y0), Y0), Y0), X0)", 0}},
    {{"Add(Min(Max(Sub(Y0, X0), Y0), Y0), X0)", 0}},
    {{"Add(Min(Sub(Y0, X0), K0), Max(Y0, X0))", -1}},
    {{"Add(Min(Y0, X0), Max(Sub(Y0, X0), K0))", 0}},
};
const OpIdPair single_i32_mul[][1]  = {
    {{"Mul(X0, Y0)", 1}},
    {{"Mul(Max(Min(Mul(X0, Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Sub(X0, Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Sub(Y0, X0), Y0), Y0), X0)", 1}},
    {{"Mul(Min(Max(Mul(X0, Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Min(Max(Sub(X0, Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Min(Max(Sub(Y0, X0), Y0), Y0), X0)", 1}},
    {{"Mul(Sub(Max(Min(X0, K0), Y0), Y0), X0)", -1}},
    {{"Mul(Sub(Min(Max(X0, K0), Y0), Y0), X0)", -1}},
    {{"Mul(Max(Min(Add(Max(X0, K0), Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Add(Min(X0, K0), Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Add(Min(Y0, X0), K0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Add(Mul(X0, K0), Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Add(Mul(X0, Y0), Y0), Y0), Y0), X0)", 1}},
    {{"Mul(Max(Min(Add(Y0, Mul(X0, K0)), Y0), Y0), X0)", 1}},
};
const OpIdPair single_i32_max[][1]  = {
    {{"Max(X0, Y0)", -2147483648}},
    {{"Max(Y0, X0)", -2147483648}},
    {{"Max(Min(X0, K0), Y0)", -2147483648}},
    {{"Max(Min(Y0, X0), Y0)", -2147483648}},
    {{"Max(Min(Y0, X0), Y0)", 0}},
    {{"Max(Add(Min(Y0, X0), Y0), Y0)", 0}},
    {{"Max(Min(Add(Y0, X0), Y0), Y0)", 0}},
    {{"Max(Min(Max(Y0, K0), X0), Y0)", -2147483648}},
    {{"Max(Min(Max(Y0, K0), Y0), X0)", -2147483648}},
    {{"Max(Min(Max(Y0, X0), K0), Y0)", -2147483648}},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)", -2147483648}},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)", 0}},
    {{"Max(Min(Min(Y0, K0), X0), Y0)", 0}},
    {{"Max(Min(Mul(X0, Y0), Y0), Y0)", 0}},
    {{"Max(Min(Mul(Y0, X0), Y0), Y0)", 0}},
};
const OpIdPair single_i32_min[][1]  = {
    {{"Min(X0, Y0)", 2147483647}},
    {{"Min(Max(X0, K0), Y0)", 2147483647}},
    {{"Min(Max(Y0, X0), Y0)", 0}},
    {{"Min(Max(Y0, X0), Y0)", 2147483647}},
    {{"Min(Add(Max(Y0, X0), Y0), Y0)", 0}},
    {{"Min(Max(Add(Y0, X0), Y0), Y0)", 0}},
    {{"Min(Max(Max(Y0, K0), X0), Y0)", 0}},
    {{"Min(Max(Min(Y0, K0), X0), Y0)", 2147483647}},
    {{"Min(Max(Min(Y0, K0), Y0), X0)", 2147483647}},
    {{"Min(Max(Min(Y0, X0), K0), Y0)", 2147483647}},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)", 0}},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)", 2147483647}},
    {{"Min(Max(Mul(X0, Y0), Y0), Y0)", 0}},
    {{"Min(Max(Mul(Y0, X0), Y0), Y0)", 0}},
    {{"Min(Max(Sub(K0, Y0), X0), Y0)", 0}},
};
const OpIdPair single_i32_sub[][1]  = {
    {{"Sub(Add(Max(Y0, X0), Y0), Max(X0, K0))", -2147483648}},
    {{"Sub(Add(Min(Y0, X0), Y0), Min(X0, K0))", 2147483647}},
    {{"Sub(Max(Add(Y0, X0), K0), Max(Y0, X0))", -1}},
    {{"Sub(Max(Y0, X0), Max(Sub(X0, Y0), K0))", 0}},
    {{"Sub(Min(Add(Y0, X0), K0), Min(Y0, X0))", 1}},
    {{"Sub(Min(Y0, X0), Min(Sub(X0, Y0), K0))", 0}},
    {{"Sub(Add(Max(Min(Min(Sub(Y0, X0), X0), K0), X0), Y0), X0)", 0}},
    {{"Sub(Add(Max(Min(X0, Y0), K0), Max(X0, Y0)), Max(X0, K0))", -2147483648}},
    {{"Sub(Add(Min(Max(Max(Sub(Y0, X0), X0), K0), X0), Y0), X0)", 0}},
    {{"Sub(Add(Min(Max(X0, Y0), K0), Min(X0, Y0)), Min(X0, K0))", 2147483647}},
};
const OpIdPair single_i32_select[][1]  = {
};

const OpIdPair double_i32_add[][2] = {
    {{"Add(X0, Y0)", 0}, {"Add(X0, Y1)", 0}},
    {{"Add(X0, Y0)", 0}, {"Add(X1, Y0)", 0}},
    {{"Add(X0, Y1)", 0}, {"Add(X1, Y1)", 0}},
    {{"Add(X1, Y0)", 0}, {"Add(X1, Y1)", 0}},
    {{"Add(X0, Y0)", 0}, {"Add(Mul(X0, K0), Y1)", 0}},
    {{"Add(X0, Y0)", 0}, {"Add(Mul(X0, Y0), Add(Y1, X1))", 0}},
    {{"Add(X0, Y0)", 0}, {"Max(Min(X0, X1), Max(X1, Y1))", -2147483648}},
    {{"Add(X0, Y0)", 0}, {"Max(Min(X0, Y1), Max(Y1, X1))", -2147483648}},
    {{"Add(X0, Y0)", 0}, {"Min(Max(X0, X1), Min(X1, Y1))", 2147483647}},
    {{"Add(X0, Y0)", 0}, {"Min(Max(X0, Y1), Min(Y1, X1))", 2147483647}},
    {{"Add(X0, Y0)", 0}, {"Sub(X1, Y0)", 0}},
    {{"Add(X0, Y0)", 0}, {"Sub(Y1, X0)", 0}},
    {{"Add(X0, Y0)", 0}, {"Sub(Y1, Mul(X0, K0))", 0}},
    {{"Add(X0, Y0)", 0}, {"Sub(Add(Y1, X1), Mul(X0, Y0))", 0}},
};
const OpIdPair double_i32_mul[][2] = {
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X0, Y1), X1)", 0}},
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X1, Y0), Y1)", 0}},
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X0, Y0), Sub(Y1, Y0))", 0}},
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X0, Y1), Mul(X1, Y0))", 0}},
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X1, Y0), Add(Y0, Y1))", -1}},
    {{"Mul(X0, Y0)", 1}, {"Add(Mul(X1, Y0), Sub(Y1, Y0))", 1}},
    {{"Mul(X0, Y0)", 1}, {"Mul(X0, Y1)", 0}},
    {{"Mul(X0, Y0)", 1}, {"Mul(X1, Y0)", 0}},
    {{"Mul(X1, Y0)", 0}, {"Mul(X1, Y1)", 1}},
    {{"Mul(X0, Y0)", 1}, {"Max(Min(X0, X1), Max(X1, Y1))", -2147483648}},
    {{"Mul(X0, Y0)", 1}, {"Max(Min(X0, Y1), Max(Y1, X1))", -2147483648}},
    {{"Mul(X0, Y0)", 1}, {"Min(Max(X0, X1), Min(X1, Y1))", 2147483647}},
    {{"Mul(X0, Y0)", 1}, {"Min(Max(X0, Y1), Min(Y1, X1))", 2147483647}},
    {{"Mul(X0, Y0)", 1}, {"Sub(Add(Y0, Y1), Mul(X0, Y0))", 0}},
};
const OpIdPair double_i32_max[][2] = {
    {{"Max(X0, Y0)", -2147483648}, {"Select(LT(Y0, X0), X1, Y1)", 0}},
    {{"Max(X0, Y0)", -2147483648}, {"Add(Max(X0, Y0), Sub(Y1, Y0))", 0}},
    {{"Max(X0, Y0)", -2147483648}, {"Add(Min(X0, Y0), Add(Y1, X1))", -2147483648}},
    {{"Max(X0, Y0)", -2147483648}, {"Add(Min(X0, Y0), Sub(X1, Y0))", 0}},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", -2147483648}, {"Add(Max(Sub(X1, X0), Y0), Max(X0, Y0))", 0}},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", -2147483648}, {"Add(Min(Max(X0, K0), Y0), Sub(X1, Y0))", 0}},
    {{"Max(Min(Min(Mul(X0, Y0), X0), K0), X0)", 0}, {"Add(Mul(Max(X0, X1), Y1), Add(X1, Y1))", 0}},
    {{"Max(Min(Max(X0, K0), Min(K0, X1)), Y0)", -2147483647}, {"Mul(Sub(Add(Max(X1, Y1), X1), Min(X0, X1)), Add(X0, X1))", -2147483648}},
    {{"Max(X0, Y0)", -2147483648}, {"Max(X0, Y1)", 0}},
    {{"Max(X0, Y0)", -2147483648}, {"Max(X1, Y0)", 0}},
    {{"Max(X0, Y0)", -2147483648}, {"Max(Y0, X1)", 0}},
    {{"Max(X0, Y1)", 0}, {"Max(X1, Y1)", -2147483648}},
};
const OpIdPair double_i32_min[][2] = {
    {{"Min(X0, Y0)", 2147483647}, {"Select(LT(X0, Y0), X1, Y1)", 0}},
    {{"Min(X0, Y0)", 2147483647}, {"Add(Max(X0, Y0), Add(Y1, X1))", -2147483647}},
    {{"Min(X0, Y0)", 2147483647}, {"Add(Max(X0, Y0), Sub(X1, Y0))", 0}},
    {{"Min(X0, Y0)", 2147483647}, {"Add(Min(X0, Y0), Sub(Y1, Y0))", 0}},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", 2147483647}, {"Add(Max(Min(X0, K0), Y0), Sub(X1, Y0))", 0}},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", 2147483647}, {"Add(Min(Sub(X1, X0), Y0), Min(X0, Y0))", 0}},
    {{"Min(X0, Y0)", 2147483647}, {"Mul(Max(X0, Y0), Mul(Y1, X1))", 2147483647}},
    {{"Min(X0, Y0)", 2147483647}, {"Max(Min(X0, Y1), X1)", -2147483648}},
};
const OpIdPair double_i32_sub[][2] = {
    {{"Sub(X0, Y1)", 0}, {"Add(X1, Y1)", 0}},
    {{"Sub(Y0, X1)", 0}, {"Add(X1, Y1)", 0}},
    {{"Sub(Mul(X0, Y0), Mul(X1, Y1))", 1}, {"Add(Mul(X1, Y0), Mul(X0, Y1))", 0}},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), K0))", 0}, {"Sub(Add(X1, Y1), Max(Sub(X1, X0), K0))", 2147483647}},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", 0}, {"Sub(Add(X1, Y1), Min(Sub(X1, X0), K0))", -2147483648}},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), K0))", 0}, {"Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))", 2147483647}},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", 0}, {"Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))", -2147483648}},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", 0}, {"Sub(Y1, Mul(Min(Mul(X0, X1), X0), Sub(X0, X1)))", -2147483648}},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), K0))", 0}, {"Sub(Max(X1, Y1), Mul(Min(Mul(X0, X1), X0), Add(X0, X1)))", -2147483648}},
};
const OpIdPair double_i32_select[][2] = {
};

Expr convert_to_expr_term_helper(const std::string &nodes, int &cursor, Type t) {
    std::string op = nodes.substr(cursor, 2);
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

AssociativePair convert_op_to_halide_expr(const OpIdPair &pair, Type t) {
    int cursor = 0;
    Expr expr = convert_to_expr_term_helper(pair.op, cursor, t);
    return AssociativePair(expr, make_const(t, pair.identity));
}

vector<AssociativePair> convert_ops_to_halide_expr(const OpIdPair *data, Type t, size_t size) {
    vector<AssociativePair> result(size);
    for (int i = 0; i < (int)size; ++i) {
        result[i] = convert_op_to_halide_expr(data[i], t);
    }
    return result;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_add(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(252);
    if (exprs.size() != single_i32_add_size) {
        for (int i = 0; i < (int)single_i32_add_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_add[i], Int(32), 1));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_mul(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(222);
    if (exprs.size() != single_i32_mul_size) {
        for (int i = 0; i < (int)single_i32_mul_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_mul[i], Int(32), 1));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_max(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(2880);
    if (exprs.size() != single_i32_max_size) {
        for (int i = 0; i < (int)single_i32_max_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_max[i], Int(32), 1));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_min(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(2797);
    if (exprs.size() != single_i32_min_size) {
        for (int i = 0; i < (int)single_i32_min_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_min[i], Int(32), 1));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_sub(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(10);
    if (exprs.size() != single_i32_sub_size) {
        for (int i = 0; i < (int)single_i32_sub_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_sub[i], Int(32), 1));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_single_i32_ops_table_select(Expr e) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != single_i32_select_size) {
        for (int i = 0; i < (int)single_i32_select_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(single_i32_select[i], Int(32), 1));
        }
    }
    return exprs;
}


const vector<vector<AssociativePair>> &get_double_i32_ops_table_add(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_add_size) {
        for (int i = 0; i < (int)double_i32_add_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_add[i], Int(32), 2));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_mul(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_mul_size) {
        for (int i = 0; i < (int)double_i32_mul_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_mul[i], Int(32), 2));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_max(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_max_size) {
        for (int i = 0; i < (int)double_i32_max_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_max[i], Int(32), 2));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_min(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_min_size) {
        for (int i = 0; i < (int)double_i32_min_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_min[i], Int(32), 2));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_sub(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_sub_size) {
        for (int i = 0; i < (int)double_i32_sub_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_sub[i], Int(32), 2));
        }
    }
    return exprs;
}

const vector<vector<AssociativePair>> &get_double_i32_ops_table_select(Expr e0, Expr e1) {
    static vector<vector<AssociativePair>> exprs;
    exprs.reserve(0);
    if (exprs.size() != double_i32_select_size) {
        for (int i = 0; i < (int)double_i32_select_size; ++i) {
            exprs.push_back(convert_ops_to_halide_expr(double_i32_select[i], Int(32), 2));
        }
    }
    return exprs;
}

} // anonymous namespace

const std::vector<std::vector<AssociativePair>> &get_i32_ops_table(const vector<Expr> &exprs) {

    static std::vector<std::vector<AssociativePair>> empty;
    if (exprs[0].as<Halide::Internal::Add>()) {
        debug(5) << "Returning add root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_add(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_add(exprs[0], exprs[1]);
        }
    } else if (exprs[0].as<Halide::Internal::Sub>()) {
        debug(5) << "Returning sub root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_sub(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_sub(exprs[0], exprs[1]);
        }
    } else if (exprs[0].as<Halide::Internal::Mul>()) {
        debug(5) << "Returning mul root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_mul(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_mul(exprs[0], exprs[1]);
        }
    } else if (exprs[0].as<Halide::Internal::Min>()) {
        debug(5) << "Returning min root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_min(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_min(exprs[0], exprs[1]);
        }
    } else if (exprs[0].as<Halide::Internal::Max>()) {
        debug(5) << "Returning max root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_max(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_max(exprs[0], exprs[1]);
        }
    } else if (exprs[0].as<Halide::Internal::Select>()) {
        debug(5) << "Returning select root table\n";
        if (exprs.size() == 1) {
            return get_single_i32_ops_table_select(exprs[0]);
        } else if (exprs.size() == 2) {
            return get_double_i32_ops_table_select(exprs[0], exprs[1]);
        }
    }
    debug(5) << "Returning empty table\n";
    return empty;
}

}
}
