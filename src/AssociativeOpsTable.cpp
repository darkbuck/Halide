#include "AssociativeOpsTable.h"

using std::map;
using std::string;
using std::vector;

namespace Halide {
namespace Internal {

namespace {

enum class Identity {
    Zero,
    One,
    NegOne,
    ValMax,
    ValMin,
};

struct OpsIds {
    vector<string> ops;
    vector<Identity> identities;
    bool is_commutative;

    OpsIds() : is_commutative(false) {}
    OpsIds(const vector<string> &ops, const vector<Identity> &ids, bool is_commutative)
        : ops(ops), identities(ids), is_commutative(is_commutative) {}
    size_t size() const { return ops.size(); }
};

enum class RootExpr {
    X0 = 0,
    Y0 = 1,
    X1 = 2,
    Y1 = 3,
    Constant = 4,
    Add = 5,
    Mul = 6,
    Max = 7,
    Min = 8,
    Sub = 9,
    Select = 10,
    And = 11,
    Or = 12,
    Cast = 13,
    Unknown = 14, // Not supported IR type
};

enum class ValType {
    UInt1 = 0,
    UInt8 = 1,
    UInt16 = 2,
    UInt32 = 3,
    UInt64 = 4,
    Int8 = 5,
    Int16 = 6,
    Int32 = 7,
    Int64 = 8,
    Float32 = 9,
    Float64 = 10,
    All = 11, // General type (including all previous types)
};

ValType convert_halide_type_to_val_type(const Type &halide_t) {
    internal_assert(halide_t.is_scalar() && !halide_t.is_handle());

    ValType val_t;
    if (halide_t.is_uint()) {
        if (halide_t.bits() == 1) { // Bool
            val_t = ValType::UInt1;
        } else if (halide_t.bits() == 8) {
            val_t = ValType::UInt8;
        } else if (halide_t.bits() == 16) {
            val_t = ValType::UInt16;
        } else if (halide_t.bits() == 32) {
            val_t = ValType::UInt32;
        } else {
            internal_assert(halide_t.bits() == 64);
            val_t = ValType::UInt64;
        }
    } else if (halide_t.is_int()) {
        if (halide_t.bits() == 8) {
            val_t = ValType::Int8;
        } else if (halide_t.bits() == 16) {
            val_t = ValType::UInt16;
        } else if (halide_t.bits() == 32) {
            val_t = ValType::UInt32;
        } else {
            internal_assert(halide_t.bits() == 64);
            val_t = ValType::UInt64;
        }
    } else {
        internal_assert(halide_t.is_float());
        if (halide_t.bits() == 32) {
            val_t = ValType::Float32;
        } else {
            internal_assert(halide_t.bits() == 64);
            val_t = ValType::Float64;
        }
    }
    return val_t;
}

struct TableKey {
    ValType type;
    RootExpr root;
    size_t dim;
    TableKey(ValType t, RootExpr r, size_t d) : type(t), root(r), dim(d) {}

