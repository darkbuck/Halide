#include "Halide.h"

namespace {

class Selftest : public Halide::Generator<Selftest> {
public:
    Input<Buffer<uint8_t>> input{"input", 2};
    Input<uint8_t> value{"value"};
    Output<Buffer<uint8_t>> output_xor{"output_xor", 2};
    Output<Buffer<uint8_t>> output_add{"output_add", 2};

    void generate() {
        output_xor(x, y) = input(x, y) ^ value;
        output_add(x, y) = input(x, y) + value;
    }

    void schedule() {
        const int v = natural_vector_size<uint8_t>();
        Func(output_xor)
            .parallel(y)
            .specialize(output_xor.dim(0).extent() >= v)
            .vectorize(x, v);
        Func(output_add)
            .parallel(y)
            .specialize(output_add.dim(0).extent() >= v)
            .vectorize(x, v);
     }

private:
    Var x, y;
};

HALIDE_REGISTER_GENERATOR(Selftest, "selftest")

}  // namespace
