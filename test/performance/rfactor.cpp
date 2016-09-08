#include "Halide.h"
#include "benchmark.h"
#include <stdio.h>
#include <memory>

using namespace Halide;
using namespace Halide::Internal;

// Controls the size of the input data
#define N1 4
#define N2 4

// TODO(psuriana): implement the rfactor test
int matrix_multiply() {
    const int matrix_size = 992, block_size = 32;

    ImageParam A(type_of<float>(), 2);
    ImageParam B(type_of<float>(), 2);

    Var x("x"), xi("xi"), xo("xo"), y("y"), yo("yo"), yi("yo"), yii("yii"), xii("xii");
    Func matrix_mul("matrix_mul"), ref("ref");

    RDom k(0, matrix_size);
    RVar ki;

    ref(x, y) = 0.0f;
    ref(x, y) += A(k, y) * B(x, k);

    matrix_mul(x, y) = 0.0f;
    matrix_mul(x, y) += A(k, y) * B(x, k);

    matrix_mul.vectorize(x, 8);

    matrix_mul.update(0)
        .split(x, x, xi, block_size).split(xi, xi, xii, 8)
        .split(y, y, yi, block_size).split(yi, yi, yii, 4)
        .split(k, k, ki, block_size)
        .reorder(xii, yii, xi, ki, yi, k, x, y)
        .parallel(y).vectorize(xii).unroll(xi).unroll(yii);

    matrix_mul
        .bound(x, 0, matrix_size)
        .bound(y, 0, matrix_size);

    matrix_mul.compile_jit();

    const int iterations = 50;

    Image<float> mat_A(matrix_size, matrix_size);
    Image<float> mat_B(matrix_size, matrix_size);
    Image<float> ref_output(matrix_size, matrix_size);
    Image<float> output(matrix_size, matrix_size);

    // init randomly
    for (int iy = 0; iy < matrix_size; iy++) {
        for (int ix = 0; ix < matrix_size; ix++) {
            mat_A(ix, iy) = (rand() % 256) / 256.0f;
            mat_B(ix, iy) = (rand() % 256) / 256.0f;
        }
    }

    A.set(mat_A);
    B.set(mat_B);

    double t_ref = benchmark(1, iterations, [&]() {
        ref.realize(ref_output);
    });

    double t = benchmark(1, iterations, [&]() {
        matrix_mul.realize(output);
    });

    float gflops = 2.0f * matrix_size * matrix_size * matrix_size / 1e9;

    printf("Matrix-multipy ref: %fms, %f GFLOP/s\n", t_ref * 1e3, (gflops / t_ref));
    printf("Matrix-multipy: %fms, %f GFLOP/s\n", t * 1e3, (gflops / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

int parallel_dot_product_rfactor_test() {
    const int size = 1024 * 1024 * N1 * N2;

    ImageParam A(Int(8), 1);
    ImageParam B(Int(8), 1);

    Param<int> p;

    RDom r(0, size);

    // Reference implementation
    Func dot_ref("dot_ref");
    dot_ref() = 0;
    dot_ref() += cast<int>(A(r.x))*B(r.x);

    Func dot("dot");
    dot() = 0;
    dot() += cast<int>(A(r.x))*B(r.x);
    RVar rxo, rxi, rxio, rxii;
    dot.update().split(r.x, rxo, rxi, 4*8192);

    Var u, v;
    Func intm = dot.update().rfactor(rxo, u);
    intm.compute_root()
        .update()
        .parallel(u)
        .split(rxi, rxio, rxii, 8)
        .rfactor(rxii, v)
        .compute_at(intm, u)
        .vectorize(v)
        .update()
        .vectorize(v);

    const int trials = 10;
    const int iterations = 10;

    Image<int8_t> vec_A(size), vec_B(size);
    Image<int32_t> ref_output = Image<int32_t>::make_scalar();
    Image<int32_t> output = Image<int32_t>::make_scalar();

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec_A(ix) = rand();
        vec_B(ix) = rand();
    }

    A.set(vec_A);
    B.set(vec_B);

    double t_ref = benchmark(trials, iterations, [&]() {
        dot_ref.realize(ref_output);
    });
    double t = benchmark(trials, iterations, [&]() {
        dot.realize(output);
    });

    // Note that LLVM autovectorizes the reference!

    //dot.compile_to_assembly("/dev/stdout", {A, B}, Target("host-no_asserts-no_runtime-no_bounds_query"));
    //dot_ref.compile_to_assembly("/dev/stdout", {A, B}, Target("host-no_asserts-no_runtime-no_bounds_query"));

    float gbits = 8 * size * (2 / 1e9); // bits per seconds

    printf("Dot-product ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Dot-product with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

int max_test() {
    const int size = 1024 * 1024 * N1 * N2;

    ImageParam A(Int(32), 1);

    RDom r(0, size);

    Func max_ref("max_ref");
    max_ref() = 0;
    max_ref() = max(max_ref(), A(r));

    Func maxf("maxf");
    maxf() = 0;
    RVar rxo, rxi, rxio, rxii;
    maxf() = max(maxf(), A(r));
    maxf.update().split(r.x, rxo, rxi, 4*8192);

    Var u, v;
    Func intm = maxf.update().rfactor(rxo, u);
    intm.compute_root()
        .update()
        .parallel(u)
        .split(rxi, rxio, rxii, 8)
        .rfactor(rxii, v)
        .compute_at(intm, u)
        .vectorize(v)
        .update()
        .vectorize(v);

    const int trials = 10;
    const int iterations = 10;

    Image<int32_t> vec_A(size);
    Image<int32_t> ref_output = Image<int32_t>::make_scalar();
    Image<int32_t> output = Image<int32_t>::make_scalar();

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec_A(ix) = rand();
    }

    A.set(vec_A);

    double t_ref = benchmark(trials, iterations, [&]() {
        max_ref.realize(ref_output);
    });
    double t = benchmark(trials, iterations, [&]() {
        maxf.realize(output);
    });

    // maxf.compile_to_assembly("/dev/stdout", {A}, Target("host-no_asserts-no_runtime-no_bounds_query"));
    // max_ref.compile_to_assembly("/dev/stdout", {A}, Target("host-no_asserts-no_runtime-no_bounds_query"));

    float gbits = 32.0 * size / 1e9; // bits per seconds

    printf("Max ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Max with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}


int kitchen_sink_test() {
    const int size = 1024 * 1024 * N1 * N2;

    ImageParam A(Int(32), 1);

    RDom r(0, size);

    Func sink_ref("sink_ref");
    sink_ref() = {0, 0, int(0x80000000), 0, int(0x7fffffff), 0, 0, 0};
    sink_ref() = {sink_ref()[0] * A(r), // Product
                  sink_ref()[1] + A(r), // Sum
                  max(sink_ref()[2], A(r)), // Max
                  select(sink_ref()[2] > A(r), sink_ref()[3], r), // Argmax
                  min(sink_ref()[4], A(r)), // Min
                  select(sink_ref()[4] < A(r), sink_ref()[5], r), // Argmin
                  sink_ref()[6] + A(r)*A(r), // Sum of squares
                  sink_ref()[7] + select(A(r) % 2 == 0, 1, 0) // Number of even items
    };

    Func sink("sink");
    sink() = {0, 0, int(0x80000000), 0, int(0x7fffffff), 0, 0, 0};
    sink() = {sink()[0] * A(r), // Product
              sink()[1] + A(r), // Sum
              max(sink()[2], A(r)), // Max
              select(sink()[2] > A(r), sink()[3], r), // Argmax
              min(sink()[4], A(r)), // Min
              select(sink()[4] < A(r), sink()[5], r), // Argmin
              sink()[6] + A(r)*A(r), // Sum of squares
              sink()[7] + select(A(r) % 2 == 0, 1, 0) // Number of even items
    };

    RVar rxo, rxi, rxio, rxii;
    sink.update().split(r.x, rxo, rxi, 8192);

    Var u, v;
    Func intm = sink.update().rfactor(rxo, u);
    intm.compute_root()
        .update()
        .parallel(u)
        .split(rxi, rxio, rxii, 8)
        .rfactor(rxii, v)
        .compute_at(intm, u)
        .vectorize(v)
        .update()
        .vectorize(v);

    const int trials = 10;
    const int iterations = 10;

    Image<int32_t> vec_A(size);

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec_A(ix) = rand();
    }

    A.set(vec_A);

    double t_ref = benchmark(trials, iterations, [&]() {
        sink_ref.realize();
    });
    double t = benchmark(trials, iterations, [&]() {
        sink.realize();
    });

    //sink.compile_to_assembly("/dev/stdout", {A}, Target("host-no_asserts-no_runtime-no_bounds_query"));
    //sink_ref.compile_to_assembly("/dev/stdout", {A}, Target("host-no_asserts-no_runtime-no_bounds_query"));

    float gbits = 8 * size * (2 / 1e9); // bits per seconds

    printf("Kitchen sink ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Kitchen sink with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

int bandwidth_test() {
    const int size = 1024 * 1024 * N1 * N2;

    Image<int> A(size);

    RDom r(0, size);

    Func dot("dot");
    dot() = 0;
    dot() += A(r.x);
    RVar rxo, rxi, rxio, rxii;
    dot.update().split(r.x, rxo, rxi, 64*8192);

    Var u, v;
    Func intm = dot.update().rfactor(rxo, u);
    intm.compute_root()
        .update()
        .parallel(u)
        .split(rxi, rxio, rxii, 128)
        .rfactor(rxii, v)
        .compute_at(intm, u)
        .vectorize(v)
        .update()
        .vectorize(v);

    const int trials = 10;
    const int iterations = 10;

    Image<int32_t> output = Image<int32_t>::make_scalar();

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        A(ix) = rand();
    }

    double t = benchmark(trials, iterations, [&]() {
        dot.realize(output);
    });

    // Note that LLVM autovectorizes the reference!

    //dot.compile_to_assembly("/dev/stdout", {A}, Target("host-no_asserts-no_runtime-no_bounds_query"));

    float gbits = 32.0 * size / 1e9; // Gbits

    printf("Bandwidth test: %fms, %f Gbps\n", t * 1e3, (gbits / t));

    return 0;
}

int argmin_rfactor_test() {
    const int size = 64;

    Func amin("amin"), ref("ref");

    ImageParam input(UInt(8), 4);

    RDom r(0, size, 0, size, 0, size, 0, size);

    ref() = Tuple(255, 0, 0, 0, 0);
    ref() = Tuple(min(ref()[0], input(r.x, r.y, r.y, r.z)),
                  select(ref()[0] < input(r.x, r.y, r.y, r.z), ref()[1], r.x),
                  select(ref()[0] < input(r.x, r.y, r.y, r.z), ref()[2], r.y),
                  select(ref()[0] < input(r.x, r.y, r.z, r.w), ref()[3], r.z),
                  select(ref()[0] < input(r.x, r.y, r.z, r.w), ref()[4], r.w));

    amin() = Tuple(255, 0, 0, 0, 0);
    amin() = Tuple(min(amin()[0], input(r.x, r.y, r.z, r.w)),
                   select(amin()[0] < input(r.x, r.y, r.z, r.w), amin()[1], r.x),
                   select(amin()[0] < input(r.x, r.y, r.z, r.w), amin()[2], r.y),
                   select(amin()[0] < input(r.x, r.y, r.z, r.w), amin()[3], r.z),
                   select(amin()[0] < input(r.x, r.y, r.z, r.w), amin()[4], r.w));

    Var u;
    Func intm1 = amin.update(0).rfactor(r.w, u);
    intm1.compute_root();
    intm1.update(0).parallel(u);

    Var v;
    RVar rxo, rxi;
    Func intm2 = intm1.update(0).split(r.x, rxo, rxi, 16).rfactor(rxi, v);
    intm2.compute_at(intm1, u);
    intm2.update(0).vectorize(v);

    const int iterations = 10;
    const int trials = 10;

    Image<uint8_t> vec(size, size, size, size);

    // init randomly
    for (int iw = 0; iw < size; iw++) {
        for (int iz = 0; iz < size; iz++) {
            for (int iy = 0; iy < size; iy++) {
                for (int ix = 0; ix < size; ix++) {
                    vec(ix, iy, iz, iw) = (rand() % size);
                }
            }
        }
    }

    input.set(vec);

    ref.realize();
    amin.realize();

    double t_ref = benchmark(trials, iterations, [&]() {
        ref.realize();
    });
    double t = benchmark(trials, iterations, [&]() {
        amin.realize();
    });

    float gbits = input.type().bits() * vec.number_of_elements() / 1e9; // bits per seconds

    printf("Argmin ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Argmin with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

int complex_multiply_rfactor_test() {
    const int size = 1024*1024*N1 * N2;

    Func mult("mult"), ref("ref");

    // TODO: change to float
    ImageParam input0(Int(32), 1);
    ImageParam input1(Int(32), 1);

    RDom r(0, size);

    ref() = Tuple(1, 0);
    ref() = Tuple(ref()[0]*input0(r.x) - ref()[1]*input1(r.x),
                  ref()[0]*input1(r.x) + ref()[1]*input0(r.x));

    mult() = Tuple(1, 0);
    mult() = Tuple(mult()[0]*input0(r.x) - mult()[1]*input1(r.x),
                   mult()[0]*input1(r.x) + mult()[1]*input0(r.x));

    RVar rxi, rxo, rxii, rxio;
    mult.update(0).split(r.x, rxo, rxi, 2*8192);

    Var u, v;
    Func intm = mult.update().rfactor(rxo, u);
    intm.compute_root()
        .vectorize(u, 8)
        .update()
        .parallel(u)
        .split(rxi, rxio, rxii, 8)
        .rfactor(rxii, v)
        .compute_at(intm, u)
        .vectorize(v)
        .update()
        .vectorize(v);

    const int trials = 10;
    const int iterations = 10;

    Image<int32_t> vec0(size), vec1(size);

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec0(ix) = (rand() % size);
        vec1(ix) = (rand() % size);
    }

    input0.set(vec0);
    input1.set(vec1);

    ref.realize();
    mult.realize();

    //mult.compile_to_assembly("/dev/stdout", {input0, input1}, Target("host-no_asserts-no_bounds_query-no_runtime"));

    double t_ref = benchmark(trials, iterations, [&]() {
        ref.realize();
    });
    double t = benchmark(trials, iterations, [&]() {
        mult.realize();
    });

    float gbits = input0.type().bits() * size * 2 / 1e9; // bits per seconds

    printf("Complex-multiply ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Complex-multiply with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);



    return 0;
}

int histogram_rfactor_test() {
    int W = 1024*N1, H = 1024*N2;

    Image<uint8_t> in(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            in(x, y) = rand();
        }
    }

    Func hist("hist"), ref("ref");
    Var x, y;

    RDom r(0, W, 0, H);

    ref(x) = 0;
    ref(in(r.x, r.y)) += 1;

    hist(x) = 0;
    hist(in(r.x, r.y)) += 1;

    Var u;
    RVar ryo, ryi;
    hist
        .update()
        .split(r.y, ryo, ryi, 16)
        .rfactor(ryo, u)
        .compute_root()
        .vectorize(x, 8)
        .update().parallel(u);
    hist.update().vectorize(x, 8);

    const int trials = 10;
    const int iterations = 10;

    ref.realize(256);
    hist.realize(256);

    Image<int> result(256);
    double t_ref = benchmark(trials, iterations, [&]() {
        ref.realize(result);
    });
    double t = benchmark(trials, iterations, [&]() {
        hist.realize(result);
    });

    double gbits = in.type().bits * W * H / 1e9; // bits per seconds

    printf("Histogram ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Histogram with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

//TODO(psuriana): Has to add table for the cast (not working yet)
int saturating_add_test() {
    const int size = 1024;

    ImageParam input(UInt(8), 1);

    RDom r(0, size);

    // Reference implementation
    // Saturating add: clamped to 255
    // cast<uint8_t>(min(cast<uint16_t>(x) + cast<uint16_t>(y), 255));
    Func add_ref("add_ref");
    add_ref() = make_const(UInt(8), 0);
    add_ref() = cast<uint8_t>(min(cast<uint16_t>(add_ref()) + cast<uint16_t>(input(r.x)), 255));

    // Split into outer dim "rxo" and inner dim "rxi"
    // Parallelize across "rxo"
    // Split the inner dimension "rxi" into "rxii" and "rxio" and vectorize across the inner dimension

    Func add("add");
    add() = make_const(UInt(8), 0);
    add() = cast<uint8_t>(min(cast<uint16_t>(add()) + cast<uint16_t>(input(r.x)), 255));
    RVar rxo("rxo"), rxi("rxi");
    add.update(0).split(r.x, rxo, rxi, 128);

    /*Var u("u");
    Func intm = add.update(0).rfactor(rxo, u);
    RVar rxio("rxio"), rxii("rxii");
    intm.compute_root();
    intm.update(0).parallel(u);
    intm.update(0).split(rxi, rxio, rxii, 8);*/

    const int iterations = 50;

    Image<uint8_t> vec(size);

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec(ix) = (rand() % size);
    }

    input.set(vec);

    add_ref.realize();
    add.realize();

    double t_ref = benchmark(1, iterations, [&]() {
        add_ref.realize();
    });
    double t = benchmark(1, iterations, [&]() {
        add.realize();
    });

    float gbits = input.type().bits() * size / 1e9; // bits per seconds

    printf("Saturating addition ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Saturating addition with rfactor: %fms, %f Gbps\n", t * 1e3, (gbits / t));
    double improve = t_ref / t;
    printf("Improvement: %f\n\n", improve);

    return 0;
}

void memcpy_test() {
    int size = N1 * N2 * 1024 * 1024;
    Image<int> a(size), b(size);
    double t = benchmark(10, 10, [&]() {
            memcpy(a.data(), b.data(), size * sizeof(int));
        });
    printf("Serial Bandwidth: %f\n\n", (32.0 * size / 1e9) / t);

    Func f;
    Var x;
    f(x) = a(x);

    Var xo, xi;
    f.split(x, xo, xi, 8192*8).vectorize(xi, 8).parallel(xo);
    t = benchmark(10, 10, [&]() {
            f.realize(b);
        });
    printf("Parallel in->out Bandwidth: %f\n\n", (32.0 * size / 1e9) / t);

    Func g, h;
    RDom r(0, size/512);
    Var y;
    g(x, y) += a(y*(size/64) + r*8 + x);
    h(x, y) = g(x, y);
    h.parallel(y);
    g.compute_at(h, y)
        .bound(x, 0, 8)
        .update()
        .vectorize(x);
    t = benchmark(10, 10, [&]() {
            h.realize(8, 64);
        });
    printf("Parallel in only Bandwidth: %f\n\n", (32.0 * size / 1e9) / t);
}

int main(int argc, char **argv) {
    bandwidth_test();
    //memcpy_test();
    histogram_rfactor_test();
    argmin_rfactor_test();
    complex_multiply_rfactor_test();
    max_test();
    //saturating_add_test();
    parallel_dot_product_rfactor_test();
    kitchen_sink_test();

    printf("Success!\n");
    return 0;
}
