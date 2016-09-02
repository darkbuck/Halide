#include "Halide.h"
#include "benchmark.h"
#include <stdio.h>
#include <memory>

using namespace Halide;

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

    float gflops = 2.0f * matrix_size * matrix_size * matrix_size / 1e6;

    printf("Matrix-multipy ref: %fms, %f GFLOP/s\n\n", t_ref * 1e3, (gflops / t_ref));
    printf("Matrix-multipy: %fms, %f GFLOP/s\n\n", t * 1e3, (gflops / t));

    printf("Success!\n");
    return 0;
}

int parallel_dot_product_rfactor_test() {
    const int size = 1024;

    Func f("f"), g("g");
    Var x("x");

    ImageParam A(Int(32), 1);
    ImageParam B(Int(32), 1);

    RDom r(0, size);

    // Reference implementation
    Func dot_ref("dot_ref");
    dot_ref() = 0;
    dot_ref() += A(r.x)*B(r.x);

    // Split into outer dim "rxo" and inner dim "rxi"
    // Parallelize across "rxo"
    // Split the inner dimension "rxi" into "rxii" and "rxio" and vectorize across the inner dimension

    Func dot("dot");
    dot() = 0;
    dot() += A(r.x)*B(r.x);
    RVar rxo("rxo"), rxi("rxi");
    dot.update(0).split(r.x, rxo, rxi, 128);

    Var u("u");
    Func intm1 = dot.update(0).rfactor(rxo, u);
    RVar rxio("rxio"), rxii("rxii");
    intm1.compute_root();
    intm1.update(0).parallel(u);
    intm1.update(0).split(rxi, rxio, rxii, 8);

    Var v("v");
    Func intm2 = intm1.update(0).rfactor(rxii, v);
    intm2.compute_at(intm1, u);
    intm2.update(0).vectorize(v, 8);

    const int iterations = 50;

    Image<int32_t> vec_A(size);
    Image<int32_t> vec_B(size);
    Image<int32_t> ref_output(0);
    Image<int32_t> output(0);

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec_A(ix) = (rand() % size);
        vec_B(ix) = (rand() % size);
    }

    A.set(vec_A);
    B.set(vec_B);

    double t_ref = benchmark(1, iterations, [&]() {
        dot_ref.realize(ref_output);
    });
    double t = benchmark(1, iterations, [&]() {
        dot.realize(output);
    });

    float gbits = 32 * size / 1e6; // bits per seconds

    printf("Dot-product ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Dot-product with rfactor: %fms, %f Gbps\n\n", t * 1e3, (gbits / t));

    return 0;
}

int argmin_rfactor_test() {
    const int size = 1024;

    Func g("g"), ref("ref");

    ImageParam input(Int(32), 2);

    RDom r(0, size, 0, size);

    ref() = Tuple(10, 20, 30);
    ref() = Tuple(min(ref()[0], input(r.x, r.y)),
                  select(ref()[0] < input(r.x, r.y), ref()[1], r.x),
                  select(ref()[0] < input(r.x, r.y), ref()[2], r.y));

    g() = Tuple(10, 20, 30);
    g() = Tuple(min(g()[0], input(r.x, r.y)),
                select(g()[0] < input(r.x, r.y), g()[1], r.x),
                select(g()[0] < input(r.x, r.y), g()[2], r.y));

    RVar rxi("rxi"), rxo("rxo");
    g.update(0).split(r.x, rxo, rxi, 128);

    Var u("u");
    Func intm1 = g.update(0).rfactor(rxo, u);
    RVar rxio("rxio"), rxii("rxii");
    intm1.compute_root();
    intm1.update(0).parallel(u);
    intm1.update(0).split(rxi, rxio, rxii, 8);

    Var v("v");
    Func intm2 = intm1.update(0).rfactor(rxii, v);
    intm2.compute_at(intm1, u);
    intm2.update(0).vectorize(v, 8);

    const int iterations = 50;

    Image<int32_t> vec(size, size);
    Image<int32_t> ref_output_0(0), ref_output_1(0), ref_output_2(0);
    Image<int32_t> output_0(0), output_1(0), output_2(0);

    // init randomly
    for (int iy = 0; iy < size; iy++) {
        for (int ix = 0; ix < size; ix++) {
            vec(ix, iy) = (rand() % size);
        }
    }

    input.set(vec);

    double t_ref = benchmark(1, iterations, [&]() {
        ref.realize(Realization(ref_output_0, ref_output_1, ref_output_2));
    });
    double t = benchmark(1, iterations, [&]() {
        g.realize(Realization(output_0, output_1, output_2));
    });

    float gbits = 32 * size * size/ 1e6; // bits per seconds

    printf("Argmin ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Argmin with rfactor: %fms, %f Gbps\n\n", t * 1e3, (gbits / t));
    return 0;
}

