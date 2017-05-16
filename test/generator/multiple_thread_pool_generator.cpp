#include "Halide.h"

using namespace Halide;

HalideExtern_1(int32_t, check_thread, void *);

struct MultipleThreadPool : Halide::Generator<MultipleThreadPool> {
    ImageParam in{Int(32), 2, "in"};
    Param<int32_t> bias{"bias"};

    Func build() {
        Var x, y;
        Func f;
        f(x, y) = in(x, y) + check_thread(user_context_value()) + bias;
        f.parallel(y);
        return f;
    }
};

RegisterGenerator<MultipleThreadPool> register_my_gen{"multiple_thread_pool"};
