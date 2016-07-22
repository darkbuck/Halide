#include "Halide.h"
#include <stdio.h>

using namespace Halide;

int main(int argc, char **argv) {
    Func f;
    Var x;

    // Don't bother with a pure definition. Because this will be the
    // output stage, that means leave whatever's already in the output
    // buffer untouched.
    f(x) = undef<float>();

    // But do a sum-scan of it from 0 to 100
    RDom r(1, 99);
    f(r) += f(r-1);

    // Make some test data.
    Image<float> data = lambda(x, sin(x)).realize(100);

    f.realize(data);

    // Do the same thing not in-place
    Image<float> reference_in = lambda(x, sin(x)).realize(100);
    Func g;
    g(x) = reference_in(x);
    g(r) += g(r-1);
    Image<float> reference_out = g.realize(100);

    float err = evaluate_may_gpu<float>(sum(abs(data(r) - reference_out(r))));

    if (err > 0.0001f) {
        printf("Failed\n");
        return -1;
    }

    printf("Success!\n");
    return 0;
}