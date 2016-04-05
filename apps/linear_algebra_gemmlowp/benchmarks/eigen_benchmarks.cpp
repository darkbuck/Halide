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
#include "clock.h"
#include "macros.h"

struct Benchmarks {
    typedef uint8_t Scalar;
    typedef Eigen::Matrix<uint8_t, Eigen::Dynamic, 1> Vector;
    typedef Eigen::Matrix<uint8_t, Eigen::Dynamic, Eigen::Dynamic> Matrix;
    typedef Eigen::Matrix<int32_t, Eigen::Dynamic, Eigen::Dynamic> Matrix32i;

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

    inline void gemm(bool transpose_a, bool transpose_b, uint8_t alpha, uint8_t beta,
                     const Matrix& A, const Matrix& B, Matrix& C) {
        Matrix32i A_int = A.cast<int32_t>();
        Matrix32i B_int = B.cast<int32_t>();
        Matrix32i C_int = C.cast<int32_t>();
        int32_t alpha_int = (int32_t) alpha;
        int32_t beta_int = (int32_t) beta;
        if (transpose_a) {
            A_int = A_int.transpose();
        }
        if (transpose_b) {
            B_int = B_int.transpose();
        }
        C_int = alpha_int * A_int * B_int + beta_int * C_int;
        C = C_int.cast<uint8_t>();
    }

    L3Benchmark(gemm_notrans, "i", gemm(false, false, alpha, beta, A, B, C));
    L3Benchmark(gemm_transA, "i", gemm(true, false, alpha, beta, A, B, C));
    L3Benchmark(gemm_transB, "i", gemm(false, true, alpha, beta, A, B, C));
    L3Benchmark(gemm_transAB, "i", gemm(true, true, alpha, beta, A, B, C));

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
