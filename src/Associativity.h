#ifndef HALIDE_ASSOCIATIVITY_H
#define HALIDE_ASSOCIATIVITY_H

/** \file
 *
 * Methods for extracting an associative operator from a Func's update definition
 * if there is any and computing the identity of the associative operator.
 */

#include "IR.h"
#include "IREquality.h"
#include "AssociativeOpsTable.h"

#include <functional>

namespace Halide {
namespace Internal {

/**
 * Represent the equivalent associative binary/unary operator of an associative Expr.
 * For example, the following associative Expr, min(f(x), g(r.x) + 2), where f(x)
 * is the self-recurrence term, will be represented as:
 \code
 AssociativeOps assoc = {
        min(x, y),
        +inf,
        {"x", f(x)},
        {"y", g(r.x) + 2},
 };
 \endcode
 *
 * For unary operator, 'x' is not set, i.e. it will be a pair of empty string
 * and undefined Expr: {"", Expr()}. 'op' will only contain the 'y' term in
 * this case. For example, min(g(r.x), 4), will be represented as:
 \code
 AssociativeOps assoc = {
        y,
        0,
        {"", Expr()},
        {"y", min(g(r.x), 4)},
 };
 \endcode
 * Since it is a unary operator, the identity does not matter. It can be
 * anything.
 */
struct AssociativeOps {
    struct Replacement {
        std::string var; // Variable name that is used to replace the expr in 'op'
        Expr expr;

        Replacement() {}
        Replacement(const std::string &var, Expr expr) : var(var), expr(expr) {}

        bool operator==(const Replacement &other) const {
            return (var == other.var) && equal(expr, other.expr);
        }
        bool operator!=(const Replacement &other) const {
            return !(*this == other);
        }
    };

    // List of pairs of binary associative op and its identity
    std::vector<AssociativePair> ops;
    std::vector<Replacement> x;
    std::vector<Replacement> y;

    AssociativeOps() {}
    AssociativeOps(size_t size) : ops(size), x(size), y(size) {}
    AssociativeOps(const std::vector<AssociativePair> &ops,
                   const std::vector<Replacement> &x,
                   const std::vector<Replacement> &y)
        : ops(ops), x(x), y(y) {}

    size_t size() const { return ops.size(); }
};

/**
 * Given an update definition of a Func 'f', determine its equivalent associative
 * binary/unary operator if there is any. The first boolean value of the returned pair
 * indicates if the operation was successfuly proven as associative, and the second
 * vector contains the list of AssociativeOps for each Tuple element in the update
 * definition. If it fails to prove associativity, the second vector will be empty.
 *
 * For instance, f(x) = min(f(x), g(r.x)) will return true and it will also return
 * {{min(_x_0, _y_0), +inf, {_x_0, f(x)}, {_y_0, g(r.x)}}}, where the first Expr
 * is the equivalent binary operator, the second Expr is identity of the binary
 * operator, the third and the last pair contain the corresponding definition of
 * each variable in the binary operator.
 *
 * Note that even though f(x) = f(x) is associative, we'll treat it as non-associative
 * since it doesn't really make any sense to do any associative reduction on that
 * particular update definition.
 */
struct AssociativityProverResult {
    AssociativeOps associative_ops;
    bool is_associative;

    AssociativityProverResult() : is_associative(false) {}
    AssociativityProverResult(bool is_associative, const AssociativeOps &op)
        : associative_ops(op), is_associative(is_associative) {}

    const std::vector<AssociativePair> &ops() const { return associative_ops.ops; }
    const std::vector<AssociativeOps::Replacement> &x() const { return associative_ops.x; }
    const std::vector<AssociativeOps::Replacement> &y() const { return associative_ops.y; }

    std::vector<AssociativePair> &ops() { return associative_ops.ops; }
    std::vector<AssociativeOps::Replacement> &x() { return associative_ops.x; }
    std::vector<AssociativeOps::Replacement> &y() { return associative_ops.y; }

    bool associative() const { return is_associative; }
    size_t size() const { return associative_ops.size(); }
};

AssociativityProverResult prove_associativity(
    const std::string &f, std::vector<Expr> args, std::vector<Expr> exprs);

EXPORT void associativity_test();

}
}

#endif