    bool operator==(const TableKey &other) const {
        return (type == other.type) && (root == other.root) && (dim == other.dim);
    }
    bool operator<(const TableKey &other) const {
        if (type < other.type) {
            return true;
        } else if (type > other.type) {
            return false;
        }
        if (root < other.root) {
            return true;
        } else if (root > other.root) {
            return false;
        }
        return (dim < other.dim);
    }
};

static map<TableKey, vector<AssociativePattern>> pattern_tables;

static const map<TableKey, size_t> pattern_table_sizes = {
    {TableKey(ValType::All, RootExpr::Add, 1), 15},
    {TableKey(ValType::All, RootExpr::Mul, 1), 15},
    {TableKey(ValType::All, RootExpr::Max, 1), 15},
    {TableKey(ValType::All, RootExpr::Min, 1), 15},
    {TableKey(ValType::All, RootExpr::Sub, 1), 10},
    {TableKey(ValType::All, RootExpr::Select, 1), 0},
    {TableKey(ValType::All, RootExpr::Add, 2), 14},
    {TableKey(ValType::All, RootExpr::Mul, 2), 14},
    {TableKey(ValType::All, RootExpr::Max, 2), 15},
    {TableKey(ValType::All, RootExpr::Min, 2), 15},
    {TableKey(ValType::All, RootExpr::Sub, 2), 9},
    {TableKey(ValType::All, RootExpr::Select, 2), 0},

    {TableKey(ValType::UInt1, RootExpr::And, 1), 1},
    {TableKey(ValType::UInt1, RootExpr::Or, 1), 1},

    {TableKey(ValType::UInt8, RootExpr::Cast, 1), 1},
};

const OpsIds single_gen_add[] = {
    {{"Add(X0, Y0)"}, {Identity::Zero}, true},
    {{"Add(Max(Min(Y0, Constant(All)), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Max(Sub(Constant(All), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Max(Y0, Constant(All)), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Sub(Constant(All), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Max(Min(Min(Y0, Constant(All)), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Max(Min(Mul(X0, Y0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Max(Min(Sub(X0, Y0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Max(Min(Sub(Y0, X0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Max(Max(Y0, Constant(All)), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Max(Mul(X0, Y0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Max(Sub(X0, Y0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Max(Sub(Y0, X0), Y0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Add(Min(Sub(Y0, X0), Constant(All)), Max(Y0, X0))"}, {Identity::NegOne}, true},
    {{"Add(Min(Y0, X0), Max(Sub(Y0, X0), Constant(All)))"}, {Identity::Zero}, true},
};
const OpsIds single_gen_mul[] = {
    {{"Mul(X0, Y0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Mul(X0, Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Sub(X0, Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Sub(Y0, X0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Min(Max(Mul(X0, Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Min(Max(Sub(X0, Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Min(Max(Sub(Y0, X0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Sub(Max(Min(X0, Constant(All)), Y0), Y0), X0)"}, {Identity::NegOne}, true},
    {{"Mul(Sub(Min(Max(X0, Constant(All)), Y0), Y0), X0)"}, {Identity::NegOne}, true},
    {{"Mul(Max(Min(Add(Max(X0, Constant(All)), Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Add(Min(X0, Constant(All)), Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Add(Min(Y0, X0), Constant(All)), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Add(Mul(X0, Constant(All)), Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Add(Mul(X0, Y0), Y0), Y0), Y0), X0)"}, {Identity::One}, true},
    {{"Mul(Max(Min(Add(Y0, Mul(X0, Constant(All))), Y0), Y0), X0)"}, {Identity::One}, true},
};
const OpsIds single_gen_max[] = {
    {{"Max(X0, Y0)"}, {Identity::ValMin}, true},
    {{"Max(Y0, X0)"}, {Identity::ValMin}, true},
    {{"Max(Min(X0, Constant(All)), Y0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Y0, X0), Y0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Y0, X0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Add(Min(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Min(Add(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Min(Max(Y0, Constant(All)), X0), Y0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Max(Y0, Constant(All)), Y0), X0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Max(Y0, X0), Constant(All)), Y0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)"}, {Identity::ValMin}, true},
    {{"Max(Min(Max(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Min(Min(Y0, Constant(All)), X0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Min(Mul(X0, Y0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Max(Min(Mul(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
};
const OpsIds single_gen_min[] = {
    {{"Min(X0, Y0)"}, {Identity::ValMax}, true},
    {{"Min(Max(X0, Constant(All)), Y0)"}, {Identity::ValMax}, true},
    {{"Min(Max(Y0, X0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Y0, X0), Y0)"}, {Identity::ValMax}, true},
    {{"Min(Add(Max(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Add(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Max(Y0, Constant(All)), X0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Min(Y0, Constant(All)), X0), Y0)"}, {Identity::ValMax}, true},
    {{"Min(Max(Min(Y0, Constant(All)), Y0), X0)"}, {Identity::ValMax}, true},
    {{"Min(Max(Min(Y0, X0), Constant(All)), Y0)"}, {Identity::ValMax}, true},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Min(Y0, X0), Y0), Y0)"}, {Identity::ValMax}, true},
    {{"Min(Max(Mul(X0, Y0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Mul(Y0, X0), Y0), Y0)"}, {Identity::Zero}, true},
    {{"Min(Max(Sub(Constant(All), Y0), X0), Y0)"}, {Identity::Zero}, true},
};
const OpsIds single_gen_sub[] = {
    {{"Sub(Add(Max(Y0, X0), Y0), Max(X0, Constant(All)))"}, {Identity::ValMin}, true},
    {{"Sub(Add(Min(Y0, X0), Y0), Min(X0, Constant(All)))"}, {Identity::ValMax}, true},
    {{"Sub(Max(Add(Y0, X0), Constant(All)), Max(Y0, X0))"}, {Identity::NegOne}, true},
    {{"Sub(Max(Y0, X0), Max(Sub(X0, Y0), Constant(All)))"}, {Identity::Zero}, true},
    {{"Sub(Min(Add(Y0, X0), Constant(All)), Min(Y0, X0))"}, {Identity::One}, true},
    {{"Sub(Min(Y0, X0), Min(Sub(X0, Y0), Constant(All)))"}, {Identity::Zero}, true},
    {{"Sub(Add(Max(Min(Min(Sub(Y0, X0), X0), Constant(All)), X0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Sub(Add(Max(Min(X0, Y0), Constant(All)), Max(X0, Y0)), Max(X0, Constant(All)))"}, {Identity::ValMin}, true},
    {{"Sub(Add(Min(Max(Max(Sub(Y0, X0), X0), Constant(All)), X0), Y0), X0)"}, {Identity::Zero}, true},
    {{"Sub(Add(Min(Max(X0, Y0), Constant(All)), Min(X0, Y0)), Min(X0, Constant(All)))"}, {Identity::ValMax}, true},
};
const OpsIds single_gen_select[] = {
};
const OpsIds double_gen_add[] = {
    {{"Add(X0, Y0)", "Add(X0, Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Add(X1, Y0)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y1)", "Add(X1, Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X1, Y0)", "Add(X1, Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Add(Mul(X0, Constant(All)), Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Add(Mul(X0, Y0), Add(Y1, X1))"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Max(Min(X0, X1), Max(X1, Y1))"}, {Identity::Zero, Identity::ValMin}, true},
    {{"Add(X0, Y0)", "Max(Min(X0, Y1), Max(Y1, X1))"}, {Identity::Zero, Identity::ValMin}, true},
    {{"Add(X0, Y0)", "Min(Max(X0, X1), Min(X1, Y1))"}, {Identity::Zero, Identity::ValMax}, true},
    {{"Add(X0, Y0)", "Min(Max(X0, Y1), Min(Y1, X1))"}, {Identity::Zero, Identity::ValMax}, true},
    {{"Add(X0, Y0)", "Sub(X1, Y0)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Sub(Y1, X0)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Sub(Y1, Mul(X0, Constant(All)))"}, {Identity::Zero, Identity::Zero}, true},
    {{"Add(X0, Y0)", "Sub(Add(Y1, X1), Mul(X0, Y0))"}, {Identity::Zero, Identity::Zero}, true},
};
const OpsIds double_gen_mul[] = {
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y1), X1)"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Y1)"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y0), Sub(Y1, Y0))"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X0, Y1), Mul(X1, Y0))"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Add(Y0, Y1))"}, {Identity::One, Identity::NegOne}, true},
    {{"Mul(X0, Y0)", "Add(Mul(X1, Y0), Sub(Y1, Y0))"}, {Identity::One, Identity::One}, true},
    {{"Mul(X0, Y0)", "Mul(X0, Y1)"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X0, Y0)", "Mul(X1, Y0)"}, {Identity::One, Identity::Zero}, true},
    {{"Mul(X1, Y0)", "Mul(X1, Y1)"}, {Identity::Zero, Identity::One}, true},
    {{"Mul(X0, Y0)", "Max(Min(X0, X1), Max(X1, Y1))"}, {Identity::One, Identity::ValMin}, true},
    {{"Mul(X0, Y0)", "Max(Min(X0, Y1), Max(Y1, X1))"}, {Identity::One, Identity::ValMin}, true},
    {{"Mul(X0, Y0)", "Min(Max(X0, X1), Min(X1, Y1))"}, {Identity::One, Identity::ValMax}, true},
    {{"Mul(X0, Y0)", "Min(Max(X0, Y1), Min(Y1, X1))"}, {Identity::One, Identity::ValMax}, true},
    {{"Mul(X0, Y0)", "Sub(Add(Y0, Y1), Mul(X0, Y0))"}, {Identity::One, Identity::Zero}, true},
};
const OpsIds double_gen_max[] = {
    {{"Max(X0, Y0)", "Select(LT(Y0, X0), X1, Y1)"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(X0, Y0)", "Add(Max(X0, Y0), Sub(Y1, Y0))"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(X0, Y0)", "Add(Min(X0, Y0), Add(Y1, X1))"}, {Identity::ValMin, Identity::ValMin}, true},
    {{"Max(X0, Y0)", "Add(Min(X0, Y0), Sub(X1, Y0))"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", "Add(Max(Sub(X1, X0), Y0), Max(X0, Y0))"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(Max(Min(Mul(X0, Y0), X0), Y0), X0)", "Add(Min(Max(X0, Constant(All)), Y0), Sub(X1, Y0))"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(Min(Min(Mul(X0, Y0), X0), Constant(All)), X0)", "Add(Mul(Max(X0, X1), Y1), Add(X1, Y1))"}, {Identity::Zero, Identity::Zero}, true},
    {{"Max(Min(Max(X0, Constant(All)), Min(Constant(All), X1)), Y0)", "Mul(Sub(Add(Max(X1, Y1), X1), Min(X0, X1)), Add(X0, X1))"}, {Identity::ValMin, Identity::ValMin}, true},
    {{"Max(X0, Y0)", "Max(X0, Y1)"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(X0, Y0)", "Max(X1, Y0)"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(X0, Y0)", "Max(Y0, X1)"}, {Identity::ValMin, Identity::Zero}, true},
    {{"Max(X0, Y1)", "Max(X1, Y1)"}, {Identity::Zero, Identity::ValMin}, true},
};
const OpsIds double_gen_min[] = {
    {{"Min(X0, Y0)", "Select(LT(X0, Y0), X1, Y1)"}, {Identity::ValMax, Identity::Zero}, true},
    {{"Min(X0, Y0)", "Add(Max(X0, Y0), Add(Y1, X1))"}, {Identity::ValMax, Identity::ValMin}, true},
    {{"Min(X0, Y0)", "Add(Max(X0, Y0), Sub(X1, Y0))"}, {Identity::ValMax, Identity::Zero}, true},
    {{"Min(X0, Y0)", "Add(Min(X0, Y0), Sub(Y1, Y0))"}, {Identity::ValMax, Identity::Zero}, true},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", "Add(Max(Min(X0, Constant(All)), Y0), Sub(X1, Y0))"}, {Identity::ValMax, Identity::Zero}, true},
    {{"Min(Min(Max(Mul(X0, Y0), X0), Y0), X0)", "Add(Min(Sub(X1, X0), Y0), Min(X0, Y0))"}, {Identity::ValMax, Identity::Zero}, true},
    {{"Min(X0, Y0)", "Mul(Max(X0, Y0), Mul(Y1, X1))"}, {Identity::ValMax, Identity::ValMax}, true},
    {{"Min(X0, Y0)", "Max(Min(X0, Y1), X1)"}, {Identity::ValMax, Identity::ValMin}, true},
};
const OpsIds double_gen_sub[] = {
    {{"Sub(X0, Y1)", "Add(X1, Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Sub(Y0, X1)", "Add(X1, Y1)"}, {Identity::Zero, Identity::Zero}, true},
    {{"Sub(Mul(X0, Y0), Mul(X1, Y1))", "Add(Mul(X1, Y0), Mul(X0, Y1))"}, {Identity::One, Identity::Zero}, true},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), Constant(All)))", "Sub(Add(X1, Y1), Max(Sub(X1, X0), Constant(All)))"}, {Identity::Zero, Identity::ValMax}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), Constant(All)))", "Sub(Add(X1, Y1), Min(Sub(X1, X0), Constant(All)))"}, {Identity::Zero, Identity::ValMin}, true},
    {{"Sub(Add(X1, Y0), Max(Sub(X1, X0), Constant(All)))", "Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))"}, {Identity::Zero, Identity::ValMax}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), Constant(All)))", "Sub(Y1, Mul(Max(Mul(X0, X1), X0), Sub(X0, X1)))"}, {Identity::Zero, Identity::ValMin}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), Constant(All)))", "Sub(Y1, Mul(Min(Mul(X0, X1), X0), Sub(X0, X1)))"}, {Identity::Zero, Identity::ValMin}, true},
    {{"Sub(Add(X1, Y0), Min(Sub(X1, X0), Constant(All)))", "Sub(Max(X1, Y1), Mul(Min(Mul(X0, X1), X0), Add(X0, X1)))"}, {Identity::Zero, Identity::ValMin}, true},
};
const OpsIds double_gen_select[] = {
};

