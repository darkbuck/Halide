#include "Halide.h"

#include <stdio.h>
 
#include "selftest.stub.h"
#include "selftest_impl.h"

using namespace Halide;
using namespace Halide::Internal;


int main(int argc, char **argv) {
  JITGeneratorContext context(get_jit_target_from_environment());
  Testable_JIT jit("selftest", context);
  selftest_test(jit);

  printf("Success!\n");
  return 0;
}
 