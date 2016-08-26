#ifndef HALIDE_ASSOCIATIVE_OPS_TABLE_H
#define HALIDE_ASSOCIATIVE_OPS_TABLE_H

/** \file
 * Tables listing associative operators and their identities.
 */

#include "IROperator.h"
#include "IREquality.h"

#include <iostream>
#include <vector>

namespace Halide {
namespace Internal {

// Pair of associative op and its identity
struct AssociativePair {
	Expr op;
	Expr identity;

	AssociativePair() {}
	AssociativePair(Expr op) : op(op) {}
	AssociativePair(Expr op, Expr id) : op(op), identity(id) {}

	bool operator==(const AssociativePair &other) const {
      	return equal(op, other.op) && equal(identity, other.identity);
    }
    bool operator!=(const AssociativePair &other) const {
        return !(*this == other);
    }
};

const std::vector<std::vector<AssociativePair>> &get_i32_ops_table(const std::vector<Expr> &exprs);

}
}

#endif
