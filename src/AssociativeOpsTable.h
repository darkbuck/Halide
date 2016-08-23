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
namespace AssociativeOpsTable {

extern std::array<std::string, 14487> table_single_i32_ops;
extern std::array<int32_t, 14487> table_single_i32_ids;

}
}
}

#endif
