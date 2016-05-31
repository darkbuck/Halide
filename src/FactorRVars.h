#ifndef HALIDE_FACTOR_RVARS_H
#define HALIDE_FACTOR_RVARS_H

/** \file
 *
 * Defines pass to replace args/values/rdoms in the Functions' definitions with their rfactor.
 */

#include <map>

#include "IR.h"

namespace Halide {
namespace Internal {

/** Replace args/values/rdoms in the Functions' definitions with their rfactor. */
void factor_rvars(std::map<std::string, Function> &env);

}
}

#endif
