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
 * Detect whether an operator is associative.
 */
EXPORT bool is_associative(const std::function<Expr(Expr, Expr)> &op);

EXPORT void associativity_test();

}
}

#endif
