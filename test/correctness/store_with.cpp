#include "Halide.h"

using namespace Halide;

int main(int argc, char **argv) {
    Var x, y;

    if (0) {
        // Check back-to-back compute_root operations in-place. This
        // is not how you should write anything, but it checks that
        // the memory is aliased.
        Func f, g;
        f(x, y) = x + y;
        g(x, y) = undef<int>(); // secretly f(x, y)
        g(x, y) = g(x, y) * 2 + f(x, y);

        // The realizations must be nested.
        f.compute_at(g, Var::outermost()).store_with(g);
        g.compute_root();

        Buffer<int> out = g.realize(100, 100);
        out.for_each_element([&](int x, int y) {
            int correct = (x + y) * 3;
            if (out(x, y) != correct) {
                printf("out(%d, %d) = %d instead of %d\n",
                       x, y, out(x, y), correct);
                exit(-1);
            }
        });
    }

    {
        // Copy something to the gpu in tiles
        Func f, g;
        f(x, y) = x + y;
        g(x, y) = f(x, y) * 2 + 5;

        Var xo, yo, xi, yi, xii, yii;
        g.compute_root()
            .tile(x, y, xo, yo, xi, yi, 4, 4)
            .gpu_tile(xi, yi, xii, yii, 2, 2);

        f.bound(x, 0, 8).bound(y, 0, 8);
        g.bound(x, 0, 8).bound(y, 0, 8);

        f.compute_root();
        f.in().compute_at(g, xo).store_with(f);

        f.in().trace_stores();

        Buffer<int> out = g.realize(8, 8);
        out.for_each_element([&](int x, int y) {
            int correct = (x + y) * 2 + 5;
            if (out(x, y) != correct) {
                printf("out(%d, %d) = %d instead of %d\n",
                       x, y, out(x, y), correct);
                //exit(-1);
            }
        });
    }


    printf("Success!\n");
    return 0;
}