const OpsIds single_uint1_and[] = {
    {{"And(X0, Y0)"}, {Identity::One}, true},
};
const OpsIds single_uint1_or[] = {
    {{"Or(X0, Y0)"}, {Identity::Zero}, true},
};

const OpsIds single_uint8_cast[] = {
    {{"Cast(UInt8, Min(Cast(UInt16, Add(X0, Y0)), Constant(UInt16)))"}, {Identity::Zero}, true},
};

static const map<TableKey, OpsIds const *> val_type_to_luts = {
    {TableKey(ValType::All, RootExpr::Add, 1), &single_gen_add[0]},
    {TableKey(ValType::All, RootExpr::Mul, 1), &single_gen_mul[0]},
    {TableKey(ValType::All, RootExpr::Max, 1), &single_gen_max[0]},
    {TableKey(ValType::All, RootExpr::Min, 1), &single_gen_min[0]},
    {TableKey(ValType::All, RootExpr::Sub, 1), &single_gen_sub[0]},
    {TableKey(ValType::All, RootExpr::Select, 1), &single_gen_select[0]},
    {TableKey(ValType::All, RootExpr::Add, 2), &double_gen_add[0]},
    {TableKey(ValType::All, RootExpr::Mul, 2), &double_gen_mul[0]},
    {TableKey(ValType::All, RootExpr::Max, 2), &double_gen_max[0]},
    {TableKey(ValType::All, RootExpr::Min, 2), &double_gen_min[0]},
    {TableKey(ValType::All, RootExpr::Sub, 2), &double_gen_sub[0]},
    {TableKey(ValType::All, RootExpr::Select, 2), &double_gen_select[0]},

    {TableKey(ValType::UInt1, RootExpr::And, 1), &single_uint1_and[0]},
    {TableKey(ValType::UInt1, RootExpr::Or, 1), &single_uint1_or[0]},

    {TableKey(ValType::UInt8, RootExpr::Cast, 1), &single_uint8_cast[0]},
};

