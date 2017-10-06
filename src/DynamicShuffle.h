#ifndef HALIDE_DYNAMIC_SHUFFLE_H
#define HALIDE_DYNAMIC_SHUFFLE_H

/** \file
 * Lowering pass that finds LUT lookups and replaces them with
 * dynamic_shuffle intrinsics.
 */

#include "IR.h"

namespace Halide {
namespace Internal {

/** Replace indirect and other loads with a dynamic_shuffle of a
    dense vector load. */
EXPORT Stmt find_dynamic_shuffles(Stmt s, Type index_type = UInt(8),
                                  int max_size = 256, int lut_alignment = 1);

}  // namespace Internal
}  // namespace Halide

#endif
