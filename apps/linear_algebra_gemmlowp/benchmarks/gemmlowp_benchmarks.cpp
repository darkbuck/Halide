// USAGE: eigen_benchmarks <subroutine> <size>
//
// Benchmarks BLAS subroutines using gemmlowp's implementation. Will
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
#include <eight_bit_int_gemm/eight_bit_int_gemm.h>
#include "Halide.h"
#include "clock.h"
#include "macros.h"

struct Benchmarks {
    typedef uint8_t Scalar;
    typedef std::vector<uint8_t> Vector;
    typedef std::vector<uint8_t> Matrix;

    std::random_device rand_dev;
    std::default_random_engine rand_eng{rand_dev()};

    std::string name;

    Scalar random_scalar() {
        std::uniform_int_distribution<uint8_t> uniform_dist(1, 10);
        return uniform_dist(rand_eng);
    }

    Vector random_vector(int N) {
        Vector buff(N);
        for (int i=0; i<N; ++i) {
            buff[i] = random_scalar();
        }
        return buff;
    }

    Matrix random_matrix(int N) {
        Matrix buff(N * N);
        for (int i=0; i<N*N; ++i) {
            buff[i] = random_scalar();
        }
        return buff;
    }

    Benchmarks(std::string n) : name(n) {}

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

    Scalar result;

    L3Benchmark(gemm_notrans, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            false, false, false, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transA, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            true, false, false, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transB, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            false, true, false, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transAB, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            true, true, false, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transC, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            false, false, true, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transAC, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            true, false, true, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transBC, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            false, true, true, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))

    L3Benchmark(gemm_transABC, "i",
        gemmlowp::eight_bit_int_gemm::EightBitIntGemm(
            true, true, true, N, N, N, &(A[0]), a_offset, N,
            &(B[0]), b_offset, N, &(C[0]), c_offset, c_mult_int, c_shift, N,
            gemmlowp::eight_bit_int_gemm::BitDepthSetting::A8B8))
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "USAGE: gemmlowp_benchmarks <subroutine> <size>\n";
        return 0;
    }

    std::string subroutine = argv[1];
    char type = subroutine[0];
    int  size = std::stoi(argv[2]);

    subroutine = subroutine.substr(1);
    if (type == 'i') {
        Benchmarks("gemmlowp").run(subroutine, size);
    }

    return 0;
}