Expr convert_op_halide_expr(const string &nodes, int &cursor, Type t) {
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
    } else if (op == "Constant(All)") {
        cursor += 2;
        return Variable::make(t, "k0");
    } else if (op == "LT") {
        cursor += 3;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return LT::make(lhs, rhs);
    } else if (op == "Or") {
        cursor += 3;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Or::make(lhs, rhs);
    }

    op = nodes.substr(cursor, 3);
    if (op == "Add") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Add::make(lhs, rhs);
    } else if (op == "Sub") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Sub::make(lhs, rhs);
    } else if (op == "Mul") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Mul::make(lhs, rhs);
    } else if (op == "Max") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Max::make(lhs, rhs);
    } else if (op == "Min") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Min::make(lhs, rhs);
    } else if (op == "And") {
        cursor += 4;
        Expr lhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr rhs = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return And::make(lhs, rhs);
    }

    op = nodes.substr(cursor, 4);
    if (op == "Cast") {
        cursor += 5;

        Type type;
        string type_5 = nodes.substr(cursor, 5);
        string type_6 = nodes.substr(cursor, 6);
        string type_7 = nodes.substr(cursor, 7);
        if (type_7 == "Float32") {
            type = Float(32);
            cursor += 9;
        } else if (type_7 == "Float64") {
            type = Float(64);
            cursor += 9;
        } else if (type_6 == "UInt16") {
            type = UInt(16);
            cursor += 8;
        } else if (type_6 == "UInt32") {
            type = UInt(32);
            cursor += 8;
        } else if (type_6 == "UInt64") {
            type = UInt(64);
            cursor += 8;
        } else if (type_5 == "UInt1") {
            type = UInt(1);
            cursor += 7;
        } else if (type_5 == "UInt8") {
            type = UInt(8);
            cursor += 7;
        } else if (type_5 == "Int16") {
            type = Int(16);
            cursor += 7;
        } else if (type_5 == "Int32") {
            type = Int(32);
            cursor += 7;
        } else if (type_5 == "Int64") {
            type = Int(64);
            cursor += 7;
        } else if (nodes.substr(cursor, 4) == "Int8") {
            type = Int(8);
            cursor += 6;
        } else {
            internal_assert(false) << "Unknown type\n";
        }

        Expr val = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Cast::make(type, val);
    }

    op = nodes.substr(cursor, 6);
    if (op == "Select") {
        cursor += 7;
        Expr cond = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr true_val = convert_op_halide_expr(nodes, cursor, t);
        cursor += 2;
        Expr false_val = convert_op_halide_expr(nodes, cursor, t);
        cursor += 1;
        return Select::make(cond, true_val, false_val);
    }

    op = nodes.substr(cursor, 8);
    if (op == "Constant") {
        cursor += 9;

        Type type;
        string type_5 = nodes.substr(cursor, 5);
        string type_6 = nodes.substr(cursor, 6);
        string type_7 = nodes.substr(cursor, 7);
        if (type_7 == "Float32") {
            type = Float(32);
            cursor += 8;
        } else if (type_7 == "Float64") {
            type = Float(64);
            cursor += 8;
        } else if (type_6 == "UInt16") {
            type = UInt(16);
            cursor += 7;
        } else if (type_6 == "UInt32") {
            type = UInt(32);
            cursor += 7;
        } else if (type_6 == "UInt64") {
            type = UInt(64);
            cursor += 7;
        } else if (type_5 == "UInt1") {
            type = UInt(1);
            cursor += 6;
        } else if (type_5 == "UInt8") {
            type = UInt(8);
            cursor += 6;
        } else if (type_5 == "Int16") {
            type = Int(16);
            cursor += 6;
        } else if (type_5 == "Int32") {
            type = Int(32);
            cursor += 6;
        } else if (type_5 == "Int64") {
            type = Int(64);
            cursor += 6;
        } else if (nodes.substr(cursor, 4) == "Int8") {
            type = Int(8);
            cursor += 5;
        } else if (nodes.substr(cursor, 3) == "All") {
            type = t;
            cursor += 4;
        } else {
            internal_assert(false) << "Unknown type\n";
        }
        return Variable::make(type, "k0");
    }

    internal_assert(false) << "Fail to convert " << nodes.substr(cursor);
    return Expr();
}

