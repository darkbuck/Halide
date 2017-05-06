#include "Halide.h"

namespace {

class AsyncMemcpyGenerator : public Halide::Generator<AsyncMemcpyGenerator> {
public:
    Input<Buffer<uint8_t>> input{ "input", 1 };

    Output<Buffer<uint8_t>> output{ "output", 1 };

    void generate() {
        in_f(x) = input(x);
        async_memcpy.define_extern("async_memcpy_extern", { in_f }, UInt(8), 1, NameMangling::C, Halide::DeviceAPI::ThreadAsync);

	output(x) = async_memcpy(x);
    }

    void schedule() {
        Var xi, xo;
	output.split(x, xo, xi, 1024);
	async_memcpy.compute_at(output, xo);
	in_f.compute_at(output, xo);
    }

private:
    Func in_f{"in_f"};
    Func async_memcpy{"async_memcpy"};
    Var x{"x"};
};

HALIDE_REGISTER_GENERATOR(AsyncMemcpyGenerator, "async_memcpy")

}  // namespace
