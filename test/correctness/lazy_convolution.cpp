#include "Halide.h"
#include <stdio.h>

using namespace Halide;

#ifdef _MSC_VER
#define DLLEXPORT __declspec(dllexport)
#else
#define DLLEXPORT
#endif

int call_count;
extern "C" DLLEXPORT float call_counter(float x) {
    call_count++;
    return x;
}

HalideExtern_1(float, call_counter, float);

int main(int argc, char **argv) {

    Func f;
    Var x, y;
    f(x, y) = call_counter(sin(x*3 + y));

    // f contains values in [-1, 1]. Now compute a convolution over f
    // only where f is positive. If f is negative, we'll skip the work
    // and write a zero instead.
    Func blur;
    RDom r(-5, 10, -5, 10);
    blur(x, y) = select(f(x, y) > 0, sum(f(x+r.x, y+r.y)), 0);

    call_count = 0;
    blur.realize(100, 100);

    // If we computed the convolution everywhere, call_count would be
    // 100*100*10*10. Because we only compute it in half of the
    // places, it should be smaller; roughly 100*100*10*10*0.5.
    if (call_count > 510000) {
        printf("Expected call_count ~= 500000. Instead it's %d\n", call_count);
        return -1;
    }

    printf("Success!\n");
    return 0;
}