Expr convert_identity_to_halide_expr(const Identity &id, Type t) {
    switch(id) {
    case Identity::Zero:
        return make_zero(t);
    case Identity::One:
        return make_one(t);
    case Identity::NegOne:
        return make_const(t, -1);
    case Identity::ValMax:
        return t.max();
    case Identity::ValMin:
        return t.min();
    default:
        internal_assert(false) << "Invalid Identity\n";
        return Expr();
    }
}

AssociativePattern convert_pattern_to_halide_expr(const OpsIds &ops_ids, Type t) {
    AssociativePattern pattern(ops_ids.size());
    for (size_t i = 0; i < ops_ids.size(); ++i) {
        int cursor = 0;
        pattern.ops[i] = convert_op_halide_expr(ops_ids.ops[i], cursor, t);
        pattern.identities[i] = convert_identity_to_halide_expr(ops_ids.identities[i], t);
    }
    pattern.is_commutative = ops_ids.is_commutative;
    return pattern;
}

const vector<AssociativePattern> &get_ops_table_helper(Type t, RootExpr root, size_t dim) {
    TableKey gen_key(ValType::All, root, dim);
    TableKey key(convert_halide_type_to_val_type(t), root, dim);

    const auto &table_it = pattern_tables.find(key);
    if (table_it == pattern_tables.end()) { // Populate the table if we haven't done so previously
        vector<AssociativePattern> &table = pattern_tables[key];

        const auto &gen_size_it = pattern_table_sizes.find(gen_key);
        const auto &size_it = pattern_table_sizes.find(key);
        size_t gen_size = (gen_size_it != pattern_table_sizes.end()) ? gen_size_it->second : 0;
        size_t size = (size_it != pattern_table_sizes.end()) ? size_it->second : 0;
        table.reserve(gen_size + size);

        if (gen_size > 0) {
            // Add the general associative ops LUT
            OpsIds const *gen_lut_ptr = val_type_to_luts.find(gen_key)->second;
            internal_assert(gen_lut_ptr != nullptr);
            for (size_t i = 0; i < gen_size; ++i) {
                table.push_back(convert_pattern_to_halide_expr(gen_lut_ptr[i], t));
            }
        }
        if (size > 0) {
            // Add the type-specific associative ops LUT
            OpsIds const *lut_ptr = val_type_to_luts.find(key)->second;
            internal_assert(lut_ptr != nullptr);
            for (size_t i = 0; i < size; ++i) {
                table.push_back(convert_pattern_to_halide_expr(lut_ptr[i], t));
            }
        }
        return table;
    }
    return table_it->second;
}

} // anonymous namespace

