// USAGE: halide_benchmarks <subroutine> <size>
//
// Benchmarks BLAS subroutines using Halide's implementation. Will
// construct random size x size matrices and/or size x 1 vectors
// to test the subroutine with.
//
// Accepted values for subroutine are:
//    L3: gemm_notrans, gemm_trans_A, gemm_trans_B, gemm_trans_AB
//

#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include "Halide.h"
#include "halide_blas.h"
#include "clock.h"
#include "macros.h"


struct Benchmarks {
    typedef uint8_t Scalar;
    typedef Halide::Buffer Vector;
    typedef Halide::Buffer Matrix;

    std::random_device rand_dev;
    std::default_random_engine rand_eng{rand_dev()};

    std::string name;

    Halide::Buffer result;

    Benchmarks(std::string n) : name(n), result(Halide::UInt(8), 1) {}

    Scalar random_scalar() {
        std::uniform_int_distribution<uint8_t> uniform_dist(1, 10);
        return uniform_dist(rand_eng);
    }

    Vector random_vector(int N) {
        Vector buff(Halide::type_of<uint8_t>(), N);
        Scalar *x = (Scalar*)buff.host_ptr();
        for (int i=0; i<N; ++i) {
            x[i] = random_scalar();
        }
        return buff;
    }

    Matrix random_matrix(int N) {
        Matrix buff(Halide::type_of<uint8_t>(), N, N);
        Scalar *A = (Scalar*)buff.host_ptr();
        for (int i=0; i<N*N; ++i) {
            A[i] = random_scalar();
        }
        return buff;
    }

    void run(std::string benchmark, int size) {
        if (benchmark == "gemm_notrans") {
            bench_gemm_notrans(size);
        } else if (benchmark == "gemm_transA") {
            bench_gemm_transA(size);
        } else if (benchmark == "gemm_transB") {
            bench_gemm_transB(size);
        } else if (benchmark == "gemm_transAB") {
            bench_gemm_transAB(size);
        }
    }

    L3Benchmark(gemm_notrans, "i", halide_igemm(false, false, false, A.raw_buffer(), a_offset,
                                                B.raw_buffer(), b_offset, C.raw_buffer(),
                                                c_offset, c_mult_int, c_shift))

    L3Benchmark(gemm_transA, "i", halide_igemm(true, false, false, A.raw_buffer(), a_offset,
                                                B.raw_buffer(), b_offset, C.raw_buffer(),
                                                c_offset, c_mult_int, c_shift))

    L3Benchmark(gemm_transB, "i", halide_igemm(false, true, false, A.raw_buffer(), a_offset,
                                                B.raw_buffer(), b_offset, C.raw_buffer(),
                                                c_offset, c_mult_int, c_shift))

    L3Benchmark(gemm_transAB, "i", halide_igemm(true, true, false, A.raw_buffer(), a_offset,
                                                B.raw_buffer(), b_offset, C.raw_buffer(),
                                                c_offset, c_mult_int, c_shift))
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "USAGE: halide_benchmarks <subroutine> <size>\n";
        return 0;
    }

    std::string subroutine = argv[1];
    char type = subroutine[0];
    int  size = std::stoi(argv[2]);

    subroutine = subroutine.substr(1);
    if (type == 'i') {
        Benchmarks("Halide").run(subroutine, size);
    }

    return 0;
}
