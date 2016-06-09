#ifndef HALIDE_ASSOCIATIVITY_H
#define HALIDE_ASSOCIATIVITY_H

/** \file
 *
 * Methods for checking whether an operator is associative and computing the
 * identity of an associative operator.
 */

#include "IR.h"

#include <functional>

namespace Halide {
namespace Internal {

/**
 * Given a binary expression operator 'bin_op' in the form of op(x, y), prove that
 * 'bin_op' is associative, i.e. prove that (x op y) op z == x op (y op z)
 */
EXPORT bool is_bin_op_associative(Expr bin_op, const std::string &op_x, const std::string &op_y);

EXPORT void associativity_test();

}
}

#endif
