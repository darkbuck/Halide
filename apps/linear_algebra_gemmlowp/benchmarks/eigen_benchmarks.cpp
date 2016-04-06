// USAGE: eigen_benchmarks <subroutine> <size>
//
// Benchmarks BLAS subroutines using Eigen's implementation. Will
// construct random size x size matrices and/or size x 1 vectors
// to test the subroutine with.
//
// Accepted values for subroutine are:
//    L3: gemm_notrans, gemm_trans_A, gemm_trans_B, gemm_trans_AB
//

#include <iomanip>
#include <iostream>
#include <string>
#include <Eigen/Eigen>
#include "../src/eigen_interface.h"
#include "clock.h"
#include "macros.h"

struct Benchmarks {
    typedef uint8_t Scalar;
    typedef eigen::EigenVector Vector;
    typedef eigen::EigenMatrix Matrix;

    Benchmarks(std::string n) : name(n) {}

    Scalar random_scalar() {
        Vector x(1);
        x.setRandom();
        return x[0];
    }

    Vector random_vector(int N) {
        Vector x(N);
        x.setRandom();
        return x;
    }

    Matrix random_matrix(int N) {
        Matrix A(N, N);
        A.setRandom();
        return A;
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

    L3Benchmark(gemm_notrans, "i", eigen::eigen_igemm(false, false, false, A, a_offset, B,
                                                      b_offset, C, c_offset, c_mult_int, c_shift));
    L3Benchmark(gemm_transA, "i", eigen::eigen_igemm(true, false, false, A, a_offset, B,
                                                      b_offset, C, c_offset, c_mult_int, c_shift));
    L3Benchmark(gemm_transB, "i", eigen::eigen_igemm(false, true, false, A, a_offset, B,
                                                      b_offset, C, c_offset, c_mult_int, c_shift));
    L3Benchmark(gemm_transAB, "i", eigen::eigen_igemm(true, true, false, A, a_offset, B,
                                                      b_offset, C, c_offset, c_mult_int, c_shift));

  private:
    std::string name;
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "USAGE: eigen_benchmarks <subroutine> <size>\n";
        return 0;
    }

    std::string subroutine = argv[1];
    char type = subroutine[0];
    int  size = std::stoi(argv[2]);

    subroutine = subroutine.substr(1);
    if (type == 'i') {
        Benchmarks("Eigen").run(subroutine, size);
    }

    return 0;
}
