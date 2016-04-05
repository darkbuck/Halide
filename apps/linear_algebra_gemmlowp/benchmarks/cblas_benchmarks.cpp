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
#include "clock.h"

#if defined(USE_ATLAS)
# define BLAS_NAME "Atlas"
extern "C" {
# include <cblas.h>
}
#elif defined(USE_OPENBLAS)
# define BLAS_NAME "OpenBLAS"
# include <cblas.h>
#else
# define BLAS_NAME "Cblas"
# include <cblas.h>
#endif

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
            this->bench_gemm_notrans(size);
        } else if (benchmark == "gemm_transA") {
            this->bench_gemm_transA(size);
        } else if (benchmark == "gemm_transB") {
            this->bench_gemm_transB(size);
        } else if (benchmark == "gemm_transAB") {
            this->bench_gemm_transAB(size);
        }
    }

    void cblas_igemm(const bool transpose_a, const bool transpose_b,
                     const int M, const int N, const int K, const uint8_t alpha,
                     const uint8_t *A, const int lda, const uint8_t *B, const int ldb,
                     const uint8_t beta, uint8_t *C, const int ldc) {

        std::vector<float> A_float(M*K);
        std::vector<float> B_float(K*N);
        std::vector<float> C_float(M*N);
        for (int i=0; i<M*K; ++i) {
            A_float[i] = static_cast<float>(A[i]);
        }
        for (int i=0; i<K*N; ++i) {
            B_float[i] = static_cast<float>(B[i]);
        }

        if (transpose_a && transpose_b) {
            cblas_sgemm(CblasColMajor, CblasTrans, CblasTrans, M, N, K, (float)alpha,
                        &(A_float[0]), lda, &(B_float[0]), ldb, (float)beta, &(C_float[0]), ldc);
        } else if (transpose_a) {
            cblas_sgemm(CblasColMajor, CblasTrans, CblasNoTrans, M, N, K, (float)alpha,
                        &(A_float[0]), lda, &(B_float[0]), ldb, (float)beta, &(C_float[0]), ldc);
        } else if (transpose_b) {
            cblas_sgemm(CblasColMajor, CblasNoTrans, CblasTrans, M, N, K, (float)alpha,
                        &(A_float[0]), lda, &(B_float[0]), ldb, (float)beta, &(C_float[0]), ldc);
        } else {
            cblas_sgemm(CblasColMajor, CblasNoTrans, CblasNoTrans, M, N, K, (float)alpha,
                        &(A_float[0]), lda, &(B_float[0]), ldb, (float)beta, &(C_float[0]), ldc);
        }

        for (int i=0; i<M*N; ++i) {
            C[i] = static_cast<uint8_t>(C_float[i]);
        }
    }

    L3Benchmark(gemm_notrans, "i", cblas_igemm(false, false, N, N, N,
                                               alpha, &(A[0]), N, &(B[0]), N,
                                               beta, &(C[0]), N))

    L3Benchmark(gemm_transA, "i", cblas_igemm(true, false, N, N, N,
                                              alpha, &(A[0]), N, &(B[0]), N,
                                              beta, &(C[0]), N))

    L3Benchmark(gemm_transB, "i", cblas_igemm(false, true, N, N, N,
                                              alpha, &(A[0]), N, &(B[0]), N,
                                              beta, &(C[0]), N))

    L3Benchmark(gemm_transAB, "i", cblas_igemm(true, true, N, N, N,
                                               alpha, &(A[0]), N, &(B[0]), N,
                                               beta, &(C[0]), N))
};

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cout << "USAGE: cblas_benchmarks <subroutine> <size>\n";
        return 0;
    }

    std::string subroutine = argv[1];
    char type = subroutine[0];
    int  size = std::stoi(argv[2]);

    subroutine = subroutine.substr(1);
    if (type == 'i') {
        Benchmarks(BLAS_NAME).run(subroutine, size);
    }

    return 0;
}