int complex_multiply_rfactor_test() {
    const int size = 1024;

    Func g("g"), ref("ref");

    ImageParam input0(Int(32), 1);
    ImageParam input1(Int(32), 1);

    RDom r(0, size);

    ref() = Tuple(10, 20);
    ref() = Tuple(ref()[0]*input0(r.x) - ref()[1]*input1(r.x),
                  ref()[0]*input1(r.x) + ref()[1]*input0(r.x));

    g() = Tuple(10, 20);
    g() = Tuple(g()[0]*input0(r.x) - g()[1]*input1(r.x),
                g()[0]*input1(r.x) + g()[1]*input0(r.x));

    RVar rxi("rxi"), rxo("rxo");
    g.update(0).split(r.x, rxo, rxi, 128);

    Var u("u");
    Func intm1 = g.update(0).rfactor(rxo, u);
    RVar rxio("rxio"), rxii("rxii");
    intm1.compute_root();
    intm1.update(0).parallel(u);

    const int iterations = 50;

    Image<int32_t> vec0(size), vec1(size);
    Image<int32_t> ref_output_0(0), ref_output_1(0);
    Image<int32_t> output_0(0), output_1(0);

    // init randomly
    for (int ix = 0; ix < size; ix++) {
        vec0(ix) = (rand() % size);
        vec1(ix) = (rand() % size);
    }

    input0.set(vec0);
    input1.set(vec1);

    double t_ref = benchmark(1, iterations, [&]() {
        ref.realize(Realization(ref_output_0, ref_output_1));
    });
    double t = benchmark(1, iterations, [&]() {
        g.realize(Realization(output_0, output_1));
    });

    float gbits = 32 * size / 1e6; // bits per seconds

    printf("Complex-multiply ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Complex-multiply with rfactor: %fms, %f Gbps\n\n", t * 1e3, (gbits / t));
    return 0;

    return 0;
}

int histogram_rfactor_test() {
    int W = 1024, H = 1024;

    // Compute a random image and its true histogram
    Image<float> in(W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            in(x, y) = float(rand() & 0x000000ff);
        }
    }

    Func hist("hist"), ref("ref");
    Var x("x");

    RDom r(in);

    ref(x) = 0;
    ref(clamp(cast<int>(in(r.x, r.y)), 0, 255)) += 1;

    hist(x) = 0;
    hist(clamp(cast<int>(in(r.x, r.y)), 0, 255)) += 1;

    Var u("u");
    Func intm = hist.update(0).rfactor(r.y, u);
    intm.compute_root();
    intm.update(0).parallel(u);

    const int iterations = 50;

    Image<int32_t> ref_output(256);
    Image<int32_t> output(256);

    double t_ref = benchmark(1, iterations, [&]() {
        ref.realize(ref_output);
    });
    double t = benchmark(1, iterations, [&]() {
        hist.realize(output);
    });

    float gbits = 32 * W * H / 1e6; // bits per seconds

    printf("Histogram ref: %fms, %f Gbps\n", t_ref * 1e3, (gbits / t_ref));
    printf("Histogram with rfactor: %fms, %f Gbps\n\n", t * 1e3, (gbits / t));

    return 0;
}

int main(int argc, char **argv) {
    //parallel_dot_product_rfactor_test();
    //argmin_rfactor_test();
    //complex_multiply_rfactor_test();
    //histogram_rfactor_test();
    matrix_multiply();

    printf("Success!\n");
    return 0;
}