const vector<AssociativePattern> &get_ops_table(const vector<Expr> &exprs) {
    internal_assert(!exprs.empty());

    // Make sure every expr in the list has the same type
    static vector<AssociativePattern> empty;
    for (size_t i = 1; i < exprs.size() - 1; ++i) {
        user_assert(exprs[i-1].type() == exprs[i].type())
            << "Tuple elements have different type. Can't prove associativity\n";
        return empty;
    }
    if (exprs.size() > 2) {
        debug(5) << "Returning empty table\n";
        return empty;
    }

    RootExpr root = RootExpr::Unknown;
    if (exprs[0].as<Halide::Internal::Add>()) {
        debug(5) << "Returning Add root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Add;
    } else if (exprs[0].as<Halide::Internal::Sub>()) {
        debug(5) << "Returning Sub root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Sub;
    } else if (exprs[0].as<Halide::Internal::Mul>()) {
        debug(5) << "Returning Mul root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Mul;
    } else if (exprs[0].as<Halide::Internal::Min>()) {
        debug(5) << "Returning Min root table\n";
        root = RootExpr::Min;
    } else if (exprs[0].as<Halide::Internal::Max>()) {
        debug(5) << "Returning Max root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Max;
    } else if (exprs[0].as<Halide::Internal::Select>()) {
        debug(5) << "Returning Select root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Select;
    } else if (exprs[0].as<Halide::Internal::And>()) {
        debug(5) << "Returning And root table for type " << exprs[0].type() << "\n";
        root = RootExpr::And;
    } else if (exprs[0].as<Halide::Internal::Or>()) {
        debug(5) << "Returning Or root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Or;
    } else if (exprs[0].as<Halide::Internal::Cast>()) {
        debug(5) << "Returning Cast root table for type " << exprs[0].type() << "\n";
        root = RootExpr::Cast;
    }

    if (root != RootExpr::Unknown) {
        const vector<AssociativePattern> &table = get_ops_table_helper(exprs[0].type(), root, exprs.size());
        debug(5) << "\tTable size: " << table.size() << "\n";
        for (const auto &p : table) {
            debug(5) << p << "\n";
        }
        return table;
    }
    debug(5) << "Returning empty table\n";
    return empty;
}

}
}
