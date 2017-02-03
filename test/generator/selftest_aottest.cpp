#include "HalideRuntime.h"
#include "HalideBuffer.h"

#include <stdio.h>
#include <tuple>
 
#include "selftest.h"
#include "selftest_impl.h"

using namespace Halide;
using namespace Halide::Internal;

int main(int argc, char **argv) {
  Testable_AOT aot(selftest_argv, selftest_metadata);
  selftest_test(aot);

  printf("Success!\n");
  return 0;
}
