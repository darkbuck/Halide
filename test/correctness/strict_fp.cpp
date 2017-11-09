#include <iostream>
#include <iomanip>

#include "Halide.h"

using namespace Halide;

Buffer<float> one_million_rando_floats() {
    Var x;
    Func randos;
    randos(x) = random_float();
    return randos.realize(1e6);
}

ImageParam in(Float(32), 1);

enum FPStrictness {
  FPDefault,
  FPNoSimplify,
  FPStrict
} global_strictness = FPDefault;

std::string strictness_to_string(FPStrictness strictness) {
    if (strictness == FPNoSimplify) {
        return "no_fp_simplify";
    } else if (strictness == FPStrict) {
        return "strict_fp";
    }
    return "default";
}

Expr apply_strictness(Expr x) {
    if (global_strictness == FPNoSimplify) {
        return no_fp_simplify(x);
    } else if (global_strictness == FPStrict) {
        return strict_fp(x);
    }
    return x;
}

template <typename Accum>
Func simple_sum() {
    RDom r(in);
    // Do a bit more math on the input
    Expr v = in(r) * in(r);
    return lambda(apply_strictness(cast<float>(sum(cast<Accum>(v)))));
}

Func kahan_sum() {
    RDom r(in);
    Func k_sum;

    // Do a bit more math on the input
    Expr v = in(r) * in(r);
    // Item 0 of the tuple is the sum and item 1 is an error compensation
    // See: https://en.wikipedia.org/wiki/Kahan_summation_algorithm
    k_sum() = Tuple(0.0f, 0.0f);
    k_sum() = Tuple(apply_strictness(k_sum()[0] + (v - k_sum()[1])),
                    apply_strictness((k_sum()[0] + (v - k_sum()[1])) - k_sum()[0]) - (v - k_sum()[1]));

    return lambda(k_sum()[0]);
}

float eval(Func f, const Target &t, const std::string &name, const std::string &suffix) {
      f.compile_to_llvm_assembly("/tmp/" + name + suffix + ".ll", {in}, name, t);
      f.compile_to_assembly("/tmp/" + name + suffix + ".s", {in}, name, t);
      float val = ((Buffer<float>)f.realize(t))();
      std::cout << "    " << name << ": " << val << "\n";
      return val;
}

void run_one_condition(const Target &t, FPStrictness strictness) {
    global_strictness = strictness;
    std::string suffix = "_" + t.to_string() + "_" + strictness_to_string(strictness);

    std::cout << "Target: " << t.to_string() << " Strictness: " << strictness_to_string(strictness) << "\n";

    float simple_float = eval(simple_sum<float>(), t, "simple_float", suffix);
    float simple_double = eval(simple_sum<double>(), t, "simple_double", suffix);
    float kahan = eval(kahan_sum(), t, "kahan", suffix);
    assert(fabs(simple_double - kahan) <= fabs(simple_double - simple_float));
}

int main(int argc, char **argv) {
    std::cout << std::setprecision(10);
    Buffer<float> vals = one_million_rando_floats();
    in.set(vals);

    Target loose{get_jit_target_from_environment().without_feature(Target::StrictFP)};
    Target strict{loose.with_feature(Target::StrictFP)};
  
    run_one_condition(loose, FPDefault);
    run_one_condition(strict, FPDefault);
    run_one_condition(loose, FPNoSimplify);
    run_one_condition(strict, FPNoSimplify);
    run_one_condition(loose, FPStrict);
    run_one_condition(strict, FPStrict);

    return 0;
}
