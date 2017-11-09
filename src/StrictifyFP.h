#ifndef HALIDE_STRICTIFY_FP_H
#define HALIDE_STRICTIFY_FP_H

/** \file
 * Defines a lowering pass to make all floating-point strict for all top-level Exprs.
 */

#include <map>

#include "Function.h"
#include "Target.h"

namespace Halide {
namespace Internal {

/** Take a function and wrap all top-level Exprs with strict_fp() is
 * Target::StrictFP is specified in target.
 */
void strictify_fp(std::map<std::string, Function> &env, const Target &t);

}
}

#endif
