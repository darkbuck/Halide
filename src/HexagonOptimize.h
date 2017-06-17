#ifndef HALIDE_IR_HEXAGON_OPTIMIZE_H
#define HALIDE_IR_HEXAGON_OPTIMIZE_H

/** \file
 * Tools for optimizing IR for Hexagon.
 */

#include "IR.h"

namespace Halide {
namespace Internal {

/** Replace indirect and other loads with simple loads + vlut
 * calls. */
EXPORT Stmt optimize_hexagon_shuffles(Stmt s, int lut_alignment);

/** Generate vtmpy instruction if possible */
EXPORT Stmt vtmpy_generator(Stmt s);

/** Hexagon deinterleaves when performing widening operations, and
 * interleaves when performing narrowing operations. This pass
 * rewrites widenings/narrowings to be explicit in the IR, and
 * attempts to simplify away most of the
 * interleaving/deinterleaving. */
EXPORT Stmt optimize_hexagon_instructions(Stmt s, Target t);

/* Simplify shuffles (slice_vector & concat_vectors) out and
 * upswards in an expression tree. This is done in the hope
 * of finding CSEable widening multiply add operations. However,
 * sometimes this leads to some undesirable code forms.
 * This pass fixes up such IR. */
EXPORT Stmt fixup_hoist_shuffles(Stmt s);

/** Generate deinterleave or interleave operations, operating on
 * groups of vectors at a time. */
//@{
EXPORT Expr native_deinterleave(Expr x);
EXPORT Expr native_interleave(Expr x);
EXPORT bool is_native_deinterleave(Expr x);
EXPORT bool is_native_interleave(Expr x);
//@}

}  // namespace Internal
}  // namespace Halide

#endif
