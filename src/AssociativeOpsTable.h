#ifndef HALIDE_ASSOCIATIVE_OPS_TABLE_H
#define HALIDE_ASSOCIATIVE_OPS_TABLE_H

/** \file
 * Tables listing associative operators and their identities.
 */

#include "IROperator.h"

#include <iostream>
#include <array>

namespace Halide {
namespace Internal {

struct AssociativePair {
	Expr op;
	Expr identity;

	AssociativePair() {}
	AssociativePair(Expr op) : op(op) {}
	AssociativePair(Expr op, Expr id) : op(op), identity(id) {}
};

// Single element tuple of 32-bit integer
const std::vector<AssociativePair> &get_single_i32_ops_table_add();
const std::vector<AssociativePair> &get_single_i32_ops_table_mul();
const std::vector<AssociativePair> &get_single_i32_ops_table_max();
const std::vector<AssociativePair> &get_single_i32_ops_table_min();
const std::vector<AssociativePair> &get_single_i32_ops_table_sub();

// Two element tuple of 32-bit integer
/*const std::vector<AssociativePair> &get_double_i32_ops_table_add();
const std::vector<AssociativePair> &get_double_i32_ops_table_mul();
const std::vector<AssociativePair> &get_double_i32_ops_table_max();
const std::vector<AssociativePair> &get_double_i32_ops_table_min();
const std::vector<AssociativePair> &get_double_i32_ops_table_sub();*/

}
}

#endif
